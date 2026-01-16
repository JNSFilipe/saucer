#include "wpe.webview.impl.hpp"

#include "error.impl.hpp" // IWYU pragma: keep

#include "scripts.hpp"
#include "instantiate.hpp"

#include "wpe.icon.impl.hpp"
#include "wpe.window.impl.hpp"
#include "wpe.scheme.impl.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <wpe/wpe.h>
#include <EGL/eglext.h>

namespace
{
    [[maybe_unused]] static bool egl_has_extension(EGLDisplay display, const char *name)
    {
        const char *exts = eglQueryString(display, EGL_EXTENSIONS);
        if (!exts || !name)
        {
            return false;
        }

        const std::string haystack{exts};
        const std::string needle{name};
        return haystack.find(needle) != std::string::npos;
    }

    enum class renderer_mode
    {
        egl,
        dmabuf_scanout,
        dmabuf_gl,
    };

    static renderer_mode get_renderer_mode()
    {
        const char *env = std::getenv("SAUCER_WPE_RENDERER");
        if (!env || !*env)
        {
            return renderer_mode::egl;
        }

        if (std::strcmp(env, "dmabuf") == 0)
        {
            return renderer_mode::dmabuf_scanout;
        }

        if (std::strcmp(env, "dmabuf-gl") == 0)
        {
            return renderer_mode::dmabuf_gl;
        }

        return renderer_mode::egl;
    }


        static bool debug_enabled()
    {
        return std::getenv("SAUCER_WPE_DEBUG") != nullptr;
    }

static std::string fourcc_to_string(uint32_t value)
    {
        char s[5];
        s[0] = static_cast<char>(value & 0xFF);
        s[1] = static_cast<char>((value >> 8) & 0xFF);
        s[2] = static_cast<char>((value >> 16) & 0xFF);
        s[3] = static_cast<char>((value >> 24) & 0xFF);
        s[4] = 0;
        return std::string{s};
    }

    [[maybe_unused]] static EGLImageKHR import_dmabuf_image(EGLDisplay display, const wpe_view_backend_exportable_fdo_dmabuf_resource *res)
    {
        auto create_image = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
        if (!create_image)
        {
            fprintf(stderr, "eglCreateImageKHR not available\n");
            return EGL_NO_IMAGE_KHR;
        }

        const bool force_no_modifiers = std::getenv("SAUCER_EGL_NO_MODIFIERS") != nullptr;
        const bool has_modifiers = !force_no_modifiers &&
                                   egl_has_extension(display, "EGL_EXT_image_dma_buf_import_modifiers");

        static const EGLint fd_keys[] = {
            EGL_DMA_BUF_PLANE0_FD_EXT,
            EGL_DMA_BUF_PLANE1_FD_EXT,
            EGL_DMA_BUF_PLANE2_FD_EXT,
            EGL_DMA_BUF_PLANE3_FD_EXT,
        };
        static const EGLint offset_keys[] = {
            EGL_DMA_BUF_PLANE0_OFFSET_EXT,
            EGL_DMA_BUF_PLANE1_OFFSET_EXT,
            EGL_DMA_BUF_PLANE2_OFFSET_EXT,
            EGL_DMA_BUF_PLANE3_OFFSET_EXT,
        };
        static const EGLint pitch_keys[] = {
            EGL_DMA_BUF_PLANE0_PITCH_EXT,
            EGL_DMA_BUF_PLANE1_PITCH_EXT,
            EGL_DMA_BUF_PLANE2_PITCH_EXT,
            EGL_DMA_BUF_PLANE3_PITCH_EXT,
        };
        static const EGLint mod_lo_keys[] = {
            EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
            EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
            EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
            EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT,
        };
        static const EGLint mod_hi_keys[] = {
            EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
            EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
            EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
            EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT,
        };

        std::vector<EGLint> attribs;
        attribs.reserve(64);
        attribs.push_back(EGL_WIDTH);
        attribs.push_back(static_cast<EGLint>(res->width));
        attribs.push_back(EGL_HEIGHT);
        attribs.push_back(static_cast<EGLint>(res->height));
        attribs.push_back(EGL_LINUX_DRM_FOURCC_EXT);
        attribs.push_back(static_cast<EGLint>(res->format));

        const uint8_t planes = res->n_planes > 4 ? 4 : res->n_planes;
        for (uint8_t i = 0; i < planes; ++i)
        {
            attribs.push_back(fd_keys[i]);
            attribs.push_back(res->fds[i]);
            attribs.push_back(offset_keys[i]);
            attribs.push_back(static_cast<EGLint>(res->offsets[i]));
            attribs.push_back(pitch_keys[i]);
            attribs.push_back(static_cast<EGLint>(res->strides[i]));

            if (has_modifiers)
            {
                const uint64_t mod = res->modifiers[i];
                attribs.push_back(mod_lo_keys[i]);
                attribs.push_back(static_cast<EGLint>(mod & 0xFFFFFFFFu));
                attribs.push_back(mod_hi_keys[i]);
                attribs.push_back(static_cast<EGLint>((mod >> 32) & 0xFFFFFFFFu));
            }
        }

        attribs.push_back(EGL_NONE);


        EGLImageKHR image = create_image(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs.data());
        const EGLint err = eglGetError();
        if (err != EGL_SUCCESS)
        {
            fprintf(stderr, "eglCreateImageKHR error: 0x%x\n", err);
        }

        return image;
    }
} // namespace

namespace saucer
{
    using impl = webview::impl;

    impl::impl() = default;

    result<> impl::init_platform(const options &opts)
    {
        platform = std::make_unique<native>();

        // Initialize DRM display for direct rendering
        platform->drm_display = drm::init();
        if (!platform->drm_display)
        {
            return err(std::errc::io_error);
        }

        // Initialize WPE FDO backend with DRM's EGL display
        if (!wpe_fdo_initialize_for_egl_display(platform->drm_display->egl_display))
        {
            return err(std::errc::io_error);
        }

        // Get window size from DRM display
        auto width  = platform->drm_display->width;
        auto height = platform->drm_display->height;

        // Create FDO exportable backend with callbacks
        const auto mode = get_renderer_mode();
        if (mode == renderer_mode::dmabuf_scanout || mode == renderer_mode::dmabuf_gl)
        {
            static const struct wpe_view_backend_exportable_fdo_client exportable_client = {
                .export_dmabuf_resource = [](void *data, struct wpe_view_backend_exportable_fdo_dmabuf_resource *dmabuf) {
                    auto *native_ptr = static_cast<native *>(data);
                    if (!native_ptr || !native_ptr->drm_display)
                    {
                        fprintf(stderr, "Export callback: no DRM display or null native_ptr\n");
                        return;
                    }

                    auto *disp = native_ptr->drm_display.get();
                    const auto mode = get_renderer_mode();
                    if (mode == renderer_mode::dmabuf_gl)
                    {
                        if (debug_enabled())
                        {
                            fprintf(stderr, "dmabuf-gl: importing %ux%u format=0x%x mod=0x%llx\n",
                                    dmabuf->width, dmabuf->height, dmabuf->format,
                                    static_cast<unsigned long long>(dmabuf->modifiers[0]));
                        }
                        EGLImageKHR image = import_dmabuf_image(disp->egl_display, dmabuf);
                        if (image == EGL_NO_IMAGE_KHR)
                        {
                            if (debug_enabled())
                            {
                                fprintf(stderr, "dmabuf-gl: EGLImage import failed\n");
                            }
                        }
                        else
                        {
                            drm::render_image(disp, image, static_cast<int>(dmabuf->width), static_cast<int>(dmabuf->height));
                            auto destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
                            if (destroy_image)
                            {
                                destroy_image(disp->egl_display, image);
                            }
                        }

                        if (native_ptr->exportable)
                        {
                            wpe_view_backend_exportable_fdo_dispatch_release_buffer(native_ptr->exportable, dmabuf->buffer_resource);
                            wpe_view_backend_exportable_fdo_dispatch_frame_complete(native_ptr->exportable);
                        }
                        return;
                    }

                    if (debug_enabled())
                    {
                        fprintf(stderr, "dmabuf format=0x%x (%s), planes=%u\n",
                                dmabuf->format, fourcc_to_string(dmabuf->format).c_str(), dmabuf->n_planes);
                        for (uint8_t i = 0; i < dmabuf->n_planes && i < 4; ++i)
                        {
                            fprintf(stderr, "  plane %u: fd=%d stride=%u offset=%u modifier=0x%llx\n",
                                    i, dmabuf->fds[i], dmabuf->strides[i], dmabuf->offsets[i],
                                    static_cast<unsigned long long>(dmabuf->modifiers[i]));
                        }

                        fprintf(stderr, "Export callback: rendering dmabuf %ux%u\n", dmabuf->width, dmabuf->height);
                    }

                    drm::render_dmabuf(disp, dmabuf);
                },
            };

            platform->exportable = wpe_view_backend_exportable_fdo_create(&exportable_client, platform.get(), width, height);
        }
        else
        {
            static const struct wpe_view_backend_exportable_fdo_egl_client exportable_client = {
                .export_fdo_egl_image = [](void *data, struct wpe_fdo_egl_exported_image *image) {
                    auto *native_ptr = static_cast<native *>(data);
                    if (!native_ptr || !native_ptr->drm_display)
                    {
                        fprintf(stderr, "Export callback: no DRM display or null native_ptr\n");
                        return;
                    }

                    auto *disp = native_ptr->drm_display.get();
                    auto egl_image = wpe_fdo_egl_exported_image_get_egl_image(image);
                    auto img_width = wpe_fdo_egl_exported_image_get_width(image);
                    auto img_height = wpe_fdo_egl_exported_image_get_height(image);
                    if (debug_enabled())
                    {
                        fprintf(stderr, "Export callback: rendering EGL image %ux%u\n", img_width, img_height);
                    }
                    drm::render_image(disp, egl_image, static_cast<int>(img_width), static_cast<int>(img_height));

                    if (native_ptr->exportable)
                    {
                        wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image(native_ptr->exportable, image);
                        wpe_view_backend_exportable_fdo_dispatch_frame_complete(native_ptr->exportable);
                    }
                },
                .export_shm_buffer = [](void *data, struct wpe_fdo_shm_exported_buffer *buffer) {
                    auto *native_ptr = static_cast<native *>(data);
                    if (native_ptr && native_ptr->exportable)
                    {
                        wpe_view_backend_exportable_fdo_egl_dispatch_release_shm_exported_buffer(native_ptr->exportable, buffer);
                    }
                },
            };

            platform->exportable = wpe_view_backend_exportable_fdo_egl_create(&exportable_client, platform.get(), width, height);
        }

        if (platform->drm_display)
        {
            platform->drm_display->exportable = platform->exportable;
        }

        if (!platform->exportable)
        {
            return err(std::errc::io_error);
        }

        // Get the underlying wpe_view_backend
        auto *const wpe_backend = wpe_view_backend_exportable_fdo_get_view_backend(platform->exportable);

        platform->backend = wpe_backend;
        wpe_view_backend_initialize(wpe_backend);
        wpe_view_backend_dispatch_set_size(wpe_backend, width, height);
        wpe_view_backend_add_activity_state(wpe_backend, wpe_view_activity_state_visible |
                                            wpe_view_activity_state_focused |
                                            wpe_view_activity_state_in_window);
        wpe_view_backend_dispatch_set_device_scale_factor(wpe_backend, 1.0f);

        // Create WebKitWebViewBackend - it takes ownership of the wpe_view_backend
        auto *const backend = webkit_web_view_backend_new(
            wpe_backend,
            [](gpointer data) {
                auto *exportable = static_cast<struct wpe_view_backend_exportable_fdo *>(data);
                wpe_view_backend_exportable_fdo_destroy(exportable);
            },
            platform->exportable);

        platform->web_view = WEBKIT_WEB_VIEW(webkit_web_view_new(backend));
        platform->settings = native::make_settings(opts);

        // WPE doesn't have hardware acceleration policy settings - it's always hardware accelerated

        if (opts.user_agent.has_value())
        {
            webkit_settings_set_user_agent(platform->settings.get(), opts.user_agent->c_str());
        }

        webkit_web_view_set_settings(platform->web_view, platform->settings.get());

        auto *const session = webkit_web_view_get_network_session(platform->web_view);

        // WPE doesn't have favicon support

        if (opts.persistent_cookies)
        {
            auto *const manager = webkit_network_session_get_cookie_manager(session);
            auto path           = opts.storage_path.value_or(fs::current_path() / ".saucer");
            webkit_cookie_manager_set_persistent_storage(manager, path.c_str(), WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
        }

        platform->id_context = utils::connect(platform->web_view, "context-menu", native::on_context, this);
        platform->id_load    = utils::connect(platform->web_view, "load-changed", native::on_load, this);

        // The ContentManager is ref'd to prevent it from being destroyed early when using multiple webviews
        platform->manager = content_manager_ptr::ref(webkit_web_view_get_user_content_manager(platform->web_view));
        webkit_user_content_manager_register_script_message_handler(platform->manager.get(), "saucer", nullptr);

        platform->id_message = utils::connect(platform->manager.get(), "script-message-received", native::on_message, this);

        return {};
    }

    impl::~impl()
    {
        if (!platform)
        {
            return;
        }

        for (const auto &[name, _] : native::schemes)
        {
            remove_scheme(name);
        }

        g_signal_handler_disconnect(platform->web_view, platform->id_context);
        g_signal_handler_disconnect(platform->web_view, platform->id_load);

        g_signal_handler_disconnect(platform->manager.get(), platform->id_message);
    }

    template <webview::event Event>
    void impl::setup()
    {
        platform->setup<Event>(this);
    }

    result<url> impl::url() const
    {
        const auto *uri = webkit_web_view_get_uri(platform->web_view);

        if (!uri)
        {
            return err(std::errc::not_connected);
        }

        return url::parse(uri);
    }

    icon impl::favicon() const
    {
        // WPE WebKit returns a GdkTexture, but we use GdkPixbuf for WPE
        // For now, return empty icon as favicon support in embedded is limited
        return icon{};
    }

    std::string impl::page_title() const
    {
        const auto *rtn = webkit_web_view_get_title(platform->web_view);

        if (!rtn)
        {
            return {};
        }

        return rtn;
    }

    bool impl::dev_tools() const
    {
        // WPE doesn't have web inspector support
        return false;
    }

    bool impl::context_menu() const
    {
        return platform->context_menu;
    }

    bool impl::force_dark() const
    {
        // WPE doesn't have Adwaita color scheme
        return false;
    }

    color impl::background() const
    {
        WebKitColor wk_color{};
        webkit_web_view_get_background_color(platform->web_view, &wk_color);

        return {
            .r = static_cast<std::uint8_t>(wk_color.red * 255.0),
            .g = static_cast<std::uint8_t>(wk_color.green * 255.0),
            .b = static_cast<std::uint8_t>(wk_color.blue * 255.0),
            .a = static_cast<std::uint8_t>(wk_color.alpha * 255.0),
        };
    }

    bounds impl::bounds() const
    {
        auto [width, height] = window->size();
        return {
            .x = 0,
            .y = 0,
            .w = width,
            .h = height,
        };
    }

    void impl::set_url(const saucer::url &url)
    {
        webkit_web_view_load_uri(platform->web_view, url.string().c_str());
    }

    void impl::set_html(cstring_view html)
    {
        webkit_web_view_load_html(platform->web_view, html.c_str(), nullptr);
    }

    void impl::set_dev_tools(bool enabled)
    {
        // WPE doesn't have web inspector support
        (void)enabled;
    }

    void impl::set_context_menu(bool enabled)
    {
        platform->context_menu = enabled;
    }

    void impl::set_force_dark(bool)
    {
        // WPE doesn't have Adwaita color scheme - no-op
    }

    void impl::set_background(color color)
    {
        const auto [r, g, b, a] = color;

        WebKitColor wk_color{
            .red   = static_cast<gdouble>(r) / 255.0,
            .green = static_cast<gdouble>(g) / 255.0,
            .blue  = static_cast<gdouble>(b) / 255.0,
            .alpha = static_cast<gdouble>(a) / 255.0,
        };

        webkit_web_view_set_background_color(platform->web_view, &wk_color);
    }

    void impl::reset_bounds()
    {
        // No-op for WPE (no widget margins)
    }

    void impl::set_bounds(saucer::bounds)
    {
        // No-op for WPE (no widget margins)
    }

    void impl::back()
    {
        webkit_web_view_go_back(platform->web_view);
    }

    void impl::forward()
    {
        webkit_web_view_go_forward(platform->web_view);
    }

    void impl::reload()
    {
        webkit_web_view_reload(platform->web_view);
    }

    void impl::execute(cstring_view code)
    {
        if (!platform->dom_loaded)
        {
            platform->pending.emplace_back(code);
            return;
        }

        webkit_web_view_evaluate_javascript(platform->web_view, code.c_str(), -1, nullptr, nullptr, nullptr, nullptr, nullptr);
    }

    std::size_t impl::inject(const script &script)
    {
        using enum script::time;

        const auto time = (script.run_at == creation) ? WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START //
                                                      : WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END;

        const auto frame = (script.no_frames) ? WEBKIT_USER_CONTENT_INJECT_TOP_FRAME //
                                              : WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES;

        auto *const user_script = webkit_user_script_new(script.code.c_str(), frame, time, nullptr, nullptr);
        const auto id           = platform->id_counter++;

        webkit_user_content_manager_add_script(platform->manager.get(), user_script);
        platform->scripts.emplace(id, wpe_script{.ref = user_script, .clearable = script.clearable});

        return id;
    }

    void impl::uninject()
    {
        for (auto it = platform->scripts.begin(); it != platform->scripts.end();)
        {
            const auto &[id, script] = *it;

            if (!script.clearable)
            {
                ++it;
                continue;
            }

            webkit_user_content_manager_remove_script(platform->manager.get(), script.ref.get());
            it = platform->scripts.erase(it);
        }
    }

    void impl::uninject(std::size_t id)
    {
        if (!platform->scripts.contains(id))
        {
            return;
        }

        webkit_user_content_manager_remove_script(platform->manager.get(), platform->scripts[id].ref.get());
        platform->scripts.erase(id);
    }

    void impl::handle_scheme(const std::string &name, scheme::resolver &&resolver)
    {
        if (!native::schemes.contains(name))
        {
            return;
        }

        native::schemes[name]->add_callback(platform->web_view, std::move(resolver));
    }

    void impl::remove_scheme(const std::string &name)
    {
        if (!native::schemes.contains(name))
        {
            return;
        }

        native::schemes[name]->del_callback(platform->web_view);
    }

    void impl::register_scheme(const std::string &name)
    {
        if (native::schemes.contains(name))
        {
            return;
        }

        auto *const context  = webkit_web_context_get_default();
        auto *const security = webkit_web_context_get_security_manager(context);

        auto handler  = std::make_unique<scheme::handler>();
        auto callback = reinterpret_cast<WebKitURISchemeRequestCallback>(&scheme::handler::handle);

        webkit_web_context_register_uri_scheme(context, name.c_str(), callback, handler.get(), nullptr);
        native::schemes.emplace(name, std::move(handler));

        webkit_security_manager_register_uri_scheme_as_secure(security, name.c_str());
        webkit_security_manager_register_uri_scheme_as_cors_enabled(security, name.c_str());
    }

    std::string impl::ready_script()
    {
        return "window.saucer.internal.message('dom_loaded')";
    }

    std::string impl::creation_script()
    {
        static const auto script = std::format(scripts::ipc_script, R"js(
            message: async (message) =>
            {
                window.webkit.messageHandlers.saucer.postMessage(message);
            }
        )js");

        return script;
    }

    SAUCER_INSTANTIATE_WEBVIEW_EVENTS(SAUCER_INSTANTIATE_WEBVIEW_IMPL_EVENT);
} // namespace saucer
