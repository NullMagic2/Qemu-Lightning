/* Module: lightning.guest_vulkan.map_pressure_acceptance
 * Purpose: Exercise owner-only idle reclamation between native Windows processes.
 * Created: 2026-11-07
 * Architecture: Includes the production renderer and its maintained modeled
 * kernel fixture. Real Rust workers, Win32 named objects and process views run
 * unchanged; only the host aperture's accounting is modeled across processes.
 */
int local_cache_acceptance_main(int argc, char **argv);
#define main local_cache_acceptance_main
#include "map_cache_acceptance.c"
#undef main

#define PRESSURE_CHILDREN 2u
#define PRESSURE_LIMIT_BYTES (8 * MIB)
#define PRESSURE_ACTIVE_BYTES MIB
#define PRESSURE_IDLE_BYTES (2 * MIB)
#define PRESSURE_REQUEST_BYTES (4 * MIB)
#define PRESSURE_WAIT_MS 10000u
#define PRESSURE_POLL_MS 5u
#define PRESSURE_SENTINEL 0x6bu
#define PRESSURE_NAME_BYTES 160u

struct pressure_capacity {
    uint64_t mapped;
    uint64_t limit;
    LONG stop;
    LONG ready[PRESSURE_CHILDREN];
    LONG idle_retired[PRESSURE_CHILDREN];
    LONG active_valid[PRESSURE_CHILDREN];
    LONG active_only;
    LONG child_checks[PRESSURE_CHILDREN];
};
static struct pressure_capacity *capacity;
static HANDLE capacity_mapping, capacity_guard;
static uint8_t adapter_luid[QLV_LUID_BYTES];

static void capacity_open(const char *base, bool fresh)
{
    char name[PRESSURE_NAME_BYTES];

    snprintf(name, sizeof(name), "%s.state", base);
    capacity_mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                          0, sizeof(*capacity), name);
    check(capacity_mapping != NULL, "shared aperture accounting mapping opens");
    capacity = MapViewOfFile(capacity_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*capacity));
    check(capacity != NULL, "shared aperture accounting view opens");
    snprintf(name, sizeof(name), "%s.guard", base);
    capacity_guard = CreateMutexA(NULL, FALSE, name);
    check(capacity_guard != NULL, "shared aperture accounting mutex opens");
    if (!capacity || !capacity_guard)
        ExitProcess(2);
    if (fresh) {
        memset(capacity, 0, sizeof(*capacity));
        capacity->limit = PRESSURE_LIMIT_BYTES;
    }
    /* A fixture-specific adapter namespace prevents unrelated Wine processes
     * from joining this acceptance run's production coordinator. */
    uint64_t identity = strtoull(strrchr(base, '.') + 1, NULL, 10);
    memcpy(adapter_luid, &identity, sizeof(adapter_luid));
}

static void capacity_close(void)
{
    UnmapViewOfFile(capacity);
    CloseHandle(capacity_mapping);
    CloseHandle(capacity_guard);
}

static bool reserve_shared(uint64_t bytes)
{
    check(WaitForSingleObject(capacity_guard, PRESSURE_WAIT_MS) == WAIT_OBJECT_0,
          "modeled aperture accounting lock completes");
    bool available = capacity->mapped <= capacity->limit &&
        bytes <= capacity->limit - capacity->mapped;

    if (available)
        capacity->mapped += bytes;
    ReleaseMutex(capacity_guard);
    return available;
}

static void release_shared(uint64_t bytes)
{
    check(WaitForSingleObject(capacity_guard, PRESSURE_WAIT_MS) == WAIT_OBJECT_0,
          "modeled aperture release lock completes");
    check(bytes <= capacity->mapped, "aggregate view accounting never underflows");
    capacity->mapped -= bytes;
    ReleaseMutex(capacity_guard);
}

static struct qlv_renderer *pressure_renderer(void)
{
    struct qlv_renderer *renderer = open_renderer(NULL, 1024 * MIB, 1024 * MIB);

    /* The maintained model normally returns a fixed LUID. Re-register it under
     * this run's adapter before publishing any modeled mapping. */
    qlv_map_pressure_stop(renderer->map_pressure);
    renderer->map_pressure = qlv_map_pressure_create(adapter_luid, renderer->aperture_bytes,
                                                     renderer, qlv_map_pressure_trim);
    check(renderer->map_pressure != NULL, "production Rust coordinator registers renderer");
    return renderer;
}

static int pressure_child(const char *base, unsigned slot)
{
    capacity_open(base, false);
    fixture_reserve = reserve_shared;
    fixture_release = release_shared;
    struct qlv_renderer *renderer = pressure_renderer();
    struct vn_renderer_bo *held = create_bo(renderer, PRESSURE_ACTIVE_BYTES);
    struct vn_renderer_bo *idle = create_bo(renderer, PRESSURE_IDLE_BYTES);
    uint8_t *pointer = map_bo(renderer, held);

    check(pointer != NULL, "peer publishes a real active process view");
    if (!pointer)
        return 2;
    pointer[0] = PRESSURE_SENTINEL;
    pointer[PRESSURE_ACTIVE_BYTES - 1] = PRESSURE_SENTINEL;
    if (!capacity->active_only) {
        check(map_bo(renderer, idle) != NULL, "peer publishes a reclaimable idle view");
        unmap_bo(renderer, idle);
    }
    InterlockedExchange(&capacity->ready[slot], 1);
    DWORD start = GetTickCount();

    while (!InterlockedCompareExchange(&capacity->stop, 0, 0) &&
           GetTickCount() - start < PRESSURE_WAIT_MS) {
        check(pointer[0] == PRESSURE_SENTINEL &&
              pointer[PRESSURE_ACTIVE_BYTES - 1] == PRESSURE_SENTINEL,
              "active pointer remains readable and unchanged during foreign pressure");
        simple_mtx_lock(&renderer->map_cache_lock);
        bool retired = renderer->map_cache_views == 0;
        simple_mtx_unlock(&renderer->map_cache_lock);
        if (retired)
            InterlockedExchange(&capacity->idle_retired[slot], 1);
        Sleep(PRESSURE_POLL_MS);
    }
    check(capacity->active_only ||
          InterlockedCompareExchange(&capacity->idle_retired[slot], 0, 0) != 0,
          "foreign epoch retired peer's idle view");
    check(held->mmap_ptr == pointer && pointer[0] == PRESSURE_SENTINEL &&
          pointer[PRESSURE_ACTIVE_BYTES - 1] == PRESSURE_SENTINEL,
          "epoch never revokes an application-owned active view");
    InterlockedExchange(&capacity->active_valid[slot], failures == 0);
    unmap_bo(renderer, held);
    destroy_bo(renderer, held);
    destroy_bo(renderer, idle);
    close_renderer(renderer);
    InterlockedExchange(&capacity->child_checks[slot], checks);
    capacity_close();
    printf("peer %u: %ld checks; %ld failures\n", slot, (long)checks, (long)failures);
    return failures ? 1 : 0;
}

static PROCESS_INFORMATION start_child(const char *exe, const char *base, unsigned slot)
{
    char command[MAX_PATH * 2];
    STARTUPINFOA startup = {.cb = sizeof(startup)};
    PROCESS_INFORMATION child = {0};

    snprintf(command, sizeof(command), "\"%s\" --peer %s %u", exe, base, slot);
    check(CreateProcessA(exe, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL,
                           &startup, &child) != 0, "separate native renderer process starts");
    return child;
}

static int pressure_parent(const char *exe, const char *peer_exe, bool active_only)
{
    char base[PRESSURE_NAME_BYTES];
    PROCESS_INFORMATION children[PRESSURE_CHILDREN];

    snprintf(base, sizeof(base), "Local\\QemuLightning.PressureAcceptance.%lu",
              (unsigned long)GetCurrentProcessId());
    capacity_open(base, true);
    capacity->active_only = active_only;
    fixture_reserve = reserve_shared;
    fixture_release = release_shared;
    for (unsigned slot = 0; slot < PRESSURE_CHILDREN; slot++)
        children[slot] = start_child(peer_exe ? peer_exe : exe, base, slot);
    DWORD start = GetTickCount();
    bool ready = false;

    do {
        ready = true;
        for (unsigned slot = 0; slot < PRESSURE_CHILDREN; slot++)
            ready &= InterlockedCompareExchange(&capacity->ready[slot], 0, 0) != 0;
        if (!ready)
            Sleep(PRESSURE_POLL_MS);
    } while (!ready && GetTickCount() - start < PRESSURE_WAIT_MS);
    check(ready, "both independent owners park idle views before pressure");
    struct qlv_renderer *renderer = pressure_renderer();
    uint64_t request_bytes = active_only ? PRESSURE_LIMIT_BYTES - MIB : PRESSURE_REQUEST_BYTES;
    struct vn_renderer_bo *request = create_bo(renderer, request_bytes);
    void *mapped = map_bo(renderer, request);

    check(active_only ? mapped == NULL : mapped != NULL,
          "MAP either reclaims idle capacity or returns real OOM with only active views");
    check(fake.refused > 0, "the first MAP actually exhausts aggregate modeled capacity");
    if (active_only)
        check(fake.refused == 2, "an unsuccessful foreign-pressure retry remains bounded");
    start = GetTickCount();
    bool retired = false;
    do {
        retired = true;
        for (unsigned slot = 0; slot < PRESSURE_CHILDREN; slot++)
            retired &= InterlockedCompareExchange(&capacity->idle_retired[slot], 0, 0) != 0;
        if (!retired)
            Sleep(PRESSURE_POLL_MS);
    } while (!retired && GetTickCount() - start < PRESSURE_WAIT_MS);
    check(retired, "both peers acknowledge release of their own idle views");
    InterlockedExchange(&capacity->stop, 1);
    for (unsigned slot = 0; slot < PRESSURE_CHILDREN; slot++) {
        DWORD code = 2;

        check(WaitForSingleObject(children[slot].hProcess, PRESSURE_WAIT_MS) == WAIT_OBJECT_0,
              "peer worker and renderer stop without deadlock");
        GetExitCodeProcess(children[slot].hProcess, &code);
        check(code == 0 && capacity->active_valid[slot],
              "peer confirms active-pointer preservation and clean teardown");
        printf("peer %u: %ld active-pointer and cache checks\n", slot,
               (long)capacity->child_checks[slot]);
        CloseHandle(children[slot].hThread);
        CloseHandle(children[slot].hProcess);
    }
    if (mapped)
        unmap_bo(renderer, request);
    destroy_bo(renderer, request);
    close_renderer(renderer);
    check(capacity->mapped == 0, "all aggregate modeled mappings retire at teardown");
    capacity_close();
    printf("parent: %ld checks; %ld failures\n", (long)checks, (long)failures);
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    InitializeCriticalSection(&fake_lock);
    if (argc == 4 && !strcmp(argv[1], "--peer"))
        return pressure_child(argv[2], (unsigned)strtoul(argv[3], NULL, 10));
    char exe[MAX_PATH];

    check(GetModuleFileNameA(NULL, exe, sizeof(exe)) != 0, "own executable path is available");
    const char *peer_exe = argc == 3 && !strcmp(argv[1], "--mixed") ? argv[2] : NULL;

    return pressure_parent(exe, peer_exe, argc == 2 && !strcmp(argv[1], "--active-only"));
}
