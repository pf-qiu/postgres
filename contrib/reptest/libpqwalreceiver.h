#define FRONTEND 1
#include "postgres.h"
#include "nodes/pg_list.h"
#include "utils/tuplestore.h"
//#include "replication/logicalproto.h"
/*
 * What to do with a snapshot in create replication slot command.
 */
typedef enum
{
	CRS_EXPORT_SNAPSHOT,
	CRS_NOEXPORT_SNAPSHOT,
	CRS_USE_SNAPSHOT
} CRSSnapshotAction;

typedef int64 TimestampTz;
#define LOGICALREP_PROTO_VERSION_NUM 1

/*
 * Status of walreceiver query execution.
 *
 * We only define statuses that are currently used.
 */
typedef enum
{
	WALRCV_ERROR,				/* There was error when executing the query. */
	WALRCV_OK_COMMAND,			/* Query executed utility or replication
								 * command. */
	WALRCV_OK_TUPLES,			/* Query returned tuples. */
	WALRCV_OK_COPY_IN,			/* Query started COPY FROM. */
	WALRCV_OK_COPY_OUT,			/* Query started COPY TO. */
	WALRCV_OK_COPY_BOTH			/* Query started COPY BOTH replication
								 * protocol. */
} WalRcvExecStatus;


/*
 * Return value for walrcv_exec, returns the status of the execution and
 * tuples if any.
 */
typedef struct WalRcvExecResult
{
	WalRcvExecStatus status;
	char	   *err;
	Tuplestorestate *tuplestore;
	TupleDesc	tupledesc;
} WalRcvExecResult;


typedef uint32 TimeLineID;
struct WalReceiverConn;
typedef struct WalReceiverConn WalReceiverConn;

/*
 * Pointer to a location in the XLOG.  These pointers are 64 bits wide,
 * because we don't want them ever to overflow.
 */
typedef uint64 XLogRecPtr;

typedef struct
{
	bool		logical;		/* True if this is logical replication stream,
								 * false if physical stream.  */
	char	   *slotname;		/* Name of the replication slot or NULL. */
	XLogRecPtr	startpoint;		/* LSN of starting point. */

	union
	{
		struct
		{
			TimeLineID	startpointTLI;	/* Starting timeline */
		}			physical;
		struct
		{
			uint32		proto_version;	/* Logical protocol version */
			char	   *publication_names;	/* String list of publications */
		}			logical;
	}			proto;
} WalRcvStreamOptions;


/* libpqwalreceiver hooks */
extern WalReceiverConn *walrcv_connect (const char *conninfo, bool logical,
											   const char *appname,
											   char **err);
extern char *walrcv_identify_system (WalReceiverConn *conn,
											TimeLineID *primary_tli);
extern bool walrcv_startstreaming (WalReceiverConn *conn,
										  const WalRcvStreamOptions *options);
extern void walrcv_endstreaming (WalReceiverConn *conn,
										TimeLineID *next_tli);
extern int walrcv_receive (WalReceiverConn *conn, char **buffer,
								  pgsocket *wait_fd);
extern void walrcv_send (WalReceiverConn *conn, const char *buffer,
								int nbytes);
extern void walrcv_disconnect (WalReceiverConn *conn);
