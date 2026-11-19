#include "patcher.h"
#include <tlhelp32.h>

Image::Image(HMODULE m) : base((BYTE*)m)
{
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto sh = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
    {
        Sec s = {};
        memcpy(s.name, sh[i].Name, 8);
        s.va = sh[i].VirtualAddress;
        s.size = min(sh[i].Misc.VirtualSize, sh[i].SizeOfRawData);
        secs.push_back(s);
    }
}

const Image::Sec* Image::Find(const char* name) const
{
    for (auto& s : secs) if (strcmp(s.name, name) == 0) return &s;
    return nullptr;
}

bool Image::Contains(const BYTE* p, size_t n) const
{
    for (auto& s : secs) if (p >= base + s.va && p + n <= base + s.va + s.size) return true;
    return false;
}

static std::vector<int> ParseSig(const char* sig)
{
    std::vector<int> p;
    for (const char* c = sig; *c;)
    {
        while (*c == ' ') c++;
        if (!*c) break;
        if (c[0] == '?') { p.push_back(-1); c += 2; }
        else { p.push_back((int)strtoul(std::string(c, 2).c_str(), nullptr, 16)); c += 2; }
    }
    return p;
}

static bool MatchAt(const BYTE* at, const std::vector<int>& p)
{
    for (size_t k = 0; k < p.size(); k++) if (p[k] >= 0 && at[k] != p[k]) return false;
    return true;
}

static std::vector<BYTE*> FindAll(const Image& img, const char* section, const char* sig)
{
    std::vector<BYTE*> hits;
    auto s = img.Find(section);
    if (!s) return hits;
    auto p = ParseSig(sig);
    BYTE* start = img.base + s->va;
    BYTE* end = start + s->size - p.size();
    int first = p[0];
    for (BYTE* at = start; at <= end; at++)
    {
        if (first >= 0 && *at != first) continue;
        if (MatchAt(at, p)) hits.push_back(at);
    }
    return hits;
}

BYTE* FindOne(const Image& img, const char* sig, const char* what, std::string& err)
{
    auto h = FindAll(img, ".text", sig);
    if (h.size() != 1) { err = std::string(what) + ": signature matched " + std::to_string(h.size()) + " times (expected 1)"; return nullptr; }
    return h[0];
}

bool Matches(const Image& img, const BYTE* at, const char* sig)
{
    auto p = ParseSig(sig);
    return img.Contains(at, p.size()) && MatchAt(at, p);
}

// within rel32 reach of the exe
static BYTE* AllocNear(const BYTE* near_, size_t size)
{
    uintptr_t start = ((uintptr_t)near_ & ~(uintptr_t)0xFFFF) - 0x10000;
    for (uintptr_t a = start; a > (uintptr_t)near_ - 0x7FF00000ull; a -= 0x10000)
        if (void* p = VirtualAlloc((void*)a, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)) return (BYTE*)p;
    return nullptr;
}

bool Finalize(const Image& img, Patch& p, std::string& err)
{
    if (p.cave.empty()) return true;
    BYTE* cave = AllocNear(img.base, 0x1000);
    if (!cave) { err = "no free memory near the exe for new code"; return false; }
    memcpy(cave, p.cave.data(), p.cave.size());
    FlushInstructionCache(GetCurrentProcess(), cave, p.cave.size());
    Site& s = p.sites[0];
    s.neu.push_back(s.old[0] == 0xE8 ? 0xE8 : 0xE9);
    Put32(s.neu, (int32_t)(cave - (s.at + 5)));
    while (s.neu.size() < s.old.size()) s.neu.push_back(0x90);
    return true;
}

// Nothing may allocate while threads are suspended: one of them could hold the heap lock.
bool WriteSitesSuspended(const std::vector<Site*>& sites, bool restore)
{
    static HANDLE handles[1024];
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    std::vector<DWORD> tids;
    tids.reserve(4096);
    for (int attempt = 0; attempt < 50; attempt++)
    {
        int n = 0;
        tids.clear();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) return false;
        THREADENTRY32 te = { sizeof(te) };
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
            if (te.th32OwnerProcessID == pid && te.th32ThreadID != self) tids.push_back(te.th32ThreadID);
        CloseHandle(snap);
        for (DWORD tid : tids)
        {
            if (n >= 1024) break;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
            if (!h) continue;
            if (SuspendThread(h) == (DWORD)-1) { CloseHandle(h); continue; }
            handles[n++] = h;
        }
        bool safe = true;
        for (int i = 0; i < n && safe; i++)
        {
            CONTEXT ctx = {};
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (!GetThreadContext(handles[i], &ctx)) continue;
            for (Site* s : sites)
                if (ctx.Rip >= (DWORD64)s->at && ctx.Rip < (DWORD64)(s->at + s->old.size())) safe = false;
        }
        if (safe)
        {
            for (Site* s : sites)
            {
                const std::vector<BYTE>& b = restore ? s->old : s->neu;
                DWORD old;
                VirtualProtect(s->at, b.size(), PAGE_EXECUTE_READWRITE, &old);
                memcpy(s->at, b.data(), b.size());
                VirtualProtect(s->at, b.size(), old, &old);
                FlushInstructionCache(GetCurrentProcess(), s->at, b.size());
            }
        }
        for (int i = 0; i < n; i++) { ResumeThread(handles[i]); CloseHandle(handles[i]); }
        if (safe) return true;
        Sleep(5);
    }
    return false;
}

SiteState StateOf(const Site& s)
{
    if (memcmp(s.at, s.old.data(), s.old.size()) == 0) return SiteState::Original;
    if (!s.neu.empty() && memcmp(s.at, s.neu.data(), s.neu.size()) == 0) return SiteState::Patched;
    return SiteState::Unexpected;
}
