# -*- coding: utf-8 -*-
"""PNG 小工具：读取像素、裁剪、放大，方便核对界面截图。
用法:
  python tools/pngtool.py scan  <png> <x> <y0> <y1> [step]   垂直扫描一列颜色
  python tools/pngtool.py crop  <png> <out> <x> <y> <w> <h>   裁剪
  python tools/pngtool.py zoom  <png> <out> <factor>          整图放大
"""
import os
import struct
import sys
import zlib


def load_png(path):
    p = open(path, 'rb').read()
    pos = 8
    w = h = ct = 0
    idat = b''
    plte = None
    trns = None
    while pos < len(p):
        ln = struct.unpack('>I', p[pos:pos + 4])[0]
        tag = p[pos + 4:pos + 8]
        data = p[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if tag == b'IHDR':
            w, h, bd, ct, comp, filt, inter = struct.unpack('>IIBBBBB', data[:13])
            if bd != 8:
                raise SystemExit('only 8-bit png supported, got %d' % bd)
        elif tag == b'PLTE':
            plte = data
        elif tag == b'tRNS':
            trns = data
        elif tag == b'IDAT':
            idat += data
        elif tag == b'IEND':
            break

    if ct == 0:
        nch = 1
    elif ct == 2:
        nch = 3
    elif ct == 3:
        nch = 1
    elif ct == 4:
        nch = 2
    elif ct == 6:
        nch = 4
    else:
        raise SystemExit('unsupported color type %d' % ct)

    raw = zlib.decompress(idat)
    stride = w * nch
    rows = []
    prev = bytearray(stride)
    i = 0
    for _ in range(h):
        f = raw[i]
        i += 1
        line = bytearray(raw[i:i + stride])
        i += stride
        if f == 1:
            for x in range(nch, stride):
                line[x] = (line[x] + line[x - nch]) & 255
        elif f == 2:
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 255
        elif f == 3:
            for x in range(stride):
                a = line[x - nch] if x >= nch else 0
                line[x] = (line[x] + ((a + prev[x]) >> 1)) & 255
        elif f == 4:
            for x in range(stride):
                a = line[x - nch] if x >= nch else 0
                b = prev[x]
                c = prev[x - nch] if x >= nch else 0
                pp = a + b - c
                pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 255
        rows.append(bytes(line))
        prev = line
    return w, h, nch, rows


def to_rgb(nch, row, x, plte=None):
    o = x * nch
    if nch == 3:
        return (row[o], row[o + 1], row[o + 2])
    if nch == 4:
        return (row[o], row[o + 1], row[o + 2])
    if nch == 1:
        v = row[o]
        if plte:
            return (plte[v * 3], plte[v * 3 + 1], plte[v * 3 + 2])
        return (v, v, v)
    return (row[o], row[o], row[o])


def write_png(path, w, h, rgb_rows):
    raw = bytearray()
    for r in rgb_rows:
        raw.append(0)
        raw += r

    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw), 6))
    png += chunk(b'IEND', b'')
    open(path, 'wb').write(png)


def main():
    cmd = sys.argv[1]
    if cmd == 'scan':
        path, x, y0, y1 = sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
        step = int(sys.argv[6]) if len(sys.argv) > 6 else 4
        w, h, nch, rows = load_png(path)
        print('png %dx%d ch=%d' % (w, h, nch))
        for y in range(y0, min(y1, h), step):
            print(y, to_rgb(nch, rows[y], x))
    elif cmd == 'crop':
        path, out, x, y, cw, ch = sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6]), int(sys.argv[7])
        w, h, nch, rows = load_png(path)
        out_rows = []
        for j in range(y, min(y + ch, h)):
            line = bytearray()
            for i in range(x, min(x + cw, w)):
                line += bytes(to_rgb(nch, rows[j], i))
            out_rows.append(bytes(line))
        write_png(out, min(x + cw, w) - x, len(out_rows), out_rows)
        print('cropped ->', out)
    elif cmd == 'zoom':
        path, out, f = sys.argv[2], sys.argv[3], int(sys.argv[4])
        w, h, nch, rows = load_png(path)
        out_rows = []
        for j in range(h):
            line = bytearray()
            for i in range(w):
                line += bytes(to_rgb(nch, rows[j], i)) * f
            for _ in range(f):
                out_rows.append(bytes(line))
        write_png(out, w * f, h * f, out_rows)
        print('zoomed ->', out, '%dx%d' % (w * f, h * f))
    else:
        print(__doc__)


if __name__ == '__main__':
    main()
