// ---------------------------------------------------------------------------
// probe_ccd.exe —— 只读诊断工具（全部用 SDC_VALIDATE，绝不改动显示设置）
//
// 用途：
//   1. 打印当前活动的显示路径（适配器 LUID / source id / target id / 分辨率）
//   2. 直接读 daily.ccd，用 SDC_VALIDATE 校验它还能不能被 SetDisplayConfig 接受
//   3. 把快照里的旧 LUID 替换成当前 LUID 后再校验一次 —— 用来证明
//      「存档跨重启失效」就是回日常失败的根因
//   4. 顺便校验 SDC_TOPOLOGY_EXTEND 的各种 flag 组合
// ---------------------------------------------------------------------------
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>

#pragma pack(push, 1)
struct SnapHeader {
    char     magic[8];
    uint32_t version, numPaths, numModes, pathSize, modeSize, payloadBytes;
    uint64_t payloadHash;
};
#pragma pack(pop)

static LONG Validate(UINT32 np, DISPLAYCONFIG_PATH_INFO* p,
                     UINT32 nm, DISPLAYCONFIG_MODE_INFO* m, UINT32 flags)
{
    return SetDisplayConfig(np, p, nm, m, flags | SDC_VALIDATE);
}

static void PrintErr(const char* what, LONG r)
{
    LPWSTR msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, (DWORD)r,
                   MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&msg, 0, nullptr);
    printf("  %-58s -> %-4ld %ls\n", what, r, msg ? msg : L"");
    if (msg) LocalFree(msg);
}

static bool LoadSnap(const char* file, std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
                     std::vector<DISPLAYCONFIG_MODE_INFO>& modes)
{
    std::ifstream f(file, std::ios::binary);
    if (!f) { printf("  (打不开 %s)\n", file); return false; }
    SnapHeader hd{};
    f.read((char*)&hd, sizeof(hd));
    if (memcmp(hd.magic, "DSSNAP02", 8) != 0) { printf("  (magic 不对)\n"); return false; }
    if (hd.pathSize != sizeof(DISPLAYCONFIG_PATH_INFO) ||
        hd.modeSize != sizeof(DISPLAYCONFIG_MODE_INFO)) { printf("  (结构尺寸不匹配)\n"); return false; }
    size_t pb = (size_t)hd.numPaths * hd.pathSize, mb = (size_t)hd.numModes * hd.modeSize;
    std::vector<uint8_t> buf(pb + mb);
    f.read((char*)buf.data(), (std::streamsize)buf.size());
    paths.assign(hd.numPaths, DISPLAYCONFIG_PATH_INFO{});
    modes.assign(hd.numModes, DISPLAYCONFIG_MODE_INFO{});
    if (pb) memcpy(paths.data(), buf.data(), pb);
    if (mb) memcpy(modes.data(), buf.data() + pb, mb);
    printf("  读入 %s : %u path / %u mode\n", file, hd.numPaths, hd.numModes);
    return true;
}

int main()
{
    printf("================ CCD 诊断 ================\n\n");

    // ---- 1. 当前活动配置 -------------------------------------------------
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) {
        printf("[!] 读不到显示配置（可能不在交互式会话里）\n");
        return 1;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> liveP(np ? np : 1);
    std::vector<DISPLAYCONFIG_MODE_INFO> liveM(nm ? nm : 1);
    LONG r = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, liveP.data(), &nm, liveM.data(), nullptr);
    if (r != ERROR_SUCCESS) { printf("[!] QueryDisplayConfig 失败: %ld\n", r); return 1; }
    liveP.resize(np); liveM.resize(nm);

    printf("[1] 当前活动路径 (%u 条)\n", np);
    LUID liveLuid{};
    for (UINT32 i = 0; i < np; ++i) {
        const auto& q = liveP[i];
        liveLuid = q.sourceInfo.adapterId;
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sn{};
        sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sn.header.size = sizeof(sn);
        sn.header.adapterId = q.sourceInfo.adapterId;
        sn.header.id = q.sourceInfo.id;
        wchar_t gdi[64] = L"?";
        if (DisplayConfigGetDeviceInfo(&sn.header) == ERROR_SUCCESS) wcscpy(gdi, sn.viewGdiDeviceName);
        printf("    path[%u] LUID=0x%08X%08X srcId=%u tgtId=%u  %ls  flags=0x%X\n",
               i, (unsigned)q.sourceInfo.adapterId.HighPart, (unsigned)q.sourceInfo.adapterId.LowPart,
               q.sourceInfo.id, q.targetInfo.id, gdi, q.flags);
    }
    printf("\n");

    // ---- 2. 拓扑 flag 组合校验 -------------------------------------------
    printf("[2] 拓扑调用 flag 组合校验（SDC_VALIDATE，不改设置）\n");
    PrintErr("SDC_TOPOLOGY_EXTEND | SDC_VALIDATE",
             Validate(0, nullptr, 0, nullptr, SDC_TOPOLOGY_EXTEND));
    PrintErr("SDC_TOPOLOGY_EXTEND | SDC_VALIDATE | SDC_SAVE_TO_DATABASE",
             Validate(0, nullptr, 0, nullptr, SDC_TOPOLOGY_EXTEND | SDC_SAVE_TO_DATABASE));
    PrintErr("SDC_TOPOLOGY_EXTEND | SDC_VALIDATE | SDC_NO_OPTIMIZATION",
             Validate(0, nullptr, 0, nullptr, SDC_TOPOLOGY_EXTEND | SDC_NO_OPTIMIZATION));
    PrintErr("SDC_TOPOLOGY_INTERNAL | SDC_VALIDATE | SDC_SAVE_TO_DATABASE",
             Validate(0, nullptr, 0, nullptr, SDC_TOPOLOGY_INTERNAL | SDC_SAVE_TO_DATABASE));
    printf("\n");

    // ---- 3. daily.ccd 原样校验 -------------------------------------------
    printf("[3] daily.ccd 原样校验（存档里的 LUID 是否还有效）\n");
    std::vector<DISPLAYCONFIG_PATH_INFO> sp;
    std::vector<DISPLAYCONFIG_MODE_INFO> sm;
    if (LoadSnap("daily.ccd", sp, sm)) {
        for (size_t i = 0; i < sp.size(); ++i)
            printf("    snap path[%zu] LUID=0x%08X%08X (当前=0x%08X%08X) %s\n", i,
                   (unsigned)sp[i].sourceInfo.adapterId.HighPart, (unsigned)sp[i].sourceInfo.adapterId.LowPart,
                   (unsigned)liveLuid.HighPart, (unsigned)liveLuid.LowPart,
                   sp[i].sourceInfo.adapterId.LowPart == liveLuid.LowPart &&
                   sp[i].sourceInfo.adapterId.HighPart == liveLuid.HighPart ? "[一致]" : "[*** 不一致 ***]");
        PrintErr("快照原样 SDC_USE_SUPPLIED_DISPLAY_CONFIG",
                 Validate((UINT32)sp.size(), sp.data(), (UINT32)sm.size(), sm.data(),
                          SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE));
        PrintErr("快照原样 + ALLOW_CHANGES/PERSIST",
                 Validate((UINT32)sp.size(), sp.data(), (UINT32)sm.size(), sm.data(),
                          SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE |
                          SDC_ALLOW_CHANGES | SDC_PATH_PERSIST_IF_REQUIRED |
                          SDC_ALLOW_PATH_ORDER_CHANGES));

        // ---- 4. 把旧 LUID 换成当前 LUID 再校验 ---------------------------
        printf("\n[4] 把快照里的 LUID 替换成当前 LUID 后校验\n");
        for (auto& q : sp) {
            q.sourceInfo.adapterId = liveLuid;
            q.targetInfo.adapterId = liveLuid;
        }
        for (auto& m : sm) m.adapterId = liveLuid;
        PrintErr("换 LUID 后 USE_SUPPLIED | SAVE_TO_DATABASE",
                 Validate((UINT32)sp.size(), sp.data(), (UINT32)sm.size(), sm.data(),
                          SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE));
        PrintErr("换 LUID 后 USE_SUPPLIED (不写数据库)",
                 Validate((UINT32)sp.size(), sp.data(), (UINT32)sm.size(), sm.data(),
                          SDC_USE_SUPPLIED_DISPLAY_CONFIG));
        PrintErr("换 LUID 后 USE_SUPPLIED | SAVE_TO_DATABASE | ALLOW_CHANGES",
                 Validate((UINT32)sp.size(), sp.data(), (UINT32)sm.size(), sm.data(),
                          SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE | SDC_ALLOW_CHANGES));
        PrintErr("换 LUID 后 USE_SUPPLIED | SAVE_TO_DATABASE | PERSIST_IF_REQUIRED",
                 Validate((UINT32)sp.size(), sp.data(), (UINT32)sm.size(), sm.data(),
                          SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE |
                          SDC_PATH_PERSIST_IF_REQUIRED));
        PrintErr("换 LUID 后 USE_SUPPLIED | SAVE_TO_DATABASE | ALLOW_PATH_ORDER_CHANGES",
                 Validate((UINT32)sp.size(), sp.data(), (UINT32)sm.size(), sm.data(),
                          SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE |
                          SDC_ALLOW_PATH_ORDER_CHANGES));
        PrintErr("换 LUID 后 TOPOLOGY_SUPPLIED | SAVE_TO_DATABASE",
                 Validate(0, nullptr, 0, nullptr,
                          SDC_TOPOLOGY_SUPPLIED | SDC_SAVE_TO_DATABASE));
    }

    printf("\n[5] 其它拓扑 (SDC_VALIDATE)\n");
    PrintErr("SDC_TOPOLOGY_INTERNAL | SDC_VALIDATE",
             Validate(0, nullptr, 0, nullptr, SDC_TOPOLOGY_INTERNAL));
    PrintErr("SDC_TOPOLOGY_CLONE | SDC_VALIDATE",
             Validate(0, nullptr, 0, nullptr, SDC_TOPOLOGY_CLONE));
    PrintErr("SDC_TOPOLOGY_EXTERNAL | SDC_VALIDATE",
             Validate(0, nullptr, 0, nullptr, SDC_TOPOLOGY_EXTERNAL));
    printf("\n================ 诊断结束 ================\n");
    return 0;
}
