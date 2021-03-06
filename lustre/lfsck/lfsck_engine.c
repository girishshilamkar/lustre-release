/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License version 2 for more details.  A copy is
 * included in the COPYING file that accompanied this code.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2012, 2013, Intel Corporation.
 */
/*
 * lustre/lfsck/lfsck_engine.c
 *
 * Author: Fan, Yong <fan.yong@intel.com>
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_LFSCK

#include <lu_object.h>
#include <dt_object.h>
#include <lustre_net.h>
#include <lustre_fid.h>
#include <obd_support.h>
#include <lustre_lib.h>

#include "lfsck_internal.h"

static void lfsck_unpack_ent(struct lu_dirent *ent, __u64 *cookie)
{
	fid_le_to_cpu(&ent->lde_fid, &ent->lde_fid);
	*cookie = le64_to_cpu(ent->lde_hash);
	ent->lde_reclen = le16_to_cpu(ent->lde_reclen);
	ent->lde_namelen = le16_to_cpu(ent->lde_namelen);
	ent->lde_attrs = le32_to_cpu(ent->lde_attrs);

	/* Make sure the name is terminated with '0'.
	 * The data (type) after ent::lde_name maybe
	 * broken, but we do not care. */
	ent->lde_name[ent->lde_namelen] = 0;
}

static void lfsck_di_oit_put(const struct lu_env *env, struct lfsck_instance *lfsck)
{
	const struct dt_it_ops	*iops;
	struct dt_it		*di;

	spin_lock(&lfsck->li_lock);
	iops = &lfsck->li_obj_oit->do_index_ops->dio_it;
	di = lfsck->li_di_oit;
	lfsck->li_di_oit = NULL;
	spin_unlock(&lfsck->li_lock);
	iops->put(env, di);
}

static void lfsck_di_dir_put(const struct lu_env *env, struct lfsck_instance *lfsck)
{
	const struct dt_it_ops	*iops;
	struct dt_it		*di;

	spin_lock(&lfsck->li_lock);
	iops = &lfsck->li_obj_dir->do_index_ops->dio_it;
	di = lfsck->li_di_dir;
	lfsck->li_di_dir = NULL;
	lfsck->li_cookie_dir = 0;
	spin_unlock(&lfsck->li_lock);
	iops->put(env, di);
}

static void lfsck_close_dir(const struct lu_env *env,
			    struct lfsck_instance *lfsck)
{
	struct dt_object	*dir_obj  = lfsck->li_obj_dir;
	const struct dt_it_ops	*dir_iops = &dir_obj->do_index_ops->dio_it;
	struct dt_it		*dir_di   = lfsck->li_di_dir;

	lfsck_di_dir_put(env, lfsck);
	dir_iops->fini(env, dir_di);
	lfsck->li_obj_dir = NULL;
	lfsck_object_put(env, dir_obj);
}

static int lfsck_master_dir_engine(const struct lu_env *env,
				   struct lfsck_instance *lfsck)
{
	struct lfsck_thread_info	*info	= lfsck_env_info(env);
	const struct dt_it_ops		*iops	=
			&lfsck->li_obj_dir->do_index_ops->dio_it;
	struct dt_it			*di	= lfsck->li_di_dir;
	struct lu_dirent		*ent	= &info->lti_ent;
	struct lu_fid			*fid	= &info->lti_fid;
	struct lfsck_bookmark		*bk	= &lfsck->li_bookmark_ram;
	struct ptlrpc_thread		*thread = &lfsck->li_thread;
	int				 rc;
	ENTRY;

	do {
		struct dt_object *child;

		if (OBD_FAIL_CHECK(OBD_FAIL_LFSCK_DELAY2) &&
		    cfs_fail_val > 0) {
			struct l_wait_info lwi;

			lwi = LWI_TIMEOUT(cfs_time_seconds(cfs_fail_val),
					  NULL, NULL);
			l_wait_event(thread->t_ctl_waitq,
				     !thread_is_running(thread),
				     &lwi);
		}

		lfsck->li_new_scanned++;
		rc = iops->rec(env, di, (struct dt_rec *)ent,
			       lfsck->li_args_dir);
		lfsck_unpack_ent(ent, &lfsck->li_cookie_dir);
		if (rc != 0) {
			lfsck_fail(env, lfsck, true);
			if (bk->lb_param & LPF_FAILOUT)
				RETURN(rc);
			else
				goto checkpoint;
		}

		if (ent->lde_attrs & LUDA_IGNORE)
			goto checkpoint;

		*fid = ent->lde_fid;
		child = lfsck_object_find(env, lfsck, fid);
		if (child == NULL) {
			goto checkpoint;
		} else if (IS_ERR(child)) {
			lfsck_fail(env, lfsck, true);
			if (bk->lb_param & LPF_FAILOUT)
				RETURN(PTR_ERR(child));
			else
				goto checkpoint;
		}

		/* XXX: Currently, skip remote object, the consistency for
		 *	remote object will be processed in LFSCK phase III. */
		if (dt_object_exists(child) && !dt_object_remote(child))
			rc = lfsck_exec_dir(env, lfsck, child, ent);
		lfsck_object_put(env, child);
		if (rc != 0 && bk->lb_param & LPF_FAILOUT)
			RETURN(rc);

checkpoint:
		rc = lfsck_checkpoint(env, lfsck);
		if (rc != 0 && bk->lb_param & LPF_FAILOUT)
			RETURN(rc);

		/* Rate control. */
		lfsck_control_speed(lfsck);
		if (unlikely(!thread_is_running(thread)))
			RETURN(0);

		if (OBD_FAIL_CHECK(OBD_FAIL_LFSCK_FATAL2)) {
			spin_lock(&lfsck->li_lock);
			thread_set_flags(thread, SVC_STOPPING);
			spin_unlock(&lfsck->li_lock);
			RETURN(-EINVAL);
		}

		rc = iops->next(env, di);
	} while (rc == 0);

	if (rc > 0 && !lfsck->li_oit_over)
		lfsck_close_dir(env, lfsck);

	RETURN(rc);
}

static int lfsck_master_oit_engine(const struct lu_env *env,
				   struct lfsck_instance *lfsck)
{
	struct lfsck_thread_info	*info	= lfsck_env_info(env);
	const struct dt_it_ops		*iops	=
				&lfsck->li_obj_oit->do_index_ops->dio_it;
	struct dt_it			*di	= lfsck->li_di_oit;
	struct lu_fid			*fid	= &info->lti_fid;
	struct lfsck_bookmark		*bk	= &lfsck->li_bookmark_ram;
	struct ptlrpc_thread		*thread = &lfsck->li_thread;
	int				 rc;
	ENTRY;

	do {
		struct dt_object *target;

		if (lfsck->li_di_dir != NULL) {
			rc = lfsck_master_dir_engine(env, lfsck);
			if (rc <= 0)
				RETURN(rc);
		}

		if (unlikely(lfsck->li_oit_over))
			RETURN(1);

		if (OBD_FAIL_CHECK(OBD_FAIL_LFSCK_DELAY1) &&
		    cfs_fail_val > 0) {
			struct l_wait_info lwi;

			lwi = LWI_TIMEOUT(cfs_time_seconds(cfs_fail_val),
					  NULL, NULL);
			l_wait_event(thread->t_ctl_waitq,
				     !thread_is_running(thread),
				     &lwi);
		}

		if (OBD_FAIL_CHECK(OBD_FAIL_LFSCK_CRASH))
			RETURN(0);

		lfsck->li_current_oit_processed = 1;
		lfsck->li_new_scanned++;
		rc = iops->rec(env, di, (struct dt_rec *)fid, 0);
		if (rc != 0) {
			lfsck_fail(env, lfsck, true);
			if (bk->lb_param & LPF_FAILOUT)
				RETURN(rc);
			else
				goto checkpoint;
		}

		target = lfsck_object_find(env, lfsck, fid);
		if (target == NULL) {
			goto checkpoint;
		} else if (IS_ERR(target)) {
			lfsck_fail(env, lfsck, true);
			if (bk->lb_param & LPF_FAILOUT)
				RETURN(PTR_ERR(target));
			else
				goto checkpoint;
		}

		/* XXX: Currently, skip remote object, the consistency for
		 *	remote object will be processed in LFSCK phase III. */
		if (dt_object_exists(target) && !dt_object_remote(target))
			rc = lfsck_exec_oit(env, lfsck, target);
		lfsck_object_put(env, target);
		if (rc != 0 && bk->lb_param & LPF_FAILOUT)
			RETURN(rc);

checkpoint:
		rc = lfsck_checkpoint(env, lfsck);
		if (rc != 0 && bk->lb_param & LPF_FAILOUT)
			RETURN(rc);

		/* Rate control. */
		lfsck_control_speed(lfsck);

		if (OBD_FAIL_CHECK(OBD_FAIL_LFSCK_FATAL1)) {
			spin_lock(&lfsck->li_lock);
			thread_set_flags(thread, SVC_STOPPING);
			spin_unlock(&lfsck->li_lock);
			RETURN(-EINVAL);
		}

		rc = iops->next(env, di);
		if (unlikely(rc > 0))
			lfsck->li_oit_over = 1;
		else if (likely(rc == 0))
			lfsck->li_current_oit_processed = 0;

		if (unlikely(!thread_is_running(thread)))
			RETURN(0);
	} while (rc == 0 || lfsck->li_di_dir != NULL);

	RETURN(rc);
}

int lfsck_master_engine(void *args)
{
	struct lu_env		 env;
	struct lfsck_instance	*lfsck    = (struct lfsck_instance *)args;
	struct ptlrpc_thread	*thread   = &lfsck->li_thread;
	struct dt_object	*oit_obj  = lfsck->li_obj_oit;
	const struct dt_it_ops	*oit_iops = &oit_obj->do_index_ops->dio_it;
	struct dt_it		*oit_di;
	int			 rc;
	ENTRY;

	rc = lu_env_init(&env, LCT_MD_THREAD | LCT_DT_THREAD);
	if (rc != 0) {
		CERROR("%s: LFSCK, fail to init env, rc = %d\n",
		       lfsck_lfsck2name(lfsck), rc);
		GOTO(noenv, rc);
	}

	oit_di = oit_iops->init(&env, oit_obj, lfsck->li_args_oit, BYPASS_CAPA);
	if (IS_ERR(oit_di)) {
		rc = PTR_ERR(oit_di);
		CERROR("%s: LFSCK, fail to init iteration, rc = %d\n",
		       lfsck_lfsck2name(lfsck), rc);
		GOTO(fini_env, rc);
	}

	spin_lock(&lfsck->li_lock);
	lfsck->li_di_oit = oit_di;
	spin_unlock(&lfsck->li_lock);
	rc = lfsck_prep(&env, lfsck);
	if (rc != 0)
		GOTO(fini_oit, rc);

	CDEBUG(D_LFSCK, "LFSCK entry: oit_flags = 0x%x, dir_flags = 0x%x, "
	       "oit_cookie = "LPU64", dir_cookie = "LPU64", parent = "DFID
	       ", pid = %d\n", lfsck->li_args_oit, lfsck->li_args_dir,
	       lfsck->li_pos_current.lp_oit_cookie,
	       lfsck->li_pos_current.lp_dir_cookie,
	       PFID(&lfsck->li_pos_current.lp_dir_parent),
	       cfs_curproc_pid());

	spin_lock(&lfsck->li_lock);
	thread_set_flags(thread, SVC_RUNNING);
	spin_unlock(&lfsck->li_lock);
	cfs_waitq_broadcast(&thread->t_ctl_waitq);

	if (!cfs_list_empty(&lfsck->li_list_scan) ||
	    cfs_list_empty(&lfsck->li_list_double_scan))
		rc = lfsck_master_oit_engine(&env, lfsck);
	else
		rc = 1;

	CDEBUG(D_LFSCK, "LFSCK exit: oit_flags = 0x%x, dir_flags = 0x%x, "
	       "oit_cookie = "LPU64", dir_cookie = "LPU64", parent = "DFID
	       ", pid = %d, rc = %d\n", lfsck->li_args_oit, lfsck->li_args_dir,
	       lfsck->li_pos_current.lp_oit_cookie,
	       lfsck->li_pos_current.lp_dir_cookie,
	       PFID(&lfsck->li_pos_current.lp_dir_parent),
	       cfs_curproc_pid(), rc);

	if (!OBD_FAIL_CHECK(OBD_FAIL_LFSCK_CRASH))
		rc = lfsck_post(&env, lfsck, rc);
	if (lfsck->li_di_dir != NULL)
		lfsck_close_dir(&env, lfsck);

fini_oit:
	lfsck_di_oit_put(&env, lfsck);
	oit_iops->fini(&env, oit_di);
	if (rc == 1) {
		if (!cfs_list_empty(&lfsck->li_list_double_scan))
			rc = lfsck_double_scan(&env, lfsck);
		else
			rc = 0;
	}

	/* XXX: Purge the pinned objects in the future. */

fini_env:
	lu_env_fini(&env);

noenv:
	spin_lock(&lfsck->li_lock);
	thread_set_flags(thread, SVC_STOPPED);
	cfs_waitq_broadcast(&thread->t_ctl_waitq);
	spin_unlock(&lfsck->li_lock);
	return rc;
}
