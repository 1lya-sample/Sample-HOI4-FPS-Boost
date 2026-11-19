#include "boost.h"
#include "patcher.h"
#include <stdio.h>

// HOI4 1.19.5.0. Signatures include every offset and constant a patch relies on.
static const char* SIG_WAKE = "48 89 5C 24 10 56 48 83 EC 20 48 63 41 18 33 DB 48 8B F1 90 85 D2 75 51 44 8B C3 8B D3 85 C0 7E 25 4C 8B 0E 48 8B C8 49 8D 81 98 00 00 00 66 90 80 38 01 74 17 41 FF C0 48 FF C2 48 05 A0 00 00 00 48 3B D1 7C EA 8B 46 18 90 EB 22 49 63 C0 48 8D 0C 80 48 C1 E1 05 49 03 C9 48 8B 5C 24 38 48 83 C4 20 5E E9 ?? ?? ?? ?? 83 FA 01 75 2F 85 C0 7E 2B";
static const char* SIG_PACING = "FF 90 F0 02 00 00 F3 0F 10 53 1C 8D 4F 01 F3 0F 10 0D ?? ?? ?? ?? 33 D2 84 C0 0F 44 D1 48 8D 0D ?? ?? ?? ?? 48 63 C2 F3 0F 10 34 81";
static const char* PACING_TABLE = "B9 FC 87 3C B9 FC 87 3C B9 FC 87 3C 95 65 08 3D 95 65 08 3D CD CC 4C 3D";   // 1/60 x3, 1/30 x2, 1/20
static const char* SIG_PROVINCE = "83 BE 64 17 00 00 00 7E 04 B3 01 EB 27 84 DB 75 23 B8 1F 85 EB 51 41 F7 ED C1 FA 05 8B C2 C1 E8 1F 03 D0 6B C2 64 41 8B CD 2B C8 41 3B 8F 30 01 00 00 75 ??";
static const char* SIG_WORKER = "49 63 46 38 48 8D 0C 80 48 C1 E1 05 48 03 0E E8 ?? ?? ?? ?? 90 48 8D 4C 24 50 E8";
static const char* SIG_SLEEP = "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 85 C0 75 ?? 8B 43 4C 3D FF FF FF 7F 74 ?? 80 BB 98 00 00 00 02";
static const char* SIG_MP_PAUSE = "48 8B 4F 68 48 8B 01 FF 90 88 00 00 00 48 8B 58 40 45 33 ED 44 89 6C 24 20 4C 8D 0D ?? ?? ?? ?? 4C 8D 05 ?? ?? ?? ?? 33 D2 48 8B CB E8 ?? ?? ?? ?? 48 8D 35 ?? ?? ?? ?? 4C 8D 3D ?? ?? ?? ?? 48 85 C0 75 26 44 89 6C 24 20 4C 8D 0D ?? ?? ?? ?? 4C 8D 05 ?? ?? ?? ?? 33 D2 48 8B CB E8 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ??";

static bool BuildWake(const Image& img, Patch& p, std::string& err)
{
    BYTE* f = FindOne(img, SIG_WAKE, "wake function", err);
    if (!f) return false;
    p.sites.push_back({ f + 0x16, { 0x75, 0x51 }, { 0xEB, 0x51 } });   // test edx,edx; jne wake_all -> jmp
    p.sites.push_back({ f + 0x6C, { 0x75, 0x2F }, { 0x75, 0x00 } });   // cmp edx,1; jne exit -> fall through
    return true;
}

static bool BuildPacing(const Image& img, Patch& p, std::string& err, double fps)
{
    BYTE* c = FindOne(img, SIG_PACING, "frame pacing code", err);
    if (!c) return false;
    BYTE* lea = c + 29;
    BYTE* table = lea + 7 + I32(lea + 3);
    if (!Matches(img, table, PACING_TABLE)) { err = "frame interval table has unexpected values"; return false; }
    float iv = (float)(1.0 / fps);
    std::vector<BYTE> nv((BYTE*)&iv, (BYTE*)&iv + 4);
    p.sites.push_back({ table + 12, { 0x95, 0x65, 0x08, 0x3D }, nv });
    p.sites.push_back({ table + 16, { 0x95, 0x65, 0x08, 0x3D }, nv });
    p.sites.push_back({ table + 20, { 0xCD, 0xCC, 0x4C, 0x3D }, nv });
    return true;
}

static bool BuildProvince(const Image& img, Patch& p, std::string& err)
{
    BYTE* g = FindOne(img, SIG_PROVINCE, "province refresh", err);
    if (!g) return false;
    BYTE* site = g + 0xD;                         // test bl,bl; jne refresh; mov eax, 0x51eb851f
    BYTE* resume = site + 9;
    BYTE* refresh = site + 4 + I8(site + 3);
    BYTE* jne = g + 0x32;
    BYTE* skip = jne + 2 + I8(jne + 1);
    auto& c = p.cave;
    Bytes(c, { 0x84, 0xDB });                     // test bl, bl              (visible on screen?)
    Bytes(c, { 0x75, 0x13 });                     // jne  visible
    Bytes(c, { 0xB8, 0x1F, 0x85, 0xEB, 0x51 });   // mov  eax, 0x51eb851f     (replaced instruction)
    AbsJmp(c, resume);                            // jmp  original 1/100 round-robin check
    Bytes(c, { 0x41, 0x8B, 0x87, 0x30, 0x01, 0x00, 0x00 });  // visible: mov eax, [r15+0x130] (frame slot)
    Bytes(c, { 0x44, 0x01, 0xE8 });               // add  eax, r13d           (province id)
    Bytes(c, { 0xA8, 0x01 });                     // test al, 1
    Bytes(c, { 0x75, 0x0E });                     // jne  skip                (odd: not this frame)
    AbsJmp(c, refresh);
    AbsJmp(c, skip);
    p.sites.push_back({ site, { 0x84, 0xDB, 0x75, 0x23, 0xB8, 0x1F, 0x85, 0xEB, 0x51 }, {} });
    return true;
}

static bool BuildWorkerSpin(const Image& img, Patch& p, std::string& err, int spin)
{
    BYTE* g = FindOne(img, SIG_WORKER, "worker sleep call", err);
    if (!g) return false;
    BYTE* site = g + 0xF;                         // call sleep
    BYTE* sleep = site + 5 + I32(site + 1);
    if (!Matches(img, sleep, SIG_SLEEP)) { err = "worker sleep function looks different"; return false; }
    auto& c = p.cave;
    c.push_back(0xB8); Put32(c, spin);            // mov  eax, spin
    Bytes(c, { 0x80, 0xB9, 0x98, 0x00, 0x00, 0x00, 0x02 });  // loop: cmp byte ptr [rcx+0x98], 2 (woken?)
    Bytes(c, { 0x74, 0x06 });                     // je   done
    Bytes(c, { 0xF3, 0x90 });                     // pause
    Bytes(c, { 0xFF, 0xC8 });                     // dec  eax
    Bytes(c, { 0x75, 0xF1 });                     // jne  loop
    AbsJmp(c, sleep);                             // done: original sleep
    p.sites.push_back({ site, std::vector<BYTE>(site, site + 5), {} });
    return true;
}

// The game removes the SPACE shortcut from the pause buttons when the server is a CNetworkServer or CProxyServer.
static bool BuildMpPause(const Image& img, Patch& p, std::string& err)
{
    BYTE* g = FindOne(img, SIG_MP_PAUSE, "multiplayer pause shortcut check", err);
    if (!g) return false;
    BYTE* name = g + 0x38 + I32(g + 0x34);        // lea rsi, "pause_button_date"
    BYTE* type = g + 0x20 + I32(g + 0x1C);        // lea r9, RTTI type descriptor (name at +0x10)
    if (!img.Contains(name, 18) || memcmp(name, "pause_button_date", 18) != 0) { err = "not the pause buttons"; return false; }
    if (!img.Contains(type + 0x10, 21) || memcmp(type + 0x10, ".?AVCNetworkServer@@", 21) != 0) { err = "not the multiplayer check"; return false; }
    p.sites.push_back({ g + 0x42, { 0x75, 0x26 }, { 0x90, 0x90 } });   // CNetworkServer: jne remove_shortcut -> nop
    p.sites.push_back({ g + 0x64, { 0x0F, 0x84 }, { 0x90, 0xE9 } });   // CProxyServer: je keep -> jmp keep
    return true;
}

std::vector<PatchResult> RunPatches(HMODULE module, bool renderThread, bool revertAfter)
{
    std::vector<PatchResult> out;
    Image img(module);
    double fps = IniDouble(L"patches", L"target_fps", 75.0);
    int spin = IniInt(L"patches", L"spin_iters", 2000);
    char pacingName[64];
    sprintf_s(pacingName, "frame-pacing-%gfps", fps);
    struct Def { const wchar_t* section; const wchar_t* key; std::string name; };
    Def defs[] = { { L"patches", L"wake_all_workers", "wake-all-workers" }, { L"patches", L"frame_pacing", pacingName },
                   { L"patches", L"province_throttle", "province-throttle" }, { L"patches", L"worker_spin", "worker-spin" },
                   { L"controls", L"mp_space_pause", "mp-space-pause" } };
    for (int i = 0; i < 5; i++)
    {
        PatchResult r{ defs[i].name, "" };
        if (!IniInt(defs[i].section, defs[i].key, 1)) { r.status = "off in hoi4boost.ini"; out.push_back(r); continue; }
        // with -render_thread mid-tick frames run on a worker thread and crash
        if (i == 1 && renderThread) { r.status = "skipped: not safe together with -render_thread"; out.push_back(r); continue; }
        Patch p;
        std::string err;
        bool ok = i == 0 ? BuildWake(img, p, err) : i == 1 ? BuildPacing(img, p, err, fps)
                : i == 2 ? BuildProvince(img, p, err) : i == 3 ? BuildWorkerSpin(img, p, err, spin)
                : BuildMpPause(img, p, err);
        if (!ok) { r.status = "not found in this game build, skipped (" + err + ")"; out.push_back(r); continue; }
        bool allOriginal = true;
        for (auto& s : p.sites) if (StateOf(s) != SiteState::Original) allOriginal = false;
        if (!allOriginal) { r.status = "unexpected bytes at the patch site, skipped"; out.push_back(r); continue; }
        if (!Finalize(img, p, err)) { r.status = "skipped (" + err + ")"; out.push_back(r); continue; }
        std::vector<Site*> sp;
        for (auto& s : p.sites) sp.push_back(&s);
        char where[256] = "";
        for (auto& s : p.sites) { char b[32]; sprintf_s(b, "%s0x%llX", where[0] ? ", " : "", (unsigned long long)(s.at - img.base)); strcat_s(where, b); }
        if (!WriteSitesSuspended(sp, false)) { r.status = "skipped (no safe moment to write)"; out.push_back(r); continue; }
        bool written = true;
        for (auto& s : p.sites) if (StateOf(s) != SiteState::Patched) written = false;
        r.status = std::string(written ? "applied" : "WRITE CHECK FAILED") + " (at " + where + ")";
        if (i == 1 && written)
        {
            float* slots = (float*)p.sites[0].at;
            if (!revertAfter) g_pacing = slots;
            else
            {
                bool ok = SetPacingInterval(slots, 1.0f / 30) && slots[0] == 1.0f / 30 && slots[2] == 1.0f / 30 &&
                          SetPacingInterval(slots, (float)(1.0 / fps));
                for (auto& s : p.sites) if (StateOf(s) != SiteState::Patched) ok = false;
                r.status += ok ? ", adaptive switch ok" : ", ADAPTIVE SWITCH FAILED";
            }
        }
        if (revertAfter)
        {
            WriteSitesSuspended(sp, true);
            bool back = true;
            for (auto& s : p.sites) if (StateOf(s) != SiteState::Original) back = false;
            r.status += back ? ", reverted" : ", REVERT CHECK FAILED";
            if (!p.cave.empty())
            {
                char b[64];
                sprintf_s(b, "\n  base=%llX site=%llX bytes=", (unsigned long long)img.base, (unsigned long long)p.sites[0].at);
                r.status += b;
                for (BYTE x : p.sites[0].neu) { sprintf_s(b, "%02x", x); r.status += b; }
                r.status += " cave=";
                for (BYTE x : p.cave) { sprintf_s(b, "%02x", x); r.status += b; }
            }
        }
        out.push_back(r);
    }
    return out;
}
