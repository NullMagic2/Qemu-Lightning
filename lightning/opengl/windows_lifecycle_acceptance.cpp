/*
Module: qemu_lightning.opengl.windows_lifecycle_acceptance
Purpose: Require actual native busy refusal, surface loss, device recreation and unload.
Created: 2026-11-10
Architecture: Private child process; retained GPU draw/pixel oracles plus real
Windows module and thread-hook lifetimes. No synthetic input, adapter reset or
claim of physical TDR qualification. A destroyed surface is the loss stimulus.
*/
#define QL_OPENGL_ACCEPTANCE_Z_ORDER HWND_TOPMOST
#define main ordinary_acceptance_main
#include "acceptance.cpp"
#undef main

namespace {
LONG CALLBACK report_fault(EXCEPTION_POINTERS* fault) {
    if (fault->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    static LONG entered = 0;

    if (InterlockedCompareExchange(&entered, 1, 0) != 0) { return EXCEPTION_CONTINUE_SEARCH; }
    std::printf("G12_FAULT code=%lx address=%p target=%p\n",
        fault->ExceptionRecord->ExceptionCode, fault->ExceptionRecord->ExceptionAddress,
        reinterpret_cast<void*>(fault->ExceptionRecord->ExceptionInformation[1]));
#ifdef _WIN64
    CONTEXT context = *fault->ContextRecord;

    for (unsigned index = 0; index < 32 && context.Rip; ++index) {
        HMODULE module = nullptr;
        wchar_t path[MAX_PATH]{};

        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<const wchar_t*>(context.Rip), &module)) {
            GetModuleFileNameW(module, path, MAX_PATH);
            std::printf("G12_FAULT_FRAME %u %ls rva=%llx\n", index, path,
                static_cast<unsigned long long>(context.Rip - reinterpret_cast<ULONG_PTR>(module)));
        }
        DWORD64 image = 0;
        PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(context.Rip, &image, nullptr);

        if (entry) {
            void* handler = nullptr;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip, entry,
                &context, &handler, &frame, nullptr);
        } else {
            DWORD64 address = 0;
            SIZE_T read = 0;

            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(context.Rsp),
                    &address, sizeof(address), &read) || read != sizeof(address)) { break; }
            context.Rip = address;
            context.Rsp += sizeof(address);
        }
    }
#endif
    return EXCEPTION_CONTINUE_SEARCH;
}

constexpr unsigned recreation_cycles = 4;
constexpr DWORD required_loader_refusals = 7;
constexpr DWORD fixture_wait_ms = 120000;
DWORD WINAPI quiesced_thread(void* event) {
    return WaitForSingleObject(event, fixture_wait_ms) == WAIT_OBJECT_0 ? 0 : 1;
}
bool status(GlApi& api, QemuLightningOpenGLStatus& output) {
    output = {};
    output.size = sizeof(output);
    output.abi = QL_OPENGL_RUNTIME_ABI;
    return api.status(&output) == ERROR_SUCCESS;
}
bool lose_surface(Scene& scene) {
    if (!DestroyWindow(scene.window)) { return false; }
    const BOOL published = scene.api.swap(scene.dc);
    const DWORD error = GetLastError();
    // CS_OWNDC ownership retires with its destroyed window. The WGL context is
    // still explicitly released/deleted by Scene::close before runtime teardown.
    scene.window = nullptr;
    scene.dc = nullptr;
    std::printf("G12_SURFACE_LOSS swap=%ld error=%lu\n", static_cast<long>(published),
        static_cast<unsigned long>(error));
    return published == FALSE;
}
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    AddVectoredExceptionHandler(1, report_fault);
    const bool visible = argc == 2 && std::strcmp(argv[1], "--visible") == 0;

    if (argc > 1 && !visible) { return ordinary_acceptance_main(argc, argv); }
    if (visible && qemu_lightning::opengl::require_interactive_window_station() != ERROR_SUCCESS) {
        return 1;
    }
    std::wstring manifest;
    std::wstring filename;

    if (qemu_lightning::opengl::configure_runtime(manifest) != ERROR_SUCCESS ||
        qemu_lightning::opengl::sibling_file(L"opengl32.dll", filename) != ERROR_SUCCESS) { return 1; }
    HMODULE controller = LoadLibraryExW(filename.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    GlApi control;

    if (!controller || !control.load(controller) || !control.shutdown ||
        !control.recreate || !control.status) { return 1; }
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE thread = event ? CreateThread(nullptr, 0, quiesced_thread, event, 0, nullptr) : nullptr;

    if (!thread) { return 1; }
    QemuLightningOpenGLStatus invalid{};
    invalid.abi = QL_OPENGL_RUNTIME_ABI;

    if (control.status(&invalid) != ERROR_INVALID_PARAMETER || invalid.size != 0 ||
        control.status(nullptr) != ERROR_INVALID_PARAMETER) { return 1; }
    std::wstring callout_path;

    if (qemu_lightning::opengl::sibling_file(L"QemuLightningOpenGLLifecycleCallout.dll",
            callout_path) != ERROR_SUCCESS) { return 1; }
    const HMODULE callout = LoadLibraryExW(callout_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    using CalloutResult = DWORD (WINAPI*)();
    CalloutResult observed = nullptr;

    if (!callout || !GlApi::bind(callout, "QemuLightningOpenGLLifecycleCalloutResult", observed) ||
        observed() != required_loader_refusals || !FreeLibrary(callout)) { return 1; }
    std::puts("G12_LOADER_CALLOUT_REFUSALS=3 ABI_VALIDATION=PASS");
    unsigned completed = 0;
    DWORD previous_generation = 0;

    for (unsigned cycle = 0; cycle < recreation_cycles; ++cycle) {
        Scene scene;
        RunOptions options;
        options.present = visible;
        QemuLightningOpenGLStatus before{};

        if (!initialize(scene, options) || scene.library_pinned ||
            !status(control, before) ||
            (before.flags & QL_OPENGL_RUNTIME_CONTEXTS) == 0 ||
            (before.flags & QL_OPENGL_RUNTIME_RESET_STATUS_VALID) == 0 ||
            before.graphics_reset_status != GL_NO_ERROR ||
            (cycle != 0 && before.generation <= previous_generation)) { return 1; }
        previous_generation = before.generation;
        if (control.shutdown() != ERROR_BUSY || control.recreate() != ERROR_BUSY ||
            !render(scene, options, cycle)) { return 1; }
        std::printf("G12_BUSY_REFUSAL cycle=%u generation=%lu hooks=%lu\n", cycle,
            static_cast<unsigned long>(before.generation),
            static_cast<unsigned long>(before.tracked_thread_hooks));
        if (cycle == 0 && !lose_surface(scene)) { return 1; }
        if (!scene.close()) { return 1; }
        QemuLightningOpenGLStatus stopped{};

        if (!status(control, stopped) || stopped.flags != 0 || stopped.tracked_thread_hooks != 0 ||
            control.shutdown() != ERROR_SUCCESS || control.recreate() != ERROR_SUCCESS) { return 1; }
        ++completed;
    }
    if (control.shutdown() != ERROR_SUCCESS) { return 1; }
    SetEvent(event);
    if (WaitForSingleObject(thread, fixture_wait_ms) != WAIT_OBJECT_0) { return 1; }
    DWORD exit = 1;

    if (!GetExitCodeThread(thread, &exit) || exit != 0) { return 1; }
    CloseHandle(thread);
    CloseHandle(event);
    if (!FreeLibrary(controller) || GetModuleHandleW(filename.c_str()) != nullptr ||
        GetModuleHandleW(L"QemuLightningOpenGL.dll") != nullptr) { return 1; }
    std::puts("G12_EXPLICIT_UNLOAD=PASS");

    // Drop the client's last reference while a live GPU context still exists.
    // Production worker ownership must keep executable code mapped until a new
    // owned client reference can drain and release it explicitly.
    Scene retained;
    RunOptions options;
    options.present = visible;

    if (!initialize(retained, options) || retained.library_pinned) { return 1; }
    if (!FreeLibrary(retained.library)) { return 1; }
    retained.library = nullptr;
    if (GetModuleHandleW(filename.c_str()) == nullptr) { return 1; }
    retained.library = LoadLibraryExW(filename.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);

    if (!retained.library || !render(retained, options, completed) || !retained.close() ||
        GetModuleHandleW(filename.c_str()) != nullptr ||
        GetModuleHandleW(L"QemuLightningOpenGL.dll") != nullptr) { return 1; }
    std::printf("G12_RECREATION_CYCLES=%u\n", completed);
    std::puts("G12_WORKER_REFERENCE=PASS\nG12_NATIVE_LIFECYCLE=PASS");
    return 0;
}
