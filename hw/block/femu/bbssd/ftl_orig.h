#ifndef __FEMU_FTL_H
#define __FEMU_FTL_H

#include "../nvme.h"

#define INVALID_PPA     (~(0ULL))
#define INVALID_LPN     (~(0ULL))
#define UNMAPPED_PPA    (~(0ULL))
#define INEXIST_BUFF	NULL

#define PAGESIZE  4096
#define _PME (PAGESIZE/8) //per page mapping entries uint64_t
#define _PMES 9 //calculte mapping index with unit of pages

enum {
    NAND_READ =  0,
    NAND_WRITE = 1,
    NAND_ERASE = 2,

	//NAND_READ_LATENCY = 4000, 
    //NAND_READ_LATENCY = 40000,
	//NAND_PROG_LATENCY = 20000,
    //NAND_PROG_LATENCY = 200000, 
	//NAND_ERASE_LATENCY = 200000,
    //NAND_ERASE_LATENCY = 2000000,

    NAND_READ_LATENCY = 0,
    NAND_PROG_LATENCY = 0,
    NAND_ERASE_LATENCY = 0,
};

enum {
    USER_IO = 0,
    GC_IO = 1,
};

enum {
    SEC_FREE = 0,
    SEC_INVALID = 1,
    SEC_VALID = 2,

    PG_FREE = 0,
    PG_INVALID = 1,
    PG_VALID = 2
};

enum {
    FEMU_ENABLE_GC_DELAY = 1,
    FEMU_DISABLE_GC_DELAY = 2,

    FEMU_ENABLE_DELAY_EMU = 3,
    FEMU_DISABLE_DELAY_EMU = 4,

    FEMU_RESET_ACCT = 5,
    FEMU_ENABLE_LOG = 6,
    FEMU_DISABLE_LOG = 7,
};


#define BLK_BITS    (16)
#define PG_BITS     (16)
#define SEC_BITS    (8)
#define PL_BITS     (8)
#define LUN_BITS    (8)
#define CH_BITS     (7)

#define P_BLKS_PER_PL 256
//#define CHECK_DPG
//#define FG_DEBUG
//#define FEMU_DEBUG_FTL
//#define ORG_VER
#define USE_BUFF
#define FIFO
//#define DAWID 
//#define ASYNCH
//#define USE_BUFF_DEBUG
//#define DAWID_BUFF
//#define RES
//#define GCRES
//#define LPNLOG
//#define BLKDUMP

#ifdef USE_BUFF
/* things that buffer needed */ 
//#define BUFF_SIZE 1048576
#define BUFF_SIZE 65536
//#define BUFF_THRESHOLD 524288
#define BUFF_THRESHOLD 32768
//#define BUFF_SIZE 65536
//#define BUFF_SIZE 1024
#define LINE_SIZE 1 // ssd maximum parallelism 
#define PROTECTED_RATIO 0.5
//#define PROTECTED_RATIO 1

//unsigned char dirty_option = 0x1; 
#define DIRTY_BIT_SHIFT 0
#define EXIST_IN_ZCL_BIT_SHIFT 1 // ZCL: Zero Cost List

struct zcl_node { 
	uint64_t mpg_idx; // request table maptbl pg no. 
	uint64_t dpg_cnt;
	struct zcl_node *next;
}; 

struct hnode { 
	uint64_t mpg_idx;	// mapping table page idx  
	uint64_t dpg_cnt; // number of data pages associated with the mabtbl pg 
}; 

struct max_heap {
	struct hnode* heap;
	uint64_t hsz;
};

struct dpg_node { // data page 
	NvmeRequest *req;
	uint64_t lpn; 
	int64_t stime; 
	struct dpg_node *prev; 
	struct dpg_node *next;
}; 

struct dpg_tbl_ent{ 
	struct dpg_node *head; 
	uint64_t mpg_idx; 		// mapping table page idx 
	uint64_t heap_idx; 	
	struct zcl_node *znp;
};

struct dpg_list { 
	QemuMutex lock; 
	uint64_t reqs; 
	struct dpg_node *head; 
	struct dpg_node *tail; 
}; 

struct status { 
	QemuMutex lock; 
	uint32_t tt_reqs; 
};

struct cond_chip { 
	QemuMutex lock; 
	uint32_t idle;
}; 

struct ssd_buff {
	struct cond_chip* cond_chip; 
	struct status *status; 
	uint64_t bp_reqs; 
	struct dpg_list *dpg_to_flush_list; 
	struct dpg_list *dpg_flush_list;
#ifdef DAWID
	struct dpg_tbl_ent* dpg_tbl;
	struct max_heap* mpg_value_heap; // cost-effectiveness of maptable page  
	struct zcl_node* zcl; // zero cost list 
#else
	struct dpg_node *head; 
	struct dpg_node *tail; 
#endif
};
#endif

#if 0 //fifo buff
struct data_pg {
	//NvmeRequest *req;
	uint64_t lpn;
	int64_t stime;
	struct data_pg *prev; 
	struct data_pg *next;
}; 

struct buff { 
	uint32_t tot_cnt;
	struct data_pg *head; 
	struct data_pg *tail; 
};
#endif
/* describe a physical page addr */
struct ppa {
    union {
        struct {
            uint64_t blk : BLK_BITS;
            uint64_t pg  : PG_BITS;
            uint64_t sec : SEC_BITS;
            uint64_t pl  : PL_BITS;
            uint64_t lun : LUN_BITS;
            uint64_t ch  : CH_BITS;
            uint64_t rsv : 1;
        } g;

        uint64_t ppa;
    };
// DAWID
	struct dpg_node *bfa;
};

typedef int nand_sec_status_t;

struct nand_page {
    nand_sec_status_t *sec;
    int nsecs;
    int status;
};

struct nand_block {
    struct nand_page *pg;
    int npgs;
    int ipc; /* invalid page count */
    int vpc; /* valid page count */
    int erase_cnt;
    int wp; /* current write pointer */
};

struct nand_plane {
    struct nand_block *blk;
    int nblks;
};

struct nand_lun {
    struct nand_plane *pl;
    int npls;
    uint64_t next_lun_avail_time;
    bool busy;
    uint64_t gc_endtime;
};

struct ssd_channel {
    struct nand_lun *lun;
    int nluns;
    uint64_t next_ch_avail_time;
    bool busy;
    uint64_t gc_endtime;
};

struct ssdparams {
    int secsz;        /* sector size in bytes */
    int secs_per_pg;  /* # of sectors per page */
    int pgs_per_blk;  /* # of NAND pages per block */
    int blks_per_pl;  /* # of blocks per plane */
    int pls_per_lun;  /* # of planes per LUN (Die) */
    int luns_per_ch;  /* # of LUNs per channel */
    int nchs;         /* # of channels in the SSD */

    int pg_rd_lat;    /* NAND page read latency in nanoseconds */
    int pg_wr_lat;    /* NAND page program latency in nanoseconds */
    int blk_er_lat;   /* NAND block erase latency in nanoseconds */
    int ch_xfer_lat;  /* channel transfer latency for one page in nanoseconds
                       * this defines the channel bandwith
                       */

    double gc_thres_pcent;
    int gc_thres_lines;
    double gc_thres_pcent_high;
    int gc_thres_lines_high;
    bool enable_gc_delay;

    /* below are all calculated values */
    int secs_per_blk; /* # of sectors per block */
    int secs_per_pl;  /* # of sectors per plane */
    int secs_per_lun; /* # of sectors per LUN */
    int secs_per_ch;  /* # of sectors per channel */
    int tt_secs;      /* # of sectors in the SSD */

    int pgs_per_pl;   /* # of pages per plane */
    int pgs_per_lun;  /* # of pages per LUN (Die) */
    int pgs_per_ch;   /* # of pages per channel */
    int tt_pgs;       /* total # of pages in the SSD */
#ifdef USE_BUFF
	int pgs_maptbl; /* # of pages in the maptbl */ 
	int pgs_protected; /* # of pages in the protected maptbl */ 
#endif
	
    int blks_per_lun; /* # of blocks per LUN */
    int blks_per_ch;  /* # of blocks per channel */
    int tt_blks;      /* total # of blocks in the SSD */

    int secs_per_line;
    int pgs_per_line;
    int blks_per_line;
    int tt_lines;

    int pls_per_ch;   /* # of planes per channel */
    int tt_pls;       /* total # of planes in the SSD */

    int tt_luns;      /* total # of LUNs in the SSD */
};

typedef struct line {
    int id;  /* line id, the same as corresponding block id */
    int ipc; /* invalid page count in this line */
    int vpc; /* valid page count in this line */
    QTAILQ_ENTRY(line) entry; /* in either {free,victim,full} list */
    /* position in the priority queue for victim lines */
    size_t                  pos;
} line;

/* wp: record next write addr */
struct write_pointer {
    struct line *curline;
    int ch;
    int lun;
    int pg;
    int blk;
    int pl;
};

struct line_mgmt {
    struct line *lines;
    /* free line list, we only need to maintain a list of blk numbers */
    QTAILQ_HEAD(free_line_list, line) free_line_list;
    pqueue_t *victim_line_pq;
    //QTAILQ_HEAD(victim_line_list, line) victim_line_list;
    QTAILQ_HEAD(full_line_list, line) full_line_list;
    int tt_lines;
    int free_line_cnt;
    int victim_line_cnt;
    int full_line_cnt;
};

struct nand_cmd {
    int type;
    int cmd;
    int64_t stime; /* Coperd: request arrival time */
};

struct dmpg_node { /* dirty maptbl pg node */
	uint64_t idx; 
	struct dmpg_node *next; 
};

struct toFlush_comp { 
	int *toFlush_state;
	QemuMutex lock;
}; 

struct ssd {
    char *ssdname;
    struct ssdparams sp;
    struct ssd_channel *ch;
    struct ppa *maptbl; /* page level mapping table */
#ifdef USE_BUFF
	struct ssd_buff buff;

	int tt_maptbl_dpg; 
	int tt_maptbl_flush; // number of flushes 
	int tt_maptbl_flush_pgs; // number of flushed pages 
	int tt_user_dat_flush_pgs; 
	int gc_maptbl_flush_pgs; 
	int gc_user_dat_flush_pgs;
	int *maptbl_state; //checked mapping table's dirty condition
	struct toFlush_comp *tf; //checked dirty condition to_flush_list and flush_list
	struct dmpg_node *dmpg_list;
	struct ppa *gtd; /* page level meta mapping table */ 
	uint64_t *g_rmap; /* reverse gtd, assume it's stored in OOB */

	int tt_gc_valid_pgs; 
	int tt_gc_block;
#endif
	uint64_t *rmap;     /* reverse mapptbl, assume it's stored in OOB */
    struct write_pointer wp;
    struct line_mgmt lm;

    /* lockless ring for communication with NVMe IO thread */
    struct rte_ring **to_ftl;
    struct rte_ring **to_poller;
    bool *dataplane_started_ptr;
    QemuThread ftl_thread;
    QemuThread ftl_flush_thread; 
};

#if 0 //NAM
struct dpg_node {
	//NvmeRequest *req;
	uint64_t lpn;
	int64_t stime;
	struct dpg_node *prev; 
	struct dpg_node *next;
}; 

struct buff { 
	uint32_t tot_cnt;
	struct dpg_node *head; 
	struct dpg_node *tail; 
};
#endif 

void ssd_init(FemuCtrl *n);
int16_t get_buff_tot_cnt(void);
#ifdef FEMU_DEBUG_FTL
#define ftl_debug(fmt, ...) \
    do { printf("[FEMU] FTL-Dbg: " fmt, ## __VA_ARGS__); } while (0)
#else
#define ftl_debug(fmt, ...) \
    do { } while (0)
#endif

#define ftl_err(fmt, ...) \
    do { fprintf(stderr, "[FEMU] FTL-Err: " fmt, ## __VA_ARGS__); } while (0)

#define ftl_log(fmt, ...) \
    do { printf("[FEMU] FTL-Log: " fmt, ## __VA_ARGS__); } while (0)


/* FEMU assert() */
#ifdef FEMU_DEBUG_FTL
#define ftl_assert(expression) assert(expression)
#else
#define ftl_assert(expression)
#endif

#endif
