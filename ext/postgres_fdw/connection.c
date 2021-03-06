/*-------------------------------------------------------------------------
 *
 * connection.c
 *		  Connection management functions for postgres_fdw
 *
 * Portions Copyright (c) 2012-2018, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  contrib/postgres_fdw/connection.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "postgres_fdw.h"

#include "access/global_snapshot.h"
#include "access/htup_details.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "access/xlog.h" /* GetSystemIdentifier() */
#include "catalog/pg_user_mapping.h"
#include "libpq-int.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/latch.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/snapshot.h"
#include "utils/syscache.h"


/*
 * Connection cache hash table entry
 *
 * The lookup key in this hash table is the user mapping OID. We use just one
 * connection per user mapping ID, which ensures that all the scans use the
 * same snapshot during a query.  Using the user mapping OID rather than
 * the foreign server OID + user OID avoids creating multiple connections when
 * the public user mapping applies to all user OIDs.
 *
 * The "conn" pointer can be NULL if we don't currently have a live connection.
 * When we do have a connection, xact_depth tracks the current depth of
 * transactions and subtransactions open on the remote side.  We need to issue
 * commands at the same nesting depth on the remote as we're executing at
 * ourselves, so that rolling back a subtransaction will kill the right
 * queries and not the wrong ones.
 */
typedef Oid ConnCacheKey;

struct ConnCacheEntry
{
	ConnCacheKey key;			/* hash key (must be first) */
	PGconn	   *conn;			/* connection to foreign server, or NULL */
	WaitEventSet *wait_set;		/* for data from server ready notifications */
	/* Remaining fields are invalid when conn is NULL: */
	int			xact_depth;		/* 0 = no xact open, 1 = main xact open, 2 =
								 * one level of subxact open, etc */
	bool		have_prep_stmt; /* have we prepared any stmts in this xact? */
	bool		have_error;		/* have any subxacts aborted in this xact? */
	bool		changing_xact_state;	/* xact state change in process */
	bool		invalidated;	/* true if reconnect is pending */
	uint32		server_hashvalue;	/* hash value of foreign server OID */
	uint32		mapping_hashvalue;	/* hash value of user mapping OID */
	bool		copy_from_started;	/* COPY FROM in progress on this conn */
} ;

/*
 * Connection cache (initialized on first use)
 */
static HTAB *ConnectionHash = NULL;

/*
 * FdwTransactionState
 *
 * Holds number of open remote transactions and shared state
 * needed for all connection entries.
 */
typedef struct FdwTransactionState
{
	char		gid[GIDSIZE];
	int			nparticipants;
	GlobalCSN	global_csn;
	bool		two_phase_commit;
} FdwTransactionState;
static FdwTransactionState *fdwTransState;

/* for assigning cursor numbers and prepared statement numbers */
static unsigned int cursor_number = 0;
static unsigned int prep_stmt_number = 0;

/* tracks whether any work is needed in callback functions */
static bool xact_got_connection = false;

/* counter of prepared tx made by this backend */
static int two_phase_xact_count = 0;

/* prototypes of private functions */
static void connect_pg_server(ConnCacheEntry *entry, ForeignServer *server,
							  UserMapping *user);
static void disconnect_pg_server(ConnCacheEntry *entry);
static void check_conn_params(const char **keywords, const char **values, UserMapping *user);
static void configure_remote_session(ConnCacheEntry *entry);
static void do_sql_command(ConnCacheEntry *entry, const char *sql);
static void begin_remote_xact(ConnCacheEntry *entry);
static void pgfdw_xact_callback(XactEvent event, void *arg);
static void deallocate_prepared_stmts(ConnCacheEntry *entry);
static void pgfdw_subxact_callback(SubXactEvent event,
					   SubTransactionId mySubid,
					   SubTransactionId parentSubid,
					   void *arg);
static void pgfdw_inval_callback(Datum arg, int cacheid, uint32 hashvalue);
static void pgfdw_reject_incomplete_xact_state_change(ConnCacheEntry *entry);
static bool pgfdw_cancel_query(ConnCacheEntry *entry);
static bool pgfdw_exec_cleanup_query(ConnCacheEntry *entry, const char *query,
						 bool ignore_errors);
static bool pgfdw_get_cleanup_result(ConnCacheEntry *entry, TimestampTz endtime,
						 PGresult **result);


/*
 * Get a ConnCacheEntry which can be used to execute queries on the remote PostgreSQL
 * server with the user's authorization.  A new connection is established
 * if we don't already have a suitable one, and a transaction is opened at
 * the right subtransaction nesting depth if we didn't do that already.
 *
 * will_prep_stmt must be true if caller intends to create any prepared
 * statements.  Since those don't go away automatically at transaction end
 * (not even on error), we need this flag to cue manual cleanup.
 */
ConnCacheEntry *
GetConnectionCopyFrom(UserMapping *user, bool will_prep_stmt,
					  bool **copy_from_started)
{
	bool		found;
	ConnCacheEntry *entry;
	ConnCacheKey key;

	/* First time through, initialize connection cache hashtable */
	if (ConnectionHash == NULL)
	{
		HASHCTL		ctl;

		MemSet(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(ConnCacheKey);
		ctl.entrysize = sizeof(ConnCacheEntry);
		/* allocate ConnectionHash in the cache context */
		ctl.hcxt = CacheMemoryContext;
		ConnectionHash = hash_create("postgres_fdw connections", 8,
									 &ctl,
									 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

		/*
		 * Register some callback functions that manage connection cleanup.
		 * This should be done just once in each backend.
		 */
		RegisterXactCallback(pgfdw_xact_callback, NULL);
		RegisterSubXactCallback(pgfdw_subxact_callback, NULL);
		CacheRegisterSyscacheCallback(FOREIGNSERVEROID,
									  pgfdw_inval_callback, (Datum) 0);
		CacheRegisterSyscacheCallback(USERMAPPINGOID,
									  pgfdw_inval_callback, (Datum) 0);
	}

	/* allocate FdwTransactionState */
	if (fdwTransState == NULL)
	{
		MemoryContext oldcxt;
		oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
		fdwTransState = palloc0(sizeof(FdwTransactionState));
		MemoryContextSwitchTo(oldcxt);
	}

	/* Set flag that we did GetConnection during the current transaction */
	xact_got_connection = true;

	/* Create hash key for the entry.  Assume no pad bytes in key struct */
	key = user->umid;

	/*
	 * Find or create cached entry for requested connection.
	 */
	entry = hash_search(ConnectionHash, &key, HASH_ENTER, &found);
	if (!found)
	{
		/*
		 * We need only clear "conn" here; remaining fields will be filled
		 * later when "conn" is set.
		 */
		entry->conn = NULL;
	}

	/* Reject further use of connections which failed abort cleanup. */
	pgfdw_reject_incomplete_xact_state_change(entry);

	/*
	 * If the connection needs to be remade due to invalidation, disconnect as
	 * soon as we're out of all transactions.
	 */
	if (entry->conn != NULL && entry->invalidated && entry->xact_depth == 0)
	{
		elog(DEBUG3, "closing connection %p for option changes to take effect",
			 entry->conn);
		disconnect_pg_server(entry);
	}

	/*
	 * We don't check the health of cached connection here, because it would
	 * require some overhead.  Broken connection will be detected when the
	 * connection is actually used.
	 */

	/*
	 * If cache entry doesn't have a connection, we have to establish a new
	 * connection.  (If connect_pg_server throws an error, the cache entry
	 * will remain in a valid empty state, ie conn == NULL.)
	 */
	if (entry->conn == NULL)
	{
		ForeignServer *server = GetForeignServer(user->serverid);

		/* Reset all transient state fields, to be sure all are clean */
		entry->xact_depth = 0;
		entry->have_prep_stmt = false;
		entry->have_error = false;
		entry->changing_xact_state = false;
		entry->invalidated = false;
		entry->copy_from_started = false;
		entry->server_hashvalue =
			GetSysCacheHashValue1(FOREIGNSERVEROID,
								  ObjectIdGetDatum(server->serverid));
		entry->mapping_hashvalue =
			GetSysCacheHashValue1(USERMAPPINGOID,
								  ObjectIdGetDatum(user->umid));

		/* Now try to make the connection */
		connect_pg_server(entry, server, user);

		elog(DEBUG3, "new postgres_fdw connection %p for server \"%s\" (user mapping oid %u, userid %u)",
			 entry->conn, server->servername, user->umid, user->userid);
	}

	/*
	 * Start a new transaction or subtransaction if needed.
	 */
	begin_remote_xact(entry);

	/* Remember if caller will prepare statements */
	entry->have_prep_stmt |= will_prep_stmt;

	if (copy_from_started)
		*copy_from_started = &(entry->copy_from_started);

	return entry;
}

PGconn *
ConnectionEntryGetConn(ConnCacheEntry *entry)
{
	return entry->conn;
}

ConnCacheEntry *
GetConnection(UserMapping *user, bool will_prep_stmt)
{
	return GetConnectionCopyFrom(user, will_prep_stmt, NULL);
}

/*
 * Connect to remote server using specified server and user mapping properties.
 */
static void
connect_pg_server(ConnCacheEntry *entry, ForeignServer *server, UserMapping *user)
{
	PGconn	   *volatile conn = NULL;

	entry->wait_set = NULL;

	/*
	 * Use PG_TRY block to ensure closing connection on error.
	 */
	PG_TRY();
	{
		const char **keywords;
		const char **values;
		int			n;

		/*
		 * Construct connection params from generic options of ForeignServer
		 * and UserMapping.  (Some of them might not be libpq options, in
		 * which case we'll just waste a few array slots.)  Add 3 extra slots
		 * for fallback_application_name, client_encoding, end marker.
		 */
		n = list_length(server->options) + list_length(user->options) + 3;
		keywords = (const char **) palloc(n * sizeof(char *));
		values = (const char **) palloc(n * sizeof(char *));

		n = 0;
		n += ExtractConnectionOptions(server->options,
									  keywords + n, values + n);
		n += ExtractConnectionOptions(user->options,
									  keywords + n, values + n);

		/* Use "postgres_fdw" as fallback_application_name. */
		keywords[n] = "fallback_application_name";
		values[n] = "postgres_fdw";
		n++;

		/* Set client_encoding so that libpq can convert encoding properly. */
		keywords[n] = "client_encoding";
		values[n] = GetDatabaseEncodingName();
		n++;

		keywords[n] = values[n] = NULL;

		/* verify connection parameters and make connection */
		check_conn_params(keywords, values, user);

		conn = PQconnectdbParams(keywords, values, false);
		if (!conn || PQstatus(conn) != CONNECTION_OK)
			ereport(ERROR,
					(errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION),
					 errmsg("could not connect to server \"%s\"",
							server->servername),
					 errdetail_internal("%s", pchomp(PQerrorMessage(conn)))));

		/*
		 * Check that non-superuser has used password to establish connection;
		 * otherwise, he's piggybacking on the postgres server's user
		 * identity. See also dblink_security_check() in contrib/dblink.
		 */
		if (!superuser_arg(user->userid) && !PQconnectionUsedPassword(conn))
			ereport(ERROR,
					(errcode(ERRCODE_S_R_E_PROHIBITED_SQL_STATEMENT_ATTEMPTED),
					 errmsg("password is required"),
					 errdetail("Non-superuser cannot connect if the server does not request a password."),
					 errhint("Target server's authentication method must be changed.")));

		entry->conn = conn;

		/* Here we will wait for the results */
		/* xxx check for postmaster death? */
		entry->wait_set = CreateWaitEventSet(TopMemoryContext, 2);
		AddWaitEventToSet(entry->wait_set, WL_LATCH_SET, PGINVALID_SOCKET,
						  MyLatch, NULL);
		AddWaitEventToSet(entry->wait_set, WL_SOCKET_READABLE, PQsocket(conn), NULL, NULL);

		/* Prepare new session for use */
		configure_remote_session(entry);

		pfree(keywords);
		pfree(values);
	}
	PG_CATCH();
	{
		/* Release WaitEventSet if we managed to create one */
		if (entry->wait_set)
		{
			FreeWaitEventSet(entry->wait_set);
			entry->wait_set = NULL;
		}
		/* Release PGconn data structure if we managed to create one */
		if (conn)
			PQfinish(conn);
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * Disconnect any open connection for a connection cache entry.
 */
static void
disconnect_pg_server(ConnCacheEntry *entry)
{
	if (entry->conn != NULL)
	{
		Assert(entry->wait_set);
		FreeWaitEventSet(entry->wait_set);
		entry->wait_set = NULL;
		PQfinish(entry->conn);
		entry->conn = NULL;
	}
}

/*
 * For non-superusers, insist that the connstr specify a password.  This
 * prevents a password from being picked up from .pgpass, a service file,
 * the environment, etc.  We don't want the postgres user's passwords
 * to be accessible to non-superusers.  (See also dblink_connstr_check in
 * contrib/dblink.)
 */
static void
check_conn_params(const char **keywords, const char **values, UserMapping *user)
{
	int			i;

	/* no check required if superuser */
	if (superuser_arg(user->userid))
		return;

	/* ok if params contain a non-empty password */
	for (i = 0; keywords[i] != NULL; i++)
	{
		if (strcmp(keywords[i], "password") == 0 && values[i][0] != '\0')
			return;
	}

	ereport(ERROR,
			(errcode(ERRCODE_S_R_E_PROHIBITED_SQL_STATEMENT_ATTEMPTED),
			 errmsg("password is required"),
			 errdetail("Non-superusers must provide a password in the user mapping.")));
}

/*
 * Issue SET commands to make sure remote session is configured properly.
 *
 * We do this just once at connection, assuming nothing will change the
 * values later.  Since we'll never send volatile function calls to the
 * remote, there shouldn't be any way to break this assumption from our end.
 * It's possible to think of ways to break it at the remote end, eg making
 * a foreign table point to a view that includes a set_config call ---
 * but once you admit the possibility of a malicious view definition,
 * there are any number of ways to break things.
 */
static void
configure_remote_session(ConnCacheEntry *entry)
{
	int			remoteversion = PQserverVersion(entry->conn);

	/* Force the search path to contain only pg_catalog (see deparse.c) */
	do_sql_command(entry, "SET search_path = pg_catalog");

	/*
	 * Set remote timezone; this is basically just cosmetic, since all
	 * transmitted and returned timestamptzs should specify a zone explicitly
	 * anyway.  However it makes the regression test outputs more predictable.
	 *
	 * We don't risk setting remote zone equal to ours, since the remote
	 * server might use a different timezone database.  Instead, use UTC
	 * (quoted, because very old servers are picky about case).
	 */
	do_sql_command(entry, "SET timezone = 'UTC'");

	/*
	 * Set values needed to ensure unambiguous data output from remote.  (This
	 * logic should match what pg_dump does.  See also set_transmission_modes
	 * in postgres_fdw.c.)
	 */
	do_sql_command(entry, "SET datestyle = ISO");
	if (remoteversion >= 80400)
		do_sql_command(entry, "SET intervalstyle = postgres");
	if (remoteversion >= 90000)
		do_sql_command(entry, "SET extra_float_digits = 3");
	else
		do_sql_command(entry, "SET extra_float_digits = 2");
}

/*
 * Convenience subroutine to issue a non-data-returning SQL command or
 * statement to remote node.
 */
static void
do_sql_command(ConnCacheEntry *entry, const char *sql)
{
	PGconn	   *conn = entry->conn;
	PGresult   *res;

	if (!PQsendQuery(conn, sql))
		pgfdw_report_error(ERROR, NULL, conn, false, sql);
	res = pgfdw_get_result(entry, sql);
	if (PQresultStatus(res) != PGRES_COMMAND_OK &&
		PQresultStatus(res) != PGRES_TUPLES_OK)
		pgfdw_report_error(ERROR, res, conn, true, sql);
	PQclear(res);
}

/*
 * Start remote transaction or subtransaction, if needed.
 *
 * Note that we always use at least REPEATABLE READ in the remote session.
 * This is so that, if a query initiates multiple scans of the same or
 * different foreign tables, we will get snapshot-consistent results from
 * those scans.  A disadvantage is that we can't provide sane emulation of
 * READ COMMITTED behavior --- it would be nice if we had some other way to
 * control which remote queries share a snapshot.
 */
static void
begin_remote_xact(ConnCacheEntry *entry)
{
	int			curlevel = GetCurrentTransactionNestLevel();
	char		sql[128];


	/* Start main transaction if we haven't yet */
	if (entry->xact_depth <= 0)
	{
		elog(DEBUG3, "starting remote transaction on connection %p",
			 entry->conn);

		if (UseGlobalSnapshots && (!IsolationUsesXactSnapshot() ||
								   IsolationIsSerializable()))
			elog(ERROR, "Global snapshots support only REPEATABLE READ");

		sprintf(sql, "START TRANSACTION %s; set application_name='pgfdw:%lld:%d';",
				IsolationIsSerializable() ? "ISOLATION LEVEL SERIALIZABLE" :
				UseRepeatableRead ? "ISOLATION LEVEL REPEATABLE READ" : "",
				(long long) GetSystemIdentifier(), MyProcPid);

		entry->changing_xact_state = true;
		do_sql_command(entry, sql);
		entry->xact_depth = 1;
		entry->changing_xact_state = false;

		if (UseGlobalSnapshots)
		{
			char import_sql[128];

			/* Export our snapshot */
			if (fdwTransState->global_csn == 0)
				fdwTransState->global_csn = ExportGlobalSnapshot();

			snprintf(import_sql, sizeof(import_sql),
				"SELECT pg_global_snapshot_import("UINT64_FORMAT")",
				fdwTransState->global_csn);

			do_sql_command(entry, import_sql);
		}

		fdwTransState->nparticipants += 1;
	}

	/*
	 * If we're in a subtransaction, stack up savepoints to match our level.
	 * This ensures we can rollback just the desired effects when a
	 * subtransaction aborts.
	 */
	while (entry->xact_depth < curlevel)
	{
		char		sql[64];

		snprintf(sql, sizeof(sql), "SAVEPOINT s%d", entry->xact_depth + 1);
		entry->changing_xact_state = true;
		do_sql_command(entry, sql);
		entry->xact_depth++;
		entry->changing_xact_state = false;
	}
}

/*
 * Release connection reference count created by calling GetConnection.
 */
void
ReleaseConnection(ConnCacheEntry *entry)
{
	/*
	 * Currently, we don't actually track connection references because all
	 * cleanup is managed on a transaction or subtransaction basis instead. So
	 * there's nothing to do here.
	 */
}

/*
 * Assign a "unique" number for a cursor.
 *
 * These really only need to be unique per connection within a transaction.
 * For the moment we ignore the per-connection point and assign them across
 * all connections in the transaction, but we ask for the connection to be
 * supplied in case we want to refine that.
 *
 * Note that even if wraparound happens in a very long transaction, actual
 * collisions are highly improbable; just be sure to use %u not %d to print.
 */
unsigned int
GetCursorNumber(ConnCacheEntry *entry)
{
	return ++cursor_number;
}

/*
 * Assign a "unique" number for a prepared statement.
 *
 * This works much like GetCursorNumber, except that we never reset the counter
 * within a session.  That's because we can't be 100% sure we've gotten rid
 * of all prepared statements on all connections, and it's not really worth
 * increasing the risk of prepared-statement name collisions by resetting.
 */
unsigned int
GetPrepStmtNumber(ConnCacheEntry *entry)
{
	return ++prep_stmt_number;
}

/*
 * Submit a query and wait for the result.
 *
 * This function is interruptible by signals.
 *
 * Caller is responsible for the error handling on the result.
 */
PGresult *
pgfdw_exec_query(ConnCacheEntry *entry, const char *query)
{
	/*
	 * Submit a query.  Since we don't use non-blocking mode, this also can
	 * block.  But its risk is relatively small, so we ignore that for now.
	 */
	if (!PQsendQuery(entry->conn, query))
		pgfdw_report_error(ERROR, NULL, entry->conn, false, query);

	/* Wait for the result. */
	return pgfdw_get_result(entry, query);
}

/*
 * Wait for the result from a prior asynchronous execution function call.
 *
 * This function offers quick responsiveness by checking for any interruptions.
 *
 * This function emulates PQexec()'s behavior of returning the last result
 * when there are many.
 *
 * Caller is responsible for the error handling on the result.
 */
PGresult *
pgfdw_get_result(ConnCacheEntry *entry, const char *query)
{
	PGconn	   *conn = entry->conn;
	PGresult   *volatile last_res = NULL;

	/* In what follows, do not leak any PGresults on an error. */
	PG_TRY();
	{
		for (;;)
		{
			PGresult   *res;

			while (PQisBusy(conn))
			{
				WaitEvent	ev;

				/* Sleep until there's something to do */
				WaitEventSetWait(entry->wait_set, -1L, &ev, 1, PG_WAIT_EXTENSION);
				ResetLatch(MyLatch);

				CHECK_FOR_INTERRUPTS();

				/* Data available in socket? */
				if (ev.events & WL_SOCKET_READABLE)
				{
					if (!PQconsumeInput(conn))
						pgfdw_report_error(ERROR, NULL, conn, false, query);
				}
			}

			res = PQgetResult(conn);
			if (res == NULL)
				break;			/* query is complete */

			PQclear(last_res);
			last_res = res;
		}
	}
	PG_CATCH();
	{
		PQclear(last_res);
		PG_RE_THROW();
	}
	PG_END_TRY();

	return last_res;
}

/*
 * Report an error we got from the remote server.
 *
 * elevel: error level to use (typically ERROR, but might be less)
 * res: PGresult containing the error
 * conn: connection we did the query on
 * clear: if true, PQclear the result (otherwise caller will handle it)
 * sql: NULL, or text of remote command we tried to execute
 *
 * Note: callers that choose not to throw ERROR for a remote error are
 * responsible for making sure that the associated ConnCacheEntry gets
 * marked with have_error = true.
 */
void
pgfdw_report_error(int elevel, PGresult *res, PGconn *conn,
				   bool clear, const char *sql)
{
	/* If requested, PGresult must be released before leaving this function. */
	PG_TRY();
	{
		char	   *diag_sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);
		char	   *message_primary = PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY);
		char	   *message_detail = PQresultErrorField(res, PG_DIAG_MESSAGE_DETAIL);
		char	   *message_hint = PQresultErrorField(res, PG_DIAG_MESSAGE_HINT);
		char	   *message_context = PQresultErrorField(res, PG_DIAG_CONTEXT);
		int			sqlstate;

		if (diag_sqlstate)
			sqlstate = MAKE_SQLSTATE(diag_sqlstate[0],
									 diag_sqlstate[1],
									 diag_sqlstate[2],
									 diag_sqlstate[3],
									 diag_sqlstate[4]);
		else
			sqlstate = ERRCODE_CONNECTION_FAILURE;

		/*
		 * If we don't get a message from the PGresult, try the PGconn.  This
		 * is needed because for connection-level failures, PQexec may just
		 * return NULL, not a PGresult at all.
		 */
		if (message_primary == NULL)
			message_primary = pchomp(PQerrorMessage(conn));

		ereport(elevel,
				(errcode(sqlstate),
				 message_primary ? errmsg_internal("%s", message_primary) :
				 errmsg("could not obtain message string for remote error"),
				 message_detail ? errdetail_internal("%s", message_detail) : 0,
				 message_hint ? errhint("%s", message_hint) : 0,
				 message_context ? errcontext("%s", message_context) : 0,
				 sql ? errcontext("remote SQL command: %s", sql) : 0));
	}
	PG_CATCH();
	{
		if (clear)
			PQclear(res);
		PG_RE_THROW();
	}
	PG_END_TRY();
	if (clear)
		PQclear(res);
}

/* Callback typedef for BroadcastStmt */
typedef bool (*BroadcastCmdResHandler) (PGresult *result, void *arg);

/* Broadcast sql in parallel to all ConnectionHash entries */
static bool
BroadcastStmt(char const * sql, unsigned expectedStatus,
				BroadcastCmdResHandler handler, void *arg)
{
	HASH_SEQ_STATUS scan;
	ConnCacheEntry *entry;
	bool		allOk = true;

	/* Broadcast sql */
	hash_seq_init(&scan, ConnectionHash);
	while ((entry = (ConnCacheEntry *) hash_seq_search(&scan)))
	{
		pgfdw_reject_incomplete_xact_state_change(entry);

		if (entry->xact_depth > 0 && entry->conn != NULL)
		{
			if (!PQsendQuery(entry->conn, sql))
			{
				PGresult   *res = PQgetResult(entry->conn);

				elog(WARNING, "Failed to send command %s", sql);
				pgfdw_report_error(WARNING, res, entry->conn, true, sql);
				PQclear(res);
			}
		}
	}

	/* Collect responses */
	hash_seq_init(&scan, ConnectionHash);
	while ((entry = (ConnCacheEntry *) hash_seq_search(&scan)))
	{
		if (entry->xact_depth > 0 && entry->conn != NULL)
		{
			PGresult   *result = PQgetResult(entry->conn);

			if (PQresultStatus(result) != expectedStatus ||
				(handler && !handler(result, arg)))
			{
				elog(WARNING, "Failed command %s: status=%d, expected status=%d", sql, PQresultStatus(result), expectedStatus);
				pgfdw_report_error(ERROR, result, entry->conn, true, sql);
				allOk = false;
			}
			PQclear(result);
			PQgetResult(entry->conn);	/* consume NULL result */
		}
	}

	return allOk;
}

/* Wrapper for broadcasting commands */
static bool
BroadcastCmd(char const *sql)
{
	return BroadcastStmt(sql, PGRES_COMMAND_OK, NULL, NULL);
}

/* Wrapper for broadcasting statements */
static bool
BroadcastFunc(char const *sql)
{
	return BroadcastStmt(sql, PGRES_TUPLES_OK, NULL, NULL);
}

/* Callback for selecting maximal csn */
static bool
MaxCsnCB(PGresult *result, void *arg)
{
	char		   *resp;
	GlobalCSN	   *max_csn = (GlobalCSN *) arg;
	GlobalCSN		csn = 0;

	resp = PQgetvalue(result, 0, 0);

	if (resp == NULL || (*resp) == '\0' ||
			sscanf(resp, UINT64_FORMAT, &csn) != 1)
		return false;

	if (*max_csn < csn)
		*max_csn = csn;

	return true;
}

/*
 * pgfdw_xact_callback --- cleanup at main-transaction end.
 */
static void
pgfdw_xact_callback(XactEvent event, void *arg)
{
	HASH_SEQ_STATUS scan;
	ConnCacheEntry *entry;

	/* Quick exit if no connections were touched in this transaction. */
	if (!xact_got_connection)
		return;

	/*
	 * Hack for shardman loader: it allows to do 2PC on user-issued
	 * prepare. In this case we won't be able to commit xacts because we we
	 * don't record participants info anywhere; this must be done by loader or
	 * human behind it.
	 */
	if (event == XACT_EVENT_PRE_PREPARE &&
		UseGlobalSnapshots &&
		strncmp("pgfdw:", GetPrepareGid(), strlen("pgfdw:")) == 0 &&
		strstr(GetPrepareGid(), "shmnloader") != 0)
	{
		/*
		 * Remember gid. We will PREPARE on other nodes and finish global
		 * snaps on XACT_EVENT_POST_PREPARE.
		 */
		strncpy(fdwTransState->gid, GetPrepareGid(), GIDSIZE);
		/*
		 * xact_depth and fdwTransState will be cleaned up on
		 * XACT_EVENT_POST_PREPARE.
		 */
		return;
	}
	if (event == XACT_EVENT_PREPARE && fdwTransState->gid[0] != '\0')
		return; /* prevent cleanup */
	if (event == XACT_EVENT_POST_PREPARE)
	{
		GlobalCSN	max_csn = InProgressGlobalCSN;
		GlobalCSN	my_csn = InProgressGlobalCSN;
		bool	res;
		char   *sql;

		if (fdwTransState->gid[0] == '\0')
		{
			/*
			 * Nothing to do here; since this cb is not present in vanilla,
			 * exit to avoid harming state machine
			 */
			return;
		}
		sql = psprintf("PREPARE TRANSACTION '%s'", fdwTransState->gid);
		res = BroadcastCmd(sql);
		if (!res)
			goto error;

		/* Broadcast pg_global_snapshot_prepare() */
		my_csn = GlobalSnapshotPrepareTwophase(fdwTransState->gid);

		sql = psprintf("SELECT pg_global_snapshot_prepare('%s')",
					   fdwTransState->gid);
		res = BroadcastStmt(sql, PGRES_TUPLES_OK, MaxCsnCB, &max_csn);
		if (!res)
			goto error;

		/* select maximal global csn */
		if (my_csn > max_csn)
			max_csn = my_csn;

		/* Broadcast pg_global_snapshot_assign() */
		GlobalSnapshotAssignCsnTwoPhase(fdwTransState->gid, max_csn);
		sql = psprintf("SELECT pg_global_snapshot_assign('%s',"UINT64_FORMAT")",
					   fdwTransState->gid, max_csn);
		res = BroadcastFunc(sql);

error:
		if (!res)
		{
			sql = psprintf("ABORT PREPARED '%s'", fdwTransState->gid);
			BroadcastCmd(sql);
			elog(ERROR, "failed to PREPARE transaction on remote node, ABORT PREPARED this xact");
		}
	}

	/*
	 * Handle possible two-phase commit.
	 */
	if (event == XACT_EVENT_PARALLEL_PRE_COMMIT || event == XACT_EVENT_PRE_COMMIT)
	{
		bool include_local_tx = false;

		/* Should we take into account this node? */
		if (TransactionIdIsValid(GetCurrentTransactionIdIfAny()))
		{
			include_local_tx = true;
			fdwTransState->nparticipants += 1;
		}

		/* Switch to 2PC mode there were more than one participant */
		if (UseGlobalSnapshots && fdwTransState->nparticipants > 1)
			fdwTransState->two_phase_commit = true;

		if (fdwTransState->two_phase_commit)
		{
			GlobalCSN	max_csn = InProgressGlobalCSN,
						my_csn = InProgressGlobalCSN;
			bool	res;
			char   *sql;

			snprintf(fdwTransState->gid,
					 GIDSIZE,
					 "pgfdw:%lld:%llu:%d:%u:%d:%d",
					 (long long) GetCurrentTimestamp(),
					 (long long) GetSystemIdentifier(),
					 MyProcPid,
					 GetCurrentTransactionIdIfAny(),
					 ++two_phase_xact_count,
					 fdwTransState->nparticipants);

			/* Broadcast PREPARE */
			sql = psprintf("PREPARE TRANSACTION '%s'", fdwTransState->gid);
			res = BroadcastCmd(sql);
			if (!res)
				goto error_user2pc;

			/* Broadcast pg_global_snapshot_prepare() */
			if (include_local_tx)
				my_csn = GlobalSnapshotPrepareCurrent();

			sql = psprintf("SELECT pg_global_snapshot_prepare('%s')",
						   fdwTransState->gid);
			res = BroadcastStmt(sql, PGRES_TUPLES_OK, MaxCsnCB, &max_csn);
			if (!res)
				goto error_user2pc;

			/* select maximal global csn */
			if (include_local_tx && my_csn > max_csn)
				max_csn = my_csn;

			/* Broadcast pg_global_snapshot_assign() */
			if (include_local_tx)
				GlobalSnapshotAssignCsnCurrent(max_csn);
			sql = psprintf("SELECT pg_global_snapshot_assign('%s',"UINT64_FORMAT")",
						   fdwTransState->gid, max_csn);
			res = BroadcastFunc(sql);

error_user2pc:
			if (!res)
			{
				sql = psprintf("ABORT PREPARED '%s'", fdwTransState->gid);
				BroadcastCmd(sql);
				elog(ERROR, "Failed to PREPARE transaction on remote node");
			}

			/*
			 * Do not fall down. Consequent COMMIT event will clean thing up.
			 */
			return;
		}
	}

	/* COMMIT open transaction of we were doing 2PC */
	if (fdwTransState->two_phase_commit &&
		(event == XACT_EVENT_PARALLEL_COMMIT || event == XACT_EVENT_COMMIT))
	{
		BroadcastCmd(psprintf("COMMIT PREPARED '%s'", fdwTransState->gid));
	}

	/*
	 * Scan all connection cache entries to find open remote transactions, and
	 * close them.
	 */
	hash_seq_init(&scan, ConnectionHash);
	while ((entry = (ConnCacheEntry *) hash_seq_search(&scan)))
	{
		/* Ignore cache entry if no open connection right now */
		if (entry->conn == NULL)
			continue;

		/* If it has an open remote transaction, try to close it */
		if (entry->xact_depth > 0)
		{
			bool		abort_cleanup_failure = false;

			elog(DEBUG3, "closing remote transaction on connection %p",
				 entry->conn);

			switch (event)
			{
				case XACT_EVENT_PARALLEL_PRE_COMMIT:
				case XACT_EVENT_PRE_COMMIT:
					Assert(!fdwTransState->two_phase_commit);

					/*
					 * If abort cleanup previously failed for this connection,
					 * we can't issue any more commands against it.
					 */
					pgfdw_reject_incomplete_xact_state_change(entry);

					/* Commit all remote transactions during pre-commit */
					entry->changing_xact_state = true;
					do_sql_command(entry, "COMMIT TRANSACTION");
					entry->changing_xact_state = false;

					deallocate_prepared_stmts(entry);
					break;
				case XACT_EVENT_PRE_PREPARE:

					if (fdwTransState->gid[0] != '\0')
						/* See comments above */
						break;

					/*
					 * We disallow remote transactions that modified anything,
					 * since it's not very reasonable to hold them open until
					 * the prepared transaction is committed.  For the moment,
					 * throw error unconditionally; later we might allow
					 * read-only cases.  Note that the error will cause us to
					 * come right back here with event == XACT_EVENT_ABORT, so
					 * we'll clean up the connection state at that point.
					 */
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								errmsg("cannot prepare a transaction that modified remote tables")));
					break;
				case XACT_EVENT_PARALLEL_COMMIT:
				case XACT_EVENT_COMMIT:
					if (fdwTransState->two_phase_commit)
						deallocate_prepared_stmts(entry);
					else /* Pre-commit should have closed the open transaction */
						elog(ERROR, "missed cleaning up connection during pre-commit");
					break;
				case XACT_EVENT_PREPARE:
					if (fdwTransState->gid[0] != '\0')
						break;

					/* Pre-commit should have closed the open transaction */
					elog(ERROR, "missed cleaning up connection during pre-commit");
					break;
				case XACT_EVENT_PARALLEL_ABORT:
				case XACT_EVENT_ABORT:

					/*
					 * Don't try to clean up the connection if we're already
					 * in error recursion trouble.
					 */
					if (in_error_recursion_trouble())
						entry->changing_xact_state = true;

					/*
					 * If connection is already unsalvageable, don't touch it
					 * further.
					 */
					if (entry->changing_xact_state)
						break;

					/*
					 * Mark this connection as in the process of changing
					 * transaction state.
					 */
					entry->changing_xact_state = true;

					/* Assume we might have lost track of prepared statements */
					entry->have_error = true;

					/*
					 * If a command has been submitted to the remote server by
					 * using an asynchronous execution function, the command
					 * might not have yet completed.  Check to see if a
					 * command is still being processed by the remote server,
					 * and if so, request cancellation of the command.
					 */
					if (PQtransactionStatus(entry->conn) == PQTRANS_ACTIVE &&
						!pgfdw_cancel_query(entry))
					{
						/* Unable to cancel running query. */
						abort_cleanup_failure = true;
					}
					else if (!pgfdw_exec_cleanup_query(entry,
													   "ABORT TRANSACTION",
													   false))
					{
						/* Unable to abort remote transaction. */
						abort_cleanup_failure = true;
					}
					else if (entry->have_prep_stmt && entry->have_error &&
							 !pgfdw_exec_cleanup_query(entry,
													   "DEALLOCATE ALL",
													   true))
					{
						/* Trouble clearing prepared statements. */
						abort_cleanup_failure = true;
					}
					else
					{
						entry->have_prep_stmt = false;
						entry->have_error = false;
					}

					/* Disarm changing_xact_state if it all worked. */
					entry->changing_xact_state = abort_cleanup_failure;
					break;
				case XACT_EVENT_POST_PREPARE:
					/*
					 * New event can break our state machine, so let's list
					 * them here explicitely and force compiler warning in
					 * case of unhandled event.
					 */
					break;

			}
		}

		/* Reset state to show we're out of a transaction */
		entry->xact_depth = 0;

		/*
		 * If the connection isn't in a good idle state, discard it to
		 * recover. Next GetConnection will open a new connection.
		 */
		if (PQstatus(entry->conn) != CONNECTION_OK ||
			PQtransactionStatus(entry->conn) != PQTRANS_IDLE ||
			entry->changing_xact_state)
		{
			elog(DEBUG3, "discarding connection %p", entry->conn);
			disconnect_pg_server(entry);
		}
	}

	/*
	 * Regardless of the event type, we can now mark ourselves as out of the
	 * transaction.  (Note: if we are here during PRE_COMMIT or PRE_PREPARE,
	 * this saves a useless scan of the hashtable during COMMIT or PREPARE.)
	 */
	xact_got_connection = false;

	/* Also reset cursor numbering for next transaction */
	cursor_number = 0;

	/* Reset fdwTransState */
	memset(fdwTransState, '\0', sizeof(FdwTransactionState));
}

/*
 * If there were any errors in subtransactions, and we
 * made prepared statements, do a DEALLOCATE ALL to make
 * sure we get rid of all prepared statements. This is
 * annoying and not terribly bulletproof, but it's
 * probably not worth trying harder.
 *
 * DEALLOCATE ALL only exists in 8.3 and later, so this
 * constrains how old a server postgres_fdw can
 * communicate with.  We intentionally ignore errors in
 * the DEALLOCATE, so that we can hobble along to some
 * extent with older servers (leaking prepared statements
 * as we go; but we don't really support update operations
 * pre-8.3 anyway).
 */
static void
deallocate_prepared_stmts(ConnCacheEntry *entry)
{
	PGresult   *res;

	if (entry->have_prep_stmt && entry->have_error)
	{
		res = PQexec(entry->conn, "DEALLOCATE ALL");
		PQclear(res);
	}
	entry->have_prep_stmt = false;
	entry->have_error = false;
}

/*
 * pgfdw_subxact_callback --- cleanup at subtransaction end.
 */
static void
pgfdw_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
					   SubTransactionId parentSubid, void *arg)
{
	HASH_SEQ_STATUS scan;
	ConnCacheEntry *entry;
	int			curlevel;

	/* Nothing to do at subxact start, nor after commit. */
	if (!(event == SUBXACT_EVENT_PRE_COMMIT_SUB ||
		  event == SUBXACT_EVENT_ABORT_SUB))
		return;

	/* Quick exit if no connections were touched in this transaction. */
	if (!xact_got_connection)
		return;

	/*
	 * Scan all connection cache entries to find open remote subtransactions
	 * of the current level, and close them.
	 */
	curlevel = GetCurrentTransactionNestLevel();
	hash_seq_init(&scan, ConnectionHash);
	while ((entry = (ConnCacheEntry *) hash_seq_search(&scan)))
	{
		char		sql[100];

		/*
		 * We only care about connections with open remote subtransactions of
		 * the current level.
		 */
		if (entry->conn == NULL || entry->xact_depth < curlevel)
			continue;

		if (entry->xact_depth > curlevel)
			elog(ERROR, "missed cleaning up remote subtransaction at level %d",
				 entry->xact_depth);

		if (event == SUBXACT_EVENT_PRE_COMMIT_SUB)
		{
			/*
			 * If abort cleanup previously failed for this connection, we
			 * can't issue any more commands against it.
			 */
			pgfdw_reject_incomplete_xact_state_change(entry);

			/* Commit all remote subtransactions during pre-commit */
			snprintf(sql, sizeof(sql), "RELEASE SAVEPOINT s%d", curlevel);
			entry->changing_xact_state = true;
			do_sql_command(entry, sql);
			entry->changing_xact_state = false;
		}
		else if (in_error_recursion_trouble())
		{
			/*
			 * Don't try to clean up the connection if we're already in error
			 * recursion trouble.
			 */
			entry->changing_xact_state = true;
		}
		else if (!entry->changing_xact_state)
		{
			bool		abort_cleanup_failure = false;

			/* Remember that abort cleanup is in progress. */
			entry->changing_xact_state = true;

			/* Assume we might have lost track of prepared statements */
			entry->have_error = true;

			/*
			 * If a command has been submitted to the remote server by using
			 * an asynchronous execution function, the command might not have
			 * yet completed.  Check to see if a command is still being
			 * processed by the remote server, and if so, request cancellation
			 * of the command.
			 */
			if (PQtransactionStatus(entry->conn) == PQTRANS_ACTIVE &&
				!pgfdw_cancel_query(entry))
				abort_cleanup_failure = true;
			else
			{
				/* Rollback all remote subtransactions during abort */
				snprintf(sql, sizeof(sql),
						 "ROLLBACK TO SAVEPOINT s%d; RELEASE SAVEPOINT s%d",
						 curlevel, curlevel);
				if (!pgfdw_exec_cleanup_query(entry, sql, false))
					abort_cleanup_failure = true;
			}

			/* Disarm changing_xact_state if it all worked. */
			entry->changing_xact_state = abort_cleanup_failure;
		}

		/* OK, we're outta that level of subtransaction */
		entry->xact_depth--;
	}
}

/*
 * Connection invalidation callback function
 *
 * After a change to a pg_foreign_server or pg_user_mapping catalog entry,
 * mark connections depending on that entry as needing to be remade.
 * We can't immediately destroy them, since they might be in the midst of
 * a transaction, but we'll remake them at the next opportunity.
 *
 * Although most cache invalidation callbacks blow away all the related stuff
 * regardless of the given hashvalue, connections are expensive enough that
 * it's worth trying to avoid that.
 *
 * NB: We could avoid unnecessary disconnection more strictly by examining
 * individual option values, but it seems too much effort for the gain.
 */
static void
pgfdw_inval_callback(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS scan;
	ConnCacheEntry *entry;

	Assert(cacheid == FOREIGNSERVEROID || cacheid == USERMAPPINGOID);

	/* ConnectionHash must exist already, if we're registered */
	hash_seq_init(&scan, ConnectionHash);
	while ((entry = (ConnCacheEntry *) hash_seq_search(&scan)))
	{
		/* Ignore invalid entries */
		if (entry->conn == NULL)
			continue;

		/* hashvalue == 0 means a cache reset, must clear all state */
		if (hashvalue == 0 ||
			(cacheid == FOREIGNSERVEROID &&
			 entry->server_hashvalue == hashvalue) ||
			(cacheid == USERMAPPINGOID &&
			 entry->mapping_hashvalue == hashvalue))
			entry->invalidated = true;
	}
}

/*
 * Raise an error if the given connection cache entry is marked as being
 * in the middle of an xact state change.  This should be called at which no
 * such change is expected to be in progress; if one is found to be in
 * progress, it means that we aborted in the middle of a previous state change
 * and now don't know what the remote transaction state actually is.
 * Such connections can't safely be further used.  Re-establishing the
 * connection would change the snapshot and roll back any writes already
 * performed, so that's not an option, either. Thus, we must abort.
 */
static void
pgfdw_reject_incomplete_xact_state_change(ConnCacheEntry *entry)
{
	HeapTuple	tup;
	Form_pg_user_mapping umform;
	ForeignServer *server;

	/* nothing to do for inactive entries and entries of sane state */
	if (entry->conn == NULL || !entry->changing_xact_state)
		return;

	/* make sure this entry is inactive */
	disconnect_pg_server(entry);

	/* find server name to be shown in the message below */
	tup = SearchSysCache1(USERMAPPINGOID,
						  ObjectIdGetDatum(entry->key));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for user mapping %u", entry->key);
	umform = (Form_pg_user_mapping) GETSTRUCT(tup);
	server = GetForeignServer(umform->umserver);
	ReleaseSysCache(tup);

	ereport(ERROR,
			(errcode(ERRCODE_CONNECTION_EXCEPTION),
			 errmsg("connection to server \"%s\" was lost",
					server->servername)));
}

/*
 * Cancel the currently-in-progress query (whose query text we do not have)
 * and ignore the result.  Returns true if we successfully cancel the query
 * and discard any pending result, and false if not.
 */
static bool
pgfdw_cancel_query(ConnCacheEntry *entry)
{
	PGconn	   *conn = entry->conn;
	PGcancel   *cancel;
	char		errbuf[256];
	PGresult   *result = NULL;
	TimestampTz endtime;

	/*
	 * If it takes too long to cancel the query and discard the result, assume
	 * the connection is dead.
	 */
	endtime = TimestampTzPlusMilliseconds(GetCurrentTimestamp(), 30000);

	/*
	 * If COPY IN in progress, send CopyFail. Otherwise send cancel request.
	 * TODO: make it less hackish, without libpq-int.h inclusion and handling
	 * EAGAIN.
	 */
	if (conn->asyncStatus == PGASYNC_COPY_IN)
	{
		if (PQputCopyEnd(conn, "postgres_fdw: transaction abort on source node") != 1)
		{
			ereport(WARNING,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("could not send abort copy request: %s",
							errbuf)));
			return false;
		}
	}
	else
	{
		/*
		 * Issue cancel request.  Unfortunately, there's no good way to limit the
		 * amount of time that we might block inside PQgetCancel().
		 */
		if ((cancel = PQgetCancel(conn)))
		{
			if (!PQcancel(cancel, errbuf, sizeof(errbuf)))
			{
				ereport(WARNING,
						(errcode(ERRCODE_CONNECTION_FAILURE),
						 errmsg("could not send cancel request: %s",
								errbuf)));
				PQfreeCancel(cancel);
				return false;
			}
			PQfreeCancel(cancel);
		}
	}

	/* Get and discard the result of the query. */
	if (pgfdw_get_cleanup_result(entry, endtime, &result))
		return false;
	PQclear(result);

	return true;
}

/*
 * Submit a query during (sub)abort cleanup and wait up to 30 seconds for the
 * result.  If the query is executed without error, the return value is true.
 * If the query is executed successfully but returns an error, the return
 * value is true if and only if ignore_errors is set.  If the query can't be
 * sent or times out, the return value is false.
 */
static bool
pgfdw_exec_cleanup_query(ConnCacheEntry *entry, const char *query,
						 bool ignore_errors)
{
	PGconn	   *conn = entry->conn;
	PGresult   *result = NULL;
	TimestampTz endtime;

	/*
	 * If it takes too long to execute a cleanup query, assume the connection
	 * is dead.  It's fairly likely that this is why we aborted in the first
	 * place (e.g. statement timeout, user cancel), so the timeout shouldn't
	 * be too long.
	 */
	endtime = TimestampTzPlusMilliseconds(GetCurrentTimestamp(), 30000);

	/*
	 * Submit a query.  Since we don't use non-blocking mode, this also can
	 * block.  But its risk is relatively small, so we ignore that for now.
	 */
	if (!PQsendQuery(conn, query))
	{
		pgfdw_report_error(WARNING, NULL, conn, false, query);
		return false;
	}

	/* Get the result of the query. */
	if (pgfdw_get_cleanup_result(entry, endtime, &result))
		return false;

	/* Issue a warning if not successful. */
	if (PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		pgfdw_report_error(WARNING, result, conn, true, query);
		return ignore_errors;
	}
	PQclear(result);

	return true;
}

/*
 * Get, during abort cleanup, the result of a query that is in progress.  This
 * might be a query that is being interrupted by transaction abort, or it might
 * be a query that was initiated as part of transaction abort to get the remote
 * side back to the appropriate state.
 *
 * It's not a huge problem if we throw an ERROR here, but if we get into error
 * recursion trouble, we'll end up slamming the connection shut, which will
 * necessitate failing the entire toplevel transaction even if subtransactions
 * were used.  Try to use WARNING where we can.
 *
 * endtime is the time at which we should give up and assume the remote
 * side is dead.  Returns true if the timeout expired, otherwise false.
 * Sets *result except in case of a timeout.
 */
static bool
pgfdw_get_cleanup_result(ConnCacheEntry *entry, TimestampTz endtime,
						 PGresult **result)
{
	volatile bool timed_out = false;
	PGconn *conn = entry->conn;
	PGresult   *volatile last_res = NULL;

	/* In what follows, do not leak any PGresults on an error. */
	PG_TRY();
	{
		for (;;)
		{
			PGresult   *res;

			while (PQisBusy(conn))
			{
				TimestampTz now = GetCurrentTimestamp();
				long		secs;
				int			microsecs;
				long		cur_timeout;
				WaitEvent	ev;

				/* If timeout has expired, give up, else get sleep time. */
				if (now >= endtime)
				{
					timed_out = true;
					goto exit;
				}
				TimestampDifference(now, endtime, &secs, &microsecs);

				/* To protect against clock skew, limit sleep to one minute. */
				cur_timeout = Min(60000, secs * USECS_PER_SEC + microsecs);

				/* Sleep until there's something to do */
				WaitEventSetWait(entry->wait_set, cur_timeout, &ev,
								 1, PG_WAIT_EXTENSION);
				ResetLatch(MyLatch);

				CHECK_FOR_INTERRUPTS();

				/* Data available in socket? */
				if (ev.events & WL_SOCKET_READABLE)
				{
					if (!PQconsumeInput(conn))
					{
						/* connection trouble; treat the same as a timeout */
						timed_out = true;
						goto exit;
					}
				}
			}

			res = PQgetResult(conn);
			if (res == NULL)
				break;			/* query is complete */

			PQclear(last_res);
			last_res = res;
		}
exit:	;
	}
	PG_CATCH();
	{
		PQclear(last_res);
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (timed_out)
		PQclear(last_res);
	else
		*result = last_res;
	return timed_out;
}
