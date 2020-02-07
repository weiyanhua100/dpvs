/*
 * Soft:        Keepalived is a failover program for the LVS project
 *              <www.linuxvirtualserver.org>. It monitor & manipulate
 *              a loadbalanced server pool using multi-layer checks.
 *
 * Part:        Manipulation functions for IPVS & IPFW wrappers.
 *
 * Author:      Alexandre Cassen, <acassen@linux-vs.org>
 *
 *              This program is distributed in the hope that it will be useful,
 *              but WITHOUT ANY WARRANTY; without even the implied warranty of
 *              MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *              See the GNU General Public License for more details.
 *
 *              This program is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU General Public License
 *              as published by the Free Software Foundation; either version
 *              2 of the License, or (at your option) any later version.
 *
 * Copyright (C) 2001-2017 Alexandre Cassen, <acassen@gmail.com>
 */

#include "config.h"

#include <unistd.h>
#include <inttypes.h>

#include "ipwrapper.h"
#include "check_api.h"
#include "logger.h"
#include "utils.h"
#include "main.h"
#ifdef _WITH_SNMP_CHECKER_
  #include "check_snmp.h"
#endif
#include "global_data.h"
#include "smtp.h"
#include "check_daemon.h"

static bool __attribute((pure))
vs_iseq(const virtual_server_t *vs_a, const virtual_server_t *vs_b)
{
	if (!vs_a->vsgname != !vs_b->vsgname)
		return false;

	if (vs_a->vsgname) {
		/* Should we check the vsg entries match? */
		if (inet_sockaddrport(&vs_a->addr) != inet_sockaddrport(&vs_b->addr))
			return false;

		return !strcmp(vs_a->vsgname, vs_b->vsgname);
	} else if (vs_a->af != vs_b->af)
		return false;
	else if (vs_a->vfwmark) {
		if (vs_a->vfwmark != vs_b->vfwmark)
			return false;
	} else {
		if (vs_a->service_type != vs_b->service_type ||
		    !sockstorage_equal(&vs_a->addr, &vs_b->addr))
			return false;
	}

	return true;
}

static bool __attribute((pure))
vsge_iseq(const virtual_server_group_entry_t *vsge_a, const virtual_server_group_entry_t *vsge_b)
{
	if (vsge_a->is_fwmark != vsge_b->is_fwmark)
		return false;

	if (vsge_a->is_fwmark)
		return vsge_a->vfwmark == vsge_b->vfwmark;

	if (!sockstorage_equal(&vsge_a->addr, &vsge_b->addr) ||
	    vsge_a->range != vsge_b->range)
		return false;

	return true;
}

static bool __attribute((pure))
rs_iseq(const real_server_t *rs_a, const real_server_t *rs_b)
{
	return sockstorage_equal(&rs_a->addr, &rs_b->addr);
}

/* Returns the sum of all alive RS weight in a virtual server. */
static unsigned long __attribute__ ((pure))
weigh_live_realservers(virtual_server_t * vs)
{
	element e;
	real_server_t *svr;
	long count = 0;

	LIST_FOREACH(vs->rs, svr, e) {
		if (ISALIVE(svr))
			count += svr->weight;
	}
	return count;
}

static void
notify_fifo_vs(virtual_server_t* vs)
{
	const char *state = vs->quorum_state_up ? "UP" : "DOWN";
	size_t size;
	char *line;
	const char *vs_str;

	if (global_data->notify_fifo.fd == -1 &&
	    global_data->lvs_notify_fifo.fd == -1)
		return;

	vs_str = FMT_VS(vs);
	size = strlen(vs_str) + strlen(state) + 5;
	line = MALLOC(size + 1);
	if (!line)
		return;

	snprintf(line, size + 1, "VS %s %s\n", vs_str, state);

	if (global_data->notify_fifo.fd != -1)
		if (write(global_data->notify_fifo.fd, line, size) == -1) {}

	if (global_data->lvs_notify_fifo.fd != -1)
		if (write(global_data->lvs_notify_fifo.fd, line, size) == -1) {}

	FREE(line);
}

static void
notify_fifo_rs(virtual_server_t* vs, real_server_t* rs)
{
	const char *state = rs->alive ? "UP" : "DOWN";
	size_t size;
	char *line;
	const char *rs_str;
	const char *vs_str;

	if (global_data->notify_fifo.fd == -1 &&
	    global_data->lvs_notify_fifo.fd == -1)
		return;

	rs_str = FMT_RS(rs, vs);
	vs_str = FMT_VS(vs);
	size = strlen(rs_str) + strlen(vs_str) + strlen(state) + 6;
	line = MALLOC(size + 1);
	if (!line)
		return;

	snprintf(line, size + 1, "RS %s %s %s\n", rs_str, vs_str, state);

	if (global_data->notify_fifo.fd != -1)
		if (write(global_data->notify_fifo.fd, line, size) == - 1) {}

	if (global_data->lvs_notify_fifo.fd != -1)
		if (write(global_data->lvs_notify_fifo.fd, line, size) == -1) {}

	FREE(line);
}

static void
do_vs_notifies(virtual_server_t* vs, bool init, long threshold, long weight_sum, bool stopping)
{
	notify_script_t *notify_script = vs->quorum_state_up ? vs->notify_quorum_up : vs->notify_quorum_down;
	char message[80];

#ifdef _WITH_SNMP_CHECKER_
	check_snmp_quorum_trap(vs, stopping);
#endif

	/* Only send non SNMP notifies when stopping if omega set */
	if (stopping && !vs->omega)
		return;

	if (notify_script) {
		if (stopping)
			system_call_script(master, child_killed_thread, NULL, TIMER_HZ, notify_script);
		else
			notify_exec(notify_script);
	}

	notify_fifo_vs(vs);

	if (vs->smtp_alert) {
		if (stopping)
			snprintf(message, sizeof(message), "=> Shutting down <=");
		else
			snprintf(message, sizeof(message), "=> %s %u+%u=%ld <= %ld <=",
				    vs->quorum_state_up ?
						   init ? "Starting with quorum up" :
							  "Gained quorum" :
						   init ? "Starting with quorum down" :
							  "Lost quorum",
				    vs->quorum,
				    vs->hysteresis,
				    threshold,
				    weight_sum);
		smtp_alert(SMTP_MSG_VS, vs, vs->quorum_state_up ? "UP" : "DOWN", message);
	}
}

static void
do_rs_notifies(virtual_server_t* vs, real_server_t* rs, bool stopping)
{
	notify_script_t *notify_script = rs->alive ? rs->notify_up : rs->notify_down;

	if (notify_script) {
		if (stopping)
			system_call_script(master, child_killed_thread, NULL, TIMER_HZ, notify_script);
		else
			notify_exec(notify_script);
	}

	notify_fifo_rs(vs, rs);

	/* The sending of smtp_alerts is handled by the individual checker
	 * so that the message can have context for the checker */

#ifdef _WITH_SNMP_CHECKER_
	check_snmp_rs_trap(rs, vs, stopping);
#endif
}

/* Remove a realserver IPVS rule */
static void
clear_service_rs(virtual_server_t * vs, list l, bool stopping)
{
	element e;
	real_server_t *rs;
	long weight_sum;
	long threshold = vs->quorum - vs->hysteresis;
	bool sav_inhibit;
	smtp_rs rs_info = { .vs = vs };

	LIST_FOREACH(l, rs, e) {
		if (rs->set || stopping)
			log_message(LOG_INFO, "%s %sservice %s from VS %s",
					stopping ? "Shutting down" : "Removing",
					rs->inhibit && !rs->alive ? "(inhibited) " : "",
					FMT_RS(rs, vs),
					FMT_VS(vs));

		if (!rs->set)
			continue;

		/* Force removal of real servers with inhibit_on_failure set */
		sav_inhibit = rs->inhibit;
		rs->inhibit = false;

		ipvs_cmd(LVS_CMD_DEL_DEST, vs, rs);

		rs->inhibit = sav_inhibit;	/* Restore inhibit flag */

		if (!rs->alive)
			continue;

		UNSET_ALIVE(rs);

		/* We always want to send SNMP messages on shutdown */
		if (!vs->omega && stopping) {
#ifdef _WITH_SNMP_CHECKER_
			check_snmp_rs_trap(rs, vs, true);
#endif
			continue;
		}

		/* In Omega mode we call VS and RS down notifiers
		 * all the way down the exit, as necessary.
		 */
		do_rs_notifies(vs, rs, stopping);

		/* Send SMTP alert */
		if (rs->smtp_alert) {
			rs_info.rs = rs;
			smtp_alert(SMTP_MSG_RS_SHUT, &rs_info, "DOWN", stopping ? "=> Shutting down <=" : "=> Removing <=");
		}
	}

	/* Sooner or later VS will lose the quorum (if any). However,
	 * we don't push in a sorry server then, hence the regression
	 * is intended.
	 */
	weight_sum = weigh_live_realservers(vs);
	if (stopping ||
	    (vs->quorum_state_up &&
	     (!weight_sum || weight_sum < threshold))) {
		vs->quorum_state_up = false;
		do_vs_notifies(vs, false, threshold, weight_sum, stopping);
	}
}

/* Remove a virtualserver IPVS rule */
static void
clear_service_vs(virtual_server_t * vs, bool stopping)
{
	bool sav_inhibit;

	if (global_data->lvs_flush_onstop == LVS_NO_FLUSH) {
		/* Processing real server queue */
		if (vs->s_svr && vs->s_svr->set) {
			/* Ensure removed if inhibit_on_failure set */
			sav_inhibit = vs->s_svr->inhibit;
			vs->s_svr->inhibit = false;

			ipvs_cmd(LVS_CMD_DEL_DEST, vs, vs->s_svr);

			vs->s_svr->inhibit = sav_inhibit;

			UNSET_ALIVE(vs->s_svr);
		}

		/* Even if the sorry server was configured, if we are using
		 * inhibit_on_failure, then real servers may be configured. */
		clear_service_rs(vs, vs->rs, stopping);
	}
	else if (vs->s_svr && vs->s_svr->set)
		UNSET_ALIVE(vs->s_svr);

	/* The above will handle Omega case for VS as well. */

	ipvs_cmd(LVS_CMD_DEL, vs, NULL);

	UNSET_ALIVE(vs);
}

/* IPVS cleaner processing */
void
clear_services(void)
{
	if (!check_data)
		return;

	element e;
	virtual_server_t *vs;

	if (!check_data || !check_data->vs)
		return;

	LIST_FOREACH(check_data->vs, vs, e) {
		/* Remove the real servers, and clear the vs unless it is
		 * using a VS group and it is not the last vs of the same
		 * protocol or address family using the group. */
		clear_service_vs(vs, true);
	}
}

/* Set a realserver IPVS rules */
static bool
init_service_rs(virtual_server_t * vs)
{
	element e;
	real_server_t *rs;

	LIST_FOREACH(vs->rs, rs, e) {
		if (rs->reloaded) {
			if (rs->iweight != rs->pweight)
				update_svr_wgt(rs->iweight, vs, rs, false);
			/* Do not re-add failed RS instantly on reload */
			continue;
		}

		/* In alpha mode, be pessimistic (or realistic?) and don't
		 * add real servers into the VS pool unless inhibit_on_failure.
		 * They will get there later upon healthchecks recovery (if ever).
		 */
		if ((!rs->num_failed_checkers && !ISALIVE(rs)) ||
		    (rs->inhibit && !rs->set)) {
			ipvs_cmd(LVS_CMD_ADD_DEST, vs, rs);
			if (!rs->num_failed_checkers) {
				SET_ALIVE(rs);
				if (global_data->rs_init_notifies)
					do_rs_notifies(vs, rs, false);
			}
		}
	}

	return true;
}

static int init_tunnel_entry(tunnel_entry *entry)
{
	return ipvs_tunnel_cmd(LVS_CMD_ADD_TUNNEL, entry);
}

static int init_tunnel_group(tunnel_group* group)
{
	list l;
	element e;
	tunnel_entry* entry;

	l = group->tunnel_entry;
	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		entry = ELEMENT_DATA(e);
		if (!init_tunnel_entry(entry)) {
			log_message(LOG_ERR, "%s create tunnel %s error.", __FUNCTION__, entry->ifname);
			return IPVS_ERROR;
		}
	}

	return IPVS_SUCCESS;
}

int init_tunnel(void)
{
	element e;
	list l = check_data->tunnel_group;
	tunnel_group* entry;

	if (LIST_ISEMPTY(l))
		return IPVS_SUCCESS;

	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		entry = ELEMENT_DATA(e);
		if (!init_tunnel_group(entry)) {
			log_message(LOG_ERR, "%s create tunnel group %s error.", __FUNCTION__, entry->gname);
			return IPVS_ERROR;
		}
	}
	return IPVS_SUCCESS;
}

static void
sync_service_vsg(virtual_server_t * vs)
{
	virtual_server_group_t *vsg;
	virtual_server_group_entry_t *vsge;
	list *l;
	element e;

	vsg = vs->vsg;
	list ll[] = {
		vsg->addr_range,
		vsg->vfwmark,
		NULL,
	};

	for (l = ll; *l; l++) {
		LIST_FOREACH(*l, vsge, e) {
			if (!vsge->reloaded) {
				log_message(LOG_INFO, "VS [%s:%" PRIu32 ":%u] added into group %s"
// Does this work with no address?
						    , inet_sockaddrtotrio(&vsge->addr, vs->service_type)
						    , vsge->range
						    , vsge->vfwmark
						    , vs->vsgname);
				/* add all reloaded and alive/inhibit-set dests
				 * to the newly created vsg item */
				ipvs_group_sync_entry(vs, vsge);
			}
		}
	}
}



/* add or remove _alive_ real servers from a virtual server */
static void
perform_quorum_state(virtual_server_t *vs, bool add)
{
	element e;
	real_server_t *rs;

	log_message(LOG_INFO, "%s the pool for VS %s"
			    , add?"Adding alive servers to":"Removing alive servers from"
			    , FMT_VS(vs));
	LIST_FOREACH(vs->rs, rs, e) {
		if (!ISALIVE(rs)) /* We only handle alive servers */
			continue;
// ??? The following seems unnecessary
		if (add)
			rs->alive = false;
		ipvs_cmd(add?LVS_CMD_ADD_DEST:LVS_CMD_DEL_DEST, vs, rs);
		rs->alive = true;
	}
}

void
set_quorum_states(void)
{
	virtual_server_t *vs;
	element e;

	if (LIST_ISEMPTY(check_data->vs))
		return;

	for (e = LIST_HEAD(check_data->vs); e; ELEMENT_NEXT(e)) {
		vs = ELEMENT_DATA(e);

		vs->quorum_state_up = (weigh_live_realservers(vs) >= vs->quorum + vs->hysteresis);
	}
}

/* set quorum state depending on current weight of real servers */
static void
update_quorum_state(virtual_server_t * vs, bool init)
{
	long weight_sum = weigh_live_realservers(vs);
	long threshold;

	threshold = vs->quorum + (vs->quorum_state_up ? -1 : 1) * vs->hysteresis;

	/* If we have just gained quorum, it's time to consider notify_up. */
	if (!vs->quorum_state_up &&
	    weight_sum >= threshold) {
		vs->quorum_state_up = true;
		log_message(LOG_INFO, "Gained quorum %u+%u=%ld <= %ld for VS %s"
				    , vs->quorum
				    , vs->hysteresis
				    , threshold
				    , weight_sum
				    , FMT_VS(vs));
		if (vs->s_svr && ISALIVE(vs->s_svr)) {
			/* Adding back alive real servers */
			perform_quorum_state(vs, true);

			log_message(LOG_INFO, "%s sorry server %s from VS %s"
					    , (vs->s_svr->inhibit ? "Disabling" : "Removing")
					    , FMT_RS(vs->s_svr, vs)
					    , FMT_VS(vs));

			ipvs_cmd(LVS_CMD_DEL_DEST, vs, vs->s_svr);
			vs->s_svr->alive = false;
		}

		do_vs_notifies(vs, init, threshold, weight_sum, false);

		return;
	}
	else if ((vs->quorum_state_up &&
		  (!weight_sum || weight_sum < threshold)) ||
		 (init && !vs->quorum_state_up &&
		  vs->s_svr && !ISALIVE(vs->s_svr))) {
		/* We have just lost quorum for the VS, we need to consider
		 * VS notify_down and sorry_server cases
		 *   or
		 * We are starting up and need to add the sorry server
		 */
		vs->quorum_state_up = false;
		log_message(LOG_INFO, "%s %u-%u=%ld > %ld for VS %s"
				    , init ? "Starting with quorum down" : "Lost quorum"
				    , vs->quorum
				    , vs->hysteresis
				    , threshold
				    , weight_sum
				    , FMT_VS(vs));

		if (vs->s_svr && !ISALIVE(vs->s_svr)) {
			log_message(LOG_INFO, "%s sorry server %s to VS %s"
					    , (vs->s_svr->inhibit ? "Enabling" : "Adding")
					    , FMT_RS(vs->s_svr, vs)
					    , FMT_VS(vs));

			/* the sorry server is now up in the pool, we flag it alive */
			ipvs_cmd(LVS_CMD_ADD_DEST, vs, vs->s_svr);
			vs->s_svr->alive = true;

			/* Remove remaining alive real servers */
			perform_quorum_state(vs, false);
		}

		do_vs_notifies(vs, init, threshold, weight_sum, false);
	}
}

static int
rs_aratio_action_exec(char *cmd)
{
	pid_t pid;
	int retval;

	pid = fork();

	/* In case of fork is error. */
	if (pid < 0) {
		syslog(LOG_INFO, "Failed fork process");
		return -1;
	}

	/* In case of this is parent process */
	if (pid)
		return 0;

	retval = system(cmd);
	if (retval == 127) {
		/* couldn't exec command */
		syslog(LOG_ALERT, "Couldn't exec command: %s", cmd);
	} else if (retval == -1) {
		/* other error */
		syslog(LOG_ALERT, "Error exec-ing command: %s", cmd);
	}

	exit(0);
}

static int rs_aratio_action_addr(int limit, virtual_server_t *vs,
                                            struct sockaddr_storage *addr)
{
    char buf[512];
    if (vs->rs_aratio_action) {
        memset(buf, 0, sizeof(buf));
        snprintf(buf, sizeof(buf), "%s %s %s",
            vs->rs_aratio_action, inet_sockaddrtos(addr), limit ? "upper" : "lower");
        log_message(LOG_INFO, "rs_aratio_action %s\n", buf);
        rs_aratio_action_exec(buf);
    }

    return 1;
}

static int rs_aratio_action_group_range(int limit, virtual_server_t *vs,
											virtual_server_group_entry_t *vsg_entry)
{
	uint32_t addr_ip, ip;
	struct sockaddr_storage vip_addr;

	vip_addr = vsg_entry->addr;
	if (vsg_entry->addr.ss_family == AF_INET6) {
		//inet_sockaddrip6(&vsg_entry->addr, &addr.in6);
		ip = ((struct sockaddr_in6*)&vip_addr)->sin6_addr.s6_addr32[3];
	} else {
		ip = ((struct sockaddr_in*)&vip_addr)->sin_addr.s_addr;
	}

	/* Parse the whole range */
	for (addr_ip = ip;
	     ((addr_ip >> 24) & 0xFF) <= vsg_entry->range;
	     addr_ip += 0x01000000) {
		if (vsg_entry->addr.ss_family == AF_INET6) {
			((struct sockaddr_in6*)&vip_addr)->sin6_addr.s6_addr32[3] = addr_ip;
		} else {
			((struct sockaddr_in*)&vip_addr)->sin_addr.s_addr = addr_ip;
		}
		rs_aratio_action_addr(limit, vs, &vip_addr);
	}

	return 1;
}
static int rs_aratio_action_group (int limit, virtual_server_t *vs)
{
	virtual_server_group_t *vsg = ipvs_get_group_by_name(vs->vsgname, check_data->vs_group);
	virtual_server_group_entry_t *vsg_entry;
	list l;
	element e;

	if (!vsg) return IPVS_ERROR;

	/* visit range list */
	l = vsg->addr_range;
	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		vsg_entry = ELEMENT_DATA(e);
		rs_aratio_action_group_range(limit, vs, vsg_entry);
	}

	return 1;
}

static int rs_aratio_action(int limit, virtual_server_t *vs)
{
    if (vs->vsgname) {
        rs_aratio_action_group(limit, vs);
    } else {
        rs_aratio_action_addr(limit, vs, &(vs->addr));
    }

    return 1;
}

static int rs_aratio_reach_upper_limit(thread_ref_t arg) {
	virtual_server_t * vs = THREAD_ARG(arg);
	int rs_alive_ratio = 0;

	vs->rs_upper_limit_thread = NULL;
	rs_alive_ratio = vs->rs_alive_count*100/LIST_SIZE(vs->rs);
	if (rs_alive_ratio >= vs->rs_aratio_upper_limit) {
		log_message(LOG_INFO, "VS [%s] rs_alive_ratio (%d%%) >= rs_aratio_upper_limit (%d%%) execute action",
				(vs->vsgname) ? vs->vsgname : FMT_VS(vs)
				, rs_alive_ratio
				, vs->rs_aratio_upper_limit);
		vs->flag &= ~RS_ARATIO_REACH_LOWER_LIMIT;
		rs_aratio_action(1, vs);
	} else {
		log_message(LOG_ERR, "VS [%s] rs_alive_ratio (%d%%) < rs_aratio_upper_limit (%d%%) does not execute action",
				(vs->vsgname) ? vs->vsgname : FMT_VS(vs)
				, rs_alive_ratio
				, vs->rs_aratio_upper_limit);
	}

	return 0;
}

static int vs_addr_cmp(struct sockaddr_storage *addr_a, struct sockaddr_storage *addr_b)
{
    int ret = 0;

    if (addr_a->ss_family != addr_b->ss_family)
        return 0;

    if (addr_a->ss_family == AF_INET6) {
        ret = inaddr_equal(AF_INET6, &((struct sockaddr_in6 *)addr_a)->sin6_addr,
            &((struct sockaddr_in6 *)addr_b)->sin6_addr);
    } else {
        ret = inaddr_equal(AF_INET, &((struct sockaddr_in *)addr_a)->sin_addr,
            &((struct sockaddr_in *)addr_b)->sin_addr);
    }

    return ret;
}

static int __vs_group_rang_addr_cmp(struct sockaddr_storage *addr,
										virtual_server_group_entry_t *vsg_entry)
{
	uint32_t addr_ip, ip;
	struct sockaddr_storage vip_addr;
	int ret = 0;

	vip_addr = vsg_entry->addr;
	if (vsg_entry->addr.ss_family == AF_INET6) {
		//inet_sockaddrip6(&vsg_entry->addr, &addr.in6);
		ip = ((struct sockaddr_in6*)&vip_addr)->sin6_addr.s6_addr32[3];
	} else {
		ip = ((struct sockaddr_in*)&vip_addr)->sin_addr.s_addr;
	}

	/* Parse the whole range */
	for (addr_ip = ip;
	     ((addr_ip >> 24) & 0xFF) <= vsg_entry->range;
	     addr_ip += 0x01000000) {
		if (vsg_entry->addr.ss_family == AF_INET6) {
			((struct sockaddr_in6*)&vip_addr)->sin6_addr.s6_addr32[3] = addr_ip;
		} else {
			((struct sockaddr_in*)&vip_addr)->sin_addr.s_addr = addr_ip;
		}

		ret |= vs_addr_cmp(addr, &vip_addr);
	}
	return ret;

}

static int __vs_group_addr_cmp(struct sockaddr_storage *addr, virtual_server_group_t *vsg)
{
	virtual_server_group_entry_t *vsg_entry = NULL;
	list l;
	element e;
	int ret = 0;

	/* visit addr_ip list */
	l = vsg->addr_ip;
	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		vsg_entry = ELEMENT_DATA(e);
		ret |= vs_addr_cmp(addr, &vsg_entry->addr);
	}

	/* visit range list */
	l = vsg->addr_range;
	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		vsg_entry = ELEMENT_DATA(e);
		ret |= __vs_group_rang_addr_cmp(addr, vsg_entry);
	}

    return ret;
}

static int __vs_group_rang_cmp(virtual_server_group_entry_t *vsg_entry,
                                        virtual_server_group_t *vsg)
{
	uint32_t addr_ip, ip;
	struct sockaddr_storage vip_addr;
	int ret = 0;

	vip_addr = vsg_entry->addr;
	if (vsg_entry->addr.ss_family == AF_INET6) {
		//inet_sockaddrip6(&vsg_entry->addr, &addr.in6);
		ip = ((struct sockaddr_in6*)&vip_addr)->sin6_addr.s6_addr32[3];
	} else {
		ip = ((struct sockaddr_in*)&vip_addr)->sin_addr.s_addr;
	}

	/* Parse the whole range */
	for (addr_ip = ip;
	     ((addr_ip >> 24) & 0xFF) <= vsg_entry->range;
	     addr_ip += 0x01000000) {
		if (vsg_entry->addr.ss_family == AF_INET6) {
			((struct sockaddr_in6*)&vip_addr)->sin6_addr.s6_addr32[3] = addr_ip;
		} else {
			((struct sockaddr_in*)&vip_addr)->sin_addr.s_addr = addr_ip;
		}
		ret |= __vs_group_addr_cmp(&vip_addr, vsg);
	}
	return ret;
}

static int vs_group_addr_cmp(virtual_server_t *vs_a, virtual_server_t *vs_b)
{
    virtual_server_group_t *vsg_a = ipvs_get_group_by_name(vs_a->vsgname, check_data->vs_group);
    virtual_server_group_t *vsg_b = ipvs_get_group_by_name(vs_b->vsgname, check_data->vs_group);
    virtual_server_group_entry_t *vsg_entry;
    list l;
    element e;
    int ret = 0;

    if (vsg_a == NULL || vsg_b == NULL)
        return 0;

    if (vsg_a == vsg_b)
        return 1;

	/* visit addr_ip list */
	l = vsg_a->addr_ip;
	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
        vsg_entry = ELEMENT_DATA(e);
        ret |= __vs_group_addr_cmp(&vsg_entry->addr, vsg_b);
	}

	/* visit range list */
	l = vsg_a->addr_range;
	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
        vsg_entry = ELEMENT_DATA(e);
        ret |= __vs_group_rang_cmp(vsg_entry, vsg_b);
	}

    return ret;
}

static int all_vs_rs_aratio_reach_lower_limit(virtual_server_t *var_vs)
{
    element e;
    virtual_server_t *tmp_vs = NULL;
    virtual_server_group_t *vsg = NULL;
    list l;
    int ret = 0;

	if (NULL == check_data) {
		return 1;
	}

	l = check_data->vs;

	if (LIST_ISEMPTY(l)) {
		return 1;
	}

	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		tmp_vs = ELEMENT_DATA(e);
        if (tmp_vs->flag & RS_ARATIO_REACH_LOWER_LIMIT)
            continue;

        if (var_vs->vsgname && tmp_vs->vsgname) {
            ret = vs_group_addr_cmp(var_vs, tmp_vs);
        } else if (var_vs->vsgname && NULL == tmp_vs->vsgname) {
            vsg = ipvs_get_group_by_name(var_vs->vsgname, check_data->vs_group);
            ret = __vs_group_addr_cmp(&tmp_vs->addr, vsg);
        } else if (NULL == var_vs->vsgname && tmp_vs->vsgname) {
            vsg = ipvs_get_group_by_name(tmp_vs->vsgname, check_data->vs_group);
            ret = __vs_group_addr_cmp(&var_vs->addr, vsg);
        } else {
            ret = vs_addr_cmp(&var_vs->addr, &tmp_vs->addr);
        }

        if (ret) {
            log_message(LOG_INFO, "VS [%s] does not reach lower limit, rs_alive_ratio_upper_limit (%d%%)",
            (tmp_vs->vsgname) ? tmp_vs->vsgname : FMT_VS(tmp_vs)
            , tmp_vs->rs_aratio_upper_limit);
            return 0;
        }
	}
	return 1;
}

static void vs_rs_aratio_state(bool alive, virtual_server_t * vs)
{
	int rs_alive_ratio = 0;

    if (alive) {
		vs->rs_alive_count++;
		rs_alive_ratio = vs->rs_alive_count*100/LIST_SIZE(vs->rs);
		if (rs_alive_ratio >= vs->rs_aratio_upper_limit && vs->flag & RS_ARATIO_REACH_LOWER_LIMIT) {
				log_message(LOG_INFO, "VS [%s] rs_alive_ratio (%d%%) >= rs_alive_ratio_upper_limit (%d%%)",
				(vs->vsgname) ? vs->vsgname : FMT_VS(vs)
				, rs_alive_ratio
				, vs->rs_aratio_upper_limit);
		if (NULL == vs->rs_upper_limit_thread) {
				vs->rs_upper_limit_thread = thread_add_timer(master, rs_aratio_reach_upper_limit, vs, TIMER_HZ);
		} else {
				/* be here ONLY if rs_alive_ratio_up < 100%*/
				log_message(LOG_INFO, "Timer already added, ignore..");
		    }
		}
    } else {
		vs->rs_alive_count--;
		rs_alive_ratio = vs->rs_alive_count*100/LIST_SIZE(vs->rs);
		if (rs_alive_ratio <= vs->rs_aratio_lower_limit) {
			log_message(LOG_INFO, "VS [%s] rs_alive_ratio (%d%%) <= rs_aratio_lower_limit (%d%%)",
					(vs->vsgname) ? vs->vsgname : FMT_VS(vs)
					, rs_alive_ratio
					, vs->rs_aratio_lower_limit);

			vs->flag |= RS_ARATIO_REACH_LOWER_LIMIT;
			if (all_vs_rs_aratio_reach_lower_limit(vs)) {
				if (vs->rs_aratio_action) {
					rs_aratio_action(0, vs);
				}
			}
		}

		if (rs_alive_ratio < vs->rs_aratio_upper_limit) {
			/* rs down again, so remove old timer */
			if (NULL != vs->rs_upper_limit_thread) {
				log_message(LOG_INFO, "VS [%s] rs_alive_ratio (%d%%) < rs_aratio_upper_limit (%d%%)",
						(vs->vsgname) ? vs->vsgname : FMT_VS(vs)
						, rs_alive_ratio
						, vs->rs_aratio_upper_limit);
				thread_cancel(vs->rs_upper_limit_thread);
				vs->rs_upper_limit_thread = NULL;
			}
		}
    }
}

/* manipulate add/remove rs according to alive state */
static bool
perform_svr_state(bool alive, checker_t *checker)
{
	/*
	 * | ISALIVE(rs) | alive | context
	 * | false       | false | first check failed under alpha mode, unreachable here
	 * | false       | true  | RS went up, add it to the pool
	 * | true        | false | RS went down, remove it from the pool
	 * | true        | true  | first check succeeded w/o alpha mode, unreachable here
	 */

	virtual_server_t * vs = checker->vs;
	real_server_t * rs = checker->rs;
	int rs_alive_ratio = 0;

	if (ISALIVE(rs) == alive)
		return true;

	log_message(LOG_INFO, "%sing service %s to VS %s"
			    , alive ? (rs->inhibit) ? "Enabl" : "Add" :
				      (rs->inhibit) ? "Disabl" : "Remov"
			    , FMT_RS(rs, vs)
			    , FMT_VS(vs));

	/* Change only if we have quorum or no sorry server */
	if (vs->quorum_state_up || !vs->s_svr || !ISALIVE(vs->s_svr)) {
		if (ipvs_cmd(alive ? LVS_CMD_ADD_DEST : LVS_CMD_DEL_DEST, vs, rs))
			return false;
	}
	rs->alive = alive;
	do_rs_notifies(vs, rs, false);
	vs_rs_aratio_state(alive, vs);

	/* We may have changed quorum state. If the quorum wasn't up
	 * but is now up, this is where the rs is added. */
	update_quorum_state(vs, false);

	return true;
}

/* Set a virtualserver IPVS rules */
static bool
init_service_vs(virtual_server_t * vs)
{
	/* Init the VS root */
	if (!ISALIVE(vs) || vs->vsg) {
		ipvs_cmd(LVS_CMD_ADD, vs, NULL);
		SET_ALIVE(vs);
	}

	/*Set local ip address in "FNAT" mode of IPVS */
	if ((vs->forwarding_method == IP_VS_CONN_F_FULLNAT) && vs->local_addr_gname) { 
		if (!ipvs_cmd(LVS_CMD_ADD_LADDR, vs, NULL))
			return 0; 
	}

        if ((vs->forwarding_method == IP_VS_CONN_F_FULLNAT) && vs->blklst_addr_gname) {
                if (!ipvs_cmd(LVS_CMD_ADD_BLKLST, vs, NULL))
                        return 0;
        }	    

	/* Processing real server queue */
	if (!init_service_rs(vs))
		return false;

	if (vs->reloaded && vs->vsgname) {
		/* add reloaded dests into new vsg entries */
		sync_service_vsg(vs);
	}

	/* we may have got/lost quorum due to quorum setting changed */
	/* also update, in case we need the sorry server in alpha mode */
	update_quorum_state(vs, true);

	/* If we have a sorry server with inhibit, add it now */
	if (vs->s_svr && vs->s_svr->inhibit && !vs->s_svr->set) {
		/* Make sure the sorry server is configured with weight 0 */
		vs->s_svr->num_failed_checkers = 1;

		ipvs_cmd(LVS_CMD_ADD_DEST, vs, vs->s_svr);

		vs->s_svr->num_failed_checkers = 0;
	}

	return true;
}

/* Set IPVS rules */
bool
init_services(void)
{
	element e;
	virtual_server_t *vs;

	LIST_FOREACH(check_data->vs, vs, e) {
		if (!init_service_vs(vs))
			return false;
	}

	return true;
}

/* Store new weight in real_server struct and then update kernel. */
void
update_svr_wgt(int weight, virtual_server_t * vs, real_server_t * rs
		, bool update_quorum)
{
	if (weight != rs->weight) {
		log_message(LOG_INFO, "Changing weight from %d to %d for %sactive service %s of VS %s"
				    , rs->weight
				    , weight
				    , ISALIVE(rs) ? "" : "in"
				    , FMT_RS(rs, vs)
				    , FMT_VS(vs));
		rs->weight = weight;
		/*
		 * Have weight change take effect now only if rs is in
		 * the pool and alive and the quorum is met (or if
		 * there is no sorry server). If not, it will take
		 * effect later when it becomes alive.
		 */
		if (rs->set && ISALIVE(rs) &&
		    (vs->quorum_state_up || !vs->s_svr || !ISALIVE(vs->s_svr)))
			ipvs_cmd(LVS_CMD_EDIT_DEST, vs, rs);
		if (update_quorum)
			update_quorum_state(vs, false);
	}
}

void
set_checker_state(checker_t *checker, bool up)
{
	if (checker->is_up == up)
		return;

	checker->is_up = up;

	if (!up)
		checker->rs->num_failed_checkers++;
	else if (checker->rs->num_failed_checkers)
		checker->rs->num_failed_checkers--;
}

/* Update checker's state */
void
update_svr_checker_state(bool alive, checker_t *checker)
{
	if (checker->is_up == alive) {
		if (!checker->has_run) {
			if (checker->alpha || !alive)
				do_rs_notifies(checker->vs, checker->rs, false);
			checker->has_run = true;
		}
		return;
	}

	checker->has_run = true;

	if (alive) {
		/* call the UP handler unless any more failed checks found */
		if (checker->rs->num_failed_checkers <= 1) {
			if (!perform_svr_state(true, checker))
				return;
		}
	}
	else {
		/* Handle not alive state */
		if (checker->rs->num_failed_checkers == 0) {
			if (!perform_svr_state(false, checker))
				return;
		}
	}

	set_checker_state(checker, alive);
}

/* Check if a vsg entry is in new data */
static virtual_server_group_entry_t * __attribute__ ((pure))
vsge_exist(virtual_server_group_entry_t *vsg_entry, list l)
{
	element e;
	virtual_server_group_entry_t *vsge;

	LIST_FOREACH(l, vsge, e) {
		if (vsge_iseq(vsg_entry, vsge))
			return vsge;
	}

	return NULL;
}

/* Clear the diff vsge of old group */
static void
clear_diff_vsge(list old, list new, virtual_server_t * old_vs)
{
	virtual_server_group_entry_t *vsge, *new_vsge;
	element e;

	LIST_FOREACH(old, vsge, e) {
		new_vsge = vsge_exist(vsge, new);
		if (new_vsge)
			new_vsge->reloaded = true;
		else {
			if (vsge->is_fwmark)
				log_message(LOG_INFO, "VS [%u] in group %s no longer exists",
						      vsge->vfwmark, old_vs->vsgname);
			else
				log_message(LOG_INFO, "VS [%s:%" PRIu32 "] in group %s no longer exists"
						    , inet_sockaddrtotrio(&vsge->addr, old_vs->service_type)
						    , vsge->range
						    , old_vs->vsgname);

			ipvs_group_remove_entry(old_vs, vsge);
		}
	}
}

static void
update_alive_counts(virtual_server_t *old, virtual_server_t *new)
{
	virtual_server_group_entry_t *vsge, *new_vsge;
	list *old_l, *new_l;
	element e;

	if (!old->vsg || !new->vsg)
	 	return ;

	list old_ll[] = {
		old->vsg->addr_range,
		old->vsg->vfwmark,
		NULL,
	};
	list new_ll[] = {
		new->vsg->addr_range,
		new->vsg->vfwmark,
		NULL,
	};

	for (old_l = old_ll, new_l = new_ll; *old_l; old_l++, new_l++) {
		LIST_FOREACH(*old_l, vsge, e) {
			new_vsge = vsge_exist(vsge, *new_l);
			if (new_vsge) {
				if (vsge->is_fwmark) {
					new_vsge->fwm4_alive = vsge->fwm4_alive;
					new_vsge->fwm6_alive = vsge->fwm6_alive;
				} else {
					new_vsge->tcp_alive = vsge->tcp_alive;
					new_vsge->udp_alive = vsge->udp_alive;
					new_vsge->sctp_alive = vsge->sctp_alive;
				}
			}
		}
	}
}

/* Clear the diff vsg of the old vs */
static void
clear_diff_vsg(virtual_server_t * old_vs, virtual_server_t * new_vs)
{
	virtual_server_group_t *old = old_vs->vsg;
	virtual_server_group_t *new = new_vs->vsg;

	/* Diff the group entries */
	clear_diff_vsge(old->addr_range, new->addr_range, old_vs);
	clear_diff_vsge(old->vfwmark, new->vfwmark, old_vs);
}

/* Check if a vs exist in new data and returns pointer to it */
static virtual_server_t* __attribute__ ((pure))
vs_exist(virtual_server_t * old_vs)
{
	element e;
	virtual_server_t *vs;

	LIST_FOREACH(check_data->vs, vs, e) {
		if (vs_iseq(old_vs, vs))
			return vs;
	}

	return NULL;
}

/* Check if rs is in new vs data */
static real_server_t * __attribute__ ((pure))
rs_exist(real_server_t * old_rs, list l)
{
	element e;
	real_server_t *rs;

	if (LIST_ISEMPTY(l))
		return NULL;

	LIST_FOREACH(l, rs, e) {
		if (rs_iseq(rs, old_rs))
			return rs;
	}

	return NULL;
}

static void
migrate_checkers(virtual_server_t *vs, real_server_t *old_rs, real_server_t *new_rs, list old_checkers_queue)
{
	list l;
	element e, e1;
	checker_t *old_c, *new_c;
	checker_t dummy_checker;
	bool a_checker_has_run = false;

	l = alloc_list(NULL, NULL);
	LIST_FOREACH(old_checkers_queue, old_c, e) {
		if (old_c->rs == old_rs)
			list_add(l, old_c);
	}

	if (!LIST_ISEMPTY(l)) {
		LIST_FOREACH(checkers_queue, new_c, e) {
			if (new_c->rs != new_rs || !new_c->compare)
				continue;
			LIST_FOREACH(l, old_c, e1) {
				if (old_c->compare == new_c->compare && new_c->compare(old_c, new_c)) {
					/* Update status if different */
					if (old_c->has_run && old_c->is_up != new_c->is_up)
						set_checker_state(new_c, old_c->is_up);

					/* Transfer some other state flags */
					new_c->has_run = old_c->has_run;
// retry_it needs fixing -  if retry changes, we may already have exceeded count
					new_c->retry_it = old_c->retry_it;

					break;
				}
			}
		}
	}

	/* Find out how many checkers are really failed */
	new_rs->num_failed_checkers = 0;
	LIST_FOREACH(checkers_queue, new_c, e) {
		if (new_c->rs != new_rs)
			continue;
		if (new_c->has_run && !new_c->is_up)
			new_rs->num_failed_checkers++;
		if (new_c->has_run)
			a_checker_has_run = true;
	}

	/* If a checker has failed, set new alpha checkers to be down until
	 * they have run. */
	if (new_rs->num_failed_checkers || (!new_rs->alive && !a_checker_has_run)) {
		LIST_FOREACH(checkers_queue, new_c, e) {
			if (new_c->rs != new_rs)
				continue;
			if (!new_c->has_run) {
				if (new_c->alpha)
					set_checker_state(new_c, false);
				/* One failure is enough */
				new_c->retry_it = new_c->retry;
			}
		}
	}

	/* If there are no failed checkers, the RS needs to be up */
	if (!new_rs->num_failed_checkers && !new_rs->alive) {
		dummy_checker.vs = vs;
		dummy_checker.rs = new_rs;
		perform_svr_state(true, &dummy_checker);
	} else if (new_rs->num_failed_checkers && new_rs->set != new_rs->inhibit)
		ipvs_cmd(new_rs->inhibit ? IP_VS_SO_SET_ADDDEST : IP_VS_SO_SET_DELDEST, vs, new_rs);

	free_list(&l);
}

/* Clear the diff rs of the old vs */
static void
clear_diff_rs(virtual_server_t *old_vs, virtual_server_t *new_vs, list old_checkers_queue)
{
	element e;
	real_server_t *rs, *new_rs;
	list rs_to_remove;

	/* If old vs didn't own rs then nothing return */
	if (LIST_ISEMPTY(old_vs->rs))
		return;

	/* remove RS from old vs which are not found in new vs */
	rs_to_remove = alloc_list (NULL, NULL);
	LIST_FOREACH(old_vs->rs, rs, e) {
		new_rs = rs_exist(rs, new_vs->rs);
		if (!new_rs) {
			log_message(LOG_INFO, "service %s no longer exist"
					    , FMT_RS(rs, old_vs));

			list_add (rs_to_remove, rs);
		} else {
			/*
			 * We reflect the previous alive
			 * flag value to not try to set
			 * already set IPVS rule.
			 */
			new_rs->alive = rs->alive;
			new_rs->set = rs->set;
			new_rs->weight = rs->weight;
			new_rs->pweight = rs->iweight;
			new_rs->reloaded = true;

			/*
			 * We must migrate the state of the old checkers.
			 * If we do not, the new RS is in a state where it’s reported
			 * as down with no check failed. As a result, the server will never
			 * be put back up when it’s alive again in check_tcp.c#83 because
			 * of the check that put a rs up only if it was not previously up.
			 * For alpha mode checkers, if it was up, we don't need another
			 * success to say it is now up.
			 */
			migrate_checkers(new_vs, rs, new_rs, old_checkers_queue);

			/* Do we need to update the RS configuration? */
			if (false ||
#ifdef _HAVE_IPVS_TUN_TYPE_
			    rs->tun_type != new_rs->tun_type ||
			    rs->tun_port != new_rs->tun_port ||
#ifdef _HAVE_IPVS_TUN_CSUM_
			    rs->tun_flags != new_rs->tun_flags ||
#endif
#endif
			    rs->forwarding_method != new_rs->forwarding_method)
				ipvs_cmd(LVS_CMD_EDIT_DEST, new_vs, new_rs);
		}
	}
	clear_service_rs(old_vs, rs_to_remove, false);
	free_list(&rs_to_remove);
}

/* clear sorry server, but only if changed */
static void
clear_diff_s_srv(virtual_server_t *old_vs, real_server_t *new_rs)
{
	real_server_t *old_rs = old_vs->s_svr;

	if (!old_rs)
		return;

	if (new_rs && rs_iseq(old_rs, new_rs)) {
		/* which fields are really used on s_svr? */
		new_rs->alive = old_rs->alive;
		new_rs->set = old_rs->set;
		new_rs->weight = old_rs->weight;
		new_rs->pweight = old_rs->iweight;
		new_rs->reloaded = true;
	}
	else {
		if (old_rs->inhibit) {
			if (!ISALIVE(old_rs) && old_rs->set)
				SET_ALIVE(old_rs);
			old_rs->inhibit = 0;
		}
		if (ISALIVE(old_rs)) {
			log_message(LOG_INFO, "Removing sorry server %s from VS %s"
					    , FMT_RS(old_rs, old_vs)
					    , FMT_VS(old_vs));
			ipvs_cmd(LVS_CMD_DEL_DEST, old_vs, old_rs);
		}
	}

}

/* When reloading configuration, remove negative diff entries
 * and copy status of existing entries to the new ones */
/* Check if a local address entry is in list */
static int __attribute((pure))
laddr_entry_exist(local_addr_entry *laddr_entry, list l)
{
	element e;
	local_addr_entry *entry;

	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		entry = ELEMENT_DATA(e);
		if (sockstorage_equal(&entry->addr, &laddr_entry->addr) && 
				(entry->range == laddr_entry->range) &&
                         !strcmp(entry->ifname, laddr_entry->ifname))
			return 1;
	}
	return 0;
}

/* Clear the diff local address entry of eth old vs */
static int
clear_diff_laddr_entry(list old, list new, virtual_server_t * old_vs)
{
	element e;
	local_addr_entry *laddr_entry;

	for (e = LIST_HEAD(old); e; ELEMENT_NEXT(e)) {
		laddr_entry = ELEMENT_DATA(e);
		if (!laddr_entry_exist(laddr_entry, new)) {
			log_message(LOG_INFO, "VS [%s-%d] in local address group %s no longer exist\n" 
					    , inet_sockaddrtos(&laddr_entry->addr)
					    , laddr_entry->range
					    , old_vs->local_addr_gname);

			if (!ipvs_laddr_remove_entry(old_vs, laddr_entry))
				return 0;
		}
	}

	return 1;
}

/* Clear the diff local address of the old vs */
static int
clear_diff_laddr(virtual_server_t * old_vs)
{
	local_addr_group *old;
	local_addr_group *new;

	/*
 	 *  If old vs was not in fulllnat mod or didn't own local address group, 
 	 * then do nothing and return 
 	 */
	if ((old_vs->forwarding_method != IP_VS_CONN_F_FULLNAT) || 
						!old_vs->local_addr_gname)
		return 1;

	/* Fetch local address group */
	old = ipvs_get_laddr_group_by_name(old_vs->local_addr_gname, 
							old_check_data->laddr_group);
	new = ipvs_get_laddr_group_by_name(old_vs->local_addr_gname, 
							check_data->laddr_group);

	if (!clear_diff_laddr_entry(old->addr_ip, new->addr_ip, old_vs))
		return 0;
	if (!clear_diff_laddr_entry(old->range, new->range, old_vs))
		return 0;

	return 1;
}

/* Check if a blacklist address entry is in list */
static int __attribute((pure))
blklst_entry_exist(blklst_addr_entry *blklst_entry, list l)
{
        element e;
        blklst_addr_entry *entry;

        for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
                entry = ELEMENT_DATA(e);
                if (sockstorage_equal(&entry->addr, &blklst_entry->addr) &&
                                        entry->range == blklst_entry->range)
                        return 1;
        }
        return 0;
}

/* Clear the diff blklst address entry of the old vs */
static int
clear_diff_blklst_entry(list old, list new, virtual_server_t * old_vs)
{
        element e;
        blklst_addr_entry *blklst_entry;

        for (e = LIST_HEAD(old); e; ELEMENT_NEXT(e)) {
                blklst_entry = ELEMENT_DATA(e);
                if (!blklst_entry_exist(blklst_entry, new)) {
                        log_message(LOG_INFO, "VS [%s-%d] in blacklist address group %s no longer exist\n"
                                            , inet_sockaddrtos(&blklst_entry->addr)
                                            , blklst_entry->range
                                            , old_vs->blklst_addr_gname);

                        if (!ipvs_blklst_remove_entry(old_vs, blklst_entry))
                                return 0;
                }
        }

        return 1;
}

/* Clear the diff blacklist address of the old vs */
static int
clear_diff_blklst(virtual_server_t * old_vs)
{
        blklst_addr_group *old;
        blklst_addr_group *new;

        /*
         *  If old vs  didn't own blacklist address group, 
         * then do nothing and return 
         */
        if (!old_vs->blklst_addr_gname)
                return 1;

        /* Fetch blacklist address group */
        old = ipvs_get_blklst_group_by_name(old_vs->blklst_addr_gname,
                                                        old_check_data->blklst_group);
        new = ipvs_get_blklst_group_by_name(old_vs->blklst_addr_gname,
                                                        check_data->blklst_group);

        if (!clear_diff_blklst_entry(old->addr_ip, new->addr_ip, old_vs))
                return 0;
        if (!clear_diff_blklst_entry(old->range, new->range, old_vs))
                return 0;

        return 1;
}

/* When reloading configuration, remove negative diff entries */
void
clear_diff_services(list old_checkers_queue)
{
	element e;
	virtual_server_t *vs, *new_vs;

	/* Remove diff entries from previous IPVS rules */
	LIST_FOREACH(old_check_data->vs, vs, e) {
		/*
		 * Try to find this vs into the new conf data
		 * reloaded.
		 */
		new_vs = vs_exist(vs);
		if (!new_vs) {
			if (vs->vsgname) {
				log_message(LOG_INFO, "Removing Virtual Server Group [%s]"
						    , vs->vsgname);
			}
			else
				log_message(LOG_INFO, "Removing Virtual Server %s", FMT_VS(vs));

			/* Clear VS entry */
			clear_service_vs(vs, false);
		} else {
			/* copy status fields from old VS */
			new_vs->alive = vs->alive;
			new_vs->quorum_state_up = vs->quorum_state_up;
			new_vs->reloaded = true;
			if (using_ha_suspend)
				new_vs->ha_suspend_addr_count = vs->ha_suspend_addr_count;

			if (vs->vsgname)
				clear_diff_vsg(vs, new_vs);

			/* If vs exist, perform rs pool diff */
			/* omega = false must not prevent the notifiers from being called,
			   because the VS still exists in new configuration */
			if (strcmp(vs->sched, new_vs->sched) ||
			    vs->flags != new_vs->flags ||
			    vs->persistence_granularity != new_vs->persistence_granularity ||
			    vs->persistence_timeout != new_vs->persistence_timeout) {
				ipvs_cmd(IP_VS_SO_SET_EDIT, new_vs, NULL);
			}

			vs->omega = true;
			clear_diff_rs(vs, new_vs, old_checkers_queue);
			clear_diff_s_srv(vs, new_vs->s_svr);

			update_alive_counts(vs, new_vs);
			/* perform local address diff */
			if (!clear_diff_laddr(vs))
				return;
                        /* perform blacklist address diff */
                        if (!clear_diff_blklst(vs))
                                return;
		}
	}
}

/* This is only called during a reload. Any new real server with
 * alpha mode checkers should start in down state */
void
check_new_rs_state(void)
{
	element e;
	checker_t *checker;

	LIST_FOREACH(checkers_queue, checker, e) {
		if (checker->rs->reloaded)
			continue;
		if (!checker->alpha)
			continue;
		set_checker_state(checker, false);
		UNSET_ALIVE(checker->rs);
	}
}

void
link_vsg_to_vs(void)
{
	element e, e1, next;
	virtual_server_t *vs;
	int vsg_af;
	virtual_server_group_t *vsg;
	virtual_server_group_entry_t *vsge;
	unsigned vsg_member_no;

	if (LIST_ISEMPTY(check_data->vs))
		return;

	LIST_FOREACH_NEXT(check_data->vs, vs, e, next) {
		if (vs->vsgname) {
			vs->vsg = ipvs_get_group_by_name(vs->vsgname, check_data->vs_group);
			if (!vs->vsg) {
				log_message(LOG_INFO, "Virtual server group %s specified but not configured - ignoring virtual server %s", vs->vsgname, FMT_VS(vs));
				free_vs_checkers(vs);
				free_list_element(check_data->vs, e);
				continue;
			}

			/* Check the vsg has some configuration */
			if (LIST_ISEMPTY(vs->vsg->addr_range) &&
			    LIST_ISEMPTY(vs->vsg->vfwmark)) {
				log_message(LOG_INFO, "Virtual server group %s has no configuration - ignoring virtual server %s", vs->vsgname, FMT_VS(vs));
				free_vs_checkers(vs);
				free_list_element(check_data->vs, e);
				continue;
			}

			/* Check the vs and vsg address families match */
			if (!LIST_ISEMPTY(vs->vsg->addr_range)) {
				vsge = ELEMENT_DATA(LIST_HEAD(vs->vsg->addr_range));
				vsg_af = vsge->addr.ss_family;
			}
			else {
				/* fwmark only */
				vsg_af = AF_UNSPEC;
			}

			if (vsg_af != AF_UNSPEC && vsg_af != vs->af) {
				log_message(LOG_INFO, "Virtual server group %s address family doesn't match virtual server %s - ignoring", vs->vsgname, FMT_VS(vs));
				free_vs_checkers(vs);
				free_list_element(check_data->vs, e);
			}
		}
	}

	/* The virtual server port number is used to identify the sequence number of the virtual server in the group */
	LIST_FOREACH(check_data->vs_group, vsg, e) {
		vsg_member_no = 0;

		LIST_FOREACH(check_data->vs, vs, e1) {
			if (!vs->vsgname)
				continue;

			if (!strcmp(vs->vsgname, vsg->gname)) {
				/* We use the IPv4 port since there is no address family */
				((struct sockaddr_in *)&vs->addr)->sin_port = htons(vsg_member_no);
				vsg_member_no++;
			}
		}
	}
}

static tunnel_group * __attribute((pure))
get_tunnel_group_by_name(char *gname, list l)
{
	element e;
	tunnel_group* group;

	if (LIST_ISEMPTY(l))
		return NULL;

	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		group = ELEMENT_DATA(e);
		if (!strcmp(group->gname, gname))
			return group;
	}

	return NULL;
}

static int __attribute((pure))
tunnel_entry_exist(tunnel_entry* old_entry, tunnel_group* new_group)
{
	element e;
	tunnel_entry* new_entry;
	list l = new_group->tunnel_entry;

	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		new_entry = ELEMENT_DATA(e);
		if (!strcmp(old_entry->ifname, new_entry->ifname) &&
			!strcmp(old_entry->link, new_entry->link) &&
			!strcmp(old_entry->kind, new_entry->kind) &&
			sockstorage_equal(&old_entry->local, &new_entry->local) &&
			sockstorage_equal(&old_entry->remote, &new_entry->remote)) {
			return 1;
		}
	}

	return 0;
}

static int
clear_tunnel_entry(tunnel_entry *entry)
{
	return ipvs_tunnel_cmd(LVS_CMD_DEL_TUNNEL, entry);
}

static int
clear_tunnel_group(tunnel_group* group)
{
	element e;
	tunnel_entry *entry;
	list l = group->tunnel_entry;

	if (LIST_ISEMPTY(l))
		return IPVS_SUCCESS;

	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		entry = ELEMENT_DATA(e);
		if (!clear_tunnel_entry(entry)) {
			log_message(LOG_ERR, "%s clear tunnel %s error.", __FUNCTION__, entry->ifname);
			return IPVS_ERROR;
		}
	}

	return IPVS_SUCCESS;
}

static int
clear_diff_tunnel_group(tunnel_group* old_group, tunnel_group* new_group)
{
	element e;
	tunnel_entry *entry;
	list l = old_group->tunnel_entry;

	if (LIST_ISEMPTY(old_group->tunnel_entry))
		return IPVS_SUCCESS;

	if (LIST_ISEMPTY(new_group->tunnel_entry))
		return clear_tunnel_group(old_group);

	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		entry = ELEMENT_DATA(e);
		if (!tunnel_entry_exist(entry, new_group))
			clear_tunnel_entry(entry);
	}

	return IPVS_SUCCESS;
}

int clear_diff_tunnel(void)
{
	element e;
	tunnel_group *group;
	tunnel_group* new_group;
	list l = old_check_data->tunnel_group;

	/* If old config didn't own tunnel then nothing return */
	if (LIST_ISEMPTY(l))
		return IPVS_SUCCESS;

	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		group = ELEMENT_DATA(e);
		new_group = get_tunnel_group_by_name(group->gname, check_data->tunnel_group);
		if (new_group) {
			clear_diff_tunnel_group(group, new_group);
		} else {
			clear_tunnel_group(group);
		}
	}

	return IPVS_SUCCESS;
}

