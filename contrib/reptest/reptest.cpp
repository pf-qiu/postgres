#include "walproto.h"

static void apply_dispatch(StringInfo s);
static void send_feedback(WalReceiverConn *wrconn, XLogRecPtr recvpos);

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        printf("%s connstr pub sub\n", argv[0]);
        return 0;
    }
    char *err;
    WalReceiverConn *wrconn = NULL;
    TimeLineID startpointTLI;
    WalRcvStreamOptions options;
    
    char* pubname = argv[2];
    char* subname = argv[3];
    XLogRecPtr origin_startpos = 0;

    wrconn = walrcv_connect(argv[1], true, subname, &err);
    if (wrconn == NULL)
    {
        fprintf(stderr, "could not connect to the publisher: %s\n", err);
        return 1;
    }
    
    /*
	 * We don't really use the output identify_system for anything but it
	 * does some initializations on the upstream so let's still call it.
	 */
    (void)walrcv_identify_system(wrconn, &startpointTLI);

    XLogRecPtr lsn;
    char* snapshot = walrcv_create_slot(wrconn, subname, true, CRS_NOEXPORT_SNAPSHOT);
    printf("snapshot %s:\n", snapshot);

    /* Build logical replication streaming options. */
    options.logical = true;
    options.startpoint = origin_startpos;
    options.slotname = subname;
    options.proto.logical.proto_version = LOGICALREP_PROTO_VERSION_NUM;
    options.proto.logical.publication_names = pubname;

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