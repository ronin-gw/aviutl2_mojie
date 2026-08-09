#include "app_state.hpp"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <mutex>
#include <utility>

#include <aviutl2_sdk/plugin2.h>

#include "aviutl_plugin.hpp"
#include "config_ui.hpp"
#include "replacement.hpp"

namespace mojie {
namespace {

struct ApplicationState {
    std::mutex mutex;
    std::filesystem::path app_data_path;
    std::filesystem::path config_path;
    std::filesystem::path project_directory;
    std::filesystem::path local_config_path;
    GlobalConfig global;
    LocalConfig local;
    bool local_config_valid = true;
    bool local_config_exists = false;
    std::wstring local_config_error;
    std::shared_ptr<const ReplacementEngine> engine =
        std::make_shared<const ReplacementEngine>(std::vector<ReplacementRule>{});
    bool initialized = false;
};

ApplicationState& State() {
    static auto* state = new ApplicationState();
    return *state;
}

std::string ToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), length, nullptr, nullptr) != length) {
        return {};
    }
    return result;
}

std::shared_ptr<const ReplacementEngine> BuildEngine(const GlobalConfig& config) {
    std::vector<ReplacementRule> rules;
    int priority = 0;
    for (const ConfigOrigin origin : {ConfigOrigin::Global, ConfigOrigin::Local}) {
        // Within the same origin, compositions are appended after ordinary
        // images so an equal match selects the composition. The entire local
        // origin comes last and therefore still wins over every global rule.
        for (const auto& image : config.images) {
            if (image.origin != origin) {
                continue;
            }
            if (image.present && image.enabled && !image.cache_stem.empty()) {
                for (const auto& text : image.match_texts) {
                    ReplacementRule rule;
                    rule.match_text = ToUtf8(text);
                    rule.emoji_name = ToUtf8(image.cache_stem);
                    rule.ruby_enabled = image.ruby_enabled;
                    rule.ruby_text_override = ToUtf8(image.ruby_text_override);
                    rule.size_mode = config.default_size.mode;
                    rule.size_value = config.default_size.value;
                    rule.padding_x = config.default_padding.x;
                    rule.padding_y = config.default_padding.y;
                    rule.priority = priority;
                    if (!rule.match_text.empty() && !rule.emoji_name.empty()) {
                        rules.push_back(std::move(rule));
                    }
                }
            }
            ++priority;
        }
        for (const auto& composition : config.compositions) {
            if (composition.origin != origin) {
                continue;
            }
            std::vector<std::string> emoji_names;
            if (IsCompositionResolvable(config, composition)) {
                emoji_names.reserve(composition.images.size());
                for (const auto& reference : composition.images) {
                    const ImageEntry* image = ResolveImageReference(config, reference);
                    if (image == nullptr || image->cache_stem.empty()) {
                        emoji_names.clear();
                        break;
                    }
                    emoji_names.push_back(ToUtf8(image->cache_stem));
                }
            }
            if (!emoji_names.empty()) {
                for (const auto& text : composition.match_texts) {
                    ReplacementRule rule;
                    rule.match_text = ToUtf8(text);
                    rule.emoji_names = emoji_names;
                    rule.ruby_enabled = composition.ruby_enabled;
                    // A composition's display name is stable across all of its
                    // match-text aliases, so use it as the automatic ruby text.
                    // An explicit inline ruby still takes precedence in the
                    // replacement renderer.
                    rule.ruby_text_override = ToUtf8(composition.name);
                    rule.image_margin = composition.image_margin;
                    rule.size_mode = config.default_size.mode;
                    rule.size_value = config.default_size.value;
                    rule.padding_x = config.default_padding.x;
                    rule.padding_y = config.default_padding.y;
                    rule.priority = priority;
                    if (!rule.match_text.empty()) {
                        rules.push_back(std::move(rule));
                    }
                }
            }
            ++priority;
        }
    }
    return std::make_shared<const ReplacementEngine>(
        std::move(rules),
        config.normalize_width,
        AutomaticRubySettings{config.default_ruby.size_percent});
}

bool BuildEffectiveLocked(ApplicationState& state, std::wstring* error = nullptr) {
    GlobalConfig effective = MakeEffectiveConfig(
        state.global, state.local, state.project_directory);
    const CacheSyncResult cache = SyncManagedCache(effective, state.app_data_path);
    if (!cache.diagnostics.empty()) {
        if (error != nullptr) {
            *error = cache.diagnostics.front().message;
        }
        return false;
    }
    state.engine = BuildEngine(effective);
    return true;
}

LocalConfig ResolveLocalPathsForTransfer(
    LocalConfig local,
    const std::filesystem::path& old_directory) {
    if (old_directory.empty()) {
        return local;
    }
    const auto resolve = [&](std::filesystem::path& path) {
        if (path.is_relative()) {
            path = (old_directory / path).lexically_normal();
        }
    };
    for (auto& source : local.sources) {
        resolve(source.path);
    }
    for (auto& image : local.images) {
        resolve(image.source_path);
    }
    for (auto& composition : local.compositions) {
        for (auto& reference : composition.images) {
            if (reference.origin == ConfigOrigin::Local) {
                resolve(reference.source_path);
            }
        }
    }
    return local;
}

} // namespace

bool InitializeApplication(std::wstring* error) {
    auto& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);

    state.app_data_path = aviutl::GetAppDataPath();
    if (state.app_data_path.empty()) {
        if (error != nullptr) {
            *error = L"AviUtl2のデータフォルダーを取得できません。";
        }
        return false;
    }
    state.config_path = state.app_data_path / L"Plugin" / L"mojie" / L"config.json";
    if (!LoadGlobalConfig(state.config_path, state.global, error)) {
        return false;
    }

    if (!BuildEffectiveLocked(state, error)) {
        return false;
    }
    state.initialized = true;
    return true;
}

void ShutdownApplication() {
    auto& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.initialized = false;
    state.engine = std::make_shared<const ReplacementEngine>(
        std::vector<ReplacementRule>{});
}

std::string RenderText(
    std::string_view text,
    double base_size,
    const ReplacementStyleOverride& style_override) {
    std::shared_ptr<const ReplacementEngine> engine;
    {
        auto& state = State();
        std::lock_guard<std::mutex> lock(state.mutex);
        engine = state.engine;
    }
    const std::string decoded = DecodeAliasTextValue(text);
    return engine ? engine->render(decoded, base_size, style_override) : decoded;
}

GlobalConfig GetGlobalConfigCopy() {
    auto& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.global;
}

LocalConfig GetLocalConfigCopy() {
    auto& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.local;
}

ConfigurationSnapshot GetConfigurationSnapshot() {
    auto& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    return ConfigurationSnapshot{
        state.global,
        state.local,
        state.config_path,
        state.local_config_path,
    };
}

bool ApplyGlobalConfig(
    GlobalConfig config,
    bool* font_cache_changed,
    std::wstring* error) {
    auto& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.initialized) {
        if (error != nullptr) {
            *error = L"mojieの初期化が完了していません。";
        }
        return false;
    }

    ScanAndReconcile(config);
    GlobalConfig effective = MakeEffectiveConfig(
        config, state.local, state.project_directory);
    const CacheSyncResult cache = SyncManagedCache(effective, state.app_data_path);
    if (!cache.diagnostics.empty()) {
        if (error != nullptr) {
            *error = cache.diagnostics.front().message;
        }
        return false;
    }
    if (!SaveGlobalConfigAtomic(state.config_path, config, error)) {
        return false;
    }

    if (font_cache_changed != nullptr) {
        *font_cache_changed = cache.copied_count != 0 || cache.removed_count != 0;
    }
    state.global = std::move(config);
    state.engine = BuildEngine(effective);
    return true;
}

void RefreshProjectContext(PROJECT_FILE* project) {
    if (project == nullptr) {
        return;
    }

    std::filesystem::path directory;
    std::filesystem::path local_config_path;
    const wchar_t* project_path = project->get_project_file_path();
    if (project_path != nullptr && *project_path != L'\0') {
        directory = std::filesystem::path(project_path).parent_path();
        local_config_path = directory / L"mojie.json";
    }

    LocalConfig loaded;
    std::wstring load_error;
    bool loaded_ok = true;
    bool file_exists = false;
    if (!local_config_path.empty()) {
        std::error_code exists_error;
        file_exists = std::filesystem::exists(local_config_path, exists_error);
        loaded_ok = LoadLocalConfig(local_config_path, loaded, &load_error);
    }

    auto& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.project_directory = std::move(directory);
    state.local_config_path = std::move(local_config_path);
    state.local = loaded_ok ? std::move(loaded) : LocalConfig{};
    state.local_config_valid = loaded_ok;
    state.local_config_exists = file_exists;
    state.local_config_error = loaded_ok ? std::wstring{} : load_error;
    if (state.initialized) {
        if (!BuildEffectiveLocked(state)) {
            state.engine = std::make_shared<const ReplacementEngine>(
                std::vector<ReplacementRule>{});
        }
    }

    if (!loaded_ok) {
        OutputDebugStringW(L"mojie: ローカル設定を読み込めませんでした: ");
        OutputDebugStringW(load_error.c_str());
        OutputDebugStringW(L"\n");
    }
}

void HandleProjectLoad(PROJECT_FILE* project) {
    RefreshProjectContext(project);
}

void HandleProjectSave(PROJECT_FILE* project) {
    if (project == nullptr) {
        return;
    }

    std::filesystem::path directory;
    std::filesystem::path local_config_path;
    const wchar_t* project_path = project->get_project_file_path();
    if (project_path != nullptr && *project_path != L'\0') {
        directory = std::filesystem::path(project_path).parent_path();
        local_config_path = directory / L"mojie.json";
    }

    LocalConfig local;
    bool old_valid = true;
    bool old_exists = false;
    std::wstring old_error;
    std::filesystem::path old_local_config_path;
    {
        auto& state = State();
        std::lock_guard<std::mutex> lock(state.mutex);
        local = state.local;
        old_valid = state.local_config_valid;
        old_exists = state.local_config_exists;
        old_error = state.local_config_error;
        old_local_config_path = state.local_config_path;
    }

    const bool same_path =
        local_config_path == old_local_config_path ||
        (!local_config_path.empty() && !old_local_config_path.empty() &&
         NormalizedPathKey(local_config_path) ==
             NormalizedPathKey(old_local_config_path));
    if (same_path) {
        // A normal project save must never write the sidecar. Reloading here
        // makes external edits visible without risking that the in-memory copy
        // overwrites them.
        RefreshProjectContext(project);
        return;
    }

    LocalConfig next_local;
    bool next_valid = true;
    bool destination_exists = false;
    std::wstring transition_error;
    if (!local_config_path.empty()) {
        std::error_code exists_error;
        destination_exists = std::filesystem::exists(local_config_path, exists_error);
        if (exists_error) {
            next_valid = false;
            transition_error = L"ローカル設定ファイルの有無を確認できません。";
        }
    }

    if (next_valid && !local_config_path.empty() && destination_exists) {
        // The destination sidecar belongs to the Save As target and always
        // wins over the old project's in-memory settings.
        next_valid = LoadLocalConfig(
            local_config_path, next_local, &transition_error);
    } else if (next_valid && !local_config_path.empty()) {
        const bool has_content =
            !local.sources.empty() || !local.images.empty() ||
            !local.compositions.empty();
        if (old_valid && (old_exists || has_content)) {
            LocalConfig transferred = ResolveLocalPathsForTransfer(
                local, old_local_config_path.parent_path());
            if (SaveLocalConfigAtomic(
                    local_config_path, transferred, &transition_error)) {
                next_local = std::move(transferred);
                destination_exists = true;
            } else {
                next_valid = false;
            }
        } else if (!old_valid) {
            next_valid = false;
            transition_error = old_error.empty()
                ? L"旧プロジェクトのローカル設定が無効なため引き継げません。"
                : old_error;
        }
    }

    auto& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.project_directory = std::move(directory);
    state.local_config_path = std::move(local_config_path);
    // Never interpret the old local snapshot relative to the new directory
    // after a transition failure.
    state.local = next_valid ? std::move(next_local) : LocalConfig{};
    state.local_config_valid = next_valid;
    state.local_config_exists = destination_exists;
    state.local_config_error = next_valid ? std::wstring{} : transition_error;
    if (state.initialized && !BuildEffectiveLocked(state)) {
        state.engine = std::make_shared<const ReplacementEngine>(
            std::vector<ReplacementRule>{});
    }

    if (!next_valid) {
        OutputDebugStringW(L"mojie: ローカル設定を切り替えられませんでした: ");
        OutputDebugStringW(transition_error.c_str());
        OutputDebugStringW(L"\n");
    }
}

void ShowSettings(HWND parent, HINSTANCE instance) {
    // A user may have repaired mojie.json with an external editor after a
    // failed project load. Retry before taking the dialog snapshot.
    std::filesystem::path retry_local_path;
    {
        auto& state = State();
        std::lock_guard<std::mutex> lock(state.mutex);
        if (!state.local_config_valid && !state.local_config_path.empty()) {
            retry_local_path = state.local_config_path;
        }
    }
    if (!retry_local_path.empty()) {
        LocalConfig repaired;
        std::wstring repair_error;
        const bool repaired_ok = LoadLocalConfig(
            retry_local_path, repaired, &repair_error);
        auto& state = State();
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.local_config_path == retry_local_path) {
            state.local = repaired_ok ? std::move(repaired) : LocalConfig{};
            state.local_config_valid = repaired_ok;
            state.local_config_error = repaired_ok ? std::wstring{} : repair_error;
        }
    }

    GlobalConfig working;
    LocalConfig local_working;
    std::filesystem::path config_path;
    std::filesystem::path local_config_path;
    std::filesystem::path app_data_path;
    bool local_config_valid = true;
    std::wstring local_config_error;
    {
        auto& state = State();
        std::lock_guard<std::mutex> lock(state.mutex);
        working = state.global;
        local_working = state.local;
        config_path = state.config_path;
        local_config_path = state.local_config_path;
        app_data_path = state.app_data_path;
        local_config_valid = state.local_config_valid;
        local_config_error = state.local_config_error;
    }

    if (!local_config_valid && !local_config_path.empty()) {
        MessageBoxW(
            parent,
            (L"ローカル設定ファイルを読み込めません。\n"
             L"ローカル設定の編集と保存を無効にして設定画面を開きます。\n\n" +
             local_config_path.wstring() + L"\n\n" + local_config_error).c_str(),
            L"mojie", MB_OK | MB_ICONWARNING);
    }

    if (!ShowConfigDialog(
            parent, instance, working, local_working,
            config_path, local_config_path, local_config_valid, app_data_path)) {
        return;
    }

    auto& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.global = std::move(working);
    if (local_config_valid) {
        state.local = std::move(local_working);
        state.local_config_valid = true;
        if (!state.local_config_path.empty()) {
            std::error_code exists_error;
            state.local_config_exists =
                std::filesystem::exists(state.local_config_path, exists_error);
        } else {
            state.local_config_exists = false;
        }
        state.local_config_error.clear();
    }
    std::wstring error;
    if (!BuildEffectiveLocked(state, &error)) {
        state.engine = std::make_shared<const ReplacementEngine>(
            std::vector<ReplacementRule>{});
        MessageBoxW(
            parent,
            (L"設定は保存しましたが、画像を同期できませんでした。\n\n" + error).c_str(),
            L"mojie", MB_OK | MB_ICONERROR);
    }
}

} // namespace mojie
