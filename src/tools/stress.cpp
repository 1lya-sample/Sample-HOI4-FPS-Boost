// Memory stress test for hoi4boost.dll: hoi4.exe [seconds] [threads] [pin]
// Inject the DLL while it runs to mix Windows-heap and mimalloc blocks.
#include <windows.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <vector>
#include <atomic>

static std::atomic<long long> g_errors{0}, g_ops{0};

static bool g_pin = false;   // thread 1 on CPU 6, like the game's main thread

static void Worker(int seed, int seconds)
{
    if (g_pin && seed == 1) SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << 6);
    unsigned s = seed * 2654435761u + 1;
    auto rnd = [&]() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; };
    const int N = 4096;
    std::vector<unsigned char*> blocks(N, nullptr);
    std::vector<size_t> sizes(N, 0);
    ULONGLONG end = GetTickCount64() + seconds * 1000ull;
    while (GetTickCount64() < end)
    {
        for (int k = 0; k < 1000; k++)
        {
            int i = rnd() % N;
            unsigned char tag = (unsigned char)(i * 31 + 7);
            if (blocks[i])
            {
                for (size_t j = 0; j < sizes[i]; j += 61) if (blocks[i][j] != tag) g_errors++;
                if (_msize(blocks[i]) < sizes[i]) g_errors++;
                if (rnd() % 3 == 0)
                {
                    size_t ns = (rnd() % 4 == 0) ? (rnd() % 200000) + 1 : (rnd() % 600) + 1;
                    auto p = (unsigned char*)realloc(blocks[i], ns);
                    if (!p) { g_errors++; continue; }
                    size_t keep = ns < sizes[i] ? ns : sizes[i];
                    for (size_t j = 0; j < keep; j += 61) if (p[j] != tag) g_errors++;
                    memset(p, tag, ns);
                    blocks[i] = p; sizes[i] = ns;
                }
                else { free(blocks[i]); blocks[i] = nullptr; }
            }
            else
            {
                size_t ns = (rnd() % 8 == 0) ? (rnd() % 100000) + 1 : (rnd() % 300) + 1;
                auto p = (unsigned char*)((rnd() & 1) ? calloc(1, ns) : malloc(ns));
                if (!p) { g_errors++; continue; }
                memset(p, tag, ns);
                blocks[i] = p; sizes[i] = ns;
            }
            g_ops++;
        }
    }
    for (auto p : blocks) free(p);
}

int main(int argc, char** argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 10;
    int threads = argc > 2 ? atoi(argv[2]) : 8;
    g_pin = argc > 3 && strcmp(argv[3], "pin") == 0;
    printf("pid %lu, %d threads, %d s\n", GetCurrentProcessId(), threads, seconds);
    fflush(stdout);
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; t++) ts.emplace_back(Worker, t + 1, seconds);
    for (auto& t : ts) t.join();
    HMODULE m = GetModuleHandleW(L"hoi4boost.dll");
    printf("ops %lld, errors %lld, hoi4boost.dll loaded: %s\n", g_ops.load(), g_errors.load(), m ? "yes" : "no");
    return g_errors.load() ? 1 : 0;
}
