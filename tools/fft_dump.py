#!/usr/bin/env python3
"""Listen to WLED audio-sync multicast and dump per-bin FFT stats."""
import socket, struct, sys, time

GRP, PORT = "239.0.0.1", 11988
DUR = float(sys.argv[1]) if len(sys.argv) > 1 else 20

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("", PORT))
s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
             struct.pack("4sl", socket.inet_aton(GRP), socket.INADDR_ANY))
s.settimeout(2)

fmt = "<6s2xffBB16B2xff"   # v2 audioSyncPacket, 44 bytes
n, sums, maxs = 0, [0]*16, [0]*16
t0 = time.time()
while time.time() - t0 < DUR:
    try:
        data, _ = s.recvfrom(128)
    except socket.timeout:
        continue
    if len(data) < 44 or data[:5] != b"00002":
        continue
    u = struct.unpack(fmt, data[:44])
    bins = u[5:21]
    n += 1
    for i, v in enumerate(bins):
        sums[i] += v
        maxs[i] = max(maxs[i], v)

print(f"packets: {n}")
if n:
    print("bin :", " ".join(f"{i:>4}" for i in range(16)))
    print("mean:", " ".join(f"{sums[i]/n:4.0f}" for i in range(16)))
    print("max :", " ".join(f"{maxs[i]:4}" for i in range(16)))
