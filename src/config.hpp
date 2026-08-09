#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "replacement.hpp"

namespace mojie {

enum class SourceKind {
    Directory,
    File,
};

enum class ConfigOrigin {
    Global,
    Local,
};

struct ImageSource {
    SourceKind kind = SourceKind::Directory;
    std::filesystem::path path;
    bool recursive = false;
};

struct ImageSize {
    SizeMode mode = SizeMode::LineHeight;
    double value = 100.0;
};

struct ImagePadding {
    double x = 0.0;
    double y = 0.0;
};

struct RubySettings {
    double size_percent = 50.0;
};

struct ImageEntry {
    std::filesystem::path source_path;
    std::vector<std::wstring> match_texts;
    bool enabled = true;
    bool ruby_enabled = false;
    // Empty means that the matched source text supplies the ruby text.
    std::wstring ruby_text_override;

    // Runtime fields. They are rebuilt by ScanAndReconcile() and are not saved.
    bool present = false;
    std::wstring stable_id;
    std::wstring cache_stem;
    ConfigOrigin origin = ConfigOrigin::Global;
};

// A composition identifies images by their configuration origin and source
// path rather than by recognition-list position, which can change after a
// rescan. The path is persisted; no runtime cache identifier is serialized.
struct ImageReference {
    ConfigOrigin origin = ConfigOrigin::Global;
    std::filesystem::path source_path;
};

struct ImageComposition {
    std::wstring name;
    bool enabled = true;
    bool ruby_enabled = false;
    // Horizontal spacing, in pixels, inserted only between component images.
    double image_margin = 0.0;
    std::vector<std::wstring> match_texts;
    std::vector<ImageReference> images;

    // Runtime-only. The containing configuration determines this value.
    ConfigOrigin origin = ConfigOrigin::Global;
};

struct GlobalConfig {
    bool normalize_width = true;
    bool load_global = true;
    bool load_local = true;
    ImageSize default_size;
    ImagePadding default_padding;
    RubySettings default_ruby;
    std::vector<ImageSource> sources;
    std::vector<ImageEntry> images;
    // Paths explicitly removed from the recognition list. They remain
    // excluded when their source folder is scanned again.
    std::vector<std::filesystem::path> unregistered_images;
    std::vector<ImageComposition> compositions;
};

// Stored in mojie.json beside the AviUtl2 project. Relative paths in this
// configuration are resolved from the directory containing mojie.json.
struct LocalConfig {
    std::vector<ImageSource> sources;
    std::vector<ImageEntry> images;
    std::vector<std::filesystem::path> unregistered_images;
    std::vector<ImageComposition> compositions;
};

struct Diagnostic {
    std::filesystem::path path;
    std::wstring message;
};

struct ScanResult {
    std::size_t present_count = 0;
    std::size_t missing_count = 0;
    std::vector<Diagnostic> diagnostics;
};

struct CacheSyncResult {
    std::size_t copied_count = 0;
    std::size_t removed_count = 0;
    std::vector<Diagnostic> diagnostics;
    std::vector<ImageReference> skipped_images;
};

enum class CacheSyncFailureAction {
    RecordAndContinue,
    SkipImage,
    Stop,
};

using CacheSyncFailureHandler = std::function<CacheSyncFailureAction(
    const Diagnostic& diagnostic,
    const ImageEntry& image)>;

// The path comparison key is absolute, lexically normal and case-insensitive.
// It is also the input to StableImageId(), so IDs remain stable for a given
// Windows path regardless of slash or letter casing.
std::wstring NormalizedPathKey(
    const std::filesystem::path& path,
    const std::filesystem::path& base_directory = {});
std::wstring StableImageId(const std::wstring& normalized_path_key);

// Decoder availability for optional WIC formats such as WebP depends on the
// Windows installation. JPG/JPEG, GIF, TIFF and ICO use built-in codecs.
bool IsSupportedImageFile(const std::filesystem::path& path);

bool LoadGlobalConfig(
    const std::filesystem::path& file,
    GlobalConfig& config,
    std::wstring* error = nullptr);
bool SaveGlobalConfigAtomic(
    const std::filesystem::path& file,
    const GlobalConfig& config,
    std::wstring* error = nullptr);
bool LoadLocalConfig(
    const std::filesystem::path& file,
    LocalConfig& config,
    std::wstring* error = nullptr);
bool SaveLocalConfigAtomic(
    const std::filesystem::path& file,
    const LocalConfig& config,
    std::wstring* error = nullptr);

// Enabled global and local libraries are scanned independently, then joined in
// that order. Local images therefore have later replacement priority. General
// and default settings always come from the global configuration. If both load
// flags are false the returned library is empty (the plugin is disabled).
GlobalConfig MakeEffectiveConfig(
    const GlobalConfig& global,
    const LocalConfig& local,
    const std::filesystem::path& local_config_directory);

// Resolves against a reconciled/effective library. A disabled or missing
// image is not usable and therefore returns nullptr. Origin is part of the
// identity, even when two entries point to the same file.
const ImageEntry* ResolveImageReference(
    const GlobalConfig& effective,
    const ImageReference& reference);
bool IsCompositionResolvable(
    const GlobalConfig& effective,
    const ImageComposition& composition);

// Replaces the runtime recognition order with deterministic scan order while
// preserving edited image settings. Missing saved entries are retained at the
// end and marked present=false.
ScanResult ScanAndReconcile(
    GlobalConfig& config,
    const std::filesystem::path& relative_base = {});
ScanResult ScanAndReconcile(
    LocalConfig& config,
    const std::filesystem::path& relative_base = {});

// Removes only recognition entries that a preceding ScanAndReconcile() marked
// missing. Image compositions deliberately retain their image references.
std::size_t UnregisterMissingImages(GlobalConfig& config);
std::size_t UnregisterMissingImages(LocalConfig& config);

// Copies native Font images or converts supported raster images to managed PNG
// files under <app-data>/Font/mojie using generated stable names. Cache
// management is deliberately copy-only and never removes stale or unowned
// files.
CacheSyncResult SyncManagedCache(
    GlobalConfig& config,
    const std::filesystem::path& app_data_path,
    const CacheSyncFailureHandler& failure_handler = {});

// Maps every non-empty match text to its image index. Assignment is performed
// in recognition order, so a later image wins. Missing images are ignored.
std::unordered_map<std::wstring, std::size_t> BuildReplacementIndex(
    const GlobalConfig& config);

} // namespace mojie
