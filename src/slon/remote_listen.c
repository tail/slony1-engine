/*-------------------------------------------------------------------------
 * remote_listen.c
 *
 *	Implementation of the thread listening for events on
 *	a remote node database.
 *
 *	Copyright (c) 2003-2004, PostgreSQL Global Development Group
 *	Author: Jan Wieck, Afilias USA INC.
 *
 *	$Id: remote_listen.c,v 1.15 2004-06-16 11:34:31 wieck Exp $
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

#include "libpq-fe.h"
#include "c.h"

#include "slon.h"


/* ----------
 * struct listat
 *
 *	local data structure for nodes we are currently listening
 *	for events from.
 * ----------
 */
struct listat {
	int				li_origin;

	struct listat  *prev;
	struct listat  *next;
};


/* ----------
 * Local functions
 * ----------
 */
static void		remoteListen_adjust_listat(SlonNode *node,
							struct listat **listat_head, 
							struct listat **listat_tail);
static void		remoteListen_cleanup(struct listat **listat_head,
							struct listat **listat_tail);
static int		remoteListen_forward_confirm(SlonNode *node,
							SlonConn *conn);
static int		remoteListen_receive_events(SlonNode *node,
							SlonConn *conn, struct listat *listat);



/* ----------
 * slon_remoteListenThread
 *
 *	Listen for events on a remote database connection. This means,
 *	events generated by every other node we listen for on this one.
 * ----------
 */
void *
remoteListenThread_main(void *cdata)
{
	SlonNode   *node = (SlonNode *)cdata;
	SlonConn   *conn = NULL;
	char	   *conn_conninfo = NULL;
	char		conn_symname[64];
	int			rc;
	SlonDString	query1;
	PGconn	   *dbconn = NULL;
	PGresult   *res;
	PGnotify   *notification;
	char		notify_confirm[256];
	int			forward_confirm = true;

	struct listat  *listat_head;
	struct listat  *listat_tail;
	int64			last_config_seq = 0;
	int64			new_config_seq = 0;

	slon_log(SLON_DEBUG1,
			"remoteListenThread_%d: thread starts\n",
			node->no_id);

	/*
	 * Initialize local data
	 */
	listat_head = NULL;
	listat_tail = NULL;
	dstring_init(&query1);

	sprintf(conn_symname, "node_%d_listen", node->no_id);
	sprintf(notify_confirm, "_%s_Confirm", rtcfg_cluster_name);

	/*
	 * Work until doomsday
	 */
	while (true)
	{
		if (last_config_seq != (new_config_seq = rtcfg_seq_get()))
		{
			/*
			 * Lock the configuration and check if we are (still) supposed
			 * to exist.
			 */
			rtcfg_lock();

			/*
			 * If we have a database connection to the remote node, check
			 * if there was a change in the connection information.
			 */
			if (conn != NULL)
			{
				if (node->pa_conninfo == NULL || 
						strcmp(conn_conninfo, node->pa_conninfo) != 0)
				{
					slon_log(SLON_DEBUG1,
							"remoteListenThread_%d: "
							"disconnecting from '%s'\n",
							node->no_id, conn_conninfo);
					slon_disconnectdb(conn);
					free(conn_conninfo);

					conn = NULL;
					conn_conninfo = NULL;
				}
			}

			/*
			 * Check our node's listen_status
			 */
			if (node->listen_status == SLON_TSTAT_NONE ||
				node->listen_status == SLON_TSTAT_SHUTDOWN ||
				!node->no_active)
			{
				rtcfg_unlock();
				break;
			}
			if (node->listen_status == SLON_TSTAT_RESTART)
				node->listen_status = SLON_TSTAT_RUNNING;

			/*
			 * Adjust the listat list and see if there is anything to
			 * listen for. If not, sleep for a while and check again,
			 * some node reconfiguration must be going on here.
			 */
			remoteListen_adjust_listat(node, &listat_head, &listat_tail);

			last_config_seq = new_config_seq;

			if (listat_head == NULL)
			{
				rtcfg_unlock();
				slon_log(SLON_DEBUG2,
						"remoteListenThread_%d: nothing to listen for\n",
						node->no_id);
				rc = sched_msleep(node, 10000);
				if (rc != SCHED_STATUS_OK && rc != SCHED_STATUS_CANCEL)
					break;
				continue;
			}

			rtcfg_unlock();
		}

		/*
		 * Check if we have a database connection
		 */
		if (conn == NULL)
		{
			int		pa_connretry;

			/*
			 * Make sure we have connection info
			 */
			rtcfg_lock();
			if (node->pa_conninfo == NULL)
			{
				slon_log(SLON_WARN,
						"remoteListenThread_%d: no conninfo - "
						"sleep 10 seconds\n",
						node->no_id);

				rtcfg_unlock();
				rc = sched_msleep(node, 10000);
				if (rc != SCHED_STATUS_OK && rc != SCHED_STATUS_CANCEL)
					break;

				continue;
			}

			/*
			 * Try to establish a database connection to the remote
			 * node's database.
			 */
			conn_conninfo = strdup(node->pa_conninfo);
			pa_connretry = node->pa_connretry;
			rtcfg_unlock();

			conn = slon_connectdb(conn_conninfo, conn_symname);
			if (conn == NULL)
			{
				free(conn_conninfo);
				conn_conninfo = NULL;

				slon_log(SLON_WARN,
						"remoteListenThread_%d: DB connection failed - "
						"sleep %d seconds\n",
						node->no_id, pa_connretry);

				rc = sched_msleep(node, pa_connretry * 1000);
				if (rc != SCHED_STATUS_OK && rc != SCHED_STATUS_CANCEL)
					break;

				continue;
			}
			dbconn = conn->dbconn;

			/*
			 * Listen on the connection for events and confirmations
			 */
			slon_mkquery(&query1,
					"listen \"_%s_Event\"; "
					"listen \"_%s_Confirm\"; "
					"listen \"_%s_Node_%d\"; ",
					rtcfg_cluster_name, rtcfg_cluster_name,
					rtcfg_cluster_name, rtcfg_nodeid);
			res = PQexec(dbconn, dstring_data(&query1));
			if (PQresultStatus(res) != PGRES_COMMAND_OK)
			{
				slon_log(SLON_ERROR,
						"remoteListenThread_%d: \"%s\" - %s",
						node->no_id,
						dstring_data(&query1), PQresultErrorMessage(res));
				PQclear(res);
				slon_disconnectdb(conn);
				free(conn_conninfo);
				conn = NULL;
				conn_conninfo = NULL;

				rc = sched_msleep(node, pa_connretry * 1000);
				if (rc != SCHED_STATUS_OK && rc != SCHED_STATUS_CANCEL)
					break;

				continue;
			}

			rc = db_getLocalNodeId(dbconn);
			if (rc != node->no_id)
			{
				slon_log(SLON_ERROR,
						"remoteListenThread_%d: db_getLocalNodeId() "
						"returned %d - wrong database?\n",
						node->no_id, rc);

				slon_disconnectdb(conn);
				free(conn_conninfo);
				conn = NULL;
				conn_conninfo = NULL;

				rc = sched_msleep(node, pa_connretry * 1000);
				if (rc != SCHED_STATUS_OK && rc != SCHED_STATUS_CANCEL)
					break;

				continue;
			}

			slon_log(SLON_DEBUG1,
					"remoteListenThread_%d: connected to '%s'\n",
					node->no_id, conn_conninfo);

		}

		/*
		 * Receive events from the provider node
		 */
		rc = remoteListen_receive_events(node, conn, listat_head);
		if (rc < 0)
		{
			slon_disconnectdb(conn);
			free(conn_conninfo);
			conn = NULL;
			conn_conninfo = NULL;

			rc = sched_msleep(node, 10000);
			if (rc != SCHED_STATUS_OK && rc != SCHED_STATUS_CANCEL)
				break;

			continue;
		}

		/*
		 * If the remote node notified for new confirmations,
		 * read them and queue them into the remote worker for
		 * storage in our local database.
		 */
		if (forward_confirm)
		{
			rc = remoteListen_forward_confirm(node, conn);
			if (rc < 0)
			{
				slon_disconnectdb(conn);
				free(conn_conninfo);
				conn = NULL;
				conn_conninfo = NULL;

				rc = sched_msleep(node, 10000);
				if (rc != SCHED_STATUS_OK && rc != SCHED_STATUS_CANCEL)
					break;

				continue;
			}
			forward_confirm = false;
		}

		/*
		 * Wait for notification.
		 */
		rc = sched_wait_time(conn, SCHED_WAIT_SOCK_READ, 10000);
		if (rc == SCHED_STATUS_CANCEL)
			continue;
		if (rc != SCHED_STATUS_OK)
			break;

		/*
		 * Set the forward_confirm flag if there was any Confirm
		 * notification sent.
		 */
		PQconsumeInput(dbconn);
		while ((notification = PQnotifies(dbconn)) != NULL)
		{
			if (strcmp(notification->relname, notify_confirm) == 0)
				forward_confirm = true;
			PQfreemem(notification);
		}
	}

	/*
	 * Doomsday!
	 */
	if (conn != NULL)
	{
		slon_log(SLON_INFO,
				"remoteListenThread_%d: "
				"disconnecting from '%s'\n",
				node->no_id, conn_conninfo);
		slon_disconnectdb(conn);
		free(conn_conninfo);
		conn = NULL;
		conn_conninfo = NULL;
	}

	remoteListen_cleanup(&listat_head, &listat_tail);

	rtcfg_lock();
	node->listen_status = SLON_TSTAT_DONE;
	rtcfg_unlock();

	slon_log(SLON_DEBUG1,
			"remoteListenThread_%d: thread done\n",
			node->no_id);
	pthread_exit(NULL);
}


/* ----------
 * remoteListen_adjust_listat
 *
 *	local function to (re)adjust the known nodes to the global
 *	configuration.
 * ----------
 */
static void
remoteListen_adjust_listat(SlonNode *node, struct listat **listat_head, 
			struct listat **listat_tail)
{
	SlonNode	   *lnode;
	SlonListen	   *listen;
	struct listat  *listat;
	struct listat  *linext;
	int				found;

	/*
	 * Remove listat entries for event origins that this remote node
	 * stopped providing for us, or where the origin got disabled.
	 */
	for (listat = *listat_head; listat;)
	{
		linext = listat->next;

		found = false;
		for (listen = node->listen_head; listen; listen = listen->next)
		{
			/*
			 * Check if the sl_listen entry still exists and that
			 * the li_origin is active.
			 */
			if (listen->li_origin == listat->li_origin)
			{
				lnode = rtcfg_findNode(listat->li_origin);
				if (lnode != NULL && lnode->no_active)
					found = true;
				break;
			}
		}

		/* 
		 * Remove obsolete item
		 */
		if (!found)
		{
			slon_log(SLON_DEBUG2,
					"remoteListenThread_%d: stop listening for "
					"event origin %d\n",
					node->no_id, listat->li_origin);
			DLLIST_REMOVE(*listat_head, *listat_tail, listat);
			free(listat);
		}

		listat = linext;
	}

	/*
	 * Now add new or newly enabled sl_listen entries to it.
	 */
	for (listen = node->listen_head; listen; listen = listen->next)
	{
		/*
		 * skip inactive or unknown nodes
		 */
		lnode = rtcfg_findNode(listen->li_origin);
		if (lnode == NULL || !(lnode->no_active))
			continue;

		/*
		 * check if we have that entry
		 */
		found = false;
		for (listat = *listat_head; listat; listat = listat->next)
		{
			if (listen->li_origin == listat->li_origin)
			{
				found = true;
				break;
			}
		}

		/*
		 * Add missing entries
		 */
		if (!found)
		{
			slon_log(SLON_DEBUG2,
					"remoteListenThread_%d: start listening for "
					"event origin %d\n",
					node->no_id, listen->li_origin);
			listat = (struct listat *)malloc(sizeof(struct listat));
			if (listat == NULL)
			{
				perror("remoteListen_adjust_listat: malloc()");
				slon_abort();
			}
			memset(listat, 0, sizeof(struct listat));

			listat->li_origin = listen->li_origin;
			DLLIST_ADD_TAIL(*listat_head, *listat_tail, listat);
		}
	}
}


/* ----------
 * remoteListen_cleanup
 *
 *	Free resources used by the remoteListen thread
 * ----------
 */
static void
remoteListen_cleanup(struct listat **listat_head, struct listat **listat_tail)
{
	struct listat  *listat;

	/*
	 * Free the listen status list
	 */
	while ((listat = *listat_head) != NULL)
	{
		DLLIST_REMOVE(*listat_head, *listat_tail, listat);
		free(listat);
	}
}


/* ----------
 * remoteListen_forward_confirm
 *
 *	Read the last confirmed event sequence for all nodes from the
 *	remote database and forward it to the local database so that
 *	the cleanup process will know when all nodes have confirmed
 *	an event and it can be thrown away (together with its log data).
 * ----------
 */
static int
remoteListen_forward_confirm(SlonNode *node, SlonConn *conn)
{
	SlonDString		query;
	PGresult	   *res;
	int				ntuples;
	int				tupno;

	dstring_init(&query);

	/*
	 * Select the max(con_seqno) grouped by con_origin and con_received
	 * from the sl_confirm table.
	 */
	slon_mkquery(&query,
			"select con_origin, con_received, "
			"    max(con_seqno) as con_seqno, "
			"    max(con_timestamp) as con_timestamp "
			"from %s.sl_confirm "
			"where con_received <> %d "
			"group by con_origin, con_received",
			rtcfg_namespace, rtcfg_nodeid);
	res = PQexec(conn->dbconn, dstring_data(&query));
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		slon_log(SLON_ERROR,
				"remoteListenThread_%d: \"%s\" %s",
				node->no_id, dstring_data(&query),
				PQresultErrorMessage(res));
		dstring_free(&query);
		PQclear(res);
		return -1;
	}

	/*
	 * We actually do not do the forwardiing ourself here. We send
	 * a special message to the remote worker for that node.
	 */
	ntuples = PQntuples(res);
	for (tupno = 0; tupno < ntuples; tupno++)
	{
		remoteWorker_confirm(
				node->no_id,
				PQgetvalue(res, tupno, 0),
				PQgetvalue(res, tupno, 1),
				PQgetvalue(res, tupno, 2),
				PQgetvalue(res, tupno, 3));
	}

	PQclear(res);
	dstring_free(&query);

	return 0;
}


/* ----------
 * remoteListen_receive_events
 *
 *	Retrieve all new events that origin from nodes for which we
 *	listen on this node as provider and add them to the node
 *	specific worker message queue..
 * ----------
 */
static int
remoteListen_receive_events(SlonNode *node, SlonConn *conn,
			struct listat *listat)
{
	SlonNode	   *origin;
	SlonDString		query;
	char		   *where_or_or;
	char			seqno_buf[64];
	PGresult	   *res;
	int				ntuples;
	int				tupno;
	time_t			timeout;
	time_t			now;

	dstring_init(&query);

	/*
	 * In the runtime configuration info for the node, we remember
	 * the last event sequence that we actually have received. If the
	 * remote worker thread has processed it yet or not isn't important,
	 * we have it in the message queue at least and don't need to
	 * select it again.
	 *
	 * So the query we construct contains a qualification
	 * (ev_origin = <remote_node> and ev_seqno > <last_seqno>) per
	 * remote node we're listen for here.
	 */
	slon_mkquery(&query,
			"select ev_origin, ev_seqno, ev_timestamp, "
			"       ev_minxid, ev_maxxid, ev_xip, "
			"       ev_type, "
			"       ev_data1, ev_data2, "
			"       ev_data3, ev_data4, "
			"       ev_data5, ev_data6, "
			"       ev_data7, ev_data8 "
			"from %s.sl_event e",
			rtcfg_namespace);

	rtcfg_lock();

	where_or_or = "where";
	while (listat)
	{
		if ((origin = rtcfg_findNode(listat->li_origin)) == NULL)
		{
			rtcfg_unlock();
			slon_log(SLON_ERROR,
					"remoteListenThread_%d: unknown node %d\n",
					node->no_id, listat->li_origin);
			dstring_free(&query);
			return -1;
		}

		sprintf(seqno_buf, INT64_FORMAT, origin->last_event);
		slon_appendquery(&query,
				" %s (e.ev_origin = '%d' and e.ev_seqno > '%s')",
				where_or_or, listat->li_origin, seqno_buf);

		where_or_or = "or";
		listat = listat->next;
	}
	slon_appendquery(&query, " order by e.ev_origin, e.ev_seqno");

	rtcfg_unlock();

	if (PQsendQuery(conn->dbconn, dstring_data(&query)) == 0)
	{
		slon_log(SLON_ERROR,
				"remoteListenThread_%d: \"%s\" - %s",
				node->no_id,
				dstring_data(&query), PQerrorMessage(conn->dbconn));
		dstring_free(&query);
		return -1;
	}

	time(&timeout);
	timeout += 300;
	while (PQisBusy(conn->dbconn) != 0)
	{
		time(&now);
		if (now >= timeout)
		{
			slon_log(SLON_ERROR,
					"remoteListenThread_%d: timeout for event selection\n",
					node->no_id);
			dstring_free(&query);
			return -1;
		}
		if (PQconsumeInput(conn->dbconn) == 0)
		{
			slon_log(SLON_ERROR,
					"remoteListenThread_%d: \"%s\" - %s",
					node->no_id,
					dstring_data(&query), PQerrorMessage(conn->dbconn));
			dstring_free(&query);
			return -1;
		}
		if (PQisBusy(conn->dbconn) != 0)
			sched_wait_time(conn, SCHED_WAIT_SOCK_READ, 10000);
	}

	res = PQgetResult(conn->dbconn);

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		slon_log(SLON_ERROR,
				"remoteListenThread_%d: \"%s\" - %s",
				node->no_id,
				dstring_data(&query), PQresultErrorMessage(res));
		PQclear(res);
		dstring_free(&query);
		return -1;
	}
	dstring_free(&query);

	/*
	 * Add all events found to the remote worker message queue.
	 */
	ntuples = PQntuples(res);
	for (tupno = 0; tupno < ntuples; tupno++)
	{
		int			ev_origin;
		int64		ev_seqno;

		ev_origin = strtol(PQgetvalue(res, tupno, 0), NULL, 10);
		slon_scanint64(PQgetvalue(res, tupno, 1), &ev_seqno);

		slon_log(SLON_DEBUG2, "remoteListenThread_%d: "
				"queue event %d,%s %s\n",
				node->no_id, ev_origin, PQgetvalue(res, tupno, 1),
				PQgetvalue(res, tupno, 6));

		remoteWorker_event(node->no_id,
				ev_origin, ev_seqno,
				PQgetvalue(res, tupno, 2),		/* ev_timestamp */
				PQgetvalue(res, tupno, 3),		/* ev_minxid */
				PQgetvalue(res, tupno, 4),		/* ev_maxxid */
				PQgetvalue(res, tupno, 5),		/* ev_xip */
				PQgetvalue(res, tupno, 6),		/* ev_type */
				(PQgetisnull(res, tupno, 7)) ? NULL : PQgetvalue(res, tupno, 7),
				(PQgetisnull(res, tupno, 8)) ? NULL : PQgetvalue(res, tupno, 8),
				(PQgetisnull(res, tupno, 9)) ? NULL : PQgetvalue(res, tupno, 9),
				(PQgetisnull(res, tupno, 10)) ? NULL : PQgetvalue(res, tupno, 10),
				(PQgetisnull(res, tupno, 11)) ? NULL : PQgetvalue(res, tupno, 11),
				(PQgetisnull(res, tupno, 12)) ? NULL : PQgetvalue(res, tupno, 12),
				(PQgetisnull(res, tupno, 13)) ? NULL : PQgetvalue(res, tupno, 13),
				(PQgetisnull(res, tupno, 14)) ? NULL : PQgetvalue(res, tupno, 14));
	}

	PQclear(res);

	return 0;
}


