// =============================================================================
//  main_cli.cpp  --  控制台前端 (DisplaySwitchCLI.exe)
// =============================================================================
#include "core.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
// ===========================================================================
// 10. main
// ===========================================================================
int main(int /*argc*/, char** /*argv*/)
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleW(L"DisplaySwitch - 显示模式切换");

    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);

    std::vector<std::wstring> args;
    for (int i = 1; i < wargc; ++i) args.push_back(wargv[i]);
    if (wargv) LocalFree(wargv);

    Config cfg = InitRuntime();

    std::wstring cmd;
    bool force = false, noElevate = false;
    for (const auto& a : args) {
        std::wstring la = LowerW(a);
        if (la == L"--force")            force = true;
        else if (la == L"--hard")        g_hardReset = true;
        else if (la == L"--quiet")       g_quiet = true;
        else if (la == L"--no-elevate")  noElevate = true;
        else if (la.rfind(L"--settle=", 0) == 0) cfg.settleMs = (UINT32)_wtoi(a.c_str() + 9);
        else if (la.rfind(L"--res=", 0) == 0) {
            std::wstring v = a.substr(6);
            size_t x = LowerW(v).find(L'x');
            if (x != std::wstring::npos) { cfg.stretchW = (UINT32)_wtoi(v.substr(0, x).c_str()); cfg.stretchH = (UINT32)_wtoi(v.substr(x + 1).c_str()); }
        }
        else if (la.rfind(L"--", 0) == 0) { /* 其它开关忽略 */ }
        else if (cmd.empty())            cmd = la;
    }

    bool launchedElevated = false;
    for (const auto& a : args) if (LowerW(a) == L"--elevated") launchedElevated = true;

    if (cmd.empty() || cmd == L"help" || cmd == L"-h" || cmd == L"--help") {
        OutRaw(kUsage);
        return cmd.empty() ? EXIT_USAGE : EXIT_OK;
    }

    AppendLog(L"--- DisplaySwitch 启动: " + cmd + (launchedElevated ? L" (elevated)" : L"") + L" ---");

    int rc = EXIT_OK;
    if      (cmd == L"list")        rc = CmdList(cfg);
    else if (cmd == L"status")      rc = CmdStatus(cfg);
    else if (cmd == L"where") {
        Out(L"程序:     " + g_paths.exePath.wstring());
        Out(L"目录:     " + g_paths.dir.wstring());
        Out(L"配置文件: " + g_paths.cfg.wstring());
        Out(L"日常存档: " + g_paths.dailySnap.wstring());
        Out(L"回滚存档: " + g_paths.rollbackSnap.wstring());
        Out(L"日志:     " + g_paths.log.wstring());
        Out(Fmt(L"管理员权限: %s", IsElevated() ? L"是" : L"否"));
    }
    else if (cmd == L"save")        rc = CmdSave(cfg, force);
    else if (cmd == L"stretch" || cmd == L"go" || cmd == L"on")     rc = CmdStretch(cfg);
    else if (cmd == L"daily" || cmd == L"back" || cmd == L"off" || cmd == L"restore") rc = CmdDaily(cfg);
    else if (cmd == L"toggle")      rc = CmdToggle(cfg);
    else if (cmd == L"emergency" || cmd == L"panic") rc = CmdEmergentlyRestore();
    else if (cmd == L"nvcp")        rc = CmdOpenNvcp();
    else {
        Out(L"[!] 未知命令: " + cmd);
        OutRaw(kUsage);
        return EXIT_USAGE;
    }

    // 权限不足 -> 自动提权重跑
    if (rc == EXIT_ELEVATION && cfg.autoElevate && !noElevate && !launchedElevated) {
        Out(L"[i] 正在请求管理员权限重新执行 ...");
        std::vector<std::wstring> childArgs;
        for (const auto& a : args) if (LowerW(a) != L"--elevated") childArgs.push_back(a);
        DWORD childExit = EXIT_OK;
        if (RelaunchElevated(childArgs, childExit)) rc = (int)childExit;
    }

    AppendLog(Fmt(L"--- 结束, 退出码 %d ---", rc));

    // 被提权拉起的新窗口，出错时停一下让用户看清
    if (launchedElevated && rc != EXIT_OK) {
        Out(L"");
        Out(L"按回车键关闭窗口 ...");
        (void)getchar();
    }
    return rc;
}
