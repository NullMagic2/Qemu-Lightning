/* Module: qemu_lightning.direct3d_umd.rotated_blit
 * Purpose: Submit rotated display copies through the existing native GPU blitter.
 * Created: 2026-11-07
 * Architecture: Thin pinned Gallium/Zink ABI adapter; Zink owns GPU state and hazards.
 */
#pragma once
struct pipe_context;
struct pipe_blit_info;
#ifdef __cplusplus
extern "C" {
#endif
enum { LIGHTNING_ROTATION_FULL_TURN = 4 };
enum lightning_rotated_blit_result {
    LIGHTNING_ROTATED_BLIT_OK,
    LIGHTNING_ROTATED_BLIT_UNSUPPORTED,
    LIGHTNING_ROTATED_BLIT_FAILED,
};
/* The caller serializes the immediate context and supplies validated 2D image
 * boxes. Quarter turns are counter-clockwise, in [1, 3]. No CPU pixels or waits.
 * Failure always restores the native rotation override. */
enum lightning_rotated_blit_result
lightning_rotated_blit(struct pipe_context *pipe, const struct pipe_blit_info *info,
                       unsigned quarter_turns);
#ifdef __cplusplus
}
#endif
