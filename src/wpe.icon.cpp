#include "wpe.icon.impl.hpp"

#include "wpe.error.hpp"

#include <cassert>

namespace saucer
{
    icon::icon() : m_impl(std::make_unique<impl>()) {}

    icon::icon(impl data) : m_impl(std::make_unique<impl>(std::move(data))) {}

    icon::icon(const icon &other) : icon(*other.m_impl) {}

    icon::icon(icon &&other) noexcept : icon()
    {
        swap(*this, other);
    }

    icon::~icon() = default;

    icon &icon::operator=(icon other) noexcept
    {
        swap(*this, other);
        return *this;
    }

    void swap(icon &first, icon &second) noexcept
    {
        using std::swap;
        swap(first.m_impl, second.m_impl);
    }

    bool icon::empty() const
    {
        return !m_impl->pixbuf;
    }

    stash icon::data() const
    {
        if (!m_impl->pixbuf)
        {
            return stash::empty();
        }

        gchar *buffer{};
        gsize size{};
        auto error = utils::g_error_ptr{};

        if (!gdk_pixbuf_save_to_buffer(m_impl->pixbuf.get(), &buffer, &size, "png", &error.reset(), nullptr))
        {
            return stash::empty();
        }

        auto result = stash::from({reinterpret_cast<const std::uint8_t *>(buffer),
                                   reinterpret_cast<const std::uint8_t *>(buffer) + size});
        g_free(buffer);

        return result;
    }

    void icon::save(const fs::path &path) const
    {
        assert(path.extension() == ".png");

        if (!m_impl->pixbuf)
        {
            return;
        }

        gdk_pixbuf_save(m_impl->pixbuf.get(), path.c_str(), "png", nullptr, nullptr);
    }

    result<icon> icon::from(const stash &ico)
    {
        auto error = utils::g_error_ptr{};
        auto *const stream = g_memory_input_stream_new_from_data(ico.data(), static_cast<gssize>(ico.size()), nullptr);
        auto *const pixbuf = gdk_pixbuf_new_from_stream(G_INPUT_STREAM(stream), nullptr, &error.reset());
        g_object_unref(stream);

        if (!pixbuf)
        {
            return err(std::move(error));
        }

        return icon{{pixbuf}};
    }

    result<icon> icon::from(const fs::path &file)
    {
        auto error = utils::g_error_ptr{};
        auto *const pixbuf = gdk_pixbuf_new_from_file(file.string().c_str(), &error.reset());

        if (!pixbuf)
        {
            return err(std::move(error));
        }

        return icon{{pixbuf}};
    }
} // namespace saucer
