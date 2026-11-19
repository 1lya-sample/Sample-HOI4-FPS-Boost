// patchtest.exe [path\to\hoi4.exe]
#include <windows.h>
#include <stdio.h>

int wmain(int argc, wchar_t** argv)
{
    const wchar_t* exe = argc > 1 ? argv[1] : L"D:\\SteamLibrary\\steamapps\\common\\Hearts of Iron IV\\hoi4.exe";
    wchar_t dll[MAX_PATH];
    GetModuleFileNameW(nullptr, dll, MAX_PATH);
    if (wchar_t* s = wcsrchr(dll, L'\\')) *s = 0;
    wcscat_s(dll, L"\\..\\..\\bin\\hoi4boost.dll");

    HMODULE boost = LoadLibraryW(dll);
    if (!boost) { wprintf(L"cannot load %ls (error %lu)\n", dll, GetLastError()); return 2; }
    auto test = (int(*)(HMODULE, char*, int))GetProcAddress(boost, "HB_TestPatches");
    HMODULE image = LoadLibraryExW(exe, nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!test || !image) { wprintf(L"cannot map %ls (error %lu)\n", exe, GetLastError()); return 2; }
    static char report[8192];
    int applied = test(image, report, sizeof(report));
    wprintf(L"%ls\n", exe);
    printf("%s", report);
    printf("%d patch(es) applied, verified and reverted\n", applied);
    return 0;
}
