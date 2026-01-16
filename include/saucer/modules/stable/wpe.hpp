#pragma once

#include <saucer/modules/module.hpp>

#include <saucer/app.hpp>
#include <saucer/window.hpp>
#include <saucer/webview.hpp>

#include <glib.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <wpe/webkit.h>

namespace saucer
{
    template <>
    struct stable_natives<application>
    {
        GMainLoop *main_loop;
    };

    template <>
    struct stable_natives<window>
    {
        // WPE doesn't have traditional windowing
        // No native window type to expose
    };

    template <>
    struct stable_natives<webview>
    {
        WebKitWebView *webview;
    };

    template <>
    struct stable_natives<permission::request>
    {
        WebKitPermissionRequest *request;
    };

    template <>
    struct stable_natives<url>
    {
        GUri *uri;
    };

    template <>
    struct stable_natives<icon>
    {
        GdkPixbuf *icon;
    };
} // namespace saucer
