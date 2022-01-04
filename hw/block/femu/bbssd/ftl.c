#include "ftl.h"/* dirty maptbl pg node */
int mycnt = 0;

static void *ftl_thread(void *arg);
#ifdef ASYNCH
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
    //return ssd->maptbl[lpn];
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
    //ssd->maptbl[lpn] = *ppa;
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
    spp->pgs_per_blk = 256;
    spp->blks_per_pl = 256; /* 16GB */
    spp->pls_per_lun = 1;
    spp->luns_per_ch = 8;
    spp->nchs = 8;

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
	ftl_log("ssd_init_gtd ... \n");
	struct ssdparams *spp = &ssd->sp; 

	ssd->gtd = g_malloc0(sizeof(struct ppa) * spp->pgs_maptbl); 
	for (int i = 0; i < spp->pgs_maptbl; i++) { 
		ssd->gtd[i].ppa = UNMAPPED_PPA; 
	}
} 
#endif


#if 0
//#ifdef FIFO_BUFF
static void ssd_init_buff(struct ssd *ssd)
{ 
	//buff = (struct buff*)malloc(sizeof(struct buff)); 
	struct ssdparams *spp = &ssd->sp; 

	buff.tot_cnt = 0; 
	buff.head = NULL; 
	buff.tail = NULL;

	/* initialize parameters related with maptbl flush */
	ssd->maptbl_state = (unsigned char*)malloc(sizeof(unsigned char)*spp->pgs_maptbl); 
	for (uint64_t i = 0; i < spp->pgs_maptbl; i++) { 
		ssd->maptbl_state[i] = 0; 
	} 

	ssd->dmpg_list = NULL; 
	ssd->tt_maptbl_dpg = 0; 
	ssd->tt_maptbl_flush = 0; 
	ssd->tt_maptbl_flush_pgs = 0; 
}
//#elif defined DAWID_BUFF
#endif

#ifdef DAWID
static void ssd_init_buff(struct ssd *ssd)
{
	ftl_log("ssd_init_buff ... \n");
	struct ssdparams *spp = &ssd->sp; 
	struct ssd_buff *bp = &ssd->buff; 

	/* initialize request group table */
	bp->dpg_tbl = g_malloc0(sizeof(struct dpg_tbl_ent) * spp->pgs_maptbl);
	for (uint64_t i = 0; i < spp->pgs_maptbl; i++) { 
		bp->dpg_tbl[i].head = NULL;
		bp->dpg_tbl[i].mpg_idx = i;
	}

	/* initialize buff status */
	bp->status = (struct status*)malloc(sizeof(struct status));
//	qemu_mutex_lock(&bp->status->lock); 
	bp->status->tt_reqs = 0;
//	qemu_mutex_unlock(&bp->status->lock); 
	bp->zcl = NULL;

	/* initialize dpg_to_flush_list */ 
	bp->dpg_to_flush_list = (struct dpg_list*)malloc(sizeof(struct dpg_list));
	bp->dpg_to_flush_list->head = NULL; 
	bp->dpg_to_flush_list->tail = NULL; 
	bp->dpg_to_flush_list->reqs = 0;	

	/* initialize dpg_flush_list */
	bp->dpg_flush_list = (struct dpg_list*)malloc(sizeof(struct dpg_list));
	bp->dpg_flush_list->head = NULL; 
	bp->dpg_flush_list->tail = NULL; 
	bp->dpg_flush_list->reqs = 0;

	qemu_mutex_init(&bp->dpg_to_flush_list->lock); 
	qemu_mutex_init(&bp->dpg_flush_list->lock);
	qemu_mutex_init(&bp->status->lock);

	/* initialize dirty bit of maptbl pgs */
	ssd->maptbl_state = g_malloc0(sizeof(int) * spp->pgs_maptbl); 

	ssd->dmpg_list = NULL; 
	ssd->tt_maptbl_dpg = 0; 
	ssd->tt_maptbl_flush = 0;
	ssd->tt_maptbl_flush_pgs = 0; 
	ssd->tt_user_dat_flush_pgs = 0; 

	/* initialize max heap */
	bp->mpg_value_heap = g_malloc0(sizeof(struct max_heap));
	bp->mpg_value_heap->heap = g_malloc0(sizeof(struct hnode) * spp->pgs_maptbl);
	bp->mpg_value_heap->hsz = 0;

	return; 
} 
//#endif
#endif

#ifdef FIFO
static void ssd_init_buff(struct ssd *ssd) 
{ 
	struct ssdparams *spp = &ssd->sp; 
	struct ssd_buff *bp = &ssd->buff; 

	/* initialize request linked list */ 
	bp->head = NULL; 
	bp->tail = NULL; 

	/* initialize buff status */ 
	bp->tt_reqs = 0; 

	/* initialize dirty bit of maptbl pgs */ 
	ssd->maptbl_state = g_malloc0(sizeof(int) * spp->pgs_maptbl); 

	ssd->dmpg_list = NULL; 
	ssd->tt_maptbl_dpg = 0; 
	ssd->tt_maptbl_flush = 0; 
	ssd->tt_maptbl_flush_pgs = 0; 
	ssd->tt_user_dat_flush_pgs = 0; 

	return; 
} 
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
    
    qemu_thread_create(&ssd->ftl_flush_thread, "FEMU-FTL-Flush_Thread", ftl_flush_thread, n, 
		       QEMU_THREAD_JOINABLE);
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
		//ftl_log("lun_avail_time: %ld  nand_stime: %ld  pg_wr_lat: %d\n", lun->next_lun_avail_time, nand_stime, spp->pg_wr_lat); 
#if 0
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time; 
#endif
	//ftl_log("next: %ld  cmd_stime: %ld\n", lun->next_lun_avail_time, cmd_stime);
	nand_stime = cmd_stime;
	//ftl_log("next: %ld  cmd_stime: %ld\n", lun->next_lun_avail_time, cmd_stime);
#if 0 	
	if (lun->next_lun_avail_time >= cmd_stime) 
		ftl_log("next: %ld  cmd_stime: %ld\n", lun->next_lun_avail_time, cmd_stime);
#endif
	if (ncmd->type == USER_IO) {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        } else {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        }
	//ftl_log("next: %ld  cmd_stime: %ld\n", lun->next_lun_avail_time, cmd_stime);
        lat = lun->next_lun_avail_time - cmd_stime;
	//ftl_log("next: %ld  cmd_stime: %ld\n", lun->next_lun_avail_time, cmd_stime);

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
}

static int do_gc(struct ssd *ssd, bool force)
{
    struct line *victim_line = NULL;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lunp;
    struct ppa ppa;
    int ch, lun;
	
    ftl_log("do_gc\n");
    victim_line = select_victim_line(ssd, force);
    if (!victim_line) {
        return -1;
    }

    ppa.g.blk = victim_line->id;
    ftl_debug("GC-ing line:%d,ipc=%d,victim=%d,full=%d,free=%d\n", ppa.g.blk,
              victim_line->ipc, ssd->lm.victim_line_cnt, ssd->lm.full_line_cnt,
              ssd->lm.free_line_cnt);

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

#ifdef USE_BUFF
static void add_to_dirty_mpg_list(struct ssd *ssd, uint64_t fidx)
{ 
	struct dmpg_node *nn = g_malloc0(sizeof(struct dmpg_node)); 

	nn->idx = fidx; 
	nn->next = ssd->dmpg_list; 
	ssd->dmpg_list = nn; 

	return; 
} 
#endif


#ifdef USE_BUFF
/* This function sets the maptbl page dirty and flushes maptbl 
 * if the number of dirty maptbl pages exceeds the protected number. */
static uint64_t set_maptbl_pg_dirty(struct ssd *ssd, uint64_t lba)
{
	//struct ssdparams *spp = &ssd->sp; 
	uint64_t idx = get_mpg_idx(lba);

	//uint64_t curlat = 0, maxlat = 0; 
	uint64_t accumulat = 0; 
	//int r; 

	//int tt_flush_mpgs = 0;
	
	//ftl_log("dirty page: %d\n", ssd->tt_maptbl_dpg); 

	/* Nothing to do if maptbl page is dirty */
	if (ssd->maptbl_state[idx] & (1 << DIRTY_BIT_SHIFT)) { 
		return 0; 
	} 

	//ftl_log("dirty page: %d\n", ssd->tt_maptbl_dpg);
	
	/* set the maptbl pg dirty */
	ssd->maptbl_state[idx] |= (1 << DIRTY_BIT_SHIFT); 

	/* add to dirty maptbl page list */ 
	add_to_dirty_mpg_list(ssd, idx); 

	/* increment the number of dirty maptbl pages */ 
	ssd->tt_maptbl_dpg++; 
	//ftl_log("ssd->tt_maptbl_dpg: %d  spp->pgs_protected: %d  spp->tt_pgs: %d  \n", ssd->tt_maptbl_dpg, spp->pgs_protected, spp->tt_pgs);

#ifdef USE_BUFF_DEBUG_L1
	ftl_log("tt_maptbl_dpgs = %d protected = %d\n", ssd->tt_maptbl_dpg, spp->pgs_protected);
#endif

#if 0
	/* check the need of mapping table flush */
	if (ssd->tt_maptbl_dpg >= spp->pgs_protected) {
#ifdef USE_BUFF_DEBUG_L1
		ftl_log("do flush mapping table ...\n");
#endif
		/* do flush mapping table */ 
		/* 이것도 한번에 하나가 아니라 LINE_SIZE 만큼 내쫓아야 함 */
		/* 지금은 아예 전체를 내쫓는 듯 */ 
		//while (ssd->dmpg_list != NULL) { 
		//while (ssd->dmpg_list != NULL && tt_flush_mpgs < LINE_SIZE) { 
		while (ssd->dmpg_list != NULL) { 
		//for (int i = 0; i < 64; i++) { 
			while (should_gc_high(ssd)) { 
				/* perform GC here until !should_gc(ssd) */
				r = do_gc(ssd, true); 
				if (r == -1)
					break; 
			} 
			
			/* sanghyeon fixed */ 
			idx = ssd->dmpg_list->idx; 
			ssd->maptbl_state[idx] &= ~(1 << DIRTY_BIT_SHIFT);

			struct ppa ppa = get_gtd_ent(ssd, idx); // lpn >> idx ??  
	
			/* update old page information first */
			if (mapped_ppa(&ppa)) { 
				mark_page_invalid(ssd, &ppa); 
				set_rmap_ent(ssd, INVALID_LPN, &ppa); 
			} 
			
			/* new wirte */ 
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
			// how can I treat this latency .. 
			accumulat += maxlat; 

			struct dmpg_node* tmp = ssd->dmpg_list; 
			ssd->dmpg_list = ssd->dmpg_list->next; 
			free(tmp);
	
			tt_flush_mpgs++;
		}
		ssd->tt_maptbl_dpg = 0; 
		//ssd->tt_maptbl_dpg -= 64;
		ssd->tt_maptbl_flush++; 
		ssd->tt_maptbl_flush_pgs += tt_flush_mpgs; 
//		//ftl_log("maptbl_flush_pgs: %d\n", ssd->tt_maptbl_flush_pgs);
		//ftl_log("spp->pgs_protected: %d  tt_maptbl_flush: %d\n", spp->pgs_protected, ssd->tt_maptbl_flush); 
		//return maxlat; 
	}
#endif
//sleep:
	/* sleep for the duration of (tt_flush_mpgs / LINE_SIZE * maxlat) */

	//return 0;
	return accumulat;
}
#endif

#ifdef DAWID
#ifdef USE_BUFF_DEBUG_L1
static void show_max_heap(struct ssd* ssd)
{
	ftl_log("show_max_heap ..\n");

	struct ssd_buff* bp = &ssd->buff;
	struct hnode* heap = bp->mpg_value_heap->heap;
	uint64_t hsz = bp->mpg_value_heap->hsz;

	uint64_t i;
	for(i = 1; i <= hsz; i++) {
		ftl_log("%lu: midx = %lu, dpg_cnt = %lu\n", i, heap[i].mpg_idx, heap[i].dpg_cnt);
		ftl_log("%lu: dpg_tbl[i].heap_idx = %lu\n", i, bp->dpg_tbl[heap[i].mpg_idx].heap_idx);
		assert(bp->dpg_tbl[heap[i].mpg_idx].heap_idx == i);
	}

	return;
}
#endif

//static void insert_max_heap(struct ssd *ssd, struct hnode nnode)
static uint64_t insert_max_heap(struct ssd *ssd, struct hnode hn)
{ 
	struct ssd_buff* bp = &ssd->buff;
	struct hnode* heap = bp->mpg_value_heap->heap;

	uint64_t i = ++(bp->mpg_value_heap->hsz);


	while ((i != 1) && (hn.dpg_cnt > heap[i/2].dpg_cnt)) { 
		heap[i] = heap[i/2]; 
		bp->dpg_tbl[heap[i/2].mpg_idx].heap_idx = i;
		i /= 2; 
	}
	heap[i] = hn;
	bp->dpg_tbl[hn.mpg_idx].heap_idx = i;

#ifdef USE_BUFF_DEBUG_L1
	ftl_log("insert_max_heap ... mpg_idx = %lu, hsz = %lu\n", hn.mpg_idx, i);
#endif

	return i; 
}

static void update_max_heap(struct ssd *ssd, uint64_t idx)
{
//	struct ssdparams *spp = &ssd->sp; 
	struct ssd_buff *bp = &ssd->buff;

	struct hnode* heap = bp->mpg_value_heap->heap;	
	//uint64_t hsz = bp->req_heap_size;
	uint64_t i = bp->dpg_tbl[idx].heap_idx;

	/* update request dpg_cnt for the heap node */
	heap[i].dpg_cnt++;

#if 0
	uint64_t i = 1; 
	struct hnode tmp;

	// search max heap 
	while (h.heap[i].idx != idx) { 
		i++; 
		if (i == spp->pgs_maptbl) { 
			struct hnode newElement; 

			newElement.idx = idx; 
			newElement.dpg_cnt = 1; 

			insert_max_heap(newElement); 

			return; 
		}
	}
#endif
#ifdef USE_BUFF_DEBUG_L1
	ftl_log("update_max_heap ... mpg_idx = %lu, dpg_cnt = %lu\n", idx, heap[i].dpg_cnt);
#endif

	/* go upwards if needed */ 
	struct hnode hn;

//	if (heap[i].dpg_cnt > heap[i/2].dpg_cnt) { 
		hn = heap[i]; 
		while ((i != 1) && (heap[i].dpg_cnt > heap[i/2].dpg_cnt)) { 
			heap[i] = heap[i/2]; 
			bp->dpg_tbl[heap[i/2].mpg_idx].heap_idx = i;
			i /= 2; 
		} 
		heap[i] = hn; 
		bp->dpg_tbl[hn.mpg_idx].heap_idx = i;
//	}
	return; 
}


#ifdef USE_BUFF
static struct hnode* pop_max_heap(struct ssd* ssd)
{
	struct ssd_buff* bp = &ssd->buff;
	struct max_heap* hp = bp->mpg_value_heap;
	struct hnode* heap = bp->mpg_value_heap->heap;
	struct hnode* max_node;

	uint64_t parent, child; 
	struct hnode last_node; 

	if (!hp->hsz)
		return NULL; 
	
	max_node = g_malloc0(sizeof(struct hnode));
	memcpy(max_node, &heap[1], sizeof(struct hnode));

	/* delete max_node and adjust heap */
	last_node = heap[(hp->hsz)--];  // last node 
//	ftl_log("hp->hsz: %ld\n", hp->hsz); 
	parent = 1;
	child = 2; 

	while (child <= hp->hsz) {
		/* choose a child node with larger count among left and right ones */
		if ((child < hp->hsz) && ((heap[child].dpg_cnt) < heap[child + 1].dpg_cnt)) 
			child++; 

		if (last_node.dpg_cnt >= heap[child].dpg_cnt)
			break; 

		/* swap parent and child */
		heap[parent] = heap[child];
		bp->dpg_tbl[heap[child].mpg_idx].heap_idx = parent;

		parent = child; 
		child *= 2; 
	}

	heap[parent] = last_node;
	bp->dpg_tbl[last_node.mpg_idx].heap_idx = parent;

#ifdef USE_BUFF_DEBUG_L1
	ftl_log("pop_max_heap .. mpg_idx = %lu dpg_cnt = %lu\n", max_node->mpg_idx, max_node->dpg_cnt);
#endif

	return max_node; 
}
#endif

static int32_t is_maptbl_pg_dirty(struct ssd *ssd, uint64_t idx) 
{ 
	assert(ssd->maptbl_state != NULL);
	assert(idx < ssd->sp.pgs_maptbl);

	if(ssd->maptbl_state[idx] & (1 << DIRTY_BIT_SHIFT))
		return 1;
	else
		return 0;

//	return ssd->maptbl_state[idx] & DIRTY_BIT_SHIFT? 1 : 0; 
}

static void add_to_zcl(struct ssd *ssd, uint64_t midx)
{ 

	struct ssd_buff* bp = &ssd->buff;

#ifdef USE_BUFF_DEBUG_L1
	struct zcl_node* np;
	ftl_log("add_to_zcl .. midx = %lu\n", midx);
#endif
	if(ssd->maptbl_state[midx] & (1 << EXIST_IN_ZCL_BIT_SHIFT))
		goto out;
	//	return; 


	struct zcl_node* nn = g_malloc0(sizeof(struct zcl_node)); 
	assert(nn != NULL);
	nn->mpg_idx = midx; 
	nn->next = bp->zcl; 
	bp->zcl = nn; 
	ssd->maptbl_state[midx] |= (1 << EXIST_IN_ZCL_BIT_SHIFT); 

out:
	return; 
} 

//static void pageIsClean(struct ssd *ssd, uint64_t idx)
static void add_to_heap(struct ssd *ssd, uint64_t mpg_idx)
{
#ifdef USE_BUFF_DEBUG_L1
//	ftl_log("add_to_heap ... mpg_idx = %lu\n", mpg_idx);
#endif
	struct ssd_buff* bp = &ssd->buff;

	/* Insert a new hnode into the max heap for the first request
 	 * falling into the maptbl pg idx */
	if(!bp->dpg_tbl[mpg_idx].head) {
		struct hnode nn; 
		nn.mpg_idx = mpg_idx; 
		nn.dpg_cnt = 1; 
		insert_max_heap(ssd, nn); 
	} else {
		/* Increase the number of requests for the maptbl page */
		update_max_heap(ssd, mpg_idx); 
	}
#ifdef USE_BUFF_DEBUG_L1
	uint64_t hidx = bp->dpg_tbl[mpg_idx].heap_idx;
	if(bp->mpg_value_heap->heap[hidx].mpg_idx != mpg_idx){
		ftl_log("Failed: hidx = %lu, heap midx = %lu, mpg_idx %lu\n", hidx, bp->mpg_value_heap->heap[hidx].mpg_idx, mpg_idx );
		assert(0);
	}
	//assert(bp->mpg_value_heap->heap[hidx].mpg_idx == mpg_idx);
#endif

	return; 
}
#endif

#ifdef USE_BUFF
static uint64_t ssd_buff_flush_one_page(struct ssd *ssd, struct dpg_node* dpg)
{

	uint64_t lpn;
	struct ppa ppa; 
	int r;

	lpn = dpg->lpn; 

	while (should_gc_high(ssd)) { 
		r = do_gc(ssd, true); 
		if (r == -1) 
			break; 
	} 
	ppa = get_maptbl_ent(ssd, lpn); 

	/* new write */
	ppa = get_new_page(ssd); 
	/* update maptbl */ 
	//sample.. 
	ssd->maptbl[lpn].bfa = INEXIST_BUFF;

	set_maptbl_ent(ssd, lpn, &ppa); 
	/* update rmap */ 
	set_rmap_ent(ssd, lpn, &ppa); 

	mark_page_valid(ssd, &ppa); 

	/* need to advance the write pointer here */ 
	ssd_advance_write_pointer(ssd); 

	struct nand_cmd swr; 
	swr.type = USER_IO; 
	swr.cmd = NAND_WRITE; 
	swr.stime = 0; // set zero for buffer data write
	//swr.stime = stime; 
	/* get latency statistics */ 
	return ssd_advance_status(ssd, &ppa, &swr); 

}
#endif

#ifdef DAWID
static uint32_t ssd_buff_flush(struct ssd *ssd)
{
#ifdef USE_BUFF_DEBUG_L1
	ftl_log("ssd_buff_flush ..\n"); 
#endif
	struct ssd_buff *bp = &ssd->buff;
	struct zcl_node* znp = bp->zcl;
	//uint64_t curlat = 0, maxlat = 0; 
	//uint64_t accumulat = 0; 
	uint64_t idx = 0;

	//int tt_flush_dpgs = 0;
	struct dpg_node* dpg;

	qemu_mutex_lock(&bp->dpg_to_flush_list->lock);
	//ftl_log("catched lock\n");
	/* flush data pages of which maptbl pages are in zcl */
	while (znp!= NULL) { 
		//ftl_log("znp checkpoint\n");
		idx = znp->mpg_idx;  // idx for request table
		struct dpg_tbl_ent* ep = &bp->dpg_tbl[idx];

#ifdef USE_BUFF_DEBUG_L1
		ftl_log("mpg_idx = %lu exists in zcl\n", idx);
#endif

		/* send request from dram buffer to nand flash */ 
		//bp->head = NULL;
		//qemu_mutex_lock(&bp->dpg_to_flush_list->lock); 
		while (ep->head != NULL) { 
			dpg = ep->head;
			ep->head = ep->head->next;  
			dpg->next = NULL;

			if (!bp->dpg_to_flush_list->head & !bp->dpg_to_flush_list->tail) { 
				bp->dpg_to_flush_list->head = dpg; 
				bp->dpg_to_flush_list->tail = dpg;
			} else { 
				bp->dpg_to_flush_list->tail->next = dpg;
				bp->dpg_to_flush_list->tail = bp->dpg_to_flush_list->tail->next; 
			}
			bp->dpg_to_flush_list->reqs++;	
		} 
		//qemu_mutex_unlock(&bp->dpg_to_flush_list->lock); 

		/* remove znode from zcl */
		bp->zcl = bp->zcl->next;
		
		ssd->maptbl_state[znp->mpg_idx] &= ~(1 << EXIST_IN_ZCL_BIT_SHIFT); // off
		free(znp); 

		znp = bp->zcl;
#if 0
		// 한번에 flush 해야하는 데이터 양 충족하면 종료. 
		if(tt_flush_dpgs > LINE_SIZE)
			goto sleep;
#endif
	}
	
	//ftl_log("checkpoint 0\n");
	if (bp->mpg_value_heap->hsz >= 1) { 
		//ftl_log("hsz checkpoint\n");
		struct hnode* max_node = pop_max_heap(ssd); 

		assert(max_node);
		idx = max_node->mpg_idx; 

		struct dpg_tbl_ent* ep = &bp->dpg_tbl[idx];

		/* send request from dram buffer to nand flash */ 
		//uint64_t dpgs = 0;
		//qemu_mutex_lock(&bp->dpg_to_flush_list->lock); 
		while (ep->head != NULL) {
			dpg = ep->head; 
			ep->head = ep->head->next; 
			dpg->next = NULL;

			if (!bp->dpg_to_flush_list->head & !bp->dpg_to_flush_list->tail) { 
				bp->dpg_to_flush_list->head = dpg; 
				bp->dpg_to_flush_list->tail = dpg; 
			} else { 
				bp->dpg_to_flush_list->tail->next = dpg; 
				bp->dpg_to_flush_list->tail = bp->dpg_to_flush_list->tail->next; 
			}
			bp->dpg_to_flush_list->reqs++; 
		}
		//qemu_mutex_unlock(&bp->dpg_to_flush_list->lock); 
		//assert(dpgs == max_node->dpg_cnt);
	
		//ftl_log("flush dpgs: %d\n", tt_flush_dpgs);
	}
	mycnt++; 
	//ssd->tt_user_dat_flush_pgs += tt_flush_dpgs; 
	
	qemu_mutex_unlock(&bp->dpg_to_flush_list->lock);
	//ftl_log("put the lock\n");
	//ftl_log("user data write cnt: %d  mycnt: %d\n", ssd->tt_user_dat_flush_pgs, mycnt); 
//sleep:
	/* sleep for the duration of (tt_flush_dpgs / LINE_SIZE * maxlat) */
	//return 0;
	//ftl_log("to_flush_list: %ld\n", bp->dpg_to_flush_list->reqs);
	return 0;
}
#endif
	
#ifdef DAWID
static uint64_t ssd_buff_write(struct ssd *ssd, NvmeRequest *req)
{ 
#ifdef USE_BUFF_DEBUG_L1
	ftl_log("ssd_buff_write .. \n");
#endif
	/* FEMU maintains data in dram space. When buffer is used, we only update
	 * the metadata associated with buffer. Dawid buffer updates 
	 * 1) request table, 2) max heap, and 3) zero cost list */ 

	struct ssdparams *spp = &ssd->sp; 
	struct ssd_buff *bp = &ssd->buff; 

	uint64_t lba = req->slba; 
	int len = req->nlb; 
	uint64_t start_lpn = lba / spp->secs_per_pg; 
	uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg; 
	struct ppa ppa; 
	uint64_t lpn;
	uint64_t maxlat = 0;
//	struct dpg_node* nn; 

	/* Walk through all pages for the request */
	for (lpn = start_lpn; lpn <= end_lpn; lpn++) { 
		/* calculate the cost of each maptble page
		 * and maintain pages accordingly. 
		 * zero cost list: 
		 * heap:  */ 
		uint64_t idx = get_mpg_idx(lpn); 

		if (is_maptbl_pg_dirty(ssd, idx)) {
		//	ftl_log("11\n");
			add_to_zcl(ssd, idx);
		} else {
		//	ftl_log("222\n");
			add_to_heap(ssd, idx); 
		} 
		//ftl_log("not jump\n");
		/* insert ssd request into request table */	
		/* request table is used to flush data buffer associated with the
 		 * maptbl page */
//here: 
//		nn = g_malloc0(sizeof(struct dpg_node)); 
		struct dpg_node* nn = g_malloc0(sizeof(struct dpg_node)); 

		if (!nn) { 
			ftl_log("nn ERR\n"); 
		} 

		/* fill the request information */
		nn->lpn = lpn; 
		nn->stime = req->stime; 
		nn->prev = NULL;
		nn->next = NULL; 

		/* insert a new request into the request table */ 
		struct dpg_tbl_ent* ep = &bp->dpg_tbl[idx];
		nn->next = ep->head; 
		ep->head = nn; 

		/* increase number of requests in buffer */	
		qemu_mutex_lock(&bp->status->lock); 
		bp->status->tt_reqs++;
		qemu_mutex_unlock(&bp->status->lock);
#ifdef USE_BUFF_DEBUG_L1
		ftl_log("tt_reqs = %d\n", bp->tt_reqs);
#endif

		//ftl_log("tt_reqs = %d\n", bp->status->tt_reqs);
		/* update mapping table entry to point to buffer address */ 
		ppa = get_maptbl_ent(ssd, lpn); 

		if (mapped_ppa(&ppa)) { 
			/* update old page information first */
			mark_page_invalid(ssd, &ppa); 
			set_rmap_ent(ssd, INVALID_LPN, &ppa); 
		} 

		/* update maptbl entry with buffer address */
		set_maptbl_ent_with_buff(ssd, lpn, nn); 

		/* flush data buffer */ 
#ifdef USE_BUFF_DEBUG_L1
	////	ftl_log("bp->tt_reqs: %u .. \n", bp->tt_reqs);
#endif	
#if 0
		//ftl_log("debug1\n");	
//		if (bp->tt_reqs > BUFF_SIZE) { 
		if (bp->status->tt_reqs > 64 && bp->dpg_flush_list->reqs <= 64) { 
		//if (bp->status->tt_reqs > 16384 && bp->dpg_flush_list->reqs <= 64) { 
		//	ftl_log("debug2\n");
			ftl_log("debug bp->status->tt_reqs:%d\n", bp->status->tt_reqs);
			maxlat = ssd_buff_flush(ssd); 
		} 
#endif		
	//	while (bp->status->tt_reqs > 64 && bp->dpg_flush_list->reqs <= 64) { 
		while (bp->status->tt_reqs > 64) {
			maxlat = ssd_buff_flush(ssd); 
			if (bp->status->tt_reqs < BUFF_SIZE) 
				break; 
		}
	} 
	return maxlat/64; 
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
            req->expire_time += lat;

            rc = femu_ring_enqueue(ssd->to_poller[i], (void *)&req, 1);
            if (rc != 1) {
                ftl_err("FTL to_poller enqueue failed\n");
            }

      		/* clean one line if needed (in the background) */
            if (should_gc(ssd)) {
                do_gc(ssd, false);
            }
        }
    }

    return NULL;
}

#ifdef DAWID
#ifdef ASYNCH
static int32_t move_dpg_list(struct dpg_list *src, struct dpg_list *dest)
{ 
	qemu_mutex_lock(&src->lock); 
	//ftl_log("before src->reqs: %ld, dest->reqs: %ld\n", src->reqs, dest->reqs);

	if (!src->reqs) { 
		qemu_mutex_unlock(&src->lock); 
		return -1; 
	} 
	qemu_mutex_lock(&dest->lock); 
	
	assert(dest!=NULL); 
	assert(src!=NULL);
	
	if (dest->tail) { 
		//assert(dest->tail->next == NULL); 
		dest->tail->next = src->head; 
	} 
	
	if (src->head) { 
		//assert(src->head->prev == NULL); 
		src->head->prev = dest->tail; 
	}

	dest->tail = src->tail; 
	
	if (dest->head == NULL) 
		dest->head = src->head; 

	dest->reqs += src->reqs; 
	
	src->head = NULL; 
	src->tail = NULL; 
	src->reqs = 0; 

	//ftl_log("after src->reqs: %ld, dest->reqs: %ld\n", src->reqs, dest->reqs);
	qemu_mutex_unlock(&dest->lock); 
	qemu_mutex_unlock(&src->lock);
	
	return 0;  
} 

static void calc_delay_maptbl_flush(struct ssd *ssd)
{
	//struct ssdparams *spp = &ssd->sp; 
	uint64_t curlat = 0, maxlat = 0; 
	uint64_t accumulat = 0; 
	int r; 

	uint64_t idx; 
	int tt_flush_mpgs = 0;
	uint64_t now, stime; 
	/* do flush mapping table */ 
	/* 이것도 한번에 하나가 아니라 LINE_SIZE 만큼 내쫓아야 함 */
	/* 지금은 아예 전체를 내쫓는 듯 */ 
	//while (ssd->dmpg_list != NULL) { 
	//while (ssd->dmpg_list != NULL && tt_flush_mpgs < LINE_SIZE) { 
	//while (ssd->dmpg_list != NULL) {
	//ftl_log("checkpoint0\n"); 
	for (int i = 0; i < 64; i++) { 
		while (should_gc_high(ssd)) { 
			/* perform GC here until !should_gc(ssd) */
			r = do_gc(ssd, true); 
			if (r == -1)
				break; 
		} 
		
		/* sanghyeon fixed */ 
		idx = ssd->dmpg_list->idx; 
		ssd->maptbl_state[idx] &= ~(1 << DIRTY_BIT_SHIFT);

		struct ppa ppa = get_gtd_ent(ssd, idx); // lpn >> idx ??  

		/* update old page information first */
		if (mapped_ppa(&ppa)) { 
			mark_page_invalid(ssd, &ppa); 
			set_rmap_ent(ssd, INVALID_LPN, &ppa); 
		} 
		
		/* new wirte */ 
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
		// how can I treat this latency .. 
		accumulat += maxlat; 

		struct dmpg_node* tmp = ssd->dmpg_list; 
		ssd->dmpg_list = ssd->dmpg_list->next; 
		free(tmp);

		tt_flush_mpgs++;
	}
	
	//ftl_log("checkpoint1\n");
	stime = qemu_clock_get_ns(QEMU_CLOCK_REALTIME); 
	while (1) { 
		now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME); 
		if (now > stime + maxlat)
			break; 
	}	
	//ftl_log("mapdat maxlat: %ld  calc: %ld\n", maxlat, now);	

	//ssd->tt_maptbl_dpg = 0; 
	ssd->tt_maptbl_dpg -= 64;
	ssd->tt_maptbl_flush++; 
	ssd->tt_maptbl_flush_pgs += tt_flush_mpgs; 
	//ftl_log("maptbl_flush_pgs: %d\n", ssd->tt_maptbl_flush_pgs);
	//ftl_log("spp->pgs_protected: %d  tt_maptbl_flush: %d\n", spp->pgs_protected, ssd->tt_maptbl_flush); 
	//return maxlat;
	return;  
} 

static void *ftl_flush_thread(void *arg)
{ 
	FemuCtrl *n = (FemuCtrl *)arg; 
	struct ssd *ssd = n->ssd; 
	struct ssd_buff *bp = &ssd->buff;
	struct ssdparams *spp = &ssd->sp; 

	struct dpg_node* dpg;
	uint64_t curlat = 0;//, maxlat = 0;
	//uint64_t accumulat = 0;//, maptblat = 0;
	uint64_t now, stime; 
	//uint64_t stime = qemu_clock_get_ns(QEMU_CLOCK_REALTIME); 
	//uint64_t tt_flush_dpgs; 

	while (1) {
		//ftl_log("check0\n");
		/* move dpg_to_flush_list to dpg_flush_list */
		// Condition variable? or busy waiting? 
//		stime = qemu_clock_get_ns(QEMU_CLOCK_REALTIME); 
		if (bp->dpg_to_flush_list->reqs > 64 && bp->dpg_flush_list->reqs <= 64) { 
			ftl_log("check0\n");
			//ftl_log("before tt_reqs: %d, to_flush_list: %ld, flush_list: %ld\n", bp->status->tt_reqs, bp->dpg_to_flush_list->reqs, bp->dpg_flush_list->reqs);
			int32_t ret = move_dpg_list(bp->dpg_to_flush_list, bp->dpg_flush_list);
			//ftl_log("after tt_reqs: %d, to_flush_list: %ld, flush_list: %ld\n", bp->status->tt_reqs, bp->dpg_to_flush_list->reqs, bp->dpg_flush_list->reqs);
			if (ret == -1) 
				ftl_log("move_dpg_list err!\n"); 
			ftl_log("check1\n");
		}
	//	} else { 
			//ftl_log("prob check0\n"); 
	//	}
		
		//ftl_log("check1\n");
		//ftl_log("flush_list->reqs: %ld\n", bp->dpg_flush_list->reqs);
		if (bp->dpg_flush_list->reqs > 64) { // Channel Cnt
			/* Flush dpg_flush_list to Chip */ 
			//ftl_log("check1\n");
			uint64_t maxlat = 0;
			for (int i = 0; i < 64; i++) { 
				dpg = bp->dpg_flush_list->head; 
				
				/* write one page into flash */ 
				curlat = ssd_buff_flush_one_page(ssd, dpg); 
				//ftl_log("curlat: %ld, maxlat: %ld\n", curlat, maxlat);
				maxlat = (curlat > maxlat) ? curlat : maxlat; 
				//accumulat += maxlat; 

				/* update maptbl for the flushed data */ 
				set_maptbl_pg_dirty(ssd, dpg->lpn); 
				//accumulat += maxlat; 
				//if (accumulat < maptblat) 
					//accumulat = maptblat; 

				/* remove request from the request table */
				bp->dpg_flush_list->head = bp->dpg_flush_list->head->next; 
				free(dpg);
				ssd->tt_user_dat_flush_pgs++;  
			}
			//ftl_log("maxlat: %ld\n", maxlat);	
					
			ftl_log("userdat pgs: %d, mapdat pgs: %d\n", ssd->tt_user_dat_flush_pgs, ssd->tt_maptbl_flush_pgs);   
			//ftl_log("check2\n");	
			/* sleep during max latency */
			ftl_log("check2\n");
			stime = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
			while (1) { 
				now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
				//ftl_log("now: %ld, stime: %ld, maxlat: %ld\n", now, stime, maxlat);
				if(now > stime + maxlat)
			//	if (now > stime + maxlat + accumulat)
					break; 		
			} 
			//ftl_log("userdat maxlat: %ld  calc: %ld\n", maxlat, now);	
			//ftl_log("now: %ld, stime: %ld, maxlat: %ld\n", now, stime, maxlat);
			
			ftl_log("check3\n");
			while (ssd->tt_maptbl_dpg >= spp->pgs_protected) { 
				calc_delay_maptbl_flush(ssd);			
			} 

			ftl_log("check4\n");	
			/* decrease the number of requests in buffer */
			//ftl_log("check3\n");
			qemu_mutex_lock(&bp->status->lock);
			bp->status->tt_reqs -= 64;
			qemu_mutex_unlock(&bp->status->lock); 
			bp->dpg_flush_list->reqs -= 64; 
			//tt_flush_dpgs += 64;
			//ftl_log("check4\n");
		}
		//ftl_log("userdat pgs: %d, mapdat pgs: %d\n", ssd->tt_user_dat_flush_pgs, ssd->tt_maptbl_flush_pgs);   
	}

	return NULL; 
}
#endif
#endif
