#include "aviutl_plugin.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <utility>

#include <aviutl2_sdk/config2.h>
#include <aviutl2_sdk/filter2.h>
#include <aviutl2_sdk/module2.h>
#include <aviutl2_sdk/plugin2.h>

namespace mojie::aviutl {
namespace {

struct IntegrationState {
    std::mutex mutex;
    std::shared_ptr<const Callbacks> callbacks;
    std::wstring app_data_path;
    bool registered = false;
    bool active = true;
};

IntegrationState& State() {
    // Intentionally retained until process teardown. Rendering callbacks may be
    // finishing while AviUtl2 starts unloading plugins.
    static auto* state = new IntegrationState();
    return *state;
}

std::shared_ptr<const Callbacks> SnapshotCallbacks() {
    auto& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.active ? state.callbacks : nullptr;
}

void OpenSettings(HWND parent, HINSTANCE instance) noexcept {
    if (instance == nullptr) {
        MessageBoxW(
            parent,
            L"プラグインのモジュール情報を取得できないため、設定画面を開けません。",
            L"mojie",
            MB_OK | MB_ICONERROR);
        return;
    }

    try {
        const auto callbacks = SnapshotCallbacks();
        if (!callbacks || !callbacks->settings) {
            MessageBoxW(
                parent,
                L"設定画面はまだ利用できません。",
                L"mojie",
                MB_OK | MB_ICONINFORMATION);
            return;
        }
        callbacks->settings(parent, instance);
    } catch (...) {
        MessageBoxW(
            parent,
            L"設定画面を開けませんでした。",
            L"mojie",
            MB_OK | MB_ICONERROR);
    }
}

void OpenSettingsFromConfig(HWND parent, HINSTANCE instance) noexcept {
    OpenSettings(parent, instance);
}

bool ProcessNoOpVideo(FILTER_PROC_VIDEO*) noexcept {
    return true;
}

FILTER_ITEM_TEXT kBodyItem(L"本文", L"");
FILTER_ITEM_CHECK kImageOverride(L"画像設定を一括上書き", false);
FILTER_ITEM_SELECT::ITEM kSizeModeList[] = {
    {L"行の高さ", static_cast<int>(ImageSizeMode::LineHeight)},
    {L"割合", static_cast<int>(ImageSizeMode::Percent)},
    {L"ピクセル", static_cast<int>(ImageSizeMode::Pixels)},
    {nullptr, 0},
};
FILTER_ITEM_SELECT kSizeMode(
    L"サイズ方式", static_cast<int>(ImageSizeMode::LineHeight), kSizeModeList);
FILTER_ITEM_TRACK kSizeValue(L"サイズ", 100.0, 0.01, 10000.0, 0.01);
FILTER_ITEM_TRACK kPaddingX(L"横余白", 0.0, -10000.0, 10000.0, 0.01);
FILTER_ITEM_TRACK kPaddingY(L"縦余白", 0.0, -10000.0, 10000.0, 0.01);
void* kBodyItems[] = {
    &kBodyItem,
    &kImageOverride,
    &kSizeMode,
    &kSizeValue,
    &kPaddingX,
    &kPaddingY,
    nullptr,
};

FILTER_PLUGIN_TABLE kBodyFilter = {
    FILTER_PLUGIN_TABLE::FLAG_VIDEO,
    L"mojie本文",
    L"mojie",
    L"mojieの置換前テキストを保持するフィルタ効果",
    kBodyItems,
    ProcessNoOpVideo,
    nullptr,
    nullptr,
    nullptr,
};

void Render(SCRIPT_MODULE_PARAM* param) noexcept {
    try {
        if (param == nullptr) {
            return;
        }
        const int parameter_count = param->get_param_num();
        const char* value =
            parameter_count >= 1 ? param->get_param_string(0) : nullptr;
        const std::string_view input = value == nullptr ? std::string_view{} : value;
        const double base_size =
            parameter_count >= 2 ? param->get_param_double(1) : 34.0;
        ImageOverride image_override;
        if (parameter_count >= 3 &&
            param->get_param_type(2) == PARAM_TYPE::TABLE) {
            image_override.enabled =
                param->get_param_table_boolean(2, "enabled");
            const int size_mode = param->get_param_table_int(2, "sizeMode");
            if (size_mode >= static_cast<int>(ImageSizeMode::LineHeight) &&
                size_mode <= static_cast<int>(ImageSizeMode::Pixels)) {
                image_override.size_mode = static_cast<ImageSizeMode>(size_mode);
            }
            const double size_value =
                param->get_param_table_double(2, "sizeValue");
            if (std::isfinite(size_value) && size_value > 0.0) {
                image_override.size_value = size_value;
            }
            const double padding_x =
                param->get_param_table_double(2, "paddingX");
            const double padding_y =
                param->get_param_table_double(2, "paddingY");
            image_override.padding_x = std::isfinite(padding_x) ? padding_x : 0.0;
            image_override.padding_y = std::isfinite(padding_y) ? padding_y : 0.0;
        }
        const auto callbacks = SnapshotCallbacks();
        if (callbacks && callbacks->render) {
            const std::string output = callbacks->render(
                input, base_size, image_override);
            param->push_result_string(output.c_str());
        } else {
            const std::string unchanged(input);
            param->push_result_string(unchanged.c_str());
        }
    } catch (...) {
        if (param != nullptr) {
            param->set_error(u8"mojieのテキスト置換処理でエラーが発生しました");
        }
    }
}

SCRIPT_MODULE_FUNCTION kModuleFunctions[] = {
    {L"render", Render},
    {nullptr, nullptr},
};

SCRIPT_MODULE_TABLE kScriptModule = {
    L"mojie テキスト置換モジュール",
    kModuleFunctions,
};

} // namespace

void SetCallbacks(Callbacks callbacks) {
    auto& state = State();
    auto replacement = std::make_shared<const Callbacks>(std::move(callbacks));
    std::lock_guard<std::mutex> lock(state.mutex);
    state.callbacks = std::move(replacement);
    state.active = true;
}

void ClearCallbacks() {
    auto& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.callbacks.reset();
}

void InitializeConfig(CONFIG_HANDLE* config) {
    auto& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.app_data_path =
        config != nullptr && config->app_data_path != nullptr
            ? config->app_data_path
            : L"";
}

bool RegisterPlugin(HOST_APP_TABLE* host) {
    if (host == nullptr) {
        return false;
    }

    {
        auto& state = State();
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.registered) {
            return false;
        }
        state.registered = true;
        state.active = true;
    }

    host->register_filter_plugin(&kBodyFilter);
    host->register_script_module_name(&kScriptModule, L"mojie");
    host->register_config_menu(L"mojie", OpenSettingsFromConfig);
    return true;
}

void UninitializePlugin() {
    auto& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.active = false;
    state.callbacks.reset();
}

std::wstring GetAppDataPath() {
    auto& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.app_data_path;
}

} // namespace mojie::aviutl
