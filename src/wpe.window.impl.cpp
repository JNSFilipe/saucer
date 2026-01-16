#include "wpe.window.impl.hpp"

namespace saucer
{
    using native = window::impl::native;
    using event  = window::event;

    // WPE doesn't have traditional windowing, so most event setups are no-ops

    template <>
    void native::setup<event::decorated>(impl *)
    {
    }

    template <>
    void native::setup<event::resize>(impl *)
    {
    }

    template <>
    void native::setup<event::maximize>(impl *)
    {
    }

    template <>
    void native::setup<event::minimize>(impl *)
    {
    }

    template <>
    void native::setup<event::focus>(impl *)
    {
    }

    template <>
    void native::setup<event::close>(impl *)
    {
    }

    template <>
    void native::setup<event::closed>(impl *)
    {
    }
} // namespace saucer
