/* Module: lightning.guest_vulkan.map_pressure_registry_acceptance
 * Purpose: Verify coordinator failure, capacity, identity and process cleanup.
 * Created: 2026-11-07
 * Architecture: Native C ABI fixture links the unchanged Rust coordinator and
 * real Win32 objects. Only the idle-trim callback is an observable counter.
 */
#include "windows_platform.h"
#include <windows.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REGISTRY_APERTURE_BYTES (UINT64_C(512) * 1024 * 1024)
#define REGISTRY_WAIT_MS 10000u
#define REGISTRY_NAME_BYTES 160u
#define REGISTRY_SETTLE_MS 20u
#define REGISTRY_IDENTITY_TAG 0x514c4251u
static unsigned checks, failures;
static volatile LONG callbacks;

static void check(bool condition, const char *message)
{
    checks++;
    if (!condition) {
        failures++;
        printf("FAIL %s\n", message);
    }
}

static uint32_t trim(void *cookie)
{
    InterlockedIncrement(cookie);
    return 1;
}

static QLV_MAP_PRESSURE *open_pressure(uint64_t identity)
{
    uint8_t luid[QLV_LUID_BYTES];

    memcpy(luid, &identity, sizeof(luid));
    return qlv_map_pressure_create(luid, REGISTRY_APERTURE_BYTES, (void *)&callbacks, trim);
}

static int hold_registry(uint64_t identity, const char *ready_name)
{
    QLV_MAP_PRESSURE *owners[QLV_KERNEL_MAX_CLIENTS - 1];

    for (unsigned index = 0; index < QLV_KERNEL_MAX_CLIENTS - 1; index++) {
        owners[index] = open_pressure(identity);
        check(owners[index] != NULL, "child occupies an independent coordinator slot");
    }
    HANDLE ready = OpenEventA(EVENT_MODIFY_STATE, FALSE, ready_name);

    check(ready != NULL, "child opens readiness event");
    if (!failures)
        SetEvent(ready);
    Sleep(REGISTRY_WAIT_MS);
    for (unsigned index = 0; index < QLV_KERNEL_MAX_CLIENTS - 1; index++)
        qlv_map_pressure_stop(owners[index]);
    CloseHandle(ready);
    return failures ? 1 : 0;
}

static void check_capacity_and_dead_owner(const char *exe, uint64_t identity)
{
    QLV_MAP_PRESSURE *anchor = open_pressure(identity);
    char ready_name[REGISTRY_NAME_BYTES];
    char command[MAX_PATH * 2];
    STARTUPINFOA startup = {.cb = sizeof(startup)};
    PROCESS_INFORMATION child = {0};

    check(anchor != NULL, "parent retains a live registry anchor");
    check(qlv_map_pressure_reclaim(anchor) == 0, "no peers causes no retry request");
    snprintf(ready_name, sizeof(ready_name), "Local\\QemuLightning.RegistryAcceptance.%lu",
              (unsigned long)GetCurrentProcessId());
    HANDLE ready = CreateEventA(NULL, TRUE, FALSE, ready_name);

    snprintf(command, sizeof(command), "\"%s\" --hold %llu %s", exe,
              (unsigned long long)identity, ready_name);
    check(CreateProcessA(exe, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL,
                           &startup, &child) != 0, "bounded child registry process starts");
    check(WaitForSingleObject(ready, REGISTRY_WAIT_MS) == WAIT_OBJECT_0,
          "child fills the remaining production participant slots");
    QLV_MAP_PRESSURE *overflow = open_pressure(identity);

    check(overflow == NULL, "registry capacity failure leaves current owners untouched");
    qlv_map_pressure_stop(overflow);
    QLV_MAP_PRESSURE *other_adapter = open_pressure(identity + 1);

    check(other_adapter != NULL, "a different adapter namespace has independent capacity");
    qlv_map_pressure_stop(other_adapter);
    check(TerminateProcess(child.hProcess, 0) != 0, "only this fixture's child is terminated");
    check(WaitForSingleObject(child.hProcess, REGISTRY_WAIT_MS) == WAIT_OBJECT_0,
          "dead owner has exited before slot reuse");
    QLV_MAP_PRESSURE *replacement = open_pressure(identity);

    check(replacement != NULL, "process-creation identity reaps dead owners and permits reuse");
    InterlockedExchange(&callbacks, 0);
    check(qlv_map_pressure_reclaim(anchor) != 0, "replacement participates in a fresh epoch");
    check(InterlockedCompareExchange(&callbacks, 0, 0) > 0,
          "a real process-local worker invokes its owner callback");
    qlv_map_pressure_stop(replacement);
    qlv_map_pressure_stop(anchor);
    LONG completed = InterlockedCompareExchange(&callbacks, 0, 0);

    Sleep(REGISTRY_SETTLE_MS);
    check(InterlockedCompareExchange(&callbacks, 0, 0) == completed,
          "stop joins workers before callback ownership ends");
    CloseHandle(ready);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "--hold"))
        return hold_registry(strtoull(argv[2], NULL, 10), argv[3]);
    uint64_t identity = ((uint64_t)GetCurrentProcessId() << 32) | REGISTRY_IDENTITY_TAG;
    uint8_t luid[QLV_LUID_BYTES];
    char exe[MAX_PATH];

    memcpy(luid, &identity, sizeof(luid));
    check(qlv_map_pressure_create(NULL, REGISTRY_APERTURE_BYTES, NULL, trim) == NULL,
          "null adapter identity is rejected");
    check(qlv_map_pressure_create(luid, 0, NULL, trim) == NULL,
          "zero aperture is rejected");
    check(qlv_map_pressure_create(luid, REGISTRY_APERTURE_BYTES, NULL, NULL) == NULL,
          "missing trim owner is rejected");
    qlv_map_pressure_stop(NULL);
    check(qlv_map_pressure_reclaim(NULL) == 0, "unavailable coordination retains local-only policy");
    check(GetModuleFileNameA(NULL, exe, sizeof(exe)) != 0, "own executable path is available");
    check_capacity_and_dead_owner(exe, identity);
    printf("%u registry checks; %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
