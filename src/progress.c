#include "progress.h"

#include "assert.h"
#include "configuration.h"
#include "log.h"
#include "tracing.h"
#include "event.h"

#ifdef ENABLE_TRACE
#define tracef(...) Tracef(r->tracer, __VA_ARGS__)
#else
#define tracef(...)
#endif

#ifndef max
#define max(a, b) ((a) < (b) ? (b) : (a))
#endif

#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif

/* Initialize a single progress object. */
static void initProgress(struct raft_progress *p, raft_index last_index)
{
    p->next_index = last_index + 1;
    p->match_index = 0;
    p->snapshot_index = 0;
    p->last_send = 0;
    p->snapshot_last_send = 0;
    p->recent_recv = false;
    p->state = PROGRESS__PROBE;
}

int progressBuildArray(struct raft *r)
{
    struct raft_progress *progress;
    unsigned i;
    raft_index last_index = logLastIndex(&r->log);
    progress = raft_malloc(r->configuration.n * sizeof *progress);
    if (progress == NULL) {
        return RAFT_NOMEM;
    }
    for (i = 0; i < r->configuration.n; i++) {
        initProgress(&progress[i], last_index);
	progress[i].recent_recv_time = r->io->time(r->io);
        if (r->configuration.servers[i].id == r->id) {
            progress[i].match_index = r->last_stored;
        }
    }
    r->leader_state.progress = progress;
    r->leader_state.min_match_index = 0;
    return 0;
}

int progressRebuildArray(struct raft *r,
                         const struct raft_configuration *configuration)
{
    raft_index last_index = logLastIndex(&r->log);
    struct raft_progress *progress;
    unsigned i;
    unsigned j;
    raft_id id;

    progress = raft_malloc(configuration->n * sizeof *progress);
    if (progress == NULL) {
        evtErrf("%s", "malloc");
        return RAFT_NOMEM;
    }

    /* First copy the progress information for the servers that exists both in
     * the current and in the new configuration. */
    for (i = 0; i < r->configuration.n; i++) {
        id = r->configuration.servers[i].id;
        j = configurationIndexOf(configuration, id);
        if (j == configuration->n) {
            /* This server is not present in the new configuration, so we just
             * skip it. */
            continue;
        }
        progress[j] = r->leader_state.progress[i];
    }

    /* Then reset the replication state for servers that are present in the new
     * configuration, but not in the current one. */
    for (i = 0; i < configuration->n; i++) {
        id = configuration->servers[i].id;
        j = configurationIndexOf(&r->configuration, id);
        if (j < r->configuration.n) {
            /* This server is present both in the new and in the current
             * configuration, so we have already copied its next/match index
             * value in the loop above. */
            continue;
        }
        assert(j == r->configuration.n);
        initProgress(&progress[i], last_index);
	progress[i].recent_recv_time = r->io->time(r->io);
    }

    raft_free(r->leader_state.progress);
    r->leader_state.progress = progress;

    return 0;
}

bool progressIsUpToDate(struct raft *r, unsigned i)
{
    struct raft_progress *p = &r->leader_state.progress[i];
    raft_index last_index = logLastIndex(&r->log);
    return p->next_index == last_index + 1;
}

static bool progressShouldPipeMore(struct raft *r, unsigned i)
{
	unsigned long long size;

	if (r->inflight_log_threshold == 0)
		return true;

	if (progressNextIndex(r, i) <= progressMatchIndex(r, i))
		return true;

	size = progressNextIndex(r, i) - progressMatchIndex(r, i) - 1;
	return size < r->inflight_log_threshold;
}

bool progressShouldReplicate(struct raft *r, unsigned i)
{
    struct raft_progress *p = &r->leader_state.progress[i];
    raft_time now = r->io->time(r->io);
    bool needs_heartbeat = now - p->last_send >= r->heartbeat_timeout;
    raft_index last_index = logLastIndex(&r->log);
    bool result = false;

    /* We must be in a valid state. */
    assert(p->state == PROGRESS__PROBE || p->state == PROGRESS__PIPELINE ||
           p->state == PROGRESS__SNAPSHOT);

    /* The next index to send must be lower than the highest index in our
     * log. */
    assert(p->next_index <= last_index + 1);

    switch (p->state) {
        case PROGRESS__SNAPSHOT:
            /* Snapshot timed out, move to PROBE */
            if (now - p->snapshot_last_send >= r->install_snapshot_timeout) {
                result = true;
                progressAbortSnapshot(r, i);
            } else {
                /* Enforce Leadership during follower Snapshot installation */
                result = needs_heartbeat;
            }
            break;
        case PROGRESS__PROBE:
            /* We send at most one message per heartbeat interval. */
            result = needs_heartbeat;
            break;
        case PROGRESS__PIPELINE:
            /* In replication mode we send empty append entries messages only if
             * haven't sent anything in the last heartbeat interval. */
            result = (!progressIsUpToDate(r, i) && progressShouldPipeMore(r, i))
		    || needs_heartbeat;
            break;
    }
    return result;
}

raft_index progressNextIndex(struct raft *r, unsigned i)
{
    return r->leader_state.progress[i].next_index;
}

raft_index progressMatchIndex(struct raft *r, unsigned i)
{
    return r->leader_state.progress[i].match_index;
}

void progressUpdateLastSend(struct raft *r, unsigned i)
{
    r->leader_state.progress[i].last_send = r->io->time(r->io);
}

void progressUpdateSnapshotLastSend(struct raft *r, unsigned i)
{
    r->leader_state.progress[i].snapshot_last_send = r->io->time(r->io);
}

bool progressResetRecentRecv(struct raft *r, const unsigned i)
{
    bool prev = r->leader_state.progress[i].recent_recv;
    r->leader_state.progress[i].recent_recv = false;
    return prev;
}

void progressMarkRecentRecv(struct raft *r, const unsigned i)
{
    r->leader_state.progress[i].recent_recv = true;
    r->leader_state.progress[i].recent_recv_time = r->io->time(r->io);
}

bool progressGetRecentRecv(const struct raft *r, const unsigned i)
{
    return r->leader_state.progress[i].recent_recv;
}

void progressToSnapshot(struct raft *r, unsigned i)
{
    struct raft_progress *p = &r->leader_state.progress[i];
    p->state = PROGRESS__SNAPSHOT;
    p->snapshot_index = logSnapshotIndex(&r->log);
}

void progressAbortSnapshot(struct raft *r, const unsigned i)
{
    struct raft_progress *p = &r->leader_state.progress[i];
    p->snapshot_index = 0;
    p->state = PROGRESS__PROBE;
}

int progressState(struct raft *r, const unsigned i)
{
    struct raft_progress *p = &r->leader_state.progress[i];
    return p->state;
}

bool progressMaybeDecrement(struct raft *r,
                            const unsigned i,
                            raft_index rejected,
                            raft_index last_index)
{
    assert(i < r->configuration.n);
    raft_id id = r->configuration.servers[i].id;
    struct raft_progress *p = &r->leader_state.progress[i];

    assert(p->state == PROGRESS__PROBE || p->state == PROGRESS__PIPELINE ||
           p->state == PROGRESS__SNAPSHOT);

    evtNoticef("raft(%llx) %llx progress %u %llu %llu %llu %llu %llu %d",
	       r->id, id, p->state, p->next_index, p->match_index,
	       p->snapshot_index, p->last_send, p->snapshot_last_send,
	       p->recent_recv);
    if (p->state == PROGRESS__SNAPSHOT) {
        /* The rejection must be stale or spurious if the rejected index does
         * not match the last snapshot index. */
        if (rejected != p->snapshot_index) {
            evtWarnf("raft(%llx) %llx rejected %lu diff snapshot index %llu",
		     r->id, id, rejected, p->snapshot_index);
            return false;
        }
        progressAbortSnapshot(r, i);
        evtNoticef("raft(%llx) %llx abort snapshot", r->id, id);
        return true;
    }

    if (p->state == PROGRESS__PIPELINE) {
        /* The rejection must be stale if the rejected index is smaller than
         * the matched one. */
        if (rejected <= p->match_index) {
            tracef("match index is up to date -> ignore ");
            evtNoticef("raft(%llx) %llx reject %lld <= match %lld", r->id, id,
		       rejected, p->match_index);
            if (last_index == 1) {
                initProgress(p, logLastIndex(&r->log));
                evtWarnf("raft(%llx) %llx start over", r->id, id);
            }
            return false;
        }
        /* Directly decrease next to match + 1 */
        p->next_index = min(rejected, p->match_index + 1);
        progressToProbe(r, i);
        evtNoticef("raft(%llx) %llx to probe next_index %llu",
		   r->id, id, p->next_index);
        return true;
    }

    /* The rejection must be stale or spurious if the rejected index does not
     * match the next index minus one. */
    if (rejected != p->next_index - 1) {
        tracef("rejected index %llu different from next index %lld -> ignore ",
               rejected, p->next_index);
	evtWarnf("raft(%llx) %llx rejected %lu diff next index %llu",
		     r->id, id, rejected, p->next_index);
        return false;
    }

    p->next_index = min(rejected, last_index + 1);
    evtNoticef("raft(%llx) %llx set next_index %llu", r->id, id, p->next_index);
    return true;
}

void progressOptimisticNextIndex(struct raft *r,
                                 unsigned i,
                                 raft_index next_index)
{
    struct raft_progress *p = &r->leader_state.progress[i];
    p->next_index = next_index;
}

bool progressMaybeUpdate(struct raft *r, unsigned i, raft_index last_index)
{
    struct raft_progress *p = &r->leader_state.progress[i];
    bool updated = false;
    if (p->match_index < last_index) {
        p->match_index = last_index;
        updated = true;
    }
    if (p->next_index < last_index + 1) {
        p->next_index = last_index + 1;
    }

    return updated;
}

void progressToProbe(struct raft *r, const unsigned i)
{
    struct raft_progress *p = &r->leader_state.progress[i];

    /* If the current state is snapshot, we know that the pending snapshot has
     * been sent to this peer successfully, so we probe from snapshot_index +
     * 1.*/
    if (p->state == PROGRESS__SNAPSHOT) {
        assert(p->snapshot_index > 0);
        p->next_index = max(p->match_index + 1, p->snapshot_index);
        p->snapshot_index = 0;
    } else {
        p->next_index = p->match_index + 1;
    }
    p->state = PROGRESS__PROBE;
}

void progressToPipeline(struct raft *r, const unsigned i)
{
    struct raft_progress *p = &r->leader_state.progress[i];
    p->state = PROGRESS__PIPELINE;
}

bool progressSnapshotDone(struct raft *r, const unsigned i)
{
    struct raft_progress *p = &r->leader_state.progress[i];
    assert(p->state == PROGRESS__SNAPSHOT);
    return p->match_index >= p->snapshot_index;
}

void progressUpdateMinMatch(struct raft *r)
{
	assert(r->state == RAFT_LEADER);
	unsigned i;
	struct raft_server *s;
	struct raft_progress *p;
	raft_index tmp = logLastIndex(&r->log);
	raft_id id = 0;

	assert(r->sync_replication);
	for (i = 0; i < r->configuration.n; ++i) {
		s = &r->configuration.servers[i];
		if (configurationIsSpare(&r->configuration, s, s->group) &&
			s->id != r->leader_state.promotee_id) {
			continue;
		}
		p = &r->leader_state.progress[i];
		if (p->match_index <= tmp) {
			tmp = p->match_index;
			id = s->id;
		}
	}
	r->leader_state.min_match_index = tmp;
	r->leader_state.slowest_replica_id = id;
}

#undef tracef
