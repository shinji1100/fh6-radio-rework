// camera_re_probe - PHASE 1 (load test only)
//
// Minimal possible probe: DllMain creates one marker file on
// DLL_PROCESS_ATTACH and does nothing else. No threads, no VEH, no debug
// registers, no PAGE_GUARD, no hooks.
//
// Marker: D:\Forza Horizon 6\fh6-radio-rework\camera_re_probe_loaded.txt

#include <windows.h>

#include <cstdio>

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        if (FILE* f = std::fopen(
                "D:\\Forza Horizon 6\\fh6-radio-rework\\camera_re_probe_loaded.txt",
                "w")) {
            std::fprintf(f, "loaded pid=%lu\n", static_cast<unsigned long>(GetCurrentProcessId()));
            std::fflush(f);
            std::fclose(f);
        }
    }
    return TRUE;
}
