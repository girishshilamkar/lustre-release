#ifndef _MDS_INTERNAL_H
#define _MDS_INTERNAL_H
static inline struct mds_obd *mds_req2mds(struct ptlrpc_request *req)
{
        return &req->rq_export->exp_obd->u.mds;
}

/* mds/mds_fs.c */
struct llog_handle *mds_log_create(struct obd_device *obd);
int mds_log_close(struct llog_handle *cathandle, struct llog_handle *loghandle);
struct llog_handle *mds_log_open(struct obd_device *obd,
                                 struct llog_cookie *logcookie);
struct llog_handle *mds_get_catalog(struct obd_device *obd);
void mds_put_catalog(struct llog_handle *cathandle);


/* mds/mds_reint.c */
void mds_commit_cb(struct obd_device *, __u64 last_rcvd, void *data, int error);
int mds_finish_transno(struct mds_obd *mds, struct inode *inode, void *handle,
                       struct ptlrpc_request *req, int rc, __u32 op_data);

/* mds/mds_lib.c */
int mds_update_unpack(struct ptlrpc_request *, int offset,
                      struct mds_update_record *);

/* mds/mds_lov.c */
int mds_get_lovtgts(struct mds_obd *mds, int tgt_count,
                    struct obd_uuid *uuidarray);

/* mds/mds_open.c */
int mds_open(struct mds_update_record *rec, int offset,
             struct ptlrpc_request *req, struct lustre_handle *);
int mds_pin(struct ptlrpc_request *req);
int mds_mfd_close(struct ptlrpc_request *req, struct obd_device *obd,
		  struct mds_file_data *mfd);
int mds_close(struct ptlrpc_request *req);


/* mds/mds_fs.c */
int mds_client_add(struct obd_device *obd, struct mds_obd *mds,
		   struct mds_export_data *med, int cl_off);
int mds_client_free(struct obd_export *exp, int clear_client);

#ifdef __KERNEL__
void mds_pack_inode2fid(struct ll_fid *fid, struct inode *inode);
void mds_pack_inode2body(struct mds_body *body, struct inode *inode);
#endif

#endif /* _MDS_INTERNAL_H */
