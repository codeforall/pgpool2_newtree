/* -*-pgsql-c-*- */
/*
 * $Header$
 *
 * pgpool: a language independent connection pool server for PostgreSQL
 * written by Tatsuo Ishii
 *
 * Copyright (c) 2003-2012	PgPool Global Development Group
 *
 * Permission to use, copy, modify, and distribute this software and
 * its documentation for any purpose and without fee is hereby
 * granted, provided that the above copyright notice appear in all
 * copies and that both that copyright notice and this permission
 * notice appear in supporting documentation, and that the name of the
 * author not be used in advertising or publicity pertaining to
 * distribution of the software without specific, written prior
 * permission. The author makes no representations about the
 * suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * child.c: child process main
 *
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <signal.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/time.h>
#include "pool.h"
#include "pool_config.h"
#include "watchdog/wd_ext.h"
#include "watchdog/watchdog.h"

pid_t wd_child(int fork_wait_time);
static void wd_child_exit(int exit_signo);
static int wd_send_response(int sock, WdPacket * recv_pack);
static void wd_node_request_signal(WD_PACKET_NO packet_no, WdNodeInfo *node);

pid_t
wd_child(int fork_wait_time)
{
	int sock;
	int fd;
	int rtn;
	pid_t pid = 0;

	pid = fork();
	if (pid != 0)
	{
		if (pid == -1)
			pool_error("wd_child: fork() failed.");

		return pid;
	}

	if (fork_wait_time > 0)
	{
		sleep(fork_wait_time);
	}

	myargv = save_ps_display_args(myargc, myargv);

	POOL_SETMASK(&UnBlockSig);

	signal(SIGTERM, wd_child_exit);
	signal(SIGINT, wd_child_exit);
	signal(SIGQUIT, wd_child_exit);
	signal(SIGCHLD, SIG_IGN);
	signal(SIGHUP, SIG_IGN);
	signal(SIGUSR1, SIG_IGN);
	signal(SIGUSR2, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGALRM, SIG_IGN);

	init_ps_display("", "", "", "");

	if (WD_List == NULL)
	{
		/* memory allocate is not ready */
		wd_child_exit(15);
	}

	sock = wd_create_recv_socket(WD_MYSELF->wd_port);

	if (sock < 0)
	{
		/* socket create failed */
		wd_child_exit(15);
	}

	set_ps_display("watchdog", false);

	/* child loop */
	for(;;)
	{
		WdPacket buf;
		fd = wd_accept(sock);
		if (fd < 0)
		{
			continue;
		}
		rtn = wd_recv_packet(fd, &buf);
		if (rtn == WD_OK)
		{
			wd_send_response(fd, &buf);
		}
		close(fd);
	}
	return pid;
}

static void
wd_child_exit(int exit_signo)
{
	sigset_t mask;

	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGQUIT);
	sigaddset(&mask, SIGCHLD);
	sigprocmask(SIG_BLOCK, &mask, NULL);

	exit(0);
}

static int
wd_send_response(int sock, WdPacket * recv_pack)
{
	int rtn = WD_NG;
	WdInfo * p, *q;
	WdNodeInfo * node;
	WdLockInfo * lock;
	WdPacket send_packet;
	struct timeval tv;
	char pack_str[WD_MAX_PACKET_STRING];
	int pack_str_len;
	char hash[(MD5_PASSWD_LEN+1)*2];
	bool is_node_packet = false;

	if (recv_pack == NULL)
	{
		return rtn;
	}
	memset(&send_packet, 0, sizeof(WdPacket));
	p = &(recv_pack->wd_body.wd_info);	

	/* authentication */
	if (strlen(pool_config->wd_authkey))
	{
		/* calculate hash from packet */
		pack_str_len = wd_packet_to_string(*recv_pack, pack_str, sizeof(pack_str));
		wd_calc_hash(pack_str, pack_str_len, hash);

		if (strcmp(recv_pack->hash, hash))
		{
			pool_log("wd_send_response: watchdog authentication failed");
			rtn = wd_authentication_failed(sock);
			return rtn;
		}
	}

	/* set response packet no */
	switch (recv_pack->packet_no)
	{
		/* add request into the watchdog list */
		case WD_ADD_REQ:
			p = &(recv_pack->wd_body.wd_info);	
			if (wd_set_wd_list(p->hostname,p->pgpool_port, p->wd_port, p->delegate_ip, &(p->tv), p->status) > 0)
			{
				send_packet.packet_no = WD_ADD_ACCEPT;
			}
			else
			{
				send_packet.packet_no = WD_ADD_REJECT;
			}
			memcpy(&(send_packet.wd_body.wd_info), WD_MYSELF, sizeof(WdInfo));
			break;

		/* announce candidacy to be the new master */
		case WD_STAND_FOR_MASTER:
			p = &(recv_pack->wd_body.wd_info);	
			wd_set_wd_list(p->hostname,p->pgpool_port, p->wd_port, p->delegate_ip, &(p->tv), p->status);
			/* check exist master */
			if ((q = wd_is_alive_master()) != NULL)
			{
				/* vote against the candidate */
				send_packet.packet_no = WD_MASTER_EXIST;
				memcpy(&(send_packet.wd_body.wd_info), q, sizeof(WdInfo));
			}
			else
			{
				if (WD_MYSELF->tv.tv_sec <= p->tv.tv_sec )
				{
					memcpy(&tv,&(p->tv),sizeof(struct timeval));
					tv.tv_sec += 1;
					wd_set_myself(&tv, WD_NORMAL);
				}
				/* vote for the candidate */
				send_packet.packet_no = WD_VOTE_YOU;
				memcpy(&(send_packet.wd_body.wd_info), WD_MYSELF, sizeof(WdInfo));
			}
			break;

		/* announce assumption to be the new master */
		case WD_DECLARE_NEW_MASTER:
			p = &(recv_pack->wd_body.wd_info);	
			wd_set_wd_list(p->hostname,p->pgpool_port, p->wd_port, p->delegate_ip, &(p->tv), p->status);
			if (WD_MYSELF->status == WD_MASTER)
			{
				/* resign master server */
				pool_log("wd_declare_new_master: ifconfig down to resign master server");
				wd_IP_down();
				wd_set_myself(NULL, WD_NORMAL);
			}
			send_packet.packet_no = WD_READY;
			memcpy(&(send_packet.wd_body.wd_info), WD_MYSELF, sizeof(WdInfo));
			break;

		/* announce to assume lock holder */
		case WD_STAND_FOR_LOCK_HOLDER:
			p = &(recv_pack->wd_body.wd_info);	
			wd_set_wd_list(p->hostname,p->pgpool_port, p->wd_port, p->delegate_ip, &(p->tv), p->status);
			/* only master handles lock holder privilege */
			if (WD_MYSELF->status == WD_MASTER)
			{
				/* if there are no lock holder yet */
				if (wd_get_lock_holder() != NULL)
				{
					send_packet.packet_no = WD_LOCK_HOLDER_EXIST;
				}
			}
			memcpy(&(send_packet.wd_body.wd_info), WD_MYSELF, sizeof(WdInfo));
			break;

		case WD_DECLARE_LOCK_HOLDER:
			p = &(recv_pack->wd_body.wd_info);	
			wd_set_wd_list(p->hostname,p->pgpool_port, p->wd_port, p->delegate_ip, &(p->tv), p->status);
			wd_set_lock_holder(p, true);
			send_packet.packet_no = WD_READY;
			memcpy(&(send_packet.wd_body.wd_info), WD_MYSELF, sizeof(WdInfo));
			break;

		/* announce to resign lock holder */
		case WD_RESIGN_LOCK_HOLDER:
			p = &(recv_pack->wd_body.wd_info);	
			wd_set_wd_list(p->hostname,p->pgpool_port, p->wd_port, p->delegate_ip, &(p->tv), p->status);
			wd_set_lock_holder(p, false);
			send_packet.packet_no = WD_READY;
			memcpy(&(send_packet.wd_body.wd_info), WD_MYSELF, sizeof(WdInfo));
			break;

		case WD_START_INTERLOCK:
			p = &(recv_pack->wd_body.wd_info);	
			wd_set_wd_list(p->hostname,p->pgpool_port, p->wd_port, p->delegate_ip, &(p->tv), p->status);
			wd_set_interlocking(p, true);
			break;

		case WD_END_INTERLOCK:
			p = &(recv_pack->wd_body.wd_info);	
			wd_set_wd_list(p->hostname,p->pgpool_port, p->wd_port, p->delegate_ip, &(p->tv), p->status);
			wd_set_interlocking(p, false);
			break;

		/* announce that server is down */
		case WD_SERVER_DOWN:
			p = &(recv_pack->wd_body.wd_info);	
			wd_set_wd_list(p->hostname,p->pgpool_port, p->wd_port, p->delegate_ip, &(p->tv), WD_DOWN);
			send_packet.packet_no = WD_READY;
			memcpy(&(send_packet.wd_body.wd_info), WD_MYSELF, sizeof(WdInfo));
			if (wd_am_I_oldest() == WD_OK && WD_MYSELF->status != WD_MASTER)
			{
				wd_escalation();
			}
			break;

		/* announce start online recovery */
		case WD_START_RECOVERY:
			if (*InRecovery != RECOVERY_INIT)
			{
				send_packet.packet_no = WD_NODE_FAILED;
			}
			else
			{
				send_packet.packet_no = WD_NODE_READY;
				*InRecovery = RECOVERY_ONLINE;
				if (wait_connection_closed() != 0)
				{
					send_packet.packet_no = WD_NODE_FAILED;
				}
			}
			break;
		case WD_END_RECOVERY:
			send_packet.packet_no = WD_NODE_READY;
			*InRecovery = RECOVERY_INIT;
			kill(wd_ppid, SIGUSR2);
			break;
		case WD_FAILBACK_REQUEST:
			node = &(recv_pack->wd_body.wd_node_info);	
			wd_set_node_mask(WD_FAILBACK_REQUEST,node->node_id_set,node->node_num);
			is_node_packet = true;
			send_packet.packet_no = WD_NODE_READY;
			break;
		case WD_DEGENERATE_BACKEND:
			node = &(recv_pack->wd_body.wd_node_info);	
			wd_set_node_mask(WD_DEGENERATE_BACKEND,node->node_id_set, node->node_num);
			is_node_packet = true;
			send_packet.packet_no = WD_NODE_READY;
			break;
		case WD_PROMOTE_BACKEND:
			node = &(recv_pack->wd_body.wd_node_info);	
			wd_set_node_mask(WD_PROMOTE_BACKEND,node->node_id_set, node->node_num);
			is_node_packet = true;
			send_packet.packet_no = WD_NODE_READY;
			break;

		case WD_UNLOCK_REQUEST:
			lock = &(recv_pack->wd_body.wd_lock_info);	
			wd_set_lock(lock->lock_id, false);
			send_packet.packet_no = WD_LOCK_READY;
			break;

		default:
			send_packet.packet_no = WD_INVALID;
			memcpy(&(send_packet.wd_body.wd_info), WD_MYSELF, sizeof(WdInfo));
			break;
	}

	/* send response packet */
	rtn = wd_send_packet(sock, &send_packet);

	/* send node request signal.
	 * wd_node_request_signal() uses a semaphore lock internally, so should be
	 * called after sending a response packet to prevent dead lock.
	 */
	if (is_node_packet)
		wd_node_request_signal(recv_pack->packet_no, node);

	return rtn;
}

/* send node request signal */
static void
wd_node_request_signal(WD_PACKET_NO packet_no, WdNodeInfo *node)
{
	switch (packet_no)
	{
		case WD_FAILBACK_REQUEST:
			send_failback_request(node->node_id_set[0]);
			break;
		case WD_DEGENERATE_BACKEND:
			degenerate_backend_set(node->node_id_set, node->node_num);
			break;
		case WD_PROMOTE_BACKEND:
			promote_backend(node->node_id_set[0]);
			break;
		default:
			pool_error("wd_node_request_signal: unknown packet number");
			break;
	}
}
