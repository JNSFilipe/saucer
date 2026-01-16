#pragma once

#include "window.impl.hpp"

#include "lease.hpp"
#include "wpe.utils.hpp"

#include <optional>

namespace saucer
{
    struct window::impl::native
    {
        // WPE doesn't have traditional windowing
        // We store state locally for compatibility

      public:
        saucer::size current_size{800, 600};
        saucer::size min_size{};
        saucer::size max_size{};

      public:
        saucer::position current_position{};

      public:
        std::string title_text;
        saucer::color bg_color{};

      public:
        bool visible{false};
        bool is_focused{false};
        bool is_minimized{false};
        bool is_maximized{false};
        bool is_resizable{true};
        bool is_fullscreen{false};
        bool is_always_on_top{false};
        bool is_click_through{false};

      public:
        decoration current_decoration{decoration::full};

      public:
        utils::lease<window::impl *> lease;

      public:
        template <event>
        void setup(impl *);
    };
} // namespace saucer
