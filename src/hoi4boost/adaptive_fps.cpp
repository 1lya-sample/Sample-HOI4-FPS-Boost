#include "boost.h"

float* g_pacing = nullptr;

bool SetPacingInterval(float* slots, float interval)
{
    DWORD old;
    if (!VirtualProtect(slots, 12, PAGE_READWRITE, &old)) return false;
    LONG bits;
    memcpy(&bits, &interval, 4);
    for (int i = 0; i < 3; i++) InterlockedExchange((volatile LONG*)(slots + i), bits);
    VirtualProtect(slots, 12, old, &old);
    return true;
}

static bool PlayerIsActive(DWORD idleMs)
{
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (!fg || !GetWindowThreadProcessId(fg, &pid) || pid != GetCurrentProcessId()) return false;
    LASTINPUTINFO li = { sizeof(li) };
    if (GetLastInputInfo(&li) && GetTickCount() - li.dwTime < idleMs) return true;
    // a cursor resting at the edge scrolls the map without new input
    POINT c;
    RECT r;
    if (GetCursorPos(&c) && GetWindowRect(fg, &r) && PtInRect(&r, c))
    {
        const LONG edge = 8;
        if (c.x < r.left + edge || c.x >= r.right - edge || c.y < r.top + edge || c.y >= r.bottom - edge) return true;
    }
    return false;
}

DWORD WINAPI AdaptiveFpsThread(LPVOID)
{
    double fps = IniDouble(L"patches", L"target_fps", 75.0);
    double idleFps = IniDouble(L"patches", L"idle_fps", 30.0);
    DWORD idleMs = (DWORD)IniInt(L"patches", L"idle_after_ms", 2000);
    if (idleFps < 10) idleFps = 10;
    if (idleFps >= fps) { Log("adaptive-fps: idle_fps is not below target_fps, nothing to do"); return 0; }
    Log("adaptive-fps: %g FPS while you use the game, %g FPS after %lu ms without input or with the game in the background",
        fps, idleFps, idleMs);
    const float activeIv = (float)(1.0 / fps), idleIv = (float)(1.0 / idleFps);
    bool active = true;
    for (;;)
    {
        bool now = PlayerIsActive(idleMs);
        if (now != active && SetPacingInterval(g_pacing, now ? activeIv : idleIv)) active = now;
        Sleep(50);
    }
}
