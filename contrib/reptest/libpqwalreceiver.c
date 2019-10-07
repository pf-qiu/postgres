/*-------------------------------------------------------------------------
 *
 * libpqwalreceiver.c
 *
 * This file contains the libpq-specific parts of walreceiver. It's
 * loaded as a dynamic module to avoid linking the main server binary with
 * libpq.
 *
 * Portions Copyright (c) 2010-2019, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/replication/libpqwalreceiver/libpqwalreceiver.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <sys/time.h>

#include "libpq-fe.h"
#include "pqexpbuffer.h"
#include "access/xlog.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "libpqwalreceiver.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/pg_lsn.h"
#include "utils/tuplestore.h"

struct WalReceiverConn
{
	/* Current connection to the primary, if any */
	PGconn	   *streamConn;
	/* Used to remember if the connection is logical or physical */
	bool		logical;
	/* Buffer for currently read records */
	char	   *recvBuf;
};

/* Prototypes for private functions */
static PGresult *walrcv_PQexec(PGconn *streamConn, const char *query);
static PGresult *walrcv_PQgetResult(PGconn *streamConn);
//static char *stringlist_to_identifierstr(PGconn *conn, List *strings);

struct Latch *MyLatch;
/*
 * Establish the connection to the primary server for XLOG streaming
 *
 * Returns NULL on error and fills the err with palloc'ed error message.
 */
WalReceiverConn *
walrcv_connect(const char *conninfo, bool logical, const char *appname,
				 char **err)
{
	WalReceiverConn *conn;
	PostgresPollingStatusType status;
	const char *keys[5];
	const char *vals[5];
	int			i = 0;

	/*
	 * We use the expand_dbname parameter to process the connection string (or
	 * URI), and pass some extra options.
	 */
	keys[i] = "dbname";
	vals[i] = conninfo;
	keys[++i] = "replication";
	vals[i] = logical ? "database" : "true";
	if (!logical)
	{
		/*
		 * The database name is ignored by the server in replication mode, but
		 * specify "replication" for .pgpass lookup.
		 */
		keys[++i] = "dbname";
		vals[i] = "replication";
	}
	keys[++i] = "fallback_application_name";
	vals[i] = appname;
	if (logical)
	{
		keys[++i] = "client_encoding";
		vals[i] = "utf8";
	}
	keys[++i] = NULL;
	vals[i] = NULL;

	Assert(i < sizeof(keys));

	conn = palloc0(sizeof(WalReceiverConn));
	conn->streamConn = PQconnectStartParams(keys, vals,
											 /* expand_dbname = */ true);
	if (PQstatus(conn->streamConn) == CONNECTION_BAD)
	{
		*err = PQerrorMessage(conn->streamConn);
		return NULL;
	}

	/*
	 * Poll connection until we have OK or FAILED status.
	 *
	 * Per spec for PQconnectPoll, first wait till socket is write-ready.
	 */
	status = PGRES_POLLING_WRITING;
	do
	{
		status = PQconnectPoll(conn->streamConn);
	} while (status != PGRES_POLLING_OK && status != PGRES_POLLING_FAILED);

	if (PQstatus(conn->streamConn) != CONNECTION_OK)
	{
		*err = PQerrorMessage(conn->streamConn);
		return NULL;
	}

	conn->logical = logical;

	return conn;
}

/*
 * Check that primary's system identifier matches ours, and fetch the current
 * timeline ID of the primary.
 */
char *
walrcv_identify_system(WalReceiverConn *conn, TimeLineID *primary_tli)
{
	PGresult   *res;
	char	   *primary_sysid;

	/*
	 * Get the system identifier and timeline ID as a DataRow message from the
	 * primary server.
	 */
	res = walrcv_PQexec(conn->streamConn, "IDENTIFY_SYSTEM");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		PQclear(res);
		ereport(ERROR,
				(errmsg("could not receive database system identifier and timeline ID from "
						"the primary server: %s",
						(PQerrorMessage(conn->streamConn)))));
	}
	if (PQnfields(res) < 3 || PQntuples(res) != 1)
	{
		int			ntuples = PQntuples(res);
		int			nfields = PQnfields(res);

		PQclear(res);
		ereport(ERROR,
				(errmsg("invalid response from primary server"),
				 errdetail("Could not identify system: got %d rows and %d fields, expected %d rows and %d or more fields.",
						   ntuples, nfields, 3, 1)));
	}
	primary_sysid = pstrdup(PQgetvalue(res, 0, 0));
	sscanf(PQgetvalue(res, 0, 1), "%d", primary_tli);
	//*primary_tli = pg_strtoint32(PQgetvalue(res, 0, 1));
	PQclear(res);

	return primary_sysid;
}

/*
 * Start streaming WAL data from given streaming options.
 *
 * Returns true if we switched successfully to copy-both mode. False
 * means the server received the command and executed it successfully, but
 * didn't switch to copy-mode.  That means that there was no WAL on the
 * requested timeline and starting point, because the server switched to
 * another timeline at or before the requested starting point. On failure,
 * throws an ERROR.
 */
bool
walrcv_startstreaming(WalReceiverConn *conn,
						const WalRcvStreamOptions *options)
{
	StringInfoData cmd;
	PGresult   *res;

	Assert(options->logical == conn->logical);
	Assert(options->slotname || !options->logical);

	initStringInfo(&cmd);

	/* Build the command. */
	appendStringInfoString(&cmd, "START_REPLICATION");
	if (options->slotname != NULL)
		appendStringInfo(&cmd, " SLOT \"%s\"",
						 options->slotname);

	if (options->logical)
		appendStringInfoString(&cmd, " LOGICAL");

	appendStringInfo(&cmd, " %X/%X",
					 (uint32) (options->startpoint >> 32),
					 (uint32) options->startpoint);

	/*
	 * Additional options are different depending on if we are doing logical
	 * or physical replication.
	 */
	if (options->logical)
	{
		char	   *pubnames_str;
		//List	   *pubnames;
		char	   *pubnames_literal;

		appendStringInfoString(&cmd, " (");

		appendStringInfo(&cmd, "proto_version '%u'",
						 options->proto.logical.proto_version);

		//pubnames = options->proto.logical.publication_names;
		//pubnames_str = stringlist_to_identifierstr(conn->streamConn, pubnames);
		pubnames_str = options->proto.logical.publication_names;
		if (!pubnames_str)
			ereport(ERROR,
					(errmsg("could not start WAL streaming: %s",
							(PQerrorMessage(conn->streamConn)))));
		pubnames_literal = PQescapeLiteral(conn->streamConn, pubnames_str,
										   strlen(pubnames_str));
		if (!pubnames_literal)
			ereport(ERROR,
					(errmsg("could not start WAL streaming: %s",
							(PQerrorMessage(conn->streamConn)))));
		appendStringInfo(&cmd, ", publication_names %s", pubnames_literal);
		PQfreemem(pubnames_literal);
		//pfree(pubnames_str);

		appendStringInfoChar(&cmd, ')');
	}
	else
		appendStringInfo(&cmd, " TIMELINE %u",
						 options->proto.physical.startpointTLI);

	/* Start streaming. */
	res = walrcv_PQexec(conn->streamConn, cmd.data);
	pfree(cmd.data);

	if (PQresultStatus(res) == PGRES_COMMAND_OK)
	{
		PQclear(res);
		return false;
	}
	else if (PQresultStatus(res) != PGRES_COPY_BOTH)
	{
		PQclear(res);
		ereport(ERROR,
				(errmsg("could not start WAL streaming: %s",
						(PQerrorMessage(conn->streamConn)))));
	}
	PQclear(res);
	return true;
}

/*
 * Stop streaming WAL data. Returns the next timeline's ID in *next_tli, as
 * reported by the server, or 0 if it did not report it.
 */
void
walrcv_endstreaming(WalReceiverConn *conn, TimeLineID *next_tli)
{
	PGresult   *res;

	/*
	 * Send copy-end message.  As in walrcv_PQexec, this could theoretically
	 * block, but the risk seems small.
	 */
	if (PQputCopyEnd(conn->streamConn, NULL) <= 0 ||
		PQflush(conn->streamConn))
		ereport(ERROR,
				(errmsg("could not send end-of-streaming message to primary: %s",
						(PQerrorMessage(conn->streamConn)))));

	*next_tli = 0;

	/*
	 * After COPY is finished, we should receive a result set indicating the
	 * next timeline's ID, or just CommandComplete if the server was shut
	 * down.
	 *
	 * If we had not yet received CopyDone from the backend, PGRES_COPY_OUT is
	 * also possible in case we aborted the copy in mid-stream.
	 */
	res = walrcv_PQgetResult(conn->streamConn);
	if (PQresultStatus(res) == PGRES_TUPLES_OK)
	{
		/*
		 * Read the next timeline's ID. The server also sends the timeline's
		 * starting point, but it is ignored.
		 */
		if (PQnfields(res) < 2 || PQntuples(res) != 1)
			ereport(ERROR,
					(errmsg("unexpected result set after end-of-streaming")));
		sscanf(PQgetvalue(res, 0, 0), "%d", next_tli);
		//*next_tli = pg_strtoint32(PQgetvalue(res, 0, 0));
		PQclear(res);

		/* the result set should be followed by CommandComplete */
		res = walrcv_PQgetResult(conn->streamConn);
	}
	else if (PQresultStatus(res) == PGRES_COPY_OUT)
	{
		PQclear(res);

		/* End the copy */
		if (PQendcopy(conn->streamConn))
			ereport(ERROR,
					(errmsg("error while shutting down streaming COPY: %s",
							(PQerrorMessage(conn->streamConn)))));

		/* CommandComplete should follow */
		res = walrcv_PQgetResult(conn->streamConn);
	}

	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		ereport(ERROR,
				(errmsg("error reading result of streaming command: %s",
						(PQerrorMessage(conn->streamConn)))));
	PQclear(res);

	/* Verify that there are no more results */
	res = walrcv_PQgetResult(conn->streamConn);
	if (res != NULL)
		ereport(ERROR,
				(errmsg("unexpected result after CommandComplete: %s",
						(PQerrorMessage(conn->streamConn)))));
}

/*
 * Send a query and wait for the results by using the asynchronous libpq
 * functions and socket readiness events.
 *
 * We must not use the regular blocking libpq functions like PQexec()
 * since they are uninterruptible by signals on some platforms, such as
 * Windows.
 *
 * The function is modeled on PQexec() in libpq, but only implements
 * those parts that are in use in the walreceiver api.
 *
 * May return NULL, rather than an error result, on failure.
 */
static PGresult *
walrcv_PQexec(PGconn *streamConn, const char *query)
{
	PGresult   *lastResult = NULL;

	/*
	 * PQexec() silently discards any prior query results on the connection.
	 * This is not required for this function as it's expected that the caller
	 * (which is this library in all cases) will behave correctly and we don't
	 * have to be backwards compatible with old libpq.
	 */

	/*
	 * Submit the query.  Since we don't use non-blocking mode, this could
	 * theoretically block.  In practice, since we don't send very long query
	 * strings, the risk seems negligible.
	 */
	if (!PQsendQuery(streamConn, query))
		return NULL;

	for (;;)
	{
		/* Wait for, and collect, the next PGresult. */
		PGresult   *result;

		result = walrcv_PQgetResult(streamConn);
		if (result == NULL)
			break;				/* query is complete, or failure */

		/*
		 * Emulate PQexec()'s behavior of returning the last result when there
		 * are many.  We are fine with returning just last error message.
		 */
		PQclear(lastResult);
		lastResult = result;

		if (PQresultStatus(lastResult) == PGRES_COPY_IN ||
			PQresultStatus(lastResult) == PGRES_COPY_OUT ||
			PQresultStatus(lastResult) == PGRES_COPY_BOTH ||
			PQstatus(streamConn) == CONNECTION_BAD)
			break;
	}

	return lastResult;
}

/*
 * Perform the equivalent of PQgetResult(), but watch for interrupts.
 */
static PGresult *
walrcv_PQgetResult(PGconn *streamConn)
{
	/*
	 * Collect data until PQgetResult is ready to get the result without
	 * blocking.
	 */
	while (PQisBusy(streamConn))
	{
		/* Consume whatever data is available from the socket */
		if (PQconsumeInput(streamConn) == 0)
		{
			/* trouble; return NULL */
			return NULL;
		}
	}

	/* Now we can collect and return the next PGresult */
	return PQgetResult(streamConn);
}

/*
 * Disconnect connection to primary, if any.
 */
void
walrcv_disconnect(WalReceiverConn *conn)
{
	PQfinish(conn->streamConn);
	if (conn->recvBuf != NULL)
		PQfreemem(conn->recvBuf);
	pfree(conn);
}

/*
 * Receive a message available from XLOG stream.
 *
 * Returns:
 *
 *	 If data was received, returns the length of the data. *buffer is set to
 *	 point to a buffer holding the received message. The buffer is only valid
 *	 until the next walrcv_* call.
 *
 *	 If no data was available immediately, returns 0, and *wait_fd is set to a
 *	 socket descriptor which can be waited on before trying again.
 *
 *	 -1 if the server ended the COPY.
 *
 * ereports on error.
 */
int
walrcv_receive(WalReceiverConn *conn, char **buffer,
				 pgsocket *wait_fd)
{
	int			rawlen;

	if (conn->recvBuf != NULL)
		PQfreemem(conn->recvBuf);
	conn->recvBuf = NULL;

	/* Try to receive a CopyData message */
	rawlen = PQgetCopyData(conn->streamConn, &conn->recvBuf, 1);
	if (rawlen == 0)
	{
		/* Try consuming some data. */
		if (PQconsumeInput(conn->streamConn) == 0)
			ereport(ERROR,
					(errmsg("could not receive data from WAL stream: %s",
							(PQerrorMessage(conn->streamConn)))));

		/* Now that we've consumed some input, try again */
		rawlen = PQgetCopyData(conn->streamConn, &conn->recvBuf, 1);
		if (rawlen == 0)
		{
			/* Tell caller to try again when our socket is ready. */
			*wait_fd = PQsocket(conn->streamConn);
			return 0;
		}
	}
	if (rawlen == -1)			/* end-of-streaming or error */
	{
		PGresult   *res;

		res = walrcv_PQgetResult(conn->streamConn);
		if (PQresultStatus(res) == PGRES_COMMAND_OK)
		{
			PQclear(res);

			/* Verify that there are no more results. */
			res = walrcv_PQgetResult(conn->streamConn);
			if (res != NULL)
			{
				PQclear(res);

				/*
				 * If the other side closed the connection orderly (otherwise
				 * we'd seen an error, or PGRES_COPY_IN) don't report an error
				 * here, but let callers deal with it.
				 */
				if (PQstatus(conn->streamConn) == CONNECTION_BAD)
					return -1;

				ereport(ERROR,
						(errmsg("unexpected result after CommandComplete: %s",
								PQerrorMessage(conn->streamConn))));
			}

			return -1;
		}
		else if (PQresultStatus(res) == PGRES_COPY_IN)
		{
			PQclear(res);
			return -1;
		}
		else
		{
			PQclear(res);
			ereport(ERROR,
					(errmsg("could not receive data from WAL stream: %s",
							(PQerrorMessage(conn->streamConn)))));
		}
	}
	if (rawlen < -1)
		ereport(ERROR,
				(errmsg("could not receive data from WAL stream: %s",
						(PQerrorMessage(conn->streamConn)))));

	/* Return received messages to caller */
	*buffer = conn->recvBuf;
	return rawlen;
}

/*
 * Send a message to XLOG stream.
 *
 * ereports on error.
 */
void
walrcv_send(WalReceiverConn *conn, const char *buffer, int nbytes)
{
	if (PQputCopyData(conn->streamConn, buffer, nbytes) <= 0 ||
		PQflush(conn->streamConn))
		ereport(ERROR,
				(errmsg("could not send data to WAL stream: %s",
						(PQerrorMessage(conn->streamConn)))));
}


/*
 * Create new replication slot.
 * Returns the name of the exported snapshot for logical slot or NULL for
 * physical slot.
 */
char *
walrcv_create_slot(WalReceiverConn *conn, const char *slotname,
					 bool temporary, CRSSnapshotAction snapshot_action)
{
	PGresult   *res;
	StringInfoData cmd;
	char	   *snapshot;

	initStringInfo(&cmd);

	appendStringInfo(&cmd, "CREATE_REPLICATION_SLOT \"%s\"", slotname);

	if (temporary)
		appendStringInfoString(&cmd, " TEMPORARY");

	if (conn->logical)
	{
		appendStringInfoString(&cmd, " LOGICAL pgoutput");
		switch (snapshot_action)
		{
			case CRS_EXPORT_SNAPSHOT:
				appendStringInfoString(&cmd, " EXPORT_SNAPSHOT");
				break;
			case CRS_NOEXPORT_SNAPSHOT:
				appendStringInfoString(&cmd, " NOEXPORT_SNAPSHOT");
				break;
			case CRS_USE_SNAPSHOT:
				appendStringInfoString(&cmd, " USE_SNAPSHOT");
				break;
		}
	}

	res = walrcv_PQexec(conn->streamConn, cmd.data);
	pfree(cmd.data);

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		PQclear(res);
		ereport(ERROR,
				(errmsg("could not create replication slot \"%s\": %s",
						slotname, PQerrorMessage(conn->streamConn))));
	}

	if (!PQgetisnull(res, 0, 2))
		snapshot = pstrdup(PQgetvalue(res, 0, 2));
	else
		snapshot = NULL;

	PQclear(res);

	return snapshot;
}

/*
 * Given a List of strings, return it as single comma separated
 * string, quoting identifiers as needed.
 *
 * This is essentially the reverse of SplitIdentifierString.
 *
 * The caller should free the result.
 */
/*static char *
stringlist_to_identifierstr(PGconn *conn, List *strings)
{
	ListCell   *lc;
	StringInfoData res;
	bool		first = true;

	initStringInfo(&res);

	foreach(lc, strings)
	{
		char	   *val = strVal(lfirst(lc));
		char	   *val_escaped;

		if (first)
			first = false;
		else
			appendStringInfoChar(&res, ',');

		val_escaped = PQescapeIdentifier(conn, val, strlen(val));
		if (!val_escaped)
		{
			free(res.data);
			return NULL;
		}
		appendStringInfoString(&res, val_escaped);
		PQfreemem(val_escaped);
	}

	return res.data;
}*/