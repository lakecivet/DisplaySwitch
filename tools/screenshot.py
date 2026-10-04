# -*- coding: utf-8 -*-
"""
把当前屏幕截图保存为 PNG（纯标准库 + ctypes，用来人工核对界面效果）。
用法: python tools/screenshot.py [输出文件] [目标窗口标题关键字]
"""
import ctypes
import ctypes.wintypes as wt
import os
import struct
import sys
import zlib

user32 = ctypes.WinDLL('user32', use_last_error=True)
gdi32 = ctypes.WinDLL('gdi32', use_last_error=True)

SRCCOPY = 0x00CC0020
DIB_RGB_COLORS = 0


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ('biSize', wt.DWORD), ('biWidth', wt.LONG), ('biHeight', wt.LONG),
        ('biPlanes', wt.WORD), ('biBitCount', wt.WORD), ('biCompression', wt.DWORD),
        ('biSizeImage', wt.DWORD), ('biXPelsPerMeter', wt.LONG),
        ('biYPelsPerMeter', wt.LONG), ('biClrUsed', wt.DWORD), ('biClrImportant', wt.DWORD),
    ]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [('bmiHeader', BITMAPINFOHEADER), ('bmiColors', wt.DWORD * 3)]


def find_window(keyword):
    """按标题关键字找窗口（含子串匹配），返回 hwnd。"""
    result = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, _):
        n = user32.GetWindowTextLengthW(hwnd)
        if n > 0:
            buf = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(hwnd, buf, n + 1)
            if keyword.lower() in buf.value.lower() and user32.IsWindowVisible(hwnd):
                result.append((hwnd, buf.value))
        return True

    user32.EnumWindows(cb, 0)
    return result


def grab(rect):
    """rect = (l, t, r, b)，返回 (w, h, BGRA bytes top-down)。"""
    l, t, r, b = rect
    w, h = r - l, b - t
    hdc = user32.GetDC(0)
    mem = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(mem, bmp)
    gdi32.BitBlt(mem, 0, 0, w, h, hdc, l, t, SRCCOPY)

    bi = BITMAPINFO()
    bi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.bmiHeader.biWidth = w
    bi.bmiHeader.biHeight = -h          # 负数 = top-down
    bi.bmiHeader.biPlanes = 1
    bi.bmiHeader.biBitCount = 32
    bi.bmiHeader.biCompression = 0

    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bi), DIB_RGB_COLORS)

    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mem)
    user32.ReleaseDC(0, hdc)
    return w, h, buf.raw


def write_png(path, w, h, bgra):
    raw = bytearray()
    for j in range(h):
        raw.append(0)
        row = bgra[j * w * 4:(j + 1) * w * 4]
        for i in range(w):
            o = i * 4
            raw += bytes((row[o + 2], row[o + 1], row[o]))

    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw), 6))
    png += chunk(b'IEND', b'')
    with open(path, 'wb') as f:
        f.write(png)


def main():
    # 让本进程 DPI 感知，这样拿到的坐标都是真实物理像素（不混用单位）
    try:
        ctypes.WinDLL('shcore').SetProcessDpiAwareness(2)
    except OSError:
        try:
            user32.SetProcessDPIAware()
        except OSError:
            pass

    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'shot.png')
    keyword = sys.argv[2] if len(sys.argv) > 2 else ''

    rect = None
    if keyword:
        found = find_window(keyword)
        if found:
            hwnd = found[0][0]
            user32.SetForegroundWindow(hwnd)
            r = wt.RECT()
            # 用 DWM 扩展边界拿到含阴影/标题栏的完整矩形
            try:
                dwm = ctypes.WinDLL('dwmapi')
                DWMWA_EXTENDED_FRAME_BOUNDS = 9
                if dwm.DwmGetWindowAttribute(wt.HWND(hwnd), DWMWA_EXTENDED_FRAME_BOUNDS,
                                             ctypes.byref(r), ctypes.sizeof(r)) != 0:
                    user32.GetWindowRect(hwnd, ctypes.byref(r))
            except OSError:
                user32.GetWindowRect(hwnd, ctypes.byref(r))
            rect = (r.left - 8, r.top - 8, r.right + 8, r.bottom + 8)
            print('window found:', found[0][1], rect)

    if rect is None:
        vx = user32.GetSystemMetrics(76)
        vy = user32.GetSystemMetrics(77)
        vw = user32.GetSystemMetrics(78)
        vh = user32.GetSystemMetrics(79)
        rect = (vx, vy, vx + vw, vy + vh)
        print('full screen:', rect)

    # 裁剪到虚拟屏幕范围内
    vx = user32.GetSystemMetrics(76); vy = user32.GetSystemMetrics(77)
    vw = user32.GetSystemMetrics(78); vh = user32.GetSystemMetrics(79)
    l = max(rect[0], vx); t = max(rect[1], vy)
    r = min(rect[2], vx + vw); b = min(rect[3], vy + vh)
    w, h, bgra = grab((l, t, r, b))
    write_png(out, w, h, bgra)
    print('saved %s (%dx%d)' % (out, w, h))


if __name__ == '__main__':
    main()
