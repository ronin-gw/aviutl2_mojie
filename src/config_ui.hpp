#pragma once

#include <windows.h>

#include <filesystem>

#include "config.hpp"

namespace mojie {

// Opens a resource-less modal editor. On success, both configurations are
// replaced with their saved and scanned working copies. Cancel leaves them
// untouched. An empty local_config_file means that the project is unsaved and
// local configuration controls are unavailable.
bool ShowConfigDialog(
    HWND owner,
    HINSTANCE instance,
    GlobalConfig& global_config,
    LocalConfig& local_config,
    const std::filesystem::path& global_config_file,
    const std::filesystem::path& local_config_file,
    bool local_config_editable,
    const std::filesystem::path& app_data_path);

} // namespace mojie
