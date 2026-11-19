#include "boost.h"
#include <mimalloc.h>

typedef LPVOID(WINAPI* HeapAlloc_t)(HANDLE, DWORD, SIZE_T);
typedef BOOL(WINAPI* HeapFree_t)(HANDLE, DWORD, LPVOID);
typedef LPVOID(WINAPI* HeapReAlloc_t)(HANDLE, DWORD, LPVOID, SIZE_T);
typedef SIZE_T(WINAPI* HeapSize_t)(HANDLE, DWORD, LPCVOID);
static HANDLE g_processHeap;
static HeapAlloc_t real_HeapAlloc;
static HeapFree_t real_HeapFree;
static HeapReAlloc_t real_HeapReAlloc;
static HeapSize_t real_HeapSize;

static inline bool IsMi(const void* p) { return p != nullptr && mi_is_in_heap_region(p); }

static LPVOID WINAPI My_HeapAlloc(HANDLE heap, DWORD flags, SIZE_T size)
{
    if (heap == g_processHeap)
    {
        void* p = (flags & HEAP_ZERO_MEMORY) ? mi_zalloc(size) : mi_malloc(size);
        if (p) return p;
    }
    return real_HeapAlloc(heap, flags, size);
}

static BOOL WINAPI My_HeapFree(HANDLE heap, DWORD flags, LPVOID p)
{
    if (IsMi(p)) { mi_free(p); return TRUE; }
    return real_HeapFree(heap, flags, p);
}

static LPVOID WINAPI My_HeapReAlloc(HANDLE heap, DWORD flags, LPVOID p, SIZE_T size)
{
    if (!IsMi(p)) return real_HeapReAlloc(heap, flags, p, size);
    size_t old = mi_usable_size(p);
    void* q = (flags & HEAP_REALLOC_IN_PLACE_ONLY) ? mi_expand(p, size) : mi_realloc(p, size);
    if (q && (flags & HEAP_ZERO_MEMORY) && size > old) memset((char*)q + old, 0, size - old);
    return q;
}

static SIZE_T WINAPI My_HeapSize(HANDLE heap, DWORD flags, LPCVOID p)
{
    if (IsMi(p)) return mi_usable_size(p);
    return real_HeapSize(heap, flags, p);
}

static void* PatchImport(HMODULE module, const char* dll, const char* name, void* replacement)
{
    auto base = (BYTE*)module;
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp)
    {
        if (_stricmp((const char*)(base + imp->Name), dll) != 0) continue;
        auto names = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto slots = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots)
        {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto byName = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((const char*)byName->Name, name) != 0) continue;
            DWORD old;
            if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return nullptr;
            void* prev = (void*)slots->u1.Function;
            InterlockedExchangePointer((PVOID*)&slots->u1.Function, replacement);
            VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
            return prev;
        }
    }
    return nullptr;
}

static bool EnableLockMemoryPrivilege()
{
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return false;
    TOKEN_PRIVILEGES tp = {};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool ok = LookupPrivilegeValueW(nullptr, L"SeLockMemoryPrivilege", &tp.Privileges[0].Luid) &&
              AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return ok;
}

void InstallMimalloc()
{
    if (!IniInt(L"memory", L"mimalloc", 1)) { Log("mimalloc: off in hoi4boost.ini"); return; }
    int largePages = IniInt(L"memory", L"large_pages", 1);
    int reserveGiB = IniInt(L"memory", L"reserve_huge_gib", 0);
    bool privilege = largePages && EnableLockMemoryPrivilege();
    mi_option_set(mi_option_allow_large_os_pages, privilege ? 1 : 0);
    if (privilege && reserveGiB > 0) mi_option_set(mi_option_reserve_huge_os_pages, reserveGiB);

    g_processHeap = GetProcessHeap();
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    real_HeapAlloc = (HeapAlloc_t)GetProcAddress(k32, "HeapAlloc");
    real_HeapFree = (HeapFree_t)GetProcAddress(k32, "HeapFree");
    real_HeapReAlloc = (HeapReAlloc_t)GetProcAddress(k32, "HeapReAlloc");
    real_HeapSize = (HeapSize_t)GetProcAddress(k32, "HeapSize");
    if (!real_HeapAlloc || !real_HeapFree || !real_HeapReAlloc || !real_HeapSize) { Log("mimalloc: kernel32 heap functions not found"); return; }

    HMODULE exe = GetModuleHandleW(nullptr);
    // free/realloc/size must understand mimalloc pointers before alloc starts handing them out
    int n = 0;
    n += PatchImport(exe, "KERNEL32.dll", "HeapFree", (void*)My_HeapFree) != nullptr;
    n += PatchImport(exe, "KERNEL32.dll", "HeapSize", (void*)My_HeapSize) != nullptr;
    n += PatchImport(exe, "KERNEL32.dll", "HeapReAlloc", (void*)My_HeapReAlloc) != nullptr;
    n += PatchImport(exe, "KERNEL32.dll", "HeapAlloc", (void*)My_HeapAlloc) != nullptr;
    Log("mimalloc %d: %d of 4 heap imports redirected; large pages %s", mi_version(), n,
        !largePages ? "off in ini" : privilege ? "ON" : "unavailable (no 'Lock pages in memory' right), normal pages");
}
