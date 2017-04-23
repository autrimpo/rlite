/*
 * Copyright (C) 2015-2017 Nextworks
 * Author: Vincenzo Maffione <v.maffione@gmail.com>
 *
 * This file is part of rlite.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <sys/time.h>
#include <assert.h>
#include <endian.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <pthread.h>

#include <rina/api.h>


#define SDU_SIZE_MAX        65535
#define RP_MAX_WORKERS      1023

#define RP_OPCODE_DATAFLOW  0
#define RP_OPCODE_PING      1
#define RP_OPCODE_RR        2
#define RP_OPCODE_PERF      3

#define CLI_FA_TIMEOUT_MSECS        5000
#define CLI_RESULT_TIMEOUT_MSECS    5000
#define RP_DATA_WAIT_MSECS          2000

static int stop = 0; /* Used to stop client on SIGINT. */
static int cli_flow_allocated = 0; /* Avoid to get stuck in rina_flow_alloc(). */

struct rinaperf;
struct worker;

struct rp_config_msg {
    uint32_t opcode;    /* opcode: ping, perf, rr ... */
    uint32_t ticket;    /* valid with RP_OPCODE_DATAFLOW */
    uint32_t size;      /* packet size in bytes */
    uint64_t cnt;       /* packet/transaction count for the test
                         * (0 means infinite) */
};

struct rp_ticket_msg {
    uint32_t ticket; /* ticket allocated by the server for the
                      * client to identify the data flow */
};

struct rp_result_msg {
    uint64_t cnt; /* number of packets or completed transactions
                   * as seen by the sender or the receiver */
    uint64_t pps; /* average packet rate measured by the sender or receiver */
    uint64_t bps; /* average bandwidth measured by the sender or receiver */
    uint64_t latency; /* in nanoseconds */
};

typedef int (*perf_fn_t)(struct worker *);
typedef void (*report_fn_t)(struct rp_result_msg *snd,
                                  struct rp_result_msg *rcv);

struct worker {
    pthread_t th;
    struct rinaperf         *rp;   /* backpointer */
    struct worker           *next; /* next worker */
    struct rp_config_msg    test_config;
    struct rp_result_msg    result;
    uint32_t                ticket; /* ticket to be sent to the client */
    pthread_cond_t          data_flow_ready; /* to wait for dfd */
    unsigned int            interval;
    unsigned int            burst;
    int                     ping;
    struct rp_test_desc     *desc;
    int                     cfd; /* control file descriptor */
    int                     dfd; /* data file descriptor */
};

struct rinaperf {
    struct rina_flow_spec   flowspec;
    const char              *cli_appl_name;
    const char              *srv_appl_name;
    const char              *dif_name;
    int                     cfd; /* Control file descriptor */
    int                     parallel;
    int                     verbose;

    /* Ticket table. */
    pthread_mutex_t         ticket_lock;
    struct worker           *ticket_table[RP_MAX_WORKERS];

    /* List of workers. */
    struct worker           *workers_head;
    struct worker           *workers_tail;
    unsigned int            workers_num;
};

static void
worker_init(struct worker *w, struct rinaperf *rp)
{
    memset(w, 0, sizeof(*w));
    w->rp = rp;
    w->cfd = w->dfd = -1;
    pthread_cond_init(&w->data_flow_ready, NULL);
}

static void
worker_fini(struct worker *w)
{
    pthread_cond_destroy(&w->data_flow_ready);

    if (w->cfd >= 0) {
        close(w->cfd);
        w->cfd = -1;
    }

    if (w->dfd >= 0) {
        close(w->dfd);
        w->dfd = -1;
    }
}

/* Used for both ping and rr tests. */
static int
ping_client(struct worker *w)
{
    unsigned int limit = w->test_config.cnt;
    struct timespec t_start, t_end, t1, t2;
    unsigned int interval = w->interval;
    int size = w->test_config.size;
    char buf[SDU_SIZE_MAX];
    volatile uint16_t *seqnum = (uint16_t *)buf;
    uint16_t expected = 0;
    unsigned int timeouts = 0;
    int ping = w->ping;
    unsigned int i = 0;
    unsigned long long ns;
    struct pollfd pfd;
    int ret = 0;

    if (size > sizeof(buf)) {
        printf("Warning: size truncated to %u\n", (unsigned int)sizeof(buf));
        size = sizeof(buf);
    }

    if (!w->ping) {
        printf("Starting request-response test; message size: %d, "
               " number of messages: ", size);
        if (limit) {
            printf("%d\n", limit);
        } else {
            printf("inf\n");
        }
    }

    pfd.fd = w->dfd;
    pfd.events = POLLIN;

    memset(buf, 'x', size);

    clock_gettime(CLOCK_MONOTONIC, &t_start);

    for (i = 0; !stop && (!limit || i < limit); i++, expected++) {
        if (ping) {
            clock_gettime(CLOCK_MONOTONIC, &t1);
        }

        *seqnum = (uint16_t)expected;

        ret = write(w->dfd, buf, size);
        if (ret != size) {
            if (ret < 0) {
                perror("write(buf)");
            } else {
                printf("Partial write %d/%d\n", ret, size);
            }
            break;
        }
repoll:
        ret = poll(&pfd, 1, RP_DATA_WAIT_MSECS);
        if (ret < 0) {
            perror("poll(flow)");
        }

        if (ret == 0) {
            printf("Timeout: %d bytes lost\n", size);
            if (++ timeouts > 8) {
                printf("Stopping after %u consecutive timeouts\n", timeouts);
                stop = 1;
            }
        } else {
            /* Ready to read. */
            timeouts = 0;
            ret = read(w->dfd, buf, sizeof(buf));
            if (ret <= 0) {
                if (ret) {
                    perror("read(buf");
                }
                break;
            }

            if (ping) {
                if (*seqnum == expected) {
                    clock_gettime(CLOCK_MONOTONIC, &t2);
                    ns = 1000000000 * (t2.tv_sec - t1.tv_sec) +
                                      (t2.tv_nsec - t1.tv_nsec);
                    printf("%d bytes from server: rtt = %.3f ms\n", ret,
                           ((float)ns)/1000000.0);
                } else {
                    printf("Packet lost or out of order: got %u, "
                           "expected %u\n", *seqnum, expected);
                    if (*seqnum < expected) {
                        goto repoll;
                    }
                }
            }
        }

        if (interval) {
            usleep(interval);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t_end);
    ns = 1000000000 * (t_end.tv_sec - t_start.tv_sec) +
            (t_end.tv_nsec - t_start.tv_nsec);

    w->result.cnt = i;
    w->result.pps = (1000000000ULL * i) / ns;
    w->result.bps = w->result.pps * 8 * size;
    w->result.latency = (ns/i) - interval * 1000;

    return 0;
}

/* Used for both ping and rr tests. */
static int
ping_server(struct worker *w)
{
    unsigned int limit = w->test_config.cnt;
    char buf[SDU_SIZE_MAX];
    struct pollfd pfd;
    unsigned int i;
    int n, ret;

    pfd.fd = w->dfd;
    pfd.events = POLLIN;

    for (i = 0; !limit || i < limit; i++) {
        n = poll(&pfd, 1, RP_DATA_WAIT_MSECS);
        if (n < 0) {
            perror("poll(flow)");
        } else if (n == 0) {
            /* Timeout */
            if (w->rp->verbose) {
                printf("Timeout occurred\n");
            }
            break;
        }

        /* File descriptor is ready for reading. */
        n = read(w->dfd, buf, sizeof(buf));
        if (n < 0) {
            perror("read(flow)");
            return -1;
        } else if (n == 0) {
            printf("Flow deallocated remotely\n");
            break;
        }

        ret = write(w->dfd, buf, n);
        if (ret != n) {
            if (ret < 0) {
                perror("write(flow)");
            } else {
                printf("Partial write");
            }
            return -1;
        }
    }

    w->result.cnt = i;

    if (w->rp->verbose) {
        printf("received %u PDUs out of %u\n", i, limit);
    }

    return 0;
}

static void
ping_report(struct rp_result_msg *snd, struct rp_result_msg *rcv)
{
    printf("%10s %15s %10s %10s %15s\n",
            "", "Transactions", "Kpps", "Mbps", "Latency (ns)");
    printf("%-10s %15lu %10.3f %10.3f %15lu\n",
            "Sender", snd->cnt, (double)snd->pps/1000.0,
                (double)snd->bps/1000000.0, snd->latency);
#if 0
    printf("%-10s %15lu %10.3f %10.3f %15lu\n",
            "Receiver", rcv->cnt, (double)rcv->pps/1000.0,
                (double)rcv->bps/1000000.0, rcv->latency);
#endif
}

static int
perf_client(struct worker *w)
{
    unsigned limit = w->test_config.cnt;
    int size = w->test_config.size;
    unsigned int interval = w->interval;
    unsigned int burst = w->burst;
    unsigned int cdown = burst;
    struct timeval t_start, t_end;
    struct timeval w1, w2;
    char buf[SDU_SIZE_MAX];
    unsigned long us;
    unsigned int i = 0;
    int ret;

    if (size > sizeof(buf)) {
        printf("Warning: size truncated to %u\n", (unsigned int)sizeof(buf));
        size = sizeof(buf);
    }

    printf("Starting unidirectional throughput test; message size: %d, "
            " number of messages: ", size);
    if (limit) {
        printf("%d\n", limit);
    } else {
        printf("inf\n");
    }

    memset(buf, 'x', size);

    gettimeofday(&t_start, NULL);

    for (i = 0; !stop && (!limit || i < limit); i++) {
        ret = write(w->dfd, buf, size);
        if (ret != size) {
            if (ret < 0) {
                perror("write(buf)");
            } else {
                printf("Partial write %d/%d\n", ret, size);
            }
            break;
        }

        if (interval && --cdown == 0) {
            if (interval > 50) { /* slack default is 50 us*/
                usleep(interval);
            } else {
                gettimeofday(&w1, NULL);
                for (;;) {
                    gettimeofday(&w2, NULL);
                    us = 1000000 * (w2.tv_sec - w1.tv_sec) +
                        (w2.tv_usec - w1.tv_usec);
                    if (us >= interval) {
                        break;
                    }
                }
            }
            cdown = burst;
        }
    }

    gettimeofday(&t_end, NULL);
    us = 1000000 * (t_end.tv_sec - t_start.tv_sec) +
            (t_end.tv_usec - t_start.tv_usec);

    if (us) {
        w->result.cnt = i;
        w->result.pps = (1000000ULL * i) / us;
        w->result.bps = w->result.pps * 8 * size;

        printf("Throughput: %.3f Kpps, %.3f Mbps\n",
                ((float)w->result.pps) / 1000.0,
                ((float)w->result.bps) / 1000000.0);
    }

    return 0;
}

static void
rate_print(unsigned long long *bytes, unsigned long long *cnt,
            unsigned long long *bytes_limit, struct timespec *ts,
            struct rp_result_msg *rmsg)
{
    struct timespec now;
    unsigned long long elapsed_ns;
    double kpps;
    double mbps;

    clock_gettime(CLOCK_MONOTONIC, &now);

    elapsed_ns = ((now.tv_sec - ts->tv_sec) * 1000000000 +
                    now.tv_nsec - ts->tv_nsec);

    kpps = ((1000000) * (double) *cnt) / elapsed_ns;
    mbps = ((8 * 1000) * (double) *bytes) / elapsed_ns;

    /* We don't want to prints which are too close. */
    if (elapsed_ns > 500000000U) {
        printf("rate: %f Kpss, %f Mbps\n", kpps, mbps);
    }

    rmsg->pps = (1000000000ULL * *cnt) / elapsed_ns;
    rmsg->bps = (8000000000ULL * *bytes) / elapsed_ns;

    if (elapsed_ns < 1000000000U) {
            *bytes_limit *= 2;
    } else if (elapsed_ns > 3 * 1000000000U && *bytes >= 1000) {
            *bytes_limit /= 2;
    }

    if (*bytes >= 1000) {
        clock_gettime(CLOCK_MONOTONIC, ts);
        *cnt = 0;
        *bytes = 0;
    }
}

static int
perf_server(struct worker *w)
{
    unsigned limit = w->test_config.cnt;
    unsigned long long rate_cnt = 0;
    unsigned long long rate_bytes_limit = 1000;
    unsigned long long rate_bytes = 0;
    struct timespec rate_ts, t_start, t_end;
    char buf[SDU_SIZE_MAX];
    unsigned long long ns;
    struct pollfd pfd;
    unsigned int i;
    int verb = w->rp->verbose;
    int timeout = 0;
    int n;

    pfd.fd = w->dfd;
    pfd.events = POLLIN;

    clock_gettime(CLOCK_MONOTONIC, &rate_ts);
    t_start = rate_ts;

    for (i = 0; !limit || i < limit; i++) {
        n = poll(&pfd, 1, RP_DATA_WAIT_MSECS);
        if (n < 0) {
            perror("poll(flow)");

        } else if (n == 0) {
            /* Timeout */
            timeout = 1;
            if (w->rp->verbose) {
                printf("Timeout occurred\n");
            }
            break;
        }

        /* Ready to read. */
        n = read(w->dfd, buf, sizeof(buf));
        if (n < 0) {
            perror("read(flow)");
            return -1;

        } else if (n == 0) {
            printf("Flow deallocated remotely\n");
            break;
        }

        rate_bytes += n;
        rate_cnt++;

        if (rate_bytes >= rate_bytes_limit && verb) {
            rate_print(&rate_bytes, &rate_cnt, &rate_bytes_limit,
                       &rate_ts, &w->result);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t_end);
    ns = 1000000000 * (t_end.tv_sec - t_start.tv_sec) +
                        (t_end.tv_nsec - t_start.tv_nsec);
    if (timeout) {
        /* There was a timeout, adjust the time measurement. */
        if (ns < RP_DATA_WAIT_MSECS * 1000000) {
            ns = 0;
        } else {
            ns -= RP_DATA_WAIT_MSECS * 1000000;
        }
    }

    w->result.pps = (1000000000ULL * i) / ns;
    w->result.bps = w->result.pps * 8 * w->test_config.size;
    w->result.cnt = i;

    if (verb) {
        printf("Received %u PDUs out of %u\n", i, limit);
    }

    return 0;
}

static void
perf_report(struct rp_result_msg *snd, struct rp_result_msg *rcv)
{
    printf("%10s %12s %10s %10s\n",
            "", "Packets", "Kpps", "Mbps");
    printf("%-10s %12lu %10.3f %10.3f\n",
            "Sender", snd->cnt, (double)snd->pps/1000.0,
                (double)snd->bps/1000000.0);
    printf("%-10s %12lu %10.3f %10.3f\n",
            "Receiver", rcv->cnt, (double)rcv->pps/1000.0,
                (double)rcv->bps/1000000.0);
}

struct rp_test_desc {
    const char      *name;
    unsigned int    opcode;
    perf_fn_t       client_fn;
    perf_fn_t       server_fn;
    report_fn_t     report_fn;
};

static struct rp_test_desc descs[] = {
    {   /* placeholder */
        .name = NULL,
        .opcode = RP_OPCODE_DATAFLOW,
        .client_fn = NULL,
        .server_fn = NULL,
        .report_fn = NULL,
    },
    {
        .name = "ping",
        .opcode = RP_OPCODE_PING,
        .client_fn = ping_client,
        .server_fn = ping_server,
        .report_fn = ping_report,
    },
    {
        .name = "rr",
        .opcode = RP_OPCODE_RR,
        .client_fn = ping_client,
        .server_fn = ping_server,
        .report_fn = ping_report,
    },
    {   .name = "perf",
        .opcode = RP_OPCODE_PERF,
        .client_fn = perf_client,
        .server_fn = perf_server,
        .report_fn = perf_report,
    },
};

static void *
client_worker_function(void *opaque)
{
    struct worker *w = opaque;
    struct rp_config_msg cfg = w->test_config;
    struct rp_ticket_msg tmsg;
    struct rp_result_msg rmsg;
    struct pollfd pfd;
    int ret;

    /* Allocate the control flow to be used for test configuration and
     * to receive test result.
     * We should always use reliable flows. */
    pfd.fd = rina_flow_alloc(w->rp->dif_name, w->rp->cli_appl_name,
                             w->rp->srv_appl_name, &w->rp->flowspec,
                             RINA_F_NOWAIT);
    pfd.events = POLLIN;
    ret = poll(&pfd, 1, CLI_FA_TIMEOUT_MSECS);
    if (ret <= 0) {
        if (ret < 0) {
            perror("poll(cfd)");
        } else {
            printf("Flow allocation timed out for control flow\n");
        }
        close(pfd.fd);
        return NULL;
    }
    w->cfd = rina_flow_alloc_wait(pfd.fd);
    if (w->cfd < 0) {
        perror("rina_flow_alloc(cfd)");
        goto out;
    }

    /* Send test configuration to the server. */
    cfg.opcode = htole32(cfg.opcode);
    cfg.cnt = htole64(cfg.cnt);
    cfg.size = htole32(cfg.size);

    ret = write(w->cfd, &cfg, sizeof(cfg));
    if (ret != sizeof(cfg)) {
        if (ret < 0) {
            perror("write(cfg)");
        } else {
            printf("Partial write %d/%lu\n", ret,
                    (unsigned long int)sizeof(cfg));
        }
        goto out;
    }

    /* Wait for the ticket message from the server and read it. */
    pfd.fd = w->cfd;
    pfd.events = POLLIN;
    ret = poll(&pfd, 1, RP_DATA_WAIT_MSECS);
    if (ret <= 0) {
        if (ret < 0) {
            perror("poll(ticket)");
        } else {
            printf("timeout while waiting for ticket message\n");
        }
        goto out;
    }

    ret = read(w->cfd, &tmsg, sizeof(tmsg));
    if (ret != sizeof(tmsg)) {
        if (ret < 0) {
            perror("read(ticket)");
        } else {
            printf("Error reading ticket message: wrong length %d "
                   "(should be %lu)\n", ret, (unsigned long int)sizeof(tmsg));
        }
        goto out;
    }

    /* Allocate a data flow for the test. */
    pfd.fd = rina_flow_alloc(w->rp->dif_name, w->rp->cli_appl_name,
                             w->rp->srv_appl_name, &w->rp->flowspec,
                             RINA_F_NOWAIT);
    pfd.events = POLLIN;
    ret = poll(&pfd, 1, CLI_FA_TIMEOUT_MSECS);
    if (ret <= 0) {
        if (ret < 0) {
            perror("poll(dfd)");
        } else {
            printf("Flow allocation timed out for data flow\n");
        }
        close(pfd.fd);
        goto out;
    }
    w->dfd = rina_flow_alloc_wait(pfd.fd);
    cli_flow_allocated = 1;
    if (w->dfd < 0) {
        perror("rina_flow_alloc(dfd)");
        goto out;
    }

    /* Send the ticket to the server to identify the data flow. */
    memset(&cfg, 0, sizeof(cfg));
    cfg.opcode = htole32(RP_OPCODE_DATAFLOW);
    cfg.ticket = tmsg.ticket;
    ret = write(w->dfd, &cfg, sizeof(cfg));
    if (ret != sizeof(cfg)) {
        if (ret < 0) {
            perror("write(identify)");
        } else {
            printf("Partial write %d/%lu\n", ret,
                    (unsigned long int)sizeof(cfg));
        }
        goto out;
    }

    /* Run the test. */
    w->desc->client_fn(w);

    if (!stop) {
        /* Wait for the result message from the server and read it. */
        pfd.fd = w->cfd;
        pfd.events = POLLIN;
        ret = poll(&pfd, 1, CLI_RESULT_TIMEOUT_MSECS);
        if (ret <= 0) {
            if (ret < 0) {
                perror("poll(result)");
            } else {
                printf("timeout while waiting for result message\n");
            }
            goto out;
        }

        ret = read(w->cfd, &rmsg, sizeof(rmsg));
        if (ret != sizeof(rmsg)) {
            if (ret < 0) {
                perror("read(result)");
            } else {
                printf("Error reading result message: wrong length %d "
                       "(should be %lu)\n", ret, (unsigned long int)sizeof(rmsg));
            }
            goto out;
        }

        rmsg.cnt        = le64toh(rmsg.cnt);
        rmsg.pps        = le64toh(rmsg.pps);
        rmsg.bps        = le64toh(rmsg.bps);
        rmsg.latency    = le64toh(rmsg.latency);

        w->desc->report_fn(&w->result, &rmsg);
    }

out:
    worker_fini(w);

    return NULL;
}

static void *
server_worker_function(void *opaque)
{
    struct worker *w = opaque;
    struct rinaperf *rp = w->rp;
    struct rp_config_msg cfg;
    struct rp_ticket_msg tmsg;
    struct rp_result_msg rmsg;
    struct pollfd pfd;
    uint32_t ticket;
    int ret;

    /* Wait for test configuration message and read it. */
    pfd.fd = w->cfd;
    pfd.events = POLLIN;
    ret = poll(&pfd, 1, RP_DATA_WAIT_MSECS);
    if (ret <= 0) {
        if (ret < 0) {
            perror("poll(cfg)");
        } else {
            printf("timeout while waiting for configuration message\n");
        }
        goto out;
    }

    ret = read(w->cfd, &cfg, sizeof(cfg));
    if (ret != sizeof(cfg)) {
        if (ret < 0) {
            perror("read(cfg)");
        } else {
            printf("Error reading test configuration: wrong length %d "
                   "(should be %lu)\n", ret, (unsigned long int)sizeof(cfg));
        }
        goto out;
    }

    cfg.opcode  = le32toh(cfg.opcode);
    cfg.ticket  = le32toh(cfg.ticket);
    cfg.cnt     = le64toh(cfg.cnt);
    cfg.size    = le32toh(cfg.size);

    if (cfg.opcode >= sizeof(descs)) {
        printf("Invalid test configuration: test type %u is invalid\n", cfg.opcode);
        goto out;
    }

    if (cfg.opcode == RP_OPCODE_DATAFLOW) {
        /* This is a data flow. We need to pass the file descriptor to the worker
         * associated to the ticket, and notify it. */
        int err = 0;

        pthread_mutex_lock(&rp->ticket_lock);
        if (cfg.ticket >= RP_MAX_WORKERS || !rp->ticket_table[cfg.ticket]) {
            printf("Invalid ticket request: ticket %u is invalid\n", cfg.ticket);
            err = -1;
        } else {
            struct worker *tw = rp->ticket_table[cfg.ticket];

            tw->dfd = w->cfd;
            w->cfd = -1;
            pthread_cond_signal(&tw->data_flow_ready);
        }
        pthread_mutex_unlock(&rp->ticket_lock);

        if (err) {
            goto out;
        }

        return NULL;
    }

    if (cfg.size < sizeof(uint16_t)) {
        printf("Invalid test configuration: size %u is invalid\n", cfg.size);
        goto out;
    }

    /* Allocate a ticket for the client. */
    {
        pthread_mutex_lock(&rp->ticket_lock);
        for (ticket = 0; ticket < RP_MAX_WORKERS; ticket ++) {
            if (rp->ticket_table[ticket] == NULL) {
                rp->ticket_table[ticket] = w;
                break;
            }
        }
        pthread_mutex_unlock(&rp->ticket_lock);
        assert(ticket < RP_MAX_WORKERS);
    }

    /* Send ticket back to the client. */
    tmsg.ticket = htole32(ticket);
    ret = write(w->cfd, &tmsg, sizeof(tmsg));
    if (ret != sizeof(tmsg)) {
        if (ret < 0) {
            perror("write(ticket)");
        } else {
            printf("Error writing ticket: wrong length %d "
                   "(should be %lu)\n", ret, (unsigned long int)sizeof(tmsg));
        }
        goto out;
    }

    if (rp->verbose) {
        printf("Configuring test type %u, SDU count %lu, SDU size %u, "
                "ticket %u\n",
               cfg.opcode, cfg.cnt, cfg.size, ticket);
    }

    /* Wait for the client to allocate a data flow and come back to us. */
    pthread_mutex_lock(&rp->ticket_lock);
    ret = 0;
    while (w->dfd == -1 && ret == 0) {
        struct timespec to;

        clock_gettime(CLOCK_REALTIME, &to);
        to.tv_sec += 2;

        ret = pthread_cond_timedwait(&w->data_flow_ready,
                                     &rp->ticket_lock, &to);
    }
    rp->ticket_table[ticket] = NULL;
    pthread_mutex_unlock(&rp->ticket_lock);
    if (ret) {
        if (ret == ETIMEDOUT) {
            printf("Timed out waiting for data flow [ticket %u]\n", ticket);
        } else {
            printf("pthread_cond_timedwait() failed [%d]\n", ret);
        }
        goto out;
    }

    /* Serve the client on the flow file descriptor. */
    w->test_config = cfg;
    w->desc = descs + cfg.opcode;
    assert(w->desc);
    w->desc->server_fn(w);

    /* Write the result back to the client on the control file descriptor. */
    rmsg = w->result;
    rmsg.cnt        = htole64(rmsg.cnt);
    rmsg.pps        = htole64(rmsg.pps);
    rmsg.bps        = htole64(rmsg.bps);
    rmsg.latency    = htole64(rmsg.latency);
    ret = write(w->cfd, &rmsg, sizeof(rmsg));
    if (ret != sizeof(rmsg)) {
        if (ret < 0) {
            perror("write(result)");
        } else {
            printf("Error writing result: wrong length %d "
                   "(should be %lu)\n", ret, (unsigned long int)sizeof(rmsg));
        }
        goto out;
    }

out:
    worker_fini(w);

    return NULL;
}

static int
server(struct rinaperf *rp)
{
    struct worker *w = NULL;
    int ret;

    /* Server-side initializations. */
    ret = rina_register(rp->cfd, rp->dif_name, rp->srv_appl_name, 0);
    if (ret) {
        perror("rina_register()");
        return ret;
    }

    for (;;) {
        struct worker *p;
        int ret;

        for (;;) {
            /* Try to join terminated threads. */
            for (p = NULL, w = rp->workers_head; w; ) {
                ret = pthread_tryjoin_np(w->th, NULL);
                if (ret == 0) {
                    if (w == rp->workers_head) {
                        rp->workers_head = w->next;
                    }
                    if (p) {
                        p->next = w->next;
                    }
                    if (w == rp->workers_tail) {
                        rp->workers_tail = p;
                    }
                    {
                        struct worker *tmp;
                        tmp = w;
                        w = w->next;
                        worker_fini(tmp);
                        free(tmp);
                    }
                    rp->workers_num --;

                } else {
                    if (ret != EBUSY) {
                        printf("Failed to tryjoin() pthread: %s\n", strerror(ret));
                    }
                    p = w;
                    w = w->next;
                }
            }

            if (rp->workers_num < RP_MAX_WORKERS) {
                break;
            }
            usleep(10000);
        }

        /* Allocate new worker and accept a new flow. */
        w = malloc(sizeof(*w));
        if (!w) {
            printf("Out of memory\n");
            return -1;
        }
        memset(w, 0, sizeof(*w));
        worker_init(w, rp);

        w->cfd = rina_flow_accept(rp->cfd, NULL, NULL, 0);
        if (w->cfd < 0) {
            perror("rina_flow_accept()");
            break;
        }

        ret = pthread_create(&w->th, NULL, server_worker_function, w);
        if (ret) {
            printf("pthread_create() failed: %s\n", strerror(ret));
            break;
        }

        /* List tail insertion */
        if (rp->workers_tail == NULL) {
            rp->workers_head = rp->workers_tail = w;
        } else {
            rp->workers_tail->next = w;
            rp->workers_tail = w;
        }
        rp->workers_num ++;
#if 0
        printf("Active workers %u\n", rp->workers_num);
#endif
    }

    if (w) {
        worker_fini(w);
        free(w);
    }

    return 0;
}

static void
sigint_handler_client(int signum)
{
    if (!cli_flow_allocated) {
        exit(EXIT_SUCCESS);
    }
    stop = 1;
}

static void
sigint_handler_server(int signum)
{
    exit(EXIT_SUCCESS);
}

static void
parse_bandwidth(struct rina_flow_spec *spec, const char *arg)
{
    size_t arglen = strlen(arg);

    if (arglen < 2) {
        goto err;
    }

    spec->avg_bandwidth = 1;
    switch (arg[arglen-1]) {
        case 'G':
            spec->avg_bandwidth *= 1000;
        case 'M':
            spec->avg_bandwidth *= 1000;
        case 'K':
            spec->avg_bandwidth *= 1000;
            break;
        default:
            if (arg[arglen-1] < '0' || arg[arglen-1] > '9') {
                goto err;
            }
            break;
    }

    spec->avg_bandwidth *= strtoul(arg, NULL, 10);
    printf("Parsed bandwidth %llu\n", (long long unsigned)spec->avg_bandwidth);

    return;
err:
    printf("Invalid bandwidth format '%s'\n", arg);
}

static void
usage(void)
{
    printf("rinaperf [OPTIONS]\n"
        "   -h : show this help\n"
        "   -l : run in server mode (listen)\n"
        "   -t TEST : specify the type of the test to be performed "
            "(ping, perf, rr)\n"
        "   -d DIF : name of DIF to which register or ask to allocate a flow\n"
        "   -c NUM : number of SDUs to send during the test\n"
        "   -s NUM : size of the SDUs that are sent during the test\n"
        "   -i NUM : number of microseconds to wait after each SDUs is sent\n"
        "   -g NUM : max SDU gap to use for the data flow\n"
        "   -B NUM : average bandwitdh for the data flow, in bits per second\n"
        "   -f : enable flow control\n"
        "   -b NUM : How many SDUs to send before waiting as "
                "specified by -i option (default b=1)\n"
        "   -a APNAME : application process name and instance of the rinaperf client\n"
        "   -z APNAME : application process name and instance of the rinaperf server\n"
        "   -p NUM : clients run NUM parallel instances, using NUM threads\n"
        "   -v : be verbose\n"
          );
}

int
main(int argc, char **argv)
{
    struct sigaction sa;
    struct rinaperf rp;
    const char *type = "ping";
    int interval_specified = 0;
    int listen = 0;
    int cnt = 0;
    int size = sizeof(uint16_t);
    int interval = 0;
    int burst = 1;
    struct worker wt; /* template */
    int ret;
    int opt;
    int i;

    memset(&rp, 0, sizeof(rp));
    pthread_mutex_init(&rp.ticket_lock, NULL);

    memset(&wt, 0, sizeof(wt));
    wt.rp = &rp;
    wt.cfd = -1;

    rp.cli_appl_name = "rinaperf-data:client";
    rp.srv_appl_name = "rinaperf-data:server";
    rp.parallel = 1;
    rp.verbose = 0;

    /* Start with a default flow configuration (unreliable flow). */
    rina_flow_spec_default(&rp.flowspec);

    while ((opt = getopt(argc, argv, "hlt:d:c:s:i:B:g:fb:a:z:p:v")) != -1) {
        switch (opt) {
            case 'h':
                usage();
                return 0;

            case 'l':
                listen = 1;
                break;

            case 't':
                type = optarg;
                break;

            case 'd':
                rp.dif_name = optarg;
                break;

            case 'c':
                cnt = atoi(optarg);
                if (cnt < 0) {
                    printf("    Invalid 'cnt' %d\n", cnt);
                    return -1;
                }
                break;

            case 's':
                size = atoi(optarg);
                if (size < sizeof(uint16_t)) {
                    printf("    Invalid 'size' %d\n", size);
                    return -1;
                }
                break;

            case 'i':
                interval = atoi(optarg);
                if (interval < 0) {
                    printf("    Invalid 'interval' %d\n", interval);
                    return -1;
                }
                interval_specified = 1;
                break;

            case 'g': /* Set max_sdu_gap flow specification parameter. */
                rp.flowspec.max_sdu_gap = atoll(optarg);
                break;

            case 'B': /* Set the average bandwidth parameter. */
                parse_bandwidth(&rp.flowspec, optarg);
                break;

            case 'f': /* Enable flow control. */
                rp.flowspec.spare3 = 1;
                break;

            case 'b':
                burst = atoi(optarg);
                if (burst <= 0) {
                    printf("    Invalid 'burst' %d\n", burst);
                    return -1;
                }
                break;

            case 'a':
                rp.cli_appl_name = optarg;
                break;

            case 'z':
                rp.srv_appl_name = optarg;
                break;

            case 'p':
                rp.parallel = atoi(optarg);
                if (rp.parallel <= 0) {
                    printf("    Invalid 'parallel' %d\n", rp.parallel);
                    return -1;
                }
                break;

            case 'v':
                rp.verbose = 1;
                break;

            default:
                printf("    Unrecognized option %c\n", opt);
                usage();
                return -1;
        }
    }

    /*
     * Fixups:
     *   - Use 1 second interval for ping tests, if the user did not
     *     specify the interval explicitly.
     *   - Set rp.ping variable to distinguish between ping and rr tests,
     *     which share the same functions.
     */
    if (strcmp(type, "ping") == 0) {
        if (!interval_specified) {
            interval = 1000000;
        }
        wt.ping = 1;

    } else if (strcmp(type, "rr") == 0) {
        wt.ping = 0;
    }

    /* Set defaults. */
    wt.interval = interval;
    wt.burst = burst;

    /* Function selection. */
    if (!listen) {
        for (i = 0; i < sizeof(descs)/sizeof(descs[0]); i++) {
            if (descs[i].name && strcmp(descs[i].name, type) == 0) {
                wt.desc = descs + i;
                break;
            }
        }

        if (wt.desc == NULL) {
            printf("    Unknown test type '%s'\n", type);
            usage();
            return -1;
        }
        wt.test_config.opcode = descs[i].opcode;
        wt.test_config.cnt = cnt;
        wt.test_config.size = size;
    }

    /* Set some signal handler */
    sa.sa_handler = listen ? sigint_handler_server : sigint_handler_client;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    ret = sigaction(SIGINT, &sa, NULL);
    if (ret) {
        perror("sigaction(SIGINT)");
        return ret;
    }
    ret = sigaction(SIGTERM, &sa, NULL);
    if (ret) {
        perror("sigaction(SIGTERM)");
        return ret;
    }

    /* Open control file descriptor. */
    rp.cfd = rina_open();
    if (rp.cfd < 0) {
        perror("rina_open()");
        return rp.cfd;
    }

    if (listen) {
        server(&rp);
    } else if (rp.parallel == 1) {
        client_worker_function(&wt);
    } else {
        struct worker *workers = calloc(rp.parallel, sizeof(*workers));

        if (workers == NULL) {
            printf("Failed to allocate client workers\n");
            return -1;
        }

        for (i = 0; i < rp.parallel; i++) {
            memcpy(workers + i, &wt, sizeof(wt));
            ret = pthread_create(&workers[i].th, NULL, client_worker_function,
                                 workers + i);
            if (ret) {
                printf("pthread_create(#%d) failed: %s\n", i, strerror(ret));
                break;
            }
        }

        for (i = 0; i < rp.parallel; i++) {
            ret = pthread_join(workers[i].th, NULL);
            if (ret) {
                printf("pthread_join(#%d) failed: %s\n", i, strerror(ret));
            }
        }
    }

    return close(rp.cfd);
}
