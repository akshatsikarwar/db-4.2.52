/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2001-2003
 *	Sleepycat Software.  All rights reserved.
 */
#include "db_config.h"

#ifndef lint
static const char revid[] = "$Id: rep_method.c,v 1.134 2003/11/13 15:41:51 sue Exp $";
#endif /* not lint */

#ifndef NO_SYSTEM_INCLUDES
#include <sys/types.h>

#ifdef HAVE_RPC
#include <rpc/rpc.h>
#endif

#include <stdlib.h>
#include <string.h>
#endif

#include "db_int.h"
#include "dbinc/db_page.h"
#include "dbinc/btree.h"
#include "dbinc/log.h"
#include "dbinc/txn.h"

#ifdef HAVE_RPC
#include "dbinc_auto/db_server.h"
#include "dbinc_auto/rpc_client_ext.h"
#endif

static int __rep_abort_prepared __P((DB_ENV *));
static int __rep_bt_cmp __P((DB *, const DBT *, const DBT *));
static int __rep_client_dbinit __P((DB_ENV *, int));
static int __rep_elect __P((DB_ENV *, int, int, u_int32_t, int *));
static int __rep_elect_init
	       __P((DB_ENV *, DB_LSN *, int, int, int *, u_int32_t *));
static int __rep_flush __P((DB_ENV *));
static int __rep_restore_prepared __P((DB_ENV *));
static int __rep_get_limit __P((DB_ENV *, u_int32_t *, u_int32_t *));
static int __rep_set_limit __P((DB_ENV *, u_int32_t, u_int32_t));
static int __rep_set_request __P((DB_ENV *, u_int32_t, u_int32_t));
static int __rep_set_rep_transport __P((DB_ENV *, int,
    int (*)(DB_ENV *, const DBT *, const DBT *, const DB_LSN *,
    int, u_int32_t)));
static int __rep_start __P((DB_ENV *, DBT *, u_int32_t));
static int __rep_stat __P((DB_ENV *, DB_REP_STAT **, u_int32_t));
static int __rep_wait __P((DB_ENV *, u_int32_t, int *, u_int32_t));

/*
 * __rep_dbenv_create --
 *	Replication-specific initialization of the DB_ENV structure.
 *
 * PUBLIC: int __rep_dbenv_create __P((DB_ENV *));
 */
int
__rep_dbenv_create(dbenv)
	DB_ENV *dbenv;
{
#ifdef HAVE_RPC
	if (F_ISSET(dbenv, DB_ENV_RPCCLIENT)) {
		dbenv->rep_elect = __dbcl_rep_elect;
		dbenv->rep_flush = __dbcl_rep_flush;
		dbenv->rep_process_message = __dbcl_rep_process_message;
		dbenv->rep_start = __dbcl_rep_start;
		dbenv->rep_stat = __dbcl_rep_stat;
		dbenv->get_rep_limit = __dbcl_rep_get_limit;
		dbenv->set_rep_limit = __dbcl_rep_set_limit;
		dbenv->set_rep_request = __dbcl_rep_set_request;
		dbenv->set_rep_transport = __dbcl_rep_set_rep_transport;

	} else
#endif
	{
		dbenv->rep_elect = __rep_elect;
		dbenv->rep_flush = __rep_flush;
		dbenv->rep_process_message = __rep_process_message;
		dbenv->rep_start = __rep_start;
		dbenv->rep_stat = __rep_stat;
		dbenv->get_rep_limit = __rep_get_limit;
		dbenv->set_rep_limit = __rep_set_limit;
		dbenv->set_rep_request = __rep_set_request;
		dbenv->set_rep_transport = __rep_set_rep_transport;
	}

	return (0);
}

/*
 * __rep_open --
 *	Replication-specific initialization of the DB_ENV structure.
 *
 * PUBLIC: int __rep_open __P((DB_ENV *));
 */
int
__rep_open(dbenv)
	DB_ENV *dbenv;
{
	DB_REP *db_rep;
	int ret;

	if ((ret = __os_calloc(dbenv, 1, sizeof(DB_REP), &db_rep)) != 0)
		return (ret);
	dbenv->rep_handle = db_rep;
	ret = __rep_region_init(dbenv);
	return (ret);
}

/*
 * __rep_start --
 *	Become a master or client, and start sending messages to participate
 * in the replication environment.  Must be called after the environment
 * is open.
 *
 * We must protect rep_start, which may change the world, with the rest
 * of the DB library.  Each API interface will count itself as it enters
 * the library.  Rep_start checks the following:
 *
 * rep->msg_th - this is the count of threads currently in rep_process_message
 * rep->start_th - this is set if a thread is in rep_start.
 * rep->handle_cnt - number of threads actively using a dbp in library.
 * rep->txn_cnt - number of active txns.
 * REP_F_READY - Replication flag that indicates that we wish to run
 * recovery, and want to prohibit new transactions from entering and cause
 * existing ones to return immediately (with a DB_LOCK_DEADLOCK error).
 *
 * There is also the rep->timestamp which is updated whenever significant
 * events (i.e., new masters, log rollback, etc).  Upon creation, a handle
 * is associated with the current timestamp.  Each time a handle enters the
 * library it must check if the handle timestamp is the same as the one
 * stored in the replication region.  This prevents the use of handles on
 * clients that reference non-existent files whose creation was backed out
 * during a synchronizing recovery.
 */
static int
__rep_start(dbenv, dbt, flags)
	DB_ENV *dbenv;
	DBT *dbt;
	u_int32_t flags;
{
	DB_LOG *dblp;
	DB_LSN lsn;
	DB_REP *db_rep;
	REP *rep;
	u_int32_t repflags;
	int announce, init_db, redo_prepared, ret, sleep_cnt, t_ret, was_client;

	PANIC_CHECK(dbenv);
	ENV_ILLEGAL_BEFORE_OPEN(dbenv, "DB_ENV->rep_start");
	ENV_REQUIRES_CONFIG(dbenv, dbenv->rep_handle, "rep_start", DB_INIT_REP);

	db_rep = dbenv->rep_handle;
	rep = db_rep->region;

	if ((ret = __db_fchk(dbenv, "DB_ENV->rep_start", flags,
	    DB_REP_CLIENT | DB_REP_LOGSONLY | DB_REP_MASTER)) != 0)
		return (ret);

	/* Exactly one of CLIENT and MASTER must be specified. */
	if ((ret = __db_fcchk(dbenv,
	    "DB_ENV->rep_start", flags, DB_REP_CLIENT, DB_REP_MASTER)) != 0)
		return (ret);
	if (!LF_ISSET(DB_REP_CLIENT | DB_REP_MASTER | DB_REP_LOGSONLY)) {
		__db_err(dbenv,
	"DB_ENV->rep_start: replication mode must be specified");
		return (EINVAL);
	}

	/* Masters can't be logs-only. */
	if ((ret = __db_fcchk(dbenv,
	    "DB_ENV->rep_start", flags, DB_REP_LOGSONLY, DB_REP_MASTER)) != 0)
		return (ret);

	/* We need a transport function. */
	if (dbenv->rep_send == NULL) {
		__db_err(dbenv,
    "DB_ENV->set_rep_transport must be called before DB_ENV->rep_start");
		return (EINVAL);
	}

	/*
	 * If we are about to become (or stay) a master.  Let's flush the log
	 * to close any potential holes that might happen when upgrading from
	 * client to master status.
	 */
	if (LF_ISSET(DB_REP_MASTER) && (ret = __log_flush(dbenv, NULL)) != 0)
		return (ret);

	MUTEX_LOCK(dbenv, db_rep->rep_mutexp);
	/*
	 * We only need one thread to start-up replication, so if
	 * there is another thread in rep_start, we'll let it finish
	 * its work and have this thread simply return.
	 */
	if (rep->start_th != 0) {
		/*
		 * There is already someone in rep_start.  Return.
		 */
#ifdef DIAGNOSTIC
		if (FLD_ISSET(dbenv->verbose, DB_VERB_REPLICATION))
			__db_err(dbenv, "Thread already in rep_start");
#endif
		goto err;
	} else
		rep->start_th = 1;

	/*
	 * We are about to delete the replication database.  We need to
	 * make sure that no one else is in the midst of processing a
	 * replication message.
	 */
	for (sleep_cnt = 0; rep->msg_th != 0;) {
		if (++sleep_cnt % 60 == 0)
			__db_err(dbenv,
	"DB_ENV->rep_start waiting %d minutes for replication message thread",
			    sleep_cnt / 60);
		MUTEX_UNLOCK(dbenv, db_rep->rep_mutexp);
		(void)__os_sleep(dbenv, 1, 0);
		MUTEX_LOCK(dbenv, db_rep->rep_mutexp);
	}

	if (rep->eid == DB_EID_INVALID)
		rep->eid = dbenv->rep_eid;

	if (LF_ISSET(DB_REP_MASTER)) {
		was_client = F_ISSET(rep, REP_F_UPGRADE);
		if (was_client) {
			/*
			 * If we're upgrading from having been a client,
			 * preclose, so that we close our temporary database.
			 *
			 * Do not close files that we may have opened while
			 * doing a rep_apply;  they'll get closed when we
			 * finally close the environment, but for now, leave
			 * them open, as we don't want to recycle their
			 * fileids, and we may need the handles again if
			 * we become a client and the original master
			 * that opened them becomes a master again.
			 */
			if ((ret = __rep_preclose(dbenv, 0)) != 0)
				goto errunlock;
		}

		redo_prepared = 0;
		if (!F_ISSET(rep, REP_F_MASTER)) {
			/* Master is not yet set. */
			if (was_client) {
				if (rep->w_gen > rep->recover_gen)
					rep->gen = ++rep->w_gen;
				else if (rep->gen > rep->recover_gen)
					rep->gen++;
				else
					rep->gen = rep->recover_gen + 1;
				/*
				 * There could have been any number of failed
				 * elections, so jump the gen if we need to now.
				 */
				if (rep->egen > rep->gen)
					rep->gen = rep->egen;
				redo_prepared = 1;
			} else if (rep->gen == 0)
				rep->gen = rep->recover_gen + 1;
			if (F_ISSET(rep, REP_F_MASTERELECT)) {
				__rep_elect_done(dbenv, rep);
				F_CLR(rep, REP_F_MASTERELECT);
			}
			if (rep->egen <= rep->gen)
				rep->egen = rep->gen + 1;
#ifdef DIAGNOSTIC
			if (FLD_ISSET(dbenv->verbose, DB_VERB_REPLICATION))
				__db_err(dbenv, "New master gen %lu, egen %lu",
				    (u_long)rep->gen, (u_long)rep->egen);
#endif
		}
		rep->master_id = rep->eid;
		/*
		 * Note, setting flags below implicitly clears out
		 * REP_F_NOARCHIVE, REP_F_INIT and REP_F_READY.
		 */
		rep->flags = REP_F_MASTER;
		MUTEX_UNLOCK(dbenv, db_rep->rep_mutexp);
		dblp = (DB_LOG *)dbenv->lg_handle;
		R_LOCK(dbenv, &dblp->reginfo);
		lsn = ((LOG *)dblp->reginfo.primary)->lsn;
		R_UNLOCK(dbenv, &dblp->reginfo);

		/*
		 * Send the NEWMASTER message first so that clients know
		 * subsequent messages are coming from the right master.
		 * We need to perform all actions below no master what
		 * regarding errors.
		 */
		(void)__rep_send_message(dbenv,
		    DB_EID_BROADCAST, REP_NEWMASTER, &lsn, NULL, 0);
		ret = 0;
		if (was_client)
			ret = __txn_reset(dbenv);
		/*
		 * Take a transaction checkpoint so that our new generation
		 * number get written to the log.
		 */
		if ((t_ret = __txn_checkpoint(dbenv, 0, 0, DB_FORCE)) != 0 &&
		    ret == 0)
			ret = t_ret;
		if (redo_prepared &&
		    (t_ret = __rep_restore_prepared(dbenv)) != 0 && ret == 0)
			ret = t_ret;
	} else {
		init_db = 0;
		was_client = F_ISSET(rep, REP_ISCLIENT);
		announce = !was_client || rep->master_id == DB_EID_INVALID;

		/* Zero out everything except recovery and tally flags. */
		repflags = F_ISSET(rep, REP_F_NOARCHIVE |
		    REP_F_READY | REP_F_RECOVER | REP_F_TALLY);
		if (LF_ISSET(DB_REP_LOGSONLY))
			FLD_SET(repflags, REP_F_LOGSONLY);
		else
			FLD_SET(repflags, REP_F_UPGRADE);

		rep->flags = repflags;
		if (!was_client) {
			rep->master_id = DB_EID_INVALID;
			init_db = 1;
		}
		MUTEX_UNLOCK(dbenv, db_rep->rep_mutexp);

		/*
		 * Abort any prepared transactions that were restored
		 * by recovery.  We won't be able to create any txns of
		 * our own until they're resolved, but we can't resolve
		 * them ourselves;  the master has to.  If any get
		 * resolved as commits, we'll redo them when commit
		 * records come in.  Aborts will simply be ignored.
		 */
		if ((ret = __rep_abort_prepared(dbenv)) != 0)
			goto errlock;

		if ((ret = __rep_client_dbinit(dbenv, init_db)) != 0)
			goto errlock;

		/*
		 * If this client created a newly replicated environment,
		 * then announce the existence of this client.  The master
		 * should respond with a message that will tell this client
		 * the current generation number and the current LSN.  This
		 * will allow the client to either perform recovery or
		 * simply join in.
		 */
		if (announce)
			(void)__rep_send_message(dbenv,
			    DB_EID_BROADCAST, REP_NEWCLIENT, NULL, dbt, 0);
	}

	/*
	 * We have separate labels for errors.  If we're returning an
	 * error before we've set start_th, we use 'err'.  If
	 * we are erroring while holding the rep_mutex, then we use
	 * 'errunlock' label.  If we're erroring without holding the rep
	 * mutex we must use 'errlock'.
	 */
errlock:
	MUTEX_LOCK(dbenv, db_rep->rep_mutexp);
errunlock:
	rep->start_th = 0;
err:	MUTEX_UNLOCK(dbenv, db_rep->rep_mutexp);
	return (ret);
}

/*
 * __rep_client_dbinit --
 *
 * Initialize the LSN database on the client side.  This is called from the
 * client initialization code.  The startup flag value indicates if
 * this is the first thread/process starting up and therefore should create
 * the LSN database.  This routine must be called once by each process acting
 * as a client.
 */
static int
__rep_client_dbinit(dbenv, startup)
	DB_ENV *dbenv;
	int startup;
{
	DB_REP *db_rep;
	DB *dbp;
	int ret, t_ret;
	u_int32_t flags;

	PANIC_CHECK(dbenv);
	db_rep = dbenv->rep_handle;
	dbp = NULL;

#define	REPDBNAME	"__db.rep.db"

	/* Check if this has already been called on this environment. */
	if (db_rep->rep_db != NULL)
		return (0);

	MUTEX_LOCK(dbenv, db_rep->db_mutexp);

	if (startup) {
		if ((ret = db_create(&dbp, dbenv, DB_REP_CREATE)) != 0)
			goto err;
		/*
		 * Ignore errors, because if the file doesn't exist, this
		 * is perfectly OK.
		 */
		(void)__db_remove(dbp, NULL, REPDBNAME, NULL, DB_FORCE);
	}

	if ((ret = db_create(&dbp, dbenv, DB_REP_CREATE)) != 0)
		goto err;
	if ((ret = __bam_set_bt_compare(dbp, __rep_bt_cmp)) != 0)
		goto err;

	/* Allow writes to this database on a client. */
	F_SET(dbp, DB_AM_CL_WRITER);

	flags = DB_NO_AUTO_COMMIT |
	    (startup ? DB_CREATE : 0) |
	    (F_ISSET(dbenv, DB_ENV_THREAD) ? DB_THREAD : 0);

	if ((ret = __db_open(dbp, NULL,
	    REPDBNAME, NULL, DB_BTREE, flags, 0, PGNO_BASE_MD)) != 0)
		goto err;

	db_rep->rep_db = dbp;

	if (0) {
err:		if (dbp != NULL &&
		    (t_ret = __db_close(dbp, NULL, DB_NOSYNC)) != 0 && ret == 0)
			ret = t_ret;
		db_rep->rep_db = NULL;
	}

	MUTEX_UNLOCK(dbenv, db_rep->db_mutexp);

	return (ret);
}

/*
 * __rep_bt_cmp --
 *
 * Comparison function for the LSN table.  We use the entire control
 * structure as a key (for simplicity, so we don't have to merge the
 * other fields in the control with the data field), but really only
 * care about the LSNs.
 */
static int
__rep_bt_cmp(dbp, dbt1, dbt2)
	DB *dbp;
	const DBT *dbt1, *dbt2;
{
	DB_LSN lsn1, lsn2;
	REP_CONTROL *rp1, *rp2;

	COMPQUIET(dbp, NULL);

	rp1 = dbt1->data;
	rp2 = dbt2->data;

	__ua_memcpy(&lsn1, &rp1->lsn, sizeof(DB_LSN));
	__ua_memcpy(&lsn2, &rp2->lsn, sizeof(DB_LSN));

	if (lsn1.file > lsn2.file)
		return (1);

	if (lsn1.file < lsn2.file)
		return (-1);

	if (lsn1.offset > lsn2.offset)
		return (1);

	if (lsn1.offset < lsn2.offset)
		return (-1);

	return (0);
}

/*
 * __rep_abort_prepared --
 *	Abort any prepared transactions that recovery restored.
 *
 *	This is used by clients that have just run recovery, since
 * they cannot/should not call txn_recover and handle prepared transactions
 * themselves.
 */
static int
__rep_abort_prepared(dbenv)
	DB_ENV *dbenv;
{
#define	PREPLISTSIZE	50
	DB_PREPLIST prep[PREPLISTSIZE], *p;
	DB_TXNMGR *mgr;
	DB_TXNREGION *region;
	int do_aborts, ret;
	long count, i;
	u_int32_t op;

	mgr = dbenv->tx_handle;
	region = mgr->reginfo.primary;

	do_aborts = 0;
	R_LOCK(dbenv, &mgr->reginfo);
	if (region->stat.st_nrestores != 0)
		do_aborts = 1;
	R_UNLOCK(dbenv, &mgr->reginfo);

	if (do_aborts) {
		op = DB_FIRST;
		do {
			if ((ret = __txn_recover(dbenv,
			    prep, PREPLISTSIZE, &count, op)) != 0)
				return (ret);
			for (i = 0; i < count; i++) {
				p = &prep[i];
				if ((ret = __txn_abort(p->txn)) != 0)
					return (ret);
			}
			op = DB_NEXT;
		} while (count == PREPLISTSIZE);
	}

	return (0);
}

/*
 * __rep_restore_prepared --
 *	Restore to a prepared state any prepared but not yet committed
 * transactions.
 *
 *	This performs, in effect, a "mini-recovery";  it is called from
 * __rep_start by newly upgraded masters.  There may be transactions that an
 * old master prepared but did not resolve, which we need to restore to an
 * active state.
 */
static int
__rep_restore_prepared(dbenv)
	DB_ENV *dbenv;
{
	DB_LOGC *logc;
	DB_LSN ckp_lsn, lsn;
	DBT rec;
	__txn_ckp_args *ckp_args;
	__txn_regop_args *regop_args;
	__txn_xa_regop_args *prep_args;
	int ret, t_ret;
	u_int32_t hi_txn, low_txn, rectype;
	void *txninfo;

	txninfo = NULL;
	ckp_args = NULL;
	prep_args = NULL;
	regop_args = NULL;
	ZERO_LSN(ckp_lsn);
	ZERO_LSN(lsn);

	if ((ret = __log_cursor(dbenv, &logc)) != 0)
		return (ret);

	/*
	 * We need to consider the set of records between the most recent
	 * checkpoint LSN and the end of the log;  any txn in that
	 * range, and only txns in that range, could still have been
	 * active, and thus prepared but not yet committed (PBNYC),
	 * when the old master died.
	 *
	 * Find the most recent checkpoint LSN, and get the record there.
	 * If there is no checkpoint in the log, start off by getting
	 * the very first record in the log instead.
	 */
	memset(&rec, 0, sizeof(DBT));
	if ((ret = __txn_getckp(dbenv, &lsn)) == 0) {
		if ((ret = __log_c_get(logc, &lsn, &rec, DB_SET)) != 0)  {
			__db_err(dbenv,
			    "Checkpoint record at LSN [%lu][%lu] not found",
			    (u_long)lsn.file, (u_long)lsn.offset);
			goto err;
		}

		if ((ret = __txn_ckp_read(dbenv, rec.data, &ckp_args)) != 0) {
			__db_err(dbenv,
			    "Invalid checkpoint record at [%lu][%lu]",
			    (u_long)lsn.file, (u_long)lsn.offset);
			goto err;
		}

		ckp_lsn = ckp_args->ckp_lsn;
		__os_free(dbenv, ckp_args);

		if ((ret = __log_c_get(logc, &ckp_lsn, &rec, DB_SET)) != 0) {
			__db_err(dbenv,
			    "Checkpoint LSN record [%lu][%lu] not found",
			    (u_long)ckp_lsn.file, (u_long)ckp_lsn.offset);
			goto err;
		}
	} else if ((ret = __log_c_get(logc, &lsn, &rec, DB_FIRST)) != 0) {
		if (ret == DB_NOTFOUND) {
			/* An empty log means no PBNYC txns. */
			ret = 0;
			goto done;
		}
		__db_err(dbenv, "Attempt to get first log record failed");
		goto err;
	}

	/*
	 * We use the same txnlist infrastructure that recovery does;
	 * it demands an estimate of the high and low txnids for
	 * initialization.
	 *
	 * First, the low txnid.
	 */
	do {
		/* txnid is after rectype, which is a u_int32. */
		memcpy(&low_txn,
		    (u_int8_t *)rec.data + sizeof(u_int32_t), sizeof(low_txn));
		if (low_txn != 0)
			break;
	} while ((ret = __log_c_get(logc, &lsn, &rec, DB_NEXT)) == 0);

	/* If there are no txns, there are no PBNYC txns. */
	if (ret == DB_NOTFOUND) {
		ret = 0;
		goto done;
	} else if (ret != 0)
		goto err;

	/* Now, the high txnid. */
	if ((ret = __log_c_get(logc, &lsn, &rec, DB_LAST)) != 0) {
		/*
		 * Note that DB_NOTFOUND is unacceptable here because we
		 * had to have looked at some log record to get this far.
		 */
		__db_err(dbenv, "Final log record not found");
		goto err;
	}
	do {
		/* txnid is after rectype, which is a u_int32. */
		memcpy(&hi_txn,
		    (u_int8_t *)rec.data + sizeof(u_int32_t), sizeof(hi_txn));
		if (hi_txn != 0)
			break;
	} while ((ret = __log_c_get(logc, &lsn, &rec, DB_PREV)) == 0);
	if (ret == DB_NOTFOUND) {
		ret = 0;
		goto done;
	} else if (ret != 0)
		goto err;

	/* We have a high and low txnid.  Initialise the txn list. */
	if ((ret =
	    __db_txnlist_init(dbenv, low_txn, hi_txn, NULL, &txninfo)) != 0)
		goto err;

	/*
	 * Now, walk backward from the end of the log to ckp_lsn.  Any
	 * prepares that we hit without first hitting a commit or
	 * abort belong to PBNYC txns, and we need to apply them and
	 * restore them to a prepared state.
	 *
	 * Note that we wind up applying transactions out of order.
	 * Since all PBNYC txns still held locks on the old master and
	 * were isolated, this should be safe.
	 */
	for (ret = __log_c_get(logc, &lsn, &rec, DB_LAST);
	    ret == 0 && log_compare(&lsn, &ckp_lsn) > 0;
	    ret = __log_c_get(logc, &lsn, &rec, DB_PREV)) {
		memcpy(&rectype, rec.data, sizeof(rectype));
		switch (rectype) {
		case DB___txn_regop:
			/*
			 * It's a commit or abort--but we don't care
			 * which!  Just add it to the list of txns
			 * that are resolved.
			 */
			if ((ret = __txn_regop_read(dbenv, rec.data,
			    &regop_args)) != 0)
				goto err;

			ret = __db_txnlist_find(dbenv,
			    txninfo, regop_args->txnid->txnid);
			if (ret == TXN_NOTFOUND)
				ret = __db_txnlist_add(dbenv, txninfo,
				    regop_args->txnid->txnid,
				    regop_args->opcode, &lsn);
			__os_free(dbenv, regop_args);
			break;
		case DB___txn_xa_regop:
			/*
			 * It's a prepare.  If its not aborted and
			 * we haven't put the txn on our list yet, it
			 * hasn't been resolved, so apply and restore it.
			 */
			if ((ret = __txn_xa_regop_read(dbenv, rec.data,
			    &prep_args)) != 0)
				goto err;
			ret = __db_txnlist_find(dbenv, txninfo,
			    prep_args->txnid->txnid);
			if (ret == TXN_NOTFOUND) {
				if (prep_args->opcode == TXN_ABORT)
					ret = __db_txnlist_add(dbenv, txninfo,
					    prep_args->txnid->txnid,
					    prep_args->opcode, &lsn);
				else if ((ret =
				    __rep_process_txn(dbenv, &rec)) == 0)
					ret = __txn_restore_txn(dbenv,
					    &lsn, prep_args);
			}
			__os_free(dbenv, prep_args);
			break;
		default:
			continue;
		}
	}

	/* It's not an error to have hit the beginning of the log. */
	if (ret == DB_NOTFOUND)
		ret = 0;

done:
err:	t_ret = __log_c_close(logc);

	if (txninfo != NULL)
		__db_txnlist_end(dbenv, txninfo);

	return (ret == 0 ? t_ret : ret);
}

static int
__rep_get_limit(dbenv, gbytesp, bytesp)
	DB_ENV *dbenv;
	u_int32_t *gbytesp, *bytesp;
{
	DB_REP *db_rep;
	REP *rep;

	PANIC_CHECK(dbenv);
	ENV_REQUIRES_CONFIG(dbenv, dbenv->rep_handle, "rep_get_limit",
	    DB_INIT_REP);

	if (!REP_ON(dbenv)) {
		__db_err(dbenv,
    "DB_ENV->get_rep_limit: database environment not properly initialized");
		return (__db_panic(dbenv, EINVAL));
	}
	db_rep = dbenv->rep_handle;
	rep = db_rep->region;

	if (gbytesp != NULL)
		*gbytesp = rep->gbytes;
	if (bytesp != NULL)
		*bytesp = rep->bytes;

	return (0);
}

/*
 * __rep_set_limit --
 *	Set a limit on the amount of data that will be sent during a single
 * invocation of __rep_process_message.
 */
static int
__rep_set_limit(dbenv, gbytes, bytes)
	DB_ENV *dbenv;
	u_int32_t gbytes, bytes;
{
	DB_REP *db_rep;
	REP *rep;

	PANIC_CHECK(dbenv);
	ENV_ILLEGAL_BEFORE_OPEN(dbenv, "DB_ENV->rep_set_limit");
	ENV_REQUIRES_CONFIG(dbenv, dbenv->rep_handle, "rep_set_limit",
	    DB_INIT_REP);

	if (!REP_ON(dbenv)) {
		__db_err(dbenv,
    "DB_ENV->set_rep_limit: database environment not properly initialized");
		return (__db_panic(dbenv, EINVAL));
	}
	db_rep = dbenv->rep_handle;
	rep = db_rep->region;
	MUTEX_LOCK(dbenv, db_rep->rep_mutexp);
	if (bytes > GIGABYTE) {
		gbytes += bytes / GIGABYTE;
		bytes = bytes % GIGABYTE;
	}
	rep->gbytes = gbytes;
	rep->bytes = bytes;
	MUTEX_UNLOCK(dbenv, db_rep->rep_mutexp);

	return (0);
}

/*
 * __rep_set_request --
 *	Set the minimum and maximum number of log records that we wait
 * before retransmitting.
 * UNDOCUMENTED.
 */
static int
__rep_set_request(dbenv, min, max)
	DB_ENV *dbenv;
	u_int32_t min, max;
{
	LOG *lp;
	DB_LOG *dblp;
	DB_REP *db_rep;
	REP *rep;

	PANIC_CHECK(dbenv);
	ENV_ILLEGAL_BEFORE_OPEN(dbenv, "DB_ENV->rep_set_request");
	ENV_REQUIRES_CONFIG(dbenv, dbenv->rep_handle, "rep_set_request",
	    DB_INIT_REP);

	if (!REP_ON(dbenv)) {
		__db_err(dbenv,
    "DB_ENV->set_rep_request: database environment not properly initialized");
		return (__db_panic(dbenv, EINVAL));
	}
	db_rep = dbenv->rep_handle;
	rep = db_rep->region;
	/*
	 * Note we acquire the rep_mutexp or the db_mutexp as needed.
	 */
	MUTEX_LOCK(dbenv, db_rep->rep_mutexp);
	rep->request_gap = min;
	rep->max_gap = max;
	MUTEX_UNLOCK(dbenv, db_rep->rep_mutexp);

	MUTEX_LOCK(dbenv, db_rep->db_mutexp);
	dblp = dbenv->lg_handle;
	if (dblp != NULL && (lp = dblp->reginfo.primary) != NULL) {
		lp->wait_recs = 0;
		lp->rcvd_recs = 0;
	}
	MUTEX_UNLOCK(dbenv, db_rep->db_mutexp);

	return (0);
}

/*
 * __rep_set_transport --
 *	Set the transport function for replication.
 */
static int
__rep_set_rep_transport(dbenv, eid, f_send)
	DB_ENV *dbenv;
	int eid;
	int (*f_send) __P((DB_ENV *, const DBT *, const DBT *, const DB_LSN *,
	    int, u_int32_t));
{
	PANIC_CHECK(dbenv);

	if (f_send == NULL) {
		__db_err(dbenv,
	"DB_ENV->set_rep_transport: no send function specified");
		return (EINVAL);
	}
	if (eid < 0) {
		__db_err(dbenv,
	"DB_ENV->set_rep_transport: eid must be greater than or equal to 0");
		return (EINVAL);
	}
	dbenv->rep_send = f_send;
	dbenv->rep_eid = eid;
	return (0);
}

/*
 * __rep_elect --
 *	Called after master failure to hold/participate in an election for
 *	a new master.
 */
static int
__rep_elect(dbenv, nsites, priority, timeout, eidp)
	DB_ENV *dbenv;
	int nsites, priority;
	u_int32_t timeout;
	int *eidp;
{
	DB_LOG *dblp;
	DB_LSN lsn;
	DB_REP *db_rep;
	REP *rep;
	int done, in_progress, ret, send_vote, tiebreaker;
	u_int32_t egen, orig_tally, pid, sec, usec;

	PANIC_CHECK(dbenv);
	ENV_REQUIRES_CONFIG(dbenv, dbenv->rep_handle, "rep_elect", DB_INIT_REP);

	/* Error checking. */
	if (nsites <= 0) {
		__db_err(dbenv,
		    "DB_ENV->rep_elect: nsites must be greater than 0");
		return (EINVAL);
	}
	if (priority < 0) {
		__db_err(dbenv,
		    "DB_ENV->rep_elect: priority may not be negative");
		return (EINVAL);
	}

	db_rep = dbenv->rep_handle;
	rep = db_rep->region;
	dblp = dbenv->lg_handle;

	R_LOCK(dbenv, &dblp->reginfo);
	lsn = ((LOG *)dblp->reginfo.primary)->lsn;
	R_UNLOCK(dbenv, &dblp->reginfo);

	orig_tally = 0;
	if ((ret = __rep_elect_init(dbenv,
	    &lsn, nsites, priority, &in_progress, &orig_tally)) != 0) {
		if (ret == DB_REP_NEWMASTER) {
			ret = 0;
			*eidp = dbenv->rep_eid;
		}
		goto err;
	}
	/*
	 * If another thread is in the middle of an election we
	 * just quietly return and not interfere.
	 */
	if (in_progress) {
		*eidp = dbenv->rep_eid;
		return (0);
	}
	(void)__rep_send_message(dbenv,
	    DB_EID_BROADCAST, REP_MASTER_REQ, NULL, NULL, 0);
	ret = __rep_wait(dbenv, timeout/4, eidp, REP_F_EPHASE1);
	switch (ret) {
		case 0:
			/* Check if we found a master. */
			if (*eidp != DB_EID_INVALID) {
#ifdef DIAGNOSTIC
				if (FLD_ISSET(dbenv->verbose,
				    DB_VERB_REPLICATION))
					__db_err(dbenv,
					    "Found master %d", *eidp);
#endif
				return (0);
			}
			/*
			 * If we didn't find a master, continue
			 * the election.
			 */
			break;
		case DB_TIMEOUT:
#ifdef DIAGNOSTIC
		if (FLD_ISSET(dbenv->verbose, DB_VERB_REPLICATION))
			__db_err(dbenv,
			    "Did not find master.  Sending vote1");
#endif
			break;
		default:
			goto err;
	}
	/* Generate a randomized tiebreaker value. */
restart:
	__os_id(&pid);
	if ((ret = __os_clock(dbenv, &sec, &usec)) != 0)
		return (ret);
	tiebreaker = pid ^ sec ^ usec ^ (u_int)rand() ^ P_TO_UINT32(&pid);

	MUTEX_LOCK(dbenv, db_rep->rep_mutexp);
	F_SET(rep, REP_F_EPHASE1 | REP_F_NOARCHIVE);
	F_CLR(rep, REP_F_TALLY);

	/* Tally our own vote */
	if (__rep_tally(dbenv, rep, rep->eid, &rep->sites, rep->egen,
	    rep->tally_off) != 0)
		goto lockdone;
	__rep_cmp_vote(dbenv, rep, &rep->eid, &lsn, priority, rep->gen,
	    tiebreaker);

#ifdef DIAGNOSTIC
	if (FLD_ISSET(dbenv->verbose, DB_VERB_REPLICATION))
		__db_err(dbenv, "Beginning an election");
#endif

	/* Now send vote */
	send_vote = DB_EID_INVALID;
	egen = rep->egen;
	MUTEX_UNLOCK(dbenv, db_rep->rep_mutexp);
	__rep_send_vote(dbenv, &lsn, nsites, priority, tiebreaker, egen,
	    DB_EID_BROADCAST, REP_VOTE1);
	ret = __rep_wait(dbenv, timeout, eidp, REP_F_EPHASE1);
	switch (ret) {
		case 0:
			/* Check if election complete or phase complete. */
			if (*eidp != DB_EID_INVALID) {
#ifdef DIAGNOSTIC
				if (FLD_ISSET(dbenv->verbose,
				    DB_VERB_REPLICATION))
					__db_err(dbenv,
					    "Ended election phase 1 %d", ret);
#endif
				return (0);
			}
			goto phase2;
		case DB_TIMEOUT:
			break;
		default:
			goto err;
	}
	/*
	 * If we got here, we haven't heard from everyone, but we've
	 * run out of time, so it's time to decide if we have enough
	 * votes to pick a winner and if so, to send out a vote to
	 * the winner.
	 */
	MUTEX_LOCK(dbenv, db_rep->rep_mutexp);
	/*
	 * If our egen changed while we were waiting.  We need to
	 * essentially reinitialize our election.
	 */
	if (egen != rep->egen) {
		MUTEX_UNLOCK(dbenv, db_rep->rep_mutexp);
#ifdef DIAGNOSTIC
		if (FLD_ISSET(dbenv->verbose, DB_VERB_REPLICATION))
			__db_err(dbenv, "Egen changed from %lu to %lu",
			    (u_long)egen, (u_long)rep->egen);
#endif
		goto restart;
	}
	if (rep->sites > rep->nsites / 2) {

		/* We think we've seen enough to cast a vote. */
		send_vote = rep->winner;
		/*
		 * See if we won.  This will make sure we
		 * don't count ourselves twice if we're racing
		 * with incoming votes.
		 */
		if (rep->winner == rep->eid) {
			(void)__rep_tally(dbenv, rep, rep->eid, &rep->votes,
			    egen, rep->v2tally_off);
#ifdef DIAGNOSTIC
			if (FLD_ISSET(dbenv->verbose, DB_VERB_REPLICATION))
				__db_err(dbenv,
				    "Counted my vote %d", rep->votes);
#endif
		}
		F_SET(rep, REP_F_EPHASE2);
		F_CLR(rep, REP_F_EPHASE1);
	}
	MUTEX_UNLOCK(dbenv, db_rep->rep_mutexp);
	if (send_vote == DB_EID_INVALID) {
		/* We do not have enough votes to elect. */
#ifdef DIAGNOSTIC
		if (FLD_ISSET(dbenv->verbose, DB_VERB_REPLICATION))
			__db_err(dbenv,
			    "Not enough votes to elect: received %d of %d",
			    rep->sites, rep->nsites);
#endif
		ret = DB_REP_UNAVAIL;
		goto err;

	} else {
		/*
		 * We have seen enough vote1's.  Now we need to wait
		 * for all the vote2's.
		 */
		if (send_vote != rep->eid) {
#ifdef DIAGNOSTIC
			if (FLD_ISSET(dbenv->verbose, DB_VERB_REPLICATION) &&
			    send_vote != rep->eid)
				__db_err(dbenv, "Sending vote");
#endif
			__rep_send_vote(dbenv, NULL, 0, 0, 0, egen,
			    send_vote, REP_VOTE2);

		}
phase2:
		ret = __rep_wait(dbenv, timeout, eidp, REP_F_EPHASE2);
#ifdef DIAGNOSTIC
				if (FLD_ISSET(dbenv->verbose,
				    DB_VERB_REPLICATION))
					__db_err(dbenv,
					    "Ended election phase 2 %d", ret);
#endif
		switch (ret) {
			case 0:
				return (0);
			case DB_TIMEOUT:
				ret = DB_REP_UNAVAIL;
				break;
			default:
				goto err;
		}
		MUTEX_LOCK(dbenv, db_rep->rep_mutexp);
		done = rep->votes > rep->nsites / 2;
#ifdef DIAGNOSTIC
		if (FLD_ISSET(dbenv->verbose, DB_VERB_REPLICATION))
			__db_err(dbenv,
			    "After phase 2: done %d, votes %d, nsites %d",
			    done, rep->votes, rep->nsites);
#endif
		if (send_vote == rep->eid && done) {
			__rep_elect_master(dbenv, rep, eidp);
			ret = 0;
			goto lockdone;
		}
		MUTEX_UNLOCK(dbenv, db_rep->rep_mutexp);
	}

err:	MUTEX_LOCK(dbenv, db_rep->rep_mutexp);
lockdone:
	/*
	 * If we get here because of a non-election error, then we
	 * did not tally our vote.  The only non-election error is
	 * from elect_init where we were unable to grow_sites.  In
	 * that case we do not want to discard all known election info.
	 */
	if (ret == 0 || ret == DB_REP_UNAVAIL)
		__rep_elect_done(dbenv, rep);
	else if (orig_tally)
		F_SET(rep, orig_tally);

#ifdef DIAGNOSTIC
	if (FLD_ISSET(dbenv->verbose, DB_VERB_REPLICATION))
		__db_err(dbenv,
		    "Ended election with %d, sites %d, egen %lu, flags 0x%lx",
		    ret, rep->sites, (u_long)rep->egen, (u_long)rep->flags);
#endif
	MUTEX_UNLOCK(dbenv, db_rep->rep_mutexp);
	return (ret);
}

/*
 * __rep_elect_init
 *	Initialize an election.  Sets beginp non-zero if the election is
 * already in progress; makes it 0 otherwise.
 */
static int
__rep_elect_init(dbenv, lsnp, nsites, priority, beginp, otally)
	DB_ENV *dbenv;
	DB_LSN *lsnp;
	int nsites, priority;
	int *beginp;
	u_int32_t *otally;
{
	DB_REP *db_rep;
	REP *rep;
	int ret;

	db_rep = dbenv->rep_handle;
	rep = db_rep->region;

	ret = 0;

	/* We may miscount, as we don't hold the replication mutex here. */
	rep->stat.st_elections++;

	/* If we are already a master; simply broadcast that fact and return. */
	if (F_ISSET(rep, REP_F_MASTER)) {
		(void)__rep_send_message(dbenv,
		    DB_EID_BROADCAST, REP_NEWMASTER, lsnp, NULL, 0);
		rep->stat.st_elections_won++;
		return (DB_REP_NEWMASTER);
	}

	MUTEX_LOCK(dbenv, db_rep->rep_mutexp);
	if (otally != NULL)
		*otally = F_ISSET(rep, REP_F_TALLY);
	*beginp = IN_ELECTION(rep);
	if (!*beginp) {
		/*
		 * Make sure that we always initialize all the election fields
		 * before putting ourselves in an election state.  That means
		 * issuing calls that can fail (allocation) before setting all
		 * the variables.
		 */
		if (nsites > rep->asites &&
		    (ret = __rep_grow_sites(dbenv, nsites)) != 0)
			goto err;
		DB_ENV_TEST_RECOVERY(dbenv, DB_TEST_ELECTINIT, ret, NULL);
		rep->nsites = nsites;
		rep->priority = priority;
		rep->master_id = DB_EID_INVALID;
	}
DB_TEST_RECOVERY_LABEL
err:	MUTEX_UNLOCK(dbenv, db_rep->rep_mutexp);
	return (ret);
}

/*
 * __rep_elect_master
 *	Set up for new master from election.  Must be called with
 *	the db_rep->rep_mutex held.
 *
 * PUBLIC: void __rep_elect_master __P((DB_ENV *, REP *, int *));
 */
void
__rep_elect_master(dbenv, rep, eidp)
	DB_ENV *dbenv;
	REP *rep;
	int *eidp;
{
	rep->master_id = rep->eid;
	F_SET(rep, REP_F_MASTERELECT);
	if (eidp != NULL)
		*eidp = rep->master_id;
	rep->stat.st_elections_won++;
#ifdef DIAGNOSTIC
	if (FLD_ISSET(dbenv->verbose, DB_VERB_REPLICATION))
		__db_err(dbenv,
    "Got enough votes to win; election done; winner is %d, gen %lu",
		    rep->master_id, (u_long)rep->gen);
#else
	COMPQUIET(dbenv, NULL);
#endif
}

static int
__rep_wait(dbenv, timeout, eidp, flags)
	DB_ENV *dbenv;
	u_int32_t timeout;
	int *eidp;
	u_int32_t flags;
{
	DB_REP *db_rep;
	REP *rep;
	int done;
	u_int32_t sleeptime;

	done = 0;
	db_rep = dbenv->rep_handle;
	rep = db_rep->region;

	/*
	 * The user specifies an overall timeout function, but checking
	 * is cheap and the timeout may be a generous upper bound.
	 * Sleep repeatedly for the smaller of .5s and timeout/10.
	 */
	sleeptime = (timeout > 5000000) ? 500000 : timeout / 10;
	if (sleeptime == 0)
		sleeptime++;
	while (timeout > 0) {
		(void)__os_sleep(dbenv, 0, sleeptime);
		MUTEX_LOCK(dbenv, db_rep->rep_mutexp);
		done = !F_ISSET(rep, flags) && rep->master_id != DB_EID_INVALID;

		*eidp = rep->master_id;
		MUTEX_UNLOCK(dbenv, db_rep->rep_mutexp);

		if (done)
			return (0);

		if (timeout > sleeptime)
			timeout -= sleeptime;
		else
			timeout = 0;
	}
	return (DB_TIMEOUT);
}

/*
 * __rep_flush --
 *	Re-push the last log record to all clients, in case they've lost
 * messages and don't know it.
 */
static int
__rep_flush(dbenv)
	DB_ENV *dbenv;
{
	DBT rec;
	DB_LOGC *logc;
	DB_LSN lsn;
	int ret, t_ret;

	PANIC_CHECK(dbenv);
	ENV_REQUIRES_CONFIG(dbenv, dbenv->rep_handle, "rep_flush", DB_INIT_REP);

	if ((ret = __log_cursor(dbenv, &logc)) != 0)
		return (ret);

	memset(&rec, 0, sizeof(rec));
	memset(&lsn, 0, sizeof(lsn));

	if ((ret = __log_c_get(logc, &lsn, &rec, DB_LAST)) != 0)
		goto err;

	(void)__rep_send_message(dbenv,
	    DB_EID_BROADCAST, REP_LOG, &lsn, &rec, 0);

err:	if ((t_ret = __log_c_close(logc)) != 0 && ret == 0)
		ret = t_ret;
	return (ret);
}

/*
 * __rep_stat --
 *	Fetch replication statistics.
 */
static int
__rep_stat(dbenv, statp, flags)
	DB_ENV *dbenv;
	DB_REP_STAT **statp;
	u_int32_t flags;
{
	DB_LOG *dblp;
	DB_REP *db_rep;
	DB_REP_STAT *stats;
	LOG *lp;
	REP *rep;
	u_int32_t queued, repflags;
	int dolock, ret;

	PANIC_CHECK(dbenv);
	ENV_REQUIRES_CONFIG(dbenv, dbenv->rep_handle, "rep_stat", DB_INIT_REP);

	db_rep = dbenv->rep_handle;
	rep = db_rep->region;
	dblp = dbenv->lg_handle;
	lp = dblp->reginfo.primary;

	*statp = NULL;
	if ((ret = __db_fchk(dbenv,
	    "DB_ENV->rep_stat", flags, DB_STAT_CLEAR)) != 0)
		return (ret);

	/* Allocate a stat struct to return to the user. */
	if ((ret = __os_umalloc(dbenv, sizeof(DB_REP_STAT), &stats)) != 0)
		return (ret);

	/*
	 * Read without holding the lock.  If we are in client
	 * recovery, we copy just the stats struct so we won't
	 * block.  We only copy out those stats that don't
	 * require acquiring any mutex.
	 */
	repflags = rep->flags;
	if (FLD_ISSET(repflags, REP_F_RECOVER))
		dolock = 0;
	else {
		dolock = 1;
		MUTEX_LOCK(dbenv, db_rep->rep_mutexp);
	}
	memcpy(stats, &rep->stat, sizeof(*stats));

	/* Copy out election stats. */
	if (IN_ELECTION_TALLY(rep)) {
		if (F_ISSET(rep, REP_F_EPHASE1))
			stats->st_election_status = 1;
		else if (F_ISSET(rep, REP_F_EPHASE2))
			stats->st_election_status = 2;

		stats->st_election_nsites = rep->sites;
		stats->st_election_cur_winner = rep->winner;
		stats->st_election_priority = rep->w_priority;
		stats->st_election_gen = rep->w_gen;
		stats->st_election_lsn = rep->w_lsn;
		stats->st_election_votes = rep->votes;
		stats->st_election_tiebreaker = rep->w_tiebreaker;
	}

	/* Copy out other info that's protected by the rep mutex. */
	stats->st_env_id = rep->eid;
	stats->st_env_priority = rep->priority;
	stats->st_nsites = rep->nsites;
	stats->st_master = rep->master_id;
	stats->st_gen = rep->gen;

	if (F_ISSET(rep, REP_F_MASTER))
		stats->st_status = DB_REP_MASTER;
	else if (F_ISSET(rep, REP_F_LOGSONLY))
		stats->st_status = DB_REP_LOGSONLY;
	else if (F_ISSET(rep, REP_F_UPGRADE))
		stats->st_status = DB_REP_CLIENT;
	else
		stats->st_status = 0;

	if (LF_ISSET(DB_STAT_CLEAR)) {
		queued = rep->stat.st_log_queued;
		memset(&rep->stat, 0, sizeof(rep->stat));
		rep->stat.st_log_queued = rep->stat.st_log_queued_total =
		    rep->stat.st_log_queued_max = queued;
	}

	if (dolock) {
		stats->st_in_recovery = 0;
		MUTEX_UNLOCK(dbenv, db_rep->rep_mutexp);
		MUTEX_LOCK(dbenv, db_rep->db_mutexp);
	} else
		stats->st_in_recovery = 1;

	/*
	 * Log-related replication info is stored in the log system and
	 * protected by the log region lock.
	 */
	if (F_ISSET(rep, REP_ISCLIENT)) {
		stats->st_next_lsn = lp->ready_lsn;
		stats->st_waiting_lsn = lp->waiting_lsn;
	} else {
		if (F_ISSET(rep, REP_F_MASTER))
			stats->st_next_lsn = lp->lsn;
		else
			ZERO_LSN(stats->st_next_lsn);
		ZERO_LSN(stats->st_waiting_lsn);
	}
	if (dolock)
		MUTEX_UNLOCK(dbenv, db_rep->db_mutexp);

	*statp = stats;
	return (0);
}
