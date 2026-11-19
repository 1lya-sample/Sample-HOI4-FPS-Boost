#pragma once
#include <windows.h>
#include <string>
#include <vector>

// main.cpp
void Log(const char* fmt, ...);
int IniInt(const wchar_t* sec, const wchar_t* key, int def);
double IniDouble(const wchar_t* sec, const wchar_t* key, double def);

// memory.cpp
void InstallMimalloc();

// patches.cpp
struct PatchResult { std::string name, status; };
std::vector<PatchResult> RunPatches(HMODULE module, bool renderThread, bool revertAfter);

// adaptive_fps.cpp
extern float* g_pacing;     // frame intervals of speeds 3..5, set once frame pacing is applied
bool SetPacingInterval(float* slots, float interval);
DWORD WINAPI AdaptiveFpsThread(LPVOID);

// cpu.cpp
void IsolationLoop();
