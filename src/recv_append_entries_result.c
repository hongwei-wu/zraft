#include "recv_append_entries_result.h"
#include "assert.h"
#include "configuration.h"
#include "tracing.h"
#include "recv.h"
#include "replication.h"

/* Set to 1 to enable tracing. */
#if 0
#define tracef(...) Tracef(r->tracer, __VA_ARGS__)
#else
#define tracef(...)
#endif

int recvAppendEntriesResult(struct raft *r,
                            const raft_id id,
                            const struct raft_append_entries_result *result)
{
    int match;
    const struct raft_server *server;
    int rv;

    assert(r != NULL);
    assert(id > 0);
    assert(result != NULL);

    if (r->state != RAFT_LEADER) {
        tracef("local server is not leader -> ignore");
        return 0;
    }

    recvCheckMatchingTerms(r, result->term, &match);
    assert(match <= 0);

    if (match < 0) {
        tracef("local term is higher -> ignore ");
        return 0;
    }

    /* If we have stepped down, abort here.
     *
     * From Figure 3.1:
     *
     *   [Rules for Servers] All Servers: If RPC request or response contains
     *   term T > currentTerm: set currentTerm = T, convert to follower.
     */
    if (match > 0) {
        assert(r->state == RAFT_FOLLOWER);
        return 0;
    }

    assert(result->term == r->current_term);

    /* Ignore responses from servers that have been removed */
    server = configurationGet(&r->configuration, id);
    if (server == NULL) {
        tracef("unknown server -> ignore");
        return 0;
    }

    /* Update the progress of this server, possibly sending further entries. */
    rv = replicationUpdate(r, server->id, result);
    if (rv != 0) {
        return rv;
    }

    return 0;
}

#undef tracef
