#include <windows.h>

#ifdef _MSC_VER
#define FWD(name) __pragma(comment(linker, "/EXPORT:" #name "=C:\\Windows\\System32\\version." #name))
FWD(GetFileVersionInfoA)
FWD(GetFileVersionInfoByHandle)
FWD(GetFileVersionInfoExA)
FWD(GetFileVersionInfoExW)
FWD(GetFileVersionInfoSizeA)
FWD(GetFileVersionInfoSizeExA)
FWD(GetFileVersionInfoSizeExW)
FWD(GetFileVersionInfoSizeW)
FWD(GetFileVersionInfoW)
FWD(VerFindFileA)
FWD(VerFindFileW)
FWD(VerInstallFileA)
FWD(VerInstallFileW)
FWD(VerLanguageNameA)
FWD(VerLanguageNameW)
FWD(VerQueryValueA)
FWD(VerQueryValueW)
#undef FWD
#endif

namespace fh6r { void run_bridge(HMODULE self) noexcept; }
namespace {
DWORD WINAPI bridge_thread(LPVOID module) {
    fh6r::run_bridge(static_cast<HMODULE>(module));
    return 0;
}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) noexcept {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        if (HANDLE thread = CreateThread(nullptr, 0, bridge_thread, module, 0, nullptr))
            CloseHandle(thread);
    }
    return TRUE;
}
