#include "boost.h"
#include <stdio.h>
#include <share.h>

static wchar_t g_ini[MAX_PATH];
static wchar_t g_log[MAX_PATH];

void Log(const char* fmt, ...)
{
    FILE* f = _wfsopen(g_log, L"a", _SH_DENYNO);
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d [game] ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

int IniInt(const wchar_t* sec, const wchar_t* key, int def) { return GetPrivateProfileIntW(sec, key, def, g_ini); }

double IniDouble(const wchar_t* sec, const wchar_t* key, double def)
{
    wchar_t buf[64];
    GetPrivateProfileStringW(sec, key, L"", buf, 64, g_ini);
    return buf[0] ? _wtof(buf) : def;
}

static void InitPaths(HMODULE self)
{
    wchar_t dir[MAX_PATH], logs[MAX_PATH];
    GetModuleFileNameW(self, dir, MAX_PATH);
    if (wchar_t* s = wcsrchr(dir, L'\\')) *s = 0;
    swprintf_s(g_ini, L"%s\\hoi4boost.ini", dir);
    swprintf_s(logs, L"%s\\..\\logs", dir);
    CreateDirectoryW(logs, nullptr);
    swprintf_s(g_log, L"%s\\boost.log", logs);
}

static bool IsHoi4()
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t* name = wcsrchr(exe, L'\\');
    return name && _wcsicmp(name + 1, L"hoi4.exe") == 0;
}

static DWORD WINAPI BoostThread(LPVOID)
{
    bool renderThread = wcsstr(GetCommandLineW(), L"render_thread") != nullptr;
    if (IniInt(L"patches", L"enabled", 1))
        for (auto& r : RunPatches(GetModuleHandleW(nullptr), renderThread, false))
            Log("patch %s: %s", r.name.c_str(), r.status.c_str());
    else
        Log("patches: off in hoi4boost.ini");
    if (g_pacing && IniInt(L"patches", L"adaptive_fps", 1))
    {
        if (HANDLE t = CreateThread(nullptr, 0, AdaptiveFpsThread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    else if (g_pacing)
        Log("adaptive-fps: off in hoi4boost.ini");
    IsolationLoop();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    InitPaths(module);
    if (!IsHoi4()) return TRUE;     // patchtest.exe
    // the game holds mimalloc memory and runs our code: never unload
    HMODULE pinned;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCWSTR)&DllMain, &pinned);
    Log("loaded into hoi4.exe (pid %lu)", GetCurrentProcessId());
    InstallMimalloc();
    if (HANDLE t = CreateThread(nullptr, 0, BoostThread, nullptr, 0, nullptr)) CloseHandle(t);
    return TRUE;
}

// patchtest.exe: apply, verify and revert all patches on a mapped hoi4.exe
extern "C" __declspec(dllexport) int HB_TestPatches(HMODULE target, char* report, int reportLen)
{
    std::string s;
    int applied = 0;
    for (auto& r : RunPatches(target, false, true))
    {
        s += r.name + ": " + r.status + "\n";
        if (r.status.rfind("applied", 0) == 0 && r.status.find("FAILED") == std::string::npos) applied++;
    }
    strncpy_s(report, reportLen, s.c_str(), _TRUNCATE);
    return applied;
}
