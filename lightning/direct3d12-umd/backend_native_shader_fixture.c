/* Module: lightning.direct3d12_umd.backend_native_shader_fixture
 * Purpose: Exercise real Vulkan shim constructors against typed failure models.
 * Created: 2026-10-24
 * Architecture: Genuine Vulkan function types model dependency errors/ownership;
 * separate CPU-Vulkan validation covers real compiler output and driver objects.
 */
#include "backend_native_shader.h"
enum { GOOD, MISSING_CREATE, MISSING_DESTROY, FAILED_OOM, FAILED_LOST,
    FAILED_OTHER, SUCCESS_NULL };
static UINT mode, creates, destroys, generation;
static VkDevice selected = (VkDevice)(uintptr_t)0x1000;
static uintptr_t live[2];
static const uintptr_t sentinel = UINTPTR_MAX;
static const uint32_t fixture_words[] = {0x07230203, 0x00010000, 0, 1, 0};

static void check(BOOL condition, const char *message)
{
    DWORD written;
    if (!condition) {
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), message, (DWORD)lstrlenA(message), &written, NULL);
        ExitProcess(1);
    }
}
static VkResult created(uintptr_t *handle)
{
    ++creates;
    /* Vulkan creation errors may scribble nonnull undefined output values. */
    *handle = sentinel;
    if (mode == FAILED_OOM) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (mode == FAILED_LOST) return VK_ERROR_DEVICE_LOST;
    if (mode == FAILED_OTHER) return VK_ERROR_INITIALIZATION_FAILED;
    if (mode == SUCCESS_NULL) { *handle = 0; return VK_SUCCESS; }
    check(generation < ARRAYSIZE(live), "bounded fixture ownership");
    live[generation] = generation + 1;
    *handle = live[generation++];
    return VK_SUCCESS;
}
static void retired(uintptr_t handle)
{
    UINT index;
    for (index = 0; index < ARRAYSIZE(live); ++index) {
        if (live[index] == handle && handle != 0) {
            live[index] = 0;
            ++destroys;
            return;
        }
    }
    check(FALSE, "never destroy failed/null/stale Vulkan creation output");
}
static VkResult VKAPI_CALL layout_create(VkDevice device,
    const VkPipelineLayoutCreateInfo *info, const VkAllocationCallbacks *allocator,
    VkPipelineLayout *layout)
{
    uintptr_t handle;
    VkResult status;
    check(device == selected && !allocator && info &&
        info->sType == VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO && !info->pNext &&
        !info->flags && !info->setLayoutCount && !info->pSetLayouts &&
        !info->pushConstantRangeCount && !info->pPushConstantRanges, "official empty layout contract");
    status = created(&handle);
    *layout = (VkPipelineLayout)handle;
    return status;
}
static void VKAPI_CALL layout_destroy(VkDevice device, VkPipelineLayout layout,
    const VkAllocationCallbacks *allocator)
{
    check(device == selected && !allocator, "same-device layout cleanup");
    retired((uintptr_t)layout);
}
static VkResult VKAPI_CALL shader_create(VkDevice device,
    const VkShaderModuleCreateInfo *info, const VkAllocationCallbacks *allocator,
    VkShaderModule *module)
{
    uintptr_t handle;
    VkResult status;
    check(device == selected && !allocator && info &&
        info->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO && !info->pNext &&
        !info->flags && info->codeSize == sizeof(fixture_words) &&
        info->pCode == fixture_words, "official borrowed shader code contract");
    status = created(&handle);
    *module = (VkShaderModule)handle;
    return status;
}
static void VKAPI_CALL shader_destroy(VkDevice device, VkShaderModule module,
    const VkAllocationCallbacks *allocator)
{
    check(device == selected && !allocator, "same-device shader cleanup");
    retired((uintptr_t)module);
}
static PFN_vkVoidFunction VKAPI_CALL get(VkDevice device, const char *name)
{
    check(device == selected, "selected device dispatch only");
#define PROC(n, f, missing) if (lstrcmpA(name, n) == 0) return mode == missing ? NULL : (PFN_vkVoidFunction)f
    PROC("vkCreatePipelineLayout", layout_create, MISSING_CREATE);
    PROC("vkDestroyPipelineLayout", layout_destroy, MISSING_DESTROY);
    PROC("vkCreateShaderModule", shader_create, MISSING_CREATE);
    PROC("vkDestroyShaderModule", shader_destroy, MISSING_DESTROY);
#undef PROC
    return NULL;
}
static HRESULT create(BOOL shader, uintptr_t *owner)
{
    return shader ? ql_dx12_vk_shader_create(selected, get, fixture_words,
        sizeof(fixture_words), owner) : ql_dx12_vk_empty_layout_create(selected, get, owner);
}
static void destroy(BOOL shader, uintptr_t owner)
{
    if (shader) ql_dx12_vk_shader_destroy(owner);
    else ql_dx12_vk_layout_destroy(owner);
}
void fixture_entry(void)
{
    BOOL shader;
    UINT test;
    DWORD written;
    static const HRESULT expected[] = {S_OK, E_NOINTERFACE, E_NOINTERFACE,
        E_OUTOFMEMORY, DXGI_ERROR_DEVICE_REMOVED, E_FAIL, E_FAIL};
    static const char success[] = "PASS: production layout/shader create/destroy, exact official typed inputs, undefined failure outputs, missing destruction prerequisites and independent ownership\n";
    for (shader = FALSE; shader <= TRUE; ++shader) {
        for (test = MISSING_CREATE; test <= SUCCESS_NULL; ++test) {
            uintptr_t owner = sentinel;
            mode = test;
            creates = destroys = generation = 0;
            check(create(shader, &owner) == expected[test] && owner == sentinel && destroys == 0,
                "failed construction preserves output and owns no undefined handle");
            if (test == MISSING_CREATE || test == MISSING_DESTROY) {
                check(creates == 0, "cleanup prerequisite verified before creation");
            }
        }
        {
            uintptr_t first = 0, second = 0;
            mode = GOOD;
            creates = destroys = generation = 0;
            check(create(shader, &first) == S_OK && create(shader, &second) == S_OK &&
                first && second && first != second && creates == 2, "independent real helper ownership");
            destroy(shader, second);
            destroy(shader, first);
            destroy(shader, 0);
            check(destroys == 2 && live[0] == 0 && live[1] == 0, "exactly once independent destruction");
        }
    }
    {
        uintptr_t output = sentinel;
        UINT before = creates;
        check(ql_dx12_vk_shader_create(selected, get,
            (const uint32_t *)((const BYTE *)fixture_words + 1),
            sizeof(fixture_words), &output) == E_INVALIDARG && output == sentinel,
            "misaligned borrowed code rejected without mutation");
        check(ql_dx12_vk_shader_create(selected, get, fixture_words,
            sizeof(fixture_words) - 1, &output) == E_INVALIDARG && output == sentinel,
            "partial SPIR-V word rejected without mutation");
        check(ql_dx12_vk_shader_create(selected, get, fixture_words,
            sizeof(fixture_words), NULL) == E_INVALIDARG && creates == before,
            "invalid output rejected before Vulkan creation");
    }
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), success, sizeof(success) - 1, &written, NULL);
    ExitProcess(0);
}
