#include "config.hpp"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace mojie {
namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

fs::path UniqueSiblingPath(const fs::path& file, const wchar_t* purpose) {
    static std::atomic<std::uint64_t> sequence{0};
    std::wstring name = file.filename().wstring();
    name += L".";
    name += purpose;
    name += L"." + std::to_wstring(GetCurrentProcessId());
    name += L"." + std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed));
    return file.parent_path() / name;
}

bool FilesHaveSameContents(const fs::path& left_path, const fs::path& right_path) {
    std::ifstream left(left_path, std::ios::binary);
    std::ifstream right(right_path, std::ios::binary);
    if (!left || !right) {
        return false;
    }

    std::array<char, 64 * 1024> left_buffer{};
    std::array<char, 64 * 1024> right_buffer{};
    for (;;) {
        left.read(left_buffer.data(), static_cast<std::streamsize>(left_buffer.size()));
        right.read(right_buffer.data(), static_cast<std::streamsize>(right_buffer.size()));
        const std::streamsize left_count = left.gcount();
        const std::streamsize right_count = right.gcount();
        if (left_count != right_count) {
            return false;
        }
        if (left_count == 0) {
            return left.eof() && right.eof();
        }
        if (!std::equal(
                left_buffer.begin(), left_buffer.begin() + left_count,
                right_buffer.begin())) {
            return false;
        }
    }
}

std::wstring LowerExtension(const fs::path& path) {
    std::wstring extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return extension;
}

struct DecodedBgraFrame {
    UINT width = 0;
    UINT height = 0;
    UINT stride = 0;
    std::vector<BYTE> pixels;
};

bool DecodeFirstFrameToBgra(
    const fs::path& source,
    DecodedBgraFrame& decoded) {
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = apartment == S_OK || apartment == S_FALSE;
    if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE) {
        return false;
    }

    bool succeeded = false;
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
                source.c_str(), nullptr, GENERIC_READ,
                WICDecodeMetadataCacheOnLoad, &decoder);
        }
        if (SUCCEEDED(result)) {
            result = decoder->GetFrame(0, &frame);
        }
        if (SUCCEEDED(result)) {
            result = frame->GetSize(&width, &height);
        }
        if (SUCCEEDED(result) && width != 0 && height != 0 &&
            width <= std::numeric_limits<UINT>::max() / 4) {
            result = factory->CreateFormatConverter(&converter);
        } else if (SUCCEEDED(result)) {
            result = E_INVALIDARG;
        }
        if (SUCCEEDED(result)) {
            result = converter->Initialize(
                frame.Get(), GUID_WICPixelFormat32bppBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0,
                WICBitmapPaletteTypeCustom);
        }
        const UINT stride = width * 4;
        if (SUCCEEDED(result) &&
            height <= std::numeric_limits<UINT>::max() / stride) {
            const UINT buffer_size = stride * height;
            decoded.pixels.resize(buffer_size);
            result = converter->CopyPixels(
                nullptr, stride, buffer_size, decoded.pixels.data());
            if (SUCCEEDED(result)) {
                decoded.width = width;
                decoded.height = height;
                decoded.stride = stride;
            }
        } else if (SUCCEEDED(result)) {
            result = E_OUTOFMEMORY;
        }
        succeeded = SUCCEEDED(result);
    }

    if (uninitialize) {
        CoUninitialize();
    }
    return succeeded;
}

std::optional<bool> PngHasTransparency(const fs::path& source) {
    DecodedBgraFrame decoded;
    if (!DecodeFirstFrameToBgra(source, decoded)) {
        return std::nullopt;
    }
    for (std::size_t offset = 3; offset < decoded.pixels.size(); offset += 4) {
        if (decoded.pixels[offset] != 255) {
            return true;
        }
    }
    return false;
}

bool ShouldCopyNativeFontImage(const fs::path& path) {
    const std::wstring extension = LowerExtension(path);
    if (extension == L".bmp") {
        return true;
    }
    if (extension != L".png") {
        return false;
    }
    // Preserve the previous copy behavior if WIC cannot inspect a PNG. A
    // decodable PNG with transparent pixels must be rewritten because the
    // AviUtl2 emoji path consumes premultiplied-alpha color values.
    return !PngHasTransparency(path).value_or(false);
}

std::optional<std::uint64_t> HashFileContents(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::uint64_t hash = UINT64_C(14695981039346656037);
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        for (std::streamsize index = 0; index < count; ++index) {
            hash ^= static_cast<unsigned char>(buffer[static_cast<std::size_t>(index)]);
            hash *= UINT64_C(1099511628211);
        }
    }
    if (!input.eof()) {
        return std::nullopt;
    }
    return hash;
}

std::string HexHash(std::uint64_t hash) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

bool FingerprintMatches(
    const fs::path& source,
    const fs::path& destination,
    const fs::path& fingerprint_file) {
    const auto source_hash = HashFileContents(source);
    const auto destination_hash = HashFileContents(destination);
    if (!source_hash || !destination_hash) {
        return false;
    }
    std::ifstream input(fingerprint_file, std::ios::binary);
    std::string saved_source;
    std::string saved_destination;
    return input && (input >> saved_source >> saved_destination) &&
        saved_source == HexHash(*source_hash) &&
        saved_destination == HexHash(*destination_hash);
}

bool SaveFingerprintAtomic(
    const fs::path& source,
    const fs::path& destination,
    const fs::path& fingerprint_file) {
    const auto source_hash = HashFileContents(source);
    const auto destination_hash = HashFileContents(destination);
    if (!source_hash || !destination_hash) {
        return false;
    }
    const fs::path temporary = UniqueSiblingPath(fingerprint_file, L"tmp");
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            return false;
        }
        output << HexHash(*source_hash) << ' ' << HexHash(*destination_hash) << '\n';
        output.flush();
        if (!output) {
            std::error_code ignored;
            fs::remove(temporary, ignored);
            return false;
        }
    }
    if (MoveFileExW(
            temporary.c_str(), fingerprint_file.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    std::error_code ignored;
    fs::remove(temporary, ignored);
    return false;
}

bool ConvertFirstFrameToPng(const fs::path& source, const fs::path& destination) {
    DecodedBgraFrame decoded;
    if (!DecodeFirstFrameToBgra(source, decoded)) {
        return false;
    }
    for (std::size_t offset = 0; offset < decoded.pixels.size(); offset += 4) {
        const unsigned alpha = decoded.pixels[offset + 3];
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const unsigned color = decoded.pixels[offset + channel];
            decoded.pixels[offset + channel] = static_cast<BYTE>(
                (color * alpha + 127U) / 255U);
        }
    }

    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = apartment == S_OK || apartment == S_FALSE;
    if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE) {
        return false;
    }

    bool succeeded = false;
    {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> output_frame;
        ComPtr<IPropertyBag2> properties;
        WICPixelFormatGUID pixel_format = GUID_WICPixelFormat32bppBGRA;

        HRESULT result = CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory));
        if (SUCCEEDED(result)) {
            result = factory->CreateStream(&stream);
        }
        if (SUCCEEDED(result)) {
            result = stream->InitializeFromFilename(destination.c_str(), GENERIC_WRITE);
        }
        if (SUCCEEDED(result)) {
            result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        }
        if (SUCCEEDED(result)) {
            result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        }
        if (SUCCEEDED(result)) {
            result = encoder->CreateNewFrame(&output_frame, &properties);
        }
        if (SUCCEEDED(result)) {
            result = output_frame->Initialize(properties.Get());
        }
        if (SUCCEEDED(result)) {
            result = output_frame->SetSize(decoded.width, decoded.height);
        }
        if (SUCCEEDED(result)) {
            result = output_frame->SetPixelFormat(&pixel_format);
        }
        if (SUCCEEDED(result) && pixel_format == GUID_WICPixelFormat32bppBGRA) {
            result = output_frame->WritePixels(
                decoded.height, decoded.stride,
                static_cast<UINT>(decoded.pixels.size()),
                decoded.pixels.data());
        } else if (SUCCEEDED(result)) {
            result = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
        }
        if (SUCCEEDED(result)) {
            result = output_frame->Commit();
        }
        if (SUCCEEDED(result)) {
            result = encoder->Commit();
        }
        succeeded = SUCCEEDED(result);
    }

    if (uninitialize) {
        CoUninitialize();
    }
    return succeeded;
}

fs::path PortableLocalPath(const fs::path& path, const fs::path& base_directory) {
    if (path.empty() || path.is_relative() || base_directory.empty()) {
        return path.lexically_normal();
    }
    std::error_code error;
    fs::path relative = fs::relative(path, base_directory, error);
    if (error || relative.empty() || relative.is_absolute()) {
        return path;
    }
    const auto first = relative.begin();
    if (first != relative.end() && *first == L"..") {
        return path;
    }
    return relative.lexically_normal();
}

std::string ToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        throw std::runtime_error("UTF-16 to UTF-8 conversion failed");
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), size, nullptr, nullptr) != size) {
        throw std::runtime_error("UTF-16 to UTF-8 conversion failed");
    }
    return result;
}

std::wstring FromUtf8(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0);
    if (size <= 0) {
        throw std::runtime_error("UTF-8 to UTF-16 conversion failed");
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), size) != size) {
        throw std::runtime_error("UTF-8 to UTF-16 conversion failed");
    }
    return result;
}

std::string PathToUtf8(const fs::path& path) {
    return ToUtf8(path.generic_wstring());
}

fs::path PathFromUtf8(const std::string& path) {
    return fs::path(FromUtf8(path));
}

std::wstring ErrorText(const std::exception& exception) {
    try {
        return FromUtf8(exception.what());
    } catch (...) {
        return L"設定データの処理に失敗しました。";
    }
}

const char* SourceKindName(SourceKind kind) {
    return kind == SourceKind::File ? "file" : "directory";
}

const char* ConfigOriginName(ConfigOrigin origin) {
    return origin == ConfigOrigin::Local ? "local" : "global";
}

ConfigOrigin ParseConfigOrigin(const json& value) {
    const std::string name = value.get<std::string>();
    if (name == "global") {
        return ConfigOrigin::Global;
    }
    if (name == "local") {
        return ConfigOrigin::Local;
    }
    throw std::runtime_error("unknown configuration origin");
}

SourceKind ParseSourceKind(const json& value) {
    const std::string name = value.get<std::string>();
    if (name == "file") {
        return SourceKind::File;
    }
    if (name == "directory") {
        return SourceKind::Directory;
    }
    throw std::runtime_error("unknown image source kind");
}

const char* SizeModeName(SizeMode mode) {
    switch (mode) {
    case SizeMode::LineHeight:
        return "lineHeight";
    case SizeMode::Percent:
        return "percent";
    case SizeMode::Pixels:
        return "pixels";
    }
    return "lineHeight";
}

SizeMode ParseSizeMode(const json& value) {
    const std::string name = value.get<std::string>();
    if (name == "lineHeight") {
        return SizeMode::LineHeight;
    }
    if (name == "percent") {
        return SizeMode::Percent;
    }
    if (name == "pixels") {
        return SizeMode::Pixels;
    }
    throw std::runtime_error("unknown image size mode");
}

json SourceToJson(const ImageSource& source) {
    return json{
        {"kind", SourceKindName(source.kind)},
        {"path", PathToUtf8(source.path)},
        {"recursive", source.recursive},
    };
}

ImageSource SourceFromJson(const json& value) {
    ImageSource source;
    source.kind = ParseSourceKind(value.at("kind"));
    source.path = PathFromUtf8(value.at("path").get<std::string>());
    source.recursive = value.value("recursive", false);
    if (source.kind == SourceKind::File) {
        source.recursive = false;
    }
    return source;
}

json SizeToJson(const ImageSize& size) {
    if (!std::isfinite(size.value) || size.value <= 0.0 || size.value > 10000.0) {
        throw std::runtime_error("image size is outside the supported range");
    }
    return json{{"mode", SizeModeName(size.mode)}, {"value", size.value}};
}

ImageSize SizeFromJson(const json& value) {
    ImageSize size;
    size.mode = ParseSizeMode(value.at("mode"));
    size.value = value.value("value", 100.0);
    if (!std::isfinite(size.value) || size.value <= 0.0 || size.value > 10000.0) {
        throw std::runtime_error("image size is outside the supported range");
    }
    return size;
}

json PaddingToJson(const ImagePadding& padding) {
    if (!std::isfinite(padding.x) || !std::isfinite(padding.y) ||
        padding.x < -10000.0 || padding.x > 10000.0 ||
        padding.y < -10000.0 || padding.y > 10000.0) {
        throw std::runtime_error("image padding is outside the supported range");
    }
    return json{{"x", padding.x}, {"y", padding.y}};
}

ImagePadding PaddingFromJson(const json& value) {
    ImagePadding padding;
    padding.x = value.value("x", 0.0);
    padding.y = value.value("y", 0.0);
    if (!std::isfinite(padding.x) || !std::isfinite(padding.y) ||
        padding.x < -10000.0 || padding.x > 10000.0 ||
        padding.y < -10000.0 || padding.y > 10000.0) {
        throw std::runtime_error("image padding is outside the supported range");
    }
    return padding;
}

void ValidateRuby(const RubySettings& ruby) {
    if (!std::isfinite(ruby.size_percent) || ruby.size_percent <= 0.0 ||
        ruby.size_percent > 10000.0) {
        throw std::runtime_error("ruby size percent is outside the supported range");
    }
}

void ValidateRubyTextOverride(const std::wstring& text) {
    if (text.empty()) {
        return;
    }
    for (const wchar_t character : text) {
        if (character == L'<' || character == L'>' ||
            character <= 0x1f || character == 0x7f ||
            character == 0x0085 || character == 0x2028 || character == 0x2029) {
            throw std::runtime_error("ruby text override contains an unsafe character");
        }
    }
}

json RubyToJson(const RubySettings& ruby) {
    ValidateRuby(ruby);
    return json{
        {"sizePercent", ruby.size_percent},
    };
}

RubySettings RubyFromJson(const json& value) {
    RubySettings ruby;
    ruby.size_percent = value.value("sizePercent", 50.0);
    ValidateRuby(ruby);
    return ruby;
}

std::vector<std::wstring> MatchTextsFromJson(const json& value) {
    std::vector<std::wstring> result;
    for (const auto& item : value) {
        result.push_back(FromUtf8(item.get<std::string>()));
    }
    return result;
}

json MatchTextsToJson(const std::vector<std::wstring>& values) {
    json result = json::array();
    for (const auto& value : values) {
        result.push_back(ToUtf8(value));
    }
    return result;
}

json PathsToJson(const std::vector<fs::path>& paths) {
    json result = json::array();
    for (const auto& path : paths) {
        result.push_back(PathToUtf8(path));
    }
    return result;
}

std::vector<fs::path> PathsFromJson(const json& value) {
    std::vector<fs::path> result;
    for (const auto& item : value) {
        const fs::path path = PathFromUtf8(item.get<std::string>());
        if (!path.empty()) {
            result.push_back(path);
        }
    }
    return result;
}

json ImageToJson(const ImageEntry& image) {
    ValidateRubyTextOverride(image.ruby_text_override);
    return json{
        {"sourcePath", PathToUtf8(image.source_path)},
        {"matchTexts", MatchTextsToJson(image.match_texts)},
        {"enabled", image.enabled},
        {"rubyEnabled", image.ruby_enabled},
        {"rubyTextOverride", ToUtf8(image.ruby_text_override)},
    };
}

ImageEntry ImageFromJson(const json& value) {
    ImageEntry image;
    image.source_path = PathFromUtf8(value.at("sourcePath").get<std::string>());
    image.match_texts = MatchTextsFromJson(value.value("matchTexts", json::array()));
    image.enabled = value.value("enabled", true);
    image.ruby_enabled = value.value("rubyEnabled", false);
    image.ruby_text_override = FromUtf8(value.value("rubyTextOverride", std::string{}));
    ValidateRubyTextOverride(image.ruby_text_override);
    return image;
}

json ImageReferenceToJson(const ImageReference& reference) {
    if (reference.source_path.empty()) {
        throw std::runtime_error("composition image reference has an empty path");
    }
    return json{
        {"origin", ConfigOriginName(reference.origin)},
        {"sourcePath", PathToUtf8(reference.source_path)},
    };
}

ImageReference ImageReferenceFromJson(const json& value) {
    ImageReference reference;
    reference.origin = ParseConfigOrigin(value.at("origin"));
    reference.source_path = PathFromUtf8(value.at("sourcePath").get<std::string>());
    if (reference.source_path.empty()) {
        throw std::runtime_error("composition image reference has an empty path");
    }
    return reference;
}

json CompositionToJson(const ImageComposition& composition, ConfigOrigin owner_origin) {
    if (!std::isfinite(composition.image_margin) ||
        composition.image_margin < 0.0 || composition.image_margin > 10000.0) {
        throw std::runtime_error("composition image margin is out of range");
    }
    json images = json::array();
    for (const auto& reference : composition.images) {
        if (owner_origin == ConfigOrigin::Global && reference.origin != ConfigOrigin::Global) {
            throw std::runtime_error("global composition cannot reference a local image");
        }
        images.push_back(ImageReferenceToJson(reference));
    }
    return json{
        {"name", ToUtf8(composition.name)},
        {"enabled", composition.enabled},
        {"rubyEnabled", composition.ruby_enabled},
        {"imageMargin", composition.image_margin},
        {"matchTexts", MatchTextsToJson(composition.match_texts)},
        {"images", std::move(images)},
    };
}

ImageComposition CompositionFromJson(const json& value, ConfigOrigin owner_origin) {
    ImageComposition composition;
    composition.name = FromUtf8(value.value("name", std::string{}));
    composition.enabled = value.value("enabled", true);
    composition.ruby_enabled = value.value("rubyEnabled", false);
    composition.image_margin = value.value("imageMargin", 0.0);
    if (!std::isfinite(composition.image_margin) ||
        composition.image_margin < 0.0 || composition.image_margin > 10000.0) {
        throw std::runtime_error("composition image margin is out of range");
    }
    composition.match_texts = MatchTextsFromJson(value.value("matchTexts", json::array()));
    composition.origin = owner_origin;
    for (const auto& item : value.value("images", json::array())) {
        ImageReference reference = ImageReferenceFromJson(item);
        if (owner_origin == ConfigOrigin::Global && reference.origin != ConfigOrigin::Global) {
            throw std::runtime_error("global composition cannot reference a local image");
        }
        composition.images.push_back(std::move(reference));
    }
    return composition;
}

json GlobalToJson(const GlobalConfig& config) {
    json sources = json::array();
    for (const auto& source : config.sources) {
        sources.push_back(SourceToJson(source));
    }
    json images = json::array();
    for (const auto& image : config.images) {
        images.push_back(ImageToJson(image));
    }
    json compositions = json::array();
    for (const auto& composition : config.compositions) {
        compositions.push_back(CompositionToJson(composition, ConfigOrigin::Global));
    }
    return json{
        {"normalizeWidth", config.normalize_width},
        {"loadGlobal", config.load_global},
        {"loadLocal", config.load_local},
        {"defaultSize", SizeToJson(config.default_size)},
        {"defaultPadding", PaddingToJson(config.default_padding)},
        {"defaultRuby", RubyToJson(config.default_ruby)},
        {"sources", std::move(sources)},
        {"images", std::move(images)},
        {"unregisteredImages", PathsToJson(config.unregistered_images)},
        {"compositions", std::move(compositions)},
    };
}

GlobalConfig GlobalFromJson(const json& value) {
    GlobalConfig config;
    config.normalize_width = value.value("normalizeWidth", true);
    config.load_global = value.value("loadGlobal", true);
    config.load_local = value.value("loadLocal", true);
    config.default_size = value.contains("defaultSize")
        ? SizeFromJson(value.at("defaultSize")) : ImageSize{};
    config.default_padding = value.contains("defaultPadding")
        ? PaddingFromJson(value.at("defaultPadding")) : ImagePadding{};
    config.default_ruby = value.contains("defaultRuby")
        ? RubyFromJson(value.at("defaultRuby")) : RubySettings{};
    for (const auto& source : value.value("sources", json::array())) {
        config.sources.push_back(SourceFromJson(source));
    }
    for (const auto& image : value.value("images", json::array())) {
        ImageEntry entry = ImageFromJson(image);
        entry.origin = ConfigOrigin::Global;
        config.images.push_back(std::move(entry));
    }
    config.unregistered_images = PathsFromJson(
        value.value("unregisteredImages", json::array()));
    for (const auto& item : value.value("compositions", json::array())) {
        config.compositions.push_back(CompositionFromJson(item, ConfigOrigin::Global));
    }
    return config;
}

json LocalToJson(const LocalConfig& config) {
    json sources = json::array();
    for (const auto& source : config.sources) {
        sources.push_back(SourceToJson(source));
    }
    json images = json::array();
    for (const auto& image : config.images) {
        images.push_back(ImageToJson(image));
    }
    json compositions = json::array();
    for (const auto& composition : config.compositions) {
        compositions.push_back(CompositionToJson(composition, ConfigOrigin::Local));
    }
    return json{
        {"sources", std::move(sources)},
        {"images", std::move(images)},
        {"unregisteredImages", PathsToJson(config.unregistered_images)},
        {"compositions", std::move(compositions)},
    };
}

LocalConfig LocalFromJson(const json& value) {
    LocalConfig config;
    for (const auto& source : value.value("sources", json::array())) {
        config.sources.push_back(SourceFromJson(source));
    }
    for (const auto& image : value.value("images", json::array())) {
        ImageEntry entry = ImageFromJson(image);
        entry.origin = ConfigOrigin::Local;
        config.images.push_back(std::move(entry));
    }
    config.unregistered_images = PathsFromJson(
        value.value("unregisteredImages", json::array()));
    for (const auto& item : value.value("compositions", json::array())) {
        config.compositions.push_back(CompositionFromJson(item, ConfigOrigin::Local));
    }
    return config;
}

fs::path ResolvePath(const fs::path& path, const fs::path& base) {
    fs::path resolved = path;
    if (resolved.is_relative() && !base.empty()) {
        resolved = base / resolved;
    }
    std::error_code error;
    fs::path absolute = fs::absolute(resolved, error);
    return (error ? resolved : absolute).lexically_normal();
}

void PopulateRuntimeFields(ImageEntry& image, const fs::path& base) {
    const std::wstring key = NormalizedPathKey(image.source_path, base);
    image.stable_id = StableImageId(key);
    image.cache_stem = L"mojie_" + image.stable_id;
}

std::vector<fs::path> EnumerateSource(
    const ImageSource& source,
    const fs::path& base,
    std::vector<Diagnostic>& diagnostics) {
    std::vector<fs::path> files;
    const fs::path resolved = ResolvePath(source.path, base);
    std::error_code error;

    if (source.kind == SourceKind::File) {
        if (fs::is_regular_file(resolved, error) && IsSupportedImageFile(resolved)) {
            files.push_back(resolved);
        } else {
            diagnostics.push_back({resolved, L"対応画像を読み込めません。"});
        }
        return files;
    }

    if (!fs::is_directory(resolved, error)) {
        diagnostics.push_back({resolved, L"画像フォルダーを読み込めません。"});
        return files;
    }

    error.clear();
    if (source.recursive) {
        fs::recursive_directory_iterator iterator(
            resolved, fs::directory_options::skip_permission_denied, error);
        const fs::recursive_directory_iterator end;
        for (; !error && iterator != end; iterator.increment(error)) {
            std::error_code status_error;
            if (iterator->is_regular_file(status_error) &&
                IsSupportedImageFile(iterator->path())) {
                files.push_back(iterator->path().lexically_normal());
            }
        }
    } else {
        fs::directory_iterator iterator(
            resolved, fs::directory_options::skip_permission_denied, error);
        const fs::directory_iterator end;
        for (; !error && iterator != end; iterator.increment(error)) {
            std::error_code status_error;
            if (iterator->is_regular_file(status_error) &&
                IsSupportedImageFile(iterator->path())) {
                files.push_back(iterator->path().lexically_normal());
            }
        }
    }
    if (error) {
        diagnostics.push_back({resolved, L"画像フォルダーの走査中にエラーが発生しました。"});
    }

    std::sort(files.begin(), files.end(), [&](const fs::path& left, const fs::path& right) {
        return NormalizedPathKey(left) < NormalizedPathKey(right);
    });
    return files;
}

} // namespace

bool IsSupportedImageFile(const fs::path& path) {
    static constexpr std::array<std::wstring_view, 18> kExtensions = {
        L".png", L".bmp",
        L".jpg", L".jpeg", L".jpe", L".jfif", L".gif",
        L".tif", L".tiff", L".ico", L".dds",
        L".wdp", L".jxr", L".hdp",
        L".webp", L".heic", L".heif", L".avif",
    };
    const std::wstring extension = LowerExtension(path);
    return std::find(kExtensions.begin(), kExtensions.end(), extension) !=
        kExtensions.end();
}

std::wstring NormalizedPathKey(const fs::path& path, const fs::path& base_directory) {
    std::wstring key = ResolvePath(path, base_directory).generic_wstring();
    std::transform(key.begin(), key.end(), key.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return key;
}

std::wstring StableImageId(const std::wstring& normalized_path_key) {
    // 64-bit FNV-1a over UTF-16 code units. This is an identifier, not a
    // security boundary; the full canonical path remains the reconciliation key.
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (const wchar_t character : normalized_path_key) {
        const std::uint16_t unit = static_cast<std::uint16_t>(character);
        hash ^= static_cast<std::uint8_t>(unit & 0xff);
        hash *= UINT64_C(1099511628211);
        hash ^= static_cast<std::uint8_t>((unit >> 8) & 0xff);
        hash *= UINT64_C(1099511628211);
    }
    std::wostringstream stream;
    stream << std::hex << std::setfill(L'0') << std::setw(16) << hash;
    return stream.str();
}

bool LoadGlobalConfig(const fs::path& file, GlobalConfig& config, std::wstring* error) {
    try {
        std::ifstream input(file, std::ios::binary);
        if (!input) {
            if (!fs::exists(file)) {
                config = GlobalConfig{};
                return true;
            }
            throw std::runtime_error("could not open global config");
        }
        config = GlobalFromJson(json::parse(input));
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr) {
            *error = ErrorText(exception);
        }
        return false;
    }
}

bool SaveGlobalConfigAtomic(const fs::path& file, const GlobalConfig& config, std::wstring* error) {
    const fs::path temporary = UniqueSiblingPath(file, L"tmp");
    try {
        std::error_code directory_error;
        if (!file.parent_path().empty()) {
            fs::create_directories(file.parent_path(), directory_error);
            if (directory_error) {
                throw std::system_error(directory_error);
            }
        }
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) {
                throw std::runtime_error("could not create temporary config");
            }
            output << GlobalToJson(config).dump(2) << '\n';
            output.flush();
            if (!output) {
                throw std::runtime_error("could not write temporary config");
            }
        }
        if (!MoveFileExW(
                temporary.c_str(), file.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::system_error(
                static_cast<int>(GetLastError()), std::system_category(),
                "could not replace global config");
        }
        return true;
    } catch (const std::exception& exception) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        if (error != nullptr) {
            *error = ErrorText(exception);
        }
        return false;
    }
}

bool LoadLocalConfig(const fs::path& file, LocalConfig& config, std::wstring* error) {
    try {
        std::ifstream input(file, std::ios::binary);
        if (!input) {
            if (!fs::exists(file)) {
                config = LocalConfig{};
                return true;
            }
            throw std::runtime_error("could not open local config");
        }
        config = LocalFromJson(json::parse(input));
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr) {
            *error = ErrorText(exception);
        }
        return false;
    }
}

bool SaveLocalConfigAtomic(
    const fs::path& file,
    const LocalConfig& config,
    std::wstring* error) {
    const fs::path temporary = UniqueSiblingPath(file, L"tmp");
    try {
        std::error_code directory_error;
        if (!file.parent_path().empty()) {
            fs::create_directories(file.parent_path(), directory_error);
            if (directory_error) {
                throw std::system_error(directory_error);
            }
        }
        LocalConfig portable = config;
        const fs::path base_directory = file.parent_path();
        for (auto& source : portable.sources) {
            source.path = PortableLocalPath(source.path, base_directory);
        }
        for (auto& image : portable.images) {
            image.source_path = PortableLocalPath(image.source_path, base_directory);
        }
        for (auto& path : portable.unregistered_images) {
            path = PortableLocalPath(path, base_directory);
        }
        for (auto& composition : portable.compositions) {
            for (auto& reference : composition.images) {
                if (reference.origin == ConfigOrigin::Local) {
                    reference.source_path =
                        PortableLocalPath(reference.source_path, base_directory);
                }
            }
        }
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) {
                throw std::runtime_error("could not create temporary local config");
            }
            output << LocalToJson(portable).dump(2) << '\n';
            output.flush();
            if (!output) {
                throw std::runtime_error("could not write temporary local config");
            }
        }
        if (!MoveFileExW(
                temporary.c_str(), file.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::system_error(
                static_cast<int>(GetLastError()), std::system_category(),
                "could not replace local config");
        }
        return true;
    } catch (const std::exception& exception) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        if (error != nullptr) {
            *error = ErrorText(exception);
        }
        return false;
    }
}

GlobalConfig MakeEffectiveConfig(
    const GlobalConfig& global,
    const LocalConfig& local,
    const fs::path& local_config_directory) {
    GlobalConfig effective = global;
    effective.sources.clear();
    effective.images.clear();
    effective.compositions.clear();

    if (global.load_global) {
        GlobalConfig global_library = global;
        ScanAndReconcile(global_library);
        for (auto& image : global_library.images) {
            image.origin = ConfigOrigin::Global;
        }
        effective.sources = std::move(global_library.sources);
        effective.images = std::move(global_library.images);
        for (auto& composition : global_library.compositions) {
            composition.origin = ConfigOrigin::Global;
            effective.compositions.push_back(std::move(composition));
        }
    }

    if (global.load_local) {
        LocalConfig local_library = local;
        ScanAndReconcile(local_library, local_config_directory);
        for (auto& source : local_library.sources) {
            source.path = ResolvePath(source.path, local_config_directory);
            effective.sources.push_back(std::move(source));
        }
        for (auto& image : local_library.images) {
            image.source_path = ResolvePath(image.source_path, local_config_directory);
            image.origin = ConfigOrigin::Local;
            effective.images.push_back(std::move(image));
        }
        for (auto& composition : local_library.compositions) {
            composition.origin = ConfigOrigin::Local;
            for (auto& reference : composition.images) {
                if (reference.origin == ConfigOrigin::Local) {
                    reference.source_path =
                        ResolvePath(reference.source_path, local_config_directory);
                }
            }
            effective.compositions.push_back(std::move(composition));
        }
    }
    return effective;
}

const ImageEntry* ResolveImageReference(
    const GlobalConfig& effective,
    const ImageReference& reference) {
    const std::wstring reference_key = NormalizedPathKey(reference.source_path);
    for (const auto& image : effective.images) {
        if (image.origin == reference.origin && image.present && image.enabled &&
            NormalizedPathKey(image.source_path) == reference_key) {
            return &image;
        }
    }
    return nullptr;
}

bool IsCompositionResolvable(
    const GlobalConfig& effective,
    const ImageComposition& composition) {
    if (!composition.enabled || composition.images.empty()) {
        return false;
    }
    return std::all_of(
        composition.images.begin(), composition.images.end(),
        [&](const ImageReference& reference) {
            return ResolveImageReference(effective, reference) != nullptr;
        });
}

ScanResult ScanAndReconcile(GlobalConfig& config, const fs::path& relative_base) {
    ScanResult result;
    std::unordered_set<std::wstring> unregistered;
    for (const auto& path : config.unregistered_images) {
        unregistered.insert(NormalizedPathKey(path, relative_base));
    }
    std::unordered_map<std::wstring, ImageEntry> saved;
    std::vector<std::wstring> saved_order;
    for (auto image : config.images) {
        const std::wstring key = NormalizedPathKey(image.source_path, relative_base);
        PopulateRuntimeFields(image, relative_base);
        saved[key] = std::move(image);
        saved_order.push_back(key);
    }
    for (const auto& key : unregistered) {
        saved.erase(key);
    }

    std::vector<ImageEntry> reconciled;
    std::unordered_map<std::wstring, std::size_t> discovered;
    for (const auto& source : config.sources) {
        for (const auto& file : EnumerateSource(source, relative_base, result.diagnostics)) {
            const std::wstring key = NormalizedPathKey(file);
            if (unregistered.count(key) != 0) {
                continue;
            }
            ImageEntry image;
            const auto duplicate = discovered.find(key);
            if (duplicate != discovered.end()) {
                image = std::move(reconciled[duplicate->second]);
                reconciled.erase(reconciled.begin() + static_cast<std::ptrdiff_t>(duplicate->second));
                discovered.clear();
                for (std::size_t index = 0; index < reconciled.size(); ++index) {
                    discovered[NormalizedPathKey(reconciled[index].source_path)] = index;
                }
            } else {
                const auto existing = saved.find(key);
                if (existing != saved.end()) {
                    image = existing->second;
                } else {
                    image.source_path = ResolvePath(file, {});
                    image.match_texts.push_back(image.source_path.stem().wstring());
                }
            }
            image.source_path = ResolvePath(file, {});
            image.present = true;
            PopulateRuntimeFields(image, {});
            discovered[key] = reconciled.size();
            reconciled.push_back(std::move(image));
            saved.erase(key);
        }
    }

    for (const auto& key : saved_order) {
        const auto missing = saved.find(key);
        if (missing == saved.end()) {
            continue;
        }
        ImageEntry image = missing->second;
        image.present = false;
        PopulateRuntimeFields(image, relative_base);
        reconciled.push_back(std::move(image));
        saved.erase(missing);
    }
    for (auto& remaining : saved) {
        remaining.second.present = false;
        PopulateRuntimeFields(remaining.second, relative_base);
        reconciled.push_back(std::move(remaining.second));
    }

    config.images = std::move(reconciled);
    for (auto& image : config.images) {
        image.origin = ConfigOrigin::Global;
        image.present ? ++result.present_count : ++result.missing_count;
    }
    for (auto& composition : config.compositions) {
        composition.origin = ConfigOrigin::Global;
    }
    return result;
}

ScanResult ScanAndReconcile(LocalConfig& config, const fs::path& relative_base) {
    GlobalConfig library;
    library.sources = config.sources;
    library.images = config.images;
    library.unregistered_images = config.unregistered_images;
    ScanResult result = ScanAndReconcile(library, relative_base);
    config.sources = std::move(library.sources);
    config.images = std::move(library.images);
    config.unregistered_images = std::move(library.unregistered_images);

    for (auto& image : config.images) {
        image.origin = ConfigOrigin::Local;
        if (relative_base.empty() || image.source_path.is_relative()) {
            continue;
        }
        std::error_code relative_error;
        fs::path relative = fs::relative(image.source_path, relative_base, relative_error);
        if (relative_error || relative.empty()) {
            continue;
        }
        const auto first = relative.begin();
        if (first != relative.end() && *first != L"..") {
            image.source_path = relative.lexically_normal();
        }
    }
    for (auto& composition : config.compositions) {
        composition.origin = ConfigOrigin::Local;
    }
    return result;
}

std::size_t UnregisterMissingImages(GlobalConfig& config) {
    const std::size_t previous_size = config.images.size();
    config.images.erase(
        std::remove_if(config.images.begin(), config.images.end(),
                       [](const ImageEntry& image) { return !image.present; }),
        config.images.end());
    return previous_size - config.images.size();
}

std::size_t UnregisterMissingImages(LocalConfig& config) {
    const std::size_t previous_size = config.images.size();
    config.images.erase(
        std::remove_if(config.images.begin(), config.images.end(),
                       [](const ImageEntry& image) { return !image.present; }),
        config.images.end());
    return previous_size - config.images.size();
}

namespace {

CacheSyncResult SyncManagedCacheImpl(
    GlobalConfig& config,
    const fs::path& app_data_path,
    const CacheSyncFailureHandler& failure_handler,
    bool force_regeneration) {
    CacheSyncResult result;
    const fs::path cache_directory = app_data_path / L"Font" / L"mojie";
    std::error_code error;
    fs::create_directories(cache_directory, error);
    if (error) {
        result.diagnostics.push_back({cache_directory, L"管理画像フォルダーを作成できません。"});
        return result;
    }

    for (auto& image : config.images) {
        if (!image.present || (!force_regeneration && !image.enabled)) {
            continue;
        }
        PopulateRuntimeFields(image, {});
        const bool native_image = ShouldCopyNativeFontImage(image.source_path);
        const std::wstring cache_extension = native_image
            ? LowerExtension(image.source_path) : L".png";
        const fs::path destination =
            cache_directory / (image.cache_stem + cache_extension);
        const fs::path fingerprint = destination.wstring() + L".source";
        if (!force_regeneration && native_image) {
            std::error_code source_size_error;
            std::error_code destination_size_error;
            std::error_code source_time_error;
            std::error_code destination_time_error;
            const auto source_size = fs::file_size(image.source_path, source_size_error);
            const auto destination_size = fs::file_size(destination, destination_size_error);
            const auto source_time = fs::last_write_time(image.source_path, source_time_error);
            const auto destination_time = fs::last_write_time(destination, destination_time_error);
            if (!source_size_error && !destination_size_error && !source_time_error &&
                !destination_time_error && source_size == destination_size &&
                source_time == destination_time &&
                FilesHaveSameContents(image.source_path, destination)) {
                continue;
            }
        } else if (!force_regeneration &&
                   FingerprintMatches(image.source_path, destination, fingerprint)) {
            continue;
        }

        const fs::path temporary = UniqueSiblingPath(destination, L"tmp");
        error.clear();
        bool prepared = false;
        if (native_image) {
            fs::copy_file(
                image.source_path, temporary,
                fs::copy_options::overwrite_existing, error);
            prepared = !error;
        } else {
            prepared = ConvertFirstFrameToPng(image.source_path, temporary);
        }
        if (!prepared || !MoveFileExW(
                temporary.c_str(), destination.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            std::error_code ignored;
            fs::remove(temporary, ignored);
            const Diagnostic diagnostic{
                image.source_path,
                native_image
                    ? L"管理画像へコピーできません。"
                    : L"画像をPNGへ変換できません。対応するWindows画像コーデックを確認してください。"};
            if (!native_image && failure_handler) {
                const CacheSyncFailureAction action =
                    failure_handler(diagnostic, image);
                if (action == CacheSyncFailureAction::SkipImage) {
                    result.skipped_images.push_back(
                        {image.origin, image.source_path});
                    continue;
                }
                if (action == CacheSyncFailureAction::Stop) {
                    result.diagnostics.push_back(diagnostic);
                    return result;
                }
            }
            result.diagnostics.push_back(diagnostic);
            continue;
        }

        ++result.copied_count;
        if (!native_image &&
            !SaveFingerprintAtomic(image.source_path, destination, fingerprint)) {
            result.diagnostics.push_back({
                image.source_path, L"変換画像の更新情報を保存できません。"});
        }
    }

    // Never remove cache files. A generated-looking filename
    // is not sufficient proof of ownership, and an image can still be required
    // by a temporarily unavailable global or local source. Cleanup requires a
    // durable ownership manifest first.
    return result;
}

} // namespace

CacheSyncResult SyncManagedCache(
    GlobalConfig& config,
    const fs::path& app_data_path,
    const CacheSyncFailureHandler& failure_handler) {
    return SyncManagedCacheImpl(
        config, app_data_path, failure_handler, false);
}

CacheSyncResult RegenerateManagedCache(
    GlobalConfig& config,
    const fs::path& app_data_path,
    const CacheSyncFailureHandler& failure_handler) {
    return SyncManagedCacheImpl(
        config, app_data_path, failure_handler, true);
}

std::unordered_map<std::wstring, std::size_t> BuildReplacementIndex(const GlobalConfig& config) {
    std::unordered_map<std::wstring, std::size_t> result;
    for (std::size_t index = 0; index < config.images.size(); ++index) {
        const auto& image = config.images[index];
        if (!image.present || !image.enabled) {
            continue;
        }
        for (const auto& text : image.match_texts) {
            if (!text.empty()) {
                result[text] = index;
            }
        }
    }
    return result;
}

} // namespace mojie
