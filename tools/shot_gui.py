# -*- coding: utf-8 -*-
"""
在同一个进程里「启动 DisplaySwitch.exe -> 等窗口 -> 截图 -> 保存 PNG」。

为什么不能直接双击/用 start 启动后再截图：
    WorkBuddy 的 bash 后台启动会把进程放进隔离的 window station，
    那边的窗口在本进程桌面里既看不见也截不到。
    用 ShellExecuteW 启动 + 同进程内立即截图，才能拿到真实界面。

用法:
    python tools/shot_gui.py [输出路径] [等待秒数]
"""
import ctypes
import ctypes.wintypes as wt
import os
import struct
import sys
import time
import zlib

user32 = ctypes.WinDLL('user32', use_last_error=True)
gdi32 = ctypes.WinDLL('gdi32', use_last_error=True)

EXE = r'D:\DisplaySwitch\DisplaySwitch.exe'
WORKDIR = r'D:\DisplaySwitch'


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ('biSize', wt.DWORD), ('biWidth', wt.LONG), ('biHeight', wt.LONG),
        ('biPlanes', wt.WORD), ('biBitCount', wt.WORD), ('biCompression', wt.DWORD),
        ('biSizeImage', wt.DWORD), ('biXPelsPerMeter', wt.LONG),
        ('biYPelsPerMeter', wt.LONG), ('biClrUsed', wt.DWORD), ('biClrImportant', wt.DWORD),
    ]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [('bmiHeader', BITMAPINFOHEADER), ('bmiColors', wt.DWORD * 3)]


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
    out = sys.argv[1] if len(sys.argv) > 1 else r'D:\DisplaySwitch\shot-new.png'
    wait = float(sys.argv[2]) if len(sys.argv) > 2 else 2.5

    try:
        ctypes.WinDLL('shcore').SetProcessDpiAwareness(2)
    except OSError:
        user32.SetProcessDPIAware()

    # 先在进程里杀掉旧实例，避免单实例 Mutex 让新窗口不显示
    os.system('taskkill /F /IM DisplaySwitch.exe >nul 2>nul')
    time.sleep(0.8)

    ctypes.windll.shell32.ShellExecuteW(None, 'open', EXE, None, WORKDIR, 1)

    hwnd = 0
    for _ in range(60):
        time.sleep(0.25)
        hwnd = user32.FindWindowW('DisplaySwitchWnd', None)
        if hwnd:
            break
    if not hwnd:
        print('ERROR: window not found')
        return 1
    time.sleep(wait)

    r = wt.RECT()
    try:
        dwm = ctypes.WinDLL('dwmapi')
        DWMWA_EXTENDED_FRAME_BOUNDS = 9
        if dwm.DwmGetWindowAttribute(wt.HWND(hwnd), DWMWA_EXTENDED_FRAME_BOUNDS,
                                     ctypes.byref(r), ctypes.sizeof(r)) != 0:
            user32.GetWindowRect(hwnd, ctypes.byref(r))
    except OSError:
        user32.GetWindowRect(hwnd, ctypes.byref(r))

    l, t = r.left - 8, r.top - 8
    rr, bb = r.right + 8, r.bottom + 8
    w, h = rr - l, bb - t

    hdc = user32.GetDC(0)
    mem = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(mem, bmp)
    gdi32.BitBlt(mem, 0, 0, w, h, hdc, l, t, 0x00CC0020)

    bi = BITMAPINFO()
    bi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.bmiHeader.biWidth = w
    bi.bmiHeader.biHeight = -h
    bi.bmiHeader.biPlanes = 1
    bi.bmiHeader.biBitCount = 32
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bi), 0)

    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mem)
    user32.ReleaseDC(0, hdc)

    write_png(out, w, h, buf.raw)
    print('saved %s (%dx%d) hwnd=%d' % (out, w, h, hwnd))
    return 0


if __name__ == '__main__':
    sys.exit(main())
