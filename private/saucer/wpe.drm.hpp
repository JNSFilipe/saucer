#pragma once

#include <cstdint>
#include <memory>
#include <functional>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <wpe/fdo.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

namespace saucer::drm
{
    struct display
    {
        int drm_fd{-1};
        gbm_device *gbm{nullptr};
        EGLDisplay egl_display{EGL_NO_DISPLAY};
        EGLContext egl_context{EGL_NO_CONTEXT};
        EGLConfig egl_config{nullptr};

        uint32_t connector_id{0};
        uint32_t crtc_id{0};
        int crtc_index{-1};
        uint32_t plane_id{0};
        bool atomic_supported{false};
        drmModeModeInfo mode{};

        uint32_t width{0};
        uint32_t height{0};

        gbm_surface *gbm_surface{nullptr};
        EGLSurface egl_surface{EGL_NO_SURFACE};

        gbm_bo *current_bo{nullptr};
        gbm_bo *next_bo{nullptr};
        uint32_t current_fb{0};

        struct wpe_view_backend_exportable_fdo *exportable{nullptr};

        struct buffer_entry
        {
            gbm_bo *bo{nullptr};
            uint32_t fb_id{0};
            struct wl_resource *resource{nullptr};
        };

        buffer_entry current_buffer{};
        buffer_entry pending_buffer{};
        bool mode_set{false};
        int drm_event_source{-1};
        ~display();
    };

    std::unique_ptr<display> init();

    void swap_buffers(display *disp);
    void page_flip(display *disp);

    // Render an EGL image to the display
    void render_image(display *disp, EGLImageKHR image, int width, int height);
    void render_dmabuf(display *disp, const struct wpe_view_backend_exportable_fdo_dmabuf_resource *dmabuf);

} // namespace saucer::drm
