#include "wpe.app.impl.hpp"

namespace saucer
{
    using native = application::impl::native;

    void native::iteration()
    {
        auto *const context = g_main_context_default();
        g_main_context_iteration(context, false);
    }

    screen native::convert_screen()
    {
        // WPE doesn't have traditional display enumeration
        // Return a default screen representing the embedded display
        return {
            .name     = "WPE Display",
            .size     = {.w = 1920, .h = 1080},
            .position = {.x = 0, .y = 0},
        };
    }
} // namespace saucer
