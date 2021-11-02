#include "ftl.h"
#define FEMU_DEBUG_FTL

static void *ftl_thread(void *arg);

#ifdef DFTL
static void mark_translation_page_valid(struct ssd *ssd, struct ppa *ppa);
static uint64_t ssd_address_translation(struct ssd *ssd, NvmeRequest *req, uint64_t lpn);
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

static inline void set_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    ftl_assert(lpn < ssd->sp.tt_pgs);
    ssd->maptbl[lpn] = *ppa;
}

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

		#ifdef DFTL
		line->data_or_map = INVALID_LINE;						// In DFTL, have to set data_or_map
		#endif

        QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
        lm->free_line_cnt++;
    }

    ftl_assert(lm->free_line_cnt == lm->tt_lines);
    lm->victim_line_cnt = 0;
    lm->full_line_cnt = 0;
}

#ifdef DFTL
static void ssd_init_DFTL_write_pointer(struct ssd *ssd)		// Data line and translation line were seperated, so the same goes for write_pointer 
{																
	struct write_pointer *wpp = &ssd->wp;
	struct line_mgmt *lm = &ssd->lm;
	struct line *Tcurline = NULL;

		
	Tcurline = QTAILQ_FIRST(&lm->free_line_list);
	QTAILQ_REMOVE(&lm->free_line_list, Tcurline, entry);
	lm->free_line_cnt--;

	wpp->Tcurline = Tcurline;
	wpp->Tcurline->data_or_map = TRANSLATION_LINE;				// Initialization for current translation block
	wpp->Tch = 0;
	wpp->Tlun = 0;
	wpp->Tpg = 0;
	wpp->Tblk = 0;
	wpp->Tpl = 0;
}
#endif

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

	#ifdef DFTL
 	wpp->curline->data_or_map = DATA_LINE;						// In DFTL, ssd_init_write_pointer is used to initialize data line
	#endif

    wpp->ch = 0;
    wpp->lun = 0;
    wpp->pg = 0;
	
	#ifdef DFTL
    wpp->blk = wpp->curline->id;     							// In DFTL, data line id is the next line of after finish translation block initialization
	#elif defined FTL
	wpp->blk = 0;					  
	#endif

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

#ifdef DFTL
static void ssd_advance_translation_write_pointer(struct ssd *ssd)		// Function is the same as ssd_advance_write_pointer
{																		// But in DFTL, this function is used for translation line
    struct ssdparams *spp = &ssd->sp;
    struct write_pointer *wpp = &ssd->wp;
    struct line_mgmt *lm = &ssd->lm;

    check_addr(wpp->Tch, spp->nchs);
    wpp->Tch++;
    if (wpp->Tch == spp->nchs) {
        wpp->Tch = 0;
        check_addr(wpp->Tlun, spp->luns_per_ch);
        wpp->Tlun++;
        /* in this case, we should go to next lun */
        if (wpp->Tlun == spp->luns_per_ch) {
            wpp->Tlun = 0;
            /* go to next page in the block */
            check_addr(wpp->Tpg, spp->pgs_per_blk);
            wpp->Tpg++;
            if (wpp->Tpg == spp->pgs_per_blk) {
                wpp->Tpg = 0;
                /* move current line to {victim,full} line list */
                if (wpp->Tcurline->vpc == spp->pgs_per_line) {
                    /* all pgs are still valid, move to full line list */
                    ftl_assert(wpp->Tcurline->ipc == 0);
                    QTAILQ_INSERT_TAIL(&lm->full_line_list, wpp->Tcurline, entry);
                    lm->full_line_cnt++;
                } else {
                    ftl_assert(wpp->Tcurline->vpc >= 0 && wpp->Tcurline->vpc < spp->pgs_per_line);
                    /* there must be some invalid pages in this line */
                    ftl_assert(wpp->Tcurline->ipc > 0);
                    pqueue_insert(lm->victim_line_pq, wpp->Tcurline);
                    lm->victim_line_cnt++;
                }
                /* current line is used up, pick another empty line */
                check_addr(wpp->Tblk, spp->blks_per_pl);
                wpp->Tcurline = NULL;
                wpp->Tcurline = get_next_free_line(ssd);
                if (!wpp->Tcurline) {
                    abort();
                }
				wpp->Tcurline->data_or_map = TRANSLATION_LINE;
                wpp->Tblk = wpp->Tcurline->id;
                check_addr(wpp->Tblk, spp->blks_per_pl);
                /* make sure we are starting from page 0 in the super block */
                ftl_assert(wpp->Tpg == 0);
                ftl_assert(wpp->Tlun == 0);
                ftl_assert(wpp->Tch == 0);
                ftl_assert(wpp->Tpl == 0);
            }
        }
    }
}
#endif

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
					abort();
                }

				#ifdef DFTL
				wpp->curline->data_or_map = DATA_LINE;
				#endif

                wpp->blk = wpp->curline->id;
                check_addr(wpp->blk, spp->blks_per_pl);
                /* make sure we are starting from page 0 in the super block */
                ftl_assert(wpp->pg == 0);
                ftl_assert(wpp->lun == 0);
                ftl_assert(wpp->ch == 0);
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

#ifdef DFTL
static struct ppa get_new_translation_page(struct ssd *ssd)			// Function is the same as get_new_page
{																	// But in DFTL, this function is used for translation page
    struct write_pointer *wpp = &ssd->wp;							// So return translation page from translation line using Tch, Tlun, Tblk ...
    struct ppa ppa;
    ppa.ppa = 0;
    ppa.g.ch = wpp->Tch;
    ppa.g.lun = wpp->Tlun;
    ppa.g.pg = wpp->Tpg;
    ppa.g.blk = wpp->Tblk;
    ppa.g.pl = wpp->Tpl;
    ftl_assert(ppa.g.Tpl == 0);

    return ppa;
}
#endif

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
    spp->blks_per_pl = 65; /* 4GB */
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

    spp->blks_per_lun = spp->blks_per_pl * spp->pls_per_lun;
    spp->blks_per_ch = spp->blks_per_lun * spp->luns_per_ch;
    spp->tt_blks = spp->blks_per_ch * spp->nchs;

    spp->pls_per_ch =  spp->pls_per_lun * spp->luns_per_ch;
    spp->tt_pls = spp->pls_per_ch * spp->nchs;

    spp->tt_luns = spp->luns_per_ch * spp->nchs;

    /* line is special, put it at the end */
    spp->blks_per_line = spp->tt_luns;
    spp->pgs_per_line = spp->blks_per_line * spp->pgs_per_blk;
    spp->secs_per_line = spp->pgs_per_line * spp->secs_per_pg;
    spp->tt_lines = spp->blks_per_lun; 

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
    }
}

static void ssd_init_rmap(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->rmap = g_malloc0(sizeof(uint64_t) * (spp->tt_pgs ));    /* Allocate rmap - total number of pages */
    
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->rmap[i] = INVALID_LPN;
    }
}

#ifdef DFTL
static void ssd_init_DFTL_GTD(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t GTD_entry_count = ((sizeof(struct ppa) * spp->tt_pgs) / ssd->page_size);    /* GTD entry count = (sizeof(maptbl) / page_size) */
    uint64_t mapEntry_per_page = (ssd->page_size /sizeof(struct ppa));      			 /* mapEntry per page = mapEntry per GTD entry = page_size / sizeof(struct ppa) */
    ssd->GTD = g_malloc0(sizeof(struct GTD));
    
	ssd->GTD->current_inCMT = 0;
    ssd->GTD->max_inCMT = MAX_INCMT;
    ssd->GTD->GTD_entries = g_malloc0(sizeof(struct GTD_entry) * GTD_entry_count);    	 /* GTD size = GTD_entry_count * sizeof(GTD_entry) */
    
	for (int i = 0; i < GTD_entry_count; i++) {
        ssd->GTD->GTD_entries[i].map_page = &(ssd->maptbl[mapEntry_per_page * i]);    	 /* Store the address of maptbl page in each GTD entry */
        ssd->GTD->GTD_entries[i].inCMT = NOT_IN_CMT;
        ssd->GTD->GTD_entries[i].modified = NOT_MODIFIED;
    }
}

static void ssd_init_DFTL_map(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t GTD_entry_count = ((sizeof(struct ppa) * spp->tt_pgs) / ssd->page_size);	/* GTD entry count = (sizeof(maptbl) / page_size) */
    uint64_t map_table_tt_pgs = GTD_entry_count;		         						/* total page of entire map table */
    struct ppa ppa;
   
    for (int i = 0; i < map_table_tt_pgs; i++) {       
        ppa = get_new_translation_page(ssd);                    						/* get new translation page from wpp->Tch, Tlun ... */
        ssd->GTD->GTD_entries[i].map_page_ppa = ppa;    								/* set ppa of map table */
        mark_translation_page_valid(ssd, &ppa);                 						/* mark translation page valid, set TRANSLATION_PAGE, TRANSLATION_BLOCK, vpc ... */
        set_rmap_ent(ssd, i, &ppa);            											/* rmap set every time the translation page is flushed */
        ssd_advance_translation_write_pointer(ssd);             						/* advance translation write pointer using wpp->Tcp, Tlun ... */
    }
}

static void ssd_init_LRU(struct ssd *ssd)									// Initialization LRU list for CMT
{   
    ssd->GTD->LRU_list = g_malloc0(sizeof(struct dll));			

    ssd->GTD->LRU_list->head = g_malloc0(sizeof(struct dll_entry));
    ssd->GTD->LRU_list->tail = g_malloc0(sizeof(struct dll_entry));			// Allocate list head and tail
    
    ssd->GTD->LRU_list->head->GTD_index = -1;
    ssd->GTD->LRU_list->tail->GTD_index = -1;

    ssd->GTD->LRU_list->head->prev = NULL;
    ssd->GTD->LRU_list->head->next = ssd->GTD->LRU_list->tail;
    ssd->GTD->LRU_list->tail->prev = ssd->GTD->LRU_list->head;
    ssd->GTD->LRU_list->tail->next = NULL;
}

static void append_LRU(struct ssd *ssd, uint64_t GTD_index)
{
    int check = 1;
    struct dll_entry *new_entry;
    for(struct dll_entry *entry = ssd->GTD->LRU_list->head->next; entry->next != NULL; entry = entry->next )
    {
        if(entry->GTD_index == GTD_index) 							// If GTD_index to add already exists in list, put it right behind head
        {
            (entry->prev)->next = entry->next;
            (entry->next)->prev = entry->prev;
            (ssd->GTD->LRU_list->head->next)->prev = entry;
            entry->next = ssd->GTD->LRU_list->head->next;
            ssd->GTD->LRU_list->head->next = entry;
            entry->prev = ssd->GTD->LRU_list->head;
            check = 0;
            break;
        }
    }
    if(check)														// If GTD_index to add not exists in list, append it right before tail
    {
        new_entry = g_malloc0(sizeof(struct dll_entry));
        new_entry->GTD_index = GTD_index;

        (ssd->GTD->LRU_list->head->next)->prev = new_entry;
        new_entry->next = ssd->GTD->LRU_list->head->next;
        ssd->GTD->LRU_list->head->next = new_entry;
        new_entry->prev = ssd->GTD->LRU_list->head;

        ssd->GTD->current_inCMT++;									// Because appended 1 GTD entry in CMT, increase current_inCMT
    }
}

static int victim_LRU(struct ssd *ssd)									// Victim 1 GTD entry which right before tail from CMT
{
    struct dll_entry *victim_entry = ssd->GTD->LRU_list->tail->prev;
    uint64_t victim_index = victim_entry->GTD_index;

	(victim_entry->prev)->next = ssd->GTD->LRU_list->tail;
	ssd->GTD->LRU_list->tail->prev = victim_entry->prev;
	ssd->GTD->current_inCMT--;										

    free(victim_entry);	
    return victim_index;
}

static void ssd_init_DFTL(struct ssd *ssd)		// Initialization for DFTL
{
    ssd_init_DFTL_GTD(ssd);
    ssd_init_DFTL_write_pointer(ssd);
    ssd_init_DFTL_map(ssd);
    ssd_init_LRU(ssd);
}   
#endif

void ssd_init(FemuCtrl *n)
{
    struct ssd *ssd = n->ssd;
    struct ssdparams *spp = &ssd->sp;
    // ssd->page_size = n->page_size;
    ssd->page_size = PAGE_SIZE;
    femu_debug("ssd->page_size = %ld", ssd->page_size);
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

    /* initialize all the lines */
    ssd_init_lines(ssd);

	#ifdef DFTL
    ssd_init_DFTL(ssd);
	#endif

    /* initialize write pointer, this is how we allocate new pages for writes */
    ssd_init_write_pointer(ssd);

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

	#ifdef DFTL
    pg->data_or_map = INVALID_PAGE;									// In DFTL, have to set data_or_map
	#endif

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
	
	#ifdef DFTL
	pg->data_or_map = DATA_PAGE;           							// In DFTL, mark_page_valid used for data page
	#endif

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->vpc >= 0 && blk->vpc < ssd->sp.pgs_per_blk);
    blk->vpc++;

	#ifdef DFTL
	blk->data_or_map = DATA_BLOCK;									// In DFTL, mark_page_valid used for data page
	#endif

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    ftl_assert(line->vpc >= 0 && line->vpc < ssd->sp.pgs_per_line);
    line->vpc++;
}

#ifdef DFTL
static void mark_translation_page_valid(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    struct line *line;

    /* update page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_FREE);
    pg->status = PG_VALID;        

	#ifdef DFTL
	pg->data_or_map = TRANSLATION_PAGE;     						// Used for translation page
	#endif  

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->vpc >= 0 && blk->vpc < ssd->sp.pgs_per_blk);
    blk->vpc++;

	#ifdef DFTL
	blk->data_or_map = TRANSLATION_BLOCK;   						// Used for translation page
	#endif

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    ftl_assert(line->vpc >= 0 && line->vpc < ssd->sp.pgs_per_line);
    line->vpc++;
}
#endif

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

		#ifdef DFTL
		pg->data_or_map = INVALID_PAGE;								// In DFTL, have to set data_or_map
		#endif
    }

    /* reset block status */
    ftl_assert(blk->npgs == spp->pgs_per_blk);
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt++;
	
	#ifdef DFTL
	blk->data_or_map = INVALID_BLOCK;								// In DFTL, have to set data_or_map
	#endif
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

#ifdef DFTL
static uint64_t gc_write_translation_page(struct ssd *ssd, struct ppa *old_ppa)
{
    struct ppa new_ppa;
    struct nand_lun *new_lun;
    uint64_t GTD_index = get_rmap_ent(ssd, old_ppa);

    new_ppa = get_new_translation_page(ssd);					// Get new translation page to write

    ssd->GTD->GTD_entries[GTD_index].map_page_ppa = new_ppa;	// Update GTD with new page ppa

    set_rmap_ent(ssd, GTD_index, &new_ppa);						// Update rmap with new page ppa

    mark_translation_page_valid(ssd, &new_ppa);					// Mark new translation page valid		

    /* need to advance the write pointer here */
    ssd_advance_translation_write_pointer(ssd);					

    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        gcw.cmd = NAND_WRITE;
        gcw.stime = 0;
        ssd_advance_status(ssd, &new_ppa, &gcw);
    }															// Apply write delay using ssd_advance_status

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
	
    return victim_line;
}

/* here ppa identifies the block we want to clean */

#ifdef DFTL
static void clean_one_translation_block(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_page *pg_iter = NULL;
	uint64_t GTD_index;
	uint64_t lpn;
	uint64_t mapEntry_per_page = (ssd->page_size /sizeof(struct ppa));
    int cnt = 0;
    
    for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
        ppa->g.pg = pg;
        pg_iter = get_pg(ssd, ppa);
        /* there shouldn't be any free page in victim blocks */
        ftl_assert(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID) {
				lpn = get_rmap_ent(ssd,ppa);
				GTD_index = lpn / mapEntry_per_page;			

				if (ssd->GTD->GTD_entries[GTD_index].inCMT == IN_CMT)
				{
					ssd->GTD->GTD_entries[GTD_index].modified = MODIFIED; 	
				}
				else
				{
            		gc_read_page(ssd, ppa);						// Read valid page(apply delay using ssd_advance_status)
            		gc_write_translation_page(ssd, ppa);		// Write valid page to current translation block		
            
					set_rmap_ent(ssd, INVALID_LPN, ppa);
	
            		cnt++;
				}
        }
    }
}

static void clean_one_data_block(struct ssd *ssd, struct ppa *ppa, uint64_t *translation_update_set, int *cnt)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_page *pg_iter = NULL;
    
	uint64_t mapEntry_per_page = (ssd->page_size /sizeof(struct ppa));
    uint64_t GTD_index;
    uint64_t lpn;   

    for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
        ppa->g.pg = pg;
        pg_iter = get_pg(ssd, ppa);
        /* there shouldn't be any free page in victim blocks */
        ftl_assert(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID) {
            gc_read_page(ssd, ppa);
            gc_write_page(ssd, ppa);
	   
	    	lpn = get_rmap_ent(ssd, ppa);
	    	GTD_index = lpn / mapEntry_per_page;
	    	translation_update_set[*cnt] = GTD_index;		// Store translation pages to update in translation_update_set 

			set_rmap_ent(ssd, INVALID_LPN, ppa);

			*cnt += 1;										// The number of translation pages to update
        }
    }

    ftl_assert(get_blk(ssd, ppa)->vpc == *cnt);
}
#elif defined FTL
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
#endif

static void mark_line_free(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *line = get_line(ssd, ppa);
    line->ipc = 0;
    line->vpc = 0;
	
	#ifdef DFTL
    line->data_or_map = INVALID_LINE;
	#endif

    /* move this line to free line list */
    QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
    lm->free_line_cnt++;
}

#ifdef DFTL
static void deleteDuplicate(uint64_t* arr, int* len){		// Delete duplicate GTD_index and GTD_index in CMT from translation_update_set
	uint64_t newArr[*len];
	int arrcount = 0;

	for (int i = 0; i < *len; i++) 
	{
		int count = 0;
		for (int j = 0; j < arrcount; j++) 
		{
			if ( arr[i] == newArr [j])
			{	
				count++;
				break;
			}
		} 
		if ( count == 0)
		{
			newArr[arrcount++] = arr[i];
		}
	}
	for (int i = 0; i < arrcount; i++){
		arr[i] = newArr[i];
	}
	*len = arrcount;
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
    femu_debug("GC-ing line:%d,ipc=%d,victim=%d,full=%d,free=%d\n", ppa.g.blk,
              victim_line->ipc, ssd->lm.victim_line_cnt, ssd->lm.full_line_cnt,
              ssd->lm.free_line_cnt);

    if (victim_line->data_or_map == TRANSLATION_LINE)
    {
		femu_debug("victim_line is translation line\n");

		for (ch = 0; ch < spp->nchs; ch++) {
            for (lun = 0; lun < spp->luns_per_ch; lun++) {
                ppa.g.ch = ch;
                ppa.g.lun = lun;
                ppa.g.pl = 0;
                lunp = get_lun(ssd, &ppa);
		
                clean_one_translation_block(ssd, &ppa);				// Move valid translation pages from victim line to current translation block 
		
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
    }
    else if(victim_line->data_or_map == DATA_LINE)
    {
		femu_debug("victim_line is data line\n");

		uint64_t translation_update_set[victim_line->vpc];
		int temp = 0;
		int *cnt = &temp;

		for (ch = 0; ch < spp->nchs; ch++) {
            for (lun = 0; lun < spp->luns_per_ch; lun++) {
                	ppa.g.ch = ch;
                	ppa.g.lun = lun;
                	ppa.g.pl = 0;
                	lunp = get_lun(ssd, &ppa);

                	clean_one_data_block(ssd, &ppa, translation_update_set, cnt);	// Move valid translation pages from victim line to current translation block 
                																	// Store translation pages to update in translation_update_set(for batch update)
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
	
		deleteDuplicate(translation_update_set, cnt);								// Delete duplicate GTD_index from translation_update_set
	
		uint64_t GTD_index;
		struct ppa map_ppa;

		for (uint64_t i = 0; i < *cnt; i++) {
	    	GTD_index = translation_update_set[i];

			if (ssd->GTD->GTD_entries[GTD_index].inCMT == IN_CMT)
			{
				ssd->GTD->GTD_entries[GTD_index].modified = MODIFIED; 	
			}
			else
			{
	    		map_ppa = ssd->GTD->GTD_entries[GTD_index].map_page_ppa;

	    		gc_read_page(ssd, &map_ppa);
            	gc_write_translation_page(ssd, &map_ppa);							// Apply delay to update translation pages
	
	    		mark_page_invalid(ssd, &map_ppa);
				set_rmap_ent(ssd, INVALID_LPN, &map_ppa);
			}
		}	

    }
    else
    {
		femu_debug("victim_line.data_or_map is INVALID_LINE\n");
    }

    /* update line status */
    mark_line_free(ssd, &ppa);
    
    return 0;
}
#elif defined FTL
static int do_gc(struct ssd *ssd, bool force)
{
    struct line *victim_line = NULL;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lunp;
    struct ppa ppa;
    int ch, lun;

    victim_line = select_victim_line(ssd, force);
    if (!victim_line) {
		// femu_debug("Returned NULL from select_victim_line\n");
        return -1;
    }

    ppa.g.blk = victim_line->id;
    femu_debug("GC-ing line:%d,ipc=%d,victim=%d,full=%d,free=%d\n", ppa.g.blk,
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
#endif

#ifdef DFTL
static uint64_t ssd_read(struct ssd *ssd, NvmeRequest *req)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t lba = req->slba;
    int nsecs = req->nlb;
    struct ppa ppa;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + nsecs) / spp->secs_per_pg;
    uint64_t lpn;
    uint64_t sublat = 0, maxlat = 0, translat = 0;

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }

    /* normal IO read path */
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
		translat = ssd_address_translation(ssd, req, lpn);			// Calculate delay to address_translation	

        ppa = get_maptbl_ent(ssd, lpn);
        if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
            //printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", ssd->ssdname, lpn);
            //printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n",
            //ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
            continue;
        }

        struct nand_cmd srd;
        srd.type = USER_IO;
        srd.cmd = NAND_READ;

		if (translat == 0) 
		{
			srd.stime = req->stime;
		}
		else
		{
			srd.stime = 0;											// If there is address_translation delay, have to use real time to calculate read delay 
		}

        sublat = ssd_advance_status(ssd, &ppa, &srd);
		sublat += translat;											// Add address_translation delay with read delay
        maxlat = (sublat > maxlat) ? sublat : maxlat;
    }

    return maxlat;
}

static uint64_t ssd_write(struct ssd *ssd, NvmeRequest *req)
{
    uint64_t lba = req->slba;
    struct ssdparams *spp = &ssd->sp;
    int len = req->nlb;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg;
    struct ppa ppa;
    uint64_t lpn;
    uint64_t curlat = 0, maxlat = 0, translat = 0;
    int r;

    uint64_t mapEntry_per_page = (ssd->page_size /sizeof(struct ppa));      /* mapEntry per page = mapEntry per GTD entry = page_size / sizeof(struct ppa) */
    uint64_t GTD_index;

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }

    while (should_gc_high(ssd)) {
        /* perform GC here until !should_gc(ssd) */
        r = do_gc(ssd, true);
        if (r == -1)
            break;
    }

    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
		translat = ssd_address_translation(ssd, req, lpn);				// Calculate delay to address translation
	
        ppa = get_maptbl_ent(ssd, lpn);
        if (mapped_ppa(&ppa)) {
            /* update old page information first */
	    	GTD_index = lpn / mapEntry_per_page;
	    	ssd->GTD->GTD_entries[GTD_index].modified = MODIFIED;
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
	
		if (translat == 0)
		{
        	swr.stime = req->stime;
		}
		else
		{
			swr.stime = 0;												// If there is address_translation delay, have to use real time to calculate read delay 
		}

        /* get latency statistics */
        curlat = ssd_advance_status(ssd, &ppa, &swr);
		curlat += translat;												// Add address_translation delay with read delay
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }

    return maxlat;
}

static uint64_t ssd_address_translation(struct ssd *ssd, NvmeRequest *req, uint64_t lpn)
{
    // struct ssdparams *spp = &ssd->sp;
    struct ppa ppa;
    uint64_t curlat = 0, maxlat = 0;

    uint64_t mapEntry_per_page = (ssd->page_size /sizeof(struct ppa));      // mapEntry per page = mapEntry per GTD entry = page_size / sizeof(struct ppa)
    uint64_t GTD_index;
    uint64_t victim_index;

    GTD_index = lpn / mapEntry_per_page;

	// cache hit 
    if (ssd->GTD->GTD_entries[GTD_index].inCMT == IN_CMT) {		    		// When the map table for address translation is in CMT
		append_LRU(ssd, GTD_index);											// According to LRU, locate GTD_index the beginning of the list
        return 0;							    							// No additional latency
    }

	// cache miss 
    if (ssd->GTD->current_inCMT < ssd->GTD->max_inCMT) 			    		// When the map table for address translation is not in CMT, but CMT is not full
    {									    								// 1 ssd_read needed to fetch the map table for address translation 
        append_LRU(ssd, GTD_index);					    					// Append GTD_index in LRU list 
        ssd->GTD->GTD_entries[GTD_index].inCMT = IN_CMT;
        ppa = ssd->GTD->GTD_entries[GTD_index].map_page_ppa;
        struct nand_cmd srd;
        srd.type = USER_IO;
        srd.cmd = NAND_READ;
        srd.stime = req->stime;
        curlat = ssd_advance_status(ssd, &ppa, &srd);
        maxlat = (curlat > maxlat) ? curlat : maxlat;

		return maxlat;														// Return 1 ssd_read delay
    }

	// cache miss and CMT is full 
    victim_index = victim_LRU(ssd);					    					// Evict out a victim page from LRU list
    ssd->GTD->GTD_entries[victim_index].inCMT = NOT_IN_CMT;		    		// have to victim 1 map table with LRU in CMT

	// cache miss and CMT is full but victim has not been modified
    if (ssd->GTD->GTD_entries[victim_index].modified == NOT_MODIFIED)	    // 1 ssd_read needed to fetch the map table for address translation
    {									    
        append_LRU(ssd, GTD_index);											// Append GTD_index in LRU list
        ssd->GTD->GTD_entries[GTD_index].inCMT = IN_CMT;

        ppa = ssd->GTD->GTD_entries[GTD_index].map_page_ppa;
        struct nand_cmd srd;
        srd.type = USER_IO;
        srd.cmd = NAND_READ;
		srd.stime = 0;
        // srd.stime = req->stime;
        curlat = ssd_advance_status(ssd, &ppa, &srd);
        maxlat = (curlat > maxlat) ? curlat : maxlat;

		return maxlat;														// Return 1 ssd_read delay
    }

	// cache miss and CMT is full and victim has been modified 
    ppa = ssd->GTD->GTD_entries[victim_index].map_page_ppa;		  			// 1 ssd_read and 1 ssd_write needed
									    
    if (mapped_ppa(&ppa)) {
    
        mark_page_invalid(ssd, &ppa);
        set_rmap_ent(ssd, INVALID_LPN, &ppa);
    }									    								// mark victim map table's ppa invalid

    ppa = get_new_translation_page(ssd);
    ssd->GTD->GTD_entries[victim_index].map_page_ppa = ppa;					// Get new translation page for update victim
    mark_translation_page_valid(ssd, &ppa);
    set_rmap_ent(ssd, victim_index, &ppa);
    ssd_advance_translation_write_pointer(ssd);				    			// Update GTD, rmap ...(delay will be calculated after this)		

    struct nand_cmd swr;
    swr.type = USER_IO;
    swr.cmd = NAND_WRITE;
	swr.stime = 0;
    // swr.stime = req->stime;
    curlat = ssd_advance_status(ssd, &ppa, &swr);
    maxlat = (curlat > maxlat) ? curlat : maxlat;			    			// 1 ssd_write needed to flush modified map table to flash memory

    uint64_t writelat = maxlat;
    maxlat = 0;

    append_LRU(ssd, GTD_index);												// Perform original address translation
    ssd->GTD->GTD_entries[GTD_index].inCMT = IN_CMT;						

    ppa = ssd->GTD->GTD_entries[GTD_index].map_page_ppa;
    struct nand_cmd srd;
    srd.type = USER_IO;
    srd.cmd = NAND_READ;
    srd.stime = 0;
    curlat = ssd_advance_status(ssd, &ppa, &srd);
    maxlat = (curlat > maxlat) ? curlat : maxlat;			    			// 1 ssd_read needed to fetch the map table for address translation */
    maxlat = maxlat + writelat;						    					// Add read latency with write latency */

    return maxlat;						
}
#elif defined FTL
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
                lat = ssd_write(ssd, req);
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
				// ftl_log("in ftl_thread, in should_gc\n");
                do_gc(ssd, false);
            }
        }
    }

    return NULL;
}

