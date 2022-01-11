#include "ftl.h"/* dirty maptbl pg node */


static void *ftl_thread(void *arg);
#ifdef USE_BUFF
static void *ftl_flush_thread(void *arg);
#endif

static inline bool should_gc(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines);
}

static inline bool should_gc_high(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines_high);
}

static inline struct ppa get_maptbl_ent(struct ssd *ssd, uint64_t lpn)
{
    return ssd->maptbl[lpn];
}

#ifdef USE_BUFF
static inline struct ppa get_gtd_ent(struct ssd *ssd, uint64_t gidx)
{ 
	return ssd->gtd[gidx]; 
}
#endif


#ifdef USE_BUFF
static inline void set_gtd_ent(struct ssd *ssd, uint64_t gidx, struct ppa *ppa)
{ 
	ssd->gtd[gidx] = *ppa; 
}
#endif

static inline void set_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    ftl_assert(lpn < ssd->sp.tt_pgs);
    ssd->maptbl[lpn] = *ppa;
}

#ifdef USE_BUFF
static inline void set_maptbl_ent_with_buff(struct ssd *ssd, uint64_t lpn, struct dpg_node *bfa)
{ 
	ssd->maptbl[lpn].bfa = bfa;	
}
#endif

#ifdef USE_BUFF
static uint64_t get_mpg_idx(uint64_t lba)
{ 
	/* find index on map table page associated with lpn */ 
	return lba >> _PMES; 
} 
#endif

static uint64_t ppa2pgidx(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t pgidx;

    pgidx = ppa->g.ch  * spp->pgs_per_ch  + \
            ppa->g.lun * spp->pgs_per_lun + \
            ppa->g.pl  * spp->pgs_per_pl  + \
            ppa->g.blk * spp->pgs_per_blk + \
            ppa->g.pg;

    ftl_assert(pgidx < spp->tt_pgs);

    return pgidx;
}

static inline uint64_t get_rmap_ent(struct ssd *ssd, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    return ssd->rmap[pgidx];
}

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    ssd->rmap[pgidx] = lpn;
}

static inline int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

static inline pqueue_pri_t victim_line_get_pri(void *a)
{
    return ((struct line *)a)->vpc;
}

static inline void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
    ((struct line *)a)->vpc = pri;
}

static inline size_t victim_line_get_pos(void *a)
{
    return ((struct line *)a)->pos;
}

static inline void victim_line_set_pos(void *a, size_t pos)
{
    ((struct line *)a)->pos = pos;
}

static void ssd_init_lines(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *line;

    lm->tt_lines = spp->blks_per_pl;
    ftl_assert(lm->tt_lines == spp->tt_lines);
    lm->lines = g_malloc0(sizeof(struct line) * lm->tt_lines);

    QTAILQ_INIT(&lm->free_line_list);
    lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri,
            victim_line_get_pri, victim_line_set_pri,
            victim_line_get_pos, victim_line_set_pos);
    QTAILQ_INIT(&lm->full_line_list);

    lm->free_line_cnt = 0;
    for (int i = 0; i < lm->tt_lines; i++) {
        line = &lm->lines[i];
        line->id = i;
        line->ipc = 0;
        line->vpc = 0;
        /* initialize all the lines as free lines */
        QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
        lm->free_line_cnt++;
    }

    ftl_assert(lm->free_line_cnt == lm->tt_lines);
    lm->victim_line_cnt = 0;
    lm->full_line_cnt = 0;
}

static void ssd_init_write_pointer(struct ssd *ssd)
{
    struct write_pointer *wpp = &ssd->wp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;

    /* wpp->curline is always our next-to-write super-block */
    wpp->curline = curline;
    wpp->ch = 0;
    wpp->lun = 0;
    wpp->pg = 0;
    wpp->blk = 0;
    wpp->pl = 0;
}

static inline void check_addr(int a, int max)
{
    ftl_assert(a >= 0 && a < max);
}

static struct line *get_next_free_line(struct ssd *ssd)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    if (!curline) {
        ftl_err("No free lines left in [%s] !!!!\n", ssd->ssdname);
        return NULL;
    }

    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;
#ifdef USE_BUFF_DEBUG
	ftl_log("get_next_free_line: free_line_cnt = %d\n", lm->free_line_cnt);
#endif

    return curline;
}

static void ssd_advance_write_pointer(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct write_pointer *wpp = &ssd->wp;
    struct line_mgmt *lm = &ssd->lm;

    check_addr(wpp->ch, spp->nchs);
    wpp->ch++;
    if (wpp->ch == spp->nchs) {
        wpp->ch = 0;
        check_addr(wpp->lun, spp->luns_per_ch);
        wpp->lun++;
        /* in this case, we should go to next lun */
        if (wpp->lun == spp->luns_per_ch) {
            wpp->lun = 0;
            /* go to next page in the block */
            check_addr(wpp->pg, spp->pgs_per_blk);
            wpp->pg++;
            if (wpp->pg == spp->pgs_per_blk) {
                wpp->pg = 0;
                /* move current line to {victim,full} line list */
                if (wpp->curline->vpc == spp->pgs_per_line) {
                    /* all pgs are still valid, move to full line list */
                    ftl_assert(wpp->curline->ipc == 0);
                    QTAILQ_INSERT_TAIL(&lm->full_line_list, wpp->curline, entry);
                    lm->full_line_cnt++;
                } else {
                    ftl_assert(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_line);
                    /* there must be some invalid pages in this line */
                    ftl_assert(wpp->curline->ipc > 0);
                    pqueue_insert(lm->victim_line_pq, wpp->curline);
                    lm->victim_line_cnt++;
                }
                /* current line is used up, pick another empty line */
                check_addr(wpp->blk, spp->blks_per_pl);
                wpp->curline = NULL;
                wpp->curline = get_next_free_line(ssd);
                if (!wpp->curline) {
                    /* TODO */
                    abort();
                }
                wpp->blk = wpp->curline->id;
                check_addr(wpp->blk, spp->blks_per_pl);
                /* make sure we are starting from page 0 in the super block */
                ftl_assert(wpp->pg == 0);
                ftl_assert(wpp->lun == 0);
                ftl_assert(wpp->ch == 0);
                /* TODO: assume # of pl_per_lun is 1, fix later */
                ftl_assert(wpp->pl == 0);
            }
        }
    }
}

static struct ppa get_new_page(struct ssd *ssd)
{
    struct write_pointer *wpp = &ssd->wp;
    struct ppa ppa;
    ppa.ppa = 0;
    ppa.g.ch = wpp->ch;
    ppa.g.lun = wpp->lun;
    ppa.g.pg = wpp->pg;
    ppa.g.blk = wpp->blk;
    ppa.g.pl = wpp->pl;
    ftl_assert(ppa.g.pl == 0);

    return ppa;
}

static void check_params(struct ssdparams *spp)
{
    /*
     * we are using a general write pointer increment method now, no need to
     * force luns_per_ch and nchs to be power of 2
     */

    //ftl_assert(is_power_of_2(spp->luns_per_ch));
    //ftl_assert(is_power_of_2(spp->nchs));
}

static void ssd_init_params(struct ssdparams *spp)
{
    spp->secsz = 512;
    spp->secs_per_pg = 8;
    spp->pgs_per_blk = 512;
    //spp->pgs_per_blk = 256;
    spp->blks_per_pl = 256; /* 16GB */
    spp->pls_per_lun = 1;
    spp->luns_per_ch = 8;
    spp->nchs = 8;
#ifdef USE_BUFF
	spp->nluns = spp->nchs * spp->luns_per_ch;
#endif
	

    spp->pg_rd_lat = NAND_READ_LATENCY;
    spp->pg_wr_lat = NAND_PROG_LATENCY;
    spp->blk_er_lat = NAND_ERASE_LATENCY;
    spp->ch_xfer_lat = 0;

    /* calculated values */
    spp->secs_per_blk = spp->secs_per_pg * spp->pgs_per_blk;
    spp->secs_per_pl = spp->secs_per_blk * spp->blks_per_pl;
    spp->secs_per_lun = spp->secs_per_pl * spp->pls_per_lun;
    spp->secs_per_ch = spp->secs_per_lun * spp->luns_per_ch;
    spp->tt_secs = spp->secs_per_ch * spp->nchs;

    spp->pgs_per_pl = spp->pgs_per_blk * spp->blks_per_pl;
    spp->pgs_per_lun = spp->pgs_per_pl * spp->pls_per_lun;
    spp->pgs_per_ch = spp->pgs_per_lun * spp->luns_per_ch;
    spp->tt_pgs = spp->pgs_per_ch * spp->nchs;
#ifdef USE_BUFF
	spp->pgs_maptbl = spp->tt_pgs / _PME; 
	spp->pgs_protected = spp->pgs_maptbl * PROTECTED_RATIO; 
#endif

    spp->blks_per_lun = spp->blks_per_pl * spp->pls_per_lun;
    spp->blks_per_ch = spp->blks_per_lun * spp->luns_per_ch;
    spp->tt_blks = spp->blks_per_ch * spp->nchs;

    spp->pls_per_ch =  spp->pls_per_lun * spp->luns_per_ch;
    spp->tt_pls = spp->pls_per_ch * spp->nchs;

    spp->tt_luns = spp->luns_per_ch * spp->nchs;

    /* line is special, put it at the end */
    spp->blks_per_line = spp->tt_luns; /* TODO: to fix under multiplanes */
    spp->pgs_per_line = spp->blks_per_line * spp->pgs_per_blk;
    spp->secs_per_line = spp->pgs_per_line * spp->secs_per_pg;
    spp->tt_lines = spp->blks_per_lun; /* TODO: to fix under multiplanes */

    spp->gc_thres_pcent = 0.75;
    spp->gc_thres_lines = (int)((1 - spp->gc_thres_pcent) * spp->tt_lines);
    spp->gc_thres_pcent_high = 0.95;
    spp->gc_thres_lines_high = (int)((1 - spp->gc_thres_pcent_high) * spp->tt_lines);
    spp->enable_gc_delay = true;


    check_params(spp);
}

static void ssd_init_nand_page(struct nand_page *pg, struct ssdparams *spp)
{
    pg->nsecs = spp->secs_per_pg;
    pg->sec = g_malloc0(sizeof(nand_sec_status_t) * pg->nsecs);
    for (int i = 0; i < pg->nsecs; i++) {
        pg->sec[i] = SEC_FREE;
    }
    pg->status = PG_FREE;
}

static void ssd_init_nand_blk(struct nand_block *blk, struct ssdparams *spp)
{
    blk->npgs = spp->pgs_per_blk;
    blk->pg = g_malloc0(sizeof(struct nand_page) * blk->npgs);
    for (int i = 0; i < blk->npgs; i++) {
        ssd_init_nand_page(&blk->pg[i], spp);
    }
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt = 0;
    blk->wp = 0;
}

static void ssd_init_nand_plane(struct nand_plane *pl, struct ssdparams *spp)
{
    pl->nblks = spp->blks_per_pl;
    pl->blk = g_malloc0(sizeof(struct nand_block) * pl->nblks);
    for (int i = 0; i < pl->nblks; i++) {
        ssd_init_nand_blk(&pl->blk[i], spp);
    }
}

static void ssd_init_nand_lun(struct nand_lun *lun, struct ssdparams *spp)
{
    lun->npls = spp->pls_per_lun;
    lun->pl = g_malloc0(sizeof(struct nand_plane) * lun->npls);
    for (int i = 0; i < lun->npls; i++) {
        ssd_init_nand_plane(&lun->pl[i], spp);
    }
    lun->next_lun_avail_time = 0;
    lun->busy = false;
}

static void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp)
{
    ch->nluns = spp->luns_per_ch;
    ch->lun = g_malloc0(sizeof(struct nand_lun) * ch->nluns);
    for (int i = 0; i < ch->nluns; i++) {
        ssd_init_nand_lun(&ch->lun[i], spp);
    }
    ch->next_ch_avail_time = 0;
    ch->busy = 0;
}

static void ssd_init_maptbl(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->maptbl = g_malloc0(sizeof(struct ppa) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->maptbl[i].ppa = UNMAPPED_PPA;
		ssd->maptbl[i].bfa = INEXIST_BUFF; 
    }
}

static void ssd_init_rmap(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->rmap = g_malloc0(sizeof(uint64_t) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->rmap[i] = INVALID_LPN;
    }
}

#ifdef USE_BUFF
static void ssd_init_gtd(struct ssd *ssd)
{
#ifdef USE_BUFF_DEBUG
	ftl_log("ssd_init_gtd ... \n");
#endif
	struct ssdparams *spp = &ssd->sp; 

	ssd->gtd = g_malloc0(sizeof(struct ppa) * spp->pgs_maptbl); 
	for (int i = 0; i < spp->pgs_maptbl; i++) { 
		ssd->gtd[i].ppa = UNMAPPED_PPA; 
	}
} 
#endif


#ifdef USE_BUFF
static void ssd_init_buff(struct ssd *ssd)
{
#ifdef USE_BUFF_DEBUG
	ftl_log("ssd_init_buff ... \n");
#endif
	struct ssdparams *spp = &ssd->sp; 
	struct ssd_buff *bp = &ssd->buff; 

#ifdef USE_BUFF_DAWID
	/* initialize request group table */
	bp->dpg_tbl = g_malloc0(sizeof(struct dpg_tbl_ent) * spp->pgs_maptbl);
	for (uint64_t i = 0; i < spp->pgs_maptbl; i++) { 
		bp->dpg_tbl[i].head = NULL;
		bp->dpg_tbl[i].mpg_idx = i;
	}
#endif

#ifdef USE_BUFF
	bp->dpg_running_list = (struct dpg_list*)malloc(sizeof(struct dpg_list));
	bp->dpg_to_flush_list = (struct dpg_list*)malloc(sizeof(struct dpg_list));
	bp->dpg_flush_list = (struct dpg_list*)malloc(sizeof(struct dpg_list));
	bp->dpg_finished_list = (struct dpg_list*)malloc(sizeof(struct dpg_list));
	bp->dpg_zombie_list = (struct dpg_list*)malloc(sizeof(struct dpg_list));

	qemu_mutex_init(&bp->dpg_running_list->lock);
	qemu_mutex_init(&bp->dpg_to_flush_list->lock);
	qemu_mutex_init(&bp->dpg_flush_list->lock);
	qemu_mutex_init(&bp->dpg_finished_list->lock);
	qemu_mutex_init(&bp->dpg_zombie_list->lock);

	qemu_mutex_init(&bp->qlock);
	qemu_cond_init(&bp->need_flush_cond);
	qemu_cond_init(&bp->empty_slot_cond);

	bp->need_flush = 0;
	bp->flush_in_progress = 0;
	bp->need_maptbl_update = 0;
	
#endif
	/* initialize buff status */
	bp->tt_reqs = 0;

	/* initialize dirty bit of maptbl pgs */
	ssd->maptbl_state = g_malloc0(sizeof(int) * spp->pgs_maptbl); 

	ssd->dmpg_list = NULL; 
	ssd->tt_maptbl_dpg = 0; 
	ssd->tt_maptbl_flush = 0;
	ssd->tt_maptbl_flush_pgs = 0; 

#ifdef USE_BUFF_DAWID
	/* initialize zero-cost list and max heap */
	bp->zcl_max_heap = g_malloc0(sizeof(struct zcl_max_heap));
	bp->zcl_max_heap->zcl = NULL;
	bp->zcl_max_heap->heap = g_malloc0(sizeof(struct hnode) * spp->pgs_maptbl);
	bp->zcl_max_heap->hsz = 0;
	bp->zcl_max_heap->zsz = 0;
	bp->zcl_max_heap->tt_reqs = 0;
#endif

	return; 
} 
//#endif
#endif

void ssd_init(FemuCtrl *n)
{
    struct ssd *ssd = n->ssd;
    struct ssdparams *spp = &ssd->sp;

    ftl_assert(ssd);

    ssd_init_params(spp);

    /* initialize ssd internal layout architecture */
    ssd->ch = g_malloc0(sizeof(struct ssd_channel) * spp->nchs);
    for (int i = 0; i < spp->nchs; i++) {
        ssd_init_ch(&ssd->ch[i], spp);
    }

    /* initialize maptbl */
    ssd_init_maptbl(ssd);

    /* initialize rmap */
    ssd_init_rmap(ssd);

#ifdef USE_BUFF
	/* initialize gtd */
	ssd_init_gtd(ssd);
#endif

    /* initialize all the lines */
    ssd_init_lines(ssd);

    /* initialize write pointer, this is how we allocate new pages for writes */
    ssd_init_write_pointer(ssd);

#ifdef USE_BUFF
	/* initialize write buffer */
	ssd_init_buff(ssd); 
#endif

    qemu_thread_create(&ssd->ftl_thread, "FEMU-FTL-Thread", ftl_thread, n,
                       QEMU_THREAD_JOINABLE);

#ifdef USE_BUFF
    qemu_thread_create(&ssd->ftl_flush_thread, "FEMU-FTL-Flush-Thread", ftl_flush_thread, n,
                       QEMU_THREAD_JOINABLE);
#endif
}

static inline bool valid_ppa(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    int ch = ppa->g.ch;
    int lun = ppa->g.lun;
    int pl = ppa->g.pl;
    int blk = ppa->g.blk;
    int pg = ppa->g.pg;
    int sec = ppa->g.sec;

    if (ch >= 0 && ch < spp->nchs && lun >= 0 && lun < spp->luns_per_ch && pl >=
        0 && pl < spp->pls_per_lun && blk >= 0 && blk < spp->blks_per_pl && pg
        >= 0 && pg < spp->pgs_per_blk && sec >= 0 && sec < spp->secs_per_pg)
        return true;

    return false;
}

static inline bool valid_lpn(struct ssd *ssd, uint64_t lpn)
{
    return (lpn < ssd->sp.tt_pgs);
}

static inline bool mapped_ppa(struct ppa *ppa)
{
    return !(ppa->ppa == UNMAPPED_PPA);
}

static inline bool existed_buff(struct ppa *ppa)
{ 
	return !(ppa->bfa == INEXIST_BUFF); 
}

static inline struct ssd_channel *get_ch(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->ch[ppa->g.ch]);
}

static inline struct nand_lun *get_lun(struct ssd *ssd, struct ppa *ppa)
{
    struct ssd_channel *ch = get_ch(ssd, ppa);
    return &(ch->lun[ppa->g.lun]);
}

static inline struct nand_plane *get_pl(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_lun *lun = get_lun(ssd, ppa);
    return &(lun->pl[ppa->g.pl]);
}

static inline struct nand_block *get_blk(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_plane *pl = get_pl(ssd, ppa);
    return &(pl->blk[ppa->g.blk]);
}

static inline struct line *get_line(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->lm.lines[ppa->g.blk]);
}

static inline struct nand_page *get_pg(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = get_blk(ssd, ppa);
    return &(blk->pg[ppa->g.pg]);
}

static uint64_t ssd_advance_status(struct ssd *ssd, struct ppa *ppa, struct
        nand_cmd *ncmd)
{
    int c = ncmd->cmd;
    uint64_t cmd_stime = (ncmd->stime == 0) ? \
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;
    uint64_t nand_stime;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lun = get_lun(ssd, ppa);
    uint64_t lat = 0;

    switch (c) {
    case NAND_READ:
        /* read: perform NAND cmd first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;
        lat = lun->next_lun_avail_time - cmd_stime;
#if 0
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;

        /* read: then data transfer through channel */
        chnl_stime = (ch->next_ch_avail_time < lun->next_lun_avail_time) ? \
            lun->next_lun_avail_time : ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        lat = ch->next_ch_avail_time - cmd_stime;
#endif
        break;

    case NAND_WRITE:
        /* write: transfer data through channel first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        if (ncmd->type == USER_IO) {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        } else {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        }
        lat = lun->next_lun_avail_time - cmd_stime;

#if 0
        chnl_stime = (ch->next_ch_avail_time < cmd_stime) ? cmd_stime : \
                     ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        /* write: then do NAND program */
        nand_stime = (lun->next_lun_avail_time < ch->next_ch_avail_time) ? \
            ch->next_ch_avail_time : lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
#endif
        break;

    case NAND_ERASE:
        /* erase: only need to advance NAND status */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->blk_er_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
        break;

    default:
        ftl_err("Unsupported NAND command: 0x%x\n", c);
    }

    return lat;
}

/* update SSD status about one page from PG_VALID -> PG_VALID */
static void mark_page_invalid(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    bool was_full_line = false;
    struct line *line;

    /* update corresponding page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_VALID);
    pg->status = PG_INVALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
    blk->ipc++;
    ftl_assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
    blk->vpc--;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    ftl_assert(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
    if (line->vpc == spp->pgs_per_line) {
        ftl_assert(line->ipc == 0);
        was_full_line = true;
    }
    line->ipc++;
    ftl_assert(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
    line->vpc--;
    if (was_full_line) {
        /* move line: "full" -> "victim" */
        QTAILQ_REMOVE(&lm->full_line_list, line, entry);
        lm->full_line_cnt--;
        pqueue_insert(lm->victim_line_pq, line);
        lm->victim_line_cnt++;
    }
}

static void mark_page_valid(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    struct line *line;

    /* update page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_FREE);
    pg->status = PG_VALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->vpc >= 0 && blk->vpc < ssd->sp.pgs_per_blk);
    blk->vpc++;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    ftl_assert(line->vpc >= 0 && line->vpc < ssd->sp.pgs_per_line);
    line->vpc++;
}

static void mark_block_free(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = get_blk(ssd, ppa);
    struct nand_page *pg = NULL;

    for (int i = 0; i < spp->pgs_per_blk; i++) {
        /* reset page status */
        pg = &blk->pg[i];
        ftl_assert(pg->nsecs == spp->secs_per_pg);
        pg->status = PG_FREE;
    }

    /* reset block status */
    ftl_assert(blk->npgs == spp->pgs_per_blk);
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt++;
}

static void gc_read_page(struct ssd *ssd, struct ppa *ppa)
{
    /* advance ssd status, we don't care about how long it takes */
    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcr;
        gcr.type = GC_IO;
        gcr.cmd = NAND_READ;
        gcr.stime = 0;
        ssd_advance_status(ssd, ppa, &gcr);
    }
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t gc_write_page(struct ssd *ssd, struct ppa *old_ppa)
{
    struct ppa new_ppa;
    struct nand_lun *new_lun;
    uint64_t lpn = get_rmap_ent(ssd, old_ppa);

    ftl_assert(valid_lpn(ssd, lpn));
    new_ppa = get_new_page(ssd);
    /* update maptbl */
    set_maptbl_ent(ssd, lpn, &new_ppa);
    /* update rmap */
    set_rmap_ent(ssd, lpn, &new_ppa);

    mark_page_valid(ssd, &new_ppa);

    /* need to advance the write pointer here */
    ssd_advance_write_pointer(ssd);

    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        gcw.cmd = NAND_WRITE;
        gcw.stime = 0;
        ssd_advance_status(ssd, &new_ppa, &gcw);
    }

    /* advance per-ch gc_endtime as well */
#if 0
    new_ch = get_ch(ssd, &new_ppa);
    new_ch->gc_endtime = new_ch->next_ch_avail_time;
#endif

    new_lun = get_lun(ssd, &new_ppa);
    new_lun->gc_endtime = new_lun->next_lun_avail_time;

    return 0;
}

static struct line *select_victim_line(struct ssd *ssd, bool force)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *victim_line = NULL;

    victim_line = pqueue_peek(lm->victim_line_pq);
    if (!victim_line) {
        return NULL;
    }

    if (!force && victim_line->ipc < ssd->sp.pgs_per_line / 8) {
        return NULL;
    }

    pqueue_pop(lm->victim_line_pq);
    lm->victim_line_cnt--;

    /* victim_line is a danggling node now */
    return victim_line;
}

/* here ppa identifies the block we want to clean */
static void clean_one_block(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_page *pg_iter = NULL;
    int cnt = 0;

    for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
        ppa->g.pg = pg;
        pg_iter = get_pg(ssd, ppa);
        /* there shouldn't be any free page in victim blocks */
        ftl_assert(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID) {
            gc_read_page(ssd, ppa);
            /* delay the maptbl update until "write" happens */
            gc_write_page(ssd, ppa);
            cnt++;
        }
    }

    ftl_assert(get_blk(ssd, ppa)->vpc == cnt);
}

static void mark_line_free(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *line = get_line(ssd, ppa);
    line->ipc = 0;
    line->vpc = 0;
    /* move this line to free line list */
    QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
    lm->free_line_cnt++;
#ifdef USE_BUFF_DEBUG	
    ftl_log("mark_line_free: free_line_cnt = %d\n", lm->free_line_cnt);
#endif

}

static int do_gc(struct ssd *ssd, bool force)
{
    struct line *victim_line = NULL;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lunp;
    struct ppa ppa;
    int ch, lun;
	
    victim_line = select_victim_line(ssd, force);
    if (!victim_line) {
        return -1;
    }

    ppa.g.blk = victim_line->id;
    ftl_debug("GC-ing line:%d,ipc=%d,victim=%d,full=%d,free=%d\n", ppa.g.blk,
              victim_line->ipc, ssd->lm.victim_line_cnt, ssd->lm.full_line_cnt,
              ssd->lm.free_line_cnt);
#ifdef USE_BUFF_DEBUG	
    ftl_log("GC-ing line:%d,ipc=%d,victim=%d,full=%d,free=%d\n", ppa.g.blk,
              victim_line->ipc, ssd->lm.victim_line_cnt, ssd->lm.full_line_cnt,
              ssd->lm.free_line_cnt);
#endif

    /* copy back valid data */
    for (ch = 0; ch < spp->nchs; ch++) {
        for (lun = 0; lun < spp->luns_per_ch; lun++) {
            ppa.g.ch = ch;
            ppa.g.lun = lun;
            ppa.g.pl = 0;
            lunp = get_lun(ssd, &ppa);
            clean_one_block(ssd, &ppa);
            mark_block_free(ssd, &ppa);

            if (spp->enable_gc_delay) {
                struct nand_cmd gce;
                gce.type = GC_IO;
                gce.cmd = NAND_ERASE;
                gce.stime = 0;
                ssd_advance_status(ssd, &ppa, &gce);
            }

            lunp->gc_endtime = lunp->next_lun_avail_time;
        }
    }

    /* update line status */
    mark_line_free(ssd, &ppa);

    return 0;
}

static uint64_t ssd_read(struct ssd *ssd, NvmeRequest *req)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t lba = req->slba;
    int nsecs = req->nlb;
    struct ppa ppa;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + nsecs) / spp->secs_per_pg;
    uint64_t lpn;
    uint64_t sublat, maxlat = 0;

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }
	
    /* normal IO read path */
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        ppa = get_maptbl_ent(ssd, lpn);
        if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
            //printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", ssd->ssdname, lpn);
            //printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n",
            //ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
            continue;
        } else if (existed_buff(&ppa)) { 
			/* if the data is located in buff, latency is zero */
			continue; 	
		} 
        struct nand_cmd srd;
        srd.type = USER_IO;
        srd.cmd = NAND_READ;
        srd.stime = req->stime;
        sublat = ssd_advance_status(ssd, &ppa, &srd);
        maxlat = (sublat > maxlat) ? sublat : maxlat;
    }

    return maxlat;
}

//#if 1 //PFTL
//#if 1 //PROTECTED
#ifdef USE_BUFF
#ifdef PARTIAL_PROTECTED
static void add_to_dirty_mpg_list(struct ssd *ssd, uint64_t fidx)
{ 
	struct dmpg_node *nn = g_malloc0(sizeof(struct dmpg_node)); 

	nn->idx = fidx; 
	nn->next = ssd->dmpg_list; 
	ssd->dmpg_list = nn; 

	return; 
} 
#endif
#endif

#ifdef USE_BUFF
#ifdef PARTIAL_PROTECTED
/* This function sets the maptbl page dirty and flushes maptbl 
 * if the number of dirty maptbl pages exceeds the protected number. */
static uint64_t set_maptbl_pg_dirty(struct ssd *ssd, uint64_t lba)
{
	/* Nothing to do if maptbl page is dirty */
	uint64_t idx = get_mpg_idx(lba);

	if (ssd->maptbl_state[idx] & (1 << DIRTY_BIT_SHIFT)) { 
		return 0; 
	} 

	/* set the maptbl pg dirty */
	// 여기에서 lock 필요한지 체크 
	// clear 는 어디에서 하는지?? 
	ssd->maptbl_state[idx] |= (1 << DIRTY_BIT_SHIFT); 

	/* add to dirty maptbl page list */ 
	add_to_dirty_mpg_list(ssd, idx); 

	/* increment the number of dirty maptbl pages */ 
	ssd->tt_maptbl_dpg++; 

	return 0;
}
#endif
#endif

#ifdef USE_BUFF
#ifdef PARTIAL_PROTECTED
static void calc_delay_maptbl_flush(struct ssd *ssd) 
{ 
	struct ssdparams *spp = &ssd->sp; 
	uint64_t curlat = 0;
	uint64_t maxlat = 0; 
	uint64_t idx = 0;
	uint64_t stime, now;
	uint64_t tt_flush_mpgs = 0;

#ifdef USE_BUFF_DEBUG
		ftl_log("do maptbl flush .. \n");
#endif

	/* do flush mapping table */ 
	for(int i = 0; i < spp->nluns; i++) {  
		assert(ssd->dmpg_list != NULL);
#if 0
		while (should_gc_high(ssd)) { 
			/* perform GC here until !should_gc(ssd) */
			r = do_gc(ssd, true); 
			if (r == -1)
				break; 
		} 
#endif
		idx = ssd->dmpg_list->idx;
		
		/* clear dirty bit */ 
		ssd->maptbl_state[idx] &= ~(1 << DIRTY_BIT_SHIFT);
		ssd->tt_maptbl_dpg--;

		struct ppa ppa = get_gtd_ent(ssd, idx);

		/* update old page information first */
		if (mapped_ppa(&ppa)) { 
			mark_page_invalid(ssd, &ppa); 
			set_rmap_ent(ssd, INVALID_LPN, &ppa); 
		} 
			
		/* new write */ 
		ppa = get_new_page(ssd); 
		/* update gtd */ 
		set_gtd_ent(ssd, idx, &ppa); 
		/* update rmap */
		set_rmap_ent(ssd, idx, &ppa); // maybe we changed rmap struct.. 
	
	
		mark_page_valid(ssd, &ppa); 
	
		/* need to advance the write pointer here */
		ssd_advance_write_pointer(ssd); 

		struct nand_cmd swr; 
		swr.type = USER_IO;
		swr.cmd = NAND_WRITE; 
		swr.stime = 0; 
		/* get latency statistics */
		curlat = ssd_advance_status(ssd, &ppa, &swr); 
		maxlat = (curlat > maxlat) ? curlat : maxlat; 

		/* advance dirty mpg pointer */ 
		struct dmpg_node* tmp = ssd->dmpg_list; 
		ssd->dmpg_list = ssd->dmpg_list->next; 
		free(tmp);
		
		tt_flush_mpgs++;
	
	}

	/* waiting until flush is finished */
	stime = qemu_clock_get_ns(QEMU_CLOCK_REALTIME); 
	while(1) {
		now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
		if (now > stime + maxlat)
			break;
	}

	ssd->tt_maptbl_flush++; 
	ssd->tt_maptbl_flush_pgs += tt_flush_mpgs; 

#ifdef USE_BUFF_DEBUG
	ftl_log("do maptbl flush .. %d completed\n",ssd->tt_maptbl_flush_pgs);
#endif



	return; 
}
#endif
#endif

#ifdef USE_BUFF
#ifdef USE_BUFF_DAWID
#if 0
static void show_max_heap(struct ssd* ssd)
{
	ftl_log("show_max_heap ..\n");

	struct ssd_buff* bp = &ssd->buff;
	struct hnode* heap = bp->zcl_max_heap->heap;
	uint64_t hsz = bp->zcl_max_heap->hsz;

	uint64_t i;
	for(i = 1; i <= hsz; i++) {
		ftl_log("%lu: mpg_idx = %lu, dpg_cnt = %lu\n", i, heap[i].mpg_idx, heap[i].dpg_cnt);
		ftl_log("%lu: dpg_tbl[i].heap_idx = %lu\n", i, bp->dpg_tbl[heap[i].mpg_idx].heap_idx);
		assert(bp->dpg_tbl[heap[i].mpg_idx].heap_idx == i);
	}

	return;
}
#endif

static uint64_t insert_max_heap(struct ssd *ssd, struct hnode hn)
{ 

	struct ssd_buff* bp = &ssd->buff;
	struct hnode* heap = bp->zcl_max_heap->heap;
	struct dpg_tbl_ent* dtp = bp->dpg_tbl; 

	uint64_t i = ++(bp->zcl_max_heap->hsz);

	//while ((i != 1) && (hn.dpg_cnt > heap[i/2].dpg_cnt)) { 
	uint64_t pivot = dtp[hn.mpg_idx].dpg_cnt;

	while ((i != 1) && (pivot > dtp[heap[i/2].mpg_idx].dpg_cnt)) { 
		heap[i] = heap[i/2]; 
		bp->dpg_tbl[heap[i/2].mpg_idx].heap_idx = i;
		i /= 2; 
	}
	heap[i] = hn;
	bp->dpg_tbl[hn.mpg_idx].heap_idx = i;

#ifdef USE_BUFF_DEBUG
	ftl_log("insert_max_heap: hsz = %d\n", bp->zcl_max_heap->hsz);
#endif
	return i; 
}

static void update_max_heap(struct ssd *ssd, uint64_t mpg_idx)
{

//	struct ssdparams *spp = &ssd->sp; 
	struct ssd_buff *bp = &ssd->buff;
	struct dpg_tbl_ent *dtp = bp->dpg_tbl;

	struct hnode* heap = bp->zcl_max_heap->heap;	
	//uint64_t hsz = bp->req_heap_size;
	uint64_t i = dtp[mpg_idx].heap_idx;

	/* update request dpg_cnt for the heap node */
	//heap[i].dpg_cnt++;
//	dtp[mpg_idx].dpg_cnt++;

#ifdef USE_BUFF_DEBUG
//	ftl_log("update_max_heap: mpg_idx = %ld, hidx = %ld dpg_cnt = %ld hsz = %d\n", heap[i].mpg_idx, i, dtp[mpg_idx].dpg_cnt, bp->zcl_max_heap->hsz);
#endif

	/* go upwards if needed */ 
	struct hnode hn;

	hn = heap[i]; 
	while ((i != 1) && (dtp[heap[i].mpg_idx].dpg_cnt > dtp[heap[i/2].mpg_idx].dpg_cnt)) { 
		heap[i] = heap[i/2]; 
		dtp[heap[i/2].mpg_idx].heap_idx = i;
		i /= 2; 
	} 
	heap[i] = hn; 
	dtp[hn.mpg_idx].heap_idx = i;

	return; 
}

static struct hnode* pop_max_heap(struct ssd* ssd)
{
	struct ssd_buff* bp = &ssd->buff;
	struct dpg_tbl_ent *dtp = bp->dpg_tbl;
	struct zcl_max_heap* hp = bp->zcl_max_heap;
	struct hnode* heap = bp->zcl_max_heap->heap;
	struct hnode* max_node;

	uint64_t parent, child; 
	struct hnode last_node; 

	if (!hp->hsz)
		return NULL; 

	max_node = g_malloc0(sizeof(struct hnode));
	memcpy(max_node, &heap[1], sizeof(struct hnode));

	/* delete max_node and adjust heap */
	last_node = heap[(hp->hsz)--];  // last node 
	parent = 1;
	child = 2; 

	while (child <= hp->hsz) {
		/* choose a child node with larger count among left and right ones */
		if ((child < hp->hsz) && ((dtp[heap[child].mpg_idx].dpg_cnt) < dtp[heap[child + 1].mpg_idx].dpg_cnt)) 
			child++; 

		if (dtp[last_node.mpg_idx].dpg_cnt >= dtp[heap[child].mpg_idx].dpg_cnt)
			break; 

		/* swap parent and child */
		heap[parent] = heap[child];
		bp->dpg_tbl[heap[child].mpg_idx].heap_idx = parent;

		parent = child; 
		child *= 2; 
	}

	heap[parent] = last_node;
	bp->dpg_tbl[last_node.mpg_idx].heap_idx = parent;

	return max_node; 
}
#endif
#endif

#ifdef USE_BUFF
static int insert_dpg_list(struct dpg_list* list, struct dpg_node* nn)
{
	int ret; 

	// enqueue at a tail and dequeue at a head 
	qemu_mutex_lock(&list->lock);

	nn->next = NULL; 
	nn->prev = list->tail;

	if(list->tail)
		list->tail->next = nn;

	if(list->head == NULL)
		list->head = nn;

	list->tail = nn;
	ret = ++list->reqs;
	qemu_mutex_unlock(&list->lock);
	
	return ret; 
}
static void move_dpg_list(struct dpg_list* src, struct dpg_list* dest)
{
	qemu_mutex_lock(&src->lock);

	if(!src->reqs){
		qemu_mutex_unlock(&src->lock);
		return; 
	}
	qemu_mutex_lock(&dest->lock);

	
	// Append a src list into the tail of dest list. 	
	if(dest->tail) {
		assert(dest->tail->next == NULL);
		dest->tail->next = src->head; 
	}

	if(src->head){
		assert(src->head->prev == NULL);
		src->head->prev = dest->tail;
	}

	dest->tail = src->tail; 

	if(dest->head == NULL)
		dest->head = src->head; 

	dest->reqs += src->reqs; 

	src->head = NULL;
	src->tail = NULL;
	src->reqs = 0;

	qemu_mutex_unlock(&dest->lock);
	qemu_mutex_unlock(&src->lock);

	return;

}

static uint64_t reap_dpg_list(struct ssd* ssd, struct dpg_list* zombie_list)
{
//	ftl_log("ftl_thread reaps zombies\n");
	struct dpg_node* zp = zombie_list->head;
	struct dpg_node* prev_zp;
	uint64_t reqs = 0;

	while(zp) {
		uint64_t lpn = zp->lpn;
		if(ssd->maptbl[lpn].bfa == zp) 
			ssd->maptbl[lpn].bfa = INEXIST_BUFF;
		prev_zp = zp;
		zp = zp->next;
		free(prev_zp);

		zombie_list->reqs--;
		reqs++;
	}

	assert(zombie_list->reqs==0);
	zombie_list->head = NULL;
	zombie_list->tail = NULL;
	
	return reqs; 
}
#endif


#ifdef USE_BUFF
#ifdef USE_BUFF_DAWID
static int32_t is_maptbl_pg_dirty(struct ssd *ssd, uint64_t idx) 
{ 
	// 여기에서 lock 필요 
	assert(ssd->maptbl_state != NULL);
	assert(idx < ssd->sp.pgs_maptbl);

	return ssd->maptbl_state[idx] & (1 << DIRTY_BIT_SHIFT);

}

static void add_to_zcl(struct ssd *ssd, uint64_t mpg_idx)
{ 
	struct ssd_buff* bp = &ssd->buff;

	if(ssd->maptbl_state[mpg_idx] & (1 << EXIST_IN_ZCL_BIT_SHIFT)){
#ifdef USE_BUFF_DEBUG
		ftl_log("add_to_zcl: hit mpg_idx = %ld, zcl_reqs = %d\n", mpg_idx, bp->zcl_max_heap->zsz);
#endif
		return;
	}

	struct zcl_node* nn = g_malloc0(sizeof(struct zcl_node));
	assert(nn != NULL);
	nn->mpg_idx = mpg_idx;
	nn->next = bp->zcl_max_heap->zcl; 
	bp->zcl_max_heap->zcl = nn; 
	bp->zcl_max_heap->zsz ++; 
	ssd->maptbl_state[mpg_idx] |= (1 << EXIST_IN_ZCL_BIT_SHIFT); 
	
#ifdef USE_BUFF_DEBUG
		ftl_log("add_to_zcl: insert mpg_idx = %ld, zcl_reqs = %d\n", mpg_idx, bp->zcl_max_heap->zsz);
#endif

	return;
} 

static void add_to_heap(struct ssd *ssd, uint64_t mpg_idx)
{
#ifdef USE_BUFF_DEBUG
//	ftl_log("add_to_heap: mpg_idx = %ld, hsz = %ld \n", mpg_idx, bp->zcl_max_heap->hsz);
#endif

	struct ssd_buff* bp = &ssd->buff;

	/* Insert a new dpg_node into the max heap for the first request
 	 * falling into the maptbl pg idx */
	//if(!bp->dpg_tbl[mpg_idx].head) {
	if(bp->dpg_tbl[mpg_idx].dpg_cnt == 1) {
		struct hnode nn;
		nn.mpg_idx = mpg_idx;
//		assert(bp->dpg_tbl[mpg_idx].dpg_cnt == 0);
//		bp->dpg_tbl[mpg_idx].dpg_cnt = 1;
		insert_max_heap(ssd, nn); 
	} else {
		/* Increase the number of requests for the maptbl page */
		update_max_heap(ssd, mpg_idx); 
	}
	return; 
}

static int insert_mpg_idx_heap(struct ssd* ssd, uint64_t mpg_idx)
{
	struct ssd_buff* bp = &ssd->buff;

	if(is_maptbl_pg_dirty(ssd, mpg_idx)) 
		add_to_zcl(ssd, mpg_idx);
	else
		add_to_heap(ssd, mpg_idx);

	return ++bp->zcl_max_heap->tt_reqs;
}

#if 0
static void delete_max_heap(void) 
{
	uint64_t parent, child; 
	struct dpg_node temp; 

//	item = h.heap[1]; 
	if (h.heap_size == 0)
		return; 

	temp = h.heap[(h.heap_size)--]; 
	parent = 1;
	child = 2; 

	while (child <= h.heap_size) {
		if ((child < h.heap_size) && ((h.heap[child].dpg_cnt) < h.heap[child + 1].dpg_cnt)) 
			child++; 

		if (temp.dpg_cnt >= h.heap[child].dpg_cnt)
			break; 

		h.heap[parent] = h.heap[child];
		parent = child; 
		child *= 2; 
	}

	h.heap[parent] = temp;
	return; 
}
#endif


static void move_heap_to_running_list(struct ssd* ssd)
{

	struct ssd_buff *bp = &ssd->buff;
	struct ssdparams *spp = &ssd->sp;
	struct zcl_node* znp; 
	uint64_t mpg_idx = 0;

	int tt_moved_dpgs = 0;
	struct dpg_node* dpg;

	/* flush data pages of which maptbl pages are in zcl */
	znp = bp->zcl_max_heap->zcl;

	while (znp!= NULL) { 
		mpg_idx = znp->mpg_idx;  
		struct dpg_tbl_ent* ep = &bp->dpg_tbl[mpg_idx];
#ifdef USE_BUFF_DEBUG
		ftl_log("move zcl to list: mpg_idx = %ld, dpg_cnt = %ld, zsz = %d\n", mpg_idx, ep->dpg_cnt, bp->zcl_max_heap->zsz);
#endif
		// move dpg into a running list 
		while (ep->head != NULL) { 
			dpg = ep->head; 
			ep->head = ep->head->next;
			insert_dpg_list(bp->dpg_running_list, dpg);
			tt_moved_dpgs++;
			ep->dpg_cnt--;
		}

		/* remove znode from zcl */
		bp->zcl_max_heap->zcl = bp->zcl_max_heap->zcl->next;
		ssd->maptbl_state[znp->mpg_idx] &= ~(1 << EXIST_IN_ZCL_BIT_SHIFT);
		free(znp); 
		bp->zcl_max_heap->zsz--;

		znp = bp->zcl_max_heap->zcl;
	}
#ifdef USE_BUFF_DEBUG
	assert(bp->zcl_max_heap->zsz == 0);
#endif
		

	/* 여기까지 왔다는 것은 더 flush 는 해야하는데 ZCL 은 비어 있다는 뜻 */ 
	while(1) {	
#ifdef USE_BUFF_DEBUG
		ftl_log("move heap to list: hsz = %d\n", bp->zcl_max_heap->hsz);
#endif

		//if(tt_moved_dpgs % spp->luns == 0)
		if(tt_moved_dpgs >= spp->nluns)
			goto out;

		if(!bp->zcl_max_heap->hsz) 
			goto out;

		struct hnode* max_node = pop_max_heap(ssd);
		mpg_idx = max_node->mpg_idx; 
		struct dpg_tbl_ent* ep = &bp->dpg_tbl[mpg_idx];
#ifdef USE_BUFF_DEBUG
		ftl_log("move heap to list: mpg_idx = %ld, dpg_cnt = %ld\n", mpg_idx, bp->dpg_tbl[mpg_idx].dpg_cnt);
		uint64_t moved_dpgs = 0;
#endif
		// move dpg into a running list 
		while (ep->head != NULL) { 
			dpg = ep->head; 
			ep->head = ep->head->next;
			insert_dpg_list(bp->dpg_running_list, dpg);
//			free(dpg);
			tt_moved_dpgs++;
			ep->dpg_cnt--;
#ifdef USE_BUFF_DEBUG
			moved_dpgs++;
#endif
		}
#ifdef USE_BUFF_DEBUG
		ftl_log("max_node mpg_idx = %ld, dpg_cnt = %ld, moved_dpgs = %ld\n",  
			max_node->mpg_idx, bp->dpg_tbl[max_node->mpg_idx].dpg_cnt, moved_dpgs);
#endif
	}

out:
	bp->zcl_max_heap->tt_reqs -= tt_moved_dpgs;
#ifdef USE_BUFF_DEBUG
	ftl_log("move_heap_to_running_list: zcl_max_heap tt_reqs = %d\n", bp->zcl_max_heap->tt_reqs);
	if(bp->zcl_max_heap->tt_reqs > 0)
		assert(tt_moved_dpgs >= 64);
	ftl_log("move_heap_to_running_list: tt_moved_pages %d\n", tt_moved_dpgs);
#endif

	return;
}
#endif
#endif


#ifdef USE_BUFF
static int32_t ssd_buff_flush_one_page(struct ssd *ssd, struct dpg_node* dpg, uint64_t stime)
{
	uint64_t lpn;
	struct ppa ppa; 
	int r;

	lpn = dpg->lpn; 

	while (should_gc_high(ssd)) { 
		ftl_log("ssd_buff_flush_one_page: do_gc\n");
		r = do_gc(ssd, true); 
		if (r == -1) 
			break; 
	} 

	/* new write */
	ppa = get_new_page(ssd); 
	/* update maptbl */ 
	set_maptbl_ent(ssd, lpn, &ppa); 
	/* update rmap */ 
	set_rmap_ent(ssd, lpn, &ppa); 

	mark_page_valid(ssd, &ppa); 

	/* need to advance the write pointer here */ 
	ssd_advance_write_pointer(ssd); 

	struct nand_cmd swr; 
	swr.type = USER_IO; 
	swr.cmd = NAND_WRITE; 
	swr.stime = stime; // set zero for buffer data write

	/* get latency statistics */ 
	return ssd_advance_status(ssd, &ppa, &swr); 

}
#endif


#ifdef USE_BUFF
static uint64_t ssd_buff_flush(struct ssd *ssd)
{
#ifdef PARTIAL_PROTECTED
	struct ssd_buff *bp = &ssd->buff;
#endif
	struct ssdparams *spp = &ssd->sp;
	uint64_t curlat = 0, maxlat = 0; 

	uint64_t tt_flush_dpgs = 0;
	uint64_t stime = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
	uint64_t now;

	// Don't need to lock for the flush list. This is the only one thread 
	// that accesses this list. 
	struct dpg_node* dpg = bp->dpg_flush_list->head;

	/* send request from dram buffer to nand flash */ 
	while (dpg) {
		/* write one page into flash */
		curlat = ssd_buff_flush_one_page(ssd, dpg, stime);
		maxlat = (curlat > maxlat) ? curlat : maxlat; 

#ifdef PARTIAL_PROTECTED
		/* update maptbl for the flushed data */
		set_maptbl_pg_dirty(ssd, dpg->lpn); 
#endif

		/* remove request from the request table */
//		prev_dpg = dpg;
		dpg = dpg->next;
//		free(prev_dpg);

		tt_flush_dpgs++;
	}
	/* sleep during max latency */
	while(1) {
		now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME); 
		if(now > stime + maxlat)
			break;
	}

#ifdef PARTIAL_PROTECTED
	/* check the need of mapping table flush */ 
	while(ssd->tt_maptbl_dpg >= spp->pgs_protected) {
		calc_delay_maptbl_flush(ssd);

	}
#endif

	return tt_flush_dpgs;
}
#endif



#ifdef USE_BUFF
static uint64_t ssd_buff_write(struct ssd *ssd, NvmeRequest *req)
{ 
	/* FEMU maintains data in dram space. When buffer is used, we only update
	 * the metadata associated with buffer. Dawid buffer updates 
	 * 1) request table, 2) max heap, and 3) zero cost list */ 

	struct ssdparams *spp = &ssd->sp; 
	struct ssd_buff *bp = &ssd->buff; 

	uint64_t lba = req->slba; 
	int len = req->nlb; 
	uint64_t start_lpn = lba / spp->secs_per_pg; 
	uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg; 
	struct dpg_node* nn;
	struct ppa ppa; 
	uint64_t lpn; 

	uint64_t lat = 0;

	/* Walk through all pages for the request */
	for (lpn = start_lpn; lpn <= end_lpn; lpn++) { 

		/* Wait until an empty slot is ready in buffer */
		qemu_mutex_lock(&bp->qlock);
		while(!(bp->tt_reqs < BUFF_SIZE)){
			qemu_cond_wait(&bp->empty_slot_cond, &bp->qlock);
		}
		qemu_mutex_unlock(&bp->qlock);

		/* Invalidate data and reclaim buffer slots */
		move_dpg_list(bp->dpg_finished_list, bp->dpg_zombie_list);

		/* Reap a zombie nodes */ 
		reap_dpg_list(ssd, bp->dpg_zombie_list);

		/****************************************/
		/*      Processing a write request      */
		/****************************************/

		/* Increase number of requests in buffer */	
		qemu_mutex_lock(&bp->qlock);
		bp->tt_reqs++;
		qemu_mutex_unlock(&bp->qlock);


		/* Update mapping table entry to point to buffer address */ 
		ppa = get_maptbl_ent(ssd, lpn); 

		if (mapped_ppa(&ppa)) { 
			/* update old page information first */
			mark_page_invalid(ssd, &ppa); 
			set_rmap_ent(ssd, INVALID_LPN, &ppa); 
		} 

		/* Update maptbl entry with buffer address */
		if(!(nn=g_malloc0(sizeof(struct dpg_node)))){
			ftl_log("Failed to allocate dpg_node\n"); 
		} 

		/* fill the request information */
		nn->lpn = lpn; 
		nn->stime = req->stime; 
		nn->prev = NULL;
		nn->next = NULL;

		set_maptbl_ent_with_buff(ssd, lpn, nn); 

		/****************************************/
		/*      Buffer management for flushing 	*/
		/****************************************/

		/* insert a new request into the running dpg_list */ 
		int running_reqs = 0;

#ifdef USE_BUFF_FIFO	
		running_reqs = insert_dpg_list(bp->dpg_running_list, nn); 
#endif
#ifdef USE_BUFF_DAWID
		uint64_t mpg_idx = get_mpg_idx(lpn);

		/* Insert this request to dpg list. */
		struct dpg_tbl_ent* ep = &bp->dpg_tbl[mpg_idx];
		nn->next = ep->head;
		ep->head = nn;
		bp->dpg_tbl[mpg_idx].dpg_cnt++;

		running_reqs = insert_mpg_idx_heap(ssd, mpg_idx); 
#ifdef USE_BUFF_DEBUG
//		ftl_log("running_reqs = %d, spp->nluns = %d\n", running_reqs, spp->nluns);
#endif

#if 0
#ifdef USE_BUFF_DEBUG
		uint64_t _dpg_cnt = 0;
		struct dpg_tbl_ent* _ep = &bp->dpg_tbl[mpg_idx];
		struct dpg_node* _dp = _ep->head;
		while(_dp) {
			_dpg_cnt++;	
			_dp = _dp->next;
		}
		ftl_log("mpg_idx = %ld, dpg_cnt %ld, _dpg_cnt = %ld\n", 
			mpg_idx, _ep->dpg_cnt, _dpg_cnt);
		assert(_dpg_cnt == ep->dpg_cnt);
#endif
#endif
#endif

		/* trigger flush if needed */
		if (running_reqs >= spp->nluns) { 
#ifdef USE_BUFF_DEBUG
//			ftl_log("flush: running_reqs = %d, spp->nluns = %d\n", running_reqs, spp->nluns);
#endif
			qemu_mutex_lock(&bp->qlock);
			if(!bp->flush_in_progress) {
				bp->need_flush = 1;
#ifdef USE_BUFF_DAWID
				move_heap_to_running_list(ssd); 
#endif
				move_dpg_list(bp->dpg_running_list, bp->dpg_to_flush_list);
				qemu_cond_signal(&bp->need_flush_cond);
			}
			qemu_mutex_unlock(&bp->qlock);
		}
	} 
	return lat; 
} 
#endif

#ifndef USE_BUFF
static uint64_t ssd_write(struct ssd *ssd, NvmeRequest *req)
{
    uint64_t lba = req->slba;
    struct ssdparams *spp = &ssd->sp;
    int len = req->nlb;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg;
    struct ppa ppa;
    uint64_t lpn;
    uint64_t curlat = 0, maxlat = 0;
    int r;

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }

    while (should_gc_high(ssd)) {
        /* perform GC here until !should_gc(ssd) */
		ftl_log("ssd_write: do_gc\n");
        r = do_gc(ssd, true);
        if (r == -1)
            break;
    }

    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        ppa = get_maptbl_ent(ssd, lpn);
        if (mapped_ppa(&ppa)) {
            /* update old page information first */
            mark_page_invalid(ssd, &ppa);
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
        }

        /* new write */
        ppa = get_new_page(ssd);
        /* update maptbl */
        set_maptbl_ent(ssd, lpn, &ppa);
        /* update rmap */
        set_rmap_ent(ssd, lpn, &ppa);

        mark_page_valid(ssd, &ppa);

        /* need to advance the write pointer here */
        ssd_advance_write_pointer(ssd);

        struct nand_cmd swr;
        swr.type = USER_IO;
        swr.cmd = NAND_WRITE;
        swr.stime = req->stime;
        /* get latency statistics */
        curlat = ssd_advance_status(ssd, &ppa, &swr);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }

    return maxlat;
}
#endif

static void *ftl_thread(void *arg)
{
    FemuCtrl *n = (FemuCtrl *)arg;
    struct ssd *ssd = n->ssd;
    NvmeRequest *req = NULL;
    uint64_t lat = 0;
    int rc;
    int i;
#ifdef EUNJI
	uint64_t now;
#endif
	
	// debug
	//ftl_log("debug test in ftl_thread\n"); 

    while (!*(ssd->dataplane_started_ptr)) {
        usleep(100000);
    }

    /* FIXME: not safe, to handle ->to_ftl and ->to_poller gracefully */
    ssd->to_ftl = n->to_ftl;
    ssd->to_poller = n->to_poller;

    while (1) {
        for (i = 1; i <= n->num_poller; i++) {
            if (!ssd->to_ftl[i] || !femu_ring_count(ssd->to_ftl[i]))
                continue;

            rc = femu_ring_dequeue(ssd->to_ftl[i], (void *)&req, 1);
            if (rc != 1) {
                printf("FEMU: FTL to_ftl dequeue failed\n");
            }

            ftl_assert(req);
            switch (req->is_write) {
            case 1:
#ifdef USE_BUFF
 				lat = ssd_buff_write(ssd, req); 
#else
                lat = ssd_write(ssd, req);
#endif
				break;
            case 0:
                lat = ssd_read(ssd, req);
                break;
            default:
                ftl_err("FTL received unkown request type, ERROR\n");
            }

            req->reqlat = lat;

#ifdef EUNJI
			now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
            req->expire_time = now + lat; // EUNJI: queing delay 반영하려면 여기에서 expire time 을 now 로 설정. 
#else
            req->expire_time += lat;
#endif

            rc = femu_ring_enqueue(ssd->to_poller[i], (void *)&req, 1);
            if (rc != 1) {
                ftl_err("FTL to_poller enqueue failed\n");
            }

      		/* clean one line if needed (in the background) */
            if (should_gc(ssd)) {
				ftl_log("ftl_thread: do_gc\n");
                do_gc(ssd, false);
            }
        }
    }

    return NULL;
}

#ifdef USE_BUFF

static void *ftl_flush_thread(void *arg)
{
    FemuCtrl *n = (FemuCtrl *)arg;
    struct ssd *ssd = n->ssd;
//	struct ssdparams *spp = &ssd->sp; 
	struct ssd_buff* bp = &ssd->buff;
	uint64_t tt_flush_dpgs;

	while(1) {
        //usleep(10000000);
		qemu_mutex_lock(&bp->qlock);
		while(!bp->need_flush){
#ifdef USE_BUFF_DEBUG_L1
			ftl_log("ftl_flush_thread: sleep on need_flush_cond\n");
#endif
			qemu_cond_wait(&bp->need_flush_cond, &bp->qlock);
		}
#ifdef USE_BUFF_DEBUG
		ftl_log("ftl_flush_thread: wakes up on need_flush_cond\n");
#endif
		bp->need_flush = 0;
		bp->flush_in_progress = 1;
		qemu_mutex_unlock(&bp->qlock);

		/**************************/
		/*		start flushing    */
		/**************************/
#ifdef USE_BUFF_DEBUG_L1
		ftl_log("ftl_flush_thread starts flushing. \n");
#endif
		move_dpg_list(bp->dpg_to_flush_list, bp->dpg_flush_list);

		/* do flush */
		tt_flush_dpgs = ssd_buff_flush(ssd);

		/* finished flush */
#ifdef USE_BUFF_DEBUG_L1
		ftl_log("ftl_flush_thread finished flushing\n");
#endif
		move_dpg_list(bp->dpg_flush_list, bp->dpg_finished_list);

		qemu_mutex_lock(&bp->qlock);
		bp->flush_in_progress = 0;

#ifdef USE_BUFF_DEBUG_L1
		ftl_log("flush_thread: signal empty_slot_cond\n");
#endif
		bp->tt_reqs -= tt_flush_dpgs;
		qemu_cond_signal(&bp->empty_slot_cond);
		qemu_mutex_unlock(&bp->qlock);
	}
    return NULL;
}
#endif

#if 0 //PFTL
int16_t get_buff_tot_cnt(void)
{
	return buff.tot_cnt;
}	
#endif
