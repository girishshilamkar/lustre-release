/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 */
#ifndef _LUSTRE_QUOTA_H
#define _LUSTRE_QUOTA_H

#ifdef __KERNEL__
#include <linux/version.h>
#include <linux/fs.h>
#include <linux/quota.h>
#include <linux/quotaops.h>
#endif
#include <linux/lustre_idl.h>
#include <linux/lustre_net.h>
#include <linux/lvfs.h>

struct obd_device;
struct client_obd;

#ifndef NR_DQHASH
#define NR_DQHASH 45
#endif

#ifdef HAVE_QUOTA_SUPPORT

#ifdef __KERNEL__

/* structures to access admin quotafile */
struct lustre_mem_dqinfo {
        unsigned int dqi_bgrace;
        unsigned int dqi_igrace;
        unsigned long dqi_flags;
        unsigned int dqi_blocks;
        unsigned int dqi_free_blk;
        unsigned int dqi_free_entry;
};

struct lustre_quota_info {
        struct file *qi_files[MAXQUOTAS];
        struct lustre_mem_dqinfo qi_info[MAXQUOTAS];
};

#define DQ_STATUS_AVAIL         0x0     /* Available dquot */
#define DQ_STATUS_SET           0x01    /* Sombody is setting dquot */
#define DQ_STATUS_RECOVERY      0x02    /* dquot is in recovery */

struct lustre_dquot {
        /* Hash list in memory, protect by dquot_hash_lock */
        struct list_head dq_hash;
        /* Protect the data in lustre_dquot */
        struct semaphore dq_sem;
        /* Use count */
        int dq_refcnt;
        /* Pointer of quota info it belongs to */
        struct lustre_quota_info *dq_info;
        
        loff_t dq_off;                  /* Offset of dquot on disk */
        unsigned int dq_id;             /* ID this applies to (uid, gid) */
        int dq_type;                    /* Type fo quota (USRQUOTA, GRPQUOUTA) */
        unsigned short dq_status;       /* See DQ_STATUS_ */
        unsigned long dq_flags;         /* See DQ_ in quota.h */
        struct mem_dqblk dq_dqb;        /* Diskquota usage */
};

struct dquot_id {
        struct list_head        di_link;
        __u32                   di_id;
};

#define QFILE_CHK               1
#define QFILE_RD_INFO           2
#define QFILE_WR_INFO           3
#define QFILE_INIT_INFO         4
#define QFILE_GET_QIDS          5
#define QFILE_RD_DQUOT          6
#define QFILE_WR_DQUOT          7

/* admin quotafile operations */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,0)
int lustre_check_quota_file(struct lustre_quota_info *lqi, int type);
int lustre_read_quota_info(struct lustre_quota_info *lqi, int type);
int lustre_write_quota_info(struct lustre_quota_info *lqi, int type);
int lustre_read_dquot(struct lustre_dquot *dquot);
int lustre_commit_dquot(struct lustre_dquot *dquot);
int lustre_init_quota_info(struct lustre_quota_info *lqi, int type);
int lustre_get_qids(struct lustre_quota_info *lqi, int type, 
                    struct list_head *list);
#else

#ifndef DQ_FAKE_B
#define DQ_FAKE_B       6
#endif

static inline int lustre_check_quota_file(struct lustre_quota_info *lqi,
                                          int type)
{
        return 0;
}
static inline int lustre_read_quota_info(struct lustre_quota_info *lqi,
                                         int type)
{
        return 0;
}
static inline int lustre_write_quota_info(struct lustre_quota_info *lqi,
                                          int type)
{
        return 0;
}
static inline int lustre_read_dquot(struct lustre_dquot *dquot)
{
        return 0;
}
static inline int lustre_commit_dquot(struct lustre_dquot *dquot)
{
        return 0;
}
static inline int lustre_init_quota_info(struct lustre_quota_info *lqi,
                                         int type)
{
        return 0;
}
#endif  /* KERNEL_VERSION(2,5,0) */

#define LL_DQUOT_OFF(sb)    DQUOT_OFF(sb)

typedef int (*dqacq_handler_t) (struct obd_device * obd, struct qunit_data * qd,
                                int opc);
struct lustre_quota_ctxt {
        struct super_block *lqc_sb;     /* superblock this applies to */
        struct obd_import *lqc_import;  /* import used to send dqacq/dqrel RPC */
        dqacq_handler_t lqc_handler;    /* dqacq/dqrel RPC handler, only for quota master */ 
        unsigned long lqc_recovery:1;   /* Doing recovery */ 
        unsigned long lqc_iunit_sz;     /* Unit size of file quota */
        unsigned long lqc_itune_sz;     /* Trigger dqacq when available file quota less than
                                         * this value, trigger dqrel when available file quota
                                         * more than this value + 1 iunit */
        unsigned long lqc_bunit_sz;     /* Unit size of block quota */
        unsigned long lqc_btune_sz;     /* See comment of lqc_itune_sz */
};

#else

struct lustre_quota_info {
};

struct lustre_quota_ctxt {
};

#endif  /* !__KERNEL__ */

#else

#define LL_DQUOT_OFF(sb) do {} while(0)

struct lustre_quota_info {
};

struct lustre_quota_ctxt {
};

#endif /* !HAVE_QUOTA_SUPPORT */

/* If the (quota limit < qunit * slave count), the slave which can't
 * acquire qunit should set it's local limit as MIN_QLIMIT */
#define MIN_QLIMIT      1

struct quotacheck_thread_args {
        struct obd_export   *qta_exp;   /* obd export */
        struct obd_quotactl  qta_oqctl; /* obd_quotactl args */
        struct super_block  *qta_sb;    /* obd super block */
        atomic_t            *qta_sem;   /* obt_quotachecking */
};

typedef struct {
        int (*quota_init) (void);
        int (*quota_exit) (void);
        int (*quota_setup) (struct obd_device *, struct lustre_cfg *);
        int (*quota_cleanup) (struct obd_device *);
        /* For quota master, close admin quota files */
        int (*quota_fs_cleanup) (struct obd_device *);
        int (*quota_ctl) (struct obd_export *, struct obd_quotactl *);
        int (*quota_check) (struct obd_export *, struct obd_quotactl *);
        int (*quota_recovery) (struct obd_device *);
        
        /* For quota master/slave, adjust quota limit after fs operation */
        int (*quota_adjust) (struct obd_device *, unsigned int[], 
                             unsigned int[], int, int); 
        
        /* For quota slave, set import, trigger quota recovery */
        int (*quota_setinfo) (struct obd_export *, struct obd_device *);
        
        /* For quota slave, set proper thread resoure capability */
        int (*quota_enforce) (struct obd_device *, unsigned int);
        
        /* For quota slave, check whether specified uid/gid is over quota */
        int (*quota_getflag) (struct obd_device *, struct obdo *);
        
        /* For quota slave, acquire/release quota from master if needed */
        int (*quota_acquire) (struct obd_device *, unsigned int, unsigned int);
        
        /* For quota client, poll if the quota check done */
        int (*quota_poll_check) (struct obd_export *, struct if_quotacheck *);
        
        /* For quota client, check whether specified uid/gid is over quota */
        int (*quota_chkdq) (struct client_obd *, unsigned int, unsigned int);
        
        /* For quota client, set over quota flag for specifed uid/gid */
        int (*quota_setdq) (struct client_obd *, unsigned int, unsigned int,
                            obd_flag, obd_flag);
} quota_interface_t;

#define Q_COPY(out, in, member) (out)->member = (in)->member

#define QUOTA_OP(interface, op) interface->quota_ ## op         

#define QUOTA_CHECK_OP(interface, op)                           \
do {                                                            \
        if (!interface)                                         \
                RETURN(0);                                      \
        if (!QUOTA_OP(interface, op)) {                         \
                CERROR("no quota operation: " #op "\n");        \
                RETURN(-EOPNOTSUPP);                            \
        }                                                       \
} while(0)

static inline int lquota_init(quota_interface_t *interface)
{
        int rc;
        ENTRY;
        
        QUOTA_CHECK_OP(interface, init);
        rc = QUOTA_OP(interface, init)();
        RETURN(rc);
}

static inline int lquota_exit(quota_interface_t *interface) 
{
        int rc;
        ENTRY;
        
        QUOTA_CHECK_OP(interface, exit);
        rc = QUOTA_OP(interface, exit)();
        RETURN(rc);
}

static inline int lquota_setup(quota_interface_t *interface,
                               struct obd_device *obd, 
                               struct lustre_cfg *lcfg) 
{
        int rc;
        ENTRY;
        
        QUOTA_CHECK_OP(interface, setup);
        rc = QUOTA_OP(interface, setup)(obd, lcfg);
        RETURN(rc);
}

static inline int lquota_cleanup(quota_interface_t *interface,
                                 struct obd_device *obd) 
{
        int rc;
        ENTRY;
        
        QUOTA_CHECK_OP(interface, cleanup);
        rc = QUOTA_OP(interface, cleanup)(obd);
        RETURN(rc);
}

static inline int lquota_fs_cleanup(quota_interface_t *interface,
                                    struct obd_device *obd)
{
        int rc;
        ENTRY;
        
        QUOTA_CHECK_OP(interface, fs_cleanup);
        rc = QUOTA_OP(interface, fs_cleanup)(obd);
        RETURN(rc);
}

static inline int lquota_recovery(quota_interface_t *interface,
                                  struct obd_device *obd) 
{        
        int rc;
        ENTRY;
        
        QUOTA_CHECK_OP(interface, recovery);
        rc = QUOTA_OP(interface, recovery)(obd);
        RETURN(rc);
}

static inline int lquota_adjust(quota_interface_t *interface,
                                struct obd_device *obd, 
                                unsigned int qcids[], 
                                unsigned int qpids[], 
                                int rc, int opc) 
{
        int ret;
        ENTRY;
        
        QUOTA_CHECK_OP(interface, adjust);
        ret = QUOTA_OP(interface, adjust)(obd, qcids, qpids, rc, opc);
        RETURN(ret);
}

static inline int lquota_chkdq(quota_interface_t *interface,
                               struct client_obd *cli,
                               unsigned int uid, unsigned int gid)
{
        int rc;
        ENTRY;
        
        QUOTA_CHECK_OP(interface, chkdq);
        rc = QUOTA_OP(interface, chkdq)(cli, uid, gid);
        RETURN(rc);
}

static inline int lquota_setdq(quota_interface_t *interface,
                               struct client_obd *cli,
                               unsigned int uid, unsigned int gid,
                               obd_flag valid, obd_flag flags)
{
        int rc;
        ENTRY;
        
        QUOTA_CHECK_OP(interface, setdq);
        rc = QUOTA_OP(interface, setdq)(cli, uid, gid, valid, flags);
        RETURN(rc);
}

static inline int lquota_poll_check(quota_interface_t *interface,
                                    struct obd_export *exp,
                                    struct if_quotacheck *qchk)
{
        int rc;
        ENTRY;
        
        QUOTA_CHECK_OP(interface, poll_check);
        rc = QUOTA_OP(interface, poll_check)(exp, qchk);
        RETURN(rc);
}

       
static inline int lquota_setinfo(quota_interface_t *interface,
                                 struct obd_export *exp, 
                                 struct obd_device *obd) 
{
        int rc;
        ENTRY;

        QUOTA_CHECK_OP(interface, setinfo);
        rc = QUOTA_OP(interface, setinfo)(exp, obd);
        RETURN(rc);
}

static inline int lquota_enforce(quota_interface_t *interface, 
                                 struct obd_device *obd,
                                 unsigned int ignore)
{
        int rc;
        ENTRY;

        QUOTA_CHECK_OP(interface, enforce);
        rc = QUOTA_OP(interface, enforce)(obd, ignore);
        RETURN(rc);
}

static inline int lquota_getflag(quota_interface_t *interface,
                                 struct obd_device *obd, struct obdo *oa)
{
        int rc;
        ENTRY;

        QUOTA_CHECK_OP(interface, getflag);
        rc = QUOTA_OP(interface, getflag)(obd, oa);
        RETURN(rc);
}
        
static inline int lquota_acquire(quota_interface_t *interface,
                                 struct obd_device *obd, 
                                 unsigned int uid, unsigned int gid)
{
        int rc;
        ENTRY;

        QUOTA_CHECK_OP(interface, acquire);
        rc = QUOTA_OP(interface, acquire)(obd, uid, gid);
        RETURN(rc);
}

#ifndef __KERNEL__
extern quota_interface_t osc_quota_interface;
extern quota_interface_t mdc_quota_interface;
extern quota_interface_t lov_quota_interface;
#endif

#endif /* _LUSTRE_QUOTA_H */
