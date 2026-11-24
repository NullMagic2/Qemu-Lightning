/* Module: lightning.direct3d12_umd.shader_platform_internal
 * Purpose: Share private upstream layout observations between compiler adapters.
 * Created: 2026-10-24
 * Architecture: These pointers stay borrowed inside the serialized compiler call.
 */
#ifndef QL_SHADER_PLATFORM_INTERNAL_H
#define QL_SHADER_PLATFORM_INTERNAL_H
#include <stddef.h>
#include <stdint.h>
struct vkd3d_shader_desc;
struct ql_shader_metadata {
    uint32_t stage, major, minor, restricted;
};
struct ql_shader_output {
    const void *code;
    size_t bytes;
    uint32_t flags, reserved;
};
int32_t ql_shader_inspect_desc_constants(const struct vkd3d_shader_desc *desc,struct ql_shader_metadata *metadata);
int32_t ql_shader_inspect_desc(const struct vkd3d_shader_desc *desc,
    struct ql_shader_metadata *metadata);
int32_t ql_shader_inspect_desc_root_buffers(const struct vkd3d_shader_desc *,struct ql_shader_metadata *);
int32_t ql_shader_inspect_desc_storage_buffers(const struct vkd3d_shader_desc *,struct ql_shader_metadata *);
int32_t ql_shader_inspect_desc_structured(const struct vkd3d_shader_desc *,struct ql_shader_metadata *);
int32_t ql_shader_inspect_desc_pixel_math(const struct vkd3d_shader_desc *,struct ql_shader_metadata *);
int32_t ql_shader_native_pixel_math_operation(uint32_t,uint32_t);
int32_t ql_shader_native_vertex_compare_operation(uint32_t,uint32_t);
int32_t ql_shader_inspect_desc_vertex_flow(const struct vkd3d_shader_desc *,struct ql_shader_metadata *);
#endif

int32_t ql_shader_inspect_desc_pixel_flow(const struct vkd3d_shader_desc *,struct ql_shader_metadata *);
