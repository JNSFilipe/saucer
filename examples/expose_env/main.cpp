#include <saucer/smartview.hpp>

#include <cstdlib>

static constexpr const char *demo = R"html(
<!DOCTYPE html>
<html>
    <body>
        <h1>Saucer WPE/DRM (env set in code)</h1>
        <p>This example sets required WPE/DRM environment variables in C++.</p>
    </body>
</html>
)html";

static void set_env(const char *key, const char *value)
{
#ifdef _WIN32
    _putenv_s(key, value);
#else
    setenv(key, value, 1);
#endif
}

coco::stray start(saucer::application *app)
{
    auto window  = saucer::window::create(app).value();
    auto webview = saucer::smartview::create({.window = window});

    webview->set_background({255, 255, 255, 255});
    webview->set_html(demo);
    window->show();

    co_await app->finish();
}

int main()
{
    // WPE/DRM defaults for Raspberry Pi
    set_env("WPE_BACKEND_LIBRARY", "/usr/lib/aarch64-linux-gnu/libWPEBackend-fdo-1.0.so.1");
    set_env("WPE_FDO_PLATFORM", "drm");
    set_env("SAUCER_WPE_RENDERER", "dmabuf");

    if (!std::getenv("XDG_RUNTIME_DIR"))
    {
        set_env("XDG_RUNTIME_DIR", "/run/user/1000");
    }

    return saucer::application::create({.id = "example-env"})->run(start);
}
