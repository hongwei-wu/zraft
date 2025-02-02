#include "../../src/byte.h"
#include "../../src/configuration.h"
#include "../lib/heap.h"
#include "../lib/runner.h"

/******************************************************************************
 *
 * Fixture
 *
 *****************************************************************************/

struct fixture
{
    FIXTURE_HEAP;
    struct raft_configuration configuration;
};

static void *setUp(const MunitParameter params[], MUNIT_UNUSED void *user_data)
{
    struct fixture *f = munit_malloc(sizeof *f);
    SET_UP_HEAP;
    configurationInit(&f->configuration);
    return f;
}

static void tearDown(void *data)
{
    struct fixture *f = data;
    configurationClose(&f->configuration);
    TEAR_DOWN_HEAP;
    free(f);
}

/******************************************************************************
 *
 * Helper macros
 *
 *****************************************************************************/

/* Accessors */
#define VOTER_COUNT configurationVoterCount(&f->configuration, RAFT_GROUP_ANY)
#define INDEX_OF(ID) configurationIndexOf(&f->configuration, ID)
#define INDEX_OF_VOTER(ID) configurationIndexOfVoter(&f->configuration, ID)
#define GET(ID) configurationGet(&f->configuration, ID)

/* Add a server to the fixture's configuration. */
#define ADD_RV(ID, ROLE) \
    configurationAdd(&f->configuration, ID, ROLE, ROLE, RAFT_GROUP_OLD)
#define ADD(...) munit_assert_int(ADD_RV(__VA_ARGS__), ==, 0)
#define ADD_ERROR(RV, ...) munit_assert_int(ADD_RV(__VA_ARGS__), ==, RV)

/* Remove a server from the fixture's configuration */
#define REMOVE_RV(ID) configurationRemove(&f->configuration, ID)
#define REMOVE(...) munit_assert_int(REMOVE_RV(__VA_ARGS__), ==, 0)
#define REMOVE_ERROR(RV, ...) munit_assert_int(REMOVE_RV(__VA_ARGS__), ==, RV)

/* Copy the fixture's configuration into the given one. */
#define COPY_RV(CONF) configurationCopy(&f->configuration, CONF)
#define COPY(...) munit_assert_int(COPY_RV(__VA_ARGS__), ==, 0)
#define COPY_ERROR(RV, ...) munit_assert_int(COPY_RV(__VA_ARGS__), ==, RV)

/* Encode the fixture's configuration into the given buffer. */
#define ENCODE_RV(BUF) configurationEncode(&f->configuration, BUF)
#define ENCODE(...) munit_assert_int(ENCODE_RV(__VA_ARGS__), ==, 0)
#define ENCODE_ERROR(RV, ...) munit_assert_int(ENCODE_RV(__VA_ARGS__), ==, RV)

/* Decode the given buffer into the fixture's configuration. */
#define DECODE_RV(BUF) configurationDecode(BUF, &f->configuration)
#define DECODE(...) munit_assert_int(DECODE_RV(__VA_ARGS__), ==, 0)
#define DECODE_ERROR(RV, ...) munit_assert_int(DECODE_RV(__VA_ARGS__), ==, RV)

/******************************************************************************
 *
 * Assertions
 *
 *****************************************************************************/

/* Assert that the fixture's configuration has n servers. */
#define ASSERT_N(N)                                              \
    {                                                            \
        munit_assert_int(f->configuration.n, ==, N);             \
        if (N == 0) {                                            \
            munit_assert_ptr_null(f->configuration.servers);     \
        } else {                                                 \
            munit_assert_ptr_not_null(f->configuration.servers); \
        }                                                        \
    }

/* Assert that the attributes of the I'th server in the fixture's configuration
 * match the given values. */
#define ASSERT_SERVER(I, ID, ROLE, NROLE, GROUP)             \
    {                                                        \
        struct raft_server *server;                          \
        munit_assert_int(I, <, f->configuration.n);          \
        server = &f->configuration.servers[I];               \
        munit_assert_int(server->id, ==, ID);                \
        munit_assert_int(server->role, ==, ROLE);            \
        munit_assert_int(server->role_new, ==, NROLE);       \
        munit_assert_int(server->group, ==, GROUP);          \
    }

/******************************************************************************
 *
 * configurationVoterCount
 *
 *****************************************************************************/

SUITE(configurationVoterCount)

/* All servers are voting. */
TEST(configurationVoterCount, all_voters, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    ADD(1, RAFT_VOTER);
    ADD(2, RAFT_VOTER);
    munit_assert_int(VOTER_COUNT, ==, 2);
    return MUNIT_OK;
}

/* Return only voting servers. */
TEST(configurationVoterCount, filter, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    ADD(1, RAFT_VOTER);
    ADD(2, RAFT_STANDBY);
    munit_assert_int(VOTER_COUNT, ==, 1);
    return MUNIT_OK;
}

/******************************************************************************
 *
 * configurationIndexOf
 *
 *****************************************************************************/

SUITE(configurationIndexOf)

/* If a matching server is found, it's index is returned. */
TEST(configurationIndexOf, match, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    ADD(1, RAFT_VOTER);
    ADD(2, RAFT_STANDBY);
    munit_assert_int(INDEX_OF(2), ==, 1);
    return MUNIT_OK;
}

/* If no matching server is found, the length of the configuration is
 * returned. */
TEST(configurationIndexOf, no_match, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    ADD(1, RAFT_VOTER);
    munit_assert_int(INDEX_OF(3), ==, f->configuration.n);
    return MUNIT_OK;
}

/******************************************************************************
 *
 * configurationIndexOfVoter
 *
 *****************************************************************************/

SUITE(configurationIndexOfVoter)

/* The index of the matching voting server (relative to the number of voting
   servers) is returned. */
TEST(configurationIndexOfVoter, match, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    ADD(1, RAFT_STANDBY);
    ADD(2, RAFT_VOTER);
    ADD(3, RAFT_VOTER);
    munit_assert_int(INDEX_OF_VOTER(3), ==, 1);
    return MUNIT_OK;
}

/* If no matching server is found, the length of the configuration is
 * returned. */
TEST(configurationIndexOfVoter, no_match, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    ADD(1, RAFT_VOTER);
    munit_assert_int(INDEX_OF_VOTER(3), ==, 1);
    return MUNIT_OK;
}

/* If the server exists but is non-voting, the length of the configuration is
 * returned. */
TEST(configurationIndexOfVoter, non_voting, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    ADD(1, RAFT_STANDBY);
    munit_assert_int(INDEX_OF_VOTER(1), ==, 1);
    return MUNIT_OK;
}

/******************************************************************************
 *
 * configurationGet
 *
 *****************************************************************************/

SUITE(configurationGet)

/* If a matching server is found, it's returned. */
TEST(configurationGet, match, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    const struct raft_server *server;
    ADD(1, RAFT_VOTER);
    ADD(2, RAFT_STANDBY);
    server = GET(2);
    munit_assert_ptr_not_null(server);
    munit_assert_int(server->id, ==, 2);
    return MUNIT_OK;
}

/* If no matching server is found, NULL is returned. */
TEST(configurationGet, no_match, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    ADD(1, RAFT_VOTER);
    munit_assert_ptr_null(GET(3));
    return MUNIT_OK;
}

/******************************************************************************
 *
 * configurationCopy
 *
 *****************************************************************************/

SUITE(configurationCopy)

/* Copy a configuration containing two servers */
TEST(configurationCopy, two, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    struct raft_configuration configuration;
    ADD(1, RAFT_STANDBY);
    ADD(2, RAFT_VOTER);
    COPY(&configuration);
    munit_assert_int(configuration.n, ==, 2);
    munit_assert_int(configuration.servers[0].id, ==, 1);
    munit_assert_int(configuration.servers[1].id, ==, 2);
    configurationClose(&configuration);
    return MUNIT_OK;
}

/* Out of memory */
TEST(configurationCopy, oom, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    struct raft_configuration configuration;
    ADD(1, RAFT_STANDBY);
    HeapFaultConfig(&f->heap, 0, 1);
    HeapFaultEnable(&f->heap);
    COPY_ERROR(RAFT_NOMEM, &configuration);
    return MUNIT_OK;
}

/******************************************************************************
 *
 * raft_configuration_add
 *
 *****************************************************************************/

SUITE(configurationAdd)

/* Add a server to the configuration. */
TEST(configurationAdd, one, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    ADD(1, RAFT_VOTER);
    ASSERT_N(1);
    ASSERT_SERVER(0, 1, RAFT_VOTER, RAFT_VOTER, RAFT_GROUP_OLD);
    return MUNIT_OK;
}

/* Add two servers to the configuration. */
TEST(configurationAdd, two, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    ADD(1, RAFT_VOTER);
    ADD(2, RAFT_STANDBY);
    ASSERT_N(2);
    ASSERT_SERVER(0, 1, RAFT_VOTER, RAFT_VOTER, RAFT_GROUP_OLD);
    ASSERT_SERVER(1, 2, RAFT_STANDBY, RAFT_STANDBY, RAFT_GROUP_OLD);
    return MUNIT_OK;
}

/* Add a server with an ID which is already in use. */
TEST(configurationAdd, duplicateId, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    ADD(1, RAFT_VOTER);
    ADD_ERROR(RAFT_DUPLICATEID, 1, RAFT_STANDBY);
    return MUNIT_OK;
}

/* Add a server with an invalid role. */
TEST(configurationAdd, invalidRole, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    ADD_ERROR(RAFT_BADROLE, 2, 666);
    return MUNIT_OK;
}

static char *add_oom_heap_fault_delay[] = {"0", "0", NULL};
static char *add_oom_heap_fault_repeat[] = {"1", NULL};

static MunitParameterEnum add_oom_params[] = {
    {TEST_HEAP_FAULT_DELAY, add_oom_heap_fault_delay},
    {TEST_HEAP_FAULT_REPEAT, add_oom_heap_fault_repeat},
    {NULL, NULL},
};

/* Out of memory. */
TEST(configurationAdd, oom, setUp, tearDown, 0, add_oom_params)
{
    struct fixture *f = data;
    HeapFaultEnable(&f->heap);
    ADD_ERROR(RAFT_NOMEM, 1, RAFT_VOTER);
    return MUNIT_OK;
}

/******************************************************************************
 *
 * configurationRemove
 *
 *****************************************************************************/

SUITE(configurationRemove)

/* Remove the last and only server. */
TEST(configurationRemove, last, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    ADD(1, RAFT_VOTER);
    REMOVE(1);
    ASSERT_N(0);
    return MUNIT_OK;
}

/* Remove the first server. */
TEST(configurationRemove, first, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    ADD(1, RAFT_VOTER);
    ADD(2, RAFT_STANDBY);
    REMOVE(1);
    ASSERT_N(1);
    ASSERT_SERVER(0, 2, RAFT_STANDBY, RAFT_STANDBY, RAFT_GROUP_OLD);
    return MUNIT_OK;
}

/* Remove a server in the middle. */
TEST(configurationRemove, middle, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    ADD(1, RAFT_VOTER);
    ADD(2, RAFT_STANDBY);
    ADD(3, RAFT_VOTER);
    REMOVE(2);
    ASSERT_N(2);
    ASSERT_SERVER(0, 1, RAFT_VOTER, RAFT_VOTER, RAFT_GROUP_OLD);
    ASSERT_SERVER(1, 3, RAFT_VOTER, RAFT_VOTER, RAFT_GROUP_OLD);
    return MUNIT_OK;
}

/* Attempts to remove a server with an unknown ID result in an error. */
TEST(configurationRemove, unknownId, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    REMOVE_ERROR(RAFT_BADID, 1);
    return MUNIT_OK;
}

/* Out of memory. */
TEST(configurationRemove, oom, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    ADD(1, RAFT_VOTER);
    ADD(2, RAFT_STANDBY);
    HeapFaultConfig(&f->heap, 0, 1);
    HeapFaultEnable(&f->heap);
    REMOVE_ERROR(RAFT_NOMEM, 1);
    return MUNIT_OK;
}

/******************************************************************************
 *
 * configurationEncode
 *
 *****************************************************************************/

SUITE(configurationEncode)

/* Encode a configuration with one server. */
TEST(configurationEncode, one_server, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    struct raft_buffer buf;
    size_t len;
    const void *cursor;
    size_t i;
    //const char *address = "127.0.0.1:666";
    ADD(1, RAFT_VOTER);
    ENCODE(&buf);

    len = 1 + 8 +             /* Version and n of servers */
          8 + 1 +             /* Old Id and role */
          256 +               /* Meta */
          8 + 1 + 1 + 1;      /* Server */
    len = bytePad64(len);

    munit_assert_int(buf.len, ==, len);

    cursor = buf.base;

    munit_assert_int(byteGet8(&cursor), ==, 1);
    munit_assert_int(byteGet64Unaligned(&cursor), ==, 1);

    munit_assert_int(byteGet64Unaligned(&cursor), ==, 1);
    munit_assert_int(byteGet8(&cursor), ==, RAFT_VOTER);

    // meta
    munit_assert_int(byteGet32(&cursor), ==, CONF_META_VERSION);
    munit_assert_int(byteGet32(&cursor), ==, CONF_SERVER_VERSION);
    munit_assert_int(byteGet32(&cursor), ==, CONF_SERVER_SIZE);
    munit_assert_int(byteGet8(&cursor), ==, 0);

    // skip reserve
    for (i = 0; i < 243; ++i)
        byteGet8(&cursor);

    munit_assert_int(byteGet64Unaligned(&cursor), ==, 1);
    munit_assert_int(byteGet8(&cursor), ==, RAFT_VOTER);
    munit_assert_int(byteGet8(&cursor), ==, RAFT_VOTER);
    munit_assert_int(byteGet8(&cursor), ==, RAFT_GROUP_OLD);

    raft_free(buf.base);

    return MUNIT_OK;
}

/* Encode a configuration with two servers. */
TEST(configurationEncode, two_servers, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    struct raft_buffer buf;
    size_t len;
    size_t i;
    const void *cursor;

    ADD(1, RAFT_STANDBY);
    ADD(2, RAFT_VOTER);
    ENCODE(&buf);

    len = 1 + 8 +             /* Version and n of servers */
          8 + 1 +             /* Server 1 */
          8 + 1 +             /* Server 2 */
          256 +               /* Meta */
          8 + 1 + 1 + 1 +     /* Server 1 */
          8 + 1 + 1 + 1;      /* Server 2*/

    len = bytePad64(len);

    munit_assert_int(buf.len, ==, len);

    cursor = buf.base;

    munit_assert_int(byteGet8(&cursor), ==, 1);
    munit_assert_int(byteGet64Unaligned(&cursor), ==, 2);

    munit_assert_int(byteGet64Unaligned(&cursor), ==, 1);
    munit_assert_int(byteGet8(&cursor), ==, RAFT_STANDBY);

    munit_assert_int(byteGet64Unaligned(&cursor), ==, 2);
    munit_assert_int(byteGet8(&cursor), ==, RAFT_VOTER);

    // meta
    munit_assert_int(byteGet32(&cursor), ==, CONF_META_VERSION);
    munit_assert_int(byteGet32(&cursor), ==, CONF_SERVER_VERSION);
    munit_assert_int(byteGet32(&cursor), ==, CONF_SERVER_SIZE);
    munit_assert_int(byteGet8(&cursor), ==, 0);

    // skip reserve
    for (i = 0; i < 243; ++i)
        byteGet8(&cursor);

    munit_assert_int(byteGet64Unaligned(&cursor), ==, 1);
    munit_assert_int(byteGet8(&cursor), ==, RAFT_STANDBY);
    munit_assert_int(byteGet8(&cursor), ==, RAFT_STANDBY);
    munit_assert_int(byteGet8(&cursor), ==, RAFT_GROUP_OLD);

    munit_assert_int(byteGet64Unaligned(&cursor), ==, 2);
    munit_assert_int(byteGet8(&cursor), ==, RAFT_VOTER);
    munit_assert_int(byteGet8(&cursor), ==, RAFT_VOTER);
    munit_assert_int(byteGet8(&cursor), ==, RAFT_GROUP_OLD);

    raft_free(buf.base);

    return MUNIT_OK;
}

/* Out of memory. */
TEST(configurationEncode, oom, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    struct raft_buffer buf;
    HeapFaultConfig(&f->heap, 1, 1);
    HeapFaultEnable(&f->heap);
    ADD(1, RAFT_VOTER);
    ENCODE_ERROR(RAFT_NOMEM, &buf);
    return MUNIT_OK;
}

/******************************************************************************
 *
 * configurationDecode
 *
 *****************************************************************************/

SUITE(configurationDecode)

/* The decode a payload encoding a configuration with one server */
TEST(configurationDecode, one_server, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    uint8_t bytes[] = {1,                      /* Version */
                       1, 0, 0, 0, 0, 0, 0, 0, /* Number of servers */
                       5, 0, 0, 0, 0, 0, 0, 0, /* Server ID */
                       2};                     /* Role code */
    uint8_t metas[CONF_META_SIZE] = {1 , 0, 0, 0, /* Version */
                                     1 , 0, 0, 0, /* Server version */
                                     11, 0, 0, 0, /* Server size*/
                                     1 };         /* Phase joint */
    uint8_t nservers[] = {5, 0, 0, 0, 0, 0, 0, 0, /* Server ID */
                          2,                      /* Role code */
                          1,                      /* New Role */
                          3};                     /* Group */
    struct raft_buffer buf;
    int rv;
    uint8_t nbytes[sizeof(bytes) + CONF_META_SIZE + sizeof(nservers)] = {0};

    memcpy(nbytes, bytes, sizeof(bytes));
    memcpy(nbytes + sizeof(bytes), metas, sizeof(metas));
    memcpy(nbytes + sizeof(bytes) + sizeof(metas), nservers, sizeof(nservers));

    buf.base = bytes;
    buf.len = sizeof bytes;
    rv = configurationDecode(&buf, &f->configuration);
    munit_assert_int(rv, ==, 0);

    ASSERT_N(1);
    ASSERT_SERVER(0, 5, RAFT_SPARE, RAFT_SPARE, RAFT_GROUP_OLD);
    configurationClose(&f->configuration);
    configurationInit(&f->configuration);

    // new format
    buf.base = nbytes;
    buf.len= sizeof(nbytes);
    rv = configurationDecode(&buf, &f->configuration);
    munit_assert_int(rv, ==, 0);

    ASSERT_N(1);
    ASSERT_SERVER(0, 5, RAFT_SPARE, RAFT_VOTER, RAFT_GROUP_OLD|RAFT_GROUP_NEW);
    munit_assert_uint32(f->configuration.phase, ==, RAFT_CONF_JOINT);

    return MUNIT_OK;
}

/* The decode size is the size of a raft_server array plus the length of the
 * addresses. */
TEST(configurationDecode, two_servers, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    uint8_t bytes[] = {1,                      /* Version */
                       2, 0, 0, 0, 0, 0, 0, 0, /* Number of servers */
                       5, 0, 0, 0, 0, 0, 0, 0, /* Server ID */
                       1,                      /* Role code */
                       3, 0, 0, 0, 0, 0, 0, 0, /* Server ID */
                       2};                     /* Role code */
    uint8_t metas[CONF_META_SIZE] = {1 , 0, 0, 0, /* Version */
                                     1 , 0, 0, 0, /* Server version */
                                     11, 0, 0, 0, /* Server size*/
                                     1 };         /* Phase joint */
    uint8_t nservers[] = {5, 0, 0, 0, 0, 0, 0, 0, /* Server ID */
                          1,                      /* Role code */
                          1,                      /* New Role */
                          3,                      /* Group */
                          3, 0, 0, 0, 0, 0, 0, 0, /* Server ID */
                          2,                      /* Role code */
                          1,                      /* New Role */
                          3};                     /* Group */

    struct raft_buffer buf;
    uint8_t nbytes[sizeof(bytes) + CONF_META_SIZE + sizeof(nservers)] = {0};

    memcpy(nbytes, bytes, sizeof(bytes));
    memcpy(nbytes + sizeof(bytes), metas, sizeof(metas));
    memcpy(nbytes + sizeof(bytes) + sizeof(metas), nservers, sizeof(nservers));

    buf.base = bytes;
    buf.len = sizeof bytes;
    DECODE(&buf);
    ASSERT_N(2);
    ASSERT_SERVER(1, 5, RAFT_VOTER, RAFT_VOTER, RAFT_GROUP_OLD);
    ASSERT_SERVER(0, 3, RAFT_SPARE, RAFT_SPARE, RAFT_GROUP_OLD);
    configurationClose(&f->configuration);
    configurationInit(&f->configuration);

     // new format
    buf.base = nbytes;
    buf.len= sizeof(nbytes);
    DECODE(&buf);
    ASSERT_N(2);
    ASSERT_SERVER(1, 5, RAFT_VOTER, RAFT_VOTER, RAFT_GROUP_OLD|RAFT_GROUP_NEW);
    ASSERT_SERVER(0, 3, RAFT_SPARE, RAFT_VOTER, RAFT_GROUP_OLD|RAFT_GROUP_NEW);
    munit_assert_uint32(f->configuration.phase, ==, RAFT_CONF_JOINT);

    return MUNIT_OK;
}

/* Not enough memory of the servers array. */
TEST(configurationDecode, oom, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    uint8_t bytes[] = {1,                            /* Version */
                       1,   0,   0,   0, 0, 0, 0, 0, /* Number of servers */
                       5,   0,   0,   0, 0, 0, 0, 0, /* Server ID */
                       1};                           /* Voting flag */
    struct raft_buffer buf;
    HeapFaultConfig(&f->heap, 0, 1);
    HeapFaultEnable(&f->heap);
    buf.base = bytes;
    buf.len = sizeof bytes;
    DECODE_ERROR(RAFT_NOMEM, &buf);
    return MUNIT_OK;
}

/* If the encoding version is wrong, an error is returned. */
TEST(configurationDecode, badVersion, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    uint8_t bytes = 127;
    struct raft_buffer buf;
    buf.base = &bytes;
    buf.len = 1;
    DECODE_ERROR(RAFT_MALFORMED, &buf);
    return MUNIT_OK;
}

