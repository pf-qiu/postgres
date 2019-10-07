extern "C"
{
#include "libpqwalreceiver.h"
#include "libpq/pqformat.h"
#include "common/fe_memutils.h"
#include <sys/time.h>
}

#include <unistd.h>
#include <string>
#include <vector>
#include <memory>

using std::string;
using std::unique_ptr;
using std::vector;

/* Tuple coming via logical replication. */
struct LogicalRepTupleData
{
    /* column values in text format, or NULL for a null value: */
    vector<unique_ptr<string>> values;
    /* markers for changed/unchanged column values: */
    vector<bool> changed;

    int natts;
};

typedef uint32 LogicalRepRelId;

/* Relation information */
struct LogicalRepRelation
{
    /* Info coming from the remote side. */
    LogicalRepRelId remoteid; /* unique id of the relation */
    string nspname;           /* schema name */
    string relname;           /* relation name */
    int natts;                /* number of columns */
    vector<string> attnames;  /* column names */
    vector<Oid> atttyps;      /* column types */
    char replident;           /* replica identity */

    //Bitmapset  *attkeys;		/* Bitmap of key columns */
};

/* Type mapping info */
typedef struct LogicalRepTyp
{
    Oid remoteid;  /* unique id of the remote type */
    char *nspname; /* schema name of remote type */
    char *typname; /* name of the remote type */
} LogicalRepTyp;

/* Transaction info */
typedef struct LogicalRepBeginData
{
    XLogRecPtr final_lsn;
    TimestampTz committime;
    TransactionId xid;
} LogicalRepBeginData;

typedef struct LogicalRepCommitData
{
    XLogRecPtr commit_lsn;
    XLogRecPtr end_lsn;
    TimestampTz committime;
} LogicalRepCommitData;

void logicalrep_read_typ(StringInfo in, LogicalRepTyp *ltyp);
void logicalrep_read_begin(StringInfo in, LogicalRepBeginData *begin_data);
void logicalrep_read_rel(StringInfo in, LogicalRepRelation *rel);
LogicalRepRelId logicalrep_read_insert(StringInfo in, LogicalRepTupleData *newtup);
LogicalRepRelId logicalrep_read_update(StringInfo in, bool *has_oldtuple,
                                       LogicalRepTupleData *oldtup,
                                       LogicalRepTupleData *newtup);
LogicalRepRelId logicalrep_read_delete(StringInfo in, LogicalRepTupleData *oldtup);
vector<LogicalRepRelId> logicalrep_read_truncate(StringInfo in, bool *cascade, bool *restart_seqs);

TimestampTz
GetCurrentTimestamp(void);

static void apply_dispatch(StringInfo s);
static void send_feedback(WalReceiverConn *wrconn, XLogRecPtr recvpos);

int main()
{
    char *err;
    WalReceiverConn *wrconn = NULL;
    TimeLineID startpointTLI;
    WalRcvStreamOptions options;

    XLogRecPtr origin_startpos = 0;
    char myslotname[] = "mysub1";
    char mypubname[] = "mypub";

    wrconn = walrcv_connect("port=5432 host=127.0.0.1 user=qpf dbname=test", true, "mysub", &err);
    if (wrconn == NULL)
    {
        fprintf(stderr, "could not connect to the publisher: %s", err);
        return 1;
    }
    /*
		 * We don't really use the output identify_system for anything but it
		 * does some initializations on the upstream so let's still call it.
		 */
    (void)walrcv_identify_system(wrconn, &startpointTLI);

    /* Build logical replication streaming options. */
    options.logical = true;
    options.startpoint = origin_startpos;
    options.slotname = myslotname;
    options.proto.logical.proto_version = LOGICALREP_PROTO_VERSION_NUM;
    options.proto.logical.publication_names = mypubname;

    /* Start normal logical streaming replication. */
    walrcv_startstreaming(wrconn, &options);

    char *buf = NULL;
    pgsocket fd = PGINVALID_SOCKET;
    XLogRecPtr last_received = 0;
    int len;
    bool eof = false;
    while (!eof)
    {
        len = walrcv_receive(wrconn, &buf, &fd);

        if (len != 0)
        {
            while (true)
            {
                if (len == 0)
                {
                    break;
                }
                else if (len < 0)
                {
                    eof = true;
                    ereport(LOG,
                            (errmsg("data stream from publisher has ended")));
                    break;
                }
                int c;
                StringInfoData s;
                s.data = buf;
                s.len = len;
                s.cursor = 0;
                s.maxlen = -1;

                c = pq_getmsgbyte(&s);
                if (c == 'w')
                {
                    XLogRecPtr start_lsn;
                    XLogRecPtr end_lsn;
                    TimestampTz send_time;

                    start_lsn = pq_getmsgint64(&s);
                    end_lsn = pq_getmsgint64(&s);
                    send_time = pq_getmsgint64(&s);

                    //printf("%lx %lx %lx\n", start_lsn, end_lsn, send_time);
                    if (last_received < start_lsn)
                        last_received = start_lsn;

                    if (last_received < end_lsn)
                        last_received = end_lsn;

                    apply_dispatch(&s);
                }
                else if (c == 'k')
                {
                    XLogRecPtr end_lsn;
                    TimestampTz timestamp;
                    bool reply_requested;

                    end_lsn = pq_getmsgint64(&s);
                    timestamp = pq_getmsgint64(&s);
                    reply_requested = pq_getmsgbyte(&s);

                    //printf("%lx %lx\n", end_lsn, timestamp);
                    if (last_received < end_lsn)
                        last_received = end_lsn;

                    if (reply_requested)
                    {
                        send_feedback(wrconn, last_received);
                    }
                }

                len = walrcv_receive(wrconn, &buf, &fd);
            }
        }

        send_feedback(wrconn, last_received);
        
        sleep(1);
    }
    TimeLineID tli;
    walrcv_endstreaming(wrconn, &tli);
    return 0;
}

/*
 * Send a Standby Status Update message to server.
 *
 * 'recvpos' is the latest LSN we've received data to, force is set if we need
 * to send a response to avoid timeouts.
 */
static void
send_feedback(WalReceiverConn *wrconn, XLogRecPtr recvpos)
{
    StringInfo reply_message = makeStringInfo();
    TimestampTz now = GetCurrentTimestamp();

    pq_sendbyte(reply_message, 'r');
    pq_sendint64(reply_message, recvpos); /* write */
    pq_sendint64(reply_message, recvpos); /* flush */
    pq_sendint64(reply_message, recvpos); /* apply */
    pq_sendint64(reply_message, now);     /* sendTime */
    pq_sendbyte(reply_message, false);    /* replyRequested */

    walrcv_send(wrconn, reply_message->data, reply_message->len);
}

/*
 * Handle BEGIN message.
 */
static void
apply_handle_begin(StringInfo s)
{
    LogicalRepBeginData begin_data;

    logicalrep_read_begin(s, &begin_data);

    printf("BEGIN:\n");
    printf("final_lsn: %ld\n", begin_data.final_lsn);
    printf("committime: %ld\n", begin_data.committime);
    printf("xid: %d\n", begin_data.xid);
}

/*
 * Read transaction COMMIT from the stream.
 */
void logicalrep_read_commit(StringInfo in, LogicalRepCommitData *commit_data)
{
    /* read flags (unused for now) */
    uint8 flags = pq_getmsgbyte(in);

    if (flags != 0)
        elog(ERROR, "unrecognized flags %u in commit message", flags);

    /* read fields */
    commit_data->commit_lsn = pq_getmsgint64(in);
    commit_data->end_lsn = pq_getmsgint64(in);
    commit_data->committime = pq_getmsgint64(in);
}

/*
 * Handle COMMIT message.
 *
 * TODO, support tracking of multiple origins
 */
static void
apply_handle_commit(StringInfo s)
{
    LogicalRepCommitData commit_data;

    logicalrep_read_commit(s, &commit_data);

    printf("COMMIT:\n");
    printf("final_lsn: %ld\n", commit_data.commit_lsn);
    printf("end_lsn: %ld\n", commit_data.end_lsn);
    printf("committime: %ld\n", commit_data.committime);
}

/*
 * Handle ORIGIN message.
 *
 * TODO, support tracking of multiple origins
 */
static void
apply_handle_origin(StringInfo s)
{
    printf("ORIGIN:\n");
}

/*
 * Read the namespace name while treating empty string as pg_catalog.
 */
static const char *
logicalrep_read_namespace(StringInfo in)
{
    const char *nspname = pq_getmsgstring(in);

    if (nspname[0] == '\0')
        nspname = "pg_catalog";

    return nspname;
}

/*
 * Handle RELATION message.
 *
 * Note we don't do validation against local schema here. The validation
 * against local schema is postponed until first change for given relation
 * comes as we only care about it when applying changes for it anyway and we
 * do less locking this way.
 */
static void
apply_handle_relation(StringInfo s)
{
    LogicalRepRelation rel;

    logicalrep_read_rel(s, &rel);

    printf("RELATION:\n");
    printf("remoteid: %d\n", rel.remoteid);
    printf("nspname: %s\n", rel.nspname.c_str());
    printf("relname: %s\n", rel.relname.c_str());
    printf("natts: %d\n", rel.natts);
    for (int i = 0; i < rel.natts; i++)
    {
        printf("attnames[%d]: %s\n", i, rel.attnames[i].c_str());
        printf("atttyps[%d]: %d\n", i, rel.atttyps[i]);
    }
    printf("replident: %d\n", rel.replident);
}

/*
 * Handle TYPE message.
 *
 * Note we don't do local mapping here, that's done when the type is
 * actually used.
 */
static void
apply_handle_type(StringInfo s)
{
    LogicalRepTyp typ;

    logicalrep_read_typ(s, &typ);

    printf("TYPE:\n");
    printf("remoteid: %d\n", typ.remoteid);
    printf("nspname: %s\n", typ.nspname);
    printf("typname: %s\n", typ.typname);
}

static void print_tuple(LogicalRepTupleData *tup)
{
    printf("natts: %d\n", tup->natts);
    for (int i = 0; i < tup->natts; i++)
    {
        int changed = tup->changed[i];
        printf("tup->changed[%d]: %d\n", i, changed);
        if (tup->values[i] == NULL)
        {
            printf("tup->values[%d]: NULL\n", i);
        }
        else
        {
            printf("tup->values[%d]: %s\n", i, tup->values[i]->c_str());
        }
    }
}

/*
 * Handle INSERT message.
 */
static void
apply_handle_insert(StringInfo s)
{
    LogicalRepTupleData newtup;
    LogicalRepRelId relid;

    /* read the relation id */
    relid = logicalrep_read_insert(s, &newtup);

    printf("INSERT:\n");
    printf("relid: %d\n", relid);
    print_tuple(&newtup);
}

/*
 * Handle UPDATE message.
 *
 * TODO: FDW support
 */
static void
apply_handle_update(StringInfo s)
{
    LogicalRepRelId relid;

    LogicalRepTupleData oldtup;
    LogicalRepTupleData newtup;
    bool has_oldtup;

    relid = logicalrep_read_update(s, &has_oldtup, &oldtup,
                                   &newtup);

    printf("UPDATE:\n");
    printf("relid: %d\n", relid);
    if (has_oldtup)
    {
        printf("oldtup:\n");
        print_tuple(&oldtup);
    }

    printf("newtup:\n");
    print_tuple(&newtup);
}

/*
 * Handle DELETE message.
 *
 * TODO: FDW support
 */
static void
apply_handle_delete(StringInfo s)
{
    LogicalRepTupleData oldtup;
    LogicalRepRelId relid;

    relid = logicalrep_read_delete(s, &oldtup);

    printf("DELETE:\n");
    printf("relid: %d\n", relid);

    printf("oldtup:\n");
    print_tuple(&oldtup);
}

/*
 * Handle TRUNCATE message.
 *
 * TODO: FDW support
 */
static void
apply_handle_truncate(StringInfo s)
{
    bool cascade = false;
    bool restart_seqs = false;
    auto remote_relids = logicalrep_read_truncate(s, &cascade, &restart_seqs);

    printf("TRUNCATE:\n");
    printf("cascade: %d\n", cascade);
    printf("restart_seqs: %d\n", restart_seqs);
    printf("remote_relids_length: %ld\n", remote_relids.size());
    for (size_t i = 0; i < remote_relids.size(); i++)
    {
        printf("remote_relids[%ld]: %d", i, remote_relids[i]);
    }
}

static void apply_dispatch(StringInfo s)
{
    char action = pq_getmsgbyte(s);

    switch (action)
    {
        /* BEGIN */
    case 'B':
        apply_handle_begin(s);
        break;
        /* COMMIT */
    case 'C':
        apply_handle_commit(s);
        break;
        /* INSERT */
    case 'I':
        apply_handle_insert(s);
        break;
        /* UPDATE */
    case 'U':
        apply_handle_update(s);
        break;
        /* DELETE */
    case 'D':
        apply_handle_delete(s);
        break;
        /* TRUNCATE */
    case 'T':
        apply_handle_truncate(s);
        break;
        /* RELATION */
    case 'R':
        apply_handle_relation(s);
        break;
        /* TYPE */
    case 'Y':
        apply_handle_type(s);
        break;
        /* ORIGIN */
    case 'O':
        apply_handle_origin(s);
        break;
    default:
        fprintf(stderr, "invalid logical replication message type \"%c\"", action);
    }
}

/*
 * Read transaction BEGIN from the stream.
 */
void logicalrep_read_begin(StringInfo in, LogicalRepBeginData *begin_data)
{
    /* read fields */
    begin_data->final_lsn = pq_getmsgint64(in);
    if (begin_data->final_lsn == InvalidXLogRecPtr)
        elog(ERROR, "final_lsn not set in begin message");
    begin_data->committime = pq_getmsgint64(in);
    begin_data->xid = pq_getmsgint(in, 4);
}

/*
 * Read type info from the output stream.
 */
void logicalrep_read_typ(StringInfo in, LogicalRepTyp *ltyp)
{
    ltyp->remoteid = pq_getmsgint(in, 4);

    /* Read type name from stream */
    ltyp->nspname = pstrdup(logicalrep_read_namespace(in));
    ltyp->typname = pstrdup(pq_getmsgstring(in));
}

/* Julian-date equivalents of Day 0 in Unix and Postgres reckoning */
#define UNIX_EPOCH_JDATE 2440588     /* == date2j(1970, 1, 1) */
#define POSTGRES_EPOCH_JDATE 2451545 /* == date2j(2000, 1, 1) */

#define SECS_PER_YEAR (36525 * 864) /* avoid floating-point computation */
#define SECS_PER_DAY 86400
#define SECS_PER_HOUR 3600
#define SECS_PER_MINUTE 60
#define MINS_PER_HOUR 60

#define USECS_PER_DAY INT64CONST(86400000000)
#define USECS_PER_HOUR INT64CONST(3600000000)
#define USECS_PER_MINUTE INT64CONST(60000000)
#define USECS_PER_SEC INT64CONST(1000000)

TimestampTz
GetCurrentTimestamp(void)
{
    TimestampTz result;
    struct timeval tp;

    gettimeofday(&tp, NULL);

    result = (TimestampTz)tp.tv_sec -
             ((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY);
    result = (result * USECS_PER_SEC) + tp.tv_usec;

    return result;
}

/*
 * Read relation attribute names from the stream.
 */
static void
logicalrep_read_attrs(StringInfo in, LogicalRepRelation *rel)
{
    int i;
    int natts;
    //Bitmapset *attkeys = NULL;

    natts = pq_getmsgint(in, 2);
    rel->natts = natts;
    rel->attnames.reserve(natts);
    rel->atttyps.reserve(natts);

    /* read the attributes */
    for (i = 0; i < natts; i++)
    {
        (void)pq_getmsgbyte(in);
        /* Check for replica identity column */
        /* uint8 flags;

        flags = pq_getmsgbyte(in);
        if (flags & LOGICALREP_IS_REPLICA_IDENTITY)
			attkeys = bms_add_member(attkeys, i);*/

        /* attribute name */

        rel->attnames.emplace_back(pq_getmsgstring(in));

        /* attribute type id */
        rel->atttyps.emplace_back((Oid)pq_getmsgint(in, 4));

        /* we ignore attribute mode for now */
        (void)pq_getmsgint(in, 4);
    }
}

/*
 * Read the relation info from stream and return as LogicalRepRelation.
 */
void logicalrep_read_rel(StringInfo in, LogicalRepRelation *rel)
{
    rel->remoteid = pq_getmsgint(in, 4);

    /* Read relation name from stream */
    rel->nspname = logicalrep_read_namespace(in);
    rel->relname = pq_getmsgstring(in);

    /* Read the replica identity. */
    rel->replident = pq_getmsgbyte(in);

    /* Get attribute description */
    logicalrep_read_attrs(in, rel);
}

/*
 * Read tuple in remote format from stream.
 *
 * The returned tuple points into the input stringinfo.
 */
static void
logicalrep_read_tuple(StringInfo in, LogicalRepTupleData *tuple)
{
    int i;
    int natts;

    /* Get number of attributes */
    natts = pq_getmsgint(in, 2);
    tuple->natts = natts;
    tuple->changed.resize(natts);
    tuple->values.resize(natts);

    /* Read the data */
    for (i = 0; i < natts; i++)
    {
        char kind;

        kind = pq_getmsgbyte(in);

        switch (kind)
        {
        case 'n': /* null */
            tuple->values[i] = nullptr;
            tuple->changed[i] = true;
            break;
        case 'u': /* unchanged column */
            /* we don't receive the value of an unchanged column */
            tuple->values[i] = nullptr;
            break;
        case 't': /* text formatted value */
        {
            int len;

            tuple->changed[i] = true;

            len = pq_getmsgint(in, 4); /* read length */

            /* and data */
            //tuple->values[i] = palloc(len + 1);
            const char *buf = pq_getmsgbytes(in, len);
            tuple->values[i] = std::make_unique<string>(buf, buf + len);
        }
        break;
        default:
            elog(ERROR, "unrecognized data representation type '%c'", kind);
        }
    }
}

/*
 * Read INSERT from stream.
 *
 * Fills the new tuple.
 */
LogicalRepRelId
logicalrep_read_insert(StringInfo in, LogicalRepTupleData *newtup)
{
    char action;
    LogicalRepRelId relid;

    /* read the relation id */
    relid = pq_getmsgint(in, 4);

    action = pq_getmsgbyte(in);
    if (action != 'N')
        elog(ERROR, "expected new tuple but got %d",
             action);

    logicalrep_read_tuple(in, newtup);

    return relid;
}

/*
 * Read UPDATE from stream.
 */
LogicalRepRelId
logicalrep_read_update(StringInfo in, bool *has_oldtuple,
                       LogicalRepTupleData *oldtup,
                       LogicalRepTupleData *newtup)
{
    char action;
    LogicalRepRelId relid;

    /* read the relation id */
    relid = pq_getmsgint(in, 4);

    /* read and verify action */
    action = pq_getmsgbyte(in);
    if (action != 'K' && action != 'O' && action != 'N')
        elog(ERROR, "expected action 'N', 'O' or 'K', got %c",
             action);

    /* check for old tuple */
    if (action == 'K' || action == 'O')
    {
        logicalrep_read_tuple(in, oldtup);
        *has_oldtuple = true;

        action = pq_getmsgbyte(in);
    }
    else
        *has_oldtuple = false;

    /* check for new  tuple */
    if (action != 'N')
        elog(ERROR, "expected action 'N', got %c",
             action);

    logicalrep_read_tuple(in, newtup);

    return relid;
}

/*
 * Read DELETE from stream.
 *
 * Fills the old tuple.
 */
LogicalRepRelId
logicalrep_read_delete(StringInfo in, LogicalRepTupleData *oldtup)
{
    char action;
    LogicalRepRelId relid;

    /* read the relation id */
    relid = pq_getmsgint(in, 4);

    /* read and verify action */
    action = pq_getmsgbyte(in);
    if (action != 'K' && action != 'O')
        elog(ERROR, "expected action 'O' or 'K', got %c", action);

    logicalrep_read_tuple(in, oldtup);

    return relid;
}

#define TRUNCATE_CASCADE (1 << 0)
#define TRUNCATE_RESTART_SEQS (1 << 1)

/*
 * Read TRUNCATE from stream.
 */
vector<LogicalRepRelId>
logicalrep_read_truncate(StringInfo in,
                         bool *cascade, bool *restart_seqs)
{
    int i;
    int nrelids;
    vector<Oid> relids;
    uint8 flags;

    nrelids = pq_getmsgint(in, 4);

    /* read and decode truncate flags */
    flags = pq_getmsgint(in, 1);
    *cascade = (flags & TRUNCATE_CASCADE) > 0;
    *restart_seqs = (flags & TRUNCATE_RESTART_SEQS) > 0;

    relids.reserve(nrelids);
    for (i = 0; i < nrelids; i++)
    {
        relids.emplace_back(pq_getmsgint(in, 4));
    }

    return relids;
}
