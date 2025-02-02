#include "../include/raft.h"
#include "assert.h"
#include "configuration.h"
#include "err.h"
#include "log.h"
#include "membership.h"
#include "progress.h"
#include "queue.h"
#include "replication.h"
#include "request.h"
#include "tracing.h"
#include "event.h"
#include "hook.h"

#ifdef ENABLE_TRACE
#define tracef(...) Tracef(r->tracer, __VA_ARGS__)
#else
#define tracef(...)
#endif

int raft_apply(struct raft *r,
               struct raft_apply *req,
               const struct raft_buffer bufs[],
               const unsigned n,
               raft_apply_cb cb)
{
    const struct raft_entry *entry;
    raft_index index;
    unsigned i;
    int rv;

    assert(r != NULL);
    assert(bufs != NULL);
    assert(n > 0);

    if (r->state != RAFT_LEADER || r->transfer != NULL
        || r->leader_state.removed_from_cluster) {
        rv = RAFT_NOTLEADER;
        ErrMsgFromCode(r->errmsg, rv);
	    evtErrf("E-1528-073", "raft(%llx) apply failed %d", r->id, rv);
        goto err;
    }

    /* Index of the first entry being appended. */
    index = logLastIndex(&r->log) + 1;
    tracef("%u commands starting at %lld", n, index);
    req->time = r->io->time(r->io);
    req->type = RAFT_COMMAND;
    req->index = index;
    req->cb = cb;

    /* Append the new entries to the log. */
    rv = logAppendCommands(&r->log, r->current_term, bufs, n);
    if (rv != 0) {
        evtErrf("E-1528-074", "raft(%llx) append cmd failed %d", r->id, rv);
        goto err;
    }

    rv = requestRegEnqueue(&r->leader_state.reg, (struct request *) req);
    if (rv != 0) {
        evtErrf("E-1528-075", "raft(%llx) append to registry failed %d", r->id, rv);
        goto err_after_log_append;
    }
    hookRequestAccept(r, index);

    for (i = 0; i < n; ++i) {
        entry = logGet(&r->log, index + i);
        assert(entry);
        assert(entry->type == RAFT_COMMAND);
        r->hook->entry_after_append_fn(r->hook, index + i, entry);
    }

    rv = replicationTrigger(r, index);
    if (rv != 0) {
        evtErrf("E-1528-076", "raft(%llx) replication trigger failed %d", r->id, rv);
        goto err_after_reg_append;
    }

    return 0;

err_after_reg_append:
	requestRegDel(&r->leader_state.reg, index);
err_after_log_append:
    logDiscard(&r->log, index);
err:
    assert(rv != 0);
    return rv;
}

int raft_barrier(struct raft *r, struct raft_barrier *req, raft_barrier_cb cb)
{
    raft_index index;
    struct raft_buffer buf;
    const struct raft_entry *entry;
    int rv;

    if (r->state != RAFT_LEADER || r->transfer != NULL) {
        rv = RAFT_NOTLEADER;
        evtErrf("E-1528-077", "raft(%llx) apply barrier failed %d", r->id, rv);
        goto err;
    }

    /* TODO: use a completely empty buffer */
    buf.len = 0;
    buf.base = NULL;

    /* Index of the barrier entry being appended. */
    index = logLastIndex(&r->log) + 1;
    tracef("barrier starting at %lld", index);
    req->time = r->io->time(r->io);
    req->type = RAFT_BARRIER;
    req->index = index;
    req->cb = cb;

    rv = logAppend(&r->log, r->current_term, RAFT_BARRIER, &buf, NULL);
    if (rv != 0) {
        evtErrf("E-1528-078", "raft(%llx) append barrier failed %d", r->id, rv);
        goto err_after_buf_alloc;
    }

    rv = requestRegEnqueue(&r->leader_state.reg, (struct request *) req);
    if (rv != 0) {
	    evtErrf("E-1528-079", "raft(%llx) append to registry failed %d", r->id, rv);
	    goto err_after_log_append;
    }
    hookRequestAccept(r, index);

    entry = logGet(&r->log, index);
    assert(entry);
    assert(entry->type == RAFT_BARRIER);
    r->hook->entry_after_append_fn(r->hook, index, entry);

    rv = replicationTrigger(r, index);
    if (rv != 0) {
        evtErrf("E-1528-080", "raft(%llx) replication trigger failed %d", r->id, rv);
        goto err_after_reg_append;
    }

    return 0;

err_after_reg_append:
	requestRegDel(&r->leader_state.reg, index);
err_after_log_append:
    logDiscard(&r->log, index);
err_after_buf_alloc:
err:
    return rv;
}

static int clientChangeConfiguration(
    struct raft *r,
    struct raft_change *req,
    const struct raft_configuration *configuration)
{
    raft_index index;
    raft_term term = r->current_term;
    int rv;
    const struct raft_entry *entry;
    unsigned server_index;

    /* Index of the entry being appended. */
    index = logLastIndex(&r->log) + 1;

    /* Encode the new configuration and append it to the log. */
    rv = logAppendConfiguration(&r->log, term, configuration);
    if (rv != 0) {
        evtErrf("E-1528-081", "raft(%llx) append conf failed %d", r->id, rv);
        goto err;
    }

    req->index = index;
    evtNoticef("N-1528-007", "raft(%llx) conf append at index %lu", r->id, index);
    evtDumpConfiguration(r, configuration);

    entry = logGet(&r->log, index);
    assert(entry);
    assert(entry->type == RAFT_CHANGE);
    r->hook->entry_after_append_fn(r->hook, index, entry);
    hookConfChange(r, configuration);

    rv = progressRebuildArray(r, configuration);
    if (rv != 0) {
        evtErrf("E-1528-082", "raft(%llx) rebuild array failed %d", r->id, rv);
        goto err;
    }

    /* Update the current configuration if we've created a new object. */
    if (configuration != &r->configuration) {
        raft_configuration_close(&r->configuration);
        r->configuration = *configuration;
        r->role = RAFT_STANDBY;
        if (configurationIndexOf(&r->configuration, r->id)
            != r->configuration.n) {
            r->role = configurationServerRole(&r->configuration, r->id);
        }
    }

    /* Start writing the new log entry to disk and send it to the followers. */
    rv = replicationTrigger(r, index);
    if (rv != 0) {
        evtErrf("E-1528-083", "raft(%llx) replication trigger failed %d", r->id, rv);
        /* TODO: restore the old next/match indexes and configuration. */
        goto err_after_log_append;
    }

    r->configuration_uncommitted_index = index;
    server_index = configurationIndexOf(&r->configuration, r->id);
    if (r->state == RAFT_LEADER && server_index == r->configuration.n) {
        r->leader_state.removed_from_cluster = true;
    }

    return 0;

err_after_log_append:
    logTruncate(&r->log, index);

err:
    assert(rv != 0);
    return rv;
}

int raft_add(struct raft *r,
             struct raft_change *req,
             raft_id id,
             raft_change_cb cb)
{
    struct raft_configuration configuration;
    int rv;

    req->cb_on_match = false;
    req->match_id = 0;
    rv = membershipCanChangeConfiguration(r, false);
    if (rv != 0) {
        evtNoticef("N-1528-008", "raft(%llx) change conf failed %d", r->id, rv);
        return rv;
    }

    tracef("add server: id %llu", id);

    /* Make a copy of the current configuration, and add the new server to
     * it. */
    rv = configurationCopy(&r->configuration, &configuration);
    if (rv != 0) {
        evtErrf("E-1528-084", "raft(%llx) copy conf failed %d", r->id, rv);
        goto err;
    }

    rv = raft_configuration_add(&configuration, id, RAFT_SPARE);
    if (rv != 0) {
        evtErrf("E-1528-085", "raft(%llx) add conf failed %d", r->id, rv);
        goto err_after_configuration_copy;
    }
    req->cb = cb;
    req->time = r->io->time(r->io);

    rv = clientChangeConfiguration(r, req, &configuration);
    if (rv != 0) {
        evtErrf("E-1528-086", "raft(%llx) change conf failed %d", r->id, rv);
        goto err_after_configuration_copy;
    }

    assert(r->leader_state.change == NULL);
    r->leader_state.change = req;

    return 0;

err_after_configuration_copy:
    raft_configuration_close(&configuration);
err:
    assert(rv != 0);
    return rv;
}

int raft_joint_promote(struct raft *r,
                       struct raft_change *req,
                       raft_id id,
                       int role,
				       raft_id remove,
                       raft_change_cb cb)
{
    const struct raft_server *server;
    unsigned server_index;
    raft_index last_index;
    int rv;

    if (role != RAFT_VOTER && role != RAFT_LOGGER) {
        rv = RAFT_BADROLE;
        ErrMsgFromCode(r->errmsg, rv);
        evtErrf("E-1528-087", "raft(%llx) promote role %d failed", r->id, role, rv);
        return rv;
    }

    server = configurationGet(&r->configuration, remove);
    if (server == NULL) {
        rv = RAFT_NOTFOUND;
        ErrMsgPrintf(r->errmsg, "no server has ID %llu", remove);
        evtErrf("E-1528-088", "raft(%llx) has no server id %llx failed", r->id, remove, rv);
        goto err;
    }

    rv = membershipCanChangeConfiguration(r, false);
    if (rv != 0) {
        evtNoticef("N-1528-009", "raft(%llx) change conf failed %d", r->id, rv);
        return rv;
    }

    server = configurationGet(&r->configuration, id);
    if (server == NULL) {
        rv = RAFT_NOTFOUND;
        ErrMsgPrintf(r->errmsg, "no server has ID %llu", id);
        evtErrf("E-1528-089", "raft(%llx) has no server id %llx failed", r->id, id, rv);
        goto err;
    }

    /* Check if we have already the desired role. */
    if (server->role == role) {
        const char *name = configurationRoleName(role);
        rv = RAFT_BADROLE;
        ErrMsgPrintf(r->errmsg, "server is already %s", name);
        evtWarnf("W-1528-062", "raft(%llx) server %llx is already %s", r->id, server->id, name);
        goto err;
    }

    server_index = configurationIndexOf(&r->configuration, id);
    assert(server_index < r->configuration.n);

    last_index = logLastIndex(&r->log);

    req->cb = cb;
    req->time = r->io->time(r->io);

    assert(r->leader_state.change == NULL);
    r->leader_state.change = req;

    /* If we are not promoting to the voter role or if the log of this server is
     * already up-to-date, we can submit the configuration change
     * immediately. */
    if (progressMatchIndex(r, server_index) == last_index) {
        configurationJointRemove(&r->configuration, remove);
        r->configuration.servers[server_index].role_new = role;

        rv = clientChangeConfiguration(r, req, &r->configuration);
        if (rv != 0) {
            configurationJointReset(&r->configuration);
            evtErrf("E-1528-090", "raft(%llx) change conf failed %d", r->id, rv);
            return rv;
        }

        return 0;
    }

    r->leader_state.promotee_id = server->id;
    r->leader_state.remove_id   = remove;
    r->leader_state.promotee_role = role;


    /* Initialize the first catch-up round. */
    r->leader_state.round_number = 1;
    r->leader_state.round_index = last_index;
    r->leader_state.round_start = r->io->time(r->io);
    evtNoticef("N-1528-010", "raft(%llx) promotee %llx round %u round_index %llu", r->id,
	       r->leader_state.promotee_id, r->leader_state.round_number,
	       r->leader_state.round_index);

    /* Immediately initiate an AppendEntries request. */
    rv = replicationProgress(r, server_index);
    if (rv != 0 && rv != RAFT_NOCONNECTION) {
        /* This error is not fatal. */
        tracef("failed to send append entries to server %llu: %s (%d)",
               server->id, raft_strerror(rv), rv);
        evtErrf("E-1528-091", "raft(%llx) replication progress failed %d", r->id, rv);
    }
    return 0;
err:
    assert(rv != 0);
    return rv;
}

int raft_dup(struct raft *r, struct raft_change *req, raft_change_cb cb)
{
	struct raft_configuration configuration;
	int rv;

	rv = membershipCanChangeConfiguration(r, false);
	if (rv != 0) {
		return rv;
	}

	tracef("dup configuration");

	/* Make a copy of the current configuration. */
	rv = configurationCopy(&r->configuration, &configuration);
	if (rv != 0) {
		goto err;
	}

	req->cb = cb;
    req->time = r->io->time(r->io);

	rv = clientChangeConfiguration(r, req, &configuration);
	if (rv != 0) {
		goto err_after_configuration_copy;
	}

	assert(r->leader_state.change == NULL);
	r->leader_state.change = req;

	return 0;
err_after_configuration_copy:
	raft_configuration_close(&configuration);
err:
	assert(rv != 0);
	return rv;
}

int raft_assign(struct raft *r,
                struct raft_change *req,
                raft_id id,
                int role,
                raft_change_cb cb)
{
    const struct raft_server *server;
    unsigned server_index;
    raft_index last_index;
    int rv;

    if (role != RAFT_STANDBY && role != RAFT_VOTER && role != RAFT_SPARE && role != RAFT_LOGGER) {
        rv = RAFT_BADROLE;
        ErrMsgFromCode(r->errmsg, rv);
        evtErrf("E-1528-092", "raft(%llx) assign role %d failed", r->id, role, rv);
        return rv;
    }

    rv = membershipCanChangeConfiguration(r, false);
    if (rv != 0) {
        evtNoticef("N-1528-011", "raft(%llx) change conf failed %d", r->id, rv);
        return rv;
    }

    server = configurationGet(&r->configuration, id);
    if (server == NULL) {
        rv = RAFT_NOTFOUND;
        ErrMsgPrintf(r->errmsg, "no server has ID %llu", id);
        evtErrf("E-1528-093", "raft(%llx) has no server id %llx failed", r->id, id, rv);
        goto err;
    }

    /* Check if we have already the desired role. */
    if (server->role == role) {
        const char *name = configurationRoleName(role);
        rv = RAFT_BADROLE;
        ErrMsgPrintf(r->errmsg, "server is already %s", name);
        evtWarnf("W-1528-063", "raft(%llx) server %llx is already %s", r->id, server->id, name);
        goto err;
    }

    server_index = configurationIndexOf(&r->configuration, id);
    assert(server_index < r->configuration.n);

    last_index = logLastIndex(&r->log);

    req->cb = cb;
    req->time = r->io->time(r->io);

    assert(r->leader_state.change == NULL);
    r->leader_state.change = req;

    /* If we are not promoting to the voter role or if the log of this server is
     * already up-to-date, we can submit the configuration change
     * immediately. */
    if ((role != RAFT_VOTER && role != RAFT_LOGGER) ||
        progressMatchIndex(r, server_index) == last_index) {
        int old_role = r->configuration.servers[server_index].role;
        r->configuration.servers[server_index].role = role;

        rv = clientChangeConfiguration(r, req, &r->configuration);
        if (rv != 0) {
            r->configuration.servers[server_index].role = old_role;
            evtErrf("E-1528-094", "raft(%llx) change conf failed %d", r->id, rv);
            return rv;
        }

        return 0;
    }

    assert(r->leader_state.remove_id == 0);
    r->leader_state.promotee_id = server->id;
    r->leader_state.promotee_role = role;

    /* Initialize the first catch-up round. */
    r->leader_state.round_number = 1;
    r->leader_state.round_index = last_index;
    r->leader_state.round_start = r->io->time(r->io);
    evtNoticef("N-1528-012", "raft(%llx) promotee %llx round %u round_index %llu", r->id,
	       r->leader_state.promotee_id, r->leader_state.round_number,
	       r->leader_state.round_index);

    /* Immediately initiate an AppendEntries request. */
    rv = replicationProgress(r, server_index);
    if (rv != 0 && rv != RAFT_NOCONNECTION) {
        /* This error is not fatal. */
        tracef("failed to send append entries to server %llu: %s (%d)",
               server->id, raft_strerror(rv), rv);
        evtErrf("E-1528-095", "raft(%llx) replication progress failed %d", r->id, rv);
    }

    return 0;

err:
    assert(rv != 0);
    return rv;
}

static int copyJointRemoveConfiguration(struct raft *r,
                                        struct raft_configuration *c,
                                        raft_id id)
{
    assert(r->configuration.phase == RAFT_CONF_JOINT);
    const struct raft_server *server;
    enum raft_group group;

    server = configurationGet(&r->configuration, id);
    assert(server);

    if (server->group & RAFT_GROUP_NEW)
        group = RAFT_GROUP_OLD;
    else
        group = RAFT_GROUP_NEW;

    return configurationJointToNormal(&r->configuration, c, group);
}

int raft_remove(struct raft *r,
                struct raft_change *req,
                raft_id id,
                raft_change_cb cb)
{
    const struct raft_server *server;
    struct raft_configuration configuration;
    int rv;
    bool joint = r->configuration.phase == RAFT_CONF_JOINT;

    req->cb_on_match = false;
    req->match_id = 0;
    rv = membershipCanChangeConfiguration(r, joint);
    if (rv != 0) {
        evtErrf("E-1528-096", "raft(%llx) change conf failed %d", r->id, rv);
        return rv;
    }

    server = configurationGet(&r->configuration, id);
    if (server == NULL) {
        rv = RAFT_BADID;
        evtErrf("E-1528-097", "raft(%llx) bad id %llx", r->id, id);
        goto err;
    }

    tracef("remove server: id %llu", id);

    if (r->configuration.phase == RAFT_CONF_JOINT) {
        rv = copyJointRemoveConfiguration(r, &configuration, id);
        if (rv != 0) {
            evtErrf("E-1528-098", "raft(%llx) copy joint conf failed %d", r->id, rv);
            goto err;
        }
        configurationRemove(&configuration, id);
    } else {
        /* Make a copy of the current configuration, and remove the given server
        * from it. */
        rv = configurationCopy(&r->configuration, &configuration);
        if (rv != 0) {
            evtErrf("E-1528-099", "raft(%llx) copy conf failed %d", r->id, rv);
            goto err;
        }

        rv = configurationRemove(&configuration, id);
        if (rv != 0) {
            evtErrf("E-1528-100", "raft(%llx) remove %llx from conf failed %d", r->id, id, rv);
            goto err_after_configuration_copy;
        }
    }

    req->cb = cb;
    req->time = r->io->time(r->io);
    rv = clientChangeConfiguration(r, req, &configuration);
    if (rv != 0) {
        evtErrf("E-1528-101", "raft(%llx) change conf failed %d", r->id, rv);
        goto err_after_configuration_copy;
    }

    assert(r->leader_state.change == NULL);
    r->leader_state.change = req;

    return 0;

err_after_configuration_copy:
    raft_configuration_close(&configuration);

err:
    assert(rv != 0);
    return rv;
}

/* Find a suitable voting follower. */
static raft_id clientSelectTransferee(struct raft *r)
{
    const struct raft_server *transferee = NULL;
    unsigned i;

    for (i = 0; i < r->configuration.n; i++) {
        const struct raft_server *server = &r->configuration.servers[i];
        if (server->id == r->id ||
            !configurationIsVoter(&r->configuration, server, RAFT_GROUP_ANY)) {
            continue;
        }
        transferee = server;
        if (progressIsUpToDate(r, i)) {
            break;
        }
    }

    if (transferee != NULL) {
        return transferee->id;
    }

    return 0;
}

int raft_transfer(struct raft *r,
                  struct raft_transfer *req,
                  raft_id id,
                  raft_transfer_cb cb)
{
    const struct raft_server *server;
    unsigned i;
    int rv;

    if (r->state != RAFT_LEADER || r->transfer != NULL) {
        rv = RAFT_NOTLEADER;
        ErrMsgFromCode(r->errmsg, rv);
        evtErrf("E-1528-102", "raft(%llx) transfer %llx failed %d", r->id, id, rv);
        goto err;
    }

    if (id == 0) {
        id = clientSelectTransferee(r);
        if (id == 0) {
            rv = RAFT_NOTFOUND;
            ErrMsgPrintf(r->errmsg, "there's no other voting server");
            evtErrf("E-1528-103", "raft(%llx) select transferee failed %d", r->id, rv);
            goto err;
        }
    }

    server = configurationGet(&r->configuration, id);
    if (server == NULL || server->id == r->id
        || !configurationIsVoter(&r->configuration, server, RAFT_GROUP_ANY)) {
        rv = RAFT_BADID;
        ErrMsgFromCode(r->errmsg, rv);
        evtErrf("E-1528-104", "raft(%llx) get transferee %lu failed %d", r->id,
            server == NULL ? 0 : server->id, rv);
        goto err;
    }

    evtNoticef("N-1528-013", "raft(%llx) transfer leader to %llx role %d %d group %d", r->id,
        server->id, server->role, server->role_new, server->group);

    /* If this follower is up-to-date, we can send it the TimeoutNow message
     * right away. */
    i = configurationIndexOf(&r->configuration, server->id);
    assert(i < r->configuration.n);

    membershipLeadershipTransferInit(r, req, id, cb);

    if (progressIsUpToDate(r, i)) {
        rv = membershipLeadershipTransferStart(r);
        if (rv != 0) {
            r->transfer = NULL;
            evtErrf("E-1528-105", "raft(%llx) transfer to %llx failed %d", r->id, id, rv);
            goto err;
        }
    }

    return 0;

err:
    assert(rv != 0);
    return rv;
}

#undef tracef
