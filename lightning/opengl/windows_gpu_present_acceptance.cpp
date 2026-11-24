/* Module: lightning.opengl.windows_gpu_present_acceptance
 * Purpose: Require visible hardware WGL frames with the CPU WSI routes disabled.
 * Created: 2026-11-08
 * Architecture: Acceptance-only import guards in the private native ICD. Pixel
 * observations are test oracles; production presentation never reads them back.
 */
#define QL_OPENGL_ACCEPTANCE_Z_ORDER HWND_TOPMOST
#define main opengl_acceptance_main
#include "acceptance.cpp"
#undef main

namespace {
volatile LONG forbidden_gdi_calls = 0;
BOOL WINAPI reject_stretch(HDC, int, int, int, int, HDC, int, int, int, int, DWORD) {
    InterlockedIncrement(&forbidden_gdi_calls);
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}
HBITMAP WINAPI reject_dib(HDC, const BITMAPINFO *, UINT, void **, HANDLE, DWORD) {
    InterlockedIncrement(&forbidden_gdi_calls);
    SetLastError(ERROR_NOT_SUPPORTED);
    return nullptr;
}
bool guard_cpu_presentation(HMODULE module) {
    auto *base = reinterpret_cast<BYTE *>(module);
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const auto directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress) return false;
    unsigned guarded = 0;
    auto *imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + directory.VirtualAddress);
    for (; imports->Name; ++imports) {
        if (!imports->OriginalFirstThunk) continue;
        auto *names = reinterpret_cast<IMAGE_THUNK_DATA *>(base + imports->OriginalFirstThunk);
        auto *addresses = reinterpret_cast<IMAGE_THUNK_DATA *>(base + imports->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addresses) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto *entry = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData);
            uintptr_t replacement = 0;
            if (!std::strcmp(reinterpret_cast<const char *>(entry->Name), "StretchBlt"))
                replacement = reinterpret_cast<uintptr_t>(&reject_stretch);
            if (!std::strcmp(reinterpret_cast<const char *>(entry->Name), "CreateDIBSection"))
                replacement = reinterpret_cast<uintptr_t>(&reject_dib);
            if (!replacement) continue;
            DWORD old = 0;
            if (!VirtualProtect(&addresses->u1.Function, sizeof(addresses->u1.Function), PAGE_READWRITE, &old)) return false;
            addresses->u1.Function = replacement;
            DWORD ignored = 0;
            if (!VirtualProtect(&addresses->u1.Function, sizeof(addresses->u1.Function), old, &ignored)) return false;
            ++guarded;
        }
    }
    return guarded == 2;
}

int lifecycle() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    constexpr unsigned cycles = 2;
    constexpr unsigned frames_per_extent = 4;
    constexpr int extents[][2] = {{64,64}, {320,240}, {641,359}, {64,64}};
    unsigned completed = 0;
    for (unsigned cycle = 0; cycle < cycles; ++cycle) {
        Scene scene;
        RunOptions options;
        options.vsync = 0;
        if (!initialize(scene, options) || !configure_vsync(scene.api, 0)) return 1;
        for (const auto &extent : extents) {
            options.width = extent[0]; options.height = extent[1];
            if (!SetWindowPos(scene.window, HWND_TOPMOST, 80, 80, options.width, options.height,
                SWP_NOACTIVATE | SWP_SHOWWINDOW)) return 1;
            if (!dispatch_messages()) return 1;
            for (unsigned frame = 0; frame < frames_per_extent; ++frame) {
                if (!render(scene, options, completed)) return 1;
                ++completed;
            }
            // Occlusion and reappearance do not require any simulated input or
            // focus changes. Reacquisition must still produce current pixels.
            ShowWindow(scene.window, SW_HIDE);
            if (!dispatch_messages()) return 1;
            ShowWindow(scene.window, SW_SHOWNOACTIVATE);
        }
        if (!scene.close()) return 1;
    }
    std::printf("G10_LIFECYCLE_FRAMES=%u CYCLES=%u\n", completed, cycles);
    return completed == cycles * frames_per_extent * (sizeof(extents)/sizeof(extents[0])) ? 0 : 1;
}
}

int main(int argc, char **argv) {
    for (int index = 1; index < argc; ++index)
        if (!std::strcmp(argv[index], "--offscreen")) return 2;
    std::wstring manifest;
    if (qemu_lightning::opengl::configure_runtime(manifest) != ERROR_SUCCESS) return 2;
    std::wstring path;
    if (qemu_lightning::opengl::sibling_file(L"QemuLightningVulkan.dll", path) != ERROR_SUCCESS) return 2;
    // Retained until process exit, matching the native GL fixture's loader policy.
    HMODULE icd = LoadLibraryExW(path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!icd || !guard_cpu_presentation(icd)) return 2;
    const int result = argc == 2 && !std::strcmp(argv[1], "--lifecycle") ?
        lifecycle() : opengl_acceptance_main(argc, argv);
    const LONG calls = InterlockedCompareExchange(&forbidden_gdi_calls, 0, 0);
    std::printf("G10_CPU_WSI_CALLS=%ld\n", static_cast<long>(calls));
    std::printf("G10_VISIBLE_GPU_PRESENT=%s\n", result == 0 && calls == 0 ? "PASS" : "FAIL");
    return result == 0 && calls == 0 ? 0 : 1;
}
