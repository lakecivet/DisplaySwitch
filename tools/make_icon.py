# -*- coding: utf-8 -*-
"""
生成 DisplaySwitch 的程序图标 icon.ico
--------------------------------------------------
纯标准库实现（不依赖 Pillow）：
  1) 用几何覆盖度算法在 256x256 上渲染一张 BGRA 位图，
  2) 逐级盒式降采样得到 16/24/32/48/64/128/256，
  3) 按 ICO 规范打包成 BMP(DIB) 条目。
图形语义：蓝色圆角方块 + 白色水平双向箭头（表示"画面被拉伸"）。
"""
import os
import struct

# --------------------------------------------------------------------------
# 配色
# --------------------------------------------------------------------------
BG_TOP = (0x4A, 0x8B, 0xF5)      # 顶部浅蓝
BG_BOT = (0x1A, 0x3F, 0xA8)      # 底部深蓝
FG     = (0xFF, 0xFF, 0xFF)      # 前景白

RADIUS = 0.215                   # 圆角半径（归一化）
SS     = 4                       # 每个像素的超采样边长


def in_rounded_rect(x, y, r):
    """点 (x,y) 在归一化 [0,1]^2 的单位圆角方块内？"""
    cx = min(max(x, r), 1.0 - r)
    cy = min(max(y, r), 1.0 - r)
    if x >= r and x <= 1.0 - r:
        return 0.0 <= y <= 1.0
    if y >= r and y <= 1.0 - r:
        return 0.0 <= x <= 1.0
    dx, dy = x - cx, y - cy
    return (dx * dx + dy * dy) <= r * r


def in_triangle(px, py, a, b, c):
    def cross(o, p, q):
        return (p[0] - o[0]) * (q[1] - o[1]) - (p[1] - o[1]) * (q[0] - o[0])
    d1 = cross(a, b, (px, py))
    d2 = cross(b, c, (px, py))
    d3 = cross(c, a, (px, py))
    neg = (d1 < 0) or (d2 < 0) or (d3 < 0)
    pos = (d1 > 0) or (d2 > 0) or (d3 > 0)
    return not (neg and pos)


# 水平双向箭头（归一化坐标）
SHAFT_X0, SHAFT_X1 = 0.255, 0.745
SHAFT_Y0, SHAFT_Y1 = 0.437, 0.563
TIP_L, TIP_R = 0.150, 0.850
HEAD_Y0, HEAD_Y1 = 0.285, 0.715
HEAD_X_L, HEAD_X_R = 0.345, 0.655

TRI_L = ((TIP_L, 0.5), (HEAD_X_L, HEAD_Y0), (HEAD_X_L, HEAD_Y1))
TRI_R = ((TIP_R, 0.5), (HEAD_X_R, HEAD_Y0), (HEAD_X_R, HEAD_Y1))


def in_arrow(x, y):
    if SHAFT_X0 <= x <= SHAFT_X1 and SHAFT_Y0 <= y <= SHAFT_Y1:
        return True
    if in_triangle(x, y, *TRI_L):
        return True
    if in_triangle(x, y, *TRI_R):
        return True
    return False


def render(size):
    """返回 size*size 的 BGRA 字节串（top-down，未预乘）。"""
    px = bytearray(size * size * 4)
    inv = 1.0 / size
    for j in range(size):
        for i in range(size):
            # 超采样累计
            bg_cov = 0.0
            fg_cov = 0.0
            for sj in range(SS):
                for si in range(SS):
                    x = (i + (si + 0.5) / SS) * inv
                    y = (j + (sj + 0.5) / SS) * inv
                    if in_rounded_rect(x, y, RADIUS):
                        bg_cov += 1.0
                        if in_arrow(x, y):
                            fg_cov += 1.0
            n = SS * SS
            bg_cov /= n
            fg_cov /= n
            if bg_cov <= 0.0001:
                continue
            # 背景渐变色（按纵向位置）
            t = (j + 0.5) * inv
            br = int(BG_TOP[0] + (BG_BOT[0] - BG_TOP[0]) * t)
            bg = int(BG_TOP[1] + (BG_BOT[1] - BG_TOP[1]) * t)
            bb = int(BG_TOP[2] + (BG_BOT[2] - BG_TOP[2]) * t)
            # 混合前景
            k = fg_cov / bg_cov if bg_cov > 0 else 0.0
            r = int(br + (FG[0] - br) * k)
            g = int(bg + (FG[1] - bg) * k)
            b = int(bb + (FG[2] - bb) * k)
            a = int(round(bg_cov * 255))
            o = (j * size + i) * 4
            px[o + 0] = max(0, min(255, b))   # B
            px[o + 1] = max(0, min(255, g))   # G
            px[o + 2] = max(0, min(255, r))   # R
            px[o + 3] = a                     # A
    return bytes(px)


def make_dib(pixels, size):
    """BGRA(top-down) -> ICO 里的 BMP(DIB) 数据块。"""
    # BITMAPINFOHEADER：高度写两倍（XOR + AND）
    header = struct.pack('<IiiHHIIiiII',
                         40, size, size * 2, 1, 32, 0,
                         size * size * 4, 0, 0, 0, 0)
    # XOR 位图：DIB 是 bottom-up
    rows = []
    for j in range(size - 1, -1, -1):
        rows.append(pixels[j * size * 4:(j + 1) * size * 4])
    xor = b''.join(rows)
    # AND 掩码：1bpp，每行补齐到 4 字节
    and_row = ((size + 31) // 32) * 4
    and_mask = b'\x00' * (and_row * size)
    return header + xor + and_mask


def build_ico(path, sizes):
    images = []
    for s in sizes:
        pixels = render(s)
        data = make_dib(pixels, s)
        images.append((s, data))

    out = bytearray()
    out += struct.pack('<HHH', 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for s, data in images:
        w = 0 if s >= 256 else s
        h = 0 if s >= 256 else s
        out += struct.pack('<BBBBHHII',
                           w, h, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    for _, data in images:
        out += data

    with open(path, 'wb') as f:
        f.write(bytes(out))
    return len(out)


def write_png(path, pixels, size):
    """把 BGRA(top-down) 写成 PNG，仅用于人工预览。"""
    import zlib
    raw = bytearray()
    for j in range(size):
        raw.append(0)                                  # filter type 0
        row = pixels[j * size * 4:(j + 1) * size * 4]
        for i in range(size):
            o = i * 4
            raw += bytes((row[o + 2], row[o + 1], row[o], row[o + 3]))

    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw), 9))
    png += chunk(b'IEND', b'')
    with open(path, 'wb') as f:
        f.write(png)


if __name__ == '__main__':
    here = os.path.dirname(os.path.abspath(__file__))
    src = os.path.join(os.path.dirname(here), 'src')
    os.makedirs(src, exist_ok=True)
    dst = os.path.join(src, 'icon.ico')
    n = build_ico(dst, [16, 24, 32, 48, 64, 128, 256])
    print('icon.ico written: %s (%d bytes)' % (dst, n))

    # 预览图（方便人工检查）
    prev = os.path.join(os.path.dirname(here), 'icon-preview.png')
    write_png(prev, render(256), 256)
    write_png(os.path.join(os.path.dirname(here), 'icon-preview-32.png'), render(32), 32)
    print('preview written: %s' % prev)
