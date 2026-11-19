// Renderer seam. Everything above this line (ultramodern, the game) only sees RendererContext; the backend
// behind it is picked here. Phase 1 ships GLideN64 on OpenGL (render_gliden64.cpp); Phase 2 adds deko3d
// (render_deko3d.cpp) as another RendererContext, selected at build time
// or -DBMHNX_RENDERER=deko3d at build time, with no change to main_switch.cpp or the game layer.
#pragma once

#include <cstdint>
#include <memory>

#include "ultramodern/renderer_context.hpp"

namespace bmhnx {
    enum class Backend { GLideN64, Deko3D };

    Backend selected_backend();
    std::unique_ptr<ultramodern::renderer::RendererContext> create_renderer(
        uint8_t* rdram, ultramodern::renderer::WindowHandle window, bool developer_mode);
}

// Backend entry points (one per render_*.cpp).
std::unique_ptr<ultramodern::renderer::RendererContext> bmhnx_create_render_context(
    uint8_t* rdram, ultramodern::renderer::WindowHandle window, bool developer_mode);  // GLideN64
#ifdef BMHNX_HAVE_DEKO3D
std::unique_ptr<ultramodern::renderer::RendererContext> bmhnx_create_deko3d_context(
    uint8_t* rdram, ultramodern::renderer::WindowHandle window, bool developer_mode);
#endif
