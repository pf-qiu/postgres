extern "C"
{
#include "c.h"
#include "libpqwalreceiver.h"
#include "libpq/pqformat.h"
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