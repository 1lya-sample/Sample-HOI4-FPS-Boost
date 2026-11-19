#include "boost.h"
#include <tlhelp32.h>
#include <winternl.h>

typedef struct { NTSTATUS ExitStatus; PVOID TebBaseAddress; CLIENT_ID ClientId; KAFFINITY AffinityMask; LONG Priority; LONG BasePriority; } THREAD_BASIC_INFO;
typedef NTSTATUS(NTAPI* NtQueryInformationThread_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);

static KAFFINITY ThreadAffinity(HANDLE h)
{
    static auto q = (NtQueryInformationThread_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
    THREAD_BASIC_INFO tbi = {};
    return q && q(h, 0, &tbi, sizeof(tbi), nullptr) == 0 ? tbi.AffinityMask : 0;
}

static ULONGLONG ThreadCpuMs(HANDLE h)
{
    FILETIME c, e, k, u;
    if (!GetThreadTimes(h, &c, &e, &k, &u)) return 0;
    return (((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime)) / 10000;
}

static KAFFINITY PhysicalCoreOf(KAFFINITY cpu)
{
    DWORD len = 0;
    GetLogicalProcessorInformation(nullptr, &len);
    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> info(len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
    if (GetLogicalProcessorInformation(info.data(), &len))
        for (auto& i : info)
            if (i.Relationship == RelationProcessorCore && (i.ProcessorMask & cpu)) return i.ProcessorMask;
    return cpu;
}

static std::vector<DWORD> OwnThreads()
{
    std::vector<DWORD> tids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return tids;
    THREADENTRY32 te = { sizeof(te) };
    DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == pid) tids.push_back(te.th32ThreadID);
    CloseHandle(snap);
    return tids;
}

// The game pins its main thread to one CPU: the busiest such thread.
static DWORD FindMainThread(KAFFINITY& mask)
{
    DWORD mainTid = 0;
    ULONGLONG best = 0;
    for (DWORD tid : OwnThreads())
    {
        HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, tid);
        if (!h) continue;
        KAFFINITY m = ThreadAffinity(h);
        ULONGLONG cpu = ThreadCpuMs(h);
        if (m && (m & (m - 1)) == 0 && cpu > 5000 && cpu > best) { best = cpu; mainTid = tid; mask = m; }
        CloseHandle(h);
    }
    return mainTid;
}

void IsolationLoop()
{
    bool isolate = IniInt(L"cpu", L"isolate_main_core", 1) != 0;
    bool boost = IniInt(L"cpu", L"boost_main_priority", 1) != 0;
    if (!isolate && !boost) { Log("cpu: core isolation and priority off in hoi4boost.ini"); return; }

    DWORD mainTid = 0;
    KAFFINITY mainMask = 0;
    while (!(mainTid = FindMainThread(mainMask))) Sleep(2000);

    DWORD_PTR procMask, sysMask;
    GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask);
    KAFFINITY core = PhysicalCoreOf(mainMask);
    KAFFINITY workerMask = procMask & ~core;
    if (!workerMask) workerMask = procMask;
    Log("cpu: main thread %lu pinned to 0x%llX; its physical core 0x%llX; other threads -> 0x%llX",
        mainTid, (unsigned long long)mainMask, (unsigned long long)core, (unsigned long long)workerMask);
    if (boost)
    {
        HANDLE h = OpenThread(THREAD_SET_LIMITED_INFORMATION, FALSE, mainTid);
        BOOL ok = h && SetThreadPriority(h, THREAD_PRIORITY_ABOVE_NORMAL);
        if (h) CloseHandle(h);
        Log("cpu: main thread priority -> above normal: %s", ok ? "yes" : "failed");
    }
    if (!isolate) return;

    // the game keeps creating threads
    for (int moved = 0;; Sleep(2000))
    {
        int changed = 0;
        for (DWORD tid : OwnThreads())
        {
            if (tid == mainTid) continue;
            HANDLE h = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, tid);
            if (!h) continue;
            KAFFINITY m = ThreadAffinity(h);
            KAFFINITY n = m & workerMask;
            if (!n) n = workerMask;
            if (m && n != m && SetThreadAffinityMask(h, n)) changed++;
            CloseHandle(h);
        }
        if (changed) { moved += changed; Log("cpu: moved %d threads off the main core (%d total)", changed, moved); }
    }
}
