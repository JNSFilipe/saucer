#pragma once

#include <saucer/permission.hpp>

#include "wpe.utils.hpp"

#include <wpe/webkit.h>

namespace saucer::permission
{
    struct request::impl
    {
        utils::g_object_ptr<WebKitPermissionRequest> request;

      public:
        saucer::url url;
        permission::type type;
    };
} // namespace saucer::permission
