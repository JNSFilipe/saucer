#include "wpe.drm.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <glib-unix.h>
#include <string>
#include <vector>

#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>

namespace saucer::drm
{

    static void log_gl_error(const char *label)
    {
        for (GLenum err = glGetError(); err != GL_NO_ERROR; err = glGetError())
        {
            std::cerr << "GL error after " << label << ": 0x" << std::hex << err << std::dec << std::endl;
        }
    }

    static bool check_shader(GLuint shader, const char *label)
    {
        GLint status = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
        if (status == GL_TRUE)
        {
            return true;
        }

        GLint log_len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_len);
        std::vector<char> log(static_cast<std::size_t>(log_len > 0 ? log_len : 1));
        glGetShaderInfoLog(shader, log_len, nullptr, log.data());
        std::cerr << "Shader compile failed (" << label << "): " << log.data() << std::endl;
        return false;
    }

    static bool check_program(GLuint program)
    {
        GLint status = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &status);
        if (status == GL_TRUE)
        {
            return true;
        }

        GLint log_len = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_len);
        std::vector<char> log(static_cast<std::size_t>(log_len > 0 ? log_len : 1));
        glGetProgramInfoLog(program, log_len, nullptr, log.data());
        std::cerr << "Program link failed: " << log.data() << std::endl;
        return false;
    }

    static void log_gl_info_once()
    {
        static bool logged = false;
        if (logged)
        {
            return;
        }

        const char *vendor = reinterpret_cast<const char *>(glGetString(GL_VENDOR));
        const char *renderer = reinterpret_cast<const char *>(glGetString(GL_RENDERER));
        const char *version = reinterpret_cast<const char *>(glGetString(GL_VERSION));
        const char *extensions = reinterpret_cast<const char *>(glGetString(GL_EXTENSIONS));

        std::cerr << "GL_VENDOR: " << (vendor ? vendor : "(null)") << std::endl;
        std::cerr << "GL_RENDERER: " << (renderer ? renderer : "(null)") << std::endl;
        std::cerr << "GL_VERSION: " << (version ? version : "(null)") << std::endl;

        if (!extensions)
        {
            std::cerr << "GL extensions string is null" << std::endl;
        }
        const std::string ext_list{extensions};
        if (ext_list.find("GL_OES_EGL_image") == std::string::npos)
        {
            std::cerr << "GL_OES_EGL_image not reported in extensions" << std::endl;
        }
        else
        {
            std::cerr << "GL_OES_EGL_image reported in extensions" << std::endl;
        }

        if (ext_list.find("GL_OES_EGL_image_external") == std::string::npos)
        {
            std::cerr << "GL_OES_EGL_image_external not reported in extensions" << std::endl;
        }
        else
        {
            std::cerr << "GL_OES_EGL_image_external reported in extensions" << std::endl;
        }

        logged = true;
    }

    static void release_buffer(display *disp, display::buffer_entry *buffer);

    display::~display()
    {
        if (drm_event_source >= 0)
        {
            g_source_remove(static_cast<guint>(drm_event_source));
            drm_event_source = -1;
        }

        release_buffer(this, &current_buffer);
        release_buffer(this, &pending_buffer);

        if (egl_surface != EGL_NO_SURFACE)
        {
            eglDestroySurface(egl_display, egl_surface);
        }

        if (egl_context != EGL_NO_CONTEXT)
        {
            eglDestroyContext(egl_display, egl_context);
        }

        if (egl_display != EGL_NO_DISPLAY)
        {
            eglTerminate(egl_display);
        }

        if (gbm_surface)
        {
            gbm_surface_destroy(gbm_surface);
        }

        if (gbm)
        {
            gbm_device_destroy(gbm);
        }

        if (drm_fd >= 0)
        {
            close(drm_fd);
        }
    }

    static void release_buffer(display *disp, display::buffer_entry *buffer)
    {
        if (!disp || !buffer)
        {
            return;
        }

        if (buffer->fb_id)
        {
            drmModeRmFB(disp->drm_fd, buffer->fb_id);
            buffer->fb_id = 0;
        }

        if (buffer->bo)
        {
            gbm_bo_destroy(buffer->bo);
            buffer->bo = nullptr;
        }

        if (buffer->resource && disp->exportable)
        {
            wpe_view_backend_exportable_fdo_dispatch_release_buffer(disp->exportable, buffer->resource);
        }

        buffer->resource = nullptr;
    }

    static void page_flip_handler(int /*fd*/, unsigned int /*frame*/, unsigned int /*sec*/, unsigned int /*usec*/, void *data)
    {
        auto *disp = static_cast<display *>(data);
        if (!disp)
        {
            return;
        }

        release_buffer(disp, &disp->current_buffer);
        disp->current_buffer = disp->pending_buffer;
        disp->pending_buffer = {};
        disp->mode_set = true;

        if (disp->exportable)
        {
            wpe_view_backend_exportable_fdo_dispatch_frame_complete(disp->exportable);
        }
    }

    static gboolean drm_event_dispatch(int fd, GIOCondition condition, gpointer /*data*/)
    {
        if (condition & (G_IO_ERR | G_IO_HUP))
        {
            return G_SOURCE_REMOVE;
        }

        if (condition & G_IO_IN)
        {
            drmEventContext ctx{};
            ctx.version = DRM_EVENT_CONTEXT_VERSION;
            ctx.page_flip_handler = page_flip_handler;
            drmHandleEvent(fd, &ctx);
        }

        return G_SOURCE_CONTINUE;
    }

    static int add_atomic_property(int fd, drmModeAtomicReq *req, uint32_t obj_id, uint32_t obj_type, const char *name, uint64_t value)
    {
        drmModeObjectProperties *props = drmModeObjectGetProperties(fd, obj_id, obj_type);
        if (!props)
        {
            return -1;
        }

        int result = -1;
        for (uint32_t i = 0; i < props->count_props; ++i)
        {
            drmModePropertyRes *prop = drmModeGetProperty(fd, props->props[i]);
            if (!prop)
            {
                continue;
            }

            if (strcmp(prop->name, name) == 0)
            {
                result = drmModeAtomicAddProperty(req, obj_id, prop->prop_id, value) > 0 ? 0 : -1;
                drmModeFreeProperty(prop);
                break;
            }

            drmModeFreeProperty(prop);
        }

        drmModeFreeObjectProperties(props);
        return result;
    }

    static drmModeConnector *find_connector(int fd, drmModeRes *resources)
    {
        for (int i = 0; i < resources->count_connectors; i++)
        {
            auto *connector = drmModeGetConnector(fd, resources->connectors[i]);
            if (connector->connection == DRM_MODE_CONNECTED)
            {
                return connector;
            }
            drmModeFreeConnector(connector);
        }
        return nullptr;
    }

    static drmModeEncoder *find_encoder(int fd, drmModeConnector *connector)
    {
        if (connector->encoder_id)
        {
            return drmModeGetEncoder(fd, connector->encoder_id);
        }
        return nullptr;
    }

    static uint32_t find_crtc(int fd, drmModeRes *resources, drmModeConnector *connector)
    {
        auto *encoder = find_encoder(fd, connector);
        if (encoder)
        {
            uint32_t crtc_id = encoder->crtc_id;
            drmModeFreeEncoder(encoder);
            if (crtc_id)
            {
                return crtc_id;
            }
        }

        // Try to find a CRTC that can drive this connector
        for (int i = 0; i < connector->count_encoders; i++)
        {
            encoder = drmModeGetEncoder(fd, connector->encoders[i]);
            if (!encoder)
                continue;

            for (int j = 0; j < resources->count_crtcs; j++)
            {
                if (encoder->possible_crtcs & (1 << j))
                {
                    uint32_t crtc_id = resources->crtcs[j];
                    drmModeFreeEncoder(encoder);
                    return crtc_id;
                }
            }
            drmModeFreeEncoder(encoder);
        }

        return 0;
    }

    static int find_crtc_index(drmModeRes *resources, uint32_t crtc_id)
    {
        if (!resources)
        {
            return -1;
        }

        for (int i = 0; i < resources->count_crtcs; ++i)
        {
            if (resources->crtcs[i] == crtc_id)
            {
                return i;
            }
        }

        return -1;
    }

    static bool plane_is_primary(int fd, uint32_t plane_id)
    {
        drmModeObjectProperties *props = drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
        if (!props)
        {
            return false;
        }

        bool primary = false;
        for (uint32_t i = 0; i < props->count_props; ++i)
        {
            drmModePropertyRes *prop = drmModeGetProperty(fd, props->props[i]);
            if (!prop)
            {
                continue;
            }

            if (strcmp(prop->name, "type") == 0 && props->prop_values[i] == DRM_PLANE_TYPE_PRIMARY)
            {
                primary = true;
                drmModeFreeProperty(prop);
                break;
            }

            drmModeFreeProperty(prop);
        }

        drmModeFreeObjectProperties(props);
        return primary;
    }

    static uint32_t find_plane_for_format(int fd, int crtc_index, uint32_t format)
    {
        drmModePlaneRes *planes = drmModeGetPlaneResources(fd);
        if (!planes)
        {
            return 0;
        }

        uint32_t plane_id = 0;
        for (int pass = 0; pass < 2 && !plane_id; ++pass)
        {
            const bool require_primary = (pass == 0);

            for (uint32_t i = 0; i < planes->count_planes; ++i)
            {
                drmModePlane *plane = drmModeGetPlane(fd, planes->planes[i]);
                if (!plane)
                {
                    continue;
                }

                if (crtc_index >= 0 && !(plane->possible_crtcs & (1u << crtc_index)))
                {
                    drmModeFreePlane(plane);
                    continue;
                }

                if (require_primary && !plane_is_primary(fd, plane->plane_id))
                {
                    drmModeFreePlane(plane);
                    continue;
                }

                for (uint32_t f = 0; f < plane->count_formats; ++f)
                {
                    if (plane->formats[f] == format)
                    {
                        plane_id = plane->plane_id;
                        break;
                    }
                }

                drmModeFreePlane(plane);

                if (plane_id)
                {
                    break;
                }
            }
        }

        drmModeFreePlaneResources(planes);
        return plane_id;
    }

    std::unique_ptr<display> init()
    {
        auto disp = std::make_unique<display>();

        // Try to find a DRM device with KMS support (display output)
        // On Raspberry Pi 5, card0 is v3d (render-only), card1 is vc4 (display controller)
        drmModeRes *resources = nullptr;
        const char *devices[] = {"/dev/dri/card1", "/dev/dri/card0", "/dev/dri/card2", nullptr};

        for (int i = 0; devices[i] != nullptr; ++i)
        {
            int fd = open(devices[i], O_RDWR | O_CLOEXEC);
            if (fd < 0)
            {
                continue;
            }

            // Check if this device has KMS support
            resources = drmModeGetResources(fd);
            if (resources && resources->count_connectors > 0)
            {
                disp->drm_fd = fd;
                std::cout << "Using DRM device: " << devices[i] << std::endl;
                break;
            }

            // This device doesn't have KMS support, try the next one
            if (resources)
            {
                drmModeFreeResources(resources);
                resources = nullptr;
            }
            close(fd);
        }

        if (disp->drm_fd < 0 || !resources)
        {
            std::cerr << "Failed to find DRM device with KMS support" << std::endl;
            return nullptr;
        }

        // Find a connected connector
        auto *connector = find_connector(disp->drm_fd, resources);
        if (!connector)
        {
            std::cerr << "No connected display found" << std::endl;
            drmModeFreeResources(resources);
            return nullptr;
        }

        disp->connector_id = connector->connector_id;

        // Use the preferred mode or the first available
        if (connector->count_modes > 0)
        {
            // Find preferred mode
            for (int i = 0; i < connector->count_modes; i++)
            {
                if (connector->modes[i].type & DRM_MODE_TYPE_PREFERRED)
                {
                    disp->mode = connector->modes[i];
                    break;
                }
            }
            // Fall back to first mode
            if (disp->mode.hdisplay == 0)
            {
                disp->mode = connector->modes[0];
            }
        }
        else
        {
            std::cerr << "No display modes available" << std::endl;
            drmModeFreeConnector(connector);
            drmModeFreeResources(resources);
            return nullptr;
        }

        disp->width  = disp->mode.hdisplay;
        disp->height = disp->mode.vdisplay;

        std::cout << "Display mode: " << disp->width << "x" << disp->height << std::endl;

        // Find CRTC
        disp->crtc_id = find_crtc(disp->drm_fd, resources, connector);
        if (!disp->crtc_id)
        {
            std::cerr << "Failed to find CRTC" << std::endl;
            drmModeFreeConnector(connector);
            drmModeFreeResources(resources);
            return nullptr;
        }

        disp->crtc_index = find_crtc_index(resources, disp->crtc_id);

        drmSetClientCap(disp->drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
        disp->plane_id = find_plane_for_format(disp->drm_fd, disp->crtc_index, DRM_FORMAT_ARGB8888);
        if (!disp->plane_id)
        {
            disp->plane_id = find_plane_for_format(disp->drm_fd, disp->crtc_index, DRM_FORMAT_XRGB8888);
        }

        disp->atomic_supported = (drmSetClientCap(disp->drm_fd, DRM_CLIENT_CAP_ATOMIC, 1) == 0) && disp->plane_id;

        drmModeFreeConnector(connector);
        drmModeFreeResources(resources);

        // Create GBM device
        disp->gbm = gbm_create_device(disp->drm_fd);
        if (!disp->gbm)
        {
            std::cerr << "Failed to create GBM device" << std::endl;
            return nullptr;
        }

        // Initialize EGL
        PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
            reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));

        if (get_platform_display)
        {
            disp->egl_display = get_platform_display(EGL_PLATFORM_GBM_KHR, disp->gbm, nullptr);
        }
        else
        {
            disp->egl_display = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(disp->gbm));
        }

        if (disp->egl_display == EGL_NO_DISPLAY)
        {
            std::cerr << "Failed to get EGL display" << std::endl;
            return nullptr;
        }

        EGLint major, minor;
        if (!eglInitialize(disp->egl_display, &major, &minor))
        {
            std::cerr << "Failed to initialize EGL" << std::endl;
            return nullptr;
        }

        std::cout << "EGL version: " << major << "." << minor << std::endl;

        if (!eglBindAPI(EGL_OPENGL_ES_API))
        {
            std::cerr << "Failed to bind OpenGL ES API" << std::endl;
            return nullptr;
        }

        // Choose EGL config
        // clang-format off
        static const EGLint config_attribs[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_NONE
        };
        // clang-format on

        EGLint num_configs;
        if (!eglChooseConfig(disp->egl_display, config_attribs, &disp->egl_config, 1, &num_configs) || num_configs == 0)
        {
            std::cerr << "Failed to choose EGL config" << std::endl;
            return nullptr;
        }

        // Create EGL context
        // clang-format off
        static const EGLint context_attribs[] = {
            EGL_CONTEXT_CLIENT_VERSION, 2,
            EGL_NONE
        };
        // clang-format on

        disp->egl_context = eglCreateContext(disp->egl_display, disp->egl_config, EGL_NO_CONTEXT, context_attribs);
        if (disp->egl_context == EGL_NO_CONTEXT)
        {
            std::cerr << "Failed to create EGL context" << std::endl;
            return nullptr;
        }

        // Create GBM surface
        disp->gbm_surface = gbm_surface_create(disp->gbm, disp->width, disp->height, GBM_FORMAT_XRGB8888,
                                               GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
        if (!disp->gbm_surface)
        {
            std::cerr << "Failed to create GBM surface" << std::endl;
            return nullptr;
        }

        // Create EGL surface
        disp->egl_surface =
            eglCreateWindowSurface(disp->egl_display, disp->egl_config,
                                   reinterpret_cast<EGLNativeWindowType>(disp->gbm_surface), nullptr);
        if (disp->egl_surface == EGL_NO_SURFACE)
        {
            std::cerr << "Failed to create EGL surface" << std::endl;
            return nullptr;
        }

        // Make context current
        if (!eglMakeCurrent(disp->egl_display, disp->egl_surface, disp->egl_surface, disp->egl_context))
        {
            std::cerr << "Failed to make EGL context current" << std::endl;
            return nullptr;
        }

        std::cout << "DRM/EGL initialization successful" << std::endl;

        disp->drm_event_source = g_unix_fd_add(disp->drm_fd, G_IO_IN, drm_event_dispatch, disp.get());

        return disp;
    }

    static void drm_fb_destroy_callback(gbm_bo *bo, void *data)
    {
        auto fb_id = reinterpret_cast<uintptr_t>(data);
        if (fb_id)
        {
            auto fd = gbm_device_get_fd(gbm_bo_get_device(bo));
            drmModeRmFB(fd, static_cast<uint32_t>(fb_id));
        }
    }


    static bool copy_bo_to_bo(gbm_bo *src_bo, gbm_bo *dst_bo, uint32_t width, uint32_t height)
    {
        if (!src_bo || !dst_bo)
        {
            return false;
        }

        uint32_t src_stride = 0;
        uint32_t dst_stride = 0;
        void *src_map = nullptr;
        void *dst_map = nullptr;
        void *src_map_data = nullptr;
        void *dst_map_data = nullptr;

        src_map = gbm_bo_map(src_bo, 0, 0, width, height, GBM_BO_TRANSFER_READ, &src_stride, &src_map_data);
        if (!src_map)
        {
            return false;
        }

        dst_map = gbm_bo_map(dst_bo, 0, 0, width, height, GBM_BO_TRANSFER_WRITE, &dst_stride, &dst_map_data);
        if (!dst_map)
        {
            gbm_bo_unmap(src_bo, src_map_data);
            return false;
        }

        const uint32_t row_bytes = width * 4;
        for (uint32_t y = 0; y < height; ++y)
        {
            std::memcpy(static_cast<unsigned char *>(dst_map) + dst_stride * y,
                        static_cast<unsigned char *>(src_map) + src_stride * y,
                        row_bytes);
        }

        gbm_bo_unmap(dst_bo, dst_map_data);
        gbm_bo_unmap(src_bo, src_map_data);
        return true;
    }

    static uint32_t get_fb_for_bo(int fd, gbm_bo *bo)
    {
        auto fb_id = reinterpret_cast<uintptr_t>(gbm_bo_get_user_data(bo));
        if (fb_id)
        {
            return static_cast<uint32_t>(fb_id);
        }

        uint32_t width   = gbm_bo_get_width(bo);
        uint32_t height  = gbm_bo_get_height(bo);
        uint32_t stride  = gbm_bo_get_stride(bo);
        uint32_t handle  = gbm_bo_get_handle(bo).u32;

        uint32_t new_fb_id = 0;
        int ret = drmModeAddFB(fd, width, height, 24, 32, stride, handle, &new_fb_id);
        if (ret)
        {
            std::cerr << "Failed to create framebuffer: " << strerror(errno) << std::endl;
            return 0;
        }

        gbm_bo_set_user_data(bo, reinterpret_cast<void *>(static_cast<uintptr_t>(new_fb_id)), drm_fb_destroy_callback);

        return new_fb_id;
    }

    void swap_buffers(display *disp)
    {
        eglSwapBuffers(disp->egl_display, disp->egl_surface);
        disp->next_bo = gbm_surface_lock_front_buffer(disp->gbm_surface);
    }

    void page_flip(display *disp)
    {
        uint32_t fb_id = get_fb_for_bo(disp->drm_fd, disp->next_bo);
        if (!fb_id)
        {
            return;
        }

        int ret = drmModeSetCrtc(disp->drm_fd, disp->crtc_id, fb_id, 0, 0, &disp->connector_id, 1, &disp->mode);
        if (ret)
        {
            std::cerr << "Failed to set CRTC: " << strerror(errno) << std::endl;
        }

        if (disp->current_bo)
        {
            gbm_surface_release_buffer(disp->gbm_surface, disp->current_bo);
        }

        disp->current_bo = disp->next_bo;
        disp->current_fb = fb_id;
        disp->next_bo    = nullptr;
    }

    void render_image(display *disp, EGLImageKHR image, int width, int height)
    {
        // Image dimensions could be used for aspect-ratio-correct rendering
        (void)width;
        (void)height;

        static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = nullptr;
        if (!glEGLImageTargetTexture2DOES)
        {
            glEGLImageTargetTexture2DOES = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
                eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        }

        if (!glEGLImageTargetTexture2DOES)
        {
            std::cerr << "glEGLImageTargetTexture2DOES not available" << std::endl;
            return;
        }

        // Make our context current
        eglMakeCurrent(disp->egl_display, disp->egl_surface, disp->egl_surface, disp->egl_context);

        log_gl_info_once();

        static bool use_solid = std::getenv("SAUCER_DRM_SOLID") != nullptr;
        const bool use_external = std::getenv("SAUCER_DRM_EXTERNAL") != nullptr;
        const GLenum tex_target = use_external ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D;

        // Create texture from EGL image
        GLuint texture = 0;
        if (!use_solid)
        {
            glGenTextures(1, &texture);
            glBindTexture(tex_target, texture);
            glTexParameteri(tex_target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(tex_target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(tex_target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(tex_target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            log_gl_error("glTexParameteri");
            glEGLImageTargetTexture2DOES(tex_target, image);
            log_gl_error("glEGLImageTargetTexture2DOES");
        }

        // Set up viewport
        glViewport(0, 0, disp->width, disp->height);

        // Clear and draw
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // Simple fullscreen quad shader
        static GLuint program = 0;
        static GLint pos_loc  = -1;
        static GLint tex_loc  = -1;

        if (program == 0)
        {
            const char *vertex_shader_src = R"(
                attribute vec4 position;
                varying vec2 texcoord;
                void main() {
                    gl_Position = position;
                    texcoord = position.xy * 0.5 + 0.5;
                    texcoord.y = 1.0 - texcoord.y;
                }
            )";

            const char *fragment_shader_src = nullptr;
            if (use_solid)
            {
                fragment_shader_src = R"(
                precision mediump float;
                void main() {
                    gl_FragColor = vec4(1.0, 0.0, 1.0, 1.0);
                }
            )";
            }
            else if (use_external)
            {
                fragment_shader_src = R"(
                #extension GL_OES_EGL_image_external : require
                precision mediump float;
                varying vec2 texcoord;
                uniform samplerExternalOES tex;
                void main() {
                    vec4 color = texture2D(tex, texcoord);
                    gl_FragColor = vec4(color.rgb, 1.0);
                }
            )";
            }
            else
            {
                fragment_shader_src = R"(
                precision mediump float;
                varying vec2 texcoord;
                uniform sampler2D tex;
                void main() {
                    vec4 color = texture2D(tex, texcoord);
                    gl_FragColor = vec4(color.rgb, 1.0);
                }
            )";
            }

            GLuint vertex_shader = glCreateShader(GL_VERTEX_SHADER);
            glShaderSource(vertex_shader, 1, &vertex_shader_src, nullptr);
            glCompileShader(vertex_shader);

            GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
            glShaderSource(fragment_shader, 1, &fragment_shader_src, nullptr);
            glCompileShader(fragment_shader);

            if (!check_shader(vertex_shader, "vertex") || !check_shader(fragment_shader, "fragment"))
            {
                glDeleteShader(vertex_shader);
                glDeleteShader(fragment_shader);
                return;
            }

            program = glCreateProgram();
            glAttachShader(program, vertex_shader);
            glAttachShader(program, fragment_shader);
            glLinkProgram(program);

            glDeleteShader(vertex_shader);
            glDeleteShader(fragment_shader);

            if (!check_program(program))
            {
                glDeleteProgram(program);
                program = 0;
                return;
            }

            pos_loc = glGetAttribLocation(program, "position");
            tex_loc = glGetUniformLocation(program, "tex");

            if (pos_loc < 0 || tex_loc < 0)
            {
                std::cerr << "Shader locations missing: position=" << pos_loc << " tex=" << tex_loc << std::endl;
            }
        }

        glUseProgram(program);
        log_gl_error("glUseProgram");

        // Fullscreen quad
        // clang-format off
        static const float vertices[] = {
            -1.0f, -1.0f,
             1.0f, -1.0f,
            -1.0f,  1.0f,
             1.0f,  1.0f,
        };
        // clang-format on

        glVertexAttribPointer(pos_loc, 2, GL_FLOAT, GL_FALSE, 0, vertices);
        glEnableVertexAttribArray(pos_loc);
        log_gl_error("glVertexAttribPointer");

        if (!use_solid)
        {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(tex_target, texture);
            log_gl_error("glBindTexture");
            glUniform1i(tex_loc, 0);
            log_gl_error("glUniform1i");
        }

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        log_gl_error("glDrawArrays");

        static bool pixel_logged = false;
        if (!pixel_logged)
        {
            unsigned char pixel[4] = {0, 0, 0, 0};
            glReadPixels(disp->width / 2, disp->height / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
            log_gl_error("glReadPixels");
            std::cerr << "Center pixel RGBA: "
                      << static_cast<int>(pixel[0]) << ","
                      << static_cast<int>(pixel[1]) << ","
                      << static_cast<int>(pixel[2]) << ","
                      << static_cast<int>(pixel[3]) << std::endl;
            pixel_logged = true;
        }

        std::cerr << "DRM: Draw complete, swapping buffers..." << std::endl;

        // Swap and flip
        swap_buffers(disp);
        std::cerr << "DRM: Buffers swapped, flipping..." << std::endl;
        page_flip(disp);
        std::cerr << "DRM: Page flip done" << std::endl;

        glFinish();
        log_gl_error("glFinish");

        // Cleanup
        if (texture != 0)
        {
            glDeleteTextures(1, &texture);
        }
    }


    void render_dmabuf(display *disp, const struct wpe_view_backend_exportable_fdo_dmabuf_resource *dmabuf)
    {
        if (!disp || !dmabuf || !disp->gbm)
        {
            return;
        }

        struct gbm_import_fd_modifier_data modifier_data = {
            .width = dmabuf->width,
            .height = dmabuf->height,
            .format = dmabuf->format,
            .num_fds = dmabuf->n_planes,
            .modifier = dmabuf->modifiers[0],
        };

        const uint32_t planes = dmabuf->n_planes > 4 ? 4 : dmabuf->n_planes;
        for (uint32_t i = 0; i < planes; ++i)
        {
            modifier_data.fds[i] = dmabuf->fds[i];
            modifier_data.strides[i] = dmabuf->strides[i];
            modifier_data.offsets[i] = dmabuf->offsets[i];
        }

        gbm_bo *bo = gbm_bo_import(disp->gbm, GBM_BO_IMPORT_FD_MODIFIER, &modifier_data, GBM_BO_USE_SCANOUT);
        if (!bo)
        {
            struct gbm_import_fd_data fd_data = {
                .fd = dmabuf->fds[0],
                .width = dmabuf->width,
                .height = dmabuf->height,
                .stride = dmabuf->strides[0],
                .format = dmabuf->format,
            };

            bo = gbm_bo_import(disp->gbm, GBM_BO_IMPORT_FD, &fd_data, GBM_BO_USE_SCANOUT);
        }

        if (!bo)
        {
            std::cerr << "Failed to import dmabuf into gbm_bo" << std::endl;
            if (disp->exportable)
            {
                wpe_view_backend_exportable_fdo_dispatch_release_buffer(disp->exportable, dmabuf->buffer_resource);
            }
            return;
        }

        gbm_bo *scanout_bo = bo;
        bool using_sw_copy = false;

        uint32_t handles[4] = {0};
        uint32_t strides[4] = {0};
        uint32_t offsets[4] = {0};
        uint64_t modifiers[4] = {0};

        const uint32_t bo_planes = gbm_bo_get_plane_count(scanout_bo);
        for (uint32_t i = 0; i < bo_planes; ++i)
        {
            handles[i] = gbm_bo_get_handle_for_plane(scanout_bo, i).u32;
            strides[i] = gbm_bo_get_stride_for_plane(scanout_bo, i);
            offsets[i] = gbm_bo_get_offset(scanout_bo, i);
            modifiers[i] = gbm_bo_get_modifier(scanout_bo);
        }

        uint32_t fb_id = 0;
        uint32_t flags = (modifiers[0] && modifiers[0] != DRM_FORMAT_MOD_INVALID) ? DRM_MODE_FB_MODIFIERS : 0;
        int ret = drmModeAddFB2WithModifiers(disp->drm_fd, dmabuf->width, dmabuf->height, dmabuf->format, handles,
                                             strides, offsets, modifiers, &fb_id, flags);
        if (ret)
        {
            static bool logged_mod_fail = false;
            if (!logged_mod_fail)
            {
                std::cerr << "DRM: dmabuf modifiers unsupported, using fallback" << std::endl;
                logged_mod_fail = true;
            }

            const bool allow_sw_copy = std::getenv("SAUCER_DRM_NO_SW_COPY") == nullptr;
            if (allow_sw_copy && (dmabuf->format == DRM_FORMAT_ARGB8888 || dmabuf->format == DRM_FORMAT_XRGB8888))
            {
                gbm_bo *linear = gbm_bo_create(disp->gbm, dmabuf->width, dmabuf->height, GBM_FORMAT_XRGB8888,
                                               GBM_BO_USE_SCANOUT | GBM_BO_USE_WRITE);
                if (!linear)
                {
                    static bool logged_sw_fail = false;
                    if (!logged_sw_fail)
                    {
                        std::cerr << "DRM: sw-copy fallback failed (gbm_bo_create)" << std::endl;
                        logged_sw_fail = true;
                    }
                }
                else if (!copy_bo_to_bo(bo, linear, dmabuf->width, dmabuf->height))
                {
                    static bool logged_sw_fail = false;
                    if (!logged_sw_fail)
                    {
                        std::cerr << "DRM: sw-copy fallback failed (map/copy)" << std::endl;
                        logged_sw_fail = true;
                    }
                    gbm_bo_destroy(linear);
                }
                else
                {
                    scanout_bo = linear;
                    using_sw_copy = true;

                    std::memset(handles, 0, sizeof(handles));
                    std::memset(strides, 0, sizeof(strides));
                    std::memset(offsets, 0, sizeof(offsets));
                    std::memset(modifiers, 0, sizeof(modifiers));

                    handles[0] = gbm_bo_get_handle(scanout_bo).u32;
                    strides[0] = gbm_bo_get_stride(scanout_bo);

                    ret = drmModeAddFB2(disp->drm_fd, dmabuf->width, dmabuf->height, GBM_FORMAT_XRGB8888, handles,
                                        strides, offsets, &fb_id, 0);
                    if (ret)
                    {
                        static bool logged_sw_fail = false;
                        if (!logged_sw_fail)
                        {
                            std::cerr << "DRM: sw-copy fallback failed (addfb)" << std::endl;
                            logged_sw_fail = true;
                        }
                        gbm_bo_destroy(linear);
                        using_sw_copy = false;
                        scanout_bo = bo;
                    }
                }
            }

            if (!using_sw_copy)
            {
                ret = drmModeAddFB2(disp->drm_fd, dmabuf->width, dmabuf->height, dmabuf->format, handles, strides, offsets,
                                    &fb_id, 0);
                if (ret == 0)
                {
                }
            }
        }

        if (ret)
        {
            std::cerr << "Failed to create framebuffer: " << strerror(errno) << std::endl;
            if (using_sw_copy && scanout_bo)
            {
                gbm_bo_destroy(scanout_bo);
            }
            gbm_bo_destroy(bo);
            if (disp->exportable)
            {
                wpe_view_backend_exportable_fdo_dispatch_release_buffer(disp->exportable, dmabuf->buffer_resource);
            }
            return;
        }

        if (using_sw_copy && scanout_bo != bo)
        {
            gbm_bo_destroy(bo);
        }

        display::buffer_entry new_buffer{};
        new_buffer.bo = scanout_bo;
        new_buffer.fb_id = fb_id;
        new_buffer.resource = dmabuf->buffer_resource;

        if (disp->atomic_supported && disp->plane_id)
        {
            drmModeAtomicReq *req = drmModeAtomicAlloc();
            if (!req)
            {
                release_buffer(disp, &new_buffer);
                return;
            }

            uint32_t commit_flags = DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK;
            uint32_t blob_id = 0;
            int err = 0;
            const uint32_t src_w = dmabuf->width;
            const uint32_t src_h = dmabuf->height;
            const uint32_t crtc_w = disp->mode.hdisplay;
            const uint32_t crtc_h = disp->mode.vdisplay;

            if (!disp->mode_set)
            {
                commit_flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
                if (drmModeCreatePropertyBlob(disp->drm_fd, &disp->mode, sizeof(drmModeModeInfo), &blob_id) != 0)
                {
                    err = -1;
                }
                else
                {
                    err |= add_atomic_property(disp->drm_fd, req, disp->connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID",
                                                disp->crtc_id);
                    err |= add_atomic_property(disp->drm_fd, req, disp->crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID", blob_id);
                    err |= add_atomic_property(disp->drm_fd, req, disp->crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", 1);
                }
            }

            err |= add_atomic_property(disp->drm_fd, req, disp->plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", fb_id);
            err |= add_atomic_property(disp->drm_fd, req, disp->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", disp->crtc_id);
            err |= add_atomic_property(disp->drm_fd, req, disp->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X", 0);
            err |= add_atomic_property(disp->drm_fd, req, disp->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y", 0);
            err |= add_atomic_property(disp->drm_fd, req, disp->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W",
                                       static_cast<uint64_t>(src_w) << 16);
            err |= add_atomic_property(disp->drm_fd, req, disp->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H",
                                       static_cast<uint64_t>(src_h) << 16);
            err |= add_atomic_property(disp->drm_fd, req, disp->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X", 0);
            err |= add_atomic_property(disp->drm_fd, req, disp->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y", 0);
            err |= add_atomic_property(disp->drm_fd, req, disp->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W", crtc_w);
            err |= add_atomic_property(disp->drm_fd, req, disp->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H", crtc_h);

            if (err == 0 && drmModeAtomicCommit(disp->drm_fd, req, commit_flags, disp) == 0)
            {
                if (disp->pending_buffer.bo)
                {
                    release_buffer(disp, &disp->pending_buffer);
                }

                disp->pending_buffer = new_buffer;
                if (!disp->mode_set)
                {
                    disp->mode_set = true;
                }
            }
            else
            {
                release_buffer(disp, &new_buffer);
            }

            if (blob_id)
            {
                drmModeDestroyPropertyBlob(disp->drm_fd, blob_id);
            }

            drmModeAtomicFree(req);
            return;
        }

        if (!disp->mode_set)
        {
            if (drmModeSetCrtc(disp->drm_fd, disp->crtc_id, fb_id, 0, 0, &disp->connector_id, 1, &disp->mode) == 0)
            {
                release_buffer(disp, &disp->current_buffer);
                disp->current_buffer = new_buffer;
                disp->mode_set = true;

                if (disp->exportable)
                {
                    wpe_view_backend_exportable_fdo_dispatch_frame_complete(disp->exportable);
                }
            }
            else
            {
                std::cerr << "Failed to set CRTC: " << strerror(errno) << std::endl;
                release_buffer(disp, &new_buffer);
            }

            return;
        }

        if (drmModePageFlip(disp->drm_fd, disp->crtc_id, fb_id, DRM_MODE_PAGE_FLIP_EVENT, disp) != 0)
        {
            if (drmModeSetCrtc(disp->drm_fd, disp->crtc_id, fb_id, 0, 0, &disp->connector_id, 1, &disp->mode) == 0)
            {
                release_buffer(disp, &disp->current_buffer);
                disp->current_buffer = new_buffer;
                disp->mode_set = true;

                if (disp->exportable)
                {
                    wpe_view_backend_exportable_fdo_dispatch_frame_complete(disp->exportable);
                }
            }
            else
            {
                std::cerr << "Failed to set CRTC: " << strerror(errno) << std::endl;
                release_buffer(disp, &new_buffer);
            }

            return;
        }

        if (disp->pending_buffer.bo)
        {
            release_buffer(disp, &disp->pending_buffer);
        }

        disp->pending_buffer = new_buffer;
    }

} // namespace saucer::drm
