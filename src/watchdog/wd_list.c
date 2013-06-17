/*
 * $Header$
 *
 * Handles watchdog connection, and protocol communication with pgpool-II
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
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>

#include "pool.h"
#include "pool_config.h"
#include "watchdog/watchdog.h"
#include "watchdog/wd_ext.h"

int wd_add_wd_list(WdDesc * other_wd);
int wd_set_wd_info(WdInfo * info);
WdInfo * wd_is_exist_master(void);
WdInfo * wd_get_lock_holder(void);
WdInfo * wd_get_interlocking(void);
void wd_set_lock_holder(WdInfo *info, bool value);
void wd_set_interlocking(WdInfo *info, bool value);
void wd_clear_interlocking_info(void);
int wd_am_I_oldest(void);
int wd_set_myself(struct timeval * tv, int status);
WdInfo * wd_is_alive_master(void);

/* add or modify watchdog information list */
int
wd_set_wd_list(char * hostname, int pgpool_port, int wd_port, char * delegate_ip, struct timeval * tv, int status)
{
	int i = 0;
	WdInfo * p = NULL;

	if ((WD_List == NULL) || (hostname == NULL))
	{
		pool_error("wd_set_wd_list: memory allocate error");
		return -1;
	}

	if (strcmp(pool_config->delegate_IP, delegate_ip))
	{
		pool_error("wd_set_wd_list: delegate IP mismatch error");
		return -1;
	}

	for ( i = 0 ; i < MAX_WATCHDOG_NUM ; i ++)
	{
		p = (WD_List+i);	

		if( p->status != WD_END)
		{
			/* found; modify the pgpool. */
			if ((!strncmp(p->hostname, hostname, sizeof(p->hostname))) &&
				(p->pgpool_port == pgpool_port)	&&
				(p->wd_port == wd_port))
			{
				p->status = status;

				if (tv != NULL)
				{
					memcpy(&(p->tv), tv, sizeof(struct timeval));
				}

				return i;
			}
		}

		/* not found; add as a new pgpool */
		else
		{
			p->status = status;
			p->pgpool_port = pgpool_port;
			p->wd_port = wd_port;
			p->in_interlocking = false;
			p->is_lock_holder = false;

			strlcpy(p->hostname, hostname, sizeof(p->hostname));
			strlcpy(p->delegate_ip, delegate_ip, sizeof(p->delegate_ip));

			if (tv != NULL)
			{
				memcpy(&(p->tv), tv, sizeof(struct timeval));
			}

			return i;
		}
	}

	pool_error("wd_set_wd_list: Can not add new watchdog information cause the WD_List is full.");
	return -1;
}

/* add watchdog information to list using config description */
int
wd_add_wd_list(WdDesc * other_wd)
{
	WdInfo * p = NULL;
	int i = 0;

	if (other_wd == NULL)
	{
		pool_error("wd_add_wd_list: memory allocate error");
		return -1;
	}

	for ( i = 0 ; i < other_wd->num_wd ; i ++)
	{
		p = &(other_wd->wd_info[i]);
		strlcpy(p->delegate_ip, pool_config->delegate_IP, sizeof(p->delegate_ip));
		wd_set_wd_info(p);
	}

	return i;
}

/* set watchdog information to list */
int
wd_set_wd_info(WdInfo * info)
{
	int rtn;
	rtn = wd_set_wd_list(info->hostname, info->pgpool_port, info->wd_port,
	                     info->delegate_ip, &(info->tv), info->status);
	return rtn;
}

/* return master if exist, NULL if not found */
WdInfo *
wd_is_exist_master(void)
{
	WdInfo * p = WD_List;

	p++;
	while (p->status != WD_END)
	{
		/* find master pgpool in the other servers */
		if (p->status == WD_MASTER)
		{
			/* master found */
			return p;
		}
		p++;
	}
	/* not found */
	return NULL;	
}

/* set or unset in_interlocking flag */
void
wd_set_interlocking(WdInfo *info, bool value)
{
	WdInfo * p = WD_List;

	while (p->status != WD_END)
	{
		if ((!strncmp(p->hostname, info->hostname, sizeof(p->hostname))) &&
			(p->pgpool_port == info->pgpool_port) &&
			(p->wd_port == info->wd_port))
		{
			p->in_interlocking = value;
			
			return;
		}
		p++;
	}
}

/* set or unset lock holder flag */
void
wd_set_lock_holder(WdInfo *info, bool value)
{
	WdInfo * p = WD_List;

	while (p->status != WD_END)
	{
		if ((!strncmp(p->hostname, info->hostname, sizeof(p->hostname))) &&
			(p->pgpool_port == info->pgpool_port) &&
			(p->wd_port == info->wd_port))
		{
			p->is_lock_holder = value;

			return;
		}
		p++;
	}
}

/* return the lock holder if exist, NULL if not found */
WdInfo *
wd_get_lock_holder(void)
{
	WdInfo * p = WD_List;

	while (p->status != WD_END)
	{
		/* find failover lock holder */
		if ((p->status == WD_NORMAL || p->status == WD_MASTER) &&
			p->is_lock_holder)
		{
			/* found */
			return p;
		}
		p++;
	}

	/* not found */
	return NULL;	
}

/* return the pgpool in interlocking found in first, NULL if not found */
WdInfo *
wd_get_interlocking(void)
{
	WdInfo * p = WD_List;

	while (p->status != WD_END)
	{
		/* find pgpool in interlocking */
		if ((p->status == WD_NORMAL || p->status == WD_MASTER) &&
		    p->in_interlocking)
		{
			/* found */
			return p;
		}
		p++;
	}

	/* not found */
	return NULL;	
}

/* clear flags for interlocking */
void
wd_clear_interlocking_info(void)
{
	WdInfo * p = WD_List;

	while (p->status != WD_END)
	{
		wd_set_lock_holder(p, false);
		wd_set_interlocking(p, false);
		p++;
	}
}

int
wd_am_I_oldest(void)
{
	WdInfo * p = WD_List;

	p++;
	while (p->status != WD_END)
	{
		if ((p->status == WD_NORMAL) ||
			(p->status == WD_MASTER))
		{
			if (WD_TIME_BEFORE(p->tv, WD_MYSELF->tv))
			{
				return WD_NG;
			}
		}
		p++;
	}
	return WD_OK;
}

int
wd_set_myself(struct timeval * tv, int status)
{
	if (WD_MYSELF == NULL)
	{
		return WD_NG;
	}

	if (tv != NULL)
	{
		memcpy(&(WD_MYSELF->tv),tv,sizeof(struct timeval));
	}

	WD_MYSELF->status = status;

	return WD_OK;
}

WdInfo *
wd_is_alive_master(void)
{
	WdInfo * master = NULL;

	master = wd_is_exist_master();
	if (master != NULL)
	{
		if (!strcmp(pool_config->wd_lifecheck_method, MODE_HEARTBEAT) ||
		    (!strcmp(pool_config->wd_lifecheck_method, MODE_QUERY) &&
			 wd_ping_pgpool(master) == WD_OK))
		{
			return master;
		}
	}
	return NULL;
}

