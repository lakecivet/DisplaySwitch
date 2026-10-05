// =============================================================================
//  main_gui.cpp  --  Win32 图形界面 (DisplaySwitch.exe)  ·  美化版
// -----------------------------------------------------------------------------
//  纯 Win32 API + comctl32，零第三方依赖。核心逻辑全部来自 core.cpp，
//  本文件只负责「界面 + 事件 -> 调用核心 -> 把日志回显到窗口」。
//
//  设计要点：
//    · 自绘按钮（主 / 次 / 危险 三档），圆角 + 悬停反馈 + 禁用态
//    · 卡片式分组（圆角白卡 + 分组标题），替代原先的控件平铺
//    · 顶部状态横幅：状态圆点 + 大标题 + 副标题
//    · 双缓冲绘制，消除闪烁
//
//  命令行：
//      DisplaySwitch.exe                          正常打开图形界面
//      DisplaySwitch.exe --op stretch|daily|...   执行一次后自动退出（供提权子进程使用）
//      DisplaySwitch.exe --elevated               内部标记：本次已被提权拉起
// =============================================================================
#include "core.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// 主题色（浅色 · 中性灰 + 单一强调蓝）
// ---------------------------------------------------------------------------
#define COL_BG          RGB(0xF5, 0xF6, 0xF8)   // 窗口底
#define COL_CARD        RGB(0xFF, 0xFF, 0xFF)   // 卡片底
#define COL_CARD_LINE   RGB(0xE6, 0xE9, 0xEE)   // 卡片描边
#define COL_HDR         RGB(0x6B, 0x72, 0x7C)   // 分组标题（小字大写风）
#define COL_TEXT        RGB(0x1C, 0x1F, 0x24)
#define COL_SUBTEXT     RGB(0x64, 0x6C, 0x77)
#define COL_MUTED       RGB(0x9A, 0xA2, 0xAE)
#define COL_LINE        RGB(0xE8, 0xEB, 0xF0)
#define COL_ACCENT      RGB(0x2B, 0x6B, 0xE8)   // 强调蓝
#define COL_ACCENT_HI   RGB(0x4A, 0x84, 0xF5)
#define COL_ACCENT_LO   RGB(0x1E, 0x56, 0xC8)
#define COL_ACCENT_TX   RGB(0xFF, 0xFF, 0xFF)

#define COL_BTN_BG      RGB(0xFF, 0xFF, 0xFF)
#define COL_BTN_HOVER   RGB(0xF1, 0xF4, 0xF9)
#define COL_BTN_DOWN    RGB(0xE4, 0xE9, 0xF2)
#define COL_BTN_LINE    RGB(0xD5, 0xDA, 0xE2)
#define COL_BTN_DIS_TX  RGB(0xAD, 0xB4, 0xBE)
#define COL_BTN_DIS_BG  RGB(0xF4, 0xF5, 0xF7)

#define COL_WARN        RGB(0xD1, 0x4B, 0x2F)   // 警示橙红
#define COL_WARN_HOVER  RGB(0xE8, 0x62, 0x44)
#define COL_WARN_LINE   RGB(0xEC, 0xB4, 0xA6)

// 状态色
#define COL_STRETCH     RGB(0x2B, 0x6B, 0xE8)
#define COL_STRETCH_BG  RGB(0xED, 0xF3, 0xFE)
#define COL_STRETCH_TX  RGB(0x17, 0x45, 0xB0)
#define COL_DAILY       RGB(0x18, 0x9A, 0x5B)
#define COL_DAILY_BG    RGB(0xEC, 0xF7, 0xF1)
#define COL_DAILY_TX    RGB(0x0E, 0x6B, 0x3E)
#define COL_UNKNOWN     RGB(0x8A, 0x92, 0x9E)
#define COL_UNKNOWN_BG  RGB(0xF2, 0xF4, 0xF6)
#define COL_UNKNOWN_TX  RGB(0x44, 0x4C, 0x56)

// ---------------------------------------------------------------------------
// 控件 ID
// ---------------------------------------------------------------------------
enum {
    IDC_BANNER_TITLE = 1000,
    IDC_BANNER_SUB,
    IDC_BTN_TOGGLE,
    IDC_BTN_STRETCH,
    IDC_BTN_DAILY,
    IDC_BTN_SAVE,
    IDC_BTN_EMERGENCY,
    IDC_BTN_NVCP,
    IDC_BTN_REFRESH,
    IDC_BTN_ELEVATE,
    IDC_BTN_SAVECFG,
    IDC_BTN_CLEARLOG,
    IDC_BTN_COPYLOG,
    IDC_LV_DISP,
    IDC_LV_MON,
    IDC_LB_MODE,
    IDC_EDIT_W,
    IDC_EDIT_H,
    IDC_EDIT_HZ,
    IDC_EDIT_HOTKEY,
    IDC_CHK_LOG,
    IDC_CHK_ELEV,
    IDC_CHK_DISMON,
    IDC_CHK_ENMON,
    IDC_CHK_NVCP,
    IDC_LBL_DISP,
    IDC_LBL_MON,
    IDC_LBL_MODE,
    IDC_STATIC_HINT,
    IDC_EDIT_LOG,
    IDC_STATUS,
    IDC_ADMIN,
    IDC_LBL_GHOTKEY,
    IDC_LBL_USTATUS
};

#define WM_APP_LOG    (WM_APP + 1)   // wParam=0, lParam=std::wstring*
#define WM_APP_DONE   (WM_APP + 2)   // wParam=退出码
#define WM_APP_TRAY   (WM_APP + 4)
#define HOTKEY_ID     0x5150

// 托盘菜单
enum { IDM_SHOW = 2001, IDM_TOGGLE = 2002, IDM_EXIT = 2003 };

// 按钮档位
enum BtnKind { BK_PRIMARY = 0, BK_NORMAL, BK_DANGER };

// ---------------------------------------------------------------------------
// 全局状态
// ---------------------------------------------------------------------------
static HINSTANCE g_inst   = nullptr;
static HWND      g_hwnd   = nullptr;
static Config    g_cfg;
static bool      g_elevated      = false;
static bool      g_launchedElev = false;
static bool      g_autoExit      = false;   // --op 模式：跑完自动退出
static std::atomic<bool> g_busy(false);
static std::wstring g_mode = L"unknown";    // daily | stretch | unknown
static LRESULT   g_result = 0;

static HFONT g_fUI = nullptr, g_fBold = nullptr, g_fBig = nullptr, g_fSmall = nullptr,
             g_fLog = nullptr, g_fHead = nullptr, g_fMed = nullptr;
static HBRUSH g_brBg = nullptr, g_brCardBg = nullptr;

static HWND hBannerTitle = nullptr, hBannerSub = nullptr;
static HWND hBtnToggle = nullptr, hBtnStretch = nullptr, hBtnDaily = nullptr, hBtnSave = nullptr,
            hBtnEmergency = nullptr, hBtnNvcp = nullptr, hBtnRefresh = nullptr,
            hBtnElevate = nullptr, hBtnSaveCfg = nullptr, hBtnClearLog = nullptr, hBtnCopyLog = nullptr;
static HWND hLvDisp = nullptr, hLvMon = nullptr, hLbMode = nullptr;
static HWND hEditW = nullptr, hEditH = nullptr, hEditHz = nullptr, hEditHotkey = nullptr;
static HWND hChkLog = nullptr, hChkElev = nullptr, hChkDisMon = nullptr, hChkEnMon = nullptr, hChkNvcp = nullptr;
static HWND hLblDisp = nullptr, hLblMon = nullptr, hLblMode = nullptr, hStaticHint = nullptr;
static HWND hLblRes = nullptr, hLblX = nullptr, hLblAt = nullptr, hLblHz = nullptr, hLblHot = nullptr;
static HWND hLog = nullptr, hStatus = nullptr, hAdmin = nullptr;

// 卡片 / 分组标题（自绘，无窗口）
struct Group {
    RECT  rc;            // 卡片矩形（客户区坐标）
    RECT  titleRc;       // 标题文字位置
    std::wstring title;
    std::wstring trailing;   // 卡片右上角的小字（可空）
};
static std::vector<Group> g_groups;

static double g_scale = 1.0;      // 系统 DPI 缩放系数（1.0 = 96 DPI）
static int Sc(int v) { return (int)(v * g_scale + 0.5); }

static NOTIFYICONDATAW g_nid{};
static bool g_trayAdded = false;
static UINT g_hotMods = 0, g_hotVk = 0;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
static void SetWndText(HWND h, const std::wstring& s)
{
    SetWindowTextW(h, s.c_str());
}

static std::wstring GetWndText(HWND h)
{
    int n = GetWindowTextLengthW(h);
    if (n <= 0) return std::wstring();
    std::wstring s((size_t)n + 1, L'\0');
    GetWindowTextW(h, &s[0], n + 1);
    s.resize((size_t)n);
    return s;
}

static HWND MakeStatic(const wchar_t* text, DWORD style)
{
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | style,
                           0, 0, 10, 10, g_hwnd, nullptr, g_inst, nullptr);
}

// 自绘按钮：BS_OWNERDRAW，档位塞在 GWLP_USERDATA
static HWND MakeButton(const wchar_t* text, int id, BtnKind kind = BK_NORMAL)
{
    HWND h = CreateWindowExW(0, L"BUTTON", text,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                             0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)id, g_inst, nullptr);
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)kind);
    return h;
}

static HWND MakeCheck(const wchar_t* text, int id)
{
    return CreateWindowExW(0, L"BUTTON", text,
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                           0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)id, g_inst, nullptr);
}

// ---------------------------------------------------------------------------
// 绘图小工具：圆角矩形 / 填充 / 描边 / 文字
// ---------------------------------------------------------------------------
static void FillRoundRect(HDC dc, const RECT& rc, int r, COLORREF fill)
{
    HBRUSH br = CreateSolidBrush(fill);
    HGDIOBJ oldBr = SelectObject(dc, br);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, r * 2, r * 2);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBr);
    DeleteObject(br);
}

static void StrokeRoundRect(HDC dc, const RECT& rc, int r, COLORREF line, int width = 1)
{
    HPEN pen = CreatePen(PS_SOLID, width, line);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBr = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, r * 2, r * 2);
    SelectObject(dc, oldBr);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

static void DrawCircle(HDC dc, int cx, int cy, int r, COLORREF fill)
{
    HBRUSH br = CreateSolidBrush(fill);
    HGDIOBJ oldBr = SelectObject(dc, br);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBr);
    DeleteObject(br);
}

static void DrawTextAt(HDC dc, const std::wstring& s, RECT rc, HFONT f,
                       COLORREF color, UINT fmt)
{
    HGDIOBJ old = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, s.c_str(), (int)s.size(), &rc, fmt | DT_NOPREFIX);
    SelectObject(dc, old);
}

// ---------------------------------------------------------------------------
// 自绘按钮
// ---------------------------------------------------------------------------
static HWND g_hoverBtn = nullptr;   // 当前鼠标悬停的按钮（用于悬停配色）

static void DrawButton(const DRAWITEMSTRUCT* dis)
{
    HDC dc = dis->hDC;
    RECT rc = dis->rcItem;
    HWND h = dis->hwndItem;
    BtnKind kind = (BtnKind)GetWindowLongPtrW(h, GWLP_USERDATA);
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    bool pressed  = (dis->itemState & ODS_SELECTED) != 0;
    bool focus    = (dis->itemState & ODS_FOCUS) != 0;
    bool hover    = (g_hoverBtn == h) && !disabled;

    // 让文字/边框更利落：先铺卡片底色（父窗口是白卡片）
    FillRect(dc, &rc, g_brCardBg);

    RECT r = rc;
    InflateRect(&r, -Sc(1), -Sc(1));
    int radius = Sc(6);

    std::wstring text = GetWndText(h);

    if (kind == BK_PRIMARY) {
        COLORREF bg = pressed ? COL_ACCENT_LO : (hover ? COL_ACCENT_HI : COL_ACCENT);
        if (disabled) bg = RGB(0xC3, 0xCD, 0xDE);
        FillRoundRect(dc, r, radius, bg);
        DrawTextAt(dc, text, rc, g_fMed, COL_ACCENT_TX,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else if (kind == BK_DANGER) {
        COLORREF bg = COL_BTN_BG, line = COL_WARN_LINE, tx = COL_WARN;
        if (disabled)     { bg = COL_BTN_DIS_BG; line = COL_BTN_LINE; tx = COL_BTN_DIS_TX; }
        else if (pressed) { bg = RGB(0xFB, 0xE9, 0xE5); line = COL_WARN; }
        else if (hover)   { bg = RGB(0xFD, 0xF4, 0xF2); line = COL_WARN_HOVER; }
        FillRoundRect(dc, r, radius, bg);
        StrokeRoundRect(dc, r, radius, line);
        DrawTextAt(dc, text, rc, g_fUI, tx, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        COLORREF bg = COL_BTN_BG, line = COL_BTN_LINE, tx = COL_TEXT;
        if (disabled)     { bg = COL_BTN_DIS_BG; tx = COL_BTN_DIS_TX; line = COL_BTN_LINE; }
        else if (pressed) { bg = COL_BTN_DOWN; line = COL_ACCENT; }
        else if (hover)   { bg = COL_BTN_HOVER; line = RGB(0xBE, 0xC6, 0xD2); }
        FillRoundRect(dc, r, radius, bg);
        StrokeRoundRect(dc, r, radius, line);
        DrawTextAt(dc, text, rc, g_fUI, tx, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    if (focus && !disabled) {
        RECT f = r; InflateRect(&f, -Sc(2), -Sc(2));
        StrokeRoundRect(dc, f, radius - Sc(1), COL_LINE);
    }
}

// 悬停追踪：鼠标下的按钮变化时重绘
static LRESULT CALLBACK ButtonSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                       UINT_PTR, DWORD_PTR)
{
    switch (msg) {
    case WM_MOUSEMOVE:
        if (g_hoverBtn != h) {
            if (g_hoverBtn) { HWND p = g_hoverBtn; g_hoverBtn = nullptr;
                              InvalidateRect(p, nullptr, FALSE); }
            g_hoverBtn = h;
            InvalidateRect(h, nullptr, FALSE);
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
        }
        break;
    case WM_MOUSELEAVE:
        if (g_hoverBtn == h) { g_hoverBtn = nullptr; InvalidateRect(h, nullptr, FALSE); }
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP: {
        // 悬停色跟随按下状态变化
        InvalidateRect(h, nullptr, FALSE);
        break;
    }
    default: break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// ListView 交替行色 + 淡表头（让表格不那么"重"）
#define COL_ROW_ALT  RGB(0xF7, 0xF9, 0xFC)

static LRESULT CALLBACK ListSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                     UINT_PTR, DWORD_PTR)
{
    if (msg == WM_NOTIFY) {
        LPNMHDR nh = (LPNMHDR)lp;
        // 只处理列表本体，放过表头（表头是独立子控件，抢它的通知会把表头画没）
        if (nh->hwndFrom == h && nh->code == NM_CUSTOMDRAW) {
            LPNMLVCUSTOMDRAW cd = (LPNMLVCUSTOMDRAW)lp;
            switch (cd->nmcd.dwDrawStage) {
            case CDDS_PREPAINT:
                return CDRF_NOTIFYITEMDRAW;
            case CDDS_ITEMPREPAINT:
                // 偶数行淡蓝底，奇数行白底
                if ((cd->nmcd.dwItemSpec % 2) == 0)
                    cd->clrTextBk = COL_ROW_ALT;
                else
                    cd->clrTextBk = RGB(0xFF, 0xFF, 0xFF);
                cd->clrText = COL_TEXT;
                return CDRF_NEWFONT;
            default: break;
            }
        }
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// 表头子类：扁平化绘制（去掉系统默认的立体渐变）
static LRESULT CALLBACK HeaderSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                       UINT_PTR, DWORD_PTR)
{
    if (msg == WM_NOTIFY) {
        LPNMHDR nh = (LPNMHDR)lp;
        if (nh->code == NM_CUSTOMDRAW) {
            LPNMCUSTOMDRAW cd = (LPNMCUSTOMDRAW)lp;
            HWND hdr = (HWND)SendMessageW(GetParent(h), LVM_GETHEADER, 0, 0);
            if (cd->hdr.hwndFrom == hdr) {
                switch (cd->dwDrawStage) {
                case CDDS_PREPAINT: {
                    // 表头整体铺一层淡灰底
                    RECT rc;
                    GetClientRect(hdr, &rc);
                    HBRUSH br = CreateSolidBrush(RGB(0xF7, 0xF8, 0xFA));
                    FillRect(cd->hdc, &rc, br);
                    DeleteObject(br);
                    // 底部一条分隔线
                    HPEN pen = CreatePen(PS_SOLID, 1, COL_CARD_LINE);
                    HGDIOBJ op = SelectObject(cd->hdc, pen);
                    MoveToEx(cd->hdc, rc.left, rc.bottom - 1, nullptr);
                    LineTo(cd->hdc, rc.right, rc.bottom - 1);
                    SelectObject(cd->hdc, op);
                    DeleteObject(pen);
                    return CDRF_NOTIFYITEMDRAW;
                }
                case CDDS_ITEMPREPAINT: {
                    SetTextColor(cd->hdc, COL_SUBTEXT);
                    SetBkMode(cd->hdc, TRANSPARENT);
                    return CDRF_NEWFONT;
                }
                default: break;
                }
            }
        }
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// 日志 / 输出重定向
// ---------------------------------------------------------------------------
static void AppendToControl(const std::wstring& line)
{
    if (!hLog) return;
    int len = GetWindowTextLengthW(hLog);
    SendMessageW(hLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    std::wstring out = line + L"\r\n";
    SendMessageW(hLog, EM_REPLACESEL, FALSE, (LPARAM)out.c_str());
    // 太长就砍掉前半段，避免无限增长
    len = GetWindowTextLengthW(hLog);
    if (len > 240000) {
        SendMessageW(hLog, EM_SETSEL, 0, (LPARAM)(len - 160000));
        SendMessageW(hLog, EM_REPLACESEL, FALSE, (LPARAM)L"[... 前面的日志已截断 ...]\r\n");
    }
    SendMessageW(hLog, EM_SETSEL, -1, -1);
    SendMessageW(hLog, EM_SCROLLCARET, 0, 0);
}

// 图形版自己产生的日志：同时进窗口和 DisplaySwitch.log
static void AppendLogLine(const std::wstring& line)
{
    AppendToControl(line);
    AppendLog(line);
}

// 核心代码通过这个回调把日志送进窗口（可能来自工作线程）。
// 注意：核心的 Out() 已经写过日志文件了，这里只负责界面。
static void GuiSink(const std::wstring& line, void* /*user*/)
{
    if (!g_hwnd) return;
    if (GetCurrentThreadId() == GetWindowThreadProcessId(g_hwnd, nullptr)) {
        AppendToControl(line);
    } else {
        PostMessageW(g_hwnd, WM_APP_LOG, 0, (LPARAM)new std::wstring(line));
    }
}

// ---------------------------------------------------------------------------
// 热键字符串解析："Ctrl+Alt+S" -> MOD_CONTROL|MOD_ALT, 'S'
// ---------------------------------------------------------------------------
static UINT KeyNameToVk(std::wstring k)
{
    k = LowerW(k);
    if (k.size() == 1) {
        wchar_t c = k[0];
        if (c >= L'a' && c <= L'z') return (UINT)(towupper(c));
        if (c >= L'0' && c <= L'9') return (UINT)c;
        return 0;
    }
    if (k.size() >= 2 && k[0] == L'f') {
        int n = _wtoi(k.c_str() + 1);
        if (n >= 1 && n <= 24) return VK_F1 + (UINT)(n - 1);
    }
    if (k == L"space")  return VK_SPACE;
    if (k == L"tab")    return VK_TAB;
    if (k == L"enter")  return VK_RETURN;
    if (k == L"home")   return VK_HOME;
    if (k == L"end")    return VK_END;
    return 0;
}

static bool ParseHotkey(std::wstring s, UINT& mods, UINT& vk)
{
    mods = 0; vk = 0;
    if (s.empty()) return false;
    size_t start = 0;
    while (true) {
        size_t p = s.find(L'+', start);
        std::wstring part = TrimW(s.substr(start, p == std::wstring::npos ? std::wstring::npos : p - start));
        std::wstring lp = LowerW(part);
        if (!part.empty()) {
            if      (lp == L"ctrl" || lp == L"control") mods |= MOD_CONTROL;
            else if (lp == L"alt")                      mods |= MOD_ALT;
            else if (lp == L"shift")                    mods |= MOD_SHIFT;
            else if (lp == L"win")                      mods |= MOD_WIN;
            else { UINT v = KeyNameToVk(part); if (v) vk = v; }
        }
        if (p == std::wstring::npos) break;
        start = p + 1;
    }
    return vk != 0;
}

static void RegisterGlobalHotkey()
{
    UnregisterHotKey(g_hwnd, HOTKEY_ID);
    g_hotMods = 0; g_hotVk = 0;
    if (g_autoExit) return;                    // 提权子进程不抢热键
    if (g_cfg.hotkey.empty()) return;
    UINT mods = 0, vk = 0;
    if (!ParseHotkey(g_cfg.hotkey, mods, vk)) {
        AppendLogLine(L"[i] 热键格式无法识别，已跳过：" + g_cfg.hotkey + L"（示例：Ctrl+Alt+S）");
        return;
    }
    if (RegisterHotKey(g_hwnd, HOTKEY_ID, mods | MOD_NOREPEAT, vk)) {
        g_hotMods = mods; g_hotVk = vk;
        AppendLogLine(L"[i] 全局热键已注册：" + g_cfg.hotkey);
    } else {
        AppendLogLine(L"[!] 全局热键注册失败（可能被其它程序占用）：" + g_cfg.hotkey);
    }
}

// ---------------------------------------------------------------------------
// 控件文字 / 状态同步
// ---------------------------------------------------------------------------
static void SyncCfgFromUI()
{
    // 只有解析出「看起来像分辨率」的值才覆盖 g_cfg。
    // 否则输入框被清空、或填了非数字时，_wtoi 会返回 0，再被下面的下限钳成 320x200，
    // 于是拉伸目标就被悄悄改成一个垃圾值 —— 这类"顺手改配置"是最难查的 bug。
    UINT32 w = (UINT32)_wtoi(GetWndText(hEditW).c_str());
    UINT32 h = (UINT32)_wtoi(GetWndText(hEditH).c_str());
    if (w >= 320 && h >= 200) {
        g_cfg.stretchW = w;
        g_cfg.stretchH = h;
    } else {
        AppendLogLine(Fmt(L"[!] 拉伸分辨率输入无效（宽需 >=320、高需 >=200），沿用原值 %ux%u",
                          g_cfg.stretchW, g_cfg.stretchH));
    }
    g_cfg.stretchHz = (UINT32)_wtoi(GetWndText(hEditHz).c_str());
    g_cfg.logEnabled = IsDlgButtonChecked(g_hwnd, IDC_CHK_LOG) == BST_CHECKED;
    g_cfg.autoElevate = IsDlgButtonChecked(g_hwnd, IDC_CHK_ELEV) == BST_CHECKED;
    g_cfg.stretchDisableMonitor = IsDlgButtonChecked(g_hwnd, IDC_CHK_DISMON) == BST_CHECKED;
    g_cfg.dailyEnableMonitor = IsDlgButtonChecked(g_hwnd, IDC_CHK_ENMON) == BST_CHECKED;
    g_cfg.openNvcpOnMissing = IsDlgButtonChecked(g_hwnd, IDC_CHK_NVCP) == BST_CHECKED;
    g_cfg.hotkey = TrimW(GetWndText(hEditHotkey));
}

static void SyncUIToCfg()
{
    SetWndText(hEditW, std::to_wstring(g_cfg.stretchW));
    SetWndText(hEditH, std::to_wstring(g_cfg.stretchH));
    SetWndText(hEditHz, std::to_wstring(g_cfg.stretchHz));
    SetWndText(hEditHotkey, g_cfg.hotkey);
    CheckDlgButton(g_hwnd, IDC_CHK_LOG, g_cfg.logEnabled ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_hwnd, IDC_CHK_ELEV, g_cfg.autoElevate ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_hwnd, IDC_CHK_DISMON, g_cfg.stretchDisableMonitor ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_hwnd, IDC_CHK_ENMON, g_cfg.dailyEnableMonitor ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_hwnd, IDC_CHK_NVCP, g_cfg.openNvcpOnMissing ? BST_CHECKED : BST_UNCHECKED);
}

static void SetBusy(bool busy)
{
    g_busy = busy;
    int cmds[] = { IDC_BTN_TOGGLE, IDC_BTN_STRETCH, IDC_BTN_DAILY, IDC_BTN_SAVE,
                   IDC_BTN_EMERGENCY, IDC_BTN_NVCP, IDC_BTN_REFRESH };
    for (int c : cmds) EnableWindow(GetDlgItem(g_hwnd, c), busy ? FALSE : TRUE);
    EnableWindow(hBtnElevate, busy ? FALSE : TRUE);
    if (!busy) SetWndText(hStatus, L"就绪");
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void FillList(HWND lv, const std::vector<std::vector<std::wstring>>& rows)
{
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(lv);
    for (size_t i = 0; i < rows.size(); ++i) {
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        it.pszText = const_cast<LPWSTR>(rows[i][0].c_str());
        int idx = (int)SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&it);
        for (size_t c = 1; c < rows[i].size(); ++c) {
            ListView_SetItemText(lv, idx, (int)c, const_cast<LPWSTR>(rows[i][c].c_str()));
        }
    }
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lv, nullptr, TRUE);
}

static void FillDisplays(const LiveState& ls)
{
    std::vector<std::vector<std::wstring>> rows;
    for (size_t i = 0; i < ls.rows.size(); ++i) {
        const auto& r = ls.rows[i];
        std::wstring res = r.active ? Fmt(L"%ux%u@%uHz", r.width, r.height, r.refresh) : L"—";
        std::wstring pos = r.active ? Fmt(L"(%ld,%ld)", r.x, r.y) : L"—";
        rows.push_back({
            std::to_wstring(i),
            r.active ? L"启用" : L"未启用",
            r.technology,
            r.gdi.empty() ? L"—" : r.gdi,
            res,
            pos,
            r.monitor.empty() ? L"—" : r.monitor,
            r.internal ? L"是" : L""
        });
    }
    if (rows.empty()) rows.push_back({ L"—", L"没有检测到显示路径", L"", L"", L"", L"", L"", L"" });
    FillList(hLvDisp, rows);
}

static void FillMonitors()
{
    std::vector<MonitorDev> devs = EnumMonitorDevices();
    std::vector<std::vector<std::wstring>> rows;
    for (const auto& d : devs) {
        std::wstring state = d.disabled ? L"已禁用" : (d.started ? L"已启用" : L"未启动");
        rows.push_back({
            state,
            d.hardwareId.empty() ? L"—" : d.hardwareId,
            d.desc.empty() ? L"—" : d.desc,
            d.instanceId,
            d.disableable ? L"是" : L"否",
            d.problem ? std::to_wstring(d.problem) : L"—"
        });
    }
    if (rows.empty()) rows.push_back({ L"—", L"未找到监视器设备", L"", L"", L"", L"" });
    FillList(hLvMon, rows);
}

static void FillModes(const LiveState& ls)
{
    std::wstring tgt = g_cfg.stretchTarget;
    if (tgt == L"auto") tgt = (ls.externalActive > 0) ? L"external" : L"internal";
    std::wstring gdi = (tgt == L"internal") ? ls.internalGdi : ls.externalGdi;
    if (gdi.empty()) gdi = ls.internalGdi.empty() ? ls.externalGdi : ls.internalGdi;

    SendMessageW(hLbMode, LB_RESETCONTENT, 0, 0);
    std::vector<GdiMode> ms = EnumGdiModes(gdi);

    // 每个分辨率只保留最高刷新率，然后按「像素数、刷新率」从高到低排
    std::vector<GdiMode> best;
    for (const auto& m : ms) {
        bool merged = false;
        for (auto& b : best) {
            if (b.w == m.w && b.h == m.h) {
                if (m.hz > b.hz) { b.hz = m.hz; b.bpp = m.bpp; }
                merged = true;
                break;
            }
        }
        if (!merged) best.push_back(m);
    }
    std::sort(best.begin(), best.end(), [](const GdiMode& a, const GdiMode& b) {
        if (a.w * a.h != b.w * b.h) return a.w * a.h > b.w * b.h;
        return a.hz > b.hz;
    });

    for (const auto& m : best) {
        std::wstring s = Fmt(L"%ux%u   ·   %u Hz", m.w, m.h, m.hz);
        SendMessageW(hLbMode, LB_ADDSTRING, 0, (LPARAM)s.c_str());
    }
    if (best.empty()) SendMessageW(hLbMode, LB_ADDSTRING, 0, (LPARAM)L"（读取不到分辨率列表）");
    SetWndText(hLblMode, L"可用分辨率  ·  " + (gdi.empty() ? std::wstring(L"—") : gdi));
}

// ---------------------------------------------------------------------------
// 刷新界面（只读，不改变系统状态）
// ---------------------------------------------------------------------------
static void RefreshAll(bool logIt)
{
    LiveState ls;
    if (!ReadLive(ls)) {
        g_mode = L"unknown";
        SetWndText(hBannerTitle, L"无法读取当前显示配置");
        SetWndText(hBannerSub, L"请点「刷新」重试；若持续失败，可能是显卡驱动异常。");
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }

    std::wstring tgt = g_cfg.stretchTarget;
    if (tgt == L"auto") tgt = (ls.externalActive > 0) ? L"external" : L"internal";
    UINT32 w = (tgt == L"internal") ? ls.internalW : ls.externalW;
    UINT32 h = (tgt == L"internal") ? ls.internalH : ls.externalH;
    UINT32 hz = (tgt == L"internal") ? ls.internalHz : ls.externalHz;

    bool isStretch = (w == g_cfg.stretchW && h == g_cfg.stretchH);
    g_mode = isStretch ? L"stretch" : L"daily";

    if (isStretch) {
        SetWndText(hBannerTitle, Fmt(L"真实拉伸模式    %ux%u @ %uHz", w, h, hz));
        SetWndText(hBannerSub,
                   Fmt(L"%s（%s）· 监视器设备已禁用，自定义分辨率已生效",
                       tgt == L"internal" ? L"笔记本内置屏" : L"外接显示器",
                       (tgt == L"internal" ? ls.internalGdi : ls.externalGdi).c_str()));
        SetWndText(hBtnToggle, Fmt(L"←  回到日常模式  %ux%u", w, h));
    } else {
        SetWndText(hBannerTitle, Fmt(L"日常模式    %ux%u @ %uHz", w, h, hz));
        SetWndText(hBannerSub,
                   Fmt(L"%s（%s）· 点下方按钮切到 %ux%u 拉伸",
                       tgt == L"internal" ? L"笔记本内置屏" : L"外接显示器",
                       (tgt == L"internal" ? ls.internalGdi : ls.externalGdi).c_str(),
                       g_cfg.stretchW, g_cfg.stretchH));
        SetWndText(hBtnToggle, Fmt(L"切到真实拉伸  %ux%u  →", g_cfg.stretchW, g_cfg.stretchH));
    }

    FillDisplays(ls);
    FillMonitors();
    FillModes(ls);

    SetWndText(hAdmin, g_elevated ? L"管理员权限：已获得"
                                  : L"管理员权限：未获得（切换时会弹 UAC）");
    EnableWindow(hBtnElevate, g_elevated ? FALSE : TRUE);

    if (logIt) AppendLogLine(Fmt(L"[i] 已刷新：%s %ux%u@%uHz", isStretch ? L"拉伸" : L"日常", w, h, hz));
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// 执行操作（可带自动提权）
// ---------------------------------------------------------------------------
static int DispatchOp(const std::wstring& op)
{
    if (op == L"stretch")   return CmdStretch(g_cfg);
    if (op == L"daily")     return CmdDaily(g_cfg);
    if (op == L"toggle")    return CmdToggle(g_cfg);
    if (op == L"save")      return CmdSave(g_cfg, false);
    if (op == L"saveforce") return CmdSave(g_cfg, true);
    if (op == L"emergency") return CmdEmergentlyRestore();
    if (op == L"nvcp")      return CmdOpenNvcp();
    return EXIT_USAGE;
}

static DWORD WINAPI OpThread(LPVOID param)
{
    std::wstring op = *(std::wstring*)param;
    delete (std::wstring*)param;

    AppendLogLine(L"--------------------------------------------");
    int rc = DispatchOp(op);

    // 权限不足 -> 拉起提权的自己再跑一次（子进程跑完会自己退出）
    if (rc == EXIT_ELEVATION && g_cfg.autoElevate && !g_elevated && !g_launchedElev) {
        Out(L"[i] 该操作需要管理员权限，正在请求提权（会弹出 UAC / 切换过程会闪一个新窗口）...");
        std::vector<std::wstring> args{ L"--op", op };
        DWORD child = EXIT_OK;
        if (RelaunchElevated(args, child, true)) rc = (int)child;
        else                                     rc = EXIT_ELEVATION;
    }

    PostMessageW(g_hwnd, WM_APP_DONE, (WPARAM)rc, 0);
    return 0;
}

static std::wstring g_lastOp;

static void RunOp(const std::wstring& op, bool allowRerun = true)
{
    if (g_busy) {
        AppendLogLine(L"[i] 上一步操作还没结束，请稍候 ...");
        return;
    }
    (void)allowRerun;
    SyncCfgFromUI();
    g_lastOp = op;
    SetBusy(true);
    SetWndText(hStatus, L"正在执行，请稍候 ...");
    std::wstring* p = new std::wstring(op);
    HANDLE th = CreateThread(nullptr, 0, OpThread, p, 0, nullptr);
    if (!th) { delete p; SetBusy(false); AppendLogLine(L"[!] 无法创建线程"); }
    else CloseHandle(th);
}

// ---------------------------------------------------------------------------
// 托盘
// ---------------------------------------------------------------------------
static HICON AppIcon()
{
    HICON ic = (HICON)LoadImageW(g_inst, L"APPICON", IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    if (!ic) ic = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    if (!ic) ic = LoadIconW(nullptr, IDI_APPLICATION);
    return ic;
}

static void TrayAdd()
{
    if (g_trayAdded) return;
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = AppIcon();
    wcsncpy(g_nid.szTip, L"DisplaySwitch —— 显示模式切换", 127);
    g_nid.szTip[127] = L'\0';
    if (Shell_NotifyIconW(NIM_ADD, &g_nid)) g_trayAdded = true;
}

static void TrayRemove()
{
    if (!g_trayAdded) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_trayAdded = false;
}

static void ShowTrayMenu(POINT pt)
{
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_SHOW, L"显示主窗口");
    AppendMenuW(m, MF_STRING, IDM_TOGGLE, L"一键切换拉伸 / 日常");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出");
    SetForegroundWindow(g_hwnd);
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(m);
    if (cmd == IDM_SHOW)   { ShowWindow(g_hwnd, SW_SHOW); ShowWindow(g_hwnd, SW_RESTORE); SetForegroundWindow(g_hwnd); }
    if (cmd == IDM_TOGGLE) RunOp(L"toggle");
    if (cmd == IDM_EXIT)   PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

// ---------------------------------------------------------------------------
// 布局
// ---------------------------------------------------------------------------
static void ApplyFont(HWND h, HFONT f)
{
    if (h) SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE);
}

static void SizeColumns();   // 定义在后面

// 卡片内边距
static const int PAD = 14;      // 卡片内左右留白
static const int CARD_R = 8;    // 卡片圆角

static void Layout()
{
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int W = rc.right - rc.left;
    int H = rc.bottom - rc.top;
    const int M = Sc(14);        // 窗口外边距
    const int G = Sc(12);        // 卡片间距

    g_groups.clear();

    // ---------------- 1. 横幅卡片 ----------------
    int banH = Sc(84);
    RECT rBan{ M, M, W - M, M + banH };
    {
        Group g;
        g.rc = rBan;
        g.title = L"";
        g_groups.push_back(g);
    }
    // 横幅内控件：留出左侧圆点位置
    int dotPad = Sc(58);
    MoveWindow(hBannerTitle, rBan.left + dotPad, rBan.top + Sc(16),
               rBan.right - rBan.left - dotPad - PAD, Sc(30), TRUE);
    MoveWindow(hBannerSub, rBan.left + dotPad, rBan.top + Sc(48),
               rBan.right - rBan.left - dotPad - PAD, Sc(20), TRUE);

    // ---------------- 2. 操作卡片 ----------------
    int y = rBan.bottom + G;
    int opH = Sc(124);
    RECT rOp{ M, y, W - M, y + opH };
    {
        Group g;
        g.rc = rOp;
        g.title = L"操作";
        g.titleRc = { rOp.left + PAD, rOp.top + Sc(10),
                      rOp.right - PAD, rOp.top + Sc(28) };
        g_groups.push_back(g);
    }
    // 主按钮：大而醒目，占左侧约 1/3
    int innerTop = rOp.top + Sc(36);
    int bigH = Sc(52);
    int bigW = (W - 2 * M - 2 * PAD) / 3;
    if (bigW < Sc(200)) bigW = Sc(200);
    MoveWindow(hBtnToggle, rOp.left + PAD, innerTop, bigW, bigH, TRUE);

    // 右侧按钮 2x2 网格
    int gx = rOp.left + PAD + bigW + G;
    int gw = (W - M - PAD - gx - G) / 2;
    if (gw < Sc(110)) gw = Sc(110);
    int gh = Sc(24);
    int gy1 = innerTop;
    int gy2 = innerTop + Sc(28);
    MoveWindow(hBtnStretch, gx, gy1, gw, gh, TRUE);
    MoveWindow(hBtnDaily,   gx + gw + G, gy1, gw, gh, TRUE);
    MoveWindow(hBtnSave,    gx, gy2, gw, gh, TRUE);
    MoveWindow(hBtnEmergency, gx + gw + G, gy2, gw, gh, TRUE);

    // ---------------- 3. 中部：左 设备 / 右 设置 ----------------
    // 底部区域自下而上：日志卡片 -> 工具条（状态 + 次级按钮）
    int logH = Sc(118);
    int logTop = H - M - logH;
    int barH = Sc(34);                      // 工具条
    int barTop = logTop - Sc(16) - barH;    // 与日志卡留出 16px 间距

    int midTop = rOp.bottom + G;
    int midBot = barTop - G;
    int midH = midBot - midTop;
    if (midH < Sc(150)) midH = Sc(150);

    int rightW = Sc(326);
    if (W < Sc(940)) rightW = Sc(300);
    int leftW = W - 2 * M - rightW - G;
    if (leftW < Sc(320)) leftW = Sc(320);

    // --- 左卡片：显示设备 ---
    RECT rDev{ M, midTop, M + leftW, midTop + midH };
    {
        Group g;
        g.rc = rDev;
        g.title = L"显示设备";
        g.trailing = L"（只读）";
        g.titleRc = { rDev.left + PAD, rDev.top + Sc(10),
                      rDev.right - PAD, rDev.top + Sc(28) };
        g_groups.push_back(g);
    }
    int devTop = rDev.top + Sc(38);
    int dynH = (midH - Sc(38) - PAD) / 2;
    int lvH = dynH - Sc(28);
    if (lvH < Sc(60)) lvH = Sc(60);
    MoveWindow(hLblDisp, rDev.left + PAD, devTop, leftW - 2 * PAD, Sc(16), TRUE);
    MoveWindow(hLvDisp, rDev.left + PAD, devTop + Sc(18), leftW - 2 * PAD, lvH, TRUE);
    int monTop = devTop + Sc(18) + lvH + Sc(14);
    MoveWindow(hLblMon, rDev.left + PAD, monTop, leftW - 2 * PAD, Sc(16), TRUE);
    int monH = (rDev.bottom - PAD) - (monTop + Sc(18));
    if (monH < Sc(50)) monH = Sc(50);
    MoveWindow(hLvMon, rDev.left + PAD, monTop + Sc(18), leftW - 2 * PAD, monH, TRUE);

    // --- 右卡片：分辨率与设置 ---
    int rx0 = M + leftW + G;
    RECT rSet{ rx0, midTop, rx0 + rightW, midTop + midH };
    {
        Group g;
        g.rc = rSet;
        g.title = L"分辨率与设置";
        g.titleRc = { rSet.left + PAD, rSet.top + Sc(10),
                      rSet.right - PAD, rSet.top + Sc(28) };
        g_groups.push_back(g);
    }
    int ix = rSet.left + PAD;
    int iw = rightW - 2 * PAD;
    // 卡片可用高度
    int cardTop = rSet.top + Sc(34);
    int cardBot = rSet.bottom - Sc(12);
    int avail = cardBot - cardTop;

    // 底部固定块（从下往上）：5 行选项 + 热键 + 拉伸分辨率
    int chkH = Sc(19);
    int rowH = Sc(22);
    int labelH = Sc(15);
    int inputH = Sc(23);
    // 选项占 3 行（两列）
    int optBlock = 3 * rowH;
    int hotBlock = labelH + Sc(3) + inputH;
    int resBlock = labelH + Sc(3) + inputH;
    int fixedH = optBlock + Sc(10) + hotBlock + Sc(10) + resBlock + Sc(10);
    // 剩下的给分辨率列表
    int lbH = avail - fixedH - Sc(18) - Sc(14);   // 18=小标题, 14=间距
    if (lbH < Sc(60)) lbH = Sc(60);
    if (lbH > Sc(150)) lbH = Sc(150);

    int sy = cardTop;
    MoveWindow(hLblMode, ix, sy, iw, labelH, TRUE);
    MoveWindow(hLbMode, ix, sy + Sc(17), iw, lbH, TRUE);

    int ry = sy + Sc(17) + lbH + Sc(12);
    MoveWindow(hLblRes, ix, ry, iw, labelH, TRUE);
    int ew = (iw - Sc(48)) / 3;
    int e1 = ix;
    int e2 = ix + ew + Sc(24);
    int e3 = ix + 2 * (ew + Sc(24));
    int ey = ry + Sc(16);
    MoveWindow(hEditW, e1, ey, ew, inputH, TRUE);
    MoveWindow(hLblX,  e1 + ew, ey, Sc(24), inputH, TRUE);
    MoveWindow(hEditH, e2, ey, ew, inputH, TRUE);
    MoveWindow(hLblAt, e2 + ew, ey, Sc(24), inputH, TRUE);
    MoveWindow(hEditHz, e3, ey, ew, inputH, TRUE);
    MoveWindow(hLblHz, e3 + ew + Sc(3), ey, iw - (e3 - ix) - ew - Sc(3), inputH, TRUE);

    int hy = ey + inputH + Sc(10);
    MoveWindow(hLblHot, ix, hy, iw, labelH, TRUE);
    MoveWindow(hEditHotkey, ix, hy + Sc(16), iw, inputH, TRUE);

    // 选项：两列排布（左 3 项、右 2 项）
    int cy = hy + Sc(16) + inputH + Sc(10);
    int halfW = iw / 2;
    MoveWindow(hChkDisMon, ix, cy, halfW, chkH, TRUE);
    MoveWindow(hChkLog,    ix, cy + rowH, halfW, chkH, TRUE);
    MoveWindow(hChkElev,   ix, cy + 2 * rowH, halfW, chkH, TRUE);
    MoveWindow(hChkEnMon,  ix + halfW, cy, halfW, chkH, TRUE);
    MoveWindow(hChkNvcp,   ix + halfW, cy + rowH, halfW, chkH, TRUE);

    // ---------------- 4. 底部：工具条 + 日志卡片 ----------------
    // 日志卡片（先确定它的右边界，工具条按钮与之右对齐）
    RECT rLog{ M, logTop, W - M, logTop + logH };

    // 工具条：左侧「状态 + 管理员权限」，右侧四个次级按钮（右对齐到卡片内边距）
    int barH_ = Sc(30);
    int barY = barTop + (barH - barH_) / 2 + Sc(2);
    int bw1 = Sc(148), bw2 = Sc(64), bw3 = Sc(130), bw4 = Sc(158);
    int gapBtn = Sc(8);
    int rightEdge = rLog.right - PAD;
    int bx = rightEdge - bw4;
    MoveWindow(hBtnSaveCfg, bx, barY, bw4, barH_, TRUE);
    bx -= bw3 + gapBtn;
    MoveWindow(hBtnElevate, bx, barY, bw3, barH_, TRUE);
    bx -= bw2 + gapBtn;
    MoveWindow(hBtnRefresh, bx, barY, bw2, barH_, TRUE);
    bx -= bw1 + gapBtn;
    MoveWindow(hBtnNvcp, bx, barY, bw1, barH_, TRUE);
    // 左侧：状态文字 + 管理员权限提示
    MoveWindow(hStatus, M + Sc(2), barTop + Sc(2), Sc(380), Sc(15), TRUE);
    MoveWindow(hAdmin, M + Sc(2), barTop + Sc(17), Sc(380), Sc(15), TRUE);

    {
        Group g;
        g.rc = rLog;
        g.title = L"运行日志";
        g.titleRc = { rLog.left + PAD, rLog.top + Sc(10),
                      rLog.right - PAD, rLog.top + Sc(28) };
        g_groups.push_back(g);
    }
    int btnW = Sc(122);
    int logInnerTop = rLog.top + Sc(36);
    int logInnerH = (rLog.bottom - Sc(12)) - logInnerTop;
    MoveWindow(hLog, rLog.left + PAD, logInnerTop,
               (rLog.right - PAD) - (rLog.left + PAD) - btnW - G, logInnerH, TRUE);
    MoveWindow(hBtnClearLog, rLog.right - PAD - btnW, logInnerTop, btnW, Sc(28), TRUE);
    MoveWindow(hBtnCopyLog,  rLog.right - PAD - btnW, logInnerTop + Sc(34), btnW, Sc(28), TRUE);

    SizeColumns();
}

static void AddColumns(HWND lv, const std::vector<std::pair<const wchar_t*, int>>& cols)
{
    for (size_t i = 0; i < cols.size(); ++i) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        c.pszText = const_cast<LPWSTR>(cols[i].first);
        c.cx = cols[i].second;
        c.iSubItem = (int)i;
        SendMessageW(lv, LVM_INSERTCOLUMNW, i, (LPARAM)&c);
    }
}

// 列宽按控件实际宽度等比分配（DPI 缩放后固定像素宽会把文字挤掉）
static void SizeColumns()
{
    static const int dw[] = { 30, 54, 84, 104, 128, 72, 146, 42 };    // 显示器
    static const int mw[] = { 62, 130, 158, 300, 56, 56 };            // 监视器设备

    RECT rc;
    GetClientRect(hLvDisp, &rc);
    int w = rc.right - rc.left - Sc(20);          // 留出竖向滚动条
    int sum = 0; for (int v : dw) sum += v;
    for (int i = 0; i < 8; ++i)
        ListView_SetColumnWidth(hLvDisp, i, (int)((long long)w * dw[i] / sum));

    GetClientRect(hLvMon, &rc);
    w = rc.right - rc.left - Sc(20);
    sum = 0; for (int v : mw) sum += v;
    for (int i = 0; i < 6; ++i)
        ListView_SetColumnWidth(hLvMon, i, (int)((long long)w * mw[i] / sum));
}

// ---------------------------------------------------------------------------
// 创建子控件
// ---------------------------------------------------------------------------
static void CreateControls()
{
    hBannerTitle = MakeStatic(L"正在读取显示状态 ...", SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS);
    hBannerSub   = MakeStatic(L"", SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS);

    // 操作卡片
    hBtnToggle    = MakeButton(L"切到真实拉伸", IDC_BTN_TOGGLE, BK_PRIMARY);
    hBtnStretch   = MakeButton(L"只切拉伸", IDC_BTN_STRETCH);
    hBtnDaily     = MakeButton(L"只切日常", IDC_BTN_DAILY);
    hBtnSave      = MakeButton(L"存为日常基准", IDC_BTN_SAVE);
    hBtnEmergency = MakeButton(L"紧急恢复", IDC_BTN_EMERGENCY, BK_DANGER);

    // 底部次级
    hBtnNvcp    = MakeButton(L"打开 NVIDIA 控制面板", IDC_BTN_NVCP);
    hBtnRefresh = MakeButton(L"刷新", IDC_BTN_REFRESH);
    hBtnElevate = MakeButton(L"以管理员身份重启", IDC_BTN_ELEVATE);
    hAdmin      = MakeStatic(L"管理员权限：检查中 ...", SS_LEFT | SS_CENTERIMAGE);
    hBtnSaveCfg = MakeButton(L"保存设置到 display.cfg", IDC_BTN_SAVECFG);

    hLblDisp = MakeStatic(L"显示器（CCD 路径）", SS_LEFT);
    hLblMon  = MakeStatic(L"监视器设备（设备管理器 → 监视器）", SS_LEFT);
    hLblMode = MakeStatic(L"可用分辨率", SS_LEFT);

    hLvDisp = CreateWindowExW(0, WC_LISTVIEWW, L"",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                              0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)IDC_LV_DISP, g_inst, nullptr);
    ListView_SetExtendedListViewStyle(hLvDisp,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    ListView_SetBkColor(hLvDisp, RGB(0xFF, 0xFF, 0xFF));
    ListView_SetTextBkColor(hLvDisp, RGB(0xFF, 0xFF, 0xFF));
    ListView_SetTextColor(hLvDisp, COL_TEXT);
    AddColumns(hLvDisp, { {L"#", 30}, {L"状态", 54}, {L"接口", 84}, {L"GDI 名", 104},
                          {L"分辨率", 128}, {L"位置", 72}, {L"显示器名", 146}, {L"内置", 42} });

    hLvMon = CreateWindowExW(0, WC_LISTVIEWW, L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                             0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)IDC_LV_MON, g_inst, nullptr);
    ListView_SetExtendedListViewStyle(hLvMon,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    ListView_SetBkColor(hLvMon, RGB(0xFF, 0xFF, 0xFF));
    ListView_SetTextBkColor(hLvMon, RGB(0xFF, 0xFF, 0xFF));
    ListView_SetTextColor(hLvMon, COL_TEXT);
    AddColumns(hLvMon, { {L"状态", 62}, {L"硬件 ID", 130}, {L"描述", 158},
                         {L"实例 ID", 300}, {L"可禁用", 56}, {L"问题码", 56} });

    hLbMode = CreateWindowExW(0, L"LISTBOX", L"",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY,
                              0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)IDC_LB_MODE, g_inst, nullptr);

    hEditW = CreateWindowExW(0, L"EDIT", L"1280",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_CENTER,
                             0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)IDC_EDIT_W, g_inst, nullptr);
    hEditH = CreateWindowExW(0, L"EDIT", L"882",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_CENTER,
                             0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)IDC_EDIT_H, g_inst, nullptr);
    hEditHz = CreateWindowExW(0, L"EDIT", L"0",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_CENTER,
                              0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)IDC_EDIT_HZ, g_inst, nullptr);
    hEditHotkey = CreateWindowExW(0, L"EDIT", L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                  0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)IDC_EDIT_HOTKEY, g_inst, nullptr);

    hChkDisMon = MakeCheck(L"拉伸时禁用监视器", IDC_CHK_DISMON);
    hChkEnMon  = MakeCheck(L"回日常时启用", IDC_CHK_ENMON);
    hChkLog    = MakeCheck(L"写日志文件", IDC_CHK_LOG);
    hChkElev   = MakeCheck(L"需要时自动提权", IDC_CHK_ELEV);
    hChkNvcp   = MakeCheck(L"缺分辨率开 NV 面板", IDC_CHK_NVCP);

    hStaticHint = MakeStatic(L"运行日志", SS_LEFT);
    hLog = CreateWindowExW(0, L"EDIT", L"",
                           WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                           ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                           0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)IDC_EDIT_LOG, g_inst, nullptr);
    hBtnClearLog = MakeButton(L"清空日志", IDC_BTN_CLEARLOG);
    hBtnCopyLog  = MakeButton(L"复制全部日志", IDC_BTN_COPYLOG);
    hStatus      = MakeStatic(L"就绪", SS_LEFT | SS_CENTERIMAGE);

    // 拉伸分辨率那一行的小标签
    hLblRes = MakeStatic(L"拉伸分辨率", SS_LEFT);
    hLblX   = MakeStatic(L"×", SS_CENTER | SS_CENTERIMAGE);
    hLblAt  = MakeStatic(L"@", SS_CENTER | SS_CENTERIMAGE);
    hLblHz  = MakeStatic(L"Hz（0=自动）", SS_LEFT | SS_CENTERIMAGE);
    hLblHot = MakeStatic(L"全局热键（例：Ctrl+Alt+S）", SS_LEFT);

    // 给所有自绘按钮装悬停跟踪
    HWND btns[] = { hBtnToggle, hBtnStretch, hBtnDaily, hBtnSave, hBtnEmergency,
                    hBtnNvcp, hBtnRefresh, hBtnElevate, hBtnSaveCfg, hBtnClearLog, hBtnCopyLog };
    for (HWND b : btns) SetWindowSubclass(b, ButtonSubclass, 0, 0);
    // 列表交替行色
    SetWindowSubclass(hLvDisp, ListSubclass, 1, 0);
    SetWindowSubclass(hLvMon, ListSubclass, 2, 0);
    // 表头扁平化
    HWND hdr1 = (HWND)SendMessageW(hLvDisp, LVM_GETHEADER, 0, 0);
    HWND hdr2 = (HWND)SendMessageW(hLvMon, LVM_GETHEADER, 0, 0);
    if (hdr1) SetWindowSubclass(hdr1, HeaderSubclass, 3, 0);
    if (hdr2) SetWindowSubclass(hdr2, HeaderSubclass, 4, 0);
}

static void FontAll()
{
    HWND lists[] = { hBtnToggle, hBtnStretch, hBtnDaily, hBtnSave, hBtnEmergency,
                     hBtnNvcp, hBtnRefresh, hBtnElevate, hBtnSaveCfg, hBtnClearLog, hBtnCopyLog,
                     hLvDisp, hLvMon, hLbMode, hEditW, hEditH, hEditHz, hEditHotkey,
                     hChkLog, hChkElev, hChkDisMon, hChkEnMon, hChkNvcp,
                     hLblDisp, hLblMon, hLblMode, hStatus, hAdmin,
                     hLblRes, hLblX, hLblAt, hLblHz, hLblHot };
    for (HWND h : lists) ApplyFont(h, g_fUI);
    ApplyFont(hBannerSub, g_fSmall);
    ApplyFont(hLblDisp, g_fBold);
    ApplyFont(hLblMon, g_fBold);
    ApplyFont(hLblMode, g_fBold);
    ApplyFont(hLblRes, g_fBold);
    ApplyFont(hLblHot, g_fBold);
    ApplyFont(hLog, g_fLog);
    // 主按钮 / 紧急恢复 用中等偏粗
    ApplyFont(hBtnToggle, g_fMed);
}

// ---------------------------------------------------------------------------
// 绘制卡片与分组标题
// ---------------------------------------------------------------------------
static void PaintCards(HWND hwnd, HDC dc)
{
    for (const Group& g : g_groups) {
        // 卡片底：白底 + 细描边 + 轻投影感（用一条底部淡线模拟）
        RECT r = g.rc;
        FillRoundRect(dc, r, Sc(CARD_R), COL_CARD);
        StrokeRoundRect(dc, r, Sc(CARD_R), COL_CARD_LINE);

        // 分组标题：左侧一根强调色短竖条 + 标题
        if (!g.title.empty()) {
            int tx = g.titleRc.left;
            int ty = g.titleRc.top;
            int th = g.titleRc.bottom - g.titleRc.top;
            // 竖条
            RECT bar{ tx, ty + Sc(2), tx + Sc(3), ty + th - Sc(2) };
            HBRUSH br = CreateSolidBrush(COL_ACCENT);
            FillRect(dc, &bar, br);
            DeleteObject(br);
            RECT tr{ tx + Sc(9), ty - Sc(1), g.titleRc.right, ty + th };
            DrawTextAt(dc, g.title, tr, g_fHead, COL_TEXT,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            if (!g.trailing.empty()) {
                DrawTextAt(dc, g.trailing, g.titleRc, g_fSmall, COL_MUTED,
                           DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            }
        }
    }

    // 顶部横幅卡片：按状态着色
    if (!g_groups.empty()) {
        const RECT& r = g_groups[0].rc;
        COLORREF bg = COL_UNKNOWN_BG, accent = COL_UNKNOWN;
        if (g_mode == L"stretch") { bg = COL_STRETCH_BG; accent = COL_STRETCH; }
        else if (g_mode == L"daily") { bg = COL_DAILY_BG; accent = COL_DAILY; }
        FillRoundRect(dc, r, Sc(CARD_R), bg);
        StrokeRoundRect(dc, r, Sc(CARD_R), accent);

        // 左侧状态圆点（带晕圈）
        int cx = r.left + Sc(30);
        int cy = (r.top + r.bottom) / 2;
        DrawCircle(dc, cx, cy, Sc(13), bg);   // 晕圈（同底色，靠描边区分）
        DrawCircle(dc, cx, cy, Sc(8), accent);
    }
}

// ---------------------------------------------------------------------------
// 主窗口过程
// ---------------------------------------------------------------------------
static void OnCommand(int id, int code)
{
    switch (id) {
    case IDC_BTN_TOGGLE:    RunOp(L"toggle"); break;
    case IDC_BTN_STRETCH:   RunOp(L"stretch"); break;
    case IDC_BTN_DAILY:     RunOp(L"daily"); break;
    case IDC_BTN_SAVE:      RunOp(L"save"); break;
    case IDC_BTN_EMERGENCY: RunOp(L"emergency"); break;
    case IDC_BTN_NVCP:      RunOp(L"nvcp"); break;

    case IDC_BTN_REFRESH:
        SetWndText(hStatus, L"正在读取 ...");
        RefreshAll(true);
        SetWndText(hStatus, L"就绪");
        break;

    case IDC_BTN_SAVECFG: {
        SyncCfgFromUI();
        std::wstring err;
        if (SaveCfg(g_cfg, err)) {
            AppendLogLine(Fmt(L"[OK] 设置已保存到 %s", g_paths.cfg.wstring().c_str()));
            g_logEnabled = g_cfg.logEnabled;
            RegisterGlobalHotkey();
        } else {
            AppendLogLine(L"[!] " + err);
            MessageBoxW(g_hwnd, err.c_str(), L"保存失败", MB_ICONERROR);
        }
        break;
    }

    case IDC_BTN_ELEVATE: {
        AppendLogLine(L"[i] 正在以管理员身份重新启动 ...");
        std::vector<std::wstring> args;
        if (g_autoExit) { args.push_back(L"--op"); args.push_back(L"none"); }
        DWORD child = 0;
        if (RelaunchElevated(args, child, false)) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        else AppendLogLine(L"[!] 提权被取消。");
        break;
    }

    case IDC_BTN_CLEARLOG: SetWndText(hLog, L""); break;
    case IDC_BTN_COPYLOG: {
        std::wstring t = GetWndText(hLog);
        if (OpenClipboard(g_hwnd)) {
            EmptyClipboard();
            size_t bytes = (t.size() + 1) * sizeof(wchar_t);
            HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (g) {
                memcpy(GlobalLock(g), t.c_str(), bytes);
                GlobalUnlock(g);
                SetClipboardData(CF_UNICODETEXT, g);
            }
            CloseClipboard();
            AppendLogLine(L"[i] 日志已复制到剪贴板。");
        }
        break;
    }

    case IDC_LB_MODE:
        if (code == LBN_SELCHANGE) {
            int sel = (int)SendMessageW(hLbMode, LB_GETCURSEL, 0, 0);
            if (sel >= 0) {
                wchar_t buf[128] = {0};
                SendMessageW(hLbMode, LB_GETTEXT, (WPARAM)sel, (LPARAM)buf);
                UINT32 w = 0, h = 0, hz = 0;
                if (swscanf(buf, L"%ux%u   ·   %u", &w, &h, &hz) >= 2 && w && h) {
                    SetWndText(hEditW, std::to_wstring(w));
                    SetWndText(hEditH, std::to_wstring(h));
                    SetWndText(hEditHz, std::to_wstring(hz));
                    // 列表只是「可选项」，点一下就会写进上面的输入框。以前这是静默发生的，
                    // 很容易出现「明明没改设置，拉伸目标却变了」的困惑，所以这里明确报一句。
                    AppendLogLine(Fmt(L"[i] 已把拉伸分辨率填成 %ux%u@%uHz。"
                                      L"要让下次启动也生效，请点底部「保存设置到 display.cfg」。",
                                      w, h, hz));
                }
            }
        }
        break;

    default: break;
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        CreateControls();
        FontAll();
        SyncUIToCfg();
        Layout();
        TrayAdd();
        RegisterGlobalHotkey();
        AppendLogLine(L"==== DisplaySwitch · 显示模式切换 ====");
        AppendLogLine(Fmt(L"程序目录: %s", g_paths.dir.wstring().c_str()));
        AppendLogLine(Fmt(L"拉伸目标: %ux%u  输出到 %s",
                          g_cfg.stretchW, g_cfg.stretchH,
                          g_cfg.stretchTarget == L"internal" ? L"笔记本内置屏" : L"外接显示器"));
        RefreshAll(false);
        return 0;

    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) { Layout(); InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize.x = Sc(880);
        mm->ptMinTrackSize.y = Sc(620);
        return 0;
    }

    case WM_COMMAND:
        OnCommand(LOWORD(wp), HIWORD(wp));
        return 0;

    case WM_DRAWITEM:
        DrawButton((const DRAWITEMSTRUCT*)lp);
        return TRUE;

    case WM_APP_LOG: {
        std::wstring* p = (std::wstring*)lp;
        AppendToControl(*p);
        delete p;
        return 0;
    }

    case WM_APP_DONE: {
        int rc = (int)wp;
        SetBusy(false);
        const wchar_t* txt = L"完成";
        if (rc == EXIT_OK) txt = L"✓ 操作成功完成";
        else if (rc == EXIT_PRECHECK) txt = L"⚠ 前置条件不满足（详见日志）";
        else if (rc == EXIT_APPLY_FAILED) txt = L"⚠ 应用失败，已回滚（详见日志）";
        else if (rc == EXIT_ROLLBACK_FAIL) txt = L"✗ 失败且回滚未成功，请紧急恢复";
        else if (rc == EXIT_ELEVATION) txt = L"⚠ 需要管理员权限";
        else txt = L"⚠ 操作未成功（详见日志）";
        SetWndText(hStatus, txt);
        AppendLogLine(Fmt(L"[结束] %s（退出码 %d）", txt, rc));
        RefreshAll(false);
        g_result = rc;

        // 「存为日常基准」碰到已有基准时，问一句要不要覆盖
        if (!g_autoExit && g_lastOp == L"save" && rc == EXIT_PRECHECK) {
            int a = MessageBoxW(hwnd,
                                L"已经存在日常基准了。\n\n"
                                L"要用「当前显示状态」覆盖它吗？\n"
                                L"（如果当前不是日常模式，覆盖后回日常会回到错误的分辨率）",
                                L"覆盖日常基准？", MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2);
            if (a == IDYES) { RunOp(L"saveforce"); return 0; }
        }

        if (g_autoExit) {
            if (rc != EXIT_OK)
                MessageBoxW(hwnd, txt, L"DisplaySwitch", MB_ICONWARNING | MB_OK);
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;
    }

    case WM_HOTKEY:
        if ((UINT)wp == HOTKEY_ID) {
            AppendLogLine(L"[热键] 触发一键切换");
            RunOp(L"toggle");
        }
        return 0;

    case WM_APP_TRAY:
        if (lp == WM_LBUTTONDBLCLK) {
            ShowWindow(hwnd, SW_SHOW);
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
        } else if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) {
            POINT pt;
            GetCursorPos(&pt);
            ShowTrayMenu(pt);
        }
        return 0;

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        HWND ctl = (HWND)lp;
        // 横幅文字：背景跟随横幅卡片的底色
        if (ctl == hBannerTitle || ctl == hBannerSub) {
            COLORREF bg, tx;
            if (g_mode == L"stretch")      { bg = COL_STRETCH_BG; tx = COL_STRETCH_TX; }
            else if (g_mode == L"daily")   { bg = COL_DAILY_BG;   tx = COL_DAILY_TX; }
            else                           { bg = COL_UNKNOWN_BG; tx = COL_UNKNOWN_TX; }
            SetBkMode(dc, TRANSPARENT);
            SetBkColor(dc, bg);
            SetTextColor(dc, tx);
            static HBRUSH brS = nullptr, brD = nullptr, brU = nullptr;
            if (!brS) {
                brS = CreateSolidBrush(COL_STRETCH_BG);
                brD = CreateSolidBrush(COL_DAILY_BG);
                brU = CreateSolidBrush(COL_UNKNOWN_BG);
            }
            return (LRESULT)(g_mode == L"stretch" ? brS
                           : g_mode == L"daily"   ? brD : brU);
        }
        // 工具条上的动态文字：用窗口底色（它们不在卡片里）
        if (ctl == hStatus || ctl == hAdmin) {
            SetBkMode(dc, TRANSPARENT);
            SetBkColor(dc, COL_BG);
            SetTextColor(dc, COL_SUBTEXT);
            return (LRESULT)g_brBg;
        }
        // 其余静态控件都在白色卡片里
        SetBkMode(dc, TRANSPARENT);
        SetBkColor(dc, COL_CARD);
        SetTextColor(dc, COL_TEXT);
        return (LRESULT)g_brCardBg;
    }

    case WM_CTLCOLOREDIT:
        SetBkColor((HDC)wp, RGB(0xFF, 0xFF, 0xFF));
        SetTextColor((HDC)wp, COL_TEXT);
        return (LRESULT)GetStockObject(WHITE_BRUSH);

    case WM_CTLCOLORLISTBOX:
        SetBkColor((HDC)wp, RGB(0xFF, 0xFF, 0xFF));
        SetTextColor((HDC)wp, COL_TEXT);
        return (LRESULT)GetStockObject(WHITE_BRUSH);

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        // 双缓冲：先画到内存 DC，再一次性贴上去（消除闪烁）
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ oldBmp = SelectObject(mem, bmp);

        FillRect(mem, &rc, g_brBg);
        PaintCards(hwnd, mem);

        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);

        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CLOSE:
        TrayRemove();
        UnregisterHotKey(hwnd, HOTKEY_ID);
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_fUI) DeleteObject(g_fUI);
        if (g_fBold) DeleteObject(g_fBold);
        if (g_fBig) DeleteObject(g_fBig);
        if (g_fSmall) DeleteObject(g_fSmall);
        if (g_fLog) DeleteObject(g_fLog);
        if (g_fHead) DeleteObject(g_fHead);
        if (g_fMed) DeleteObject(g_fMed);
        if (g_brBg) DeleteObject(g_brBg);
        if (g_brCardBg) DeleteObject(g_brCardBg);
        PostQuitMessage((int)g_result);
        return 0;

    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------
static HFONT MakeFont(const wchar_t* face, int pt, bool bold)
{
    HDC dc = GetDC(nullptr);
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(pt, GetDeviceCaps(dc, LOGPIXELSY), 72);
    ReleaseDC(nullptr, dc);
    lf.lfWeight = bold ? FW_BOLD : FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    lf.lfFaceName[LF_FACESIZE - 1] = L'\0';
    return CreateFontIndirectW(&lf);
}

// 调试用：崩溃时把异常码写进日志文件（不影响正常流程）
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
{
    std::wstring s = Fmt(L"[!] 崩溃：异常码 0x%08X  地址 0x%p",
                         ep->ExceptionRecord->ExceptionCode,
                         ep->ExceptionRecord->ExceptionAddress);
    AppendLog(s);
    return EXCEPTION_EXECUTE_HANDLER;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int)
{
    g_inst = inst;
    SetUnhandledExceptionFilter(CrashFilter);   // 调试用，稳定后可留可去

    // --- 解析命令行 ---
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring op;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (LowerW(a) == L"--elevated") g_launchedElev = true;
        else if (LowerW(a) == L"--op" && i + 1 < argc) { op = LowerW(argv[++i]); g_autoExit = true; }
    }
    if (op == L"none") { op.clear(); g_autoExit = false; }   // GUI 版重启自己时用
    if (argv) LocalFree(argv);

    // 计算系统 DPI 缩放（manifest 声明为 system DPI aware）
    {
        HDC dc = GetDC(nullptr);
        int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
        if (dc) ReleaseDC(nullptr, dc);
        g_scale = dpi > 0 ? (double)dpi / 96.0 : 1.0;
        if (g_scale < 1.0) g_scale = 1.0;
        if (g_scale > 3.0) g_scale = 3.0;
    }

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    // 单实例：避免重复启动（提权子进程不算）
    if (!g_autoExit) {
        HANDLE mtx = CreateMutexW(nullptr, TRUE, L"DisplaySwitch.GUI.SingleInstance");
        if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND w = FindWindowW(L"DisplaySwitchWnd", nullptr);
            if (w) { ShowWindow(w, SW_SHOW); ShowWindow(w, SW_RESTORE); SetForegroundWindow(w); }
            return 0;
        }
    }

    // --- 初始化核心 ---
    g_quiet = true;
    SetOutputSink(GuiSink, nullptr);
    g_cfg = InitRuntime();
    g_elevated = IsElevated();

    // --- 字体 / 画刷 ---
    g_fUI     = MakeFont(L"Microsoft YaHei UI", 9,  false);
    g_fBold   = MakeFont(L"Microsoft YaHei UI", 9,  true);
    g_fMed    = MakeFont(L"Microsoft YaHei UI", 11, true);
    g_fBig    = MakeFont(L"Microsoft YaHei UI", 16, true);
    g_fSmall  = MakeFont(L"Microsoft YaHei UI", 8,  false);
    g_fHead   = MakeFont(L"Microsoft YaHei UI", 10, true);
    g_fLog    = MakeFont(L"Consolas", 9, false);
    g_brBg      = CreateSolidBrush(COL_BG);
    g_brCardBg  = CreateSolidBrush(COL_CARD);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = g_brBg;
    wc.lpszClassName = L"DisplaySwitchWnd";
    wc.hIcon = AppIcon();
    wc.hIconSm = AppIcon();
    if (!RegisterClassExW(&wc)) {
        MessageBoxW(nullptr, L"窗口类注册失败", L"DisplaySwitch", MB_ICONERROR);
        return 1;
    }

    // 初始窗口尺寸：按 DPI 缩放，同时不能超过当前桌面（拉伸模式下桌面会变小）
    int defW = Sc(1060), defH = Sc(760);
    int screenW = GetSystemMetrics(SM_CXSCREEN) - Sc(40);
    int screenH = GetSystemMetrics(SM_CYSCREEN) - Sc(70);
    if (defW > screenW) defW = screenW > Sc(880) ? screenW : Sc(880);
    if (defH > screenH) defH = screenH > Sc(620) ? screenH : Sc(620);

    HWND hwnd = CreateWindowExW(0, L"DisplaySwitchWnd",
                                g_autoExit ? L"DisplaySwitch - 正在执行操作"
                                           : L"DisplaySwitch - 显示模式切换",
                                WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, defW, defH,
                                nullptr, nullptr, inst, nullptr);
    if (!hwnd) {
        MessageBoxW(nullptr, L"创建窗口失败", L"DisplaySwitch", MB_ICONERROR);
        return 1;
    }

    if (g_autoExit) {
        // 提权子进程：不显示完整界面，只跑操作
        ShowWindow(hwnd, SW_SHOWMINNOACTIVE);
        RunOp(op);
    } else {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (IsDialogMessageW(hwnd, &msg)) continue;   // 让 Tab 键在控件间切换
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
