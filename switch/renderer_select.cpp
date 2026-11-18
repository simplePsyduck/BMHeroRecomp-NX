#include "nx_renderer.h"

#include <cstdio>

namespace bmhnx {
    Backend selected_backend() {
#if defined(BMHNX_DEFAULT_DEKO3D) && defined(BMHNX_HAVE_DEKO3D)
        return Backend::Deko3D;
#else
        return Backend::GLideN64;
#endif
    }

    std::unique_ptr<ultramodern::renderer::RendererContext> create_renderer(
        uint8_t* rdram, ultramodern::renderer::WindowHandle window, bool developer_mode) {
#ifdef BMHNX_HAVE_DEKO3D
        if (selected_backend() == Backend::Deko3D) {
            std::printf("[BMHNX] renderer: deko3d\n");
            return bmhnx_create_deko3d_context(rdram, window, developer_mode);
        }
#endif
        std::printf("[BMHNX] renderer: GLideN64 (OpenGL)\n");
        return bmhnx_create_render_context(rdram, window, developer_mode);
    }
}

// What main_switch.cpp hands to ultramodern as create_render_context.
std::unique_ptr<ultramodern::renderer::RendererContext> bmhnx_create_selected_render_context(
    uint8_t* rdram, ultramodern::renderer::WindowHandle window, bool developer_mode) {
    return bmhnx::create_renderer(rdram, window, developer_mode);
}
