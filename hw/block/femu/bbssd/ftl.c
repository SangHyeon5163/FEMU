#include "ftl.h"

#define FEMU_DEBUG_FTL
//struct buff buff; 
//struct ht h; 

static void *ftl_thread(void *arg);

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

#if 1 //map flush
static inline struct ppa get_gtd_ent(struct ssd *ssd, uint64_t gidx)
{ 
	return ssd->gtd[gidx]; 
}

static inline void set_gtd_ent(struct ssd *ssd, uint64_t gidx, struct ppa *ppa)
{ 
	//ftl_assert();
	ssd->gtd[gidx] = *ppa; 
}
#endif

static inline void set_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    ftl_assert(lpn < ssd->sp.tt_pgs);
    ssd->maptbl[lpn] = *ppa;
}

#if 1 //NAM
static inline void set_btbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa, struct ssd_req *node_ptr)
{ 
	ftl_assert(lpn < ssd->sp.tt_pgs);
//	ftl_log("%lld  %ld  %ld    1\n",UNMAPPED_PPA,WRITE_ON_BUFF,ssd->maptbl[lpn].ppa); 
	ssd->maptbl[lpn].bfa = node_ptr;	
//	ftl_log("%lld  %ld  %ld    2\n",UNMAPPED_PPA,WRITE_ON_BUFF,ssd->maptbl[lpn].ppa); 
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
	spp->pgs_maptbl = spp->tt_pgs / _PME; 
#ifdef DAWID_BUFF
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

#ifdef DAWID_BUFF
static void ssd_init_gtd(struct ssd *ssd)
{
	struct ssdparams *spp = &ssd->sp; 

	ssd->gtd = g_malloc0(sizeof(struct ppa) * spp->pgs_maptbl); 
	for (int i = 0; i < spp->pgs_maptbl; i++) { 
		ssd->gtd[i].ppa = UNMAPPED_PPA; 
	}
} 
#endif


#if 0 //fifo
static void buff_init(struct ssd *ssd)
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

	ssd->dMap_list = NULL; 
	ssd->tt_maptbl_dpg = 0; 
	ssd->tt_maptbl_flush_cnt = 0; 
}
#endif 

#ifdef DAWID_BUFF
static void ssd_init_buff(struct ssd *ssd)
{
	struct ssdparams *spp = &ssd->sp; 
	struct ssd_buff *bp = &ssd->buff; 

	/* initialize request group table */
	bp->req_tbl = g_malloc0(sizeof(struct req_tbl_ent) * spp->pgs_maptbl);
	for (uint64_t i = 0; i < spp->pgs_maptbl; i++) { 
		bp->req_tbl[i].idx = i;
	}

	/* initialize buff status */
	bp->tt_reqs = 0;
	bp->zcl = NULL;

	/* initialize dirty bit of maptbl pgs */
	ssd->maptbl_state = g_malloc0(sizeof(int) * spp->pgs_maptbl); 

	ssd->dMap_list = NULL; 
	ssd->tt_maptbl_dpg = 0; 
	ssd->tt_maptbl_flush = 0;

	/* initialize max heap */
	bp->req_max_heap = g_malloc0(sizeof(struct hnode) * spp->pgs_maptbl);
	bp->req_max_heap_size = 0;

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

#ifdef DAWID_BUFF
	/* initialize gtd */
	ssd_init_gtd(ssd);
#endif

    /* initialize all the lines */
    ssd_init_lines(ssd);

    /* initialize write pointer, this is how we allocate new pages for writes */
    ssd_init_write_pointer(ssd);

#ifdef DAWID_BUFF
	/* initialize write buffer */
	ssd_init_buff(ssd); 
#endif

    qemu_thread_create(&ssd->ftl_thread, "FEMU-FTL-Thread", ftl_thread, n,
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

#if 0 //NAM
static inline bool mapped_buff(struct ppa *ppa)
{ 
	return !(ppa->ppa == WRITE_ON_BUFF); 
} 
#endif
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
}

static int do_gc(struct ssd *ssd, bool force)
{
    struct line *victim_line = NULL;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lunp;
    struct ppa ppa;
    int ch, lun;
	
	//ftl_log("do_gc\n");
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

//#if 1 //PFTL
//#if 1 //PROTECTED
#if 0
static void update_dMaptbl_list(struct ssd *ssd, uint64_t fidx)
{ 
	struct dMap_list_node *newNode = (struct dMap_list_node*)malloc(sizeof(struct dMap_list_node)); 

	newNode->idx = fidx; 
	newNode->next = ssd->dMap_list; 

	ssd->dMap_list = newNode; 

	return; 
} 
#endif

#if 0
static uint64_t update_dMaptbl_cond(struct ssd *ssd, uint64_t lba)
{
	struct ssdparams *spp = &ssd->sp; 
	uint64_t fidx = lba >> _PMES; 
	//uint64_t curlat = 0, maxlat = 0; 
	int r; 

	if (ssd->maptbl_state[fidx] & IS_DIRTY) { 
		return 0; 
	} 

	ssd->maptbl_state[fidx] |= IS_DIRTY; 
	update_dMaptbl_list(ssd, fidx); 

	ssd->tt_maptbl_dpg++; 
//	ftl_log("spp->tot_protected_ratio: %d  ssd->tt_maptbl_dpg: %d\n", spp->protected_ratio, ssd->tt_maptbl_dpg);

#if 1 //map flush
	/* mapping flush */
	if (ssd->tt_maptbl_dpg >= spp->pgs_protected) {
		while (ssd->dMap_list != NULL) { 
			while (should_gc_high(ssd)) { 
				/* perform GC here until !should_gc(ssd) */
				r = do_gc(ssd, true); 
				if (r == -1)
					break; 
			} 
	
			struct ppa ppa = get_gtd_ent(ssd, fidx); // lpn >> fidx ??  
	
			/* update old page information first */
			if (mapped_ppa(&ppa)) { 
				mark_page_invalid(ssd, &ppa); 
				set_rmap_ent(ssd, INVALID_LPN, &ppa); 
			} 
			
			/* new wirte */ 
			ppa = get_new_page(ssd); 
			/* update gtd */ 
			set_gtd_ent(ssd, fidx, &ppa); 
			/* update rmap */
			set_rmap_ent(ssd, fidx, &ppa); // maybe we changed rmap struct.. 
	
			mark_page_valid(ssd, &ppa); 
	
			/* need to advance the write pointer here */
			ssd_advance_write_pointer(ssd); 

#if 0
			struct nand_cmd swr; 
			swr.type = USER_IO;
			swr.cmd = NAND_WRITE; 
			swr.stime = stime; 
			/* get latency statistics */
			curlat = ssd_advance_status(ssd, &ppa, &swr); 
			maxlat = (curlat > maxlat) ? curlat : maxlat; 
			// how can I treat this latency .. 
#endif

			struct dMap_list_node* tmp = ssd->dMap_list; 
			ssd->dMap_list = ssd->dMap_list->next; 
			free(tmp);
		}
		ssd->tt_maptbl_dpg = 0; 
		ssd->tt_maptbl_flush++; 
	}
#endif

	return 0; 
}
#endif

#if 0 //fifo buff
static int32_t ssd_buff_flush(struct ssd *ssd)
{
	//uint64_t lat = 0; 
	//vmeRequest *buff_req;
	struct ssd_req *tmp_node; 
	struct ppa ppa; 
	int r; 
	int64_t stime; 
	uint64_t lpn, curlat = 0, maxlat = 0; 

	tmp_node = buff.tail; 
	lpn = buff.tail->lpn;
	stime = buff.tail->stime; 

	/* send request to flash */
	//lat = ssd_write(ssd, buff_req);
	while (should_gc_high(ssd)) { 
		/* perform GC here until !should_gc(ssd) */
		r = do_gc(ssd, true); 
		if (r == -1)
			break; 
	} 

	ppa = get_maptbl_ent(ssd, lpn); 

	/* bug check */ 
	if (!existed_buff(&ppa)) { 
		ftl_log("buff dequeue ERR...\n"); 	
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
	swr.stime = stime; 
	/* get latency statistics */ 
	curlat = ssd_advance_status(ssd, &ppa, &swr); 
	maxlat = (curlat > maxlat) ? curlat : maxlat; 
	//how can I treat this latency .. 

	/* move buff's tail pointer */
	if (buff.head == buff.tail) { 
		buff.head = NULL; 
		buff.tail = NULL; 
	} else {
		buff.tail = buff.tail->prev;
		buff.tail->next = NULL; 
	}

	buff.tot_cnt--; 
	free(tmp_node);

	/* need to write mapping information on flash memory for persist */ 
	update_dMaptbl_cond(ssd, lpn); 

	return 1; 
}

static int32_t buff_write(struct ssd *ssd, NvmeRequest *req)
{
	uint64_t lba = req->slba; 
	struct ssdparams *spp = &ssd->sp; 
	int len = req->nlb; 
	uint64_t start_lpn = lba / spp->secs_per_pg; 
	uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg; 
	struct ppa ppa; 
	uint64_t lpn;

	for (lpn = start_lpn; lpn <= end_lpn; lpn++) { 
		struct ssd_req* newNode = (struct ssd_req*)malloc(sizeof(struct ssd_req)); 
		if (!newNode) { 
			ftl_log("newNode ERR\n"); 
		} 
		
		/* fill the request information */
		newNode->lpn = lpn; 
		newNode->stime = req->stime; 
		newNode->prev = NULL; 
		newNode->next = NULL; 

		/* link the pointer */
		if (buff.head == NULL && buff.tail == NULL) { 
			buff.head = newNode; 
			buff.tail = newNode;
		} else {
			newNode->next = buff.head; 
			buff.head->prev = newNode; 
			buff.head = newNode; 
		} 

		buff.tot_cnt++; 

		/* update mapping information */
		ppa = get_maptbl_ent(ssd, lpn); 
		if (existed_buff(&ppa)) {
			struct ssd_req *oldNode = ppa.buff;
			free(oldNode); 
			buff.tot_cnt--; 
		}

		if (mapped_ppa(&ppa)) { 
			/* update old page information first */
			mark_page_invalid(ssd, &ppa);
			set_rmap_ent(ssd, INVALID_LPN, &ppa); 
		}

		/* update maptbl with node pointer */
		set_btbl_ent(ssd, lpn, &ppa, newNode); 

		/* dequeue some request if buff size is full */ 
		if (buff.tot_cnt == BUFF_THRES) { 
			ssd_buff_flush(ssd); 
		}
	}
	return 0; 
}
#endif


#ifdef DAWID_BUFF
static uint64_t find_maptbl_pg_idx(uint64_t lba)
{ 
	/* find index on map table page associated with lpn */ 
	return lba >> _PMES; 
} 

static void insert_max_heap(struct ssd *ssd, struct hnode nnode)
{ 
	struct ssd_buff* bp = &ssd->buff;
	struct hnode* heap = bp->req_max_heap;	
	uint64_t hsz = bp->req_max_heap_size;

	uint64_t i = ++(hsz);

	while ((i != 1) && (nnode.count > heap[i/2].count)) { 
		heap[i] = heap[i/2]; 
		i /= 2; 
	}
	heap[i] = nnode;

	return; 
}

static void update_max_heap(struct ssd *ssd, uint64_t idx)
{
//	struct ssdparams *spp = &ssd->sp; 
	struct ssd_buff *bp = &ssd->buff;

	struct hnode* heap = bp->req_max_heap;	
	//uint64_t hsz = bp->req_max_heap_size;
	uint64_t i = bp->req_tbl[idx].heap_idx;

	/* update request count for the heap node */
	heap[i].count++;

#if 0
	uint64_t i = 1; 
	struct hnode tmp;

	// search max heap 
	while (h.heap[i].idx != idx) { 
		i++; 
		if (i == spp->pgs_maptbl) { 
			struct hnode newElement; 

			newElement.idx = idx; 
			newElement.count = 1; 

			insert_max_heap(newElement); 

			return; 
		}
	}
#endif

	/* go upwards if needed */ 
	struct hnode tmp;

	if (heap[i].count > heap[i/2].count) { 
		tmp = heap[i]; 
		while ((i != 1) && (heap[i].count > heap[i/2].count)) { 
			heap[i] = heap[i/2]; 
			i /= 2; 
		} 
		heap[i] = tmp; 
	}
	return; 
}

#if 0
static void delete_max_heap(void) 
{
	uint64_t parent, child; 
	struct hnode temp; 

//	item = h.heap[1]; 
	if (h.heap_size == 0)
		return; 

	temp = h.heap[(h.heap_size)--]; 
	parent = 1;
	child = 2; 

	while (child <= h.heap_size) {
		if ((child < h.heap_size) && ((h.heap[child].count) < h.heap[child + 1].count)) 
			child++; 

		if (temp.count >= h.heap[child].count)
			break; 

		h.heap[parent] = h.heap[child];
		parent = child; 
		child *= 2; 
	}

	h.heap[parent] = temp;
	return; 
}
#endif

static int32_t is_maptbl_pg_dirty(struct ssd *ssd, uint64_t idx) 
{ 
	return ssd->maptbl_state[idx] & IS_DIRTY? 1 : 0; 
}

static void add_to_zcl(struct ssd *ssd, uint64_t idx)
{ 
	if(ssd->maptbl_state[idx] & EXIST_IN_ZCL)
		return; 

	struct ssd_buff* bp = &ssd->buff;

	struct zcl_node* nn = g_malloc0(sizeof(struct zcl_node)); 
	nn->idx = idx; 
	nn->next = bp->zcl; 
	bp->zcl = nn; 

	ssd->maptbl_state[idx] |= EXIST_IN_ZCL; 
	return; 
} 

//static void pageIsClean(struct ssd *ssd, uint64_t idx)
static void add_to_heap(struct ssd *ssd, uint64_t idx)
{
	struct ssd_buff* bp = &ssd->buff;

	/* Insert a new hnode into the max heap if the first request
 	 * falling into the maptbl pg idx occurs */
	if(!bp->req_tbl[idx].head) {
		struct hnode nn; 
		nn.idx = idx; 
		nn.count = 1; 
		insert_max_heap(ssd, nn); 
		return;
	}
	
	/* Increase the number of requests for the maptbl page */
	update_max_heap(ssd, idx); 

	return; 
}
#endif

#ifdef DAWID_BUFF
static int32_t ssd_buff_flush(struct ssd *ssd)
{

#if 0 
	struct node_dirty *dNode;
	struct ssd_req *tmp_node; 
	struct ppa ppa; 
	int r, cmax = -1, imax = -1; 
	int64_t stime; 
	uint64_t lpn, curlat = 0, maxlat = 0; 


	/* flush maptbl pages with zero cost first */
	while (zcl != NULL) { 
		dNode = zcl; 

		cmax = buff.htable[dNode->idx].count; 
		imax = dNode->idx; 

		buff.htable[imax].count -= cmax; 
		buff.tot_cnt -= cmax; 

		while (buff.htable[imax].head != NULL) { 
			tmp_node = buff.htable[imax].head; 
			
			/* send request from dram buffer to nand flash */ 
			//tmp_node = buff.tail; 
			lpn = buff.htable[imax].head->lpn; 
			stime = buff.htable[imax].head->stime; 

			while (should_gc_high(ssd)) { 
				r = do_gc(ssd, true); 
				if (r == -1) 
					break; 
			} 

			ppa = get_maptbl_ent(ssd, lpn); 

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
			swr.stime = stime; 
			/* get latency statistics */ 
			curlat = ssd_advance_status(ssd, &ppa, &swr); 
			maxlat = (curlat > maxlat) ? curlat : maxlat; 
			//ftl_log("maxlat: %ld\n", maxlat);
 
			/* move the next req in hash bucket */ 
			buff.htable[imax].head = tmp_node->next; 
			free(tmp_node);

			/* need to write mapping information on flash memory for persist */ 
			update_dMaptbl_cond(ssd, lpn); 
		} 
		zcl = dNode->next; 
		free(dNode); 
	}

	if (h.heap_size >= 1) { 
		cmax = h.heap[1].count; 
		imax = h.heap[1].idx; 

		if (buff.htable[imax].count == 0) { 
			delete_max_heap(); 
			cmax = h.heap[1].count; 
			imax = h.heap[1].idx;
		} 
		buff.htable[imax].count = 0; 
		buff.tot_cnt -= cmax; 

		while (buff.htable[imax].head != NULL) { 
			tmp_node = buff.htable[imax].head; 

			/* send request from dram buffer to nand flash */ 
			//tmp_node = buff.tail; 
			lpn = buff.htable[imax].head->lpn; 
			stime = buff.htable[imax].head->stime; 

			while (should_gc_high(ssd)) { 
				r = do_gc(ssd, true); 
				if (r == -1) 
					break; 
			} 

			ppa = get_maptbl_ent(ssd, lpn); 

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
			swr.stime = stime; 
			/* get latency statistics */ 
			curlat = ssd_advance_status(ssd, &ppa, &swr); 
			maxlat = (curlat > maxlat) ? curlat : maxlat; 

			/* move the next req in hash bucket */ 
			buff.htable[imax].head = tmp_node->next; 
			free(tmp_node); 

			/* need to write mapping information on flash memory for persist */
			update_dMaptbl_cond(ssd, lpn);

		}
		delete_max_heap(); 
		return imax;
	}

	return -1; 
#endif
	return 0;
} 
#endif

#ifdef DAWID_BUFF
static int32_t ssd_buff_write(struct ssd *ssd, NvmeRequest *req)
{ 
	struct ssdparams *spp = &ssd->sp; 
	struct ssd_buff *bp = &ssd->buff; 

	uint64_t lba = req->slba; 
	int len = req->nlb; 
	uint64_t start_lpn = lba / spp->secs_per_pg; 
	uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg; 
	struct ppa ppa; 
	uint64_t lpn; 

	for (lpn = start_lpn; lpn <= end_lpn; lpn++) { 
		/* find map index */
		uint64_t idx = find_maptbl_pg_idx(lpn); 
		
		/* check map page condition */ 
		if (is_maptbl_pg_dirty(ssd, idx)) { 
			add_to_zcl(ssd, idx);
		} else { 
			add_to_heap(ssd, idx); 
		} 

		/* insert ssd request into request table */	
		struct ssd_req* newNode = g_malloc0(sizeof(struct ssd_req)); 

		if (!newNode) { 
			ftl_log("newNode ERR\n"); 
		} 

		/* fill the request information */
		newNode->lpn = lpn; 
		newNode->stime = req->stime; 
		newNode->prev = NULL;
		newNode->next = NULL; 

		/* link the pointer */ 
		struct req_tbl_ent* ep = &bp->req_tbl[idx];

		newNode->next = ep->head; 
		ep->head = newNode; 
		bp->tt_reqs++;


		/******************************/
		/* update mapping information */ 
		/******************************/
		ppa = get_maptbl_ent(ssd, lpn); 

		if (mapped_ppa(&ppa)) { 
			/* update old page information first */
			mark_page_invalid(ssd, &ppa); 
			set_rmap_ent(ssd, INVALID_LPN, &ppa); 
		} 

		/* update maptbl with node pointer */
		set_btbl_ent(ssd, lpn, &ppa, newNode); 

		/* flush data buffer */ 
		if (bp->tt_reqs > BUFF_SIZE) { 
			ssd_buff_flush(ssd); 
		} 
	} 
	return 0; 
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
#ifdef DAWID_BUFF
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
#if 0 //PFTL
int16_t get_buff_tot_cnt(void)
{
	return buff.tot_cnt;
}	
#endif
