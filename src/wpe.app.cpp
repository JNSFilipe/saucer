#include "wpe.app.impl.hpp"

#include <format>

namespace saucer
{
    using impl = application::impl;

    impl::impl() = default;

    result<> impl::init_platform(const options &opts)
    {
        platform = std::make_unique<native>();

        platform->context   = g_main_context_default();
        platform->main_loop = g_main_loop_new(platform->context, FALSE);

        platform->argc                       = opts.argc.value_or(0);
        platform->argv                       = opts.argv.value_or(nullptr);
        platform->quit_on_last_window_closed = opts.quit_on_last_window_closed;

        return {};
    }

    impl::~impl()
    {
        if (platform && platform->main_loop)
        {
            g_main_loop_unref(platform->main_loop);
        }
    }

    std::vector<screen> impl::screens() const
    {
        // WPE doesn't have display enumeration like GDK
        // Return a single virtual screen with default dimensions
        return {native::convert_screen()};
    }

    void application::post(post_callback_t callback) const
    {
        auto once = [](post_callback_t *data)
        {
            auto callback = std::unique_ptr<post_callback_t>{data};
            (*callback)();
        };

        g_idle_add_once(reinterpret_cast<GSourceOnceFunc>(+once), new post_callback_t{std::move(callback)});
    }

    int impl::run(application *self, callback_t callback)
    {
        auto promise = coco::promise<void>{};

        finish = promise.get_future();

        // Unlike GTK's "activate" signal, we call the callback immediately
        callback(self);

        g_main_loop_run(platform->main_loop);

        promise.set_value();

        return 0;
    }

    void impl::quit()
    {
        if (platform && platform->main_loop)
        {
            g_main_loop_quit(platform->main_loop);
        }
    }
} // namespace saucer
