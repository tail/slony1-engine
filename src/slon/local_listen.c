/*-------------------------------------------------------------------------
 * local_listen.c
 *
 *	Implementation of the thread listening for events on
 *	the local node database.
 *
 *	Copyright (c) 2003-2009, PostgreSQL Global Development Group
 *	Author: Jan Wieck, Afilias USA INC.
 *
 *	
 *-------------------------------------------------------------------------
 */


#include <pthread.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>

#include "slon.h"



/* ----------
 * slon_localListenThread
 *
 *	Listen for events on the local database connection. This means,
 *	events generated by the local node only.
 * ----------
 */
void *
localListenThread_main(/* @unused@ */ void *dummy)
{
	SlonConn   *conn;
	SlonDString query1;
	PGconn	   *dbconn;
	PGresult   *res;
	int			ntuples;
	int			tupno;
	PGnotify   *notification;
	char		restart_notify[256];
	int			restart_request;
	int poll_sleep = 0;
	int         node_lock_obtained=0;

	slon_log(SLON_INFO, "localListenThread: thread starts\n");

	/*
	 * Connect to the local database
	 */
	if ((conn = slon_connectdb(rtcfg_conninfo, "local_listen")) == NULL)
		slon_retry();
	dbconn = conn->dbconn;

	/*
	 * Initialize local data
	 */
	dstring_init(&query1);
	sprintf(restart_notify, "_%s_Restart", rtcfg_cluster_name);

	/*
	 * Listen for local events
	 */
	(void) slon_mkquery(&query1,
		     "listen \"_%s_Restart\"; ",
		     rtcfg_cluster_name);
	res = PQexec(dbconn, dstring_data(&query1));
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		slon_log(SLON_FATAL,
				 "localListenThread: \"%s\" - %s\n",
				 dstring_data(&query1), PQresultErrorMessage(res));
		PQclear(res);
		dstring_free(&query1);
		slon_retry();
	}
	PQclear(res);

	/*
	 * Check that we are the only slon daemon connected.
	 */
#define NODELOCKERROR "ERROR:  duplicate key violates unique constraint \"sl_nodelock-pkey\""

	(void) slon_mkquery(&query1,
				 "select %s.cleanupNodelock(); "
				 "insert into %s.sl_nodelock values ("
				 "    %d, 0, \"pg_catalog\".pg_backend_pid()); ",
				 rtcfg_namespace, rtcfg_namespace,
				 rtcfg_nodeid);
	while(!node_lock_obtained)
	{
		res = PQexec(dbconn, dstring_data(&query1));
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			slon_log(SLON_FATAL,
					 "localListenThread: \"%s\" - %s\n",
					 dstring_data(&query1), PQresultErrorMessage(res));
			if (strncmp(NODELOCKERROR, PQresultErrorMessage(res), strlen(NODELOCKERROR)) == 0) {
				slon_log(SLON_FATAL,
						 "Do you already have a slon running against this node?\n");
				slon_log(SLON_FATAL,
						 "Or perhaps a residual idle backend connection from a dead slon?\n");
				PQclear(res);
				sleep(5);
				continue;
			}
		    
			PQclear(res);
			dstring_free(&query1);
			slon_abort();
		}
		PQclear(res);
		node_lock_obtained=1;
	}

	/*
	 * Flag the main thread that the coast is clear and he can launch all
	 * other threads.
	 */
	pthread_mutex_lock(&slon_wait_listen_lock);
	pthread_cond_signal(&slon_wait_listen_cond);
	pthread_mutex_unlock(&slon_wait_listen_lock);

	/*
	 * Process all events, then wait for notification and repeat until
	 * shutdown time has arrived.
	 */
	while (true)
	{
		/*
		 * Start the transaction
		 */
		res = PQexec(dbconn, "start transaction; "
					 "set transaction isolation level serializable;");
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			slon_log(SLON_FATAL,
					 "localListenThread: cannot start transaction - %s\n",
					 PQresultErrorMessage(res));
			PQclear(res);
			dstring_free(&query1);
			slon_retry();
			break;
		}
		PQclear(res);

		/*
		 * Drain notifications.
		 */
		(void) PQconsumeInput(dbconn);
		restart_request = false;
		while ((notification = PQnotifies(dbconn)) != NULL)
		{
			if (strcmp(restart_notify, notification->relname) == 0)
				restart_request = true;
			(void) PQfreemem(notification);
		}
		if (restart_request)
		{
			slon_log(SLON_INFO,
					 "localListenThread: got restart notification\n");
#ifndef WIN32
			slon_restart();
#else
			/* XXX */
			/* Win32 defer to service manager to restart for now */
			slon_restart();
#endif
		}

		/*
		 * Query the database for new local events
		 */
		(void) slon_mkquery(&query1,
					 "select ev_seqno, ev_timestamp, "
					 "       'dummy', 'dummy', 'dummy', "
					 "       ev_type, "
					 "       ev_data1, ev_data2, ev_data3, ev_data4, "
					 "       ev_data5, ev_data6, ev_data7, ev_data8 "
					 "from %s.sl_event "
					 "where  ev_origin = '%d' "
					 "       and ev_seqno > '%s' "
					 "order by ev_seqno",
					 rtcfg_namespace, rtcfg_nodeid, rtcfg_lastevent);
		res = PQexec(dbconn, dstring_data(&query1));
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			slon_log(SLON_FATAL,
					 "localListenThread: \"%s\" - %s\n",
					 dstring_data(&query1), PQresultErrorMessage(res));
			PQclear(res);
			dstring_free(&query1);
			slon_retry();
			break;
		}
		ntuples = PQntuples(res);

		for (tupno = 0; tupno < ntuples; tupno++)
		{
			char	   *ev_type;

			/*
			 * Remember the event sequence number for confirmation
			 */
			strcpy(rtcfg_lastevent, PQgetvalue(res, tupno, 0));

			/*
			 * Get the event type and process configuration events
			 */
			ev_type = PQgetvalue(res, tupno, 5);
			slon_log(SLON_DEBUG2, "localListenThread: "
					 "Received event %d,%s %s\n",
					 rtcfg_nodeid, PQgetvalue(res, tupno, 0),
					 ev_type);

			if (strcmp(ev_type, "SYNC") == 0)
			{
				/*
				 * SYNC - nothing to do
				 */
			}
			else if (strcmp(ev_type, "STORE_NODE") == 0)
			{
				/*
				 * STORE_NODE
				 */
				int			no_id;
				char	   *no_comment;

				no_id = (int)strtol(PQgetvalue(res, tupno, 6), NULL, 10);
				no_comment = PQgetvalue(res, tupno, 7);

				if (no_id != rtcfg_nodeid)
					rtcfg_storeNode(no_id, no_comment);

				rtcfg_reloadListen(dbconn);
			}
			else if (strcmp(ev_type, "ENABLE_NODE") == 0)
			{
				/*
				 * ENABLE_NODE
				 */
				int			no_id;

				no_id = (int)strtol(PQgetvalue(res, tupno, 6), NULL, 10);

				if (no_id != rtcfg_nodeid)
					rtcfg_enableNode(no_id);

				rtcfg_reloadListen(dbconn);
			}
			else if (strcmp(ev_type, "DROP_NODE") == 0)
			{
				/*
				 * DROP_NODE
				 */
				int			no_id;
				char		notify_query[256];
				PGresult   *notify_res;

				no_id = (int)strtol(PQgetvalue(res, tupno, 6), NULL, 10);

				/*
				 * Deactivate the node in the runtime configuration
				 */
				rtcfg_disableNode(no_id);

				/*
				 * And cause the replication daemon to restart to get rid of
				 * it.
				 */
				snprintf(notify_query, sizeof(notify_query),
						 "notify \"_%s_Restart\";",
						 rtcfg_cluster_name);
				notify_res = PQexec(dbconn, notify_query);
				if (PQresultStatus(notify_res) != PGRES_COMMAND_OK)
				{
					slon_log(SLON_FATAL, "localListenThread: \"%s\" %s\n",
							 notify_query, PQresultErrorMessage(notify_res));
					PQclear(notify_res);
					slon_restart();
				}
				PQclear(notify_res);

				rtcfg_reloadListen(dbconn);
			}
			else if (strcmp(ev_type, "CLONE_NODE") == 0)
			{
				/*
				 * CLONE_NODE
				 */
				int			no_id;
				int			no_provider;
				char	   *no_comment;

				no_id = (int)strtol(PQgetvalue(res, tupno, 6), NULL, 10);
				no_provider = (int)strtol(PQgetvalue(res, tupno, 7), NULL, 10);
				no_comment = PQgetvalue(res, tupno, 8);

				rtcfg_storeNode(no_id, no_comment);
			}
			else if (strcmp(ev_type, "STORE_PATH") == 0)
			{
				/*
				 * STORE_PATH
				 */
				int			pa_server;
				int			pa_client;
				char	   *pa_conninfo;
				int			pa_connretry;

				pa_server = (int)strtol(PQgetvalue(res, tupno, 6), NULL, 10);
				pa_client = (int)strtol(PQgetvalue(res, tupno, 7), NULL, 10);
				pa_conninfo = PQgetvalue(res, tupno, 8);
				pa_connretry = (int)strtol(PQgetvalue(res, tupno, 9), NULL, 10);

				if (pa_client == rtcfg_nodeid)
					rtcfg_storePath(pa_server, pa_conninfo, pa_connretry);

				rtcfg_reloadListen(dbconn);
			}
			else if (strcmp(ev_type, "DROP_PATH") == 0)
			{
				/*
				 * DROP_PATH
				 */
				int			pa_server;
				int			pa_client;

				pa_server = (int)strtol(PQgetvalue(res, tupno, 6), NULL, 10);
				pa_client = (int)strtol(PQgetvalue(res, tupno, 7), NULL, 10);

				if (pa_client == rtcfg_nodeid)
					rtcfg_dropPath(pa_server);

				rtcfg_reloadListen(dbconn);
			}
			else if (strcmp(ev_type, "STORE_LISTEN") == 0)
			{
				/*
				 * STORE_LISTEN
				 */
				int			li_origin;
				int			li_provider;
				int			li_receiver;

				li_origin = (int)strtol(PQgetvalue(res, tupno, 6), NULL, 10);
				li_provider = (int)strtol(PQgetvalue(res, tupno, 7), NULL, 10);
				li_receiver = (int)strtol(PQgetvalue(res, tupno, 8), NULL, 10);

				if (li_receiver == rtcfg_nodeid)
					rtcfg_storeListen(li_origin, li_provider);
			}
			else if (strcmp(ev_type, "DROP_LISTEN") == 0)
			{
				/*
				 * DROP_LISTEN
				 */
				int			li_origin;
				int			li_provider;
				int			li_receiver;

				li_origin = (int)strtol(PQgetvalue(res, tupno, 6), NULL, 10);
				li_provider = (int)strtol(PQgetvalue(res, tupno, 7), NULL, 10);
				li_receiver = (int)strtol(PQgetvalue(res, tupno, 8), NULL, 10);

				if (li_receiver == rtcfg_nodeid)
					rtcfg_dropListen(li_origin, li_provider);
			}
			else if (strcmp(ev_type, "STORE_SET") == 0)
			{
				/*
				 * STORE_SET
				 */
				int			set_id;
				int			set_origin;
				char	   *set_comment;

				set_id = (int)strtol(PQgetvalue(res, tupno, 6), NULL, 10);
				set_origin = (int)strtol(PQgetvalue(res, tupno, 7), NULL, 10);
				set_comment = PQgetvalue(res, tupno, 8);

				rtcfg_storeSet(set_id, set_origin, set_comment);
			}
			else if (strcmp(ev_type, "DROP_SET") == 0)
			{
				/*
				 * DROP_SET
				 */
				int			set_id;

				set_id = (int)strtol(PQgetvalue(res, tupno, 6), NULL, 10);

				rtcfg_dropSet(set_id);
			}
			else if (strcmp(ev_type, "MERGE_SET") == 0)
			{
				/*
				 * MERGE_SET
				 */
				int			set_id;
				int			add_id;

				set_id = (int)strtol(PQgetvalue(res, tupno, 6), NULL, 10);
				add_id = (int)strtol(PQgetvalue(res, tupno, 7), NULL, 10);

				rtcfg_dropSet(add_id);
			}
			else if (strcmp(ev_type, "SET_ADD_TABLE") == 0)
			{
				/*
				 * SET_ADD_TABLE
				 */

				/*
				 * Nothing to do ATM ... we don't support adding tables to
				 * subscribed sets and table information is not maintained in
				 * the runtime configuration.
				 */
			}
			else if (strcmp(ev_type, "SET_ADD_SEQUENCE") == 0)
			{
				/*
				 * SET_ADD_SEQUENCE
				 */

				/*
				 * Nothing to do ATM ... we don't support adding sequences to
				 * subscribed sets and table information is not maintained in
				 * the runtime configuration.
				 */
			}
			else if (strcmp(ev_type, "SET_DROP_TABLE") == 0)
			{
				/*
				 * SET_DROP_TABLE
				 */

				/*
				 * Nothing to do ATM ... table information is not maintained
				 * in the runtime configuration.
				 */
			}
			else if (strcmp(ev_type, "SET_DROP_SEQUENCE") == 0)
			{
				/*
				 * SET_DROP_SEQUENCE
				 */

				/*
				 * Nothing to do ATM ... table information is not maintained
				 * in the runtime configuration.
				 */
			}
			else if (strcmp(ev_type, "SET_MOVE_TABLE") == 0)
			{
				/*
				 * SET_MOVE_TABLE
				 */

				/*
				 * Nothing to do ATM ... table information is not maintained
				 * in the runtime configuration.
				 */
			}
			else if (strcmp(ev_type, "SET_MOVE_SEQUENCE") == 0)
			{
				/*
				 * SET_MOVE_SEQUENCE
				 */

				/*
				 * Nothing to do ATM ... table information is not maintained
				 * in the runtime configuration.
				 */
			}
			else if (strcmp(ev_type, "ADJUST_SEQ") == 0)
			{
				/*
				 * ADJUST_SEQ
				 */
			}
			else if (strcmp(ev_type, "STORE_TRIGGER") == 0)
			{
				/*
				 * STORE_TRIGGER
				 */

				/*
				 * Nothing to do ATM
				 */
			}
			else if (strcmp(ev_type, "DROP_TRIGGER") == 0)
			{
				/*
				 * DROP_TRIGGER
				 */

				/*
				 * Nothing to do ATM
				 */
			}
			else if (strcmp(ev_type, "MOVE_SET") == 0)
			{
				/*
				 * MOVE_SET
				 */
				int			set_id;
				int			old_origin;
				int			new_origin;
				PGresult   *res2;
				SlonDString query2;
				int			sub_provider;

				set_id = (int)strtol(PQgetvalue(res, tupno, 6), NULL, 10);
				old_origin = (int)strtol(PQgetvalue(res, tupno, 7), NULL, 10);
				new_origin = (int)strtol(PQgetvalue(res, tupno, 8), NULL, 10);

				/*
				 * We have been the old origin of the set, so according to the
				 * rules we must have a provider now.
				 */
				dstring_init(&query2);
				(void) slon_mkquery(&query2,
							 "select sub_provider from %s.sl_subscribe "
					     "    where sub_receiver = %d and sub_set = %d",
					     rtcfg_namespace, rtcfg_nodeid, set_id);
				res2 = PQexec(dbconn, dstring_data(&query2));
				if (PQresultStatus(res2) != PGRES_TUPLES_OK)
				{
					slon_log(SLON_FATAL, "localListenThread: \"%s\" %s\n",
							 dstring_data(&query2),
							 PQresultErrorMessage(res2));
					dstring_free(&query2);
					PQclear(res2);
					slon_retry();
				}
				if (PQntuples(res2) != 1)
				{
					slon_log(SLON_FATAL, "localListenThread: MOVE_SET "
							 "but no provider found for set %d\n",
							 set_id);
					dstring_free(&query2);
					PQclear(res2);
					slon_retry();
				}

				sub_provider =
					(int)strtol(PQgetvalue(res2, 0, 0), NULL, 10);
				PQclear(res2);
				dstring_free(&query2);

				rtcfg_moveSet(set_id, old_origin, new_origin, sub_provider);

				rtcfg_reloadListen(dbconn);
			}
			else if (strcmp(ev_type, "FAILOVER_SET") == 0)
			{
				/*
				 * FAILOVER_SET
				 */

				/*
				 * Nothing to do. The stored procedure will restart this
				 * daemon anyway.
				 */
			}
			else if (strcmp(ev_type, "SUBSCRIBE_SET") == 0)
			{
				/*
				 * SUBSCRIBE_SET
				 */
				int			sub_set;
				int			sub_provider;
				int			sub_receiver;
				char	   *sub_forward;

				sub_set = (int)strtol(PQgetvalue(res, tupno, 6), NULL, 10);
				sub_provider = (int)strtol(PQgetvalue(res, tupno, 7), NULL, 10);
				sub_receiver = (int)strtol(PQgetvalue(res, tupno, 8), NULL, 10);
				sub_forward = PQgetvalue(res, tupno, 9);

				if (sub_receiver == rtcfg_nodeid)
					rtcfg_storeSubscribe(sub_set, sub_provider, sub_forward);

				rtcfg_reloadListen(dbconn);
			}
			else if (strcmp(ev_type, "ENABLE_SUBSCRIPTION") == 0)
			{
				/*
				 * ENABLE_SUBSCRIPTION
				 */
				int			sub_set;
				int			sub_provider;
				int			sub_receiver;
				char	   *sub_forward;

				sub_set = (int)strtol(PQgetvalue(res, tupno, 6), NULL, 10);
				sub_provider = (int)strtol(PQgetvalue(res, tupno, 7), NULL, 10);
				sub_receiver = (int)strtol(PQgetvalue(res, tupno, 8), NULL, 10);
				sub_forward = PQgetvalue(res, tupno, 9);

				if (sub_receiver == rtcfg_nodeid)
					rtcfg_enableSubscription(sub_set, sub_provider, sub_forward);

				rtcfg_reloadListen(dbconn);
			}
			else if (strcmp(ev_type, "UNSUBSCRIBE_SET") == 0)
			{
				/*
				 * UNSUBSCRIBE_SET
				 */
				int			sub_set;
				int			sub_receiver;

				sub_set = (int)strtol(PQgetvalue(res, tupno, 6), NULL, 10);
				sub_receiver = (int)strtol(PQgetvalue(res, tupno, 7), NULL, 10);

				if (sub_receiver == rtcfg_nodeid)
					rtcfg_unsubscribeSet(sub_set);

				rtcfg_reloadListen(dbconn);
			}
			else if (strcmp(ev_type, "DDL_SCRIPT") == 0)
			{
				/*
				 * DDL_SCRIPT
				 */

				/*
				 * Nothing to do ATM
				 */
			}
			else if (strcmp(ev_type, "ACCEPT_SET") == 0)
			{
				/*
				 * ACCEPT_SET
				 */

				/*
				 * Nothing to do locally
				 */
				slon_log(SLON_DEBUG1, "localListenThread: ACCEPT_SET\n");
				rtcfg_reloadListen(dbconn);
			}
			else
			{
				slon_log(SLON_FATAL,
					 "localListenThread: event %s: Unknown event type: %s\n",
						 rtcfg_lastevent, ev_type);
				slon_abort();
			}
		}

		PQclear(res);

		/*
		 * If there were events, commit the transaction.
		 */
		if (ntuples > 0)
		{
			poll_sleep = 0;  /* drop polling time back to 0... */
			res = PQexec(dbconn, "commit transaction");
			if (PQresultStatus(res) != PGRES_COMMAND_OK)
			{
				slon_log(SLON_FATAL,
						 "localListenThread: \"%s\" - %s\n",
						 dstring_data(&query1), PQresultErrorMessage(res));
				PQclear(res);
				dstring_free(&query1);
				slon_retry();
				break;
			}
			PQclear(res);
		}
		else
		{
			/*
			 * No database events received. Rollback instead.
			 */

			/* Increase the amount of time to sleep, to a max of sync_interval_timeout */
			poll_sleep += sync_interval;
			if (poll_sleep > sync_interval_timeout) {
				poll_sleep = sync_interval_timeout;
			}
			res = PQexec(dbconn, "rollback transaction;");
			if (PQresultStatus(res) != PGRES_COMMAND_OK)
			{
				slon_log(SLON_FATAL,
						 "localListenThread: \"rollback transaction;\" - %s\n",
						 PQresultErrorMessage(res));
				PQclear(res);
				slon_retry();
				break;
			}
			PQclear(res);
		}

		/*
		 * Wait for notify or for timeout
		 */
		if (sched_wait_time(conn, SCHED_WAIT_SOCK_READ, poll_sleep) != SCHED_STATUS_OK)
			break;
	}

	/*
	 * The scheduler asked us to shutdown. Free memory and close the DB
	 * connection.
	 */
	dstring_free(&query1);
	slon_disconnectdb(conn);
#ifdef SLON_MEMDEBUG
	conn = NULL;
#endif

	slon_log(SLON_INFO, "localListenThread: thread done\n");
	pthread_exit(NULL);
}



/*
 * Local Variables:
 *	tab-width: 4
 *	c-indent-level: 4
 *	c-basic-offset: 4
 * End:
 */
