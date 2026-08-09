#include "config.hpp"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace {

#undef assert
#define assert(expression) \
    ((expression) ? static_cast<void>(0) : throw std::runtime_error("assertion failed: " #expression))

fs::path MakeTemporaryDirectory() {
    const fs::path path = fs::temp_directory_path() /
        (L"mojie_config_tests_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ignored;
    fs::remove_all(path, ignored);
    fs::create_directories(path);
    return path;
}

void CopyFixture(const fs::path& destination, const wchar_t* name) {
    fs::create_directories(destination.parent_path());
    fs::copy_file(
        fs::path(MOJIE_TESTIMAGE_DIR) / name,
        destination,
        fs::copy_options::overwrite_existing);
}

bool HasPngSignature(const fs::path& path) {
    std::array<unsigned char, 8> signature{};
    std::ifstream input(path, std::ios::binary);
    input.read(
        reinterpret_cast<char*>(signature.data()),
        static_cast<std::streamsize>(signature.size()));
    return input.gcount() == static_cast<std::streamsize>(signature.size()) &&
        signature == std::array<unsigned char, 8>{
            0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
}

void WriteBgraPng(
    const fs::path& path,
    UINT width,
    UINT height,
    const std::vector<unsigned char>& pixels) {
    using Microsoft::WRL::ComPtr;

    assert(width != 0 && height != 0);
    const UINT stride = width * 4;
    assert(pixels.size() == static_cast<std::size_t>(stride) * height);
    fs::create_directories(path.parent_path());

    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = apartment == S_OK || apartment == S_FALSE;
    if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE) {
        throw std::runtime_error("could not initialize COM");
    }

    HRESULT result = E_FAIL;
    {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> properties;
        WICPixelFormatGUID pixel_format = GUID_WICPixelFormat32bppBGRA;

        result = CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory));
        if (SUCCEEDED(result)) {
            result = factory->CreateStream(&stream);
        }
        if (SUCCEEDED(result)) {
            result = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
        }
        if (SUCCEEDED(result)) {
            result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        }
        if (SUCCEEDED(result)) {
            result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        }
        if (SUCCEEDED(result)) {
            result = encoder->CreateNewFrame(&frame, &properties);
        }
        if (SUCCEEDED(result)) {
            result = frame->Initialize(properties.Get());
        }
        if (SUCCEEDED(result)) {
            result = frame->SetSize(width, height);
        }
        if (SUCCEEDED(result)) {
            result = frame->SetPixelFormat(&pixel_format);
        }
        if (SUCCEEDED(result) && pixel_format == GUID_WICPixelFormat32bppBGRA) {
            result = frame->WritePixels(
                height, stride, static_cast<UINT>(pixels.size()),
                const_cast<unsigned char*>(pixels.data()));
        } else if (SUCCEEDED(result)) {
            result = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
        }
        if (SUCCEEDED(result)) {
            result = frame->Commit();
        }
        if (SUCCEEDED(result)) {
            result = encoder->Commit();
        }
    }
    if (uninitialize) {
        CoUninitialize();
    }
    if (FAILED(result)) {
        throw std::runtime_error("could not encode WIC PNG image");
    }
}

std::vector<unsigned char> PremultiplyBgra(
    std::vector<unsigned char> pixels) {
    assert(pixels.size() % 4 == 0);
    for (std::size_t offset = 0; offset < pixels.size(); offset += 4) {
        const unsigned alpha = pixels[offset + 3];
        for (std::size_t channel = 0; channel < 3; ++channel) {
            pixels[offset + channel] = static_cast<unsigned char>(
                (pixels[offset + channel] * alpha + 127U) / 255U);
        }
    }
    return pixels;
}

bool HasTransparency(const std::vector<unsigned char>& pixels) {
    assert(pixels.size() % 4 == 0);
    for (std::size_t offset = 3; offset < pixels.size(); offset += 4) {
        if (pixels[offset] != 255) {
            return true;
        }
    }
    return false;
}

std::vector<unsigned char> ReadWicFramePixels(
    const fs::path& path, UINT frame_index) {
    using Microsoft::WRL::ComPtr;

    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = apartment == S_OK || apartment == S_FALSE;
    if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE) {
        throw std::runtime_error("could not initialize COM");
    }

    std::vector<unsigned char> pixels;
    try {
        {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        UINT width = 0;
        UINT height = 0;

        HRESULT result = CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory));
        if (SUCCEEDED(result)) {
            result = factory->CreateDecoderFromFilename(
                path.c_str(), nullptr, GENERIC_READ,
                WICDecodeMetadataCacheOnLoad, &decoder);
        }
        if (SUCCEEDED(result)) {
            result = decoder->GetFrame(frame_index, &frame);
        }
        if (SUCCEEDED(result)) {
            result = frame->GetSize(&width, &height);
        }
        if (SUCCEEDED(result) && width != 0 && height != 0) {
            result = factory->CreateFormatConverter(&converter);
        }
        if (SUCCEEDED(result)) {
            result = converter->Initialize(
                frame.Get(), GUID_WICPixelFormat32bppBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0,
                WICBitmapPaletteTypeCustom);
        }
        const UINT stride = width * 4;
        pixels.resize(static_cast<std::size_t>(stride) * height);
        if (SUCCEEDED(result)) {
            result = converter->CopyPixels(
                nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data());
        }
        if (FAILED(result)) {
            throw std::runtime_error("could not decode WIC image frame");
        }
        }
        if (uninitialize) {
            CoUninitialize();
        }
        return pixels;
    } catch (...) {
        if (uninitialize) {
            CoUninitialize();
        }
        throw;
    }
}

} // namespace

int main() {
    const fs::path root = MakeTemporaryDirectory();
    try {
        const fs::path images = root / L"images";
        CopyFixture(images / L"hello.png", L"1m.png");
        CopyFixture(images / L"nested" / L"world.png", L"2m.png");

        mojie::GlobalConfig config;
        config.sources.push_back({mojie::SourceKind::Directory, images, false});
        auto scan = mojie::ScanAndReconcile(config);
        assert(scan.present_count == 1);
        assert(config.images.size() == 1);
        assert(config.images[0].match_texts == std::vector<std::wstring>{L"hello"});
        assert(!config.images[0].stable_id.empty());
        assert(config.images[0].cache_stem.rfind(L"mojie_", 0) == 0);

        config.images[0].match_texts = {L"こんにちは", L"hello"};
        config.images[0].enabled = false;
        config.images[0].ruby_enabled = true;
        config.images[0].ruby_text_override = L"はろー";
        config.default_size = {mojie::SizeMode::Percent, 125.5};
        config.default_padding = {-2.25, 3.5};
        config.default_ruby = {42.5};
        config.normalize_width = false;
        config.load_global = false;
        config.load_local = true;
        config.sources[0].recursive = true;
        scan = mojie::ScanAndReconcile(config);
        assert(scan.present_count == 2);
        assert(config.images[0].match_texts.front() == L"こんにちは");
        mojie::GlobalConfig outside_sources = config;
        outside_sources.sources.clear();
        const auto outside_sources_scan = mojie::ScanAndReconcile(outside_sources);
        assert(outside_sources_scan.present_count == 0);
        assert(outside_sources_scan.missing_count == 2);
        assert(std::all_of(
            outside_sources.images.begin(), outside_sources.images.end(),
            [](const mojie::ImageEntry& image) {
                return !image.present && fs::is_regular_file(image.source_path);
            }));
        mojie::GlobalConfig unregistered = config;
        unregistered.unregistered_images.push_back(images / L"nested" / L"world.png");
        auto unregistered_scan = mojie::ScanAndReconcile(unregistered);
        assert(unregistered_scan.present_count == 1);
        assert(unregistered.images.size() == 1);
        assert(unregistered.images[0].source_path.filename() == L"hello.png");
        mojie::ImageComposition global_composition;
        global_composition.name = L"長さ表示";
        global_composition.ruby_enabled = true;
        global_composition.image_margin = 3.5;
        global_composition.match_texts = {L"12m"};
        global_composition.images = {
            {mojie::ConfigOrigin::Global, config.images[0].source_path},
            {mojie::ConfigOrigin::Global, config.images[1].source_path},
        };
        config.compositions.push_back(global_composition);

        const fs::path config_file = root / L"data" / L"Plugin" / L"mojie" / L"config.json";
        std::wstring error;
        assert(mojie::SaveGlobalConfigAtomic(config_file, config, &error));
        mojie::GlobalConfig loaded;
        assert(mojie::LoadGlobalConfig(config_file, loaded, &error));
        assert(loaded.sources.size() == 1);
        assert(loaded.images.size() == 2);
        assert(!loaded.normalize_width);
        assert(!loaded.load_global);
        assert(loaded.load_local);
        assert(loaded.default_size.mode == mojie::SizeMode::Percent);
        assert(loaded.default_size.value == 125.5);
        assert(loaded.default_padding.x == -2.25);
        assert(loaded.default_padding.y == 3.5);
        assert(loaded.default_ruby.size_percent == 42.5);
        assert(!loaded.images[0].enabled);
        assert(loaded.images[0].ruby_enabled);
        assert(loaded.images[0].ruby_text_override == L"はろー");
        assert(loaded.images[1].enabled);
        assert(!loaded.images[1].ruby_enabled);
        assert(loaded.images[1].ruby_text_override.empty());
        assert(loaded.compositions.size() == 1);
        assert(loaded.compositions[0].name == L"長さ表示");
        assert(loaded.compositions[0].enabled);
        assert(loaded.compositions[0].ruby_enabled);
        assert(loaded.compositions[0].image_margin == 3.5);
        assert(loaded.compositions[0].match_texts == std::vector<std::wstring>{L"12m"});
        assert(loaded.compositions[0].origin == mojie::ConfigOrigin::Global);
        assert(loaded.compositions[0].images.size() == 2);
        assert(loaded.compositions[0].images[0].origin == mojie::ConfigOrigin::Global);

        const fs::path unregistered_file = root / L"unregistered-images.json";
        assert(mojie::SaveGlobalConfigAtomic(unregistered_file, unregistered, &error));
        mojie::GlobalConfig loaded_unregistered;
        assert(mojie::LoadGlobalConfig(unregistered_file, loaded_unregistered, &error));
        assert(loaded_unregistered.unregistered_images.size() == 1);
        unregistered_scan = mojie::ScanAndReconcile(loaded_unregistered);
        assert(unregistered_scan.present_count == 1);
        assert(loaded_unregistered.images.size() == 1);

        const fs::path default_image_fields_file = root / L"default-image-fields.json";
        {
            std::ofstream defaults(default_image_fields_file, std::ios::binary);
            defaults << R"json({
  "sources": [],
  "images": [{
    "sourcePath": "C:/default.png",
    "matchTexts": ["default"]
  }]
})json";
        }
        mojie::GlobalConfig default_image_fields;
        assert(mojie::LoadGlobalConfig(
            default_image_fields_file, default_image_fields, &error));
        assert(default_image_fields.images.size() == 1);
        assert(default_image_fields.images[0].enabled);
        assert(!default_image_fields.images[0].ruby_enabled);
        assert(default_image_fields.images[0].ruby_text_override.empty());
        assert(default_image_fields.load_global);
        assert(default_image_fields.load_local);
        assert(default_image_fields.compositions.empty());

        mojie::GlobalConfig invalid_global_composition = loaded;
        invalid_global_composition.compositions[0].images[0].origin =
            mojie::ConfigOrigin::Local;
        assert(!mojie::SaveGlobalConfigAtomic(
            root / L"invalid-global-composition.json",
            invalid_global_composition,
            &error));
        mojie::GlobalConfig invalid_composition_margin = loaded;
        invalid_composition_margin.compositions[0].image_margin = -1.0;
        assert(!mojie::SaveGlobalConfigAtomic(
            root / L"invalid-composition-margin.json",
            invalid_composition_margin,
            &error));

        mojie::GlobalConfig invalid_default_size = loaded;
        invalid_default_size.default_size.value = 10001.0;
        assert(!mojie::SaveGlobalConfigAtomic(
            root / L"invalid-default-size.json", invalid_default_size, &error));
        mojie::GlobalConfig invalid_default_padding = loaded;
        invalid_default_padding.default_padding.x = -10001.0;
        assert(!mojie::SaveGlobalConfigAtomic(
            root / L"invalid-default-padding.json", invalid_default_padding, &error));

        const fs::path invalid_defaults_file = root / L"invalid-defaults-load.json";
        {
            std::ofstream invalid(invalid_defaults_file, std::ios::binary);
            invalid << R"json({
  "defaultSize": {"mode": "percent", "value": 10001},
  "defaultPadding": {"x": 0, "y": 0},
  "sources": [],
  "images": []
})json";
        }
        mojie::GlobalConfig invalid_defaults;
        assert(!mojie::LoadGlobalConfig(
            invalid_defaults_file, invalid_defaults, &error));

        const fs::path invalid_global_reference_file =
            root / L"invalid-global-reference.json";
        {
            std::ofstream invalid(invalid_global_reference_file, std::ios::binary);
            invalid << R"json({
  "sources": [],
  "images": [],
  "compositions": [{
    "name": "invalid",
    "enabled": true,
    "matchTexts": ["invalid"],
    "images": [{"origin": "local", "sourcePath": "image.png"}]
  }]
})json";
        }
        mojie::GlobalConfig invalid_global_reference;
        assert(!mojie::LoadGlobalConfig(
            invalid_global_reference_file, invalid_global_reference, &error));

        const std::vector<std::wstring> unsafe_ruby_overrides = {
            L"bad<ruby", L"bad>ruby", L"bad\nruby", L"bad\x007f", L"bad\x0085",
            L"bad\x2028", L"bad\x2029",
        };
        for (const auto& unsafe : unsafe_ruby_overrides) {
            mojie::GlobalConfig invalid = loaded;
            invalid.images[0].ruby_text_override = unsafe;
            assert(!mojie::SaveGlobalConfigAtomic(
                root / L"unsafe-ruby-save.json", invalid, &error));
        }
        {
            mojie::GlobalConfig invalid = loaded;
            invalid.images[0].ruby_text_override = L"bad";
            invalid.images[0].ruby_text_override.push_back(L'\0');
            invalid.images[0].ruby_text_override += L"ruby";
            assert(!mojie::SaveGlobalConfigAtomic(
                root / L"unsafe-ruby-nul-save.json", invalid, &error));
        }

        const fs::path invalid_image_ruby_file = root / L"invalid-image-ruby.json";
        {
            std::ofstream invalid(invalid_image_ruby_file, std::ios::binary);
            invalid << R"json({
  "sources": [],
  "images": [{
    "sourcePath": "C:/invalid.png",
    "matchTexts": ["invalid"],
    "rubyTextOverride": "two\nlines"
  }]
})json";
        }
        mojie::GlobalConfig invalid_image_ruby;
        assert(!mojie::LoadGlobalConfig(
            invalid_image_ruby_file, invalid_image_ruby, &error));

        const fs::path invalid_ruby_file = root / L"invalid-ruby.json";
        {
            std::ofstream invalid(invalid_ruby_file, std::ios::binary);
            invalid << R"json({
  "normalizeWidth": true,
  "defaultSize": {"mode": "lineHeight", "value": 100},
  "defaultPadding": {"x": 0, "y": 0},
  "defaultRuby": {"enabled": true, "sizePercent": 0},
  "sources": [],
  "images": [{
    "sourcePath": "C:/invalid.png",
    "matchTexts": ["invalid"]
  }]
})json";
        }
        mojie::GlobalConfig rejected;
        assert(!mojie::LoadGlobalConfig(invalid_ruby_file, rejected, &error));

        mojie::ScanAndReconcile(loaded);
        loaded.images[0].match_texts.push_back(L"duplicate");
        loaded.images[1].match_texts.push_back(L"duplicate");
        const auto replacement_index = mojie::BuildReplacementIndex(loaded);
        assert(replacement_index.at(L"duplicate") == 1);
        loaded.images[1].enabled = false;
        assert(mojie::BuildReplacementIndex(loaded).count(L"duplicate") == 0);
        loaded.images[1].enabled = true;
        loaded.images[0].enabled = true;
        loaded.load_global = true;

        CopyFixture(root / L"project" / L"assets" / L"project.png", L"3m.png");
        mojie::LocalConfig local;
        local.sources.push_back(
            {mojie::SourceKind::Directory, fs::path(L"assets"), false});
        auto local_scan = mojie::ScanAndReconcile(local, root / L"project");
        assert(local_scan.present_count == 1);
        assert(local.images.size() == 1);
        assert(local.images[0].origin == mojie::ConfigOrigin::Local);
        assert(local.images[0].source_path == fs::path(L"assets") / L"project.png");
        local.images[0].match_texts = {L"local-priority"};
        local.images[0].ruby_text_override = L"ろーかる";
        local.images[0].ruby_enabled = true;
        mojie::ImageComposition local_composition;
        local_composition.name = L"混合";
        local_composition.ruby_enabled = true;
        local_composition.image_margin = 7.25;
        local_composition.match_texts = {L"mixed"};
        local_composition.images = {
            {mojie::ConfigOrigin::Global, loaded.images[0].source_path},
            {mojie::ConfigOrigin::Local, local.images[0].source_path},
        };
        local.compositions.push_back(local_composition);

        const fs::path local_file = root / L"project" / L"mojie.json";
        assert(mojie::SaveLocalConfigAtomic(local_file, local, &error));
        mojie::LocalConfig loaded_local;
        assert(mojie::LoadLocalConfig(local_file, loaded_local, &error));
        assert(loaded_local.sources.size() == 1);
        assert(loaded_local.sources[0].path == fs::path(L"assets"));
        assert(loaded_local.images.size() == 1);
        assert(loaded_local.images[0].source_path == fs::path(L"assets") / L"project.png");
        assert(loaded_local.images[0].ruby_text_override == L"ろーかる");
        assert(loaded_local.images[0].ruby_enabled);
        assert(loaded_local.compositions.size() == 1);
        assert(loaded_local.compositions[0].origin == mojie::ConfigOrigin::Local);
        assert(loaded_local.compositions[0].ruby_enabled);
        assert(loaded_local.compositions[0].image_margin == 7.25);
        assert(loaded_local.compositions[0].images.size() == 2);
        assert(loaded_local.compositions[0].images[0].origin ==
               mojie::ConfigOrigin::Global);
        assert(loaded_local.compositions[0].images[0].source_path ==
               loaded.images[0].source_path);
        assert(loaded_local.compositions[0].images[1].origin ==
               mojie::ConfigOrigin::Local);
        assert(loaded_local.compositions[0].images[1].source_path ==
               fs::path(L"assets") / L"project.png");

        mojie::LocalConfig local_unregistered = loaded_local;
        local_unregistered.unregistered_images.push_back(
            fs::path(L"assets") / L"project.png");
        const fs::path local_unregistered_file =
            root / L"project" / L"unregistered-mojie.json";
        assert(mojie::SaveLocalConfigAtomic(
            local_unregistered_file, local_unregistered, &error));
        mojie::LocalConfig loaded_local_unregistered;
        assert(mojie::LoadLocalConfig(
            local_unregistered_file, loaded_local_unregistered, &error));
        assert(loaded_local_unregistered.unregistered_images.size() == 1);
        local_scan = mojie::ScanAndReconcile(
            loaded_local_unregistered, root / L"project");
        assert(local_scan.present_count == 0);
        assert(loaded_local_unregistered.images.empty());
        assert(loaded_local_unregistered.compositions.size() == 1);

        mojie::LocalConfig absolute_local = loaded_local;
        absolute_local.sources[0].path = root / L"project" / L"assets";
        absolute_local.images[0].source_path =
            root / L"project" / L"assets" / L"project.png";
        absolute_local.compositions[0].images[1].source_path =
            root / L"project" / L"assets" / L"project.png";
        const fs::path portable_file = root / L"project" / L"portable.json";
        assert(mojie::SaveLocalConfigAtomic(portable_file, absolute_local, &error));
        mojie::LocalConfig portable_local;
        assert(mojie::LoadLocalConfig(portable_file, portable_local, &error));
        assert(portable_local.sources[0].path == fs::path(L"assets"));
        assert(portable_local.images[0].source_path ==
               fs::path(L"assets") / L"project.png");
        assert(portable_local.compositions[0].images[0].source_path ==
               loaded.images[0].source_path);
        assert(portable_local.compositions[0].images[1].source_path ==
               fs::path(L"assets") / L"project.png");

        loaded.images[0].match_texts.push_back(L"local-priority");
        const mojie::GlobalConfig effective = mojie::MakeEffectiveConfig(
            loaded, loaded_local, root / L"project");
        assert(effective.images.size() == 3);
        assert(effective.images.back().source_path.filename() == L"project.png");
        assert(effective.images.front().origin == mojie::ConfigOrigin::Global);
        assert(effective.images.back().origin == mojie::ConfigOrigin::Local);
        assert(effective.images.back().source_path.is_absolute());
        assert(mojie::BuildReplacementIndex(effective).at(L"local-priority") == 2);
        assert(effective.compositions.size() == 2);
        assert(effective.compositions.front().origin == mojie::ConfigOrigin::Global);
        assert(effective.compositions.back().origin == mojie::ConfigOrigin::Local);
        assert(effective.compositions.back().images[1].source_path.is_absolute());
        assert(mojie::IsCompositionResolvable(effective, effective.compositions.front()));
        assert(mojie::IsCompositionResolvable(effective, effective.compositions.back()));
        assert(mojie::ResolveImageReference(
                   effective, effective.compositions.back().images[0]) != nullptr);

        mojie::GlobalConfig local_only_settings = loaded;
        local_only_settings.load_global = false;
        const auto local_only = mojie::MakeEffectiveConfig(
            local_only_settings, loaded_local, root / L"project");
        assert(local_only.images.size() == 1);
        assert(local_only.images[0].origin == mojie::ConfigOrigin::Local);
        assert(local_only.compositions.size() == 1);
        assert(!mojie::IsCompositionResolvable(
            local_only, local_only.compositions[0]));

        mojie::GlobalConfig disabled_settings = loaded;
        disabled_settings.load_global = false;
        disabled_settings.load_local = false;
        const auto disabled = mojie::MakeEffectiveConfig(
            disabled_settings, loaded_local, root / L"project");
        assert(disabled.sources.empty());
        assert(disabled.images.empty());
        assert(disabled.compositions.empty());

        mojie::LocalConfig absent_local;
        assert(mojie::LoadLocalConfig(
            root / L"project" / L"absent-mojie.json", absent_local, &error));
        assert(absent_local.sources.empty());
        assert(absent_local.images.empty());

        loaded.images[0].enabled = false;
        auto cache = mojie::SyncManagedCache(loaded, root / L"data");
        assert(cache.diagnostics.empty());
        assert(cache.copied_count == 1);
        assert(cache.removed_count == 0);
        const fs::path cache_directory = root / L"data" / L"Font" / L"mojie";
        const fs::path hello_cache = cache_directory /
            (loaded.images[0].cache_stem + L".png");
        assert(!fs::exists(hello_cache));

        loaded.images[0].enabled = true;
        cache = mojie::SyncManagedCache(loaded, root / L"data");
        assert(cache.copied_count == 1);
        assert(cache.removed_count == 0);
        assert(fs::exists(hello_cache));
        const fs::path hello_source = loaded.images[0].source_path;
        const auto hello_source_pixels = ReadWicFramePixels(hello_source, 0);
        assert(HasTransparency(hello_source_pixels));
        assert(ReadWicFramePixels(hello_cache, 0) ==
               PremultiplyBgra(hello_source_pixels));
        assert(fs::exists(hello_cache.wstring() + L".source"));

        // Native BMP cache identity also includes file contents. An external
        // replacement can preserve both the size and timestamp.
        const fs::path native_images = root / L"native-images";
        const fs::path native_source = native_images / L"native.bmp";
        CopyFixture(native_source, L"formats/0b-bmp.bmp");
        mojie::GlobalConfig native_config;
        native_config.sources.push_back(
            {mojie::SourceKind::Directory, native_images, false});
        assert(mojie::ScanAndReconcile(native_config).present_count == 1);
        auto native_cache = mojie::SyncManagedCache(
            native_config, root / L"native-data");
        assert(native_cache.diagnostics.empty());
        assert(native_cache.copied_count == 1);
        const fs::path native_destination =
            root / L"native-data" / L"Font" / L"mojie" /
            (native_config.images[0].cache_stem + L".bmp");
        const auto native_time = fs::last_write_time(native_source);
        std::vector<char> changed_bytes;
        {
            std::ifstream input(native_source, std::ios::binary);
            changed_bytes.assign(
                std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>());
        }
        assert(!changed_bytes.empty());
        changed_bytes.back() ^= 1;
        {
            std::ofstream output(native_source, std::ios::binary | std::ios::trunc);
            output.write(changed_bytes.data(),
                         static_cast<std::streamsize>(changed_bytes.size()));
        }
        fs::last_write_time(native_source, native_time);
        native_cache = mojie::SyncManagedCache(native_config, root / L"native-data");
        assert(native_cache.diagnostics.empty());
        assert(native_cache.copied_count == 1);
        {
            std::ifstream source_after(native_source, std::ios::binary);
            std::ifstream cache_after(native_destination, std::ios::binary);
            assert(std::vector<char>(
                       std::istreambuf_iterator<char>(source_after),
                       std::istreambuf_iterator<char>()) ==
                   std::vector<char>(
                       std::istreambuf_iterator<char>(cache_after),
                       std::istreambuf_iterator<char>()));
        }

        loaded.images[0].enabled = false;
        cache = mojie::SyncManagedCache(loaded, root / L"data");
        assert(cache.copied_count == 0);
        assert(fs::exists(hello_cache));

        // Manual regeneration rebuilds every currently recognized image,
        // including disabled entries, and never accepts an existing cache as
        // fresh.
        {
            std::ofstream output(hello_cache, std::ios::binary | std::ios::trunc);
            output << "stale cache";
        }
        cache = mojie::RegenerateManagedCache(loaded, root / L"data");
        assert(cache.diagnostics.empty());
        assert(cache.copied_count == 2);
        assert(ReadWicFramePixels(hello_cache, 0) ==
               PremultiplyBgra(ReadWicFramePixels(hello_source, 0)));
        cache = mojie::RegenerateManagedCache(loaded, root / L"data");
        assert(cache.diagnostics.empty());
        assert(cache.copied_count == 2);

        // Cache synchronization is copy-only. A generated-looking file may
        // be user-owned or required by project data and must never be removed.
        CopyFixture(cache_directory / L"mojie_user_owned.png", L"4m.png");
        cache = mojie::SyncManagedCache(loaded, root / L"data");
        assert(cache.diagnostics.empty());
        assert(cache.removed_count == 0);
        assert(fs::exists(cache_directory / L"mojie_user_owned.png"));

        // PNG stores straight alpha. Transparent PNG cache pixels are written
        // with premultiplied color values for AviUtl2's emoji renderer.
        const fs::path alpha_images = root / L"alpha-images";
        const fs::path alpha_source = alpha_images / L"alpha.png";
        const std::vector<unsigned char> straight_alpha_pixels = {
            255, 255, 255,   0,
             80, 120, 200, 128,
             10,  20,  30, 255,
        };
        WriteBgraPng(alpha_source, 3, 1, straight_alpha_pixels);
        assert(ReadWicFramePixels(alpha_source, 0) == straight_alpha_pixels);
        mojie::GlobalConfig alpha_config;
        alpha_config.sources.push_back(
            {mojie::SourceKind::Directory, alpha_images, false});
        assert(mojie::ScanAndReconcile(alpha_config).present_count == 1);
        auto alpha_cache = mojie::SyncManagedCache(
            alpha_config, root / L"alpha-data");
        assert(alpha_cache.diagnostics.empty());
        assert(alpha_cache.copied_count == 1);
        const fs::path alpha_destination =
            root / L"alpha-data" / L"Font" / L"mojie" /
            (alpha_config.images[0].cache_stem + L".png");
        assert(ReadWicFramePixels(alpha_destination, 0) ==
               PremultiplyBgra(straight_alpha_pixels));
        assert(fs::exists(alpha_destination.wstring() + L".source"));
        alpha_cache = mojie::SyncManagedCache(alpha_config, root / L"alpha-data");
        assert(alpha_cache.diagnostics.empty());
        assert(alpha_cache.copied_count == 0);

        const fs::path opaque_images = root / L"opaque-images";
        const fs::path opaque_source = opaque_images / L"opaque.png";
        const std::vector<unsigned char> opaque_pixels = {
             10,  20,  30, 255,
            200, 120,  80, 255,
        };
        WriteBgraPng(opaque_source, 2, 1, opaque_pixels);
        mojie::GlobalConfig opaque_config;
        opaque_config.sources.push_back(
            {mojie::SourceKind::Directory, opaque_images, false});
        assert(mojie::ScanAndReconcile(opaque_config).present_count == 1);
        auto opaque_cache = mojie::SyncManagedCache(
            opaque_config, root / L"opaque-data");
        assert(opaque_cache.diagnostics.empty());
        assert(opaque_cache.copied_count == 1);
        const fs::path opaque_destination =
            root / L"opaque-data" / L"Font" / L"mojie" /
            (opaque_config.images[0].cache_stem + L".png");
        std::ifstream opaque_source_stream(opaque_source, std::ios::binary);
        std::ifstream opaque_cache_stream(opaque_destination, std::ios::binary);
        assert(std::vector<char>(
                   std::istreambuf_iterator<char>(opaque_source_stream),
                   std::istreambuf_iterator<char>()) ==
               std::vector<char>(
                   std::istreambuf_iterator<char>(opaque_cache_stream),
                   std::istreambuf_iterator<char>()));
        assert(!fs::exists(opaque_destination.wstring() + L".source"));

        const fs::path format_images = root / L"format-images";
        // These are real containers produced from testimage/0b.png. Optional
        // WIC codecs (DDS, JPEG XR, WebP, HEIF, and AVIF) are intentionally
        // not decoded here because CI machines need not have them installed.
        CopyFixture(format_images / L"photo.jpg", L"formats/0b-jpg.jpg");
        CopyFixture(format_images / L"image.tiff", L"formats/0b-tiff.tiff");
        CopyFixture(format_images / L"icon.ico", L"formats/0b-ico.ico");
        CopyFixture(format_images / L"animation.gif", L"formats/0b-gif.gif");
        mojie::GlobalConfig format_config;
        format_config.sources.push_back(
            {mojie::SourceKind::Directory, format_images, false});
        const auto format_scan = mojie::ScanAndReconcile(format_config);
        assert(format_scan.diagnostics.empty());
        assert(format_scan.present_count == 4);
        assert(mojie::IsSupportedImageFile(format_images / L"photo.JPG"));
        assert(mojie::IsSupportedImageFile(format_images / L"animation.gif"));
        for (const auto* extension : {
                 L".bmp", L".jpeg", L".jpe", L".jfif", L".tif",
                 L".dds", L".wdp", L".jxr", L".hdp", L".webp", L".heic",
                 L".heif", L".avif"}) {
            assert(mojie::IsSupportedImageFile(format_images / (L"image" + std::wstring(extension))));
        }
        assert(!mojie::IsSupportedImageFile(format_images / L"notes.txt"));

        const fs::path skip_images = root / L"skip-images";
        CopyFixture(skip_images / L"remaining.png", L"0b.png");
        const fs::path broken_dds = skip_images / L"broken.dds";
        {
            std::ofstream output(broken_dds, std::ios::binary);
            output << "not a DDS image";
        }
        mojie::GlobalConfig skip_config;
        skip_config.sources.push_back(
            {mojie::SourceKind::Directory, skip_images, false});
        const auto skip_scan = mojie::ScanAndReconcile(skip_config);
        assert(skip_scan.diagnostics.empty());
        assert(skip_scan.present_count == 2);
        bool conversion_failure_reported = false;
        const auto skip_cache = mojie::SyncManagedCache(
            skip_config, root / L"skip-data",
            [&](const mojie::Diagnostic& diagnostic, const mojie::ImageEntry& image) {
                conversion_failure_reported = true;
                assert(diagnostic.path == broken_dds);
                assert(image.source_path == broken_dds);
                return mojie::CacheSyncFailureAction::SkipImage;
            });
        assert(conversion_failure_reported);
        assert(skip_cache.diagnostics.empty());
        assert(skip_cache.skipped_images.size() == 1);
        assert(skip_cache.skipped_images[0].source_path == broken_dds);
        assert(skip_cache.copied_count == 1);

        auto format_cache = mojie::SyncManagedCache(format_config, root / L"format-data");
        assert(format_cache.diagnostics.empty());
        assert(format_cache.copied_count == 4);
        for (const auto& image : format_config.images) {
            const fs::path converted = root / L"format-data" / L"Font" / L"mojie" /
                (image.cache_stem + L".png");
            assert(fs::exists(converted));
            assert(HasPngSignature(converted));
            assert(fs::exists(converted.wstring() + L".source"));
            if (image.source_path.filename() == L"animation.gif") {
                const auto first_frame = ReadWicFramePixels(image.source_path, 0);
                const auto second_frame = ReadWicFramePixels(image.source_path, 1);
                assert(first_frame != second_frame);
                assert(ReadWicFramePixels(converted, 0) ==
                       PremultiplyBgra(first_frame));
            }
        }
        format_cache = mojie::SyncManagedCache(format_config, root / L"format-data");
        assert(format_cache.diagnostics.empty());
        assert(format_cache.copied_count == 0);

        fs::remove(images / L"hello.png");
        scan = mojie::ScanAndReconcile(loaded);
        assert(scan.present_count == 1);
        assert(scan.missing_count == 1);
        cache = mojie::RegenerateManagedCache(loaded, root / L"data");
        assert(cache.diagnostics.empty());
        assert(cache.copied_count == 1);
        assert(mojie::UnregisterMissingImages(loaded) == 1);
        assert(loaded.images.size() == 1);
        // Unregistering an image must not rewrite the user's composition.
        assert(loaded.compositions.size() == 1);
        assert(loaded.compositions[0].images.size() == 2);
        assert(!mojie::IsCompositionResolvable(loaded, loaded.compositions[0]));
        cache = mojie::SyncManagedCache(loaded, root / L"data");
        assert(cache.removed_count == 0);
        assert(fs::exists(hello_cache));

        mojie::LocalConfig unsafe_local = loaded_local;
        unsafe_local.images[0].ruby_text_override = L"bad\x2028ruby";
        assert(!mojie::SaveLocalConfigAtomic(
            root / L"project" / L"unsafe-mojie.json", unsafe_local, &error));

        const std::wstring lower = mojie::NormalizedPathKey(images / L"nested" / L"world.png");
        std::wstring upper = lower;
        for (auto& character : upper) {
            character = static_cast<wchar_t>(towupper(character));
        }
        assert(mojie::StableImageId(lower) ==
               mojie::StableImageId(mojie::NormalizedPathKey(upper)));

        std::error_code ignored;
        fs::remove_all(root, ignored);
        std::cout << "config tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::error_code ignored;
        fs::remove_all(root, ignored);
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
