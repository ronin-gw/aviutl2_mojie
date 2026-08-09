#include <windows.h>

#include <aviutl2_sdk/config2.h>
#include <aviutl2_sdk/plugin2.h>

#include "app_state.hpp"
#include "aviutl_plugin.hpp"
#include "mojie_version.hpp"

namespace {

constexpr DWORD kRequiredAviUtl2Version = 2010301; // AviUtl2 2.1.3a

COMMON_PLUGIN_TABLE kPluginTable = {
    L"mojie",
    L"mojie " MOJIE_VERSION_WSTRING L" - テキストをインライン画像へ置換",
};

void DebugInitializationError(const std::wstring& error) noexcept {
    OutputDebugStringW(L"mojie: ");
    OutputDebugStringW(
        error.empty() ? L"初期化に失敗しました。" : error.c_str());
    OutputDebugStringW(L"\n");
}

void ResetAfterInitializationFailure() noexcept {
    try {
        mojie::aviutl::ClearCallbacks();
    } catch (...) {
    }
    try {
        mojie::ShutdownApplication();
    } catch (...) {
    }
}

void HandleProjectLoadBoundary(PROJECT_FILE* project) noexcept {
    try {
        mojie::HandleProjectLoad(project);
    } catch (...) {
        OutputDebugStringW(L"mojie: プロジェクト設定の読み込みに失敗しました。\n");
    }
}

void HandleProjectSaveBoundary(PROJECT_FILE* project) noexcept {
    try {
        mojie::HandleProjectSave(project);
    } catch (...) {
        OutputDebugStringW(L"mojie: プロジェクト設定の保存に失敗しました。\n");
    }
}

} // namespace

extern "C" __declspec(dllexport) DWORD RequiredVersion() noexcept {
    return kRequiredAviUtl2Version;
}

extern "C" __declspec(dllexport) void InitializeConfig(CONFIG_HANDLE* config) noexcept {
    try {
        mojie::aviutl::InitializeConfig(config);
    } catch (...) {
        OutputDebugStringW(L"mojie: AviUtl2設定の初期化に失敗しました。\n");
    }
}

extern "C" __declspec(dllexport) bool InitializePlugin(DWORD) noexcept {
    try {
        std::wstring error;
        if (!mojie::InitializeApplication(&error)) {
            DebugInitializationError(error);
            ResetAfterInitializationFailure();
            return false;
        }
        mojie::aviutl::SetCallbacks({
            [](std::string_view text, double base_size,
               const mojie::aviutl::ImageOverride& image_override) {
                mojie::ReplacementStyleOverride style;
                style.enabled = image_override.enabled;
                switch (image_override.size_mode) {
                case mojie::aviutl::ImageSizeMode::Percent:
                    style.size_mode = mojie::SizeMode::Percent;
                    break;
                case mojie::aviutl::ImageSizeMode::Pixels:
                    style.size_mode = mojie::SizeMode::Pixels;
                    break;
                case mojie::aviutl::ImageSizeMode::LineHeight:
                default:
                    style.size_mode = mojie::SizeMode::LineHeight;
                    break;
                }
                style.size_value = image_override.size_value;
                style.padding_x = image_override.padding_x;
                style.padding_y = image_override.padding_y;
                return mojie::RenderText(text, base_size, style);
            },
            [](HWND parent, HINSTANCE instance) {
                mojie::ShowSettings(parent, instance);
            },
        });
        return true;
    } catch (...) {
        OutputDebugStringW(L"mojie: 初期化中に予期しないエラーが発生しました。\n");
        ResetAfterInitializationFailure();
        return false;
    }
}

extern "C" __declspec(dllexport) COMMON_PLUGIN_TABLE* GetCommonPluginTable() noexcept {
    return &kPluginTable;
}

extern "C" __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE* host) noexcept {
    try {
        if (mojie::aviutl::RegisterPlugin(host)) {
            host->register_project_load_handler(HandleProjectLoadBoundary);
            host->register_project_save_handler(HandleProjectSaveBoundary);
        }
    } catch (...) {
        OutputDebugStringW(L"mojie: プラグイン登録に失敗しました。\n");
    }
}

extern "C" __declspec(dllexport) void UninitializePlugin() noexcept {
    try {
        mojie::aviutl::ClearCallbacks();
    } catch (...) {
    }
    try {
        mojie::ShutdownApplication();
    } catch (...) {
    }
    try {
        mojie::aviutl::UninitializePlugin();
    } catch (...) {
    }
}
