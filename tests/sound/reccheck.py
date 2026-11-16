import struct, sys
# usage: reccheck.py rec.raw ; a recording of the test microphone: left counts up by one, right is its complement
d = open(sys.argv[1], 'rb').read(); n = len(d) // 4
s = list(struct.iter_unpack('<HH', d[:n * 4]))
bad = sum(1 for l, r in s if r != l ^ 0xffff)
breaks = [(i, s[i - 1][0], s[i][0]) for i in range(1, n) if (s[i][0] - s[i - 1][0]) & 0xffff != 1]
lost = sum(((b - a - 1) & 0xffff) for i, a, b in breaks)
print('%d samples (%.3f s), not the microphone\'s pattern: %d, breaks in the count: %d, samples missing at them: %d' % (n, n / 48000, bad, len(breaks), lost))
for i, a, b in breaks[:6]: print('  break at sample %d (%.3f s): %d -> %d' % (i, i / 48000, a, b))
