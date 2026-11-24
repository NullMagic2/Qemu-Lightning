/*
 * Module: lightning.display.gpu_state
 * Purpose: Reuse the existing bounded GL state save/restore at console boundaries.
 * Created: 2026-10-21
 * Architecture: Caller owns a current desktop GL3.3 context; no GPU/context owner.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LIGHTNING_DISPLAY_GPU_STATE_HPP
#define LIGHTNING_DISPLAY_GPU_STATE_HPP
#include <epoxy/gl.h>
#include <array>
#include <cstddef>
namespace lightning::gpu {
/* Initialization boundary only: match the existing bounded GDI policy while
 * returning the first incoming error for callers that expose diagnostics.
 * New submission errors are never cleared by this helper implicitly.
 */
inline GLenum drain_preexisting_errors() noexcept {
    constexpr unsigned maximum_old_errors = 16;
    GLenum first = GL_NO_ERROR;

    for (unsigned index = 0; index < maximum_old_errors; ++index) {
        const GLenum error = glGetError();

        if (error == GL_NO_ERROR) {
            break;
        }
        if (first == GL_NO_ERROR) {
            first = error;
        }
    }
    return first;
}

inline constexpr std::array<GLenum, 11> render_switches{GL_BLEND, GL_DEPTH_TEST, GL_STENCIL_TEST,
    GL_CULL_FACE, GL_SCISSOR_TEST, GL_DITHER, GL_FRAMEBUFFER_SRGB, GL_COLOR_LOGIC_OP,
    GL_RASTERIZER_DISCARD, GL_SAMPLE_ALPHA_TO_COVERAGE, GL_SAMPLE_COVERAGE};
inline constexpr std::array<GLenum, 10> pixel_stores{GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH,
    GL_PACK_SKIP_ROWS, GL_PACK_SKIP_PIXELS, GL_UNPACK_ALIGNMENT, GL_UNPACK_ROW_LENGTH,
    GL_UNPACK_SKIP_ROWS, GL_UNPACK_SKIP_PIXELS, GL_PACK_SWAP_BYTES, GL_UNPACK_SWAP_BYTES};
class StateGuard final {
public:
    StateGuard() noexcept {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_);
        glGetIntegerv(GL_CURRENT_PROGRAM, &program_);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao_);
        glGetIntegerv(GL_VIEWPORT, viewport_.data());
        glGetIntegerv(GL_SCISSOR_BOX, scissor_.data());
        glGetIntegerv(GL_POLYGON_MODE, polygon_.data());
        glGetBooleanv(GL_COLOR_WRITEMASK, mask_.data());
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active_);
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_);
        glGetIntegerv(GL_BLEND_SRC_RGB, &blend_[0]);
        glGetIntegerv(GL_BLEND_DST_RGB, &blend_[1]);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_[2]);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_[3]);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &blend_[4]);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blend_[5]);

        for (size_t index = 0; index < textures_.size(); ++index) {
            glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(index));
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &textures_[index]);
            glGetIntegeri_v(GL_SAMPLER_BINDING, static_cast<GLuint>(index), &samplers_[index]);
            glBindSampler(static_cast<GLuint>(index), 0);
        }
        for (size_t index = 0; index < switches_.size(); ++index) {
            switches_[index] = glIsEnabled(render_switches[index]);
            glDisable(render_switches[index]);
        }
        for (size_t index = 0; index < stores_.size(); ++index) {
            glGetIntegerv(pixel_stores[index], &stores_[index]);
            glPixelStorei(pixel_stores[index], index == 0 || index == 4 ? 1 : 0);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glActiveTexture(GL_TEXTURE0);
    }
    ~StateGuard() {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(draw_));
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(read_));
        glUseProgram(static_cast<GLuint>(program_));
        glBindVertexArray(static_cast<GLuint>(vao_));
        glViewport(viewport_[0], viewport_[1], viewport_[2], viewport_[3]);
        glScissor(scissor_[0], scissor_[1], scissor_[2], scissor_[3]);
        glColorMask(mask_[0], mask_[1], mask_[2], mask_[3]);
        glBlendFuncSeparate(blend_[0], blend_[1], blend_[2], blend_[3]);
        glBlendEquationSeparate(blend_[4], blend_[5]);
        if (polygon_[0] == polygon_[1]) { glPolygonMode(GL_FRONT_AND_BACK, polygon_[0]); }
        else {
            glPolygonMode(GL_FRONT, polygon_[0]);
            glPolygonMode(GL_BACK, polygon_[1]);
        }

        for (size_t index = 0; index < switches_.size(); ++index) {
            if (switches_[index]) { glEnable(render_switches[index]); }
            else { glDisable(render_switches[index]); }
        }
        for (size_t index = 0; index < textures_.size(); ++index) {
            glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(index));
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(textures_[index]));
            glBindSampler(static_cast<GLuint>(index), static_cast<GLuint>(samplers_[index]));
        }
        glActiveTexture(active_);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(pack_));
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(unpack_));

        for (size_t index = 0; index < stores_.size(); ++index) { glPixelStorei(pixel_stores[index], stores_[index]); }
    }
    StateGuard(const StateGuard &) = delete;
    StateGuard &operator=(const StateGuard &) = delete;
private:
    GLint draw_ = 0, read_ = 0, program_ = 0, vao_ = 0, active_ = 0, pack_ = 0, unpack_ = 0;
    std::array<GLint, 4> viewport_{}, scissor_{};
    std::array<GLint, 6> blend_{};
    std::array<GLint, 2> polygon_{};
    std::array<GLint, 4> textures_{}, samplers_{};
    std::array<GLboolean, 4> mask_{};
    std::array<GLboolean, render_switches.size()> switches_{};
    std::array<GLint, pixel_stores.size()> stores_{};
};
} // namespace lightning::gpu
#endif
