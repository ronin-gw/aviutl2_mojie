#pragma once

#include <functional>
#include <string>
#include <string_view>

#include <windows.h>

struct CONFIG_HANDLE;
struct HOST_APP_TABLE;

namespace mojie::aviutl {

enum class ImageSizeMode : int {
    LineHeight = 0,
    Percent = 1,
    Pixels = 2,
};

// Per-object image presentation values passed by the generated Text wrapper.
// enabled=false means that image-specific configuration should be inherited.
struct ImageOverride {
    bool enabled = false;
    ImageSizeMode size_mode = ImageSizeMode::LineHeight;
    double size_value = 100.0;
    double padding_x = 0.0;
    double padding_y = 0.0;
};

// render is called from AviUtl2's rendering threads and therefore must be
// thread-safe.  settings is called from the main UI thread.
struct Callbacks {
    std::function<std::string(
        std::string_view, double, const ImageOverride&)> render;
    std::function<void(HWND, HINSTANCE)> settings;
};

// Replacing or clearing the callbacks is safe while a render callback is in
// progress.  The in-flight callback retains its own copy until it returns.
void SetCallbacks(Callbacks callbacks);
void ClearCallbacks();

// These entry points are intended to be forwarded from the DLL exports in the
// common-plugin translation unit.
void InitializeConfig(CONFIG_HANDLE* config);
// Returns true only when registration was performed by this call. This lets
// the common-plugin entry point avoid registering its companion callbacks
// twice if AviUtl2 calls RegisterPlugin more than once.
bool RegisterPlugin(HOST_APP_TABLE* host);
void UninitializePlugin();

// Returns the AviUtl2 application data directory captured by
// InitializeConfig().  An empty string means it is not available yet.
std::wstring GetAppDataPath();

} // namespace mojie::aviutl
