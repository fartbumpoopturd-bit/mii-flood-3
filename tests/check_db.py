#!/usr/bin/env python3
"""Independent re-parse of the files written by test_core (does not share code
with rflcore.c): walks the parade linked list and checks record/node agreement."""
import struct, sys

SIZE = 0xBB5E0

def check(path, expect_count, expect_first=None, expect_last=None):
    d = open(path, 'rb').read()
    assert len(d) == SIZE, len(d)
    assert d[0:4] == b'RNOD' and d[0x1D00:0x1D04] == b'RNHD'
    head, tail = struct.unpack('>hh', d[0x1D04:0x1D08])
    nodes = d[0x1D08:0x1D08 + 120000]
    seen, cur, prev, order = set(), head, -1, []
    while cur != -1:
        assert cur not in seen
        seen.add(cur)
        nid = nodes[cur*12:cur*12+8]
        w1, w2 = struct.unpack('>HH', nodes[cur*12+8:cur*12+12])
        nxt = w1 & 0x7FFF; nxt = -1 if nxt == 0x7FFF else nxt
        prv = w2 & 0x7FFF; prv = -1 if prv == 0x7FFF else prv
        assert prv == prev, (cur, prv, prev)
        rec = d[0x1F1E0 + cur*64: 0x1F1E0 + cur*64 + 64]
        assert rec[0x18:0x20] == nid, 'node id != record id'
        assert (w1 >> 15) == ((rec[0] >> 6) & 1), 'sex bit'
        assert rec[0x36:0x40] == b'\0'*10
        order.append((struct.unpack(">Q", nid)[0] >> 32) & 0x0FFFFFFF)
        prev, cur = cur, nxt
    assert prev == tail
    assert len(order) == expect_count, len(order)
    assert len(set(order)) == len(order), 'duplicate ids in parade'
    if expect_first is not None: assert order[0] == expect_first
    if expect_last is not None: assert order[-1] == expect_last
    # unused nodes must be pristine
    for i in range(10000):
        if i not in seen:
            assert nodes[i*12:i*12+8] == b'\0'*8
    print(path, 'OK', len(order), 'entries')

base = sys.argv[1]
check(base + '/before.dat', 0)
check(base + '/full.dat', 10000, 1, 10000)
check(base + '/evicted.dat', 10000, 501, 20500)
