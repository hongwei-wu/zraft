#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#include "assert.h"
#include "configuration.h"
#include "convert.h"
#include "entry.h"
#ifdef __GLIBC__
	#include "error.h"
#endif
#include "err.h"
#include "heap.h"
#include "log.h"
#include "membership.h"
#include "progress.h"
#include "queue.h"
#include "replication.h"
#include "request.h"
#include "snapshot.h"
#include "tracing.h"


#define UNUSED(v)	((void)v)

/* Set to 1 to enable tracing. */
#if 0
	#define tracef(...) Tracef(r->tracer, __VA_ARGS__)
#else
	#define tracef(...)
#endif


//#include<unistd.h>
//#include <fcntl.h>
//static void exception_exit_test(struct raft *r, uint32_t pgkey)
//{
//	if (pgkey != rkey(r))
//		return;
//
//	char *flag_file = "exception_exit_test";
//
//	if (access(flag_file, F_OK) == 0) {
//		return;
//	}
//
//	ZSWARNING(gzlog, "[raft][%d][%d][%s].", rkey(r), r->state, __func__);
//	creat(flag_file, S_IFREG);
//	exit(0);
//}
//
//static void exception_exit_test2(struct raft *r, uint32_t pgkey)
//{
//	if (pgkey != rkey(r))
//		return;
//
//	char *flag_file = "exception_exit_test2";
//
//	if (access(flag_file, F_OK) == 0) {
//		return;
//	}
//
//	ZSWARNING(gzlog, "[raft][%d][%d][%s].", rkey(r), r->state, __func__);
//	creat(flag_file, S_IFREG);
//	exit(0);
//}

int64_t raft_tick_count_ns(void)
{
	struct timespec time;

	clock_gettime(CLOCK_MONOTONIC, &time);
	return (int64_t)(time.tv_sec * 1000000000 + time.tv_nsec);
}

/* Context of a RAFT_IO_APPEND_ENTRIES request that was submitted with
 * raft_io_>send(). */
struct sendAppendEntries {
	struct raft *raft;          /* Instance sending the entries. */
	struct raft_io_send send;   /* Underlying I/O send request. */
	raft_index index;           /* Index of the first entry in the request. */
	struct raft_entry *entries; /* Entries referenced in the request. */
	unsigned n;                 /* Length of the entries array. */
	raft_id server_id;          /* Destination server. */
};

/* Callback invoked after request to send an AppendEntries RPC has completed. */
static void sendAppendEntriesCb(struct raft_io_send *send, const int status)
{
	struct sendAppendEntries *req = send->data;
	struct raft *r = req->raft;
	unsigned i = configurationIndexOf(&r->configuration, req->server_id);

	if (r->state == RAFT_LEADER && i < r->configuration.n) {
		if (status != 0) {
			tracef("failed to send append entries to server %u: %s",
				   req->server_id, raft_strerror(status));
			/* Go back to probe mode. */
			progressToProbe(r, i);
		}
	}

	/* Tell the log that we're done referencing these entries. */
	logRelease(&r->log, req->index, req->entries, req->n);
	raft_free(req);
}

/* Send an AppendEntries message to the i'th server, including all log entries
 * from the given point onwards. */
static int sendAppendEntries(struct raft *r,
							 const unsigned i,
							 const raft_index prev_index,
							 const raft_term prev_term,
							 struct pgrep_permit_info pi)
{
	struct raft_server *server = &r->configuration.servers[i];
	struct raft_message message;
	struct raft_append_entries *args = &message.append_entries;
	struct sendAppendEntries *req;
	raft_index next_index = prev_index + 1;
	raft_index optimistic_next_index;
	int rv;

	args->pkt = rand();
	args->term = r->current_term;
	args->prev_log_index = prev_index;
	args->prev_log_term = prev_term;
	args->src_server = r->id;
	args->pi = pi;


	/* Pgrep:
	 *
	 *  If in pgrep progress, obtain a section of log entries and send to the catch-up
	 *  followers.
	 */
	if (args->pi.permit) {
		assert(pi.replicating == PGREP_RND_BGN ||
			   pi.replicating == PGREP_RND_ING);
		/* The first pgrep message, to send 0 entries for argree the index only. */
		if (pi.replicating == PGREP_RND_BGN) {
			rv = 0;
			args->n_entries = 0;
			args->entries = NULL;
		} else {
			/* Send the section log entries bettwen prev_applied_index and last_applied_index. */
			rv = logAcquireSection(&r->log, next_index, r->last_applied, &args->entries, &args->n_entries);
		}
		if (rv != 0) {
			ZSERROR(gzlog, "[raft][%d][%d][%s]: logAcquireSection failed rv[%d].",
					rkey(r), r->state, __func__, rv);
			goto err;
		}
		/* To updating the permit. */
		r->io->pgrep_raft_permit(r->io, &args->pi);
		if (!pi.permit) {
			rv = -1;
			goto err_after_entries_acquired;
		}
	} else {
		rv = logAcquireSection(&r->log, next_index, logLastIndex(&r->log), &args->entries, &args->n_entries);
		if (rv != 0) {
			goto err;
		}
		args->pi.time = ++r->io->io_tick;
		ZSINFO(gzlog, "[raft][%d][%d][%s]: initial pi.time[%ld].",
			   rkey(r), r->state, __func__, args->pi.time);
	}

	/* From Section 3.5:
	 *
	 *   The leader keeps track of the highest index it knows to be committed,
	 *   and it includes that index in future AppendEntries RPCs (including
	 *   heartbeats) so that the other servers eventually find out. Once a
	 *   follower learns that a log entry is committed, it applies the entry to
	 *   its local state machine (in log order)
	 */
	args->leader_commit = r->commit_index;

	ZSINFO(gzlog, "[raft][%d][%d][pkt:%d][%s]: "
		   "send %u entries starting at %llu to server "
		   "%llu (last index %llu, last applied %llu)",
		   rkey(r), r->state, args->pkt, __func__,
		   args->n_entries, args->prev_log_index,
		   server->id, logLastIndex(&r->log), r->last_applied);

	const struct raft_server *me = configurationGet(&r->configuration, r->id);
	int role = -1;
	int pre_role = -1;
	if (me) {
		role = me->role;
		pre_role = me->pre_role;
	}

	ZSINFO(gzlog,
		   "dumpstatus:###"
		   "{ \"time\":%ld, "
		   "  \"raft\":%d, "
		   "  \"id\":%lld,"
		   "  \"state\":%d, "
		   "  \"role\":%d, "
		   "  \"pre_role\":%d, "
		   "  \"last_index\":%llu, "
		   "  \"last_applying\":%llu, "
		   "  \"last_applied\":%llu, "
		   "  \"last_stored\":%llu, "
		   "  \"commit_index\":%llu, "
		   "  \"configuration_uncommitted_index\":%lld, "
		   "  \"promotee_id\":%lld "
		   "}###",
		   raft_tick_count_ns(),
		   rkey(r),
		   r->id,
		   r->state,
		   role,
		   pre_role,
		   logLastIndex(&r->log),
		   r->last_applying,
		   r->last_applied,
		   r->last_stored,
		   r->commit_index,
		   r->configuration_uncommitted_index,
		   r->leader_state.promotee_id);

	message.type = RAFT_IO_APPEND_ENTRIES;
	message.server_id = server->id;
	message.server_address = server->address;

	req = raft_malloc(sizeof*req);
	if (req == NULL) {
		rv = RAFT_NOMEM;
		goto err_after_entries_acquired;
	}
	req->raft = r;
	req->index = args->prev_log_index + 1;
	req->entries = args->entries;
	req->n = args->n_entries;
	req->server_id = server->id;

	req->send.data = req;
	optimistic_next_index = req->index + req->n;
	rv = r->io->send(r->io, &req->send, &message, sendAppendEntriesCb);
	if (rv != 0) {
		goto err_after_req_alloc;
	}

	if (progressState(r, i) == PROGRESS__PIPELINE) {
		/* Optimistically update progress. */
		progressOptimisticNextIndex(r, i, optimistic_next_index);
	}

	progressUpdateLastSend(r, i);
	return 0;

err_after_req_alloc:
	raft_free(req);
err_after_entries_acquired:
	logRelease(&r->log, next_index, args->entries, args->n_entries);
err:
	assert(rv != 0);
	return rv;
}

/* Context of a RAFT_IO_INSTALL_SNAPSHOT request that was submitted with
 * raft_io_>send(). */
struct sendInstallSnapshot {
	struct raft *raft;               /* Instance sending the snapshot. */
	struct raft_io_snapshot_get get; /* Snapshot get request. */
	struct raft_io_send send;        /* Underlying I/O send request. */
	struct raft_snapshot *snapshot;  /* Snapshot to send. */
	raft_id server_id;               /* Destination server. */
};

static void sendInstallSnapshotCb(struct raft_io_send *send, int status)
{
	struct sendInstallSnapshot *req = send->data;
	struct raft *r = req->raft;
	const struct raft_server *server;

	server = configurationGet(&r->configuration, req->server_id);

	if (status != 0) {
		tracef("send install snapshot: %s", raft_strerror(status));
		if (r->state == RAFT_LEADER && server != NULL) {
			unsigned i;
			i = configurationIndexOf(&r->configuration, req->server_id);
			progressAbortSnapshot(r, i);
		}
	}

	snapshotClose(req->snapshot);
	raft_free(req->snapshot);
	raft_free(req);
}

static void sendSnapshotGetCb(struct raft_io_snapshot_get *get,
							  struct raft_snapshot *snapshot,
							  int status)
{
	struct sendInstallSnapshot *req = get->data;
	struct raft *r = req->raft;
	struct raft_message message;
	struct raft_install_snapshot *args = &message.install_snapshot;
	const struct raft_server *server = NULL;
	bool progress_state_is_snapshot = false;
	unsigned i = 0;
	int rv;

	if (status != 0) {
		tracef("get snapshot %s", raft_strerror(status));
		goto abort;
	}
	if (r->state != RAFT_LEADER) {
		goto abort_with_snapshot;
	}

	server = configurationGet(&r->configuration, req->server_id);

	if (server == NULL) {
		/* Probably the server was removed in the meantime. */
		goto abort_with_snapshot;
	}

	i = configurationIndexOf(&r->configuration, req->server_id);
	progress_state_is_snapshot = progressState(r, i) == PROGRESS__SNAPSHOT;

	if (!progress_state_is_snapshot) {
		/* Something happened in the meantime. */
		goto abort_with_snapshot;
	}

	assert(snapshot->n_bufs == 1);

	message.type = RAFT_IO_INSTALL_SNAPSHOT;
	message.server_id = server->id;
	message.server_address = server->address;

	args->term = r->current_term;
	args->last_index = snapshot->index;
	args->last_term = snapshot->term;
	args->conf_index = snapshot->configuration_index;
	args->conf = snapshot->configuration;
	args->data = snapshot->bufs[0];

	req->snapshot = snapshot;
	req->send.data = req;

	tracef("sending snapshot with last index %llu to %u", snapshot->index,
		   server->id);

	rv = r->io->send(r->io, &req->send, &message, sendInstallSnapshotCb);
	if (rv != 0) {
		goto abort_with_snapshot;
	}

	goto out;

abort_with_snapshot:
	snapshotClose(snapshot);
	raft_free(snapshot);
abort:
	if (r->state == RAFT_LEADER && server != NULL &&
		progress_state_is_snapshot) {
		progressAbortSnapshot(r, i);
	}
	raft_free(req);
out:
	return;
}

/* Send the latest snapshot to the i'th server */
static int sendSnapshot(struct raft *r, const unsigned i)
{
	struct raft_server *server = &r->configuration.servers[i];
	struct sendInstallSnapshot *request;
	int rv;

	progressToSnapshot(r, i);

	request = raft_malloc(sizeof*request);
	if (request == NULL) {
		rv = RAFT_NOMEM;
		goto err;
	}
	request->raft = r;
	request->server_id = server->id;
	request->get.data = request;

	/* TODO: make sure that the I/O implementation really returns the latest
	 * snapshot *at this time* and not any snapshot that might be stored at a
	 * later point. Otherwise the progress snapshot_index would be wrong. */
	rv = r->io->snapshot_get(r->io, &request->get, sendSnapshotGetCb);
	if (rv != 0) {
		goto err_after_req_alloc;
	}

	progressUpdateLastSend(r, i);
	return 0;

err_after_req_alloc:
	raft_free(request);
err:
	progressAbortSnapshot(r, i);
	assert(rv != 0);
	return rv;
}


struct assign_result {
	struct raft *r;
	struct raft_server *server;
};

static void assignRoleCb(struct raft_change *req, int status)
{
	(void)status;
	struct assign_result *_result = req->data;
	struct raft_server *server = _result->server;
	struct raft *r = _result->r;

	ZSINFO(gzlog, "[raft][%d][%d][%s]: server[%lld] role:[%d] return.",
		   rkey(r), r->state, __func__, server->id, server->role);

	if (server->role == RAFT_VOTER)
		r->leader_state.promotee_id = 0;

	server->pre_role = RAFT_UNKNOW;
	raft_free(_result);
	raft_free(req);

	/* Notify the upper module the role changed. */
	if (r->role_change_cb) {
		r->role_change_cb(r, server);
		ZSINFO(gzlog, "[raft][%d][%d][%s][role_notify] role[%d].",
			   rkey(r), r->state, __func__, server->role);
	}
}

static void assignRole(struct raft *r, struct raft_server *server, int role)
{
	struct raft_change *_req;
	struct assign_result *_result;
	int _rv;

	if (server->pre_role == role) {
		return;
	}

	_req = raft_malloc(sizeof(struct raft_change));
	if (!_req)
		return;

	_result = raft_malloc(sizeof(struct assign_result));
	if (!_result) {
		raft_free(_result);
		return;
	}

	server->pre_role = role;
	_result->server = server;
	_result->r = r;
	_req->data = _result;
	_req->cb = assignRoleCb;

	ZSINFO(gzlog, "[raft][%d][%d][%s]: server[%lld] role:[%d] .",
		   rkey(r), r->state, __func__, server->id, role);

	_rv = raft_assign(r, _req, server->id, role, assignRoleCb);
	if (_rv) {
		ZSINFO(gzlog, "[raft][%d][%d][%s]: server[%lld] role:[%d] failed[%d].",
			   rkey(r), r->state, __func__, server->id, role, _rv);
		server->pre_role = RAFT_UNKNOW;
		raft_free(_result);
		raft_free(_req);
	}
}

/* Pgrep:
 *
 *   Tick pgrep, and deal with various conidtion, send section log entries
 *   or just send heart beat.
 */
int sendPgrepTickMessage(struct raft *r, unsigned i, struct pgrep_permit_info pi)
{
	struct raft_server *server = &r->configuration.servers[i];
	struct raft_progress *p = &r->leader_state.progress[i];
	bool sendSectionLogs = false;
	int status = 0;
	uint16_t rep_state = PGREP_RND_ING;


	ZSINFO(gzlog, "[raft][%d][%d][%s]: server i[%d] permit[%d].",
		   rkey(r), r->state, __func__, i, pi.permit);

	unsigned inx = configurationIndexOf(&r->configuration, r->pgrep_id);

	if (server->role != RAFT_STANDBY ||
		server->pre_role == RAFT_STANDBY ||
		i != inx) {
		ZSINFO(gzlog, "[raft][%d][%d][%s]: role[%d] pre_role[%d] pgrep_id[%lld] goto heatbeat.",
			   rkey(r), r->state, __func__, server->role,  server->pre_role, r->pgrep_id);
		goto __heart_beat;
	}

	if (r->configuration_uncommitted_index) {
		ZSINFO(gzlog, "[raft][%d][%d][%s]: cui[%lld] goto heatbeat.",
			   rkey(r), r->state, __func__, r->configuration_uncommitted_index);
		goto __heart_beat;
	}

	/* Send tick message, and handle all status. */
	status = r->io->pgrep_tick(r->io, r->id, server->id, r->current_term, &pi);

	switch (status) {
	case PGREP_TICK_SUC:
		/* Update pgrep prev_applied_index to cur_applied_index */
		progressUpdateAppliedIndex(r, i, r->last_applied);
		rep_state = PGREP_RND_BGN;
		sendSectionLogs = true;
		break;
	case PGREP_TICK_RUN:
		/* There are some entries need to sending. */
		//sendSectionLogs = r->last_applied > progressGetAppliedIndex(r, i);
		sendSectionLogs = true;
		break;
	case PGREP_TICK_FIN:
	case PGREP_TICK_ABD:
	case PGREP_TICK_DLT:
		progressOptimisticNextIndex(r, i, progressGetAppliedIndex(r, i) + 1);
		progressSetPgreplicating(r, i, false);
		progressUpdateAppliedIndex(r, i, 0);
		ZSINFO(gzlog, "[raft][%d][%d][%s]: pgrep over status[%d] pgrep_id[%lld] replicating[%d].",
			   rkey(r), r->state, __func__, status, r->pgrep_id, p->replicating);
		if (status == PGREP_TICK_FIN) {
			if (pi.permit) {
				r->io->pgrep_raft_unpermit(r->io, &pi);
				pi.permit = false;
			}
			r->leader_state.promotee_id = 0;
			assignRole(r, server, RAFT_VOTER);
			/* Start a replication right now to preventing catch-up again. */
			replicationProgress(r, i);
			return 0;
		}
		goto __heart_beat;
		break;
	case PGREP_TICK_FAL:
		/* May exceed the maximum limit, just send heartbeat. */
		goto __heart_beat;
		break;
	default:
		assert(0);
	}

	if (!pi.permit) {
		/* To ask pgerp permission. */
		r->io->pgrep_raft_permit(r->io, &pi);
		if (!pi.permit) {
			ZSINFO(gzlog, "[raft][%d][%d][%s]: pgrep permit not granted.",
				   rkey(r), r->state, __func__);
			goto __heart_beat;
		}

		ZSINFO(gzlog, "[raft][%d][%d][%s]: pgrep permit granted.",
			   rkey(r), r->state, __func__);
	}

	pi.replicating = rep_state;

	ZSINFO(gzlog, "[raft][%d][%d][%s]: tick status[%d] replicating[%d] "
				  "prev_applied_index[%lld] last_applied[%lld] sendSectionLogs[%d]. ",
		   rkey(r), r->state, __func__, status, pi.replicating, p->prev_applied_index,
		   r->last_applied, sendSectionLogs);

	if (sendSectionLogs) {
		//exception_exit_test2(r, (uint32_t)(rand() % 8));
		/* Send (prev, cur] log entries. */
		raft_index prev_index = p->prev_applied_index;
		raft_term prev_term = logTermOf(&r->log, prev_index);

		ZSINFO(gzlog, "[raft][%d][%d][%s]: prev_index[%lld] prev_term[%lld] l->offset[%lld] "
			   "l->snapshot.last_index[%lld].",
			   rkey(r), r->state, __func__, prev_index, prev_term, r->log.offset,
			   r->log.snapshot.last_index);

		return sendAppendEntries(r, i, prev_index, prev_term, pi);
	}

__heart_beat:
	if (pi.permit) {
		/* If permit granted, and there has no log entries, release the permit. */
		r->io->pgrep_raft_unpermit(r->io, &pi);
		pi.permit = false;

		ZSINFO(gzlog, "[raft][%d][%d][%s]: pgrep permit released because just heart beat.",
			   rkey(r), r->state, __func__);
	}

	/* Just send empty heartbeat.*/
	raft_index prev_index = logLastIndex(&r->log);
	raft_term prev_term = logLastTerm(&r->log);
	pi.replicating = PGREP_RND_HRT;

	ZSINFO(gzlog, "[raft][%d][%d][%s]: tick status[%d] just heart beat .",
		   rkey(r), r->state, __func__, status);

	return sendAppendEntries(r, i, prev_index, prev_term, pi);
}

/* Pgrep:
 *
 *   1. Called from replicationApply which checked server i is
 *   in pg replicating.
 *   2. If pg is replicating status, goto pgrep and send pgrep
 *   tick message.
 *   3. If server role is standby and update replicating status
 *   successful, goto pgrep and send pgrep tick message.
 */
static bool enterPgrepicating(struct raft *r, unsigned i, struct pgrep_permit_info pi)
{
	struct raft_server *server = &r->configuration.servers[i];

	if (pi.permit) {
		assert(progressPgreplicating(r, i));
		ZSINFO(gzlog, "[raft][%d][%d][%s]: permit[1] goto pgrep.",
			   rkey(r), r->state, __func__);
		return true;
	}

	if (progressPgreplicating(r, i)) {
		assert(server->role == RAFT_STANDBY);
		ZSINFO(gzlog, "[raft][%d][%d][%s]: server i[%d] already "
					  "in pgreplicating state permit[%d].",
			   rkey(r), r->state, __func__, i, pi.permit);
		return true;
	}

	if (server->pre_role != RAFT_VOTER &&
		(server->role == RAFT_STANDBY || server->pre_role == RAFT_STANDBY)) {
		if (server->role == RAFT_STANDBY && server->pre_role != RAFT_STANDBY)
			progressSetPgreplicating(r, i, true);
		ZSINFO(gzlog, "[raft][%d][%d][%s]: i[%d] pgrep_id[%lld].",
			   rkey(r), r->state, __func__, i, r->pgrep_id);
		return true;
	}

	return false;
}

int replicationProgress(struct raft *r, unsigned i)
{
	struct pgrep_permit_info pi = {0};
	return replicationProgressInner(r, i, pi);
}

int replicationProgressPi(struct raft *r, unsigned i, struct pgrep_permit_info pi)
{
	return replicationProgressInner(r, i, pi);
}

int replicationProgressInner(struct raft *r, unsigned i, struct pgrep_permit_info pi)
{
	struct raft_server *server = &r->configuration.servers[i];
	raft_index snapshot_index = logSnapshotIndex(&r->log);
	raft_index next_index = progressNextIndex(r, i);
	raft_index prev_index;
	raft_term prev_term;


	assert(r->state == RAFT_LEADER);
	assert(server->id != r->id);
	assert(next_index >= 1);

	if (!pi.permit && !progressShouldReplicate(r, i)) {
		ZSINFO(gzlog, "[raft][%d][%d][%s]: progressShouldReplicate false.",
			   rkey(r), r->state, __func__);
		return 0;
	}

	/* pgrep: check if need pgrep ticking. */
	if (enterPgrepicating(r, i, pi))
		goto pgrep;


	/* From Section 3.5:
	 *
	 *   When sending an AppendEntries RPC, the leader includes the index and
	 *   term of the entry in its log that immediately precedes the new
	 *   entries. If the follower does not find an entry in its log with the
	 *   same index and term, then it refuses the new entries. The consistency
	 *   check acts as an induction step: the initial empty state of the logs
	 *   satisfies the Log Matching Property, and the consistency check
	 *   preserves the Log Matching Property whenever logs are extended. As a
	 *   result, whenever AppendEntries returns successfully, the leader knows
	 *   that the follower's log is identical to its own log up through the new
	 *   entries (Log Matching Property in Figure 3.2).
	 */
	if (next_index == 1) {
		/* We're including the very first entry, so prevIndex and prevTerm are
		 * null. If the first entry is not available anymore, send the last
		 * snapshot. */
		if (snapshot_index > 0) {
			raft_index last_index = logLastIndex(&r->log);
			assert(last_index > 0); /* The log can't be empty */
			goto change_standby;
		}
		prev_index = 0;
		prev_term = 0;
	} else {
		/* Set prevIndex and prevTerm to the index and term of the entry at
		 * next_index - 1. */
		prev_index = next_index - 1;
		prev_term = logTermOf(&r->log, prev_index);
		/* If the entry is not anymore in our log, send the last snapshot. */
		if (prev_term == 0) {
			assert(prev_index < snapshot_index);
			tracef("missing entry at index %lld -> send snapshot", prev_index);
			goto change_standby;
		}
	}

	pi.permit = false;
	pi.replicating = PGREP_RND_NML;
	return sendAppendEntries(r, i, prev_index, prev_term, pi);


pgrep:
	return sendPgrepTickMessage(r, i, pi);

/* Pgrep:
 *
 *   Chage server role to standby.
 */
change_standby:
	ZSINFO(gzlog, "[raft][%d][%d][%s]: "
		   "set server i[%d] RAFT_STANDBY state. ", rkey(r), r->state, __func__, i);
	assignRole(r, server, RAFT_STANDBY);
	return 0;

	(void)sendSnapshot;
}

/* Possibly trigger I/O requests for newly appended log entries or heartbeats.
 *
 * This function loops through all followers and triggers replication on them.
 *
 * It must be called only by leaders. */
static int triggerAll(struct raft *r)
{
	unsigned i;
	int rv;

	assert(r->state == RAFT_LEADER);

	/* Trigger replication for servers we didn't hear from recently. */
	for (i = 0; i < r->configuration.n; i++) {
		struct raft_server *server = &r->configuration.servers[i];
		if (server->id == r->id) {
			continue;
		}
		/* Skip spare servers, unless they're being promoted. */
		if (server->role == RAFT_SPARE &&
			server->id != r->leader_state.promotee_id) {
			continue;
		}

		rv = replicationProgress(r, i);
		if (rv != 0 && rv != RAFT_NOCONNECTION) {
			/* This is not a critical failure, let's just log it. */
			tracef("failed to send append entries to server %u: %s (%d)",
				   server->id, raft_strerror(rv), rv);
		}
	}

	return 0;
}

int replicationHeartbeat(struct raft *r)
{
	return triggerAll(r);
}

/* Context for a write log entries request that was submitted by a leader. */
struct appendLeader {
	struct raft *raft;          /* Instance that has submitted the request */
	raft_index index;           /* Index of the first entry in the request. */
	struct raft_entry *entries; /* Entries referenced in the request. */
	unsigned n;                 /* Length of the entries array. */
	struct raft_io_append req;
};

/* Called after a successful append entries I/O request to update the index of
 * the last entry stored on disk. Return how many new entries that are still
 * present in our in-memory log were stored. */
static size_t updateLastStored(struct raft *r,
							   raft_index first_index,
							   struct raft_entry *entries,
							   size_t n_entries)
{
	size_t i;

	/* Check which of these entries is still in our in-memory log */
	for (i = 0; i < n_entries; i++) {
		struct raft_entry *entry = &entries[i];
		raft_index index = first_index + i;
		raft_term local_term = logTermOf(&r->log, index);

		/* If we have no entry at this index, or if the entry we have now has a
		 * different term, it means that this entry got truncated, so let's stop
		 * here. */
		if (local_term == 0 || (local_term > 0 && local_term != entry->term)) {
			break;
		}

		/* If we do have an entry at this index, its term must match the one of
		 * the entry we wrote on disk. */
		assert(local_term != 0 && local_term == entry->term);
	}

	/* Pgrep may sync the last_stored to the leader. So can't add i directly. */
	raft_index old = r->last_stored;
	r->last_stored = max(first_index + i - 1, r->last_stored);

	ZSINFO(gzlog, "[raft][%d][%d][%s] first_index[%lld] old[%lld] last_stored[%lld].",
		   rkey(r), r->state, __func__, first_index, old, r->last_stored);

	return i;
}

/* Get the request matching the given index and type, if any. */
static struct request* getRequest(struct raft *r,
								  const raft_index index,
								  int type)
{
	queue *head;
	struct request *req;

	if (r->state != RAFT_LEADER) {
		return NULL;
	}
	QUEUE_FOREACH(head, &r->leader_state.requests) {
		req = QUEUE_DATA(head, struct request, queue);
		if (req->index == index) {
			assert(req->type == type);
			QUEUE_REMOVE(head);
			return req;
		}
	}
	return NULL;
}

/* Invoked once a disk write request for new entries has been completed. */
static void appendLeaderCb(struct raft_io_append *req, int status)
{
	struct appendLeader *request = req->data;
	struct raft *r = request->raft;
	size_t server_index;
	int rv;

	tracef("leader: written %u entries starting at %lld: status %d", request->n,
		   request->index, status);

	/* In case of a failed disk write, if we were the leader creating these
	 * entries in the first place, truncate our log too (since we have appended
	 * these entries to it) and fire the request callback. */
	if (status != 0) {
		struct raft_apply *apply;
		ErrMsgTransfer(r->io->errmsg, r->errmsg, "io");
		apply =
			(struct raft_apply *)getRequest(r, request->index, RAFT_COMMAND);
		if (apply != NULL) {
			if (apply->cb != NULL) {
				apply->cb(apply, status, NULL);
			}
		}
		goto out;
	}

	updateLastStored(r, request->index, request->entries, request->n);

	/* If we are not leader anymore, just discard the result. */
	if (r->state != RAFT_LEADER) {
		tracef("local server is not leader -> ignore write log result");
		goto out;
	}

	/* If Check if we have reached a quorum. */
	server_index = configurationIndexOf(&r->configuration, r->id);

	/* Only update the next index if we are part of the current
	 * configuration. The only case where this is not true is when we were
	 * asked to remove ourselves from the cluster.
	 *
	 * From Section 4.2.2:
	 *
	 *   there will be a period of time (while it is committing Cnew) when a
	 *   leader can manage a cluster that does not include itself; it
	 *   replicates log entries but does not count itself in majorities.
	 */
	if (server_index < r->configuration.n) {
		r->leader_state.progress[server_index].match_index = r->last_stored;
	} else {
		const struct raft_entry *entry = logGet(&r->log, r->last_stored);
		assert(entry->type == RAFT_CHANGE);
	}

	/* Check if we can commit some new entries. */
	replicationQuorum(r, r->last_stored);

	rv = replicationApply(r);
	if (rv != 0) {
		/* TODO: just log the error? */
	}

out:
	/* Tell the log that we're done referencing these entries. */
	logRelease(&r->log, request->index, request->entries, request->n);
	if (status != 0) {
		logTruncate(&r->log, request->index);
	}
	raft_free(request);
}

/* Submit a disk write for all entries from the given index onward. */
static int appendLeader(struct raft *r, raft_index index)
{
	struct raft_entry *entries;
	unsigned n;
	struct appendLeader *request;
	int rv;

	assert(r->state == RAFT_LEADER);
	assert(index > 0);
	assert(index > r->last_stored);

	/* Acquire all the entries from the given index onwards. */
	rv = logAcquire(&r->log, index, &entries, &n);
	if (rv != 0) {
		goto err;
	}

	/* We expect this function to be called only when there are actually
	 * some entries to write. */
	assert(n > 0);

	/* Allocate a new request. */
	request = raft_malloc(sizeof*request);
	if (request == NULL) {
		rv = RAFT_NOMEM;
		goto err_after_entries_acquired;
	}

	request->raft = r;
	request->index = index;
	request->entries = entries;
	request->n = n;
	request->req.data = request;

	rv = r->io->append(r->io, &request->req, entries, n, appendLeaderCb);
	if (rv != 0) {
		ErrMsgTransfer(r->io->errmsg, r->errmsg, "io");
		goto err_after_request_alloc;
	}

	return 0;

err_after_request_alloc:
	raft_free(request);
err_after_entries_acquired:
	logRelease(&r->log, index, entries, n);
err:
	assert(rv != 0);
	return rv;
}

int replicationTrigger(struct raft *r, raft_index index)
{
	int rv;

	rv = appendLeader(r, index);
	if (rv != 0) {
		return rv;
	}

	return triggerAll(r);
}

/* Helper to be invoked after a promotion of a non-voting server has been
 * requested via @raft_assign and that server has caught up with logs.
 *
 * This function changes the local configuration marking the server being
 * promoted as actually voting, appends the a RAFT_CHANGE entry with the new
 * configuration to the local log and triggers its replication. */
//static int triggerActualPromotion(struct raft *r)
//{
//	raft_index index;
//	raft_term term = r->current_term;
//	size_t server_index;
//	struct raft_server *server;
//	int old_role;
//	int rv;
//
//	assert(r->state == RAFT_LEADER);
//	assert(r->leader_state.promotee_id != 0);
//
//	server_index =
//		configurationIndexOf(&r->configuration, r->leader_state.promotee_id);
//	assert(server_index < r->configuration.n);
//
//	server = &r->configuration.servers[server_index];
//
//	assert(server->role != RAFT_VOTER);
//
//	/* Update our current configuration. */
//	old_role = server->role;
//	server->role = RAFT_VOTER;
//
//	/* Index of the entry being appended. */
//	index = logLastIndex(&r->log) + 1;
//
//	/* Encode the new configuration and append it to the log. */
//	rv = logAppendConfiguration(&r->log, term, &r->configuration);
//	if (rv != 0) {
//		goto err;
//	}
//
//	/* Start writing the new log entry to disk and send it to the followers. */
//	rv = replicationTrigger(r, index);
//	if (rv != 0) {
//		goto err_after_log_append;
//	}
//
//	r->leader_state.promotee_id = 0;
//	r->configuration_uncommitted_index = logLastIndex(&r->log);
//
//	return 0;
//
//err_after_log_append:
//	logTruncate(&r->log, index);
//
//err:
//	server->role = old_role;
//
//	assert(rv != 0);
//	return rv;
//}

int replicationUpdate(struct raft *r,
					  const raft_id id,
					  const struct raft_append_entries_result *result)
{
//	bool is_being_promoted;
	raft_index last_index;
	unsigned i;
	int rv;

	i = configurationIndexOf(&r->configuration, id);

	assert(r->state == RAFT_LEADER);
	assert(i < r->configuration.n);

	progressMarkRecentRecv(r, i);

	/* If the RPC failed because of a log mismatch, retry.
	 *
	 * From Figure 3.1:
	 *
	 *   [Rules for servers] Leaders:
	 *
	 *   - If AppendEntries fails because of log inconsistency:
	 *     decrement nextIndex and retry.
	 */
	if (result->rejected > 0) {
		bool retry;
		retry = progressMaybeDecrement(r, i, result->rejected,
									   result->last_log_index);
		if (retry) {
			/* Retry, ignoring errors. */
			tracef("log mismatch -> send old entries to %u", id);
			replicationProgress(r, i);
		}
		return 0;
	}

	/* In case of success the remote server is expected to send us back the
	 * value of prevLogIndex + len(entriesToAppend). If it has a longer log, it
	 * might be a leftover from previous terms. */
	last_index = result->last_log_index;
	if (last_index > logLastIndex(&r->log)) {
		last_index = logLastIndex(&r->log);
	}

	/* If the RPC succeeded, update our counters for this server.
	 *
	 * From Figure 3.1:
	 *
	 *   [Rules for servers] Leaders:
	 *
	 *   If successful update nextIndex and matchIndex for follower.
	 */
	if (!progressMaybeUpdate(r, i, last_index)) {
		return 0;
	}

	switch (progressState(r, i)) {
	case PROGRESS__SNAPSHOT:
		/* If a snapshot has been installed, transition back to probe */
		if (progressSnapshotDone(r, i)) {
			progressToProbe(r, i);
		}
		break;
	case PROGRESS__PROBE:
		/* Transition to pipeline */
		progressToPipeline(r, i);
	}

	/* Check if we can commit some new entries. */
	replicationQuorum(r, r->last_stored);

	if (!result->pi.permit)
		rv = replicationApply(r);
	if (rv != 0) {
		/* TODO: just log the error? */
	}

	/* Abort here we have been removed and we are not leaders anymore. */
	if (r->state != RAFT_LEADER) {
		goto out;
	}

	/* Get again the server index since it might have been removed from the
	 * configuration. */
	i = configurationIndexOf(&r->configuration, id);

	if (i < r->configuration.n) {
		/* If we are transferring leadership to this follower, check if its log
		 * is now up-to-date and, if so, send it a TimeoutNow RPC (unless we
		 * already did). */
		if (r->transfer != NULL && r->transfer->id == id) {
			if (progressIsUpToDate(r, i) && r->transfer->send.data == NULL) {
				rv = membershipLeadershipTransferStart(r);
				if (rv != 0) {
					membershipLeadershipTransferClose(r);
				}
			}
		}
		/* If this follower is in pipeline mode, send it more entries. */
		if (progressState(r, i) == PROGRESS__PIPELINE) {
			replicationProgress(r, i);
		}
	}

out:
	return 0;
}

static void sendAppendEntriesResultCb(struct raft_io_send *req, int status)
{
	(void)status;
	HeapFree(req);
}

void sendAppendEntriesResult(
	struct raft *r,
	const struct raft_append_entries_result *result,
	const struct raft_append_entries *args)
{
	struct raft_message message;
	struct raft_io_send *req;
	int rv;

	message.type = RAFT_IO_APPEND_ENTRIES_RESULT;
	message.server_id = r->follower_state.current_leader.id;
	message.server_address = r->follower_state.current_leader.address;
	message.append_entries_result = *result;

	if (args->src_server != (raft_id)-1 &&
		args->src_server != message.server_id) {
		ZSWARNING(gzlog, "[raft][%d][%d][pkt:%d][%s]: leader changed, discard this packet.",
				  rkey(r), r->state, args->pkt, __func__);
		return;
	}

	req = raft_malloc(sizeof*req);
	if (req == NULL) {
		return;
	}
	req->data = r;

	ZSINFO(gzlog, "[raft][%d][%d][pkt:%d][%s] permit[%d] time[%ld].",
		   rkey(r), r->state, args->pkt, __func__, result->pi.permit, result->pi.time);

	const struct raft_server *me = configurationGet(&r->configuration, r->id);
	int role = -1;
	int pre_role = -1;
	if (me) {
		role = me->role;
		pre_role = me->pre_role;
	}

	ZSINFO(gzlog,
		   "dumpstatus:###"
		   "{ \"time\":%ld, "
		   "  \"raft\":%d, "
		   "  \"id\":%lld,"
		   "  \"state\":%d, "
		   "  \"role\":%d, "
		   "  \"pre_role\":%d, "
		   "  \"last_index\":%llu, "
		   "  \"last_applying\":%llu, "
		   "  \"last_applied\":%llu, "
		   "  \"last_stored\":%llu, "
		   "  \"commit_index\":%llu, "
		   "  \"configuration_uncommitted_index\":%lld, "
		   "  \"promotee_id\":%lld "
		   "}###",
		   raft_tick_count_ns(),
		   rkey(r),
		   r->id,
		   r->state,
		   role,
		   pre_role,
		   logLastIndex(&r->log),
		   r->last_applying,
		   r->last_applied,
		   r->last_stored,
		   r->commit_index,
		   r->configuration_uncommitted_index,
		   r->leader_state.promotee_id);

	rv = r->io->send(r->io, req, &message, sendAppendEntriesResultCb);
	if (rv != 0) {
		raft_free(req);
	}
}

/* Context for a write log entries request that was submitted by a follower. */
struct appendFollower {
	struct raft *raft; /* Instance that has submitted the request */
	raft_index index;  /* Index of the first entry in the request. */
	struct raft_append_entries args;
	struct raft_io_append req;
};

static void appendFollowerCb(struct raft_io_append *req, int status)
{
	struct appendFollower *request = req->data;
	struct raft *r = request->raft;
	struct raft_append_entries *args = &request->args;
	struct raft_append_entries_result result;
	size_t i;
	size_t j;
	int rv;
	bool free_request = true;


	ZSINFO(gzlog, "[raft][%d][%d][%s]: replicating[%d] permit[%d] req_index[%lld]",
		   rkey(r), r->state, __func__, args->pi.replicating,
		   args->pi.permit, request->index);

	assert(args->entries != NULL);
	assert(args->n_entries > 0);

	result.term = r->current_term;
	if (status != 0) {
		if (r->state != RAFT_FOLLOWER) {
			tracef("local server is not follower -> ignore I/O failure");
			goto out;
		}
		result.rejected = args->prev_log_index + 1;
		goto respond;
	}

	/* If we're shutting down or have errored, ignore the result. */
	if (r->state == RAFT_UNAVAILABLE) {
		tracef("local server is unavailable -> ignore I/O result");
		goto out;
	}

	i = updateLastStored(r, request->index, args->entries, args->n_entries);

	/* If none of the entries that we persisted is present anymore in our
	 * in-memory log, there's nothing to report or to do. We just discard
	 * them. */
	if (i == 0 || r->state != RAFT_FOLLOWER) {
		goto out;
	}

	/* Possibly apply configuration changes as uncommitted. */
	for (j = 0; j < i; j++) {
		struct raft_entry *entry = &args->entries[j];
		raft_index index = request->index + j;
		raft_term local_term = logTermOf(&r->log, index);

		assert(local_term != 0 && local_term == entry->term);

		if (entry->type == RAFT_CHANGE) {
			rv = membershipUncommittedChange(r, index, entry);
			if (rv != 0) {
				goto out;
			}
		}
	}

	/* From Figure 3.1:
	 *
	 *   AppendEntries RPC: Receiver implementation: If leaderCommit >
	 *   commitIndex, set commitIndex = min(leaderCommit, index of last new
	 *   entry).
	 */
	if (args->leader_commit > r->commit_index) {

		r->commit_index = min(args->leader_commit, r->last_stored);
		if (args->pi.replicating) {
			rv = replicationApplyReq(r, request);
			if (rv == 0) {
				free_request = false;
				goto out;
			}
		} else
			rv = replicationApply(r);

		if (rv != 0)
			goto out;
	}

	if (r->state != RAFT_FOLLOWER) {
		tracef("local server is not follower -> don't send result");
		goto out;
	}

	result.rejected = 0;

respond:
	result.last_log_index = r->last_stored;
	result.pkt = args->pkt;
	result.pi = args->pi;

	/* Pgrep:
	 *
	 *  If in pgrep progress, reply in replicationApply is ecpected.
	 *  So here just return error.
	 */
	if (args->pi.replicating) {
		result.pi.replicating = PGREP_RND_ERR;
	}

	sendAppendEntriesResult(r, &result, args);

out:
	if (free_request) {
		logRelease(&r->log, request->index, request->args.entries,
				   request->args.n_entries);
		raft_free(request);
	}
}

/* Check the log matching property against an incoming AppendEntries request.
 *
 * From Figure 3.1:
 *
 *   [AppendEntries RPC] Receiver implementation:
 *
 *   2. Reply false if log doesn't contain an entry at prevLogIndex whose
 *   term matches prevLogTerm.
 *
 * Return 0 if the check passed.
 *
 * Return 1 if the check did not pass and the request needs to be rejected.
 *
 * Return -1 if there's a conflict and we need to shutdown. */
static int checkLogMatchingProperty(struct raft *r,
									const struct raft_append_entries *args)
{
	raft_term local_prev_term;

	/* If this is the very first entry, there's nothing to check. */
	if (args->prev_log_index == 0) {
		return 0;
	}

	local_prev_term = logTermOf(&r->log, args->prev_log_index);
	if (local_prev_term == 0) {
		tracef("no entry at index %llu -> reject", args->prev_log_index);
		return 1;
	}

	if (local_prev_term != args->prev_log_term) {
		if (args->prev_log_index <= r->commit_index) {
			/* Should never happen; something is seriously wrong! */
			tracef(
				"conflicting terms %llu and %llu for entry %llu (commit "
				"index %llu) -> shutdown",
				local_prev_term, args->prev_log_term, args->prev_log_index,
				r->commit_index);
			return -1;
		}
		tracef("previous term mismatch -> reject");
		return 1;
	}

	return 0;
}

/* Delete from our log all entries that conflict with the ones in the given
 * AppendEntries request.
 *
 * From Figure 3.1:
 *
 *   [AppendEntries RPC] Receiver implementation:
 *
 *   3. If an existing entry conflicts with a new one (same index but
 *   different terms), delete the existing entry and all that follow it.
 *
 * The i output parameter will be set to the array index of the first new log
 * entry that we don't have yet in our log, among the ones included in the given
 * AppendEntries request. */
static int deleteConflictingEntries(struct raft *r,
									const struct raft_append_entries *args,
									size_t *i)
{
	size_t j;
	int rv;

	for (j = 0; j < args->n_entries; j++) {
		struct raft_entry *entry = &args->entries[j];
		raft_index entry_index = args->prev_log_index + 1 + j;
		raft_term local_term = logTermOf(&r->log, entry_index);

		if (local_term > 0 && local_term != entry->term) {
			if (entry_index <= r->commit_index) {
				/* Should never happen; something is seriously wrong! */
				tracef("new index conflicts with committed entry -> shutdown");

				return RAFT_SHUTDOWN;
			}

			tracef("log mismatch -> truncate (%llu)", entry_index);

			/* Possibly discard uncommitted configuration changes. */
			if (r->configuration_uncommitted_index >= entry_index) {
				rv = membershipRollback(r);
				if (rv != 0) {
					return rv;
				}
			}

			/* Delete all entries from this index on because they don't
			 * match. */
			rv = r->io->truncate(r->io, entry_index);
			if (rv != 0) {
				return rv;
			}
			logTruncate(&r->log, entry_index);

			/* Drop information about previously stored entries that have just
			 * been discarded. */
			if (r->last_stored >= entry_index) {
				r->last_stored = entry_index - 1;
			}

			/* We want to append all entries from here on, replacing anything
			 * that we had before. */
			break;
		} else if (local_term == 0) {
			/* We don't have an entry at this index, so we want to append this
			 * new one and all the subsequent ones. */
			break;
		}
	}

	*i = j;

	return 0;
}


static int try_truncate(struct raft *r, raft_index index)
{
	int rv;
	if (r->configuration_uncommitted_index >= index) {
		rv = membershipRollback(r);
		if (rv)
			return rv;
	}

	if (index > logLastIndex(&r->log))
		return 0;

	if (isRefs(&r->log, index)) {
		return RAFT_LOG_BUSY;
	}

	r->io->truncate(r->io, index);
	if (rv)
		return rv;

	logTruncate(&r->log, index);

	return 0;
}

static void takeSnapshotCb(struct raft_io_snapshot_put *req, int status)
{
	struct raft *r = req->data;
	struct raft_snapshot *snapshot;

	r->snapshot.put.data = NULL;
	snapshot = &r->snapshot.pending;

	if (status != 0) {
		tracef("snapshot %lld at term %lld: %s", snapshot->index,
			   snapshot->term, raft_strerror(status));
		goto out;
	}

	logSnapshot(&r->log, snapshot->index, r->snapshot.trailing);

out:
	snapshotClose(&r->snapshot.pending);
	r->snapshot.pending.term = 0;
}

static int pgrep_take_snapshot(struct raft *r)
{
	struct raft_snapshot *snapshot;
	unsigned i;
	int rv;

	snapshot = &r->snapshot.pending;
	snapshot->index = r->log.snapshot.last_index;
	snapshot->term = r->log.snapshot.last_term;

	ZSINFO(gzlog, "[raft][%d][%d][%s][conf_dump] at %lld %lld.",
		   rkey(r), r->state, __func__, snapshot->term, snapshot->index);
	for (i = 0; i < r->configuration.n; i++) {
		const struct raft_server *servert = &r->configuration.servers[i];
		ZSINFO(gzlog, "[raft][%d][%d][%s][conf_dump] i[%d] id[%lld] role[%d] pre_role[%d]",
			   rkey(r), r->state, __func__, i,
			   servert->id, servert->role, servert->pre_role);
	}

	rv = configurationCopy(&r->configuration, &snapshot->configuration);
	if (rv != 0) {
		goto abort;
	}

	snapshot->configuration_index = r->configuration_index;

	rv = r->fsm->snapshot(r->fsm, &snapshot->bufs, &snapshot->n_bufs);
	if (rv != 0)
		goto abort_after_config_copy;

	assert(r->snapshot.put.data == NULL);
	r->snapshot.put.data = r;
	rv = r->io->snapshot_put(r->io, 0, &r->snapshot.put,
							 snapshot, takeSnapshotCb);
	if (rv != 0) {
		goto abort_after_fsm_snapshot;
	}

	return 0;

abort_after_fsm_snapshot:
	for (i = 0; i < snapshot->n_bufs; i++) {
		raft_free(snapshot->bufs[i].base);
	}
	raft_free(snapshot->bufs);
abort_after_config_copy:
	raft_configuration_close(&snapshot->configuration);
abort:
	r->snapshot.pending.term = 0;
	return rv;
}

int sync_pgrep_index(struct raft *r,
					 const struct raft_append_entries *args)
{
	ZSINFO(gzlog, "[raft][%d][%d][pkt:%d][%s].", rkey(r), r->state, args->pkt, __func__);

	int rv;
	raft_index last_index = r->log.snapshot.last_index;
	raft_term last_term = r->log.snapshot.last_term;
	raft_index configuration_index = r->configuration_index;

	r->log.snapshot.last_index = args->prev_log_index;
	r->log.snapshot.last_term = args->prev_log_term;
	r->configuration_index = 0;

	rv = pgrep_take_snapshot(r);
	if (rv)
		goto roll_back;

	r->log.offset = args->prev_log_index;
	r->last_stored = args->prev_log_index;
	r->commit_index = args->prev_log_index;
	r->last_applied = args->prev_log_index;
	r->last_applying = args->prev_log_index;
	r->io->pgrep_reset_ckposi(r->io);

	return 0;

roll_back:

	r->log.snapshot.last_index = last_index;
	r->log.snapshot.last_term = last_term;
	r->configuration_index = configuration_index;

	ZSINFO(gzlog, "[raft][%d][%d][pkt:%d][%s] failed[%d].", rkey(r), r->state, args->pkt, __func__, rv);

	return rv;
}

static int checkPgreplicating(
	struct raft *r,
	const struct raft_append_entries *args,
	bool *async,
	size_t *i,
	size_t *n)
{
	int rv;

	if (args->pi.replicating) {

		ZSINFO(gzlog, "[raft][%d][%d][pkt:%d][%s] dump replicating[%d]: "
			   "last_stored[%lld] last_applied[%lld] last_applying[%lld] "
			   "prev_log_index[%lld] n_entries[%d]",
			   rkey(r), r->state, args->pkt, __func__, args->pi.replicating,
			   r->last_stored, r->last_applied, r->last_applying,
			   args->prev_log_index, args->n_entries);

		if (args->term > r->last_append_term)
			r->last_append_time = 0;

		if (args->pi.time <= r->last_append_time) {
			ZSWARNING(gzlog, "[raft][%d][%d][pkt:%d] message out of date time[%ld] last_append_time[%ld].",
					  rkey(r), r->state, args->pkt, args->pi.time, r->last_append_time);
			return RAFT_DISCARD;
		}

		r->last_append_time = args->pi.time;
		r->last_append_term = r->current_term;
		ZSINFO(gzlog, "[raft][%d][%d][pkt:%d] update last_append_time[%ld].",
			   rkey(r), r->state, args->pkt, r->last_append_time);

		/* If it's the first message, just reply the last_stored index. */
		if (args->pi.replicating == PGREP_RND_BGN) {
			raft_index trunc_index = max(r->last_applied, r->last_applying) + 1;

			r->io->pgrep_update_lctime(r->io, args->pi.time);

			rv = try_truncate(r, trunc_index);
			if (rv != 0)
				goto async_false;

			r->last_stored = trunc_index - 1;
			r->commit_index = trunc_index - 1;

			rv = 0;
			goto async_false;
		}

		/* If I can't catchup the leader, sync indices with the leader for restart pgrep. */
		if (args->prev_log_index > r->last_stored) {

			/* There are some entries applying, can not truncate log. */
			if (r->last_applying != r->last_applied) {
				ZSWARNING(gzlog, "[raft][%d][%d][pkt:%d] There are some "
						  "entries applying, can not truncate log.",
						  rkey(r), r->state, args->pkt);
				rv = RAFT_APPLY_BUSY;
				goto async_false;
			}

			ZSINFO(gzlog, "[raft][%d][%d][pkt:%d] logTruncate to [%lld] nums[%ld].",
				   rkey(r), r->state, args->pkt, r->log.offset + 1, logNumEntries(&r->log));

			rv = try_truncate(r, r->log.offset + 1);
			if (rv != 0)
				goto async_false;

			ZSINFO(gzlog, "[raft][%d][%d][pkt:%d] after logTruncate to [%lld] nums[%ld].",
				   rkey(r), r->state, args->pkt, r->log.offset + 1, logNumEntries(&r->log));

			rv = sync_pgrep_index(r, args);
			if (rv != 0)
				goto async_false;

			//exception_exit_test(r, (uint32_t)(rand() % 8));
		}

		*i = r->last_stored - args->prev_log_index;
		*n = args->n_entries - *i;

		ZSINFO(gzlog, "[raft][%d][%d][pkt:%d][%s] dump after: "
			   "last_stored[%lld] last_applied[%lld] last_applying[%lld] "
			   "prev_log_index[%lld] n_entries[%d]",
			   rkey(r), r->state, args->pkt, __func__, r->last_stored,
			   r->last_applied, r->last_applying, args->prev_log_index, args->n_entries);

		/* The leader's send log entries behind me, just reply success. */
		if (args->prev_log_index + args->n_entries <= r->last_stored) {
			*n = 0;
			ZSINFO(gzlog, "[raft][%d][%d][pkt:%d] I have the log entries already.",
				   rkey(r), r->state, args->pkt);
			rv = 0;
			goto async_false;
		}
	} else {
		/* As i received normal append message, initial pgrep infomation. */
		r->io->pgrep_reset_ckposi(r->io);
		r->last_append_time = args->pi.time;
		r->last_append_term = r->current_term;
		ZSINFO(gzlog, "[raft][%d][%d][pkt:%d] update last_append_time[%ld].",
			   rkey(r), r->state, args->pkt, r->last_append_time);
	}

	return 0;

async_false:
	*async = false;
	return rv;
}

int replicationAppend(struct raft *r,
					  const struct raft_append_entries *args,
					  raft_index *rejected,
					  bool *async,
					  struct pgrep_permit_info *pi)
{
	UNUSED(pi);

	struct appendFollower *request;
	int match;
	size_t n;
	size_t i;
	size_t j;
	int rv;

	assert(r != NULL);
	assert(args != NULL);
	assert(rejected != NULL);
	assert(async != NULL);

	assert(r->state == RAFT_FOLLOWER);

	*rejected = args->prev_log_index;
	*async = false;

	ZSINFO(gzlog, "[raft][%d][%d][pkt:%d][%s]: replicating[%d] permit[%d] "
		   "last_applying[%lld] last_applied[%lld] last_stored[%lld]",
		   rkey(r), r->state, args->pkt, __func__, args->pi.replicating,
		   args->pi.permit, r->last_applying, r->last_applied, r->last_stored);

	/* Pgrep:
	 *
	 *  Skip original cheking log match && delete confilicts when pgrep progress.
	 *  But checking log match use our new rules. 
	 */

	if (!args->pi.replicating) {
		/* Check the log matching property. */
		match = checkLogMatchingProperty(r, args);
		if (match != 0) {
			assert(match == 1 || match == -1);
			return match == 1 ? 0 : RAFT_SHUTDOWN;
		}

		/* Delete conflicting entries. */
		rv = deleteConflictingEntries(r, args, &i);
		if (rv != 0) {
			return rv;
		}
	} else {
		i = 0;
	}

	*rejected = 0;

	n = args->n_entries - i; /* Number of new entries */

	/* If this is an empty AppendEntries, there's nothing to write. However we
	 * still want to check if we can commit some entry.
	 *
	 * From Figure 3.1:
	 *
	 *   AppendEntries RPC: Receiver implementation: If leaderCommit >
	 *   commitIndex, set commitIndex = min(leaderCommit, index of last new
	 *   entry).
	 */

	/* Pgrep:
	 *
	 *  When pgrep progress, every append rpc which has log entries must applied to
	 *  fsm simultaneously.
	 */
	if (!args->pi.replicating && n == 0) {
		if (args->leader_commit > r->commit_index ||
			args->leader_commit > r->last_applying) {
			r->commit_index = min(args->leader_commit, r->last_stored);
			rv = replicationApply(r);
			if (rv != 0) {
				return rv;
			}
		}

		return 0;
	}

	if (!args->pi.replicating && n == 0)
		return 0;

	*async = true;

	request = raft_malloc(sizeof*request);
	if (request == NULL) {
		rv = RAFT_NOMEM;
		goto err;
	}


	/* Pgrep:
	 *
	 *  Check if pgrep can going on or break.
	 */
	rv = checkPgreplicating(r, args, async, &i, &n);
	if (*async == false)
		return rv;
	if (rv == RAFT_DISCARD)
		return 0;


	if (rv)
		goto err;


	request->raft = r;
	request->args = *args;
	/* Index of first new entry */
	request->index = args->prev_log_index + 1 + i;

	/* Update our in-memory log to reflect that we received these entries. We'll
	 * notify the leader of a successful append once the write entries request
	 * that we issue below actually completes.  */
	for (j = 0; j < n; j++) {
		struct raft_entry *entry = &args->entries[i + j];
		/* TODO This copy should not strictly be necessary, as the batch logic will
		 * take care of freeing the batch buffer in which the entries are received.
		 * However, this would lead to memory spikes in certain edge cases.
		 * https://github.com/canonical/dqlite/issues/276
		 */
		struct raft_entry copy = { 0 };
		rv = entryCopy(entry, &copy);
		if (rv != 0) {
			goto err_after_request_alloc;
		}

		rv = logAppend(&r->log, copy.term, copy.type, &copy.buf, NULL);
		if (rv != 0) {
			goto err_after_request_alloc;
		}
	}

	/* Acquire the relevant entries from the log. */
	rv = logAcquire(&r->log, request->index, &request->args.entries,
					&request->args.n_entries);
	if (rv != 0) {
		goto err_after_request_alloc;
	}

	assert(request->args.n_entries == n);

	ZSINFO(gzlog, "[raft][%d][%d][pkt:%d] will io->append req_index[%lld] "
		   "n_entries[%d] n[%ld] last_index[%lld].",
		   rkey(r), r->state, args->pkt, request->index,
		   request->args.n_entries, n, logLastIndex(&r->log));

	request->req.data = request;
	rv = r->io->append(r->io, &request->req, request->args.entries,
					   request->args.n_entries, appendFollowerCb);
	if (rv != 0) {
		ErrMsgTransfer(r->io->errmsg, r->errmsg, "io");
		goto err_after_acquire_entries;
	}

	entryBatchesDestroy(args->entries, args->n_entries);
	return 0;

err_after_acquire_entries:
	/* Release the entries related to the IO request */
	logRelease(&r->log, request->index, request->args.entries,
			   request->args.n_entries);

err_after_request_alloc:
	/* Release all entries added to the in-memory log, making
	 * sure the in-memory log and disk don't diverge, leading
	 * to future log entries not being persisted to disk.
	 */
	if (j != 0) {
		logTruncate(&r->log, request->index);
	}
	raft_free(request);

err:
	assert(rv != 0);
	ZSINFO(gzlog, "[raft][%d][%d][pkt:%d][%s] error[%d].",
		   rkey(r), r->state, args->pkt, __func__, rv);
	return rv;
}

struct recvInstallSnapshot {
	struct raft *raft;
	struct raft_snapshot snapshot;
};

static void installSnapshotCb(struct raft_io_snapshot_put *req, int status)
{
	struct recvInstallSnapshot *request = req->data;
	struct raft *r = request->raft;
	struct raft_snapshot *snapshot = &request->snapshot;
	struct raft_append_entries_result result;
	int rv;

	r->snapshot.put.data = NULL;

	result.term = r->current_term;

	/* If we are shutting down, let's discard the result. TODO: what about other
	 * states? */
	if (r->state == RAFT_UNAVAILABLE) {
		goto discard;
	}

	if (status != 0) {
		result.rejected = snapshot->index;
		tracef("save snapshot %llu: %s", snapshot->index,
			   raft_strerror(status));
		goto discard;
	}

	/* From Figure 5.3:
	 *
	 *   7. Discard the entire log
	 *   8. Reset state machine using snapshot contents (and load lastConfig
	 *      as cluster configuration).
	 */
	rv = snapshotRestore(r, snapshot);
	if (rv != 0) {
		result.rejected = snapshot->index;
		tracef("restore snapshot %llu: %s", snapshot->index,
			   raft_strerror(status));
		goto discard;
	}

	tracef("restored snapshot with last index %llu", snapshot->index);

	result.rejected = 0;

	goto respond;

discard:
	/* In case of error we must also free the snapshot data buffer and free the
	 * configuration. */
	raft_free(snapshot->bufs[0].base);
	raft_configuration_close(&snapshot->configuration);

respond:
	if (r->state != RAFT_UNAVAILABLE) {
		result.last_log_index = r->last_stored;
		sendAppendEntriesResult(r, &result, NULL);
	}

	raft_free(request);
}

int replicationInstallSnapshot(struct raft *r,
							   const struct raft_install_snapshot *args,
							   raft_index *rejected,
							   bool *async)
{
	struct recvInstallSnapshot *request;
	struct raft_snapshot *snapshot;
	raft_term local_term;
	int rv;

	assert(r->state == RAFT_FOLLOWER);

	*rejected = args->last_index;
	*async = false;

	/* If we are taking a snapshot ourselves or installing a snapshot, ignore
	 * the request, the leader will eventually retry. TODO: we should do
	 * something smarter. */
	if (r->snapshot.pending.term != 0 || r->snapshot.put.data != NULL) {
		*async = true;
		return RAFT_BUSY;
	}

	/* If our last snapshot is more up-to-date, this is a no-op */
	if (r->log.snapshot.last_index >= args->last_index) {
		*rejected = 0;
		return 0;
	}

	/* If we already have all entries in the snapshot, this is a no-op */
	local_term = logTermOf(&r->log, args->last_index);
	if (local_term != 0 && local_term >= args->last_term) {
		*rejected = 0;
		return 0;
	}

	*async = true;

	/* Preemptively update our in-memory state. */
	logRestore(&r->log, args->last_index, args->last_term);

	r->last_stored = 0;

	request = raft_malloc(sizeof*request);
	if (request == NULL) {
		rv = RAFT_NOMEM;
		goto err;
	}
	request->raft = r;

	snapshot = &request->snapshot;
	snapshot->term = args->last_term;
	snapshot->index = args->last_index;
	snapshot->configuration_index = args->conf_index;
	snapshot->configuration = args->conf;

	snapshot->bufs = raft_malloc(sizeof*snapshot->bufs);
	if (snapshot->bufs == NULL) {
		rv = RAFT_NOMEM;
		goto err_after_request_alloc;
	}
	snapshot->bufs[0] = args->data;
	snapshot->n_bufs = 1;

	assert(r->snapshot.put.data == NULL);
	r->snapshot.put.data = request;
	rv = r->io->snapshot_put(r->io,
							 0 /* zero trailing means replace everything */,
							 &r->snapshot.put, snapshot, installSnapshotCb);
	if (rv != 0) {
		goto err_after_bufs_alloc;
	}

	return 0;

err_after_bufs_alloc:
	raft_free(snapshot->bufs);
	r->snapshot.put.data = NULL;
err_after_request_alloc:
	raft_free(request);
err:
	assert(rv != 0);
	return rv;
}

void replicationApplyLeaderCb(struct raft *r, struct pgrep_permit_info pi)
{
	/* Pgrep:
	 *
	 * After fsm applied all entries, call back to here, we still holding the pgrep permit.
	 * If there are a gap between prev_applied_index and last_applied, to starting a relicatingProgress.
	 */

	unsigned inx = configurationIndexOf(&r->configuration, r->pgrep_id);

	if (r->state != RAFT_LEADER || r->pgrep_id == (raft_id)-1 ||
		progressGetAppliedIndex(r, inx) == r->last_applied) {

		r->io->pgrep_raft_unpermit(r->io, &pi);
		pi.permit = false;
		ZSINFO(gzlog, "[raft][%d][%d][%s]: release pgrep permit.", rkey(r), r->state, __func__);
		replicationApply(r);
		return;
	}

	ZSINFO(gzlog, "[raft][%d][%d][%s]: start a replicationProgress pgrep_id[%lld] permit[%d].",
		   rkey(r), r->state, __func__, r->pgrep_id, pi.permit);

	replicationProgressPi(r, inx, pi);
}

void replicationApplyFollowerCb(
	struct raft *r,
	void *extra)
{
	if (!extra)
		return;

	struct appendFollower *request = extra;
	struct raft_append_entries *args = &request->args;
	struct raft_append_entries_result result;

	result.last_log_index = r->last_applied;
	result.rejected = 0;
	result.pi = args->pi;
	result.term = r->current_term;
	result.pkt = args->pkt;

	ZSINFO(gzlog, "[raft][%d][%d][pkt:%d][%s]: sendAppendEntriesResult.",
		   rkey(r), r->state, args->pkt, __func__);

	sendAppendEntriesResult(r, &result, args);

	logRelease(&r->log, request->index, request->args.entries,
			   request->args.n_entries);
	raft_free(request);
}

#if defined(RAFT_ASYNC_APPLY) && RAFT_ASYNC_APPLY
/* Context for a write log entries request that was submitted by a leader. */

struct applyBatch {
	int expect_num;
	int applied_num;
};

struct applyLog {
	struct raft *raft;          	/* Instance that has submitted the request */
	raft_index index;           	/* Index of the first entry in the request. */
	struct applyBatch *ab;
	void *extra;                	/* Extra params passed if reuqired. */
	struct pgrep_permit_info pi; 	/* Indicate pgrep permit granted. */
	struct raft_fsm_apply req;
};

static void applySectionCallbackCheck(
	struct raft *r,
	struct applyBatch *ab,
	struct pgrep_permit_info pi,
	void *extra
	)
{
	ZSINFO(gzlog, "[raft][%d][%d][%s]: expect_num[%d] applied_num[%d].",
		   rkey(r), r->state, __func__, ab->expect_num, ab->applied_num);

	if (ab->expect_num == ab->applied_num) {
		raft_free(ab);
		if (pi.permit) {
			replicationApplyLeaderCb(r, pi);
		} else {
			replicationApplyFollowerCb(r, extra);
		}
	}
}

static void applyCommandCb(struct raft_fsm_apply *req,
						   void *result,
						   int status)
{
	struct applyLog *request = req->data;
	struct raft_apply *req1;
	struct raft *r = request->raft;
	raft_index index = request->index;

	assert((r->last_applied + 1) == index);

	req1 = (struct raft_apply *)getRequest(r, index, RAFT_COMMAND);
	if (req1 != NULL && req1->cb != NULL) {
		ZSINFO(gzlog, "[raft][%d][%d] |usr-req-key-2|%d-%lld|.",
			   rkey(r), r->state, rkey(r), index);
		req1->cb(req1, status, result);
	}

	r->last_applied = max(index, r->last_applied);
	request->ab->applied_num++;

	applySectionCallbackCheck(r, request->ab, request->pi, request->extra);

	raft_free(request);
}
#endif

/* Apply a RAFT_COMMAND entry that has been committed. */
static int applyCommand(struct raft *r,
						const raft_index index,
						const struct raft_buffer *buf,
						struct pgrep_permit_info pi,
						struct applyBatch *ab,
						void *extra)
{
	int rv;
#if defined(RAFT_ASYNC_APPLY) && RAFT_ASYNC_APPLY
	struct applyLog *request;
	struct appendFollower *req_af = extra;

	request = raft_malloc(sizeof*request);

	if (request == NULL) {
		rv = RAFT_NOMEM;
		goto err;
	}

	struct copy_chunk_posi bd = r->io->pgrep_boundary(r->io);

	request->raft = r;
	request->index = index;
	request->ab = ab;
	request->pi = pi;
	request->req.permit = false;
	request->req.obj_id = bd.obj_id;
	request->req.chunk_id = bd.chunk_id;
	request->extra = extra;
	request->req.data = request;

	if (extra)
		request->req.permit = req_af->args.pi.permit;

	ZSINFO(gzlog, "[raft][%d][%d][%s]: permit[%d] skip apply[%d] boundary[%ld][%d].",
		   rkey(r), r->state, __func__, pi.permit, request->req.permit,
		   request->req.obj_id, request->req.chunk_id);

	rv = r->fsm->apply(r->fsm,
					   &request->req,
					   buf,
					   applyCommandCb);
	if (rv != 0) {
		goto err_after_alloc;
	}

	return 0;
err_after_alloc:
	raft_free(request);
err:
	assert(rv != 0);
	return rv;
#else
	struct raft_apply *req;
	void *result;

	rv = r->fsm->apply(r->fsm, buf, &result);
	if (rv != 0) {
		return rv;
	}
	req = (struct raft_apply *)getRequest(r, index, RAFT_COMMAND);
	if (req != NULL && req->cb != NULL) {
		req->cb(req, 0, result);
	}

	return 0;
#endif
}

/* Fire the callback of a barrier request whose entry has been committed. */
static void applyBarrier(struct raft *r, const raft_index index)
{
	struct raft_barrier *req;
	req = (struct raft_barrier *)getRequest(r, index, RAFT_BARRIER);
	if (req != NULL && req->cb != NULL) {
		ZSINFO(gzlog, "[raft][%d][%d] |usr-req-key-2|%d-%lld|.",
			   rkey(r), r->state, rkey(r), index);
		req->cb(req, 0);
	}
}

/* Apply a RAFT_CHANGE entry that has been committed. */
static void applyChange(struct raft *r, const raft_index index)
{
	struct raft_change *req;
	const struct raft_server *server;

	assert(index > 0);

	/* If this is an uncommitted configuration that we had already applied when
	 * submitting the configuration change (for leaders) or upon receiving it
	 * via an AppendEntries RPC (for followers), then reset the uncommitted
	 * index, since that uncommitted configuration is now committed. */
	if (r->configuration_uncommitted_index == index) {
		r->configuration_uncommitted_index = 0;
		ZSINFO(gzlog, "[raft][%d][%d][%s] index[%lld] "
			   "set configuration_uncommitted_index = 0.",
			   rkey(r), r->state, __func__, index);
	}

	r->configuration_index = index;

	if (r->state == RAFT_LEADER) {
		req = r->leader_state.change;
		r->leader_state.change = NULL;

		/* If we are leader but not part of this new configuration, step
		 * down.
		 *
		 * From Section 4.2.2:
		 *
		 *   In this approach, a leader that is removed from the configuration
		 *   steps down once the Cnew entry is committed.
		 */
		server = configurationGet(&r->configuration, r->id);
		if (server == NULL) {
			convertToFollower(r);
			r->removed = true;
		}

		if (req != NULL && req->cb != NULL) {
			ZSINFO(gzlog, "[raft][%d][%d][%s] change call backed.",
				      rkey(r), r->state, __func__);
			req->cb(req, 0);
		}
	} else {
		/* if we are removed from the configuration,
		 * set flag so that the owner could relase the instance
		 */
		server = configurationGet(&r->configuration, r->id);
		if (server == NULL)
			r->removed = true;
	}
}

static bool shouldTakeSnapshot(struct raft *r)
{
	/* If we are shutting down, let's not do anything. */
	if (r->state == RAFT_UNAVAILABLE) {
		return false;
	}

	/* If a snapshot is already in progress, we don't want to start another
	 *  one. */
	if (r->snapshot.pending.term != 0) {
		return false;
	};

	/* If we didn't reach the threshold yet, do nothing. */
	if (r->last_applied - r->log.snapshot.last_index < r->snapshot.threshold) {
		return false;
	}

	/* pgrep: Can not delete log entries after prev_applied_index  */
	unsigned inx = configurationIndexOf(&r->configuration, r->pgrep_id);
	if (r->state == RAFT_LEADER &&
		r->pgrep_id != RAFT_INVALID_ID && inx != r->configuration.n &&
		r->leader_state.progress[inx].prev_applied_index -
		r->log.snapshot.last_index < r->snapshot.threshold) {
		return false;
	}

	return true;
}

static int takeSnapshot(struct raft *r)
{
	struct raft_snapshot *snapshot;
	unsigned i;
	int rv;


	ZSINFO(gzlog, "[raft][%d][%d][%s][conf_dump] take snapshot at %lld.",
		   rkey(r), r->state, __func__, r->last_applied);
	for (i = 0; i < r->configuration.n; i++) {
		const struct raft_server *servert = &r->configuration.servers[i];
		ZSINFO(gzlog, "[raft][%d][%d][%s][conf_dump] i[%d] id[%lld] role[%d] pre_role[%d]",
			   rkey(r), r->state, __func__, i,
			   servert->id, servert->role, servert->pre_role);
	}

	snapshot = &r->snapshot.pending;
	snapshot->index = r->last_applied;
	snapshot->term = logTermOf(&r->log, r->last_applied);

	rv = configurationCopy(&r->configuration, &snapshot->configuration);
	if (rv != 0) {
		goto abort;
	}

	snapshot->configuration_index = r->configuration_index;

	rv = r->fsm->snapshot(r->fsm, &snapshot->bufs, &snapshot->n_bufs);
	if (rv != 0) {
		/* Ignore transient errors. We'll retry next time. */
		if (rv == RAFT_BUSY) {
			rv = 0;
		}
		goto abort_after_config_copy;
	}

	assert(r->snapshot.put.data == NULL);
	r->snapshot.put.data = r;
	rv = r->io->snapshot_put(r->io, r->snapshot.trailing, &r->snapshot.put,
							 snapshot, takeSnapshotCb);
	if (rv != 0) {
		goto abort_after_fsm_snapshot;
	}

	return 0;

abort_after_fsm_snapshot:
	for (i = 0; i < snapshot->n_bufs; i++) {
		raft_free(snapshot->bufs[i].base);
	}
	raft_free(snapshot->bufs);
abort_after_config_copy:
	raft_configuration_close(&snapshot->configuration);
abort:
	r->snapshot.pending.term = 0;
	return rv;
}

int replicationApply(struct raft *r)
{
	struct pgrep_permit_info pi = {0};
	return replicationApplyInner(r, NULL, pi);
}

int replicationApplyReq(struct raft *r, void *extra)
{
	struct pgrep_permit_info pi = {0};
	return replicationApplyInner(r, extra, pi);
}

int replicationApplyPi(struct raft *r, struct pgrep_permit_info pi)
{
	return replicationApplyInner(r, NULL, pi);
}

int replicationApplyInner(struct raft *r, void *extra, struct pgrep_permit_info pi)
{
	struct appendFollower *request = extra;
	raft_index index;
	int rv = 0;

	if (request)
		assert(request->args.pi.replicating);


	assert(r->state == RAFT_LEADER || r->state == RAFT_FOLLOWER);
	assert(r->last_applied <= r->commit_index);

	/* Pgrep:
	 *
	 * Ask permit from pgrep. Cant't apply logs if permit not granted.
	 * If there is a gap between prev_applied_index and last_applied_index.
	 * Start a replicationProgress.
	 */
	if (r->state == RAFT_LEADER && !pi.permit) {
		r->io->pgrep_raft_permit(r->io, &pi);
		if (!pi.permit) {
			ZSINFO(gzlog, "[raft][%d][%d][%s]: pgrep permit not granted r->commit_index[%lld].",
				   rkey(r), r->state, __func__, r->commit_index);
			goto pgrep_fail;
		}

		ZSINFO(gzlog, "[raft][%d][%d][%s]: pgrep permit granted r->commit_index[%lld].",
			   rkey(r), r->state, __func__, r->commit_index);
	}

	if (r->last_applying == r->commit_index ||
		r->last_applied == r->commit_index) {
		if (pi.permit) {
			r->io->pgrep_raft_unpermit(r->io, &pi);
			ZSINFO(gzlog, "[raft][%d][%d][%s]: pgrep permit released because no logs need apply.",
				   rkey(r), r->state, __func__);
		}
		goto pgrep_fail;
	}

	ZSINFO(gzlog, "[raft][%d][%d][%s]: start applying permit[%d] "
		   "r->commit_index[%lld] last_applied[%lld] last_applying[%lld].",
		   rkey(r), r->state, __func__, pi.permit, r->commit_index,
		   r->last_applied, r->last_applying);

	struct applyBatch *ab = raft_malloc(sizeof(struct applyBatch));
	if (!ab)
		goto pgrep_fail;

	/* When pgreping, apply smaller batch of entries. */
	raft_index to_commit_index = min(r->commit_index, r->last_applying + 8);
	if (r->pgrep_id == RAFT_INVALID_ID)
		to_commit_index = r->commit_index;

	ab->expect_num = (int)(to_commit_index - r->last_applying);
	ab->applied_num = 0;

	for (index = r->last_applying + 1; index <= to_commit_index; index++) {
		const struct raft_entry *entry = logGet(&r->log, index);

		assert(entry->type == RAFT_COMMAND || entry->type == RAFT_BARRIER ||
			   entry->type == RAFT_CHANGE);

		ZSINFO(gzlog, "[raft][%d][%d]apply entry, type[%d] index[%lld].",
			   rkey(r), r->state, entry->type, index);

		switch (entry->type) {
		case RAFT_COMMAND:
			rv = applyCommand(r, index, &entry->buf, pi, ab, request);
			break;
		case RAFT_BARRIER:
			if (r->last_applying > r->last_applied)
				goto pgrep_fail;
			applyBarrier(r, index);
			rv = 0;
			r->last_applied = max(index, r->last_applied);
			ab->applied_num++;
			applySectionCallbackCheck(r, ab, pi, request);
			break;
		case RAFT_CHANGE:
			if (r->last_applying > r->last_applied)
				goto pgrep_fail;
			applyChange(r, index);
			rv = 0;
			r->last_applied = max(index, r->last_applied);
			ab->applied_num++;
			applySectionCallbackCheck(r, ab, pi, request);
			break;
		default:
			rv = 0; /* For coverity. This case can't be taken. */
			break;
		}

		if (rv != 0) {
			raft_free(ab);
			break;
		}

		r->last_applying = index;
		ZSINFO(gzlog, "[raft][%d][%d][%s] update last_applying[%lld].",
			   rkey(r), r->state, __func__, r->last_applying);
	}

//out:

	if (rv == 0 && r->last_applying == r->last_applied &&
		shouldTakeSnapshot(r)) {
		rv = takeSnapshot(r);
	}

	return rv;

pgrep_fail:
	if (request) {
		logRelease(&r->log, request->index, request->args.entries,
				   request->args.n_entries);
		raft_free(request);
	}
	return 0;
}

void replicationQuorum(struct raft *r, const raft_index index)
{
	size_t votes = 0;
	size_t i;

	assert(r->state == RAFT_LEADER);

	if (index <= r->commit_index) {
		return;
	}

	/* TODO: fuzzy-test --seed 0x8db5fccc replication/entries/partitioned
	 * fails the assertion below. */
	if (logTermOf(&r->log, index) == 0) {
		return;
	}
	// assert(logTermOf(&r->log, index) > 0);
	assert(logTermOf(&r->log, index) <= r->current_term);

	for (i = 0; i < r->configuration.n; i++) {
		struct raft_server *server = &r->configuration.servers[i];
		if (server->role != RAFT_VOTER) {
			continue;
		}
		if (r->leader_state.progress[i].match_index >= index) {
			votes++;
		}
	}

	if (votes > configurationVoterCount(&r->configuration) / 2) {
		r->commit_index = min(index, r->last_stored);
		tracef("new commit index %llu", r->commit_index);
	}

	return;
}

#undef tracef
