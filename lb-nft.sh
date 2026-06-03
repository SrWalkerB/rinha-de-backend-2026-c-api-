#!/bin/sh
# Round-robin L4 load balancer with automatic kernel/userspace selection.
#   args: <listen_port> <api1> <p1> <api2> <p2>
#
# Strategy 1 (preferred): iptables DNAT + conntrack. New connections to :PORT are
#   forwarded alternately to the two backends; conntrack pins each connection to
#   its backend for its lifetime; MASQUERADE makes the backends reply through us.
#   Forwarding happens in the kernel (softirq), so the LB cgroup spends ~no CPU.
# Strategy 2 (fallback): the userspace epoll relay /app/lb, used when iptables
#   cannot be programmed (NET_ADMIN unavailable). Slower but always works.
# Neither strategy ever reads request payloads — pure round-robin forwarding.
set -e
PORT="${1:-9999}"
H1="${2:-api1}"; P1="${3:-8001}"
H2="${4:-api2}"; P2="${5:-8001}"

resolve() { getent hosts "$1" | awk '{print $1; exit}'; }

A1=""; A2=""; i=0
while [ -z "$A1" ] || [ -z "$A2" ]; do
    A1=$(resolve "$H1"); A2=$(resolve "$H2")
    i=$((i+1)); [ "$i" -gt 120 ] && { echo "lb: cannot resolve backends" >&2; exit 1; }
    [ -z "$A1" ] || [ -z "$A2" ] && sleep 0.5
done

fallback() {
    echo "lb: falling back to userspace relay (/app/lb)" >&2
    exec /app/lb "$PORT" "$H1" "$P1" "$H2" "$P2"
}

# try the kernel path; any failure -> userspace relay
sysctl -w net.ipv4.ip_forward=1 >/dev/null 2>&1 || true
if iptables -t nat -A PREROUTING -p tcp --dport "$PORT" \
        -m statistic --mode nth --every 2 --packet 0 -j DNAT --to-destination "$A1:$P1" 2>/dev/null; then
    iptables -t nat -A PREROUTING -p tcp --dport "$PORT" -j DNAT --to-destination "$A2:$P2"
    iptables -t nat -A OUTPUT -p tcp --dport "$PORT" \
        -m statistic --mode nth --every 2 --packet 0 -j DNAT --to-destination "$A1:$P1"
    iptables -t nat -A OUTPUT -p tcp --dport "$PORT" -j DNAT --to-destination "$A2:$P2"
    iptables -t nat -A POSTROUTING -j MASQUERADE
    echo "lb: kernel DNAT active :$PORT -> $A1:$P1 , $A2:$P2"
    exec sleep infinity
else
    fallback
fi
