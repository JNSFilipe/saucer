#pragma once

#include "app.impl.hpp"
#include "wpe.utils.hpp"

#include <unordered_map>

#include <glib.h>

namespace saucer
{
    struct application::impl::native
    {
        GMainLoop *main_loop;
        GMainContext *context;

      public:
        int argc;
        char **argv;

      public:
        bool quit_on_last_window_closed;
        std::unordered_map<void *, bool> instances;

      public:
        static void iteration();
        static screen convert_screen();
    };
} // namespace saucer
