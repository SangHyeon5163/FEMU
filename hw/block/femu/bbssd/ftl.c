#include "ftl.h"/* dirty maptbl pg node */
int tmp1 = 0; 
int tmp2 = 0;
int hp_cnt = 0;
int zl_cnt = 0;
int r_hp_cnt = 0; 
int r_zl_cnt = 0; 
int bp_cnt = 0;
int host_rq_cnt = 0;
int trt_rq_cnt = 0;
int debug_cnt = 0;

static void *ftl_thread(void *arg);
static void *ftl_flush_thread(void *arg);

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

#ifdef USE_BUFF
static inline uint64_t get_g_rmap_ent(struct ssd *ssd, struct ppa *ppa)
{ 
	uint64_t pgidx = ppa2pgidx(ssd, ppa); 
	
	return ssd->g_rmap[pgidx];
} 

static inline void set_g_rmap_ent(struct ssd *ssd, uint64_t idx, struct ppa *ppa)
{ 
	uint64_t pgidx = ppa2pgidx(ssd, ppa);

	ssd->g_rmap[pgidx] = idx; 
} 
#endif
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

#ifdef CHECK_DPG
static void check_dpg_maptbl_ent(struct ssd *ssd, uint64_t idx){ 
	struct ssd_buff *bp = &ssd->buff; 
	struct dpg_tbl_ent* ep = &bp->dpg_tbl[idx]; 	
	uint64_t hp_cnt;
	uint64_t zcl_cnt;
	struct dpg_node* pt = ep->head;
	uint64_t check_cnt = 0; 

	if (ep->heap_idx != 0)
		hp_cnt = bp->mpg_value_heap->heap[ep->heap_idx].dpg_cnt; 
	else 
		hp_cnt = 0;

	if (ep->znp) 
		zcl_cnt = ep->znp->dpg_cnt; 
	else 
		zcl_cnt = 0; 

	while (pt) {	
		check_cnt++; 
		
		if (!pt->next) 
			break; 

		pt = pt->next; 
	}

	if (hp_cnt + zcl_cnt == check_cnt)
		ftl_log("No problem. hp_cnt: %ld, zcl_cnt: %ld\n", hp_cnt, zcl_cnt);
	else 
		ftl_log("Problem. hp_cnt: %ld, zcl_cnt: %ld\n", hp_cnt, zcl_cnt);

	return;
} 
#endif 

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
    spp->blks_per_pl = 1024; /* 16GB */
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
	ftl_log("ssd_init_params: %d  spp->tt_pgs: %d\n", spp->pgs_maptbl, spp->tt_pgs);
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
static void ssd_init_g_rmap(struct ssd *ssd)
{ 
	struct ssdparams *spp = &ssd->sp; 
	
	ssd->g_rmap = g_malloc0(sizeof(uint64_t) * spp->tt_pgs);
	for (int i = 0; i < spp->tt_pgs; i++) { 
		ssd->g_rmap[i] = INVALID_LPN;
	} 
} 

static void ssd_init_gtd(struct ssd *ssd)
{
	//ftl_log("ssd_init_gtd ...\n");
	struct ssdparams *spp = &ssd->sp; 

	ftl_log("ssd_init_gtd ...   %d\n", spp->pgs_maptbl);
	ssd->gtd = g_malloc0(sizeof(struct ppa) * spp->pgs_maptbl); 
	for (int i = 0; i < spp->pgs_maptbl; i++) { 
		ssd->gtd[i].ppa = UNMAPPED_PPA;
		if(ssd->gtd[i].ppa != UNMAPPED_PPA)
			ftl_log("err\n"); 
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

static void ssd_init_buff(struct ssd *ssd)
{
	ftl_log("ssd_init_buff ... \n");
	struct ssdparams *spp = &ssd->sp; 
	struct ssd_buff *bp = &ssd->buff; 

#ifdef DAWID
	/* initialize request group table */
	bp->dpg_tbl = g_malloc0(sizeof(struct dpg_tbl_ent) * spp->pgs_maptbl);
	for (uint64_t i = 0; i < spp->pgs_maptbl; i++) { 
		bp->dpg_tbl[i].head = NULL;
		bp->dpg_tbl[i].mpg_idx = i;
		bp->dpg_tbl[i].heap_idx = 0;
		bp->dpg_tbl[i].znp = NULL; 
	}
#endif

	/* initialize buff status */
	bp->cond_chip = (struct cond_chip*)malloc(sizeof(struct cond_chip));
	bp->cond_chip->idle = 1;

	bp->status = (struct status*)malloc(sizeof(struct status));
//	qemu_mutex_lock(&bp->status->lock); 
	bp->status->tt_reqs = 0;
	bp->bp_reqs = 0;
//	qemu_mutex_unlock(&bp->status->lock); 
#ifdef DAWID
	bp->zcl = NULL;
#endif

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
	qemu_mutex_init(&bp->cond_chip->lock);

	/* initialize dirty bit of maptbl pgs */
	ssd->maptbl_state = g_malloc0(sizeof(int) * spp->pgs_maptbl); 
	//ssd->toFlush_state = g_malloc0(sizeof(int) *spp->pgs_maptbl);

	/* initialize toFlush_state to manage toFlush and Flush list */ 
	ssd->tf = (struct toFlush_comp*)malloc(sizeof(struct toFlush_comp));

	ssd->tf->toFlush_state = g_malloc0(sizeof(int) * spp->pgs_maptbl);
	qemu_mutex_init(&ssd->tf->lock);  

	ssd->dmpg_list = NULL; 
	ssd->tt_maptbl_dpg = 0; 
	ssd->tt_maptbl_flush = 0;
	ssd->tt_maptbl_flush_pgs = 0; 
	ssd->tt_user_dat_flush_pgs = 0; 
	ssd->gc_maptbl_flush_pgs = 0; 
	ssd->gc_user_dat_flush_pgs = 0; 

#ifdef DAWID
	/* initialize max heap */
	bp->mpg_value_heap = g_malloc0(sizeof(struct max_heap));
	bp->mpg_value_heap->heap = g_malloc0(sizeof(struct hnode) * spp->pgs_maptbl);
	bp->mpg_value_heap->hsz = 0;
#endif 

	return; 
} 

#if 0
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

	/* initialize g_rmap */ 
	ssd_init_g_rmap(ssd); 

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
#if 1
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time; 
#endif
	//ftl_log("next: %ld  cmd_stime: %ld\n", lun->next_lun_avail_time, cmd_stime);
	//nand_stime = cmd_stime;
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
#if 0
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
#endif
	nand_stime = cmd_stime; 
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

	//ftl_log("1\n");
    /* update corresponding page status */
    pg = get_pg(ssd, ppa);
    //ftl_log("mark_page_invalid: %d\n", pg->status); 
    ftl_assert(pg->status == PG_VALID);
    pg->status = PG_INVALID;

	//ftl_log("2\n");
    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
    blk->ipc++;
    ftl_assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
    blk->vpc--;

	//ftl_log("3\n");
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

#ifdef USE_BUFF
static void gc_write_user_page(struct ssd *ssd, uint64_t lpn) 
{ 
	struct ppa new_ppa; 
	struct nand_lun *new_lun; 
	
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
	new_lun = get_lun(ssd, &new_ppa); 
	new_lun->gc_endtime = new_lun->next_lun_avail_time; 

	return; 	
} 

static void gc_write_maptbl_page(struct ssd *ssd, uint64_t idx) 
{ 
	struct ppa new_ppa; 
	struct nand_lun *new_lun; 

	new_ppa = get_new_page(ssd); 
	/* update gtd */ 
	set_gtd_ent(ssd, idx, &new_ppa); 
	/* update g_rmap */ 
	set_g_rmap_ent(ssd, idx, &new_ppa); 

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
	new_lun = get_lun(ssd, &new_ppa); 
	new_lun->gc_endtime = new_lun->next_lun_avail_time; 

	return; 	
} 

static uint64_t gc_write_page(struct ssd *ssd, struct ppa *old_ppa)
{
	//struct ppa new_ppa; 
	//struct nand_lun *new_lun; 
	uint64_t lpn = get_rmap_ent(ssd, old_ppa); 

	if (lpn != INVALID_LPN) { 
		gc_write_user_page(ssd, lpn);
		ssd->gc_user_dat_flush_pgs++; 
		/* set rmap entry INVALID_LPN */
		set_rmap_ent(ssd, INVALID_LPN, old_ppa); 	 
	} else { 
		uint64_t idx = get_g_rmap_ent(ssd, old_ppa); 
		gc_write_maptbl_page(ssd, idx);
		ssd->gc_maptbl_flush_pgs++;
		/* set g_rmap entry INVALID_LPN */ 
		set_g_rmap_ent(ssd, INVALID_LPN, old_ppa);  
	} 

	return 0; 	
} 
#endif

/* move valid page data (already in DRAM) from victim line to a new page */
#if 0
static uint64_t gc_write_page(struct ssd *ssd, struct ppa *old_ppa)
{
    struct ppa new_ppa;
    struct nand_lun *new_lun;
//    uint64_t lpn = get_rmap_ent(ssd, old_ppa);
	uint64_t lpn, idx; 

	/* */ 
	lpn = get_rmap_ent(ssd, old_ppa); 
	// 없으면,, get_g_rmap_ent() 체크

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
#endif

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
#if 0 
	    	set_rmap_ent(ssd, INVALID_LPN, ppa);
#endif
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
    
    //ftl_log("check1\n");
    ppa.g.blk = victim_line->id;
    ftl_debug("GC-ing line:%d,ipc=%d,victim=%d,full=%d,free=%d\n", ppa.g.blk,
              victim_line->ipc, ssd->lm.victim_line_cnt, ssd->lm.full_line_cnt,
              ssd->lm.free_line_cnt);

    /* copy back valid data */
    //ftl_log("check2\n");
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
    //ftl_log("check3\n"); 
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

#if 0
    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64", end_lpn=%"PRIu64", tt_pgs=%d\n", start_lpn, end_lpn, ssd->sp.tt_pgs);
    }
#endif	
    if (end_lpn > spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64", end_lpn=%"PRIu64", tt_pgs=%d\n", start_lpn, end_lpn, ssd->sp.tt_pgs);
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
	uint64_t idx = get_mpg_idx(lba);
	uint64_t accumulat = 0; 

	/* Update toFlush_state for timing error(?) */ 
	qemu_mutex_lock(&ssd->tf->lock);
	ssd->tf->toFlush_state[idx]--;
	qemu_mutex_unlock(&ssd->tf->lock);

	/*** do program ***/
	/* clean or dirty check */
	if (ssd->maptbl_state[idx] & (1 << DIRTY_BIT_SHIFT)) // Dirty Condition
		goto point;
	
	// Clean Condition
	/* set the maptbl pg dirty */ 
	ssd->maptbl_state[idx] |= (1 << DIRTY_BIT_SHIFT); 

point: 
	if (ssd->tf->toFlush_state[idx] == 0) { 
		/* add to dirty maptbl page list */ 
		add_to_dirty_mpg_list(ssd, idx); 
		/* increment the number of dirty maptbl pages */ 
		ssd->tt_maptbl_dpg++; 
	}  

	
	/*** ***/ 

#if 0
	/* Nothing to do if maptbl page is dirty */
	if (ssd->maptbl_state[idx] & (1 << DIRTY_BIT_SHIFT)) { 
		return 0; 
	} 
	
	/* set the maptbl pg dirty */
	ssd->maptbl_state[idx] |= (1 << DIRTY_BIT_SHIFT); 

	/* add to dirty maptbl page list */ 
	add_to_dirty_mpg_list(ssd, idx); 

	/* increment the number of dirty maptbl pages */ 
	ssd->tt_maptbl_dpg++; 

#ifdef USE_BUFF_DEBUG_L1
	ftl_log("tt_maptbl_dpgs = %d protected = %d\n", ssd->tt_maptbl_dpg, spp->pgs_protected);
#endif
#endif
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

	return i; 
}

static void update_max_heap(struct ssd *ssd, uint64_t idx)
{
	struct ssd_buff *bp = &ssd->buff;
	struct hnode* heap = bp->mpg_value_heap->heap;	
	uint64_t i = bp->dpg_tbl[idx].heap_idx;

	/* update request dpg_cnt for the heap node */
	heap[i].dpg_cnt++;
	
	/* go upwards if needed */
	struct hnode hn = heap[i];

	while ((i != 1) && (heap[i].dpg_cnt > heap[i/2].dpg_cnt)) { 
		heap[i] = heap[i/2]; 
		bp->dpg_tbl[heap[i/2].mpg_idx].heap_idx = i;
		i /= 2; 
	} 
	heap[i] = hn; 
	bp->dpg_tbl[hn.mpg_idx].heap_idx = i;

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

	if(ssd->maptbl_state[idx] & (1 << DIRTY_BIT_SHIFT)) {
#ifdef DEBUG_CHECK_DPG
		ftl_log("idx: %ld  1\n", idx);
#endif
		return 1;
	} else {
#ifdef DEBUG_CHECK_DPG
		ftl_log("idx: %ld  2\n", idx); 
#endif
		return 0;
	} 

//	return ssd->maptbl_state[idx] & DIRTY_BIT_SHIFT? 1 : 0; 
}

static void add_to_zcl(struct ssd *ssd, uint64_t midx)
{ 

	struct ssd_buff* bp = &ssd->buff;

	if(ssd->maptbl_state[midx] & (1 << EXIST_IN_ZCL_BIT_SHIFT)) {
		struct zcl_node *np = bp->dpg_tbl[midx].znp; 
		np->dpg_cnt++; 
		goto out;
	} 


	struct zcl_node* nn = g_malloc0(sizeof(struct zcl_node)); 
	assert(nn != NULL);

	nn->mpg_idx = midx; 
	nn->dpg_cnt = 1; 
	nn->next = bp->zcl; 

	bp->zcl = nn;
	bp->dpg_tbl[midx].znp = nn;

	ssd->maptbl_state[midx] |= (1 << EXIST_IN_ZCL_BIT_SHIFT); 

out:
	return; 
} 

static void add_to_heap(struct ssd *ssd, uint64_t mpg_idx)
{
	struct ssd_buff* bp = &ssd->buff;
	/* check_dpg_maptbl_ent bug fixing */
	//struct hnode* heap = bp->mpg_value_heap->heap; 
	/* Insert a new hnode into the max heap for the first request
 	 * falling into the maptbl pg idx */
//	if(!bp->dpg_tbl[mpg_idx].head) {
	if(bp->dpg_tbl[mpg_idx].heap_idx == 0) {
		//ftl_log("11111\n");
		tmp1++;
		struct hnode nn; 
		nn.mpg_idx = mpg_idx; 
		nn.dpg_cnt = 1; 
		insert_max_heap(ssd, nn); 
	} else {
		//ftl_log("22222\n");
		/* Increase the number of requests for the maptbl page */
		tmp2++;
		update_max_heap(ssd, mpg_idx); 
	}
	
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

	//ftl_log("3-1\n");
	while (should_gc_high(ssd)) { 
		r = do_gc(ssd, true); 
		if (r == -1) 
			break; 
	} 
	//ftl_log("3-2\n");
	ppa = get_maptbl_ent(ssd, lpn); 

	/* new write */
	ppa = get_new_page(ssd); 
	/* update maptbl */ 
#if 0 //org
	//sample.. // lock 
	ssd->maptbl[lpn].bfa = INEXIST_BUFF;

	set_maptbl_ent(ssd, lpn, &ppa); 
#endif
	if (ssd->maptbl[lpn].bfa == dpg) { 
		/* set inexisted bit */
		ssd->maptbl[lpn].bfa = INEXIST_BUFF; 
		/* update maptbl ppa info */ 
		set_maptbl_ent(ssd, lpn, &ppa); 
	} 

	/* update rmap */ 
	set_rmap_ent(ssd, lpn, &ppa); 

	mark_page_valid(ssd, &ppa); 
	
	//ftl_log("3-3\n");
	/* need to advance the write pointer here */ 
	ssd_advance_write_pointer(ssd); 
	
	//ftl_log("3-1\n");
	struct nand_cmd swr; 
	swr.type = USER_IO; 
	swr.cmd = NAND_WRITE; 
	swr.stime = 0; // set zero for buffer data write
	//ftl_log("3-4\n");
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
	uint64_t idx = 0;
	uint64_t zcl_dpg_cnt = 0; 
	uint64_t hp_dpg_cnt = 0;
	uint64_t check_cnt;
	struct dpg_node* dpg;

	qemu_mutex_lock(&bp->dpg_to_flush_list->lock);
	/* flush data pages of which maptbl pages are in zcl */
	while (znp!= NULL) { 
		idx = znp->mpg_idx;  // idx for request table
		zcl_dpg_cnt = znp->dpg_cnt;
		struct dpg_tbl_ent* ep = &bp->dpg_tbl[idx];

		check_cnt = 0;

		trt_rq_cnt += zcl_dpg_cnt; 
		/* send request from dram buffer to nand flash */ 
		for (int i = 0; i < zcl_dpg_cnt; i++) { 
		//for (int i = 0; i < 1; i++) { 
			if (ep->head == NULL){
				ftl_log("zcl node err..\n");
				break;
			}

			dpg = ep->head;
		
			if (!ep->head->next) {
				ep->head = NULL; 
			} else {  
				ep->head = ep->head->next;  
			} 

			dpg->next = NULL;

			if (!bp->dpg_to_flush_list->head && !bp->dpg_to_flush_list->tail) { 
				bp->dpg_to_flush_list->head = dpg; 
				bp->dpg_to_flush_list->tail = dpg;
			} else { 
				bp->dpg_to_flush_list->tail->next = dpg;
				bp->dpg_to_flush_list->tail = bp->dpg_to_flush_list->tail->next; 
			}
			bp->dpg_to_flush_list->reqs++;
			check_cnt++;
			//trt_rq_cnt++;	
			bp->bp_reqs--;
			bp_cnt--;
			zl_cnt++; 
		} 

		if (zcl_dpg_cnt != check_cnt)
			ftl_log("flush data pages err in heap..\n");

		/* update toFlush_state for timing error? */
		qemu_mutex_lock(&ssd->tf->lock);
		ssd->tf->toFlush_state[znp->mpg_idx] += check_cnt;	
		qemu_mutex_unlock(&ssd->tf->lock);	

		ep->znp = NULL; // check_dpg_maptbl_ent bug fixing	
		/* remove znode from zcl */
		bp->zcl = bp->zcl->next;
		
		ssd->maptbl_state[znp->mpg_idx] &= ~(1 << EXIST_IN_ZCL_BIT_SHIFT); // off
		free(znp); 

		znp = bp->zcl;
	}

	if (bp->dpg_to_flush_list->reqs > 64) 
		goto ret;	
	
	if (bp->mpg_value_heap->hsz >= 1) { 
		struct hnode* max_node = pop_max_heap(ssd); 

		assert(max_node);
		idx = max_node->mpg_idx;
		hp_dpg_cnt = max_node->dpg_cnt; 

		trt_rq_cnt += hp_dpg_cnt; 
#ifdef DEBUG_CHECK_DPG
		ftl_log("max_node->heap_idx: %ld\n", idx);
#endif
		check_cnt = 0;
		struct dpg_tbl_ent* ep = &bp->dpg_tbl[idx];

		/* send request from dram buffer to nand flash */ 
		for (int i = 0; i < hp_dpg_cnt; i++) { 
		//for (int i = 0; i < 1; i++) { 
			if (ep->head == NULL){
				ftl_log("heap node err..\n");
				break;
			} 

			dpg = ep->head; 

			if (!ep->head->next) { 
				ep->head = NULL; 
			} else { 
				ep->head = ep->head->next; 
			} 

			dpg->next = NULL;
			
			if (!bp->dpg_to_flush_list->head & !bp->dpg_to_flush_list->tail) { 
				bp->dpg_to_flush_list->head = dpg; 
				bp->dpg_to_flush_list->tail = dpg; 
			} else { 
				bp->dpg_to_flush_list->tail->next = dpg; 
				bp->dpg_to_flush_list->tail = bp->dpg_to_flush_list->tail->next; 
			}
			bp->dpg_to_flush_list->reqs++;
			check_cnt++;
			//trt_rq_cnt++;
			bp->bp_reqs--;
			bp_cnt--;
			hp_cnt++; 
		}	

		ep->heap_idx = 0;	// check_dpg_maptbl_ent bug fixing	
		if (hp_dpg_cnt != check_cnt)
			ftl_log("flush data pages err in heap..\n");
		
		/* update toFlush_state for timing error? */
		qemu_mutex_lock(&ssd->tf->lock);
		ssd->tf->toFlush_state[idx] += check_cnt; 
		qemu_mutex_unlock(&ssd->tf->lock);
	} 	

ret: 	
	qemu_mutex_unlock(&bp->dpg_to_flush_list->lock);
	//ftl_log("trt_rq_cnt: %d\n", trt_rq_cnt);	

	return 0;
}

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
#ifdef DEBUG_FUNC
	uint64_t debug_stime = 0, debug_etime = 0;
#endif

	/* Walk through all pages for the request */
	for (lpn = start_lpn; lpn <= end_lpn; lpn++) { 
		/* calculate the cost of each maptble page
		 * and maintain pages accordingly. 
		 * zero cost list: 
		 * heap:  */
		//ftl_log("lpn: %ld\n", lpn); 
#ifdef DEBUG_FUNC
		debug_stime = qemu_clock_get_ns(QEMU_CLOCK_REALTIME); 
#endif 
		uint64_t idx = get_mpg_idx(lpn); 	
		//check_dpg_maptbl_ent(ssd, idx);

		if (is_maptbl_pg_dirty(ssd, idx)) {
			r_zl_cnt++; 
			add_to_zcl(ssd, idx);
		} else {
			r_hp_cnt++; 
			add_to_heap(ssd, idx); 
		} 
		/* insert ssd request into request table */	
		/* request table is used to flush data buffer associated with the
 		 * maptbl page */
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
		bp_cnt++;
		bp->bp_reqs++;
		qemu_mutex_lock(&bp->status->lock); 
		bp->status->tt_reqs++;
		qemu_mutex_unlock(&bp->status->lock);

		//ftl_log("host_rq_cnt: %d\n", ++host_rq_cnt);
		/* update mapping table entry to point to buffer address */ 
		ppa = get_maptbl_ent(ssd, lpn); 

		//ftl_log("check3\n");
		if (mapped_ppa(&ppa)) { 
			/* update old page information first */
			mark_page_invalid(ssd, &ppa); 
			set_rmap_ent(ssd, INVALID_LPN, &ppa); 
		} 

		/* update maptbl entry with buffer address */
		set_maptbl_ent_with_buff(ssd, lpn, nn); 

		//usleep(10);
#ifdef CHECK_DPG
		check_dpg_maptbl_ent(ssd, idx);
#endif
		/* flush data buffer */ 
	//	while (bp->status->tt_reqs > 64 && bp->dpg_flush_list->reqs <= 64) { 
		//ftl_log("bp: %d, hsz: %ld, cond_chip: %d\n", bp->status->tt_reqs, bp->mpg_value_heap->hsz, bp->cond_chip->idle);
		//while (bp->status->tt_reqs > 65536 && bp->mpg_value_heap->hsz >= 1 && bp->cond_chip->idle == 1) {
		//ftl_log("tt_reqs: %d, cond_chip: %d\n", bp->status->tt_reqs, bp->cond_chip->idle);
		//while (bp->status->tt_reqs > 64 && bp->cond_chip->idle == 1) {
		while (bp->status->tt_reqs > 524288 && bp->cond_chip->idle == 1) { 
			debug_cnt++;
			//ftl_log("debug_cnt: %d\n", debug_cnt);
			maxlat = ssd_buff_flush(ssd); 
			//ftl_log("010011010\n");
			if (bp->status->tt_reqs < BUFF_SIZE) 
				break; 
		
			//ftl_log("r_zl: %d zl: %d, r_hp: %d hp: %d, tmp1: %d, tmp2: %d\n", r_zl_cnt, zl_cnt, r_hp_cnt, hp_cnt, tmp1, tmp2);
		}
		
		//ftl_log("r_zl: %d zl: %d, r_hp: %d hp: %d, host_rq_cnt: %d\n", r_zl_cnt, zl_cnt, r_hp_cnt, hp_cnt, ++host_rq_cnt);
		//ftl_log("r_zl: %d zl: %d, r_hp: %d hp: %d, tmp1: %d, tmp2: %d\n", r_zl_cnt, zl_cnt, r_hp_cnt, hp_cnt, tmp1, tmp2);
#ifdef DEBUG_FUNC
		debug_etime = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
		ftl_log("time: %ld\n", debug_etime - debug_stime); 
#endif
	} 
	return maxlat/64; 
} 
#endif

#ifdef FIFO
static uint64_t ssd_buff_flush(struct ssd *ssd)
{ 
	struct ssd_buff *bp = &ssd->buff; 
	struct dpg_node *dpg; 

	qemu_mutex_lock(&bp->dpg_to_flush_list->lock);
	
	if (!bp->head) {
		qemu_mutex_unlock(&bp->dpg_to_flush_list->lock);  
		return 0;
	}

	debug_cnt++;	

#if 0
//	ftl_log("11\n"); 
	if (bp->dpg_to_flush_list->tail) {
//		ftl_log("111\n"); 
		bp->dpg_to_flush_list->tail->next = bp->head;
//		ftl_log("222 %p\n", bp->head); 
		bp->head->prev = bp->dpg_to_flush_list->tail;
//		ftl_log("333\n");
		bp->dpg_to_flush_list->tail = bp->tail;  
	} else {
		bp->dpg_to_flush_list->head = bp->head; 
		bp->dpg_to_flush_list->tail = bp->tail; 	
	}
#endif
	while (bp->head) { 
		dpg = bp->head; 
	
		if (!bp->head->next)
			bp->head = NULL; 
		else 
			bp->head = bp->head->next; 

		dpg->next = NULL; 
	
		if (!bp->dpg_to_flush_list->head && !bp->dpg_to_flush_list->tail) { 
			bp->dpg_to_flush_list->head = dpg; 
			bp->dpg_to_flush_list->tail = dpg; 
		} else { 
			bp->dpg_to_flush_list->tail->next = dpg; 
			bp->dpg_to_flush_list->tail = bp->dpg_to_flush_list->tail->next; 
		}

		bp->dpg_to_flush_list->reqs++; 
		bp->bp_reqs--;
		
		//ftl_log("!!\n"); 
	}  
	//ftl_log("$$\n");
	
//	ftl_log("22\n");
	//bp->dpg_to_flush_list->reqs += bp->bp_reqs; 
	
//	ftl_log("33\n");
	//bp->head = NULL; 
	bp->tail = NULL;
	//bp->bp_reqs = 0;  
	
 	qemu_mutex_unlock(&bp->dpg_to_flush_list->lock); 

	return 0;  
} 

static uint64_t ssd_buff_write(struct ssd *ssd, NvmeRequest *req)
{ 
	//ftl_log("ssd_buff_write_with_fifo...\n");
	struct ssdparams *spp = &ssd->sp; 
	struct ssd_buff *bp = &ssd->buff; 

	uint64_t lba = req->slba; 
	int len = req->nlb; 
	uint64_t start_lpn = lba / spp->secs_per_pg; 
	uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg; 
	struct ppa ppa; 
	uint64_t lpn; 

	uint64_t maxlat = 0;
#ifdef DEBUG_FUNC
	uint64_t debug_stime = 0, debug_etime = 0;
#endif

	for (lpn = start_lpn; lpn <= end_lpn; lpn++) { 
#if 0
		ftl_log("1\n");
#endif
#ifdef DEBUG_FUNC
		debug_stime = qemu_clock_get_ns(QEMU_CLOCK_REALTIME); 
#endif
		struct dpg_node *nn = g_malloc0(sizeof(struct dpg_node)); 	
		if (!nn) { 
			ftl_log("nn ERR\n");
		} 
	
#ifdef FG_DEBUG
		ftl_log("2\n");	
#endif
		/* fill the request information */
		nn->lpn = lpn; 
		nn->stime = req->stime; 
		nn->prev = NULL; 
		nn->next = NULL; 
		//ftl_log("nn->lpn: %ld\n", nn->lpn);
	
#ifdef FG_DEBUG
		ftl_log("3\n");
#endif
		/* insert a new request into the request linked list */ 
		if (!bp->head && !bp->tail) { 
			//ftl_log("1111\n");
			bp->head = nn; 
			bp->tail = nn;
		} else {
			//ftl_log("2222\n"); 
			nn->next = bp->head; 
			bp->head->prev = nn; 
			bp->head = nn; 
		} 

#ifdef FG_DEBUG
		ftl_log("4\n");
#endif
		/* increase number of requests in buffer */ 
		qemu_mutex_lock(&bp->status->lock); 
		bp->status->tt_reqs++; 		
		qemu_mutex_unlock(&bp->status->lock);
		bp->bp_reqs++;
	
#ifdef FG_DEBUG
		ftl_log("5\n");
#endif
		/* update mapping table entry to point to buffer address */ 
		ppa = get_maptbl_ent(ssd, lpn); 

		if (mapped_ppa(&ppa)) { 
			/* update old page information first */ 
			mark_page_invalid(ssd, &ppa); 
			set_rmap_ent(ssd, INVALID_LPN, &ppa); 
		} 

		/* update maptbl entry with buffer address */
		set_maptbl_ent_with_buff(ssd, lpn, nn);	

#ifdef FG_DEBUG
		ftl_log("6\n");
#endif
		/* flush data buffer */ 
		//ftl_log("tt_reqs: %d, cond_chip: %d\n", bp->status->tt_reqs, bp->cond_chip->idle);
#if 1 
		//while (bp->status->tt_reqs > 64 && bp->cond_chip->idle == 1) { 
		while (bp->status->tt_reqs > 524288 && bp->cond_chip->idle == 1) { 
			//debug_cnt++; 
			//ftl_log("debug_cnt: %d\n", debug_cnt);
			//ftl_log("%d\n", bp->status->tt_reqs);
			maxlat = ssd_buff_flush(ssd);
			//ftl_log("3\n"); 
			if (bp->status->tt_reqs < BUFF_SIZE) 
				break; 
		} 
#ifdef DEBUG_FUNC
		debug_etime = qemu_clock_get_ns(QEMU_CLOCK_REALTIME); 
		ftl_log("time: %ld\n", debug_etime - debug_stime);
#endif
#endif
#if 0
		ftl_log("tt_reqs: %d  idle: %d\n", bp->status->tt_reqs, bp->cond_chip->idle);
		if (bp->status->tt_reqs > 64 && bp->cond_chip->idle == 1) { 
			//ftl_log("1\n");
			while (1) { 
				//ftl_log("2\n");
				//usleep(10);
				maxlat = ssd_buff_flush(ssd); 
				if (bp->status->tt_reqs < BUFF_SIZE)
					break; 
			} 
		} else if (bp->status->tt_reqs > BUFF_SIZE && bp->cond_chip->idle == 1) { 
			//ftl_log("2222\n");
			while (1) { 
				//ftl_log("3\n");
				usleep(10000);
				if (bp->cond_chip->idle == 1) { 
					maxlat = ssd_buff_flush(ssd); 
					if (bp->status->tt_reqs < BUFF_SIZE)
						break; 
					//usleep(1000000);
				}
			} 
		}
#endif 
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
        }
    }

    return NULL;
}

#ifdef USE_BUFF
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
	uint64_t curlat = 0, maxlat = 0; 
	uint64_t accumulat = 0; 
	uint64_t idx; 
	int tt_flush_mpgs = 0;
	uint64_t now, stime; 

	for (int i = 0; i < 64; i++) { 
		/* sanghyeon fixed */ 
		idx = ssd->dmpg_list->idx; 
		ssd->maptbl_state[idx] &= ~(1 << DIRTY_BIT_SHIFT);

		struct ppa ppa = get_gtd_ent(ssd, idx); // lpn >> idx ??  

		/* update old page information first */
		if (mapped_ppa(&ppa)) { 
			mark_page_invalid(ssd, &ppa); 
			set_g_rmap_ent(ssd, INVALID_LPN, &ppa);
		} 
	
		/* new wirte */ 
		ppa = get_new_page(ssd); 
		/* update gtd */ 
		set_gtd_ent(ssd, idx, &ppa); 
		/* update rmap */
		set_g_rmap_ent(ssd, idx, &ppa);		

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
	
	//ftl_log("4\n");
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

	return;  
} 

#if 0 
static void calc_delay_maptbl_flush(struct ssd *ssd)
{ 
	uint64_t curlat = 0, maxlat = 0; 
	uint64_t accumulat = 0; 
	uint64_t idx; 
	int tt_flush_mpgs = 0; 
	uint64_t now, stime; 
	
	for (int i = 0; i < 64; i++) { 
//retry: 
		idx = ssd->dmpg_list->idx; 
		ssd->maptbl_state[idx] &= ~(1 << DIRTY_BIT_SHIFT); 
		
		struct ppa ppa = get_gtd_ent(ssd, idx); 
		
		/* update old page information first */ 
		if (mapped_ppa(&ppa)) { 
			mark_page_invalid(ssd, &ppa); 
			set_g_rmap_ent(ssd, INVALID_LPN, &ppa);
		} 
		
		/* new write */ 
		ppa = get_new_page(ssd);
		/* update gtd */ 
		set_gtd_ent(ssd, idx, &ppa); 
		/* update rmap */ 
		set_g_rmap_ent(ssd, idx, &ppa); 
		
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
		accumulat += maxlat; 
	
//		struct dmpg_node* tmp = ssd->dmpg_list; 
//		ssd->dmpg_list = ssd->dmpg_list->next; 
//		free(tmp); 
		struct dmpg_node* tmp_next = ssd->dmpg_list->next; 
		if (ssd->toFlush_state[tmp_next->idx]) { 
			
		} 

		tt_flush_mpgs++; 	
	} 
	
	stime = qemu_clock_get_ns(QEMU_CLOCK_REALTIME); 
	while (1) { 
		
	}  
		
	return; 
} 
#endif

static void *ftl_flush_thread(void *arg)
{ 
	FemuCtrl *n = (FemuCtrl *)arg; 
	struct ssd *ssd = n->ssd; 
	struct ssd_buff *bp = &ssd->buff;
	struct ssdparams *spp = &ssd->sp; 

	struct dpg_node* dpg;
	uint64_t curlat = 0;//, maxlat = 0;
	uint64_t now, stime; 

	while (1) {
		/* move dpg_to_flush_list to dpg_flush_list */
		if (bp->dpg_to_flush_list->reqs > 64 && bp->dpg_flush_list->reqs <= 64) { 
			//ftl_log("checkPoint1\n");
			int32_t ret = move_dpg_list(bp->dpg_to_flush_list, bp->dpg_flush_list);
			if (ret == -1) 
				ftl_log("move_dpg_list err!\n"); 
			//ftl_log("checkPoint1-1\n");
		}
	
		if (bp->dpg_flush_list->reqs > 64) { // Channel Cnt
			//ftl_log("checkPoint2\n");
			/* Flush dpg_flush_list to Chip */ 
			qemu_mutex_lock(&bp->cond_chip->lock); 
			bp->cond_chip->idle = 0; 
			qemu_mutex_unlock(&bp->cond_chip->lock);			

			//ftl_log("checkPoint2-1\n");
			uint64_t maxlat = 0;
			for (int i = 0; i < 64; i++) { 
				dpg = bp->dpg_flush_list->head; 
			
				//ftl_log("checkPoint2-1-1\n"); 	
				/* write one page into flash */ 
				curlat = ssd_buff_flush_one_page(ssd, dpg); 
				//ftl_log("curlat: %ld, maxlat: %ld\n", curlat, maxlat);
				maxlat = (curlat > maxlat) ? curlat : maxlat; 
				//accumulat += maxlat; 
				
				//ftl_log("checkPoint2-1-2\n");
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
			
#ifdef RES		
			ftl_log("userdat pgs: %d, mapdat pgs: %d\n", ssd->tt_user_dat_flush_pgs, ssd->tt_maptbl_flush_pgs);   
			ftl_log("gc userdat pgs: %d, mapdat pgs: %d\n", ssd->gc_user_dat_flush_pgs, ssd->gc_maptbl_flush_pgs);
#endif
			//ftl_log("checkPoint2-2\n");
			/* sleep during max latency */
			stime = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
			while (1) { 
				now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
				//ftl_log("now: %ld, stime: %ld, maxlat: %ld\n", now, stime, maxlat);
				//ftl_log("check0\n");
				if(now > stime + maxlat)
			//	if (now > stime + maxlat + accumulat)
					break; 		
			} 
			//ftl_log("userdat maxlat: %ld  calc: %ld\n", maxlat, now);	
			//ftl_log("now: %ld, stime: %ld, maxlat: %ld\n", now, stime, maxlat);
			
			//ftl_log("check4\n");
//			ftl_log("checkPoint2-3\n");
			while (ssd->tt_maptbl_dpg >= spp->pgs_protected) { 
				//ftl_log("check0\n");
				calc_delay_maptbl_flush(ssd);			
			}
			//ftl_log("check1\n"); 

			//ftl_log("check5\n");	
			/* decrease the number of requests in buffer */
			//ftl_log("check4\n");
			qemu_mutex_lock(&bp->status->lock);
			bp->status->tt_reqs -= 64;
			qemu_mutex_unlock(&bp->status->lock); 
			bp->dpg_flush_list->reqs -= 64; 
			//tt_flush_dpgs += 64;
			//ftl_log("check4\n");

			qemu_mutex_lock(&bp->cond_chip->lock);
			bp->cond_chip->idle = 1; 
			qemu_mutex_unlock(&bp->cond_chip->lock);
//			ftl_log("checkPoint2-4\n");
		}
		//ftl_log("userdat pgs: %d, mapdat pgs: %d\n", ssd->tt_user_dat_flush_pgs, ssd->tt_maptbl_flush_pgs);   
		if (should_gc(ssd)) { 
			//ftl_log("checkPoint3\n");
			do_gc(ssd, false); 
			//ftl_log("checkPoint3-1\n");
		}
	}

	return NULL; 
}
#endif
