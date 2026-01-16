#include "modules/stable/wpe.hpp"

#include "wpe.app.impl.hpp"
#include "wpe.icon.impl.hpp"
#include "wpe.window.impl.hpp"

#include "wpe.url.impl.hpp"
#include "wpe.webview.impl.hpp"
#include "wpe.permission.impl.hpp"

namespace saucer
{
    template <>
    natives<application, true> application::native<true>() const
    {
        return {.main_loop = m_impl->platform->main_loop};
    }

    template <>
    natives<window, true> window::native<true>() const
    {
        return {};
    }

    template <>
    natives<webview, true> webview::native<true>() const
    {
        return {.webview = m_impl->platform->web_view};
    }

    template <>
    natives<permission::request, true> permission::request::native<true>() const
    {
        return {.request = m_impl->request.get()};
    }

    template <>
    natives<url, true> url::native<true>() const
    {
        return {.uri = m_impl->uri.get()};
    }

    template <>
    natives<icon, true> icon::native<true>() const
    {
        return {.icon = m_impl->pixbuf.get()};
    }
} // namespace saucer
