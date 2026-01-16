#include "wpe.window.impl.hpp"

#include "instantiate.hpp"
#include "wpe.app.impl.hpp"

namespace saucer
{
    using impl = window::impl;

    impl::impl() = default;

    result<> impl::init_platform()
    {
        platform = std::make_unique<native>();

        platform->lease = utils::lease{this};
        platform->current_size = {.w = 800, .h = 600};

        return {};
    }

    impl::~impl()
    {
        if (!platform)
        {
            return;
        }

        // Notify about window close
        if (platform->visible)
        {
            auto *parent     = this->parent;
            auto *const impl = parent->native<false>()->platform.get();
            auto &instances  = impl->instances;

            instances.erase(this);
            events.get<event::closed>().fire();

            if (impl->quit_on_last_window_closed && instances.empty())
            {
                parent->quit();
            }
        }
    }

    template <window::event Event>
    void impl::setup()
    {
        platform->setup<Event>(this);
    }

    bool impl::visible() const
    {
        return platform->visible;
    }

    bool impl::focused() const
    {
        return platform->is_focused;
    }

    bool impl::minimized() const
    {
        return platform->is_minimized;
    }

    bool impl::maximized() const
    {
        return platform->is_maximized;
    }

    bool impl::resizable() const
    {
        return platform->is_resizable;
    }

    bool impl::fullscreen() const
    {
        return platform->is_fullscreen;
    }

    bool impl::always_on_top() const
    {
        return platform->is_always_on_top;
    }

    bool impl::click_through() const
    {
        return platform->is_click_through;
    }

    std::string impl::title() const
    {
        return platform->title_text;
    }

    color impl::background() const
    {
        return platform->bg_color;
    }

    window::decoration impl::decorations() const
    {
        return platform->current_decoration;
    }

    size impl::size() const
    {
        return platform->current_size;
    }

    size impl::max_size() const
    {
        return platform->max_size;
    }

    size impl::min_size() const
    {
        return platform->min_size;
    }

    position impl::position() const
    {
        return platform->current_position;
    }

    std::optional<saucer::screen> impl::screen() const
    {
        // WPE doesn't have display enumeration
        return application::impl::native::convert_screen();
    }

    void impl::hide() const
    {
        platform->visible = false;
    }

    void impl::show() const
    {
        parent->native<false>()->platform->instances[const_cast<impl *>(this)] = true;
        platform->visible = true;
    }

    void impl::close() const
    {
        if (const_cast<impl *>(this)->events.get<event::close>().fire().find(policy::block))
        {
            return;
        }

        platform->visible = false;

        auto *parent     = this->parent;
        auto *const impl = parent->native<false>()->platform.get();
        auto &instances  = impl->instances;

        instances.erase(const_cast<window::impl *>(this));
        const_cast<window::impl *>(this)->events.get<event::closed>().fire();

        if (impl->quit_on_last_window_closed && instances.empty())
        {
            parent->quit();
        }
    }

    void impl::focus() const
    {
        platform->is_focused = true;
    }

    void impl::start_drag() const
    {
        // No-op for WPE (no traditional windowing)
    }

    void impl::start_resize(edge)
    {
        // No-op for WPE (no traditional windowing)
    }

    void impl::set_minimized(bool enabled)
    {
        platform->is_minimized = enabled;
    }

    void impl::set_maximized(bool enabled)
    {
        const bool prev = platform->is_maximized;
        platform->is_maximized = enabled;

        if (prev != enabled)
        {
            events.get<event::maximize>().fire(enabled);
        }
    }

    void impl::set_resizable(bool enabled)
    {
        platform->is_resizable = enabled;
    }

    void impl::set_fullscreen(bool enabled)
    {
        platform->is_fullscreen = enabled;
    }

    void impl::set_always_on_top(bool enabled)
    {
        platform->is_always_on_top = enabled;
    }

    void impl::set_click_through(bool enabled)
    {
        platform->is_click_through = enabled;
    }

    void impl::set_icon(const icon &)
    {
        // No-op for WPE (no window icon)
    }

    void impl::set_title(cstring_view title)
    {
        platform->title_text = std::string{title};
    }

    void impl::set_background(color color)
    {
        platform->bg_color = color;
    }

    void impl::set_decorations(decoration decoration)
    {
        const auto prev = platform->current_decoration;
        platform->current_decoration = decoration;

        if (prev != decoration)
        {
            events.get<event::decorated>().fire(decoration);
        }
    }

    void impl::set_size(saucer::size size)
    {
        const auto prev = platform->current_size;
        platform->current_size = size;

        if (prev.w != size.w || prev.h != size.h)
        {
            events.get<event::resize>().fire(size.w, size.h);
        }
    }

    void impl::set_max_size(saucer::size size)
    {
        platform->max_size = size;
    }

    void impl::set_min_size(saucer::size size)
    {
        platform->min_size = size;
    }

    void impl::set_position(saucer::position pos)
    {
        platform->current_position = pos;
    }

    SAUCER_INSTANTIATE_WINDOW_EVENTS(SAUCER_INSTANTIATE_WINDOW_IMPL_EVENT);
} // namespace saucer
