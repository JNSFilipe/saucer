#pragma once

#include <saucer/navigation.hpp>

#include <wpe/webkit.h>

namespace saucer
{
    struct navigation::impl
    {
        WebKitNavigationPolicyDecision *decision;
        WebKitPolicyDecisionType type;
    };
} // namespace saucer
