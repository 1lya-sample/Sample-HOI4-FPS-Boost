#pragma once
#include <windows.h>
#include <stdint.h>
#include <string>
#include <vector>

struct Image
{
    struct Sec { char name[9]; DWORD va, size; };
    BYTE* base = nullptr;
    std::vector<Sec> secs;

    explicit Image(HMODULE m);
    const Sec* Find(const char* name) const;
    bool Contains(const BYTE* p, size_t n) const;
};

struct Site { BYTE* at; std::vector<BYTE> old, neu; };
struct Patch { std::vector<Site> sites; std::vector<BYTE> cave; };
enum class SiteState { Original, Patched, Unexpected };

// sig: hex bytes, ?? = any byte
BYTE* FindOne(const Image& img, const char* sig, const char* what, std::string& err);
bool Matches(const Image& img, const BYTE* at, const char* sig);
bool Finalize(const Image& img, Patch& p, std::string& err);
bool WriteSitesSuspended(const std::vector<Site*>& sites, bool restore);
SiteState StateOf(const Site& s);

inline void Put32(std::vector<BYTE>& v, int32_t x) { for (int i = 0; i < 4; i++) v.push_back((BYTE)(x >> (8 * i))); }
inline void Put64(std::vector<BYTE>& v, uint64_t x) { for (int i = 0; i < 8; i++) v.push_back((BYTE)(x >> (8 * i))); }
inline void Bytes(std::vector<BYTE>& v, std::initializer_list<BYTE> b) { v.insert(v.end(), b); }
inline void AbsJmp(std::vector<BYTE>& v, const BYTE* to) { Bytes(v, { 0xFF, 0x25, 0, 0, 0, 0 }); Put64(v, (uint64_t)to); }
inline int32_t I8(const BYTE* p) { return (int8_t)*p; }
inline int32_t I32(const BYTE* p) { int32_t x; memcpy(&x, p, 4); return x; }
