import math, struct, sys
# usage: tonecheck.py file.pcap ; lists each stretch of isochronous OUT packets in a QEMU usb pcap
# (48 kHz 16 bit stereo) and says whether it is, sample for sample, the tone sndtest plays at 48 kHz:
# sample n is 12000*sin(2*pi*n/48) on the left and 12000*sin(2*pi*n/16) on the right.
data = open(sys.argv[1], 'rb').read()
pos, runs, failed = 24, [], 0
while pos + 16 <= len(data):
    sec, usec, caplen, origlen = struct.unpack_from('<IIII', data, pos); pos += 16
    rec = data[pos:pos + caplen]; pos += caplen
    if len(rec) < 64: continue
    rtype, xfer, ep = rec[8], rec[9], rec[10]
    status, = struct.unpack_from('<i', rec, 28)
    length, len_cap = struct.unpack_from('<II', rec, 32)
    if xfer != 0 or ep & 0x80: continue
    if rtype != ord('S'):
        failed += status != 0; continue
    stamp = sec * 1000000 + usec
    if not runs or stamp - runs[-1]['last'] > 100000:
        runs.append({'first': stamp, 'last': stamp, 'packets': 0, 'sizes': {}, 'gap': 0, 'samples': []})
    run = runs[-1]
    run['gap'] = max(run['gap'], stamp - run['last']); run['last'] = stamp; run['packets'] += 1
    payload = rec[64:64 + len_cap]
    run['sizes'][len(payload)] = run['sizes'].get(len(payload), 0) + 1
    run['samples'] += list(struct.iter_unpack('<hh', payload[:len(payload) & ~3]))
PI = 3.14159265358979
def tone(n): return (int(12000.0 * math.sin(n * 2.0 * PI / 48.0)), int(12000.0 * math.sin(n * 2.0 * PI / 16.0)))
def hz(x):
    z = sum(1 for a, b in zip(x, x[1:]) if a < 0 <= b); return round(z * 48000 / len(x)) if x else 0
print('failed packets', failed)
for i, run in enumerate(runs):
    s = run['samples']
    exact = sum(1 for n, v in enumerate(s) if v == tone(n))
    what = 'the test tone, every sample identical' if exact == len(s) else 'other sound (left about %d Hz, right about %d Hz, peak %d)' % (hz([l for l, r in s]), hz([r for l, r in s]), max(abs(l) for l, r in s) if s else 0)
    print('stretch %d: %d packets %s, %.3f s of sound sent in %.3f s, longest gap %.2f ms: %s' % (i + 1, run['packets'], run['sizes'], len(s) / 48000, (run['last'] - run['first']) / 1e6 + 0.001, run['gap'] / 1000, what))
