/* Socket option cache helpers
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <pthread.h>
#include <stdbool.h>
#include <sys/socket.h>

#include "utils.h"

#include "runtime/thread.h"

#include "syscall/internal.h"
#include "syscall/net.h"
#include "syscall/net-sockopt.h"

static void net_socket_cache_set_many_zero(int guest_fd,
                                           const int *indices,
                                           size_t count)
{
    for (size_t i = 0; i < count; i++)
        net_socket_cache_set_index(guest_fd, indices[i], 0);
}

int net_socket_fd_is_valid(int guest_fd)
{
    if (!RANGE_CHECK(guest_fd, 0, FD_TABLE_SIZE))
        return 0;

    if (thread_is_single_active())
        return fd_table[guest_fd].type == FD_SOCKET;

    pthread_mutex_lock(&fd_lock);
    bool valid = fd_table[guest_fd].type == FD_SOCKET;
    pthread_mutex_unlock(&fd_lock);
    return valid;
}

static int net_sock_cache_get(int guest_fd, int idx, int *value)
{
    if (!RANGE_CHECK(guest_fd, 0, FD_TABLE_SIZE) || !value)
        return 0;

    if (thread_is_single_active()) {
        if (fd_table[guest_fd].type == FD_SOCKET)
            return sock_opt_get(&fd_table[guest_fd], idx, value);
        return 0;
    }

    pthread_mutex_lock(&fd_lock);
    bool ok = (fd_table[guest_fd].type == FD_SOCKET)
                  ? sock_opt_get(&fd_table[guest_fd], idx, value)
                  : false;
    pthread_mutex_unlock(&fd_lock);
    return ok;
}

void net_socket_cache_set_index(int guest_fd, int idx, int value)
{
    if (!RANGE_CHECK(guest_fd, 0, FD_TABLE_SIZE))
        return;

    if (thread_is_single_active()) {
        if (fd_table[guest_fd].type == FD_SOCKET)
            sock_opt_set(&fd_table[guest_fd], idx, value);
        return;
    }

    pthread_mutex_lock(&fd_lock);
    if (fd_table[guest_fd].type == FD_SOCKET)
        sock_opt_set(&fd_table[guest_fd], idx, value);
    pthread_mutex_unlock(&fd_lock);
}

static int net_sock_opt_index_for(int level, int optname)
{
    if (level == LINUX_SOL_SOCKET) {
        switch (optname) {
        case LINUX_SO_KEEPALIVE:
            return SOCK_OPT_KEEPALIVE;
        case LINUX_SO_REUSEADDR:
            return SOCK_OPT_REUSEADDR;
        case LINUX_SO_ACCEPTCONN:
            return SOCK_OPT_ACCEPTCONN;
        case LINUX_SO_REUSEPORT:
            return SOCK_OPT_REUSEPORT;
        case LINUX_SO_BROADCAST:
            return SOCK_OPT_BROADCAST;
        case LINUX_SO_DONTROUTE:
            return SOCK_OPT_DONTROUTE;
        case LINUX_SO_OOBINLINE:
            return SOCK_OPT_OOBINLINE;
        case LINUX_SO_RCVLOWAT:
            return SOCK_OPT_RCVLOWAT;
        case LINUX_SO_SNDLOWAT:
            return SOCK_OPT_SNDLOWAT;
        case LINUX_SO_RCVBUF:
            return SOCK_OPT_RCVBUF;
        case LINUX_SO_SNDBUF:
            return SOCK_OPT_SNDBUF;
        case LINUX_SO_TYPE:
            return SOCK_OPT_TYPE;
        case LINUX_SO_PASSCRED:
            return SOCK_OPT_PASSCRED;
        default:
            return -1;
        }
    }
    if (level == LINUX_IPPROTO_TCP) {
        switch (optname) {
        case LINUX_TCP_NODELAY:
            return SOCK_OPT_TCP_NODELAY;
        case LINUX_TCP_KEEPIDLE:
            return SOCK_OPT_TCP_KEEPIDLE;
        case LINUX_TCP_KEEPCNT:
            return SOCK_OPT_TCP_KEEPCNT;
        case LINUX_TCP_KEEPINTVL:
            return SOCK_OPT_TCP_KEEPINTVL;
        default:
            return -1;
        }
    }
    if (level == LINUX_IPPROTO_IPV6 && optname == LINUX_IPV6_V6ONLY)
        return SOCK_OPT_IPV6_V6ONLY;
    if (level == LINUX_IPPROTO_IP) {
        switch (optname) {
        case LINUX_IP_TOS:
            return SOCK_OPT_IP_TOS;
        case LINUX_IP_TTL:
            return SOCK_OPT_IP_TTL;
        case LINUX_IP_HDRINCL:
            return SOCK_OPT_IP_HDRINCL;
        case LINUX_IP_PKTINFO:
            return SOCK_OPT_IP_PKTINFO;
        case LINUX_IP_RECVTTL:
            return SOCK_OPT_IP_RECVTTL;
        case LINUX_IP_RECVTOS:
            return SOCK_OPT_IP_RECVTOS;
        case LINUX_IP_MTU_DISCOVER:
            return SOCK_OPT_IP_MTU_DISCOVER;
        default:
            return -1;
        }
    }
    return -1;
}

int net_socket_cached_int_get(int guest_fd, int level, int optname, int *value)
{
    if (level == LINUX_SOL_SOCKET && optname == LINUX_SO_ERROR)
        return 0;

    int idx = net_sock_opt_index_for(level, optname);
    if (idx < 0)
        return 0;
    return net_sock_cache_get(guest_fd, idx, value);
}

int net_socket_cached_int_get_if_generation(int guest_fd,
                                            uint64_t generation,
                                            int level,
                                            int optname,
                                            int *value)
{
    if (level == LINUX_SOL_SOCKET && optname == LINUX_SO_ERROR)
        return 0;

    int idx = net_sock_opt_index_for(level, optname);
    if (idx < 0 || !RANGE_CHECK(guest_fd, 0, FD_TABLE_SIZE) || !value)
        return 0;

    if (thread_is_single_active()) {
        fd_entry_t *entry = &fd_table[guest_fd];
        if (entry->type == FD_SOCKET && entry->generation == generation)
            return sock_opt_get(entry, idx, value);
        return 0;
    }

    pthread_mutex_lock(&fd_lock);
    fd_entry_t *entry = &fd_table[guest_fd];
    bool ok = entry->type == FD_SOCKET && entry->generation == generation &&
              sock_opt_get(entry, idx, value);
    pthread_mutex_unlock(&fd_lock);
    return ok;
}

void net_socket_cached_int_set(int guest_fd, int level, int optname, int value)
{
    if (level == LINUX_SOL_SOCKET && optname == LINUX_SO_ERROR)
        return;

    int idx = net_sock_opt_index_for(level, optname);
    if (idx >= 0)
        net_socket_cache_set_index(guest_fd, idx, value);
}

void net_socket_cache_init_defaults(int guest_fd, int domain, int real_type)
{
    static const int zero_opts[] = {
        SOCK_OPT_KEEPALIVE, SOCK_OPT_REUSEADDR, SOCK_OPT_ACCEPTCONN,
        SOCK_OPT_REUSEPORT, SOCK_OPT_BROADCAST, SOCK_OPT_DONTROUTE,
        SOCK_OPT_OOBINLINE, SOCK_OPT_PASSCRED,
    };

    net_socket_cache_set_many_zero(guest_fd, zero_opts, ARRAY_SIZE(zero_opts));

    net_socket_cache_set_index(guest_fd, SOCK_OPT_TYPE, real_type & 0xF);
    if (real_type == SOCK_STREAM &&
        (domain == LINUX_AF_INET || domain == LINUX_AF_INET6))
        net_socket_cache_set_index(guest_fd, SOCK_OPT_TCP_NODELAY, 0);
    if (domain == LINUX_AF_INET6)
        net_socket_cache_set_index(guest_fd, SOCK_OPT_IPV6_V6ONLY, 0);
}

void net_socket_cache_init_accept(int guest_fd, int inherit_passcred)
{
    static const int zero_opts[] = {
        SOCK_OPT_ACCEPTCONN, SOCK_OPT_REUSEPORT, SOCK_OPT_BROADCAST,
        SOCK_OPT_DONTROUTE,  SOCK_OPT_OOBINLINE,
    };

    net_socket_cache_set_many_zero(guest_fd, zero_opts, ARRAY_SIZE(zero_opts));

    /* AF_UNIX accept inherits SO_PASSCRED from the listener. For local
     * connects the accept path receives the value captured when the
     * connection was queued; otherwise it falls back to the listener value.
     */
    net_socket_cache_set_index(guest_fd, SOCK_OPT_PASSCRED,
                               inherit_passcred ? 1 : 0);
}
