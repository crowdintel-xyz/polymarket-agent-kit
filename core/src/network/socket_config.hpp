// ─────────────────────────────────────────────────────────────────────────────
// socket_config.hpp — Phase 1.2: Network Keep-Alive & TCP_NODELAY
// 
// PERF: Enables TCP keepalive with optimal parameters for trading bots
// Impact: Reduces reconnection detection from 2h → 90s, prevents stale sockets
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <sys/socket.h>
#include <netinet/in.h>    // IPPROTO_TCP
#include <netinet/tcp.h>   // TCP_NODELAY, TCP_KEEPIDLE, TCP_KEEPINTVL, TCP_KEEPCNT
#include <unistd.h>        // close()

namespace net {

// PERF: Keepalive parameters tuned for trading workloads
// idle=60s: Start keepalive after 60s of idle (balances false positives)
// interval=10s: Probe every 10s (detects failures quickly, total ~90s)
// probes=3: Max 3 probes before giving up
inline void apply_keepalive(int fd) noexcept {
    // PERF: TCP_NODELAY disables Nagle's algorithm — critical for sub-ms RTT
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    
    // PERF: TCP_QUICKACK for immediate ACKs (reduces latency by 1-2ms)
#ifdef TCP_QUICKACK
    setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &flag, sizeof(flag));
#endif
    
    // PERF: Keepalive settings — detect stale connections fast
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));
    int idle = 60;      // 60s idle before first probe
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    int interval = 10;  // 10s between probes
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    int probes = 3;     // 3 probes max
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &probes, sizeof(probes));
}

}  // namespace net
