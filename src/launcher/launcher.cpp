#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <stdio.h>
#include <share.h>
#include <wchar.h>

static wchar_t g_dir[MAX_PATH], g_log[MAX_PATH], g_dll[MAX_PATH];

static void Log(const char* fmt, ...)
{
    FILE* f = _wfsopen(g_log, L"a", _SH_DENYNO);
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d [launcher] ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static const wchar_t* ArgsAfterProgram()
{
    const wchar_t* c = GetCommandLineW();
    if (*c == L'"') { c++; while (*c && *c != L'"') c++; if (*c) c++; }
    else while (*c && *c != L' ' && *c != L'\t') c++;
    while (*c == L' ' || *c == L'\t') c++;
    return c;
}

static DWORD FindHoi4()
{
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = { sizeof(pe) };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, L"hoi4.exe") == 0) { pid = pe.th32ProcessID; break; }
    CloseHandle(snap);
    return pid;
}

static bool HasModule(DWORD pid, const wchar_t* name)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me = { sizeof(me) };
    bool found = false;
    for (BOOL ok = Module32FirstW(snap, &me); ok && !found; ok = Module32NextW(snap, &me))
        found = _wcsicmp(me.szModule, name) == 0;
    CloseHandle(snap);
    return found;
}

static double ProcessAgeSeconds(HANDLE h)
{
    FILETIME c, e, k, u, now;
    if (!GetProcessTimes(h, &c, &e, &k, &u)) return 0;
    GetSystemTimeAsFileTime(&now);
    ULONGLONG a = (ULONGLONG)c.dwHighDateTime << 32 | c.dwLowDateTime;
    ULONGLONG b = (ULONGLONG)now.dwHighDateTime << 32 | now.dwLowDateTime;
    return b > a ? (b - a) / 1e7 : 0;
}

// kernel32 has the same address in every process, so our LoadLibraryW is valid in the game
static bool Inject(HANDLE proc, const wchar_t* dll)
{
    size_t bytes = (wcslen(dll) + 1) * sizeof(wchar_t);
    void* mem = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) { Log("inject: VirtualAllocEx failed (%lu)", GetLastError()); return false; }
    if (!WriteProcessMemory(proc, mem, dll, bytes, nullptr)) { Log("inject: WriteProcessMemory failed (%lu)", GetLastError()); return false; }
    auto loadLibrary = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE t = CreateRemoteThread(proc, nullptr, 0, loadLibrary, mem, 0, nullptr);
    if (!t) { Log("inject: CreateRemoteThread failed (%lu)", GetLastError()); return false; }
    DWORD wait = WaitForSingleObject(t, 20000);
    DWORD code = 0;
    GetExitCodeThread(t, &code);
    CloseHandle(t);
    if (wait != WAIT_OBJECT_0) { Log("inject: LoadLibrary in the game did not finish in 20 s"); return false; }
    if (!code) { Log("inject: LoadLibrary failed inside the game"); return false; }
    return true;
}

enum class BoostResult { Loaded, AlreadyLoaded, Failed };

static BoostResult Boost(DWORD waitMs, const char*& why)
{
    why = "";
    if (GetFileAttributesW(g_dll) == INVALID_FILE_ATTRIBUTES) { why = "hoi4boost.dll not found next to launcher.exe"; Log("%s", why); return BoostResult::Failed; }
    ULONGLONG deadline = GetTickCount64() + waitMs;
    DWORD pid = 0;
    while (!(pid = FindHoi4()))
    {
        if (GetTickCount64() > deadline) { why = "hoi4.exe is not running"; Log("hoi4.exe did not start within %lu minutes", waitMs / 60000); return BoostResult::Failed; }
        Sleep(250);
    }
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ |
                              PROCESS_QUERY_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!proc) { why = "cannot open hoi4.exe"; Log("cannot open hoi4.exe (pid %lu): error %lu", pid, GetLastError()); return BoostResult::Failed; }
    // let the Windows loader finish first
    while (ProcessAgeSeconds(proc) < 1.5) Sleep(100);
    BoostResult r = BoostResult::Failed;
    if (WaitForSingleObject(proc, 0) == WAIT_OBJECT_0) { why = "hoi4.exe exited"; Log("hoi4.exe exited before the boost was loaded"); }
    else if (HasModule(pid, L"hoi4boost.dll")) { Log("hoi4.exe (pid %lu): hoi4boost.dll is already loaded", pid); r = BoostResult::AlreadyLoaded; }
    else if (Inject(proc, g_dll)) { Log("hoi4.exe (pid %lu): hoi4boost.dll loaded", pid); r = BoostResult::Loaded; }
    else why = "loading the DLL into the game failed";
    CloseHandle(proc);
    return r;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    GetModuleFileNameW(nullptr, g_dir, MAX_PATH);
    if (wchar_t* s = wcsrchr(g_dir, L'\\')) *s = 0;
    wchar_t logs[MAX_PATH];
    swprintf_s(logs, L"%s\\logs", g_dir);
    CreateDirectoryW(logs, nullptr);
    swprintf_s(g_log, L"%s\\boost.log", logs);
    swprintf_s(g_dll, L"%s\\hoi4boost.dll", g_dir);

    const wchar_t* cmd = ArgsAfterProgram();
    PROCESS_INFORMATION child = {};
    bool attach = false;
    if (*cmd)
    {
        // Steam launch options: run Steam's command and wait for it, as Steam expects
        wchar_t* buf = _wcsdup(cmd);
        STARTUPINFOW si = { sizeof(si) };
        if (!CreateProcessW(nullptr, buf, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &child))
            Log("could not run the Steam command (error %lu): %ls", GetLastError(), cmd);
        free(buf);
    }
    else if (FindHoi4())
    {
        attach = true;
        Log("the game is already running: connecting to it");
    }
    else
    {
        Log("starting Hearts of Iron IV through Steam");
        ShellExecuteW(nullptr, L"open", L"steam://rungameid/394360", nullptr, nullptr, SW_SHOWNORMAL);
    }

    // double-click and Steam launch options may both run us
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\SampleHOI4FPSBoost");
    bool busy = GetLastError() == ERROR_ALREADY_EXISTS;
    const char* why = "";
    BoostResult r = BoostResult::Failed;
    if (busy) Log("another launcher is already waiting for the game");
    else r = Boost(attach ? 5000 : 15 * 60 * 1000, why);
    if (mutex) { if (!busy) ReleaseMutex(mutex); CloseHandle(mutex); }

    if (attach && !GetEnvironmentVariableW(L"HOI4BOOST_QUIET", nullptr, 0))
    {
        wchar_t text[512];
        if (r == BoostResult::Loaded)
            wcscpy_s(text, L"Sample HOI4 FPS Boost is now connected to the running game.\n\n"
                           L"The optimizations are already working; space-to-pause in multiplayer starts working after the next save is loaded.\n"
                           L"Details: dist\\logs\\boost.log");
        else if (r == BoostResult::AlreadyLoaded)
            wcscpy_s(text, L"Sample HOI4 FPS Boost is already running in this game.");
        else if (busy)
            wcscpy_s(text, L"Another launcher is already connecting to the game.");
        else
            swprintf_s(text, L"Could not connect to the game: %hs.\n\nDetails: dist\\logs\\boost.log", why);
        MessageBoxW(nullptr, text, L"Sample HOI4 FPS Boost",
                    MB_OK | MB_SETFOREGROUND | MB_TOPMOST | (r == BoostResult::Failed ? MB_ICONWARNING : MB_ICONINFORMATION));
    }

    DWORD exitCode = 0;
    if (child.hProcess)
    {
        WaitForSingleObject(child.hProcess, INFINITE);
        GetExitCodeProcess(child.hProcess, &exitCode);
        CloseHandle(child.hProcess);
        CloseHandle(child.hThread);
    }
    return (int)exitCode;
}
