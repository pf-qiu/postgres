#include "walproto.h"
#include <sys/time.h>

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
