// =============================================================================
//  core.h  --  DisplaySwitch 公共核心（控制台版与图形版共用）
// -----------------------------------------------------------------------------
//  本模块把「显示配置 / 分辨率 / 监视器设备」的全部操作封装成可复用函数，
//  由两个前端调用：
//      main_cli.cpp  ->  DisplaySwitchCLI.exe   （控制台，供脚本与应急使用）
//      main_gui.cpp  ->  DisplaySwitch.exe      （Win32 图形界面，日常使用）
//
//  输出重定向：核心代码统一通过 Out() 打印进度。控制台版输出到 stdout，
//  图形版通过 SetOutputSink() 注册回调，把日志送进窗口里的日志框。
// =============================================================================
#pragma once

#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// 退出码
// ---------------------------------------------------------------------------
enum ExitCode {
    EXIT_OK            = 0,   // 成功
    EXIT_USAGE         = 1,   // 命令行用法错误
    EXIT_PRECHECK      = 2,   // 前置条件不满足（缺少存档 / 分辨率不存在等）
    EXIT_APPLY_FAILED  = 3,   // 应用失败，但已成功回滚
    EXIT_ROLLBACK_FAIL = 4,   // 应用失败且回滚也失败（严重）
    EXIT_ELEVATION     = 5    // 需要管理员权限但被拒绝
};

// ---------------------------------------------------------------------------
// 路径 / 配置
// ---------------------------------------------------------------------------
struct AppPaths {
    fs::path exePath;
    fs::path dir;
    fs::path cfg;
    fs::path dailySnap;
    fs::path dailyTxt;
    fs::path dailyMon;
    fs::path rollbackSnap;
    fs::path rollbackTxt;
    fs::path rollbackMon;
    fs::path log;
};

extern AppPaths g_paths;

struct Config {
    UINT32 stretchW          = 1280;
    UINT32 stretchH          = 882;
    UINT32 stretchHz         = 0;      // 0 = 自动取该分辨率下最高刷新率
    std::wstring stretchTarget = L"internal";  // internal | external | auto
    bool   keepExternalOnly  = true;
    UINT32 settleMs          = 1500;
    bool   logEnabled        = true;
    bool   autoElevate       = true;
    bool   openNvcpOnMissing = true;
    bool   stretchDisableMonitor = true;
    bool   dailyEnableMonitor   = false;   // false = 按 daily.mon 存档还原（更安全）
    std::wstring internalMatch;
    std::wstring monitorMatch;
    std::wstring hotkey;               // 全局热键，如 Ctrl+Alt+S；空=不注册（仅图形版用）
};

// ---------------------------------------------------------------------------
// 显示配置快照
// ---------------------------------------------------------------------------
struct Snapshot {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    bool empty() const { return paths.empty(); }
};

struct DisplayRow {   // 一条显示器记录（可读说明 / 界面展示都用它）
    std::wstring gdi, monitor, technology;
    std::wstring monitorDevicePath;
    UINT32 targetId = 0;
    bool   active = false;
    bool   internal = false;
    UINT32 width = 0, height = 0, refresh = 0;
    LONG   x = 0, y = 0;
};

struct LiveState {
    Snapshot snap;
    std::vector<DisplayRow> rows;
    bool internalActive = false;
    int  internalCount  = 0;
    int  externalActive = 0;
    std::wstring externalGdi;
    UINT32 externalW = 0, externalH = 0, externalHz = 0;
    std::wstring internalGdi;
    UINT32 internalW = 0, internalH = 0, internalHz = 0;
};

struct GdiMode { UINT32 w = 0, h = 0, bpp = 0, hz = 0; };

struct MonitorDev {
    std::wstring instanceId;
    std::wstring hardwareId;
    std::wstring desc;
    DEVINST      devInst     = 0;
    bool         started     = false;
    bool         disabled    = false;
    bool         disableable = false;
    ULONG        problem     = 0;
};

// ---------------------------------------------------------------------------
// 全局开关（由前端设置）
// ---------------------------------------------------------------------------
extern bool g_quiet;        // true = 不往 stdout 打印（图形版设为 true）
extern bool g_hardReset;    // --hard
extern bool g_logEnabled;   // 是否写 DisplaySwitch.log

// 帮助文本（控制台版使用）
extern const wchar_t* kUsage;

// ---------------------------------------------------------------------------
// 输出重定向
// ---------------------------------------------------------------------------
using OutputSink = void (*)(const std::wstring& line, void* user);
void SetOutputSink(OutputSink sink, void* user);

// ---------------------------------------------------------------------------
// 初始化 / 配置读写
// ---------------------------------------------------------------------------
void   ResolvePaths();                       // 计算 g_paths
void   CreateDefaultCfgIfMissing();          // 首次运行生成 display.cfg
Config LoadCfg();                            // 读 display.cfg
bool   SaveCfg(const Config& c, std::wstring& err);   // 写 display.cfg
Config InitRuntime();                        // ResolvePaths + 默认配置 + LoadCfg + 应用全局开关

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------
std::string  Narrow(const std::wstring& w);
std::wstring Widen(const std::string& s);
std::wstring TrimW(const std::wstring& s);
std::wstring LowerW(std::wstring s);
std::wstring Fmt(const wchar_t* fmt, ...);
std::wstring NowStamp();
void         Out(const std::wstring& line);
void         OutRaw(const std::wstring& line);
void         AppendLog(const std::wstring& line);   // 只写日志文件，不输出

// 显示器匹配关键字（必须在任何显示器枚举之前调用）
void SetConfigMatches(const std::wstring& internalMatch, const std::wstring& monitorMatch);

// ---------------------------------------------------------------------------
// 显示 / 分辨率 / 监视器设备
// ---------------------------------------------------------------------------
std::wstring TechName(UINT32 tech);
bool         IsInternalDisplay(UINT32 tech, const std::wstring& monitorName);
std::vector<DisplayRow> DescribeSnapshot(const Snapshot& s);

LONG QueryConfig(UINT32 flags, std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
                 std::vector<DISPLAYCONFIG_MODE_INFO>& modes);

std::vector<GdiMode> EnumGdiModes(const std::wstring& gdiName);
bool                 PickGdiMode(const std::vector<GdiMode>& modes, UINT32 w, UINT32 h,
                                 UINT32 wantHz, GdiMode& best);
std::wstring         DispChangeText(LONG code);
std::wstring         ListResolutions(const std::vector<GdiMode>& modes, size_t maxShow = 24);

std::vector<MonitorDev> EnumMonitorDevices();
int                     MatchMonitorDev(const std::vector<MonitorDev>& devs, const DisplayRow& row);
bool                    SetMonitorDisabled(const MonitorDev& m, bool disable, std::wstring& err);

bool ReadLive(LiveState& ls);
bool LiveMatches(const Snapshot& want, const LiveState& live);

bool LoadSnapshotFile(const fs::path& file, Snapshot& s, std::wstring& err);

// ---------------------------------------------------------------------------
// 各命令（返回值即退出码）
// ---------------------------------------------------------------------------
int CmdList(const Config& cfg);
int CmdStretch(const Config& cfg);
int CmdDaily(const Config& cfg);
int CmdToggle(const Config& cfg);
int CmdSave(const Config& cfg, bool force);
int CmdStatus(const Config& cfg);
int CmdEmergentlyRestore();
int CmdOpenNvcp();

// ---------------------------------------------------------------------------
// 提权
// ---------------------------------------------------------------------------
bool IsElevated();
bool RelaunchElevated(const std::vector<std::wstring>& args, DWORD& childExit, bool wait = true);
