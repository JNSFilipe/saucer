#pragma once

#include "webview.impl.hpp"

#include "wpe.utils.hpp"
#include "wpe.scheme.impl.hpp"
#include "wpe.drm.hpp"

#include <vector>

#include <wpe/webkit.h>
#include <wpe/fdo.h>
#include <wpe/fdo-egl.h>
#include <EGL/egl.h>

namespace saucer
{
    using script_ptr          = utils::ref_ptr<WebKitUserScript, webkit_user_script_ref, webkit_user_script_unref>;
    using content_manager_ptr = utils::g_object_ptr<WebKitUserContentManager>;

    struct wpe_script
    {
        script_ptr ref;
        bool clearable;
    };

    struct webview::impl::native
    {
        WebKitWebView *web_view;
        struct wpe_view_backend *backend{nullptr};
        struct wpe_view_backend_exportable_fdo *exportable{nullptr};
        std::unique_ptr<drm::display> drm_display;

      public:
        bool context_menu{true};

      public:
        content_manager_ptr manager;
        utils::g_object_ptr<WebKitSettings> settings;

      public:
        std::size_t id_counter{0};
        std::unordered_map<std::size_t, wpe_script> scripts;

      public:
        bool dom_loaded{false};
        std::vector<std::string> pending;

      public:
        std::size_t id_context;
        std::size_t id_load;

      public:
        std::size_t id_message;

      public:
        template <event>
        void setup(impl *);

      public:
        static gboolean on_context(WebKitWebView *, WebKitContextMenu *, WebKitHitTestResult *, impl *);

      public:
        static void on_message(WebKitWebView *, JSCValue *, impl *);
        static void on_load(WebKitWebView *, WebKitLoadEvent, impl *);

      public:
        static WebKitSettings *make_settings(const options &);
        static inline std::unordered_map<std::string, std::unique_ptr<scheme::handler>> schemes;
    };
} // namespace saucer
