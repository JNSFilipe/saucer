#pragma once

#include <saucer/icon.hpp>

#include "wpe.utils.hpp"

#include <gdk-pixbuf/gdk-pixbuf.h>

namespace saucer
{
    struct icon::impl
    {
        utils::g_object_ptr<GdkPixbuf> pixbuf;
    };
} // namespace saucer
