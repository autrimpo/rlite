/*
 * Unix server for uipcps daemon.
 *
 * Copyright (C) 2015-2016 Nextworks
 * Author: Vincenzo Maffione <v.maffione@gmail.com>
 *
 * This file is part of rlite.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <signal.h>
#include <assert.h>
#include <endian.h>
#include <sys/stat.h>
#include <pthread.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <sys/file.h>

#include "rlite/kernel-msg.h"
#include "rlite/uipcps-msg.h"
#include "rlite/utils.h"
#include "rlite/list.h"
#include "rlite/conf.h"
#include "rlite/uipcps-helpers.h"
#include "rlite/version.h"
#include "uipcp-container.h"

static void
unlink_and_exit(int exit_code)
{
    unlink(RLITE_UIPCPS_PIDFILE);
    exit(exit_code);
}

struct registered_ipcp {
    rl_ipcp_id_t id;
    char *name;
    char *dif_name;

    struct list_head node;
};

/* Global variable containing the main struct of the uipcps. This variable
 * should be accessed directly only by signal handlers (because I don't know
 * how to do it differently). The rest of the program should access it
 * through pointers.
 */
static struct uipcps guipcps;

static int
rl_u_response(int sfd, const struct rl_msg_base *req,
              struct rl_msg_base_resp *resp)
{
    resp->hdr.msg_type = RLITE_U_BASE_RESP;
    resp->hdr.event_id = req->hdr.event_id;

    return rl_msg_write_fd(sfd, RLITE_MB(resp));
}

static int
rl_u_ipcp_register(struct uipcps *uipcps, int sfd,
                   const struct rl_msg_base *b_req)
{
    struct rl_cmsg_ipcp_register *req = (struct rl_cmsg_ipcp_register *)b_req;
    struct rl_msg_base_resp resp;
    struct uipcp *uipcp;

    resp.result = RLITE_ERR;
    /* Grab the corresponding userspace IPCP. */
    uipcp = uipcp_get_by_name(uipcps, req->ipcp_name);
    if (!uipcp) {
        return -1;
    }

    if (uipcp->ops.register_to_lower) {
        resp.result = uipcp->ops.register_to_lower(uipcp, req);
    }
    uipcp_put(uipcp);

    return rl_u_response(sfd, RLITE_MB(req), &resp);
}

static int
rl_u_ipcp_enroll(struct uipcps *uipcps, int sfd,
                 const struct rl_msg_base *b_req)
{
    struct rl_cmsg_ipcp_enroll *req = (struct rl_cmsg_ipcp_enroll *)b_req;
    struct rl_msg_base_resp resp;
    struct uipcp *uipcp;

    resp.result = RLITE_ERR;
    /* Find the userspace part of the enrolling IPCP. */
    uipcp = uipcp_get_by_name(uipcps, req->ipcp_name);
    if (uipcp && uipcp->ops.enroll) {
        resp.result = uipcp->ops.enroll(uipcp, req, 1);
    }

    uipcp_put(uipcp);

    return rl_u_response(sfd, RLITE_MB(req), &resp);
}

static int
rl_u_ipcp_enroller_enable(struct uipcps *uipcps, int sfd,
                          const struct rl_msg_base *b_req)
{
    struct rl_cmsg_ipcp_enroller_enable *req =
        (struct rl_cmsg_ipcp_enroller_enable *)b_req;
    struct rl_msg_base_resp resp;
    struct uipcp *uipcp;

    resp.result = RLITE_ERR;
    uipcp       = uipcp_get_by_name(uipcps, req->ipcp_name);
    if (uipcp && uipcp->ops.enroller_enable) {
        resp.result = uipcp->ops.enroller_enable(uipcp, req);
    }

    uipcp_put(uipcp);

    return rl_u_response(sfd, RLITE_MB(req), &resp);
}

static int
rl_u_ipcp_lower_flow_alloc(struct uipcps *uipcps, int sfd,
                           const struct rl_msg_base *b_req)
{
    struct rl_cmsg_ipcp_enroll *req = (struct rl_cmsg_ipcp_enroll *)b_req;
    struct rl_msg_base_resp resp;
    struct uipcp *uipcp;

    resp.result = RLITE_ERR;

    /* Find the userspace part of the requestor IPCP. */
    uipcp = uipcp_get_by_name(uipcps, req->ipcp_name);
    if (uipcp && uipcp->ops.lower_flow_alloc) {
        resp.result = uipcp->ops.lower_flow_alloc(uipcp, req, 1);
    }

    uipcp_put(uipcp);

    return rl_u_response(sfd, RLITE_MB(req), &resp);
}

static int
rl_u_ipcp_rib_show(struct uipcps *uipcps, int sfd,
                   const struct rl_msg_base *b_req)
{
    struct rl_cmsg_ipcp_rib_show_req *req =
        (struct rl_cmsg_ipcp_rib_show_req *)b_req;
    struct rl_cmsg_ipcp_rib_show_resp resp;
    char *(*show)(struct uipcp *) = NULL;
    struct uipcp *uipcp;
    char *dumpstr = NULL;
    int ret;

    resp.result   = RLITE_ERR; /* Report failure by default. */
    resp.dump.buf = NULL;
    resp.dump.len = 0;

    uipcp = uipcp_get_by_name(uipcps, req->ipcp_name);
    if (!uipcp) {
        goto out;
    }

    switch (req->hdr.msg_type) {
    case RLITE_U_IPCP_RIB_SHOW_REQ:
        show = uipcp->ops.rib_show;
        break;

    case RLITE_U_IPCP_ROUTING_SHOW_REQ:
        show = uipcp->ops.routing_show;
        break;

    case RLITE_U_IPCP_RIB_PATHS_SHOW_REQ:
        show = uipcp->ops.rib_paths_show;
        break;

    case RLITE_U_IPCP_STATS_SHOW_REQ:
        show = uipcp->ops.stats_show;
        break;
    }

    if (show) {
        dumpstr = show(uipcp);
        if (dumpstr) {
            resp.result   = RLITE_SUCC;
            resp.dump.buf = dumpstr;
            resp.dump.len = strlen(dumpstr) + 1; /* include terminator */
        }
    }

    uipcp_put(uipcp);

out:
    resp.hdr.msg_type = req->hdr.msg_type + 1;
    resp.hdr.event_id = req->hdr.event_id;

    ret = rl_msg_write_fd(sfd, RLITE_MB(&resp));

    if (dumpstr) {
        rl_free(dumpstr, RL_MT_UTILS);
    }

    return ret;
}

static int
rl_u_ipcp_policy_mod(struct uipcps *uipcps, int sfd,
                     const struct rl_msg_base *b_req)
{
    struct rl_cmsg_ipcp_policy_mod *req =
        (struct rl_cmsg_ipcp_policy_mod *)b_req;
    struct rl_msg_base_resp resp;
    struct uipcp *uipcp;

    resp.result = RLITE_ERR;
    /* Grab the corresponding userspace IPCP. */
    uipcp = uipcp_get_by_name(uipcps, req->ipcp_name);
    if (!uipcp) {
        return -1;
    }

    if (uipcp->ops.policy_mod) {
        resp.result = uipcp->ops.policy_mod(uipcp, req);
    }

    uipcp_put(uipcp);

    return rl_u_response(sfd, RLITE_MB(req), &resp);
}

static int
rl_u_ipcp_policy_list(struct uipcps *uipcps, int sfd,
                      const struct rl_msg_base *b_req)
{
    struct rl_cmsg_ipcp_policy_list_req *req =
        (struct rl_cmsg_ipcp_policy_list_req *)b_req;
    struct rl_cmsg_ipcp_policy_param_list_req *preq =
        (struct rl_cmsg_ipcp_policy_param_list_req *)b_req;
    struct rl_cmsg_ipcp_policy_list_resp resp;
    struct uipcp *uipcp = NULL;
    char *msg           = NULL;
    int ret             = 0;

    resp.result   = RLITE_ERR; /* Report failure by default. */
    resp.dump.buf = NULL;
    resp.dump.len = 0;

    if (req->ipcp_name) {
        uipcp = uipcp_get_by_name(uipcps, req->ipcp_name);
    }

    if (!uipcp) {
        goto out;
    }

    switch (req->hdr.msg_type) {
    case RLITE_U_IPCP_POLICY_LIST_REQ:
        ret = uipcp->ops.policy_list(uipcp, req, &msg);
        break;

    case RLITE_U_IPCP_POLICY_PARAM_LIST_REQ:
        ret = uipcp->ops.policy_param_list(uipcp, preq, &msg);
        break;
    }

    if (msg) {
        resp.result   = ret ? RLITE_ERR : RLITE_SUCC;
        resp.dump.buf = msg;
        resp.dump.len = strlen(msg) + 1; /* include terminator */
    }

    uipcp_put(uipcp);

out:
    resp.hdr.msg_type = req->hdr.msg_type + 1;
    resp.hdr.event_id = req->hdr.event_id;

    ret = rl_msg_write_fd(sfd, RLITE_MB(&resp));

    if (msg) {
        rl_free(msg, RL_MT_UTILS);
    }

    return ret;
}

static int
rl_u_ipcp_policy_param_mod(struct uipcps *uipcps, int sfd,
                           const struct rl_msg_base *b_req)
{
    struct rl_cmsg_ipcp_policy_param_mod *req =
        (struct rl_cmsg_ipcp_policy_param_mod *)b_req;
    struct rl_msg_base_resp resp;
    struct uipcp *uipcp;

    resp.result = RLITE_ERR;
    /* Grab the corresponding userspace IPCP. */
    uipcp = uipcp_get_by_name(uipcps, req->ipcp_name);
    if (!uipcp) {
        return -1;
    }

    if (uipcp->ops.policy_mod) {
        resp.result = uipcp->ops.policy_param_mod(uipcp, req);
    }

    uipcp_put(uipcp);

    return rl_u_response(sfd, RLITE_MB(req), &resp);
}

static int
rl_u_ipcp_config(struct uipcps *uipcps, int sfd,
                 const struct rl_msg_base *b_req)
{
    struct rl_cmsg_ipcp_config *req = (struct rl_cmsg_ipcp_config *)b_req;
    struct rl_msg_base_resp resp;
    struct uipcp *uipcp;
    int ret = ENOSYS;

    /* Try to grab the corresponding userspace IPCP. If there is one,
     * check if the uipcp can satisfy this request. */
    pthread_mutex_lock(&uipcps->lock);
    uipcp = uipcp_get_by_id(uipcps, req->ipcp_id);
    pthread_mutex_unlock(&uipcps->lock);
    if (uipcp && uipcp->ops.config) {
        ret = uipcp->ops.config(uipcp, req);
    }

    if (ret == ENOSYS) {
        /* Request could not be satisfied by the uipcp (or there is no
         * uipcp). Let's forward it to the kernel. */
        ret = rl_conf_ipcp_config(req->ipcp_id, req->name, req->value);
    }

    uipcp_put(uipcp);

    resp.result = ret ? RLITE_ERR : 0;

    return rl_u_response(sfd, RLITE_MB(req), &resp);
}

static int
rl_u_ipcp_route_mod(struct uipcps *uipcps, int sfd,
                    const struct rl_msg_base *b_req)
{
    struct rl_cmsg_ipcp_route_mod *req = (struct rl_cmsg_ipcp_route_mod *)b_req;
    struct rl_msg_base_resp resp;
    struct uipcp *uipcp;

    resp.result = RLITE_ERR;

    /* Find the userspace part of the requestor IPCP. */
    uipcp = uipcp_get_by_name(uipcps, req->ipcp_name);
    if (uipcp && uipcp->ops.route_mod) {
        resp.result = uipcp->ops.route_mod(uipcp, req);
    }

    uipcp_put(uipcp);

    return rl_u_response(sfd, RLITE_MB(req), &resp);
}

static int
rl_u_probe(struct uipcps *uipcps, int sfd, const struct rl_msg_base *b_req)
{
    struct rl_msg_base_resp resp = {
        .result = 0,
    };

    return rl_u_response(sfd, RLITE_MB(b_req), &resp);
}

/* Equivalent to 'rlite-ctl reset', but supporting both synchronous
 * and asynchronous operation. */
static int
uipcps_reset(int sync)
{
    int ret = 0;
    int fd;

    /* We init an rlite control device, with IPCP updates
     * enabled. */
    fd = rina_open();
    if (fd < 0) {
        perror("rina_open()");
        return -1;
    }
    ret = ioctl(fd, RLITE_IOCTL_CHFLAGS, RL_F_IPCPS);
    if (ret < 0) {
        perror("ioctl()");
        return -1;
    }
    ret = fcntl(fd, F_SETFL, O_NONBLOCK);
    if (ret < 0) {
        perror("fcntl(F_SETFL, O_NONBLOCK)");
        return -1;
    }

    /* We will receive an update ADD message for each existing IPCP. */
    for (;;) {
        struct rl_kmsg_ipcp_update *upd;

        upd = (struct rl_kmsg_ipcp_update *)rl_read_next_msg(fd, 1);
        if (!upd) {
            if (errno && errno != EAGAIN) {
                perror("rl_read_next_msg()");
            }
            break;
        }
        assert(upd->hdr.msg_type == RLITE_KER_IPCP_UPDATE);

        if (upd->update_type == RL_IPCP_UPDATE_ADD) {
            /* Destroy the IPCP. */
            int ret;
            ret = rl_conf_ipcp_destroy(upd->ipcp_id, /*sync=*/sync);
            PD("Destroying IPCP '%s' (id=%u) --> %d\n", upd->ipcp_name,
               upd->ipcp_id, ret);
        }
        rl_msg_free(rl_ker_numtables, RLITE_KER_MSG_MAX, RLITE_MB(upd));
        rl_free(upd, RL_MT_MSG);
    }
    close(fd);
    return 0;
}

static int
rl_u_terminate(struct uipcps *uipcps, int sfd, const struct rl_msg_base *b_req)
{
    struct rl_msg_base_resp resp = {
        .result = 0,
    };

    uipcps_reset(/*sync=*/1);
    uipcps->terminate = 1;

    return rl_u_response(sfd, RLITE_MB(b_req), &resp);
}

static int
rl_u_ipcp_neigh_disconnect(struct uipcps *uipcps, int sfd,
                           const struct rl_msg_base *b_req)
{
    struct rl_cmsg_ipcp_neigh_disconnect *req =
        (struct rl_cmsg_ipcp_neigh_disconnect *)b_req;
    struct rl_msg_base_resp resp;
    struct uipcp *uipcp;

    resp.result = RLITE_ERR;
    uipcp       = uipcp_get_by_name(uipcps, req->ipcp_name);
    if (uipcp && uipcp->ops.neigh_disconnect) {
        resp.result = uipcp->ops.neigh_disconnect(uipcp, req);
    }

    uipcp_put(uipcp);

    return rl_u_response(sfd, RLITE_MB(req), &resp);
}

#ifdef RL_MEMTRACK
static int
rl_u_memtrack_dump(struct uipcps *uipcps, int sfd,
                   const struct rl_msg_base *b_req)
{
    struct rl_msg_base_resp resp;

    rl_memtrack_dump_stats();
    resp.result = 0; /* ok */

    return rl_u_response(sfd, b_req, &resp);
}
#endif /* RL_MEMTRACK */

typedef int (*rl_req_handler_t)(struct uipcps *uipcps, int sfd,
                                const struct rl_msg_base *b_req);

/* The table containing all application request handlers. */
static rl_req_handler_t rl_config_handlers[] = {
    [RLITE_U_IPCP_REGISTER]              = rl_u_ipcp_register,
    [RLITE_U_IPCP_ENROLL]                = rl_u_ipcp_enroll,
    [RLITE_U_IPCP_LOWER_FLOW_ALLOC]      = rl_u_ipcp_lower_flow_alloc,
    [RLITE_U_IPCP_RIB_SHOW_REQ]          = rl_u_ipcp_rib_show,
    [RLITE_U_IPCP_POLICY_MOD]            = rl_u_ipcp_policy_mod,
    [RLITE_U_IPCP_ENROLLER_ENABLE]       = rl_u_ipcp_enroller_enable,
    [RLITE_U_IPCP_ROUTING_SHOW_REQ]      = rl_u_ipcp_rib_show,
    [RLITE_U_IPCP_POLICY_PARAM_MOD]      = rl_u_ipcp_policy_param_mod,
    [RLITE_U_IPCP_CONFIG]                = rl_u_ipcp_config,
    [RLITE_U_PROBE]                      = rl_u_probe,
    [RLITE_U_IPCP_POLICY_LIST_REQ]       = rl_u_ipcp_policy_list,
    [RLITE_U_IPCP_POLICY_PARAM_LIST_REQ] = rl_u_ipcp_policy_list,
    [RLITE_U_TERMINATE]                  = rl_u_terminate,
    [RLITE_U_IPCP_NEIGH_DISCONNECT]      = rl_u_ipcp_neigh_disconnect,
    [RLITE_U_IPCP_RIB_PATHS_SHOW_REQ]    = rl_u_ipcp_rib_show,
    [RLITE_U_IPCP_ROUTE_ADD]             = rl_u_ipcp_route_mod,
    [RLITE_U_IPCP_ROUTE_DEL]             = rl_u_ipcp_route_mod,
    [RLITE_U_IPCP_STATS_SHOW_REQ]        = rl_u_ipcp_rib_show,
#ifdef RL_MEMTRACK
    [RLITE_U_MEMTRACK_DUMP] = rl_u_memtrack_dump,
#endif /* RL_MEMTRACK */
    [RLITE_U_MSG_MAX] = NULL,
};

struct worker_info {
    pthread_t th;
    struct uipcps *uipcps;
    int cfd;
    struct list_head node;
};

static void *
worker_fn(void *opaque)
{
    struct worker_info *wi  = opaque;
    struct rl_msg_base *req = NULL;
    char serbuf[4096];
    char msgbuf[4096];
    int ret;

    PV("Worker %p started\n", wi);

    /* Read the request message in serialized form. */
    ret = read(wi->cfd, serbuf, sizeof(serbuf));
    if (ret < 0) {
        PE("read() error [%d]\n", ret);
        goto out;
    }

    /* Deserialize into a formatted message. */
    ret = deserialize_rlite_msg(rl_uipcps_numtables, RLITE_U_MSG_MAX, serbuf,
                                ret, msgbuf, sizeof(msgbuf));
    if (ret) {
        errno = EPROTO;
        PE("deserialization error [%s]\n", strerror(errno));
        goto out;
    }

    /* Lookup the message type. */
    req = RLITE_MB(msgbuf);
    if (rl_config_handlers[req->hdr.msg_type] == NULL) {
        PE("No handler for message of type [%d]\n", req->hdr.msg_type);
        ret = -1;
        goto out;
    }

    /* Valid message type: handle the request. */
    ret = rl_config_handlers[req->hdr.msg_type](wi->uipcps, wi->cfd, req);
    if (ret) {
        PE("Error while handling message type [%d]\n", req->hdr.msg_type);
    }
out:
    if (ret) {
        struct rl_msg_base_resp resp;

        resp.hdr.msg_type = RLITE_U_BASE_RESP;
        resp.hdr.event_id = req ? req->hdr.event_id : 0;
        resp.result       = RLITE_ERR;
        rl_msg_write_fd(wi->cfd, RLITE_MB(&resp));
    }

    if (req) {
        rl_msg_free(rl_uipcps_numtables, RLITE_U_MSG_MAX, req);
    }

    /* Close the connection. */
    close(wi->cfd);

    PV("Worker %p stopped\n", wi);

    if (wi->uipcps->terminate) {
        PD("terminate command received, daemon is going to exit ...\n");
        unlink_and_exit(EXIT_SUCCESS);
    }

    return NULL;
}

/* Unix server thread to manage configuration requests. */
static int
socket_server(struct uipcps *uipcps)
{
    struct list_head threads;
    int threads_cnt = 0;
#define RL_MAX_THREADS 16

    list_init(&threads);

    for (;;) {
        struct sockaddr_un client_address;
        socklen_t client_address_len = sizeof(client_address);
        struct worker_info *wi, *tmp;
        int ret;

        for (;;) {
            /* Try to clean up previously terminated worker threads. */
            if (!list_empty(&threads)) {
                list_for_each_entry_safe (wi, tmp, &threads, node) {
                    ret = pthread_tryjoin_np(wi->th, NULL);
                    if (ret == EBUSY) {
                        /* Skip this since it has not finished yet. */
                        continue;
                    } else if (ret) {
                        PE("pthread_tryjoin_np() failed: %d\n", ret);
                    }

                    PV("Worker %p cleaned-up\n", wi);
                    list_del(&wi->node);
                    rl_free(wi, RL_MT_MISC);
                    threads_cnt--;
                }
            }

            if (threads_cnt < RL_MAX_THREADS) {
                /* We have not reached the maximum, let's go ahead. */
                break;
            }

            /* Too many threads, let's wait a bit and try again to free up
             * resources. */
            PD("Too many active threads, wait to free up some\n");
            usleep(50000);
        }

        wi         = rl_alloc(sizeof(*wi), RL_MT_MISC);
        wi->uipcps = uipcps;

        /* Accept a new client and create a thread to serve it. */
        wi->cfd = accept(uipcps->lfd, (struct sockaddr *)&client_address,
                         &client_address_len);

        ret = pthread_create(&wi->th, NULL, worker_fn, wi);
        if (ret) {
            PE("pthread_create() failed [%s]\n", strerror(ret));
            if (ret != EAGAIN) {
                /* Unrecoverable error. */
                exit(EXIT_FAILURE);
            }
            /* Temporary failure due to lack of system resources. Wait a bit
             * and retry. */
            close(wi->cfd);
            rl_free(wi, RL_MT_MISC);
            sleep(3);
            continue;
        }

        list_add_tail(&wi->node, &threads);
        threads_cnt++;
    }

    return 0;
#undef RL_MAX_THREADS
}

int
eventfd_signal(int efd, unsigned int value)
{
    uint64_t x = value;
    int n;

    n = write(efd, &x, sizeof(x));
    if (n != sizeof(x)) {
        perror("write(eventfd)");
        if (n < 0) {
            return n;
        }
        return -1;
    }

    return 0;
}

uint64_t
eventfd_drain(int efd)
{
    uint64_t x = (uint64_t)-1;
    int n;

    n = read(efd, &x, sizeof(x));
    if (n != sizeof(x)) {
        perror("read(eventfd)");
    }

    return x;
}

int
uipcps_loop_signal(struct uipcps *uipcps)
{
    return eventfd_signal(uipcps->efd, 1);
}

struct periodic_task *
periodic_task_register(struct uipcp *uipcp, periodic_task_func_t func,
                       unsigned period)
{
    struct uipcps *uipcps = uipcp->uipcps;
    struct periodic_task *task;

    if (!func || period == 0) {
        PE("Invalid parameters (%p, %u)\n", func, period);
        return NULL;
    }

    if (uipcps->num_periodic_tasks > 2048) {
        PE("Too many periodic tasks (max=%u)\n", uipcps->num_periodic_tasks);
        return NULL;
    }

    task = rl_alloc(sizeof(*task), RL_MT_EVLOOP);
    memset(task, 0, sizeof(*task));
    task->uipcp    = uipcp;
    task->func     = func;
    task->period   = period;
    task->next_exp = time(NULL) + task->period;

    pthread_mutex_lock(&uipcps->lock);
    list_add_tail(&task->node, &uipcps->periodic_tasks);
    uipcps->num_periodic_tasks++;
    pthread_mutex_unlock(&uipcps->lock);

    return task;
}

void
periodic_task_unregister(struct periodic_task *task)
{
    struct uipcps *uipcps = task->uipcp->uipcps;

    pthread_mutex_lock(&uipcps->lock);
    list_del(&task->node);
    assert(uipcps->num_periodic_tasks > 0);
    uipcps->num_periodic_tasks--;
    pthread_mutex_unlock(&uipcps->lock);
    rl_free(task, RL_MT_EVLOOP);
}

static void *
uipcps_loop(void *opaque)
{
    struct uipcps *uipcps = opaque;

    for (;;) {
        struct rl_kmsg_ipcp_update *upd;
        struct pollfd pfd[2];
        int timeout_ms = -1;
        int ret        = 0;

        pfd[0].fd     = uipcps->cfd;
        pfd[0].events = POLLIN;
        pfd[1].fd     = uipcps->efd;
        pfd[1].events = POLLIN;

        /* Compute next timeout for periodic tasks, if any. */
        {
            struct periodic_task *task;
            time_t now      = time(NULL);
            time_t next_exp = now + 10000;

            pthread_mutex_lock(&uipcps->lock);
            list_for_each_entry (task, &uipcps->periodic_tasks, node) {
                if (now >= task->next_exp) {
                    /* Something expired, set timeout to 0. */
                    timeout_ms = 0;
                    break;
                } else if (task->next_exp < next_exp) {
                    next_exp   = task->next_exp;
                    timeout_ms = (next_exp - now) * 1000;
                }
            }
            pthread_mutex_unlock(&uipcps->lock);
        }

        ret = poll(pfd, 2, timeout_ms);
        if (ret < 0) {
            PE("poll() failed [%s]\n", strerror(errno));
            break;
        }

        /* Drain notification, if any. */
        if (pfd[1].revents & POLLIN) {
            eventfd_drain(uipcps->efd);
        }

        if ((pfd[0].revents & POLLIN) == 0) {
            /* Timeout or notification, run the periodic tasks. */
            struct list_head expired;
            struct periodic_task *task, *tmp;
            time_t now = time(NULL);

            /* Collect expired tasks (under the lock). Take a reference to
             * the involved uipcps. */
            pthread_mutex_lock(&uipcps->lock);
            list_init(&expired);
            list_for_each_entry (task, &uipcps->periodic_tasks, node) {
                if (now >= task->next_exp) {
                    struct periodic_task *copy =
                        rl_alloc(sizeof(*copy), RL_MT_EVLOOP);

                    if (!copy) {
                        PE("Out of memory: cannot process periodic task\n");
                    } else {
                        memset(copy, 0, sizeof(*copy));
                        copy->func  = task->func;
                        copy->uipcp = task->uipcp;
                        list_add_tail(&copy->node, &expired);
                        task->uipcp->refcnt++;
                    }
                    task->next_exp += task->period; /* set next expiration */
                }
            }
            pthread_mutex_unlock(&uipcps->lock);

            /* Invoke the expired tasks out of the lock, dropping the
             * uipcp reference counter. */
            if (!list_empty(&expired)) {
                list_for_each_entry_safe (task, tmp, &expired, node) {
                    task->func(task->uipcp);
                    list_del(&task->node);
                    uipcp_put(task->uipcp);
                    rl_free(task, RL_MT_EVLOOP);
                }
            }

            continue;
        }

        /* There is something to read on the control file descriptor. It
         * can only be an IPCP update message. */
        upd = (struct rl_kmsg_ipcp_update *)rl_read_next_msg(uipcps->cfd, 1);
        if (!upd) {
            break;
        }

        assert(upd->hdr.msg_type == RLITE_KER_IPCP_UPDATE);

        switch (upd->update_type) {
        case RL_IPCP_UPDATE_ADD:
        case RL_IPCP_UPDATE_UPD:
            if (!upd->dif_type || !upd->dif_name ||
                !rina_sername_valid(upd->ipcp_name)) {
                PE("Invalid ipcp update\n");
            }
            break;
        }

        ret = 0;
        switch (upd->update_type) {
        case RL_IPCP_UPDATE_ADD:
            ret = uipcp_add(uipcps, upd);
            break;

        case RL_IPCP_UPDATE_UIPCP_DEL:
            /* This can be an IPCP with no userspace implementation. */
            ret = uipcp_put_by_id(uipcps, upd->ipcp_id);
            break;

        case RL_IPCP_UPDATE_UPD:
            ret = uipcp_update(uipcps, upd);
            break;
        }

        rl_msg_free(rl_ker_numtables, RLITE_KER_MSG_MAX, RLITE_MB(upd));
        rl_free(upd, RL_MT_MSG);

        if (ret) {
            PE("IPCP update synchronization failed\n");
        }
    }

    PE("uipcps main loop exits [%s]\n", strerror(errno));

    return NULL;
}

#ifdef RL_UIPCPS_BACKTRACE
#include <execinfo.h>
#endif

static void
print_backtrace(void)
{
#ifdef RL_UIPCPS_BACKTRACE
    void *array[20];
    size_t size;

    /* get void*'s for all entries on the stack */
    size = backtrace(array, 20);

    /* print out all the frames to stderr */
    backtrace_symbols_fd(array, size, STDERR_FILENO);
#endif
}

static void
sigint_handler(int signum)
{
    assert(signum != SIGPIPE);
    print_backtrace();
    PD("%s signal received, daemon is going to exit ...\n", strsignal(signum));
    unlink_and_exit(EXIT_SUCCESS);
}

static int
char_device_exists(const char *path)
{
    struct stat s;

    return stat(path, &s) == 0 && S_ISCHR(s.st_mode);
}

/* Turn this program into a daemon process. */
static void
daemonize(void)
{
    pid_t pid = fork();
    pid_t sid;

    if (pid < 0) {
        perror("fork(daemonize)");
        unlink_and_exit(EXIT_FAILURE);
    }

    if (pid > 0) {
        /* This is the parent. We can terminate it. */
        exit(0);
    }

    /* Execution continues only in the child's context. */
    sid = setsid();
    if (sid < 0) {
        unlink_and_exit(EXIT_FAILURE);
    }

    if (chdir("/")) {
        unlink_and_exit(EXIT_FAILURE);
    }
}

static void
usage(void)
{
    printf("rlite-uipcps [OPTIONS]\n"
           "   -h : show this help\n"
           "   -v VERB_LEVEL : set verbosity LEVEL: QUIET, WARN, INFO, "
           "DBG (default), VERY\n"
           "   -d : start as a daemon process\n"
           "   -p : TCP port to listen to for client connections\n"
           "   -V : show versioning info and exit\n"
           "   -T PERIOD: for periodic tasks (in seconds)\n"
           "   -H HANDOVER_MANAGER: (none, signal-strength)\n");
}

void normal_lib_init(void);

__attribute__((weak)) int
main(int argc, char **argv)
{
    int port              = RLITE_UIPCP_PORT_DEFAULT;
    struct uipcps *uipcps = &guipcps;
    struct sockaddr_in server_address;
    struct sigaction sa;
    const char *verbosity = "DBG";
    int daemon            = 0;
    int pidfd             = -1;
    int ret, opt;

    while ((opt = getopt(argc, argv, "hv:dT:H:Vp:")) != -1) {
        switch (opt) {
        case 'h':
            usage();
            return 0;

        case 'v':
            verbosity = optarg;
            break;

        case 'd':
            daemon = 1;
            break;

        case 'H':
            if (!strcmp(optarg, "none")) {
                uipcps->handover_manager = NULL;
            } else if (!strcmp(optarg, "signal-strength")) {
                uipcps->handover_manager = optarg;
            } else {
                printf("Unknown handover manager '%s'\n", optarg);
                usage();
                return -1;
            }
            break;

        case 'V':
            printf("rlite-uipcps versioning info\n");
            printf("    revision id  : %s\n", RL_REVISION_ID);
            printf("    revision date: %s\n", RL_REVISION_DATE);
            return 0;
            break;

        case 'p':
            port = atoi(optarg);
            if (port < 1 || port >= 65535) {
                printf("    Invalid value '%s' for the -p option\n", optarg);
                usage();
                return -1;
            }

        default:
            printf("    Unrecognized option %c\n", opt);
            usage();
            return -1;
        }
    }

    /* Set verbosity level. */
    if (strcmp(verbosity, "VERY") == 0) {
        rl_verbosity = RL_VERB_VERY;
    } else if (strcmp(verbosity, "INFO") == 0) {
        rl_verbosity = RL_VERB_INFO;
    } else if (strcmp(verbosity, "WARN") == 0) {
        rl_verbosity = RL_VERB_WARN;
    } else if (strcmp(verbosity, "QUIET") == 0) {
        rl_verbosity = RL_VERB_QUIET;
    } else {
        rl_verbosity = RL_VERB_DBG;
    }

    /* We require root permissions. */
    if (geteuid() != 0) {
        PE("uipcps daemon needs root permissions\n");
        return -1;
    }

    if (!char_device_exists(RLITE_CTRLDEV_NAME)) {
        PE("Device %s not found\n", RLITE_CTRLDEV_NAME);
        return -1;
    }

    if (!char_device_exists(RLITE_IODEV_NAME)) {
        PE("Device %s not found\n", RLITE_IODEV_NAME);
        return -1;
    }

    ret = mkdir(RLITE_UIPCPS_VAR, 0x777);
    if (ret && errno != EEXIST) {
        fprintf(stderr, "warning: mkdir(%s) failed: %s\n", RLITE_UIPCPS_VAR,
                strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* Create pidfile, without checking for uniqueness. There may be multiple
     * rlite-uipcps daemons running in different namespaces, but sharing the
     * same mountpoints/filesystem. */
    {
        pidfd = open(RLITE_UIPCPS_PIDFILE, O_RDWR | O_CREAT, 0644);
        if (pidfd < 0) {
            perror("open(pidfile)");
            return -1;
        }
    }

    /* Static initializations for the normal IPCP process. */
    normal_lib_init();

    uipcps->terminate = 0;

    /* Open a TCP socket to listen to. */
    uipcps->lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (uipcps->lfd < 0) {
        perror("socket(AF_INET)");
        unlink(RLITE_UIPCPS_PIDFILE);
        exit(EXIT_FAILURE);
    }
    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family      = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    server_address.sin_port        = htons(port);

    {
        int one = 1;
        if (setsockopt(uipcps->lfd, SOL_SOCKET, SO_REUSEADDR, &one,
                       sizeof(one)) < 0) {
            perror("setsockopt(AF_INET)");
            return -1;
        }
    }

    ret = bind(uipcps->lfd, (struct sockaddr *)&server_address,
               sizeof(server_address));
    if (ret) {
        perror("bind(AF_INET, path)");
        unlink(RLITE_UIPCPS_PIDFILE);
        exit(EXIT_FAILURE);
    }
    ret = listen(uipcps->lfd, 50);
    if (ret) {
        perror("listen(AF_INET)");
        unlink_and_exit(EXIT_FAILURE);
    }

    /* Change permissions to rlite control and I/O device, so that anyone can
     * read and write. This a temporary solution, to be used until a
     * well-thought permission scheme is designed. */
    if (chmod(RLITE_CTRLDEV_NAME,
              S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)) {
        fprintf(stderr, "warning: chmod(%s) failed: %s\n", RLITE_CTRLDEV_NAME,
                strerror(errno));
    }

    if (chmod(RLITE_IODEV_NAME,
              S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)) {
        fprintf(stderr, "warning: chmod(%s) failed: %s\n", RLITE_IODEV_NAME,
                strerror(errno));
    }

    list_init(&uipcps->uipcps);
    pthread_mutex_init(&uipcps->lock, NULL);
    list_init(&uipcps->periodic_tasks);
    uipcps->num_periodic_tasks = 0;
    uipcps->n_uipcps           = 0;

    if (daemon) {
        /* The daemonizing function must be called before catching signals. */
        daemonize();
    }

    /* Write our PID to the pidfile. Note that this must happen after the
     * fork() that happens in daemonize(). */
    {
        char strbuf[128];
        int n;

        n = snprintf(strbuf, sizeof(strbuf), "%u", getpid());
        if (n < 0) {
            perror("snprintf(pid)");
            return -1;
        }

        if (write(pidfd, strbuf, n) != n) {
            perror("write(pidfile)");
            return -1;
        }

        if (syncfs(pidfd)) {
            perror("sync(pidfile)");
        }

        /* Keep the file open, it will be closed when the daemon exits. */
    }

    /* Set an handler for SIGINT and SIGTERM so that we can remove
     * the Unix domain socket used to access the uipcp server. */
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    ret         = sigaction(SIGINT, &sa, NULL);
    if (ret) {
        perror("sigaction(SIGINT)");
        unlink_and_exit(EXIT_FAILURE);
    }
    ret = sigaction(SIGTERM, &sa, NULL);
    if (ret) {
        perror("sigaction(SIGTERM)");
        unlink_and_exit(EXIT_FAILURE);
    }
#ifdef RL_UIPCPS_BACKTRACE
    ret = sigaction(SIGSEGV, &sa, NULL);
    if (ret) {
        perror("sigaction(SIGINT)");
        unlink_and_exit(EXIT_FAILURE);
    }
#endif
    /* Ignore the SIGPIPE signal, which is received when
     * trying to read/write from/to a Unix domain socket
     * that has been closed by the other end. */
    sa.sa_handler = SIG_IGN;
    ret           = sigaction(SIGPIPE, &sa, NULL);
    if (ret) {
        perror("sigaction(SIGPIPE)");
        unlink_and_exit(EXIT_FAILURE);
    }

    srand(time(NULL));

    /* Init the main loop which will take care of IPCP updates, to
     * align userspace IPCPs with kernelspace ones. This
     * must be done before launching the socket server in order to
     * avoid race conditions between main thread fetching and socket
     * server thread serving a client. That is, a client could see
     * incomplete state and its operation may fail or behave
     * unexpectedly.*/
    uipcps->cfd = rina_open();
    if (uipcps->cfd < 0) {
        PE("rina_open() failed [%s]\n", strerror(errno));
        unlink_and_exit(EXIT_FAILURE);
    }

    /* The main control loop need to receive IPCP updates (creation, removal
     * and configuration changes). */
    ret = ioctl(uipcps->cfd, RLITE_IOCTL_CHFLAGS, RL_F_IPCPS);
    if (ret) {
        PE("ioctl() failed [%s]\n", strerror(errno));
        unlink_and_exit(EXIT_FAILURE);
    }

    uipcps->efd = eventfd(0, 0);
    if (uipcps->efd < 0) {
        PE("eventfd() failed [%s]\n", strerror(errno));
        unlink_and_exit(EXIT_FAILURE);
    }

    ret = pthread_create(&uipcps->th, NULL, uipcps_loop, uipcps);
    if (ret) {
        PE("pthread_create() failed [%s]\n", strerror(errno));
        unlink_and_exit(EXIT_FAILURE);
    }

    /* Start the socket server. */
    socket_server(uipcps);

    /* The following code should be never reached, since the
     * socket server should execute until a SIGINT signal comes. */

    return 0;
}
