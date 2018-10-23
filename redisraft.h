#ifndef _REDISRAFT_H
#define _REDISRAFT_H

#include <stdint.h>
#include <stdbool.h>
#include <bsd/sys/queue.h>
#include <stdio.h>
#include <unistd.h>

#define REDISMODULE_EXPERIMENTAL_API
#include "uv.h"
#include "hiredis/hiredis.h"
#include "hiredis/async.h"
#include "redismodule.h"
#include "raft.h"

/* --------------- RedisModule_Log levels used -------------- */

#define REDIS_RAFT_DATATYPE_NAME     "redisraft"
#define REDIS_RAFT_DATATYPE_ENCVER   1

/* --------------- RedisModule_Log levels used -------------- */

#define REDIS_WARNING   "warning"
#define REDIS_NOTICE    "notice"
#define REDIS_VERBOSE   "verbose"

/* -------------------- Logging macros -------------------- */

/*
 * We use our own logging mechanism because most log output is generated by
 * the Raft thread which cannot use Redis logging.
 *
 * TODO Migrate to RedisModule_Log when it's capable of logging using a
 * Thread Safe context.
 */

extern int redis_raft_loglevel;
extern FILE *redis_raft_logfile;

void raft_module_log(const char *fmt, ...);

#define LOGLEVEL_ERROR           0
#define LOGLEVEL_INFO            1
#define LOGLEVEL_VERBOSE         2
#define LOGLEVEL_DEBUG           3

#define LOG(level, fmt, ...) \
    do { if (redis_raft_loglevel >= level) \
            raft_module_log(fmt, ##__VA_ARGS__); \
    } while(0)

#define LOG_ERROR(fmt, ...) LOG(LOGLEVEL_ERROR, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) LOG(LOGLEVEL_INFO, fmt, ##__VA_ARGS__)
#define LOG_VERBOSE(fmt, ...) LOG(LOGLEVEL_VERBOSE, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) LOG(LOGLEVEL_DEBUG, fmt, ##__VA_ARGS__)

#define PANIC(fmt, ...) \
    do {  LOG_ERROR("\n\n" \
                    "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n" \
                    "REDIS RAFT PANIC\n" \
                    "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n\n" \
                    fmt, ##__VA_ARGS__); exit(1); } while (0)

#define TRACE(fmt, ...) LOG(LOGLEVEL_DEBUG, "%s:%d: " fmt, __FILE__, __LINE__, ##__VA_ARGS__)

#define NODE_LOG(level, node, fmt, ...) \
    LOG(level, "node:%d: " fmt, (node)->id, ##__VA_ARGS__)

#define NODE_LOG_ERROR(node, fmt, ...) NODE_LOG(LOGLEVEL_ERROR, node, fmt, ##__VA_ARGS__)
#define NODE_LOG_INFO(node, fmt, ...) NODE_LOG(LOGLEVEL_INFO, node, fmt, ##__VA_ARGS__)
#define NODE_LOG_VERBOSE(node, fmt, ...) NODE_LOG(LOGLEVEL_VERBOSE, node, fmt, ##__VA_ARGS__)
#define NODE_LOG_DEBUG(node, fmt, ...) NODE_LOG(LOGLEVEL_DEBUG, node, fmt, ##__VA_ARGS__)

/* Forward declarations */
struct RaftReq;
struct RedisRaftConfig;
struct Node;

/* Node address specifier. */
typedef struct node_addr {
    uint16_t port;
    char host[256];             /* Hostname or IP address */
} NodeAddr;

/* A singly linked list of NodeAddr elements */
typedef struct NodeAddrListElement {
    NodeAddr addr;
    struct NodeAddrListElement *next;
} NodeAddrListElement;

/* General state of the module */
typedef enum RedisRaftState {
    REDIS_RAFT_UNINITIALIZED,       /* Waiting for RAFT.CLUSTER command */
    REDIS_RAFT_UP,                  /* Up and running */
    REDIS_RAFT_LOADING,             /* Loading (or attempting) RDB/Raft Log on startup */
    REDIS_RAFT_JOINING              /* Processing a RAFT.CLUSTER JOIN command */
} RedisRaftState;

/* A node configuration entry that describe the known configuration of a specific
 * node at the time of snapshot.
 */
typedef struct SnapshotCfgEntry {
    raft_node_id_t  id;
    int             active;
    int             voting;
    NodeAddr        addr;
    struct SnapshotCfgEntry *next;
} SnapshotCfgEntry;

#define RAFT_DBID_LEN   32

/* Snapshot metadata.  There is a single instnace of this struct available at all times,
 * which is accessed as follows:
 * 1. During cluster setup, it is initialized (e.g. with a unique dbid).
 * 2. The last applied term and index fields are updated every time we apply a log entry
 *    into the dataset, to reflect the real-time state of the snapshot.
 * 3. On rdbsave, the record gets serialized (using a dummy key for now; TODO use a global
 *    state mechanism when Redis Module API supports it).
 * 4. On rdbload, the record gets loaded and the loaded flag is set.
 */
typedef struct RaftSnapshotInfo {
    bool loaded;
    char dbid[RAFT_DBID_LEN+1];
    raft_term_t last_applied_term;
    raft_index_t last_applied_idx;
    SnapshotCfgEntry *cfg;
} RaftSnapshotInfo;

/* State of the RAFT.CLUSTER JOIN operation.
 *
 * The address list is initialized by RAFT.CLUSTER JOIN, but it may grow if RAFT.NODE ADD
 * requests are sent to follower nodes that reply -MOVED.
 *
 * We use a fake Node structure to simplify and reuse connection management code.
 */
typedef struct RaftJoinState {
    NodeAddrListElement *addr;
    NodeAddrListElement *addr_iter;
    struct Node *node;
} RaftJoinState;

/* Global Raft context */
typedef struct {
    void *raft;                 /* Raft library context */
    RedisModuleCtx *ctx;        /* Redis module thread-safe context; only used to push commands
                                   we get from the leader. */
    RedisRaftState state;       /* Raft module state */
    uv_thread_t thread;         /* Raft I/O thread */
    uv_loop_t *loop;            /* Raft I/O loop */
    uv_async_t rqueue_sig;      /* A signal we have something on rqueue */
    uv_timer_t raft_periodic_timer;     /* Invoke Raft periodic func */
    uv_timer_t node_reconnect_timer;    /* Handle connection issues */
    uv_mutex_t rqueue_mutex;    /* Mutex protecting rqueue access */
    STAILQ_HEAD(rqueue, RaftReq) rqueue;     /* Requests queue (Redis thread -> Raft thread) */
    struct RaftLog *log;        /* Raft persistent log; May be NULL if not used */
    struct RedisRaftConfig *config;     /* User provided configuration */
    RaftJoinState *join_state;  /* Tracks state while we're in REDIS_RAFT_JOINING */
    bool snapshot_in_progress;  /* Indicates we're creating a snapshot in the background */
    raft_index_t snapshot_rewrite_last_idx; /* TODO: Needed? */
    struct RaftReq *compact_req;    /* Current RAFT.DEBUG COMPACT request */
    bool callbacks_set;         /* TODO: Needed? */
    int snapshot_child_fd;      /* Pipe connected to snapshot child process */
    RaftSnapshotInfo snapshot_info; /* Current snapshot info */
} RedisRaftCtx;

#define REDIS_RAFT_DEFAULT_INTERVAL                 100
#define REDIS_RAFT_DEFAULT_REQUEST_TIMEOUT          250
#define REDIS_RAFT_DEFAULT_ELECTION_TIMEOUT         500
#define REDIS_RAFT_DEFAULT_RECONNECT_INTERVAL       100
#define REDIS_RAFT_DEFAULT_MAX_LOG_ENTRIES          10000

typedef struct RedisRaftConfig {
    raft_node_id_t id;          /* Local node Id */
    NodeAddr addr;              /* Address of local node, if specified */
    char *rdb_filename;         /* Original Redis dbfilename */
    char *raftlog;              /* Raft log file name, derived from dbfilename */
    /* Tuning */
    int raft_interval;
    int request_timeout;
    int election_timeout;
    int reconnect_interval;
    int max_log_entries;
    /* Debug options */
    int compact_delay;
} RedisRaftConfig;

typedef void (*NodeConnectCallbackFunc)(const redisAsyncContext *, int);

typedef enum NodeState {
    NODE_DISCONNECTED,
    NODE_RESOLVING,
    NODE_CONNECTING,
    NODE_CONNECTED,
    NODE_CONNECT_ERROR
} NodeState;

typedef enum NodeFlags {
    NODE_TERMINATING    = 1 << 0
} NodeFlags;

#define NODE_STATE_IDLE(x) \
    ((x) == NODE_DISCONNECTED || \
     (x) == NODE_CONNECT_ERROR)

/* Maintains all state about peer nodes */
typedef struct Node {
    raft_node_id_t id;              /* Raft unique node ID */
    NodeState state;                /* Node connection state */
    NodeFlags flags;                /* See: enum NodeFlags */
    NodeAddr addr;                  /* Node's address */
    redisAsyncContext *rc;          /* hiredis async context */
    uv_getaddrinfo_t uv_resolver;   /* libuv resolver context */
    RedisRaftCtx *rr;               /* Pointer back to redis_raft */
    NodeConnectCallbackFunc connect_callback;   /* Connection callback */
    bool load_snapshot_in_progress; /* Are we currently pushing a snapshot? */
    raft_index_t load_snapshot_idx; /* Index of snapshot we're pushing */
    time_t load_snapshot_last_time; /* Last time we pushed a snapshot */
    uv_fs_t uv_snapshot_req;        /* libuv handle managing snapshot loading from disk */
    uv_file uv_snapshot_file;       /* libuv handle for snapshot file */
    size_t snapshot_size;           /* Size of snapshot we're pushing */
    char *snapshot_buf;             /* Snapshot buffer; TODO: Currently we buffer the entire RDB
                                     * because hiredis will not stream it for us. */
    uv_buf_t uv_snapshot_buf;       /* libuv wrapper for snapshot_buf */
    LIST_ENTRY(Node) entries;
} Node;

typedef void (*RaftReqHandler)(RedisRaftCtx *, struct RaftReq *);

/* General purpose status code.  Convention is this:
 * In redisraft.c (Redis Module wrapper) we generally use REDISMODULE_OK/REDISMODULE_ERR.
 * Elsewhere we stick to it.
 */
typedef enum RRStatus {
    RR_OK       = 0,
    RR_ERROR
} RRStatus;

/* Request types.  Note that these must match the order in RaftReqHandlers! */
enum RaftReqType {
    RR_CLUSTER_INIT = 1,
    RR_CLUSTER_JOIN,
    RR_CFGCHANGE_ADDNODE,
    RR_CFGCHANGE_REMOVENODE,
    RR_APPENDENTRIES,
    RR_REQUESTVOTE,
    RR_REDISCOMMAND,
    RR_INFO,
    RR_LOADSNAPSHOT,
    RR_COMPACT
};

typedef struct {
    raft_node_id_t id;
    NodeAddr addr;
} RaftCfgChange;

typedef struct {
    int argc;
    RedisModuleString **argv;
} RaftRedisCommand;

typedef struct RaftReq {
    int type;
    STAILQ_ENTRY(RaftReq) entries;
    RedisModuleBlockedClient *client;
    RedisModuleCtx *ctx;
    union {
        struct {
            NodeAddrListElement *addr;
        } cluster_join;
        RaftCfgChange cfgchange;
        struct {
            raft_node_id_t src_node_id;
            msg_appendentries_t msg;
        } appendentries;
        struct {
            raft_node_id_t src_node_id;
            msg_requestvote_t msg;
        } requestvote;
        struct {
            RaftRedisCommand cmd;
            msg_entry_response_t response;
        } redis;
        struct {
            raft_term_t term;
            raft_index_t idx;
            RedisModuleString *snapshot;
        } loadsnapshot;
    } r;
} RaftReq;

typedef struct RaftLog {
    uint32_t            version;
    char                dbid[RAFT_DBID_LEN+1];
    unsigned long int   num_entries;
    raft_term_t         snapshot_last_term;
    raft_index_t        snapshot_last_idx;
    raft_node_id_t      vote;
    raft_term_t         term;
    raft_index_t        index;
    FILE                *file;
} RaftLog;

#define RAFTLOG_VERSION     1

#define SNAPSHOT_RESULT_MAGIC    0x70616e73  /* "snap" */
typedef struct SnapshotResult {
    int magic;
    int success;
    long long int num_entries;
    char rdb_filename[256];
    char log_filename[256];
    char err[256];
} SnapshotResult;


/* node.c */
void NodeFree(Node *node);
Node *NodeInit(int id, const NodeAddr *addr);
bool NodeConnect(Node *node, RedisRaftCtx *rr, NodeConnectCallbackFunc connect_callback);
bool NodeAddrParse(const char *node_addr, size_t node_addr_len, NodeAddr *result);
void NodeAddrListAddElement(NodeAddrListElement **head, NodeAddr *addr);
void NodeAddrListFree(NodeAddrListElement *head);
void HandleNodeStates(RedisRaftCtx *rr);

/* raft.c */
const char *getStateStr(RedisRaftCtx *rr);
void RaftRedisCommandSerialize(raft_entry_data_t *target, RaftRedisCommand *source);
bool RaftRedisCommandDeserialize(RedisModuleCtx *ctx, RaftRedisCommand *target, raft_entry_data_t *source);
void RaftRedisCommandFree(RedisModuleCtx *ctx, RaftRedisCommand *r);
RRStatus RedisRaftInit(RedisModuleCtx *ctx, RedisRaftCtx *rr, RedisRaftConfig *config);
RRStatus RedisRaftStart(RedisModuleCtx *ctx, RedisRaftCtx *rr);

void RaftReqFree(RaftReq *req);
RaftReq *RaftReqInit(RedisModuleCtx *ctx, enum RaftReqType type);
void RaftReqSubmit(RedisRaftCtx *rr, RaftReq *req);
void RaftReqHandleQueue(uv_async_t *handle);

/* util.c */
int RedisModuleStringToInt(RedisModuleString *str, int *value);
char *catsnprintf(char *strbuf, size_t *strbuf_len, const char *fmt, ...);
int stringmatchlen(const char *pattern, int patternLen, const char *string, int stringLen, int nocase);
int stringmatch(const char *pattern, const char *string, int nocase);
int RedisInfoIterate(const char **info_ptr, size_t *info_len, const char **key, size_t *keylen, const char **value, size_t *valuelen);
char *RedisInfoGetParam(RedisRaftCtx *rr, const char *section, const char *param);

/* log.c */
typedef enum LogEntryAction {
    LA_APPEND,
    LA_REMOVE_HEAD,
    LA_REMOVE_TAIL
} LogEntryAction;

RaftLog *RaftLogCreate(const char *filename, const char *dbid, raft_term_t term, raft_index_t index);
RaftLog *RaftLogOpen(const char *filename);
void RaftLogClose(RaftLog *log);
bool RaftLogAppend(RaftLog *log, raft_entry_t *entry);
bool RaftLogSetVote(RaftLog *log, raft_node_id_t vote);
bool RaftLogSetTerm(RaftLog *log, raft_term_t term, raft_node_id_t vote);
int RaftLogLoadEntries(RaftLog *log, int (*callback)(void *, LogEntryAction action, raft_entry_t *), void *callback_arg);
bool RaftLogRemoveHead(RaftLog *log);
bool RaftLogRemoveTail(RaftLog *log);
bool RaftLogWriteEntry(RaftLog *log, raft_entry_t *entry);
bool RaftLogSync(RaftLog *log);

/* config.c */
void ConfigInit(RedisModuleCtx *ctx, RedisRaftConfig *config);
RRStatus ConfigParseArgs(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, RedisRaftConfig *target);
RRStatus ConfigValidate(RedisModuleCtx *ctx, RedisRaftConfig *config);
void handleConfigSet(RedisModuleCtx *ctx, RedisRaftConfig *config, RedisModuleString **argv, int argc);
void handleConfigGet(RedisModuleCtx *ctx, RedisRaftConfig *config, RedisModuleString **argv, int argc);
RRStatus ConfigReadFromRedis(RedisRaftCtx *rr);

/* snapshot.c */
extern RedisModuleTypeMethods RedisRaftTypeMethods;
extern RedisModuleType *RedisRaftType;
void initializeSnapshotInfo(RedisRaftCtx *rr);
void handleLoadSnapshot(RedisRaftCtx *rr, RaftReq *req);
void checkLoadSnapshotProgress(RedisRaftCtx *rr);
RRStatus initiateSnapshot(RedisRaftCtx *rr);
RRStatus finalizeSnapshot(RedisRaftCtx *rr, SnapshotResult *sr);
void cancelSnapshot(RedisRaftCtx *rr, SnapshotResult *sr);
void handleCompact(RedisRaftCtx *rr, RaftReq *req);
int pollSnapshotStatus(RedisRaftCtx *rr, SnapshotResult *sr);
void configRaftFromSnapshotInfo(RedisRaftCtx *rr);
int raftSendSnapshot(raft_server_t *raft, void *user_data, raft_node_t *raft_node);

#endif  /* _REDISRAFT_H */
