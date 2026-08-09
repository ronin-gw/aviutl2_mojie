#pragma once

#include <string>
#include <string_view>
#include <filesystem>

#include <windows.h>

#include "config.hpp"

struct PROJECT_FILE;

namespace mojie {

bool InitializeApplication(std::wstring* error = nullptr);
void ShutdownApplication();

std::string RenderText(
    std::string_view text,
    double base_size,
    const ReplacementStyleOverride& style_override = {});

GlobalConfig GetGlobalConfigCopy();
LocalConfig GetLocalConfigCopy();

struct ConfigurationSnapshot {
    GlobalConfig global;
    LocalConfig local;
    std::filesystem::path global_config_path;
    // Empty until the project has been saved to an .aup2 file.
    std::filesystem::path local_config_path;
};

ConfigurationSnapshot GetConfigurationSnapshot();
bool ApplyGlobalConfig(GlobalConfig config, bool* font_cache_changed, std::wstring* error = nullptr);

// Updates the local configuration context from the project file path. Local
// settings live beside the .aup2 file as mojie.json and are never embedded in
// the project itself.
void HandleProjectLoad(PROJECT_FILE* project);
void HandleProjectSave(PROJECT_FILE* project);

void ShowSettings(HWND parent, HINSTANCE instance);

} // namespace mojie
