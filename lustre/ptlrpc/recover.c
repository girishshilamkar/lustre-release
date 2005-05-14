/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Portal-RPC reconnection and replay operations, for use in recovery.
 *
 *  Copyright (c) 2002, 2003 Cluster File Systems, Inc.
 *   Author: Mike Shaver <shaver@clusterfs.com>
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#define DEBUG_SUBSYSTEM S_RPC
#ifdef __KERNEL__
# include <linux/config.h>
# include <linux/module.h>
# include <linux/kmod.h>
#else
# include <liblustre.h>
#endif

#include <linux/obd_support.h>
#include <linux/lustre_ha.h>
#include <linux/lustre_net.h>
#include <linux/lustre_import.h>
#include <linux/lustre_export.h>
#include <linux/obd.h>
#include <linux/obd_ost.h>
#include <linux/obd_class.h>
#include <linux/obd_lov.h> /* for IOC_LOV_SET_OSC_ACTIVE */

#include "ptlrpc_internal.h"

static int ptlrpc_recover_import_no_retry(struct obd_import *, char *);

void ptlrpc_run_recovery_over_upcall(struct obd_device *obd)
{
        char *argv[4];
        char *envp[3];
        int rc;
        ENTRY;

        argv[0] = obd_lustre_upcall;
        argv[1] = "RECOVERY_OVER";
        argv[2] = obd->obd_uuid.uuid;
        argv[3] = NULL;
        
        envp[0] = "HOME=/";
        envp[1] = "PATH=/sbin:/bin:/usr/sbin:/usr/bin";
        envp[2] = NULL;

        rc = USERMODEHELPER(argv[0], argv, envp);
        if (rc < 0) {
                CERROR("Error invoking recovery upcall %s %s %s: %d; check "
                       "/proc/sys/lustre/upcall\n",
                       argv[0], argv[1], argv[2], rc);

        } else {
                CWARN("Invoked upcall %s %s %s\n",
                      argv[0], argv[1], argv[2]);
        }
}

void ptlrpc_run_failed_import_upcall(struct obd_import* imp)
{
#ifdef __KERNEL__
        unsigned long flags;
        char *argv[7];
        char *envp[3];
        int rc;
        ENTRY;

        spin_lock_irqsave(&imp->imp_lock, flags);
        if (imp->imp_state == LUSTRE_IMP_CLOSED) {
                spin_unlock_irqrestore(&imp->imp_lock, flags);
                EXIT;
                return;
        }
        spin_unlock_irqrestore(&imp->imp_lock, flags);
        
        argv[0] = obd_lustre_upcall;
        argv[1] = "FAILED_IMPORT";
        argv[2] = imp->imp_target_uuid.uuid;
        argv[3] = imp->imp_obd->obd_name;
        argv[4] = imp->imp_connection->c_remote_uuid.uuid;
        argv[5] = imp->imp_obd->obd_uuid.uuid;
        argv[6] = NULL;

        envp[0] = "HOME=/";
        envp[1] = "PATH=/sbin:/bin:/usr/sbin:/usr/bin";
        envp[2] = NULL;

        rc = USERMODEHELPER(argv[0], argv, envp);
        if (rc < 0) {
                CERROR("Error invoking recovery upcall %s %s %s %s %s: %d; "
                       "check /proc/sys/lustre/upcall\n",
                       argv[0], argv[1], argv[2], argv[3], argv[4],rc);

        } else {
                CWARN("Invoked upcall %s %s %s %s %s\n",
                      argv[0], argv[1], argv[2], argv[3], argv[4]);
        }
#else
        if (imp->imp_state == LUSTRE_IMP_CLOSED) {
                EXIT;
                return;
        }
        ptlrpc_recover_import(imp, NULL);
#endif
}

/* This might block waiting for the upcall to start, so it should
 * not be called from a thread that shouldn't block. (Like ptlrpcd) */
void ptlrpc_initiate_recovery(struct obd_import *imp)
{
        ENTRY;

        LASSERT (obd_lustre_upcall != NULL);
        
        if (strcmp(obd_lustre_upcall, "DEFAULT") == 0) {
                CDEBUG(D_HA, "%s: starting recovery without upcall\n",
                        imp->imp_target_uuid.uuid);
                ptlrpc_connect_import(imp, NULL);
        } 
        else if (strcmp(obd_lustre_upcall, "NONE") == 0) {
                CDEBUG(D_HA, "%s: recovery disabled\n",
                        imp->imp_target_uuid.uuid);
        } 
        else {
                CDEBUG(D_HA, "%s: calling upcall to start recovery\n",
                        imp->imp_target_uuid.uuid);
                ptlrpc_run_failed_import_upcall(imp);
        }

        EXIT;
}

int ptlrpc_replay_next(struct obd_import *imp, int *inflight)
{
        int rc = 0;
        struct list_head *tmp, *pos;
        struct ptlrpc_request *req = NULL;
        unsigned long flags;
        __u64 last_transno;
        ENTRY;

        *inflight = 0;

        /* It might have committed some after we last spoke, so make sure we
         * get rid of them now.
         */
        spin_lock_irqsave(&imp->imp_lock, flags);
        ptlrpc_free_committed(imp);
        last_transno = imp->imp_last_replay_transno;
        spin_unlock_irqrestore(&imp->imp_lock, flags);

        CDEBUG(D_HA, "import %p from %s committed "LPU64" last "LPU64"\n",
               imp, imp->imp_target_uuid.uuid, imp->imp_peer_committed_transno,
               last_transno);

        /* Do I need to hold a lock across this iteration?  We shouldn't be
         * racing with any additions to the list, because we're in recovery
         * and are therefore not processing additional requests to add.  Calls
         * to ptlrpc_free_committed might commit requests, but nothing "newer"
         * than the one we're replaying (it can't be committed until it's
         * replayed, and we're doing that here).  l_f_e_safe protects against
         * problems with the current request being committed, in the unlikely
         * event of that race.  So, in conclusion, I think that it's safe to
         * perform this list-walk without the imp_lock held.
         *
         * But, the {mdc,osc}_replay_open callbacks both iterate
         * request lists, and have comments saying they assume the
         * imp_lock is being held by ptlrpc_replay, but it's not. it's
         * just a little race...
         */
        list_for_each_safe(tmp, pos, &imp->imp_replay_list) {
                req = list_entry(tmp, struct ptlrpc_request, rq_replay_list);

                /* If need to resend, stop on the matching one first. It's 
                   possible though it's already been committed, so in that case 
                   we'll just continue with replay */
                if (imp->imp_resend_replay && 
                    req->rq_transno == last_transno) {
                        lustre_msg_add_flags(req->rq_reqmsg, MSG_RESENT);
                        break;
                }

                if (req->rq_transno > last_transno) {
                        imp->imp_last_replay_transno = req->rq_transno;
                        break;
                }

                req = NULL;
        }

        imp->imp_resend_replay = 0;

        if (req != NULL) {
                rc = ptlrpc_replay_req(req);
                if (rc) {
                        CERROR("recovery replay error %d for req "
                               LPD64"\n", rc, req->rq_xid);
                        RETURN(rc);
                }
                imp->imp_reqs_replayed++;
                *inflight = 1;
        }
        RETURN(rc);
}

int ptlrpc_resend(struct obd_import *imp)
{
        struct list_head *tmp, *pos;
        struct ptlrpc_request *req;
        unsigned long flags;

        ENTRY;

        /* As long as we're in recovery, nothing should be added to the sending
         * list, so we don't need to hold the lock during this iteration and
         * resend process.
         */
        /* Well... what if lctl recover is called twice at the same time?
         */
        spin_lock_irqsave(&imp->imp_lock, flags);
        if (imp->imp_state != LUSTRE_IMP_RECOVER) {
                spin_unlock_irqrestore(&imp->imp_lock, flags);
                RETURN(-1);
        }
        spin_unlock_irqrestore(&imp->imp_lock, flags);

        list_for_each_safe(tmp, pos, &imp->imp_sending_list) {
                req = list_entry(tmp, struct ptlrpc_request, rq_list);
                ptlrpc_resend_req(req);
        }

        RETURN(0);
}

void ptlrpc_wake_delayed(struct obd_import *imp)
{
        unsigned long flags;
        struct list_head *tmp, *pos;
        struct ptlrpc_request *req;

        spin_lock_irqsave(&imp->imp_lock, flags);
        list_for_each_safe(tmp, pos, &imp->imp_delayed_list) {
                req = list_entry(tmp, struct ptlrpc_request, rq_list);

                DEBUG_REQ(D_HA, req, "waking (set %p):", req->rq_set);
                ptlrpc_wake_client_req(req);
        }
        spin_unlock_irqrestore(&imp->imp_lock, flags);
}

void ptlrpc_request_handle_notconn(struct ptlrpc_request *failed_req)
{
        int rc;
        struct obd_import *imp= failed_req->rq_import;
        unsigned long flags;
        ENTRY;

        CDEBUG(D_HA, "import %s of %s@%s abruptly disconnected: reconnecting\n",
               imp->imp_obd->obd_name,
               imp->imp_target_uuid.uuid,
               imp->imp_connection->c_remote_uuid.uuid);
        
        if (ptlrpc_set_import_discon(imp)) {
                if (!imp->imp_replayable) {
                        CDEBUG(D_HA, "import %s@%s for %s not replayable, "
                               "auto-deactivating\n",
                               imp->imp_target_uuid.uuid,
                               imp->imp_connection->c_remote_uuid.uuid,
                               imp->imp_obd->obd_name);
                        ptlrpc_deactivate_import(imp);
                }

                rc = ptlrpc_connect_import(imp, NULL);
        }

        /* Wait for recovery to complete and resend. If evicted, then
           this request will be errored out later.*/
        spin_lock_irqsave(&failed_req->rq_lock, flags);
        failed_req->rq_resend = 1;
        spin_unlock_irqrestore(&failed_req->rq_lock, flags);

        EXIT;
}

/*
 * This should only be called by the ioctl interface, currently
 * with the lctl deactivate and activate commands.
 */
int ptlrpc_set_import_active(struct obd_import *imp, int active)
{
        struct obd_device *obd = imp->imp_obd;
        int rc = 0;

        LASSERT(obd);

        /* When deactivating, mark import invalid, and abort in-flight
         * requests. */
        if (!active) {
                ptlrpc_invalidate_import(imp, 0);
                imp->imp_deactive = 1;
        }

        /* When activating, mark import valid, and attempt recovery */
        if (active) {
                imp->imp_deactive = 0;
                CDEBUG(D_HA, "setting import %s VALID\n",
                       imp->imp_target_uuid.uuid);
                rc = ptlrpc_recover_import(imp, NULL);
        }

        RETURN(rc);
}

int ptlrpc_recover_import(struct obd_import *imp, char *new_uuid)
{
        int rc;
        ENTRY;

        /* force import to be disconnected. */
        ptlrpc_set_import_discon(imp);

        rc = ptlrpc_recover_import_no_retry(imp, new_uuid);

        RETURN(rc);
}

int ptlrpc_import_in_recovery(struct obd_import *imp)
{
        unsigned long flags;
        int in_recovery = 1;
        spin_lock_irqsave(&imp->imp_lock, flags);
        if (imp->imp_state == LUSTRE_IMP_FULL ||
            imp->imp_state == LUSTRE_IMP_CLOSED ||
            imp->imp_state == LUSTRE_IMP_DISCON)
                in_recovery = 0;
        spin_unlock_irqrestore(&imp->imp_lock, flags);
        return in_recovery;
}

int ptlrpc_import_control_recovery(struct obd_import *imp, int disable)
{
        unsigned long flags;

        /* with imp_deactivate == 1 pinger won't initiate re-connect */
        spin_lock_irqsave(&imp->imp_lock, flags);
        if (disable)
                imp->imp_deactive = 1;
        else
                imp->imp_deactive = 0;
        if (imp->imp_state == LUSTRE_IMP_DISCON) {
                imp->imp_force_verify = 1;
                ptlrpc_pinger_wake_up();
        }
        spin_unlock_irqrestore(&imp->imp_lock, flags);
        RETURN(0);
}

static int ptlrpc_recover_import_no_retry(struct obd_import *imp,
                                          char *new_uuid)
{
        int rc;
        unsigned long flags;
        int in_recovery = 0;
        struct l_wait_info lwi;
        ENTRY;

        spin_lock_irqsave(&imp->imp_lock, flags);
        if (imp->imp_state != LUSTRE_IMP_DISCON) {
                in_recovery = 1;
        }
        spin_unlock_irqrestore(&imp->imp_lock, flags);

        if (in_recovery == 1)
                RETURN(-EALREADY);

        rc = ptlrpc_connect_import(imp, new_uuid);
        if (rc)
                RETURN(rc);

        CDEBUG(D_HA, "%s: recovery started, waiting\n",
               imp->imp_target_uuid.uuid);

        lwi = LWI_TIMEOUT(MAX(obd_timeout * HZ, 1), NULL, NULL);
        rc = l_wait_event(imp->imp_recovery_waitq,
                          !ptlrpc_import_in_recovery(imp), &lwi);
        CDEBUG(D_HA, "%s: recovery finished\n",
               imp->imp_target_uuid.uuid);

        RETURN(rc);
}

void ptlrpc_fail_export(struct obd_export *exp)
{
        int rc, already_failed;
        unsigned long flags;

        spin_lock_irqsave(&exp->exp_lock, flags);
        already_failed = exp->exp_failed;
        exp->exp_failed = 1;
        spin_unlock_irqrestore(&exp->exp_lock, flags);

        if (already_failed) {
                CDEBUG(D_HA, "disconnecting dead export %p/%s; skipping\n",
                       exp, exp->exp_client_uuid.uuid);
                return;
        }

        CDEBUG(D_HA, "disconnecting export %p/%s\n",
               exp, exp->exp_client_uuid.uuid);

        if (obd_dump_on_timeout)
                portals_debug_dumplog();

        /* Most callers into obd_disconnect are removing their own reference
         * (request, for example) in addition to the one from the hash table.
         * We don't have such a reference here, so make one. */
        class_export_get(exp);
        rc = obd_disconnect(exp, 0);
        if (rc)
                CERROR("disconnecting export %p failed: %d\n", exp, rc);
}
