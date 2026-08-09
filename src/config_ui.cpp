#include "config_ui.hpp"

#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <utility>

namespace mojie {
namespace {

namespace fs = std::filesystem;

constexpr wchar_t kWindowClass[] = L"mojie.ConfigDialog";

class UniqueBitmap {
public:
    UniqueBitmap() = default;
    explicit UniqueBitmap(HBITMAP bitmap) : bitmap_(bitmap) {}
    ~UniqueBitmap() { reset(); }

    UniqueBitmap(const UniqueBitmap&) = delete;
    UniqueBitmap& operator=(const UniqueBitmap&) = delete;

    UniqueBitmap(UniqueBitmap&& other) noexcept
        : bitmap_(std::exchange(other.bitmap_, nullptr)) {}
    UniqueBitmap& operator=(UniqueBitmap&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.bitmap_, nullptr));
        }
        return *this;
    }

    HBITMAP get() const { return bitmap_; }
    explicit operator bool() const { return bitmap_ != nullptr; }

    void reset(HBITMAP bitmap = nullptr) {
        if (bitmap_ != bitmap && bitmap_ != nullptr) {
            DeleteObject(bitmap_);
        }
        bitmap_ = bitmap;
    }

private:
    HBITMAP bitmap_ = nullptr;
};

class UniqueImageList {
public:
    UniqueImageList() = default;
    explicit UniqueImageList(HIMAGELIST image_list) : image_list_(image_list) {}
    ~UniqueImageList() { reset(); }
    UniqueImageList(const UniqueImageList&) = delete;
    UniqueImageList& operator=(const UniqueImageList&) = delete;
    UniqueImageList(UniqueImageList&& other) noexcept
        : image_list_(std::exchange(other.image_list_, nullptr)) {}
    UniqueImageList& operator=(UniqueImageList&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.image_list_, nullptr));
        }
        return *this;
    }
    HIMAGELIST get() const { return image_list_; }
    void reset(HIMAGELIST image_list = nullptr) {
        if (image_list_ != image_list && image_list_ != nullptr) {
            ImageList_Destroy(image_list_);
        }
        image_list_ = image_list;
    }
private:
    HIMAGELIST image_list_ = nullptr;
};

class ScopedComInitialization {
public:
    ScopedComInitialization()
        : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
    ~ScopedComInitialization() {
        if (result_ == S_OK || result_ == S_FALSE) {
            CoUninitialize();
        }
    }

    bool available() const {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }

private:
    HRESULT result_ = E_FAIL;
};

struct PreviewResult {
    UniqueBitmap bitmap;
    UINT source_width = 0;
    UINT source_height = 0;
};

PreviewResult DecodePreview(const fs::path& path, UINT maximum_width, UINT maximum_height);
UniqueBitmap MakeThumbnail(const fs::path& path, UINT size);
void RefreshCompositionPreview(struct DialogState& state);

struct FileSignature {
    bool exists = false;
    bool readable = true;
    std::uintmax_t size = 0;
    fs::file_time_type write_time{};
    std::uint64_t hash = UINT64_C(14695981039346656037);

    bool operator==(const FileSignature& other) const {
        return exists == other.exists && readable == other.readable &&
            size == other.size && write_time == other.write_time && hash == other.hash;
    }
    bool operator!=(const FileSignature& other) const { return !(*this == other); }
};

FileSignature CaptureFileSignature(const fs::path& file) {
    FileSignature signature;
    if (file.empty()) {
        return signature;
    }
    std::error_code error;
    signature.exists = fs::exists(file, error);
    if (error || !signature.exists) {
        signature.readable = !error;
        return signature;
    }
    if (!fs::is_regular_file(file, error) || error) {
        signature.readable = false;
        return signature;
    }
    signature.size = fs::file_size(file, error);
    if (error) {
        signature.readable = false;
        return signature;
    }
    signature.write_time = fs::last_write_time(file, error);
    if (error) {
        signature.readable = false;
        return signature;
    }
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        signature.readable = false;
        return signature;
    }
    char buffer[4096];
    while (input.read(buffer, sizeof(buffer)) || input.gcount() > 0) {
        for (std::streamsize index = 0; index < input.gcount(); ++index) {
            signature.hash ^= static_cast<unsigned char>(buffer[index]);
            signature.hash *= UINT64_C(1099511628211);
        }
    }
    signature.readable = input.eof();
    return signature;
}

enum ControlId {
    IdTab = 100,
    IdSourceTab,
    IdSourceList,
    IdAddFolder,
    IdAddFile,
    IdRemoveSource,
    IdRecursive,
    IdRescan,
    IdUnregisterMissing,
    IdImageList,
    IdImagePreview,
    IdImageDimensions,
    IdAliases,
    IdRubyEnabled,
    IdRubyOverride,
    IdOpenFolder,
    IdRemoveImage,
    IdNormalizeWidth,
    IdLoadGlobal,
    IdLoadLocal,
    IdGlobalConfigPath,
    IdLocalConfigPath,
    IdOpenGlobalConfig,
    IdOpenLocalConfig,
    IdDefaultSizeMode,
    IdDefaultSizeValue,
    IdDefaultPaddingX,
    IdDefaultPaddingY,
    IdDefaultRubySize,
    IdCompositionTab,
    IdCompositionList,
    IdAddComposition,
    IdRemoveComposition,
    IdCompositionName,
    IdCompositionAliases,
    IdCompositionRubyEnabled,
    IdCompositionMargin,
    IdCompositionImages,
    IdCompositionCandidate,
    IdCompositionThumbnailSize,
    IdCompositionThumbnailSizeValue,
    IdCompositionPreview,
    IdRemoveCompositionImage,
    IdMoveCompositionImageUp,
    IdMoveCompositionImageDown,
    IdOk = IDOK,
    IdCancel = IDCANCEL,
};

enum class RecognitionImageStatus {
    Normal,
    Caution,
    Warning,
};

struct DialogState {
    HWND window = nullptr;
    HWND owner = nullptr;
    HINSTANCE instance = nullptr;
    GlobalConfig global_working;
    LocalConfig local_working;
    GlobalConfig* global_output = nullptr;
    LocalConfig* local_output = nullptr;
    fs::path global_config_file;
    fs::path local_config_file;
    bool local_config_editable = true;
    FileSignature global_config_signature;
    FileSignature local_config_signature;
    fs::path app_data_path;
    ConfigOrigin source_origin = ConfigOrigin::Global;
    struct ImageRowReference {
        ConfigOrigin origin = ConfigOrigin::Global;
        std::size_t index = 0;
    };
    std::vector<ImageRowReference> image_rows;
    std::vector<RecognitionImageStatus> image_statuses;
    int editing_image = -1;
    bool refreshing_image_list = false;
    HWND image_status_tooltip = nullptr;
    int image_status_tooltip_row = -1;
    bool tracking_image_status_mouse = false;
    UniqueBitmap preview_bitmap;
    UniqueImageList origin_image_list;
    int global_origin_image = -1;
    int local_origin_image = -1;
    int caution_status_image = -1;
    int warning_status_image = -1;
    std::vector<HWND> global_controls;
    std::vector<HWND> image_controls;
    std::vector<HWND> composition_controls;
    ConfigOrigin composition_origin = ConfigOrigin::Global;
    int editing_composition = -1;
    bool refreshing_composition_list = false;
    std::vector<mojie::ImageReference> composition_candidates;
    bool composition_candidates_valid = false;
    ConfigOrigin composition_candidates_origin = ConfigOrigin::Global;
    UniqueImageList composition_candidate_image_list;
    std::vector<int> composition_candidate_image_indices;
    std::vector<UniqueBitmap> composition_preview_bitmaps;
    int composition_thumbnail_size = 48;
    bool dragging_composition_candidate = false;
    int dragging_composition_candidate_index = -1;
    ConfigOrigin dragging_composition_origin = ConfigOrigin::Global;
    int dragging_composition_editor_index = -1;
    bool accepted = false;
    bool finished = false;
};

UniqueBitmap RenderSymbolGlyph(
    const wchar_t* font_name,
    wchar_t glyph,
    int width,
    int height,
    COLORREF color) {
    HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        return {};
    }
    HDC destination = CreateCompatibleDC(screen);
    if (destination == nullptr) {
        ReleaseDC(nullptr, screen);
        return {};
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    UniqueBitmap bitmap(CreateDIBSection(
        screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0));
    ReleaseDC(nullptr, screen);
    if (!bitmap || pixels == nullptr) {
        DeleteDC(destination);
        return {};
    }
    std::fill_n(
        static_cast<std::uint32_t*>(pixels),
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
        0);

    const HGDIOBJ old_bitmap = SelectObject(destination, bitmap.get());
    HFONT font = CreateFontW(
        -height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        DEFAULT_PITCH, font_name);
    if (font == nullptr) {
        SelectObject(destination, old_bitmap);
        DeleteDC(destination);
        return {};
    }
    const HGDIOBJ old_font = SelectObject(destination, font);
    wchar_t selected_font[LF_FACESIZE]{};
    const bool requested_font_selected =
        GetTextFaceW(destination, static_cast<int>(std::size(selected_font)), selected_font) > 0 &&
        _wcsicmp(selected_font, font_name) == 0;
    if (requested_font_selected) {
        SetBkMode(destination, TRANSPARENT);
        SetTextColor(destination, RGB(255, 255, 255));
        RECT bounds{0, 0, width, height};
        DrawTextW(
            destination, &glyph, 1, &bounds,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    bool has_visible_pixels = false;
    auto* argb = static_cast<std::uint32_t*>(pixels);
    const std::size_t pixel_count =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    for (std::size_t index = 0; index < pixel_count; ++index) {
        const BYTE alpha = static_cast<BYTE>(std::max({
            argb[index] & 0xff,
            (argb[index] >> 8) & 0xff,
            (argb[index] >> 16) & 0xff}));
        has_visible_pixels = has_visible_pixels || alpha != 0;
        const BYTE red = static_cast<BYTE>(GetRValue(color) * alpha / 255);
        const BYTE green = static_cast<BYTE>(GetGValue(color) * alpha / 255);
        const BYTE blue = static_cast<BYTE>(GetBValue(color) * alpha / 255);
        argb[index] = static_cast<std::uint32_t>(alpha) << 24 |
            static_cast<std::uint32_t>(red) << 16 |
            static_cast<std::uint32_t>(green) << 8 | blue;
    }

    SelectObject(destination, old_font);
    DeleteObject(font);
    SelectObject(destination, old_bitmap);
    DeleteDC(destination);
    if (!requested_font_selected || !has_visible_pixels) {
        return {};
    }
    return bitmap;
}

int AddSymbolGlyph(
    HIMAGELIST image_list,
    wchar_t glyph,
    int width,
    int height,
    COLORREF color) {
    constexpr const wchar_t* fonts[] = {
        L"Segoe Fluent Icons",
        L"Segoe MDL2 Assets",
    };
    for (const wchar_t* font : fonts) {
        UniqueBitmap bitmap = RenderSymbolGlyph(
            font, glyph, width, height, color);
        if (bitmap) {
            return ImageList_Add(image_list, bitmap.get(), nullptr);
        }
    }
    return -1;
}

void InitializeOriginImages(DialogState& state) {
    const int width = GetSystemMetrics(SM_CXSMICON);
    const int height = GetSystemMetrics(SM_CYSMICON);
    UniqueImageList images(ImageList_Create(
        width, height, ILC_COLOR32, 4, 4));
    if (images.get() == nullptr) {
        return;
    }
    state.global_origin_image = AddSymbolGlyph(
        images.get(), L'\ue774', width, height, GetSysColor(COLOR_WINDOWTEXT));
    state.local_origin_image = AddSymbolGlyph(
        images.get(), L'\ue8b7', width, height, GetSysColor(COLOR_WINDOWTEXT));
    state.caution_status_image = AddSymbolGlyph(
        images.get(), L'\ue7ba', width, height, RGB(255, 185, 0));
    state.warning_status_image = AddSymbolGlyph(
        images.get(), L'\ue7ba', width, height, RGB(232, 17, 35));
    state.origin_image_list = std::move(images);
}

int OriginImage(const DialogState& state, ConfigOrigin origin) {
    return origin == ConfigOrigin::Global
        ? state.global_origin_image : state.local_origin_image;
}

void SetOriginCell(
    const DialogState& state,
    HWND list,
    int row,
    ConfigOrigin origin) {
    const int image = OriginImage(state, origin);
    if (image < 0) {
        ListView_SetItemText(
            list, row, 1, const_cast<wchar_t*>(
                origin == ConfigOrigin::Global ? L"グローバル" : L"ローカル"));
        return;
    }
    LVITEMW item{};
    item.mask = LVIF_TEXT | LVIF_IMAGE;
    item.iItem = row;
    item.iSubItem = 1;
    item.pszText = const_cast<wchar_t*>(L"");
    item.iImage = image;
    ListView_SetItem(list, &item);
}

void SetImageStatusCell(
    const DialogState& state,
    HWND list,
    int row,
    RecognitionImageStatus status) {
    const int requested_image = status == RecognitionImageStatus::Warning
        ? state.warning_status_image
        : status == RecognitionImageStatus::Caution
            ? state.caution_status_image
            : I_IMAGENONE;
    const int status_image = requested_image >= 0
        ? requested_image : I_IMAGENONE;
    LVITEMW item{};
    item.mask = LVIF_TEXT | LVIF_IMAGE;
    item.iItem = row;
    item.iSubItem = 2;
    item.pszText = const_cast<wchar_t*>(
        requested_image < 0 && status != RecognitionImageStatus::Normal ? L"!" : L"");
    item.iImage = status_image;
    ListView_SetItem(list, &item);
}

RecognitionImageStatus GetRecognitionImageStatus(
    const ImageEntry& image,
    const fs::path& resolved_path) {
    std::error_code exists_error;
    if (!fs::is_regular_file(resolved_path, exists_error)) {
        return RecognitionImageStatus::Warning;
    }
    return image.present
        ? RecognitionImageStatus::Normal
        : RecognitionImageStatus::Caution;
}

bool LocalPathKnown(const DialogState& state) {
    return !state.local_config_file.empty();
}

bool LocalAvailable(const DialogState& state) {
    return LocalPathKnown(state) && state.local_config_editable;
}

std::vector<ImageSource>& ActiveSources(DialogState& state) {
    return state.source_origin == ConfigOrigin::Global
        ? state.global_working.sources : state.local_working.sources;
}

const std::vector<ImageSource>& ActiveSources(const DialogState& state) {
    return state.source_origin == ConfigOrigin::Global
        ? state.global_working.sources : state.local_working.sources;
}

std::vector<ImageComposition>& ActiveCompositions(DialogState& state) {
    return state.composition_origin == ConfigOrigin::Global
        ? state.global_working.compositions : state.local_working.compositions;
}

const std::vector<ImageComposition>& ActiveCompositions(const DialogState& state) {
    return state.composition_origin == ConfigOrigin::Global
        ? state.global_working.compositions : state.local_working.compositions;
}

ImageComposition* EditingComposition(DialogState& state) {
    auto& compositions = ActiveCompositions(state);
    return state.editing_composition >= 0 &&
            static_cast<std::size_t>(state.editing_composition) < compositions.size()
        ? &compositions[static_cast<std::size_t>(state.editing_composition)] : nullptr;
}

const ImageEntry* FindConfiguredImage(
    const DialogState& state,
    const ImageReference& reference) {
    const auto& images = reference.origin == ConfigOrigin::Global
        ? state.global_working.images : state.local_working.images;
    const fs::path base = reference.origin == ConfigOrigin::Local && LocalPathKnown(state)
        ? state.local_config_file.parent_path() : fs::path{};
    const std::wstring key = NormalizedPathKey(reference.source_path, base);
    const auto found = std::find_if(images.begin(), images.end(), [&](const ImageEntry& image) {
        return NormalizedPathKey(image.source_path, base) == key;
    });
    return found == images.end() ? nullptr : &*found;
}

std::wstring CompositionImageLabel(
    const DialogState& state,
    const ImageReference& reference) {
    std::wstring label = reference.origin == ConfigOrigin::Global
        ? L"[グローバル] " : L"[ローカル] ";
    const ImageEntry* image = FindConfiguredImage(state, reference);
    label += (image != nullptr ? image->source_path : reference.source_path).filename().wstring();
    if (image == nullptr) {
        label += L"（未検出）";
    } else if (!image->present) {
        label += L"（見つかりません）";
    } else if (!image->enabled) {
        label += L"（無効）";
    }
    return label;
}

std::wstring CompositionCandidateLabel(
    const DialogState& state,
    const ImageReference& reference) {
    const ImageEntry* image = FindConfiguredImage(state, reference);
    std::wstring file_name =
        (image != nullptr ? image->source_path : reference.source_path).filename().wstring();
    constexpr std::size_t kMaximumLabelLength = 12;
    if (file_name.size() > kMaximumLabelLength) {
        const fs::path file_path(file_name);
        const std::wstring extension = file_path.extension().wstring();
        const std::size_t suffix_length = std::min<std::size_t>(extension.size(), 5);
        const std::size_t prefix_length = kMaximumLabelLength - suffix_length - 1;
        file_name = file_name.substr(0, prefix_length) + L"…" +
            extension.substr(extension.size() - suffix_length);
    }
    if (image == nullptr) {
        file_name += L" (?)";
    } else if (!image->present) {
        file_name += L" (欠落)";
    } else if (!image->enabled) {
        file_name += L" (無効)";
    }
    return file_name;
}

fs::path ResolvedReferencePath(
    const DialogState& state,
    const ImageReference& reference) {
    const ImageEntry* image = FindConfiguredImage(state, reference);
    fs::path path = image == nullptr ? reference.source_path : image->source_path;
    if (reference.origin == ConfigOrigin::Local && path.is_relative() && LocalPathKnown(state)) {
        path = state.local_config_file.parent_path() / path;
    }
    return path;
}

ImageEntry* ImageAtRow(DialogState& state, int row) {
    if (row < 0 || static_cast<std::size_t>(row) >= state.image_rows.size()) {
        return nullptr;
    }
    const auto reference = state.image_rows[static_cast<std::size_t>(row)];
    auto& images = reference.origin == ConfigOrigin::Global
        ? state.global_working.images : state.local_working.images;
    return reference.index < images.size() ? &images[reference.index] : nullptr;
}

const ImageEntry* ImageAtRow(const DialogState& state, int row) {
    return ImageAtRow(const_cast<DialogState&>(state), row);
}

fs::path ResolvedImagePath(const DialogState& state, int row) {
    const ImageEntry* image = ImageAtRow(state, row);
    if (image == nullptr) {
        return {};
    }
    const auto origin = state.image_rows[static_cast<std::size_t>(row)].origin;
    if (origin == ConfigOrigin::Local && image->source_path.is_relative()) {
        return state.local_config_file.parent_path() / image->source_path;
    }
    return image->source_path;
}

constexpr UINT_PTR kImageStatusListSubclassId = 1;
constexpr UINT_PTR kImageStatusTooltipToolId = 1;

TOOLINFOW ImageStatusTooltipTool(HWND list) {
    TOOLINFOW tool{};
    tool.cbSize = sizeof(tool);
    tool.uFlags = TTF_TRACK | TTF_ABSOLUTE;
    tool.hwnd = list;
    tool.uId = kImageStatusTooltipToolId;
    return tool;
}

void HideImageStatusTooltip(DialogState& state, HWND list) {
    if (state.image_status_tooltip != nullptr &&
        state.image_status_tooltip_row >= 0) {
        TOOLINFOW tool = ImageStatusTooltipTool(list);
        SendMessageW(
            state.image_status_tooltip, TTM_TRACKACTIVATE, FALSE,
            reinterpret_cast<LPARAM>(&tool));
    }
    state.image_status_tooltip_row = -1;
}

int ImageStatusRowAtPoint(
    const DialogState& state,
    HWND list,
    POINT point,
    RECT* icon_bounds) {
    LVHITTESTINFO hit{};
    hit.pt = point;
    const int row = ListView_SubItemHitTest(list, &hit);
    if (row < 0 || hit.iSubItem != 2 ||
        static_cast<std::size_t>(row) >= state.image_statuses.size() ||
        state.image_statuses[static_cast<std::size_t>(row)] ==
            RecognitionImageStatus::Normal) {
        return -1;
    }
    RECT bounds{};
    if (!ListView_GetSubItemRect(list, row, 2, LVIR_ICON, &bounds) ||
        !PtInRect(&bounds, point)) {
        return -1;
    }
    if (icon_bounds != nullptr) {
        *icon_bounds = bounds;
    }
    return row;
}

void ShowImageStatusTooltip(
    DialogState& state,
    HWND list,
    int row,
    const RECT& icon_bounds) {
    if (state.image_status_tooltip == nullptr ||
        static_cast<std::size_t>(row) >= state.image_statuses.size()) {
        return;
    }
    if (state.image_status_tooltip_row == row) {
        return;
    }
    HideImageStatusTooltip(state, list);
    const wchar_t* text = state.image_statuses[static_cast<std::size_t>(row)] ==
            RecognitionImageStatus::Warning
        ? L"画像が存在しません"
        : L"画像ソースに含まれていません";
    TOOLINFOW tool = ImageStatusTooltipTool(list);
    tool.lpszText = const_cast<wchar_t*>(text);
    SendMessageW(
        state.image_status_tooltip, TTM_UPDATETIPTEXTW, 0,
        reinterpret_cast<LPARAM>(&tool));
    POINT position{
        (icon_bounds.left + icon_bounds.right) / 2,
        icon_bounds.bottom + 2,
    };
    ClientToScreen(list, &position);
    SendMessageW(
        state.image_status_tooltip, TTM_TRACKPOSITION, 0,
        MAKELPARAM(position.x, position.y));
    SendMessageW(
        state.image_status_tooltip, TTM_TRACKACTIVATE, TRUE,
        reinterpret_cast<LPARAM>(&tool));
    state.image_status_tooltip_row = row;
}

LRESULT CALLBACK ImageStatusListSubclass(
    HWND list,
    UINT message,
    WPARAM wparam,
    LPARAM lparam,
    UINT_PTR subclass_id,
    DWORD_PTR reference_data) noexcept;

LRESULT ImageStatusListSubclassImpl(
    HWND list,
    UINT message,
    WPARAM wparam,
    LPARAM lparam,
    UINT_PTR subclass_id,
    DWORD_PTR reference_data) {
    auto* state = reinterpret_cast<DialogState*>(reference_data);
    if (state == nullptr) {
        return DefSubclassProc(list, message, wparam, lparam);
    }
    switch (message) {
    case WM_MOUSEMOVE: {
        if (!state->tracking_image_status_mouse) {
            TRACKMOUSEEVENT tracking{};
            tracking.cbSize = sizeof(tracking);
            tracking.dwFlags = TME_LEAVE;
            tracking.hwndTrack = list;
            state->tracking_image_status_mouse =
                TrackMouseEvent(&tracking) != FALSE;
        }
        const POINT point{
            static_cast<LONG>(static_cast<short>(LOWORD(lparam))),
            static_cast<LONG>(static_cast<short>(HIWORD(lparam))),
        };
        RECT icon_bounds{};
        const int row = ImageStatusRowAtPoint(
            *state, list, point, &icon_bounds);
        if (row >= 0) {
            ShowImageStatusTooltip(*state, list, row, icon_bounds);
        } else {
            HideImageStatusTooltip(*state, list);
        }
        break;
    }
    case WM_MOUSELEAVE:
        state->tracking_image_status_mouse = false;
        HideImageStatusTooltip(*state, list);
        break;
    case WM_NCDESTROY:
        HideImageStatusTooltip(*state, list);
        if (state->image_status_tooltip != nullptr) {
            DestroyWindow(state->image_status_tooltip);
            state->image_status_tooltip = nullptr;
        }
        RemoveWindowSubclass(list, ImageStatusListSubclass, subclass_id);
        break;
    default:
        break;
    }
    return DefSubclassProc(list, message, wparam, lparam);
}

LRESULT CALLBACK ImageStatusListSubclass(
    HWND list,
    UINT message,
    WPARAM wparam,
    LPARAM lparam,
    UINT_PTR subclass_id,
    DWORD_PTR reference_data) noexcept {
    try {
        return ImageStatusListSubclassImpl(
            list, message, wparam, lparam, subclass_id, reference_data);
    } catch (...) {
        OutputDebugStringW(
            L"mojie: 画像状態のツールチップ処理に失敗しました。\n");
        if (message == WM_NCDESTROY) {
            RemoveWindowSubclass(list, ImageStatusListSubclass, subclass_id);
        }
        return DefSubclassProc(list, message, wparam, lparam);
    }
}

HWND Item(const DialogState& state, int id) {
    return GetDlgItem(state.window, id);
}

void ShowTab(DialogState& state, int selection) {
    TabCtrl_SetCurSel(Item(state, IdTab), selection);
    for (HWND control : state.global_controls) {
        ShowWindow(control, selection == 0 ? SW_SHOW : SW_HIDE);
    }
    for (HWND control : state.image_controls) {
        ShowWindow(control, selection == 1 ? SW_SHOW : SW_HIDE);
    }
    for (HWND control : state.composition_controls) {
        ShowWindow(control, selection == 2 ? SW_SHOW : SW_HIDE);
    }
}

std::wstring WindowText(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring value(static_cast<std::size_t>(std::max(length, 0)) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(window, value.data(), length + 1);
    }
    value.resize(static_cast<std::size_t>(std::max(length, 0)));
    return value;
}

void SetDefaultFont(HWND window) {
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
}

HWND AddControl(
    DialogState& state,
    DWORD extended_style,
    const wchar_t* class_name,
    const wchar_t* text,
    DWORD style,
    int x,
    int y,
    int width,
    int height,
    int id,
    std::vector<HWND>* page_controls = nullptr) {
    HWND control = CreateWindowExW(
        extended_style, class_name, text, WS_CHILD | WS_VISIBLE | style,
        x, y, width, height, state.window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), state.instance, nullptr);
    SetDefaultFont(control);
    if (page_controls != nullptr) {
        page_controls->push_back(control);
    }
    return control;
}

std::wstring FormatNumber(double value) {
    wchar_t buffer[64] = {};
    swprintf_s(buffer, L"%.12g", value);
    return buffer;
}

bool ParseNumber(HWND edit, double& value) {
    const std::wstring text = WindowText(edit);
    if (text.empty()) {
        return false;
    }
    wchar_t* end = nullptr;
    errno = 0;
    const double parsed = std::wcstod(text.c_str(), &end);
    while (end != nullptr && *end != L'\0' && iswspace(*end)) {
        ++end;
    }
    if (end == text.c_str() || end == nullptr || *end != L'\0' || errno == ERANGE ||
        !std::isfinite(parsed)) {
        return false;
    }
    value = parsed;
    return true;
}

std::vector<std::wstring> ParseAliases(const std::wstring& text) {
    std::vector<std::wstring> aliases;
    std::set<std::wstring> seen;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t end = text.find(L'\n', begin);
        std::wstring line = text.substr(begin, end == std::wstring::npos ? end : end - begin);
        if (!line.empty() && line.back() == L'\r') {
            line.pop_back();
        }
        if (!line.empty() && seen.insert(line).second) {
            aliases.push_back(std::move(line));
        }
        if (end == std::wstring::npos) {
            break;
        }
        begin = end + 1;
    }
    return aliases;
}

std::wstring JoinAliases(const std::vector<std::wstring>& aliases) {
    std::wstring result;
    for (const auto& alias : aliases) {
        if (!result.empty()) {
            result += L" / ";
        }
        result += alias;
    }
    return result;
}

void UpdateCompositionButtons(DialogState& state) {
    const bool local_locked = state.composition_origin == ConfigOrigin::Local &&
        !LocalAvailable(state);
    const bool has_composition = EditingComposition(state) != nullptr;
    const int image_selection = static_cast<int>(SendMessageW(
        Item(state, IdCompositionImages), LB_GETCURSEL, 0, 0));
    const ImageComposition* composition = EditingComposition(state);
    const int image_count = composition == nullptr
        ? 0 : static_cast<int>(composition->images.size());
    EnableWindow(Item(state, IdAddComposition), !local_locked);
    EnableWindow(Item(state, IdRemoveComposition), has_composition && !local_locked);
    EnableWindow(Item(state, IdCompositionName), has_composition && !local_locked);
    EnableWindow(Item(state, IdCompositionAliases), has_composition && !local_locked);
    EnableWindow(Item(state, IdCompositionRubyEnabled), has_composition && !local_locked);
    EnableWindow(Item(state, IdCompositionMargin), has_composition && !local_locked);
    EnableWindow(Item(state, IdCompositionCandidate), has_composition && !local_locked &&
        !state.composition_candidates.empty());
    EnableWindow(Item(state, IdRemoveCompositionImage),
        has_composition && !local_locked && image_selection >= 0);
    EnableWindow(Item(state, IdMoveCompositionImageUp),
        has_composition && !local_locked && image_selection > 0);
    EnableWindow(Item(state, IdMoveCompositionImageDown),
        has_composition && !local_locked && image_selection >= 0 &&
        image_selection + 1 < image_count);
}

void RefreshCompositionImages(DialogState& state, int preferred_selection = 0) {
    HWND list = Item(state, IdCompositionImages);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    const ImageComposition* composition = EditingComposition(state);
    if (composition != nullptr) {
        for (const auto& reference : composition->images) {
            const std::wstring label = CompositionImageLabel(state, reference);
            SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
    }
    const int count = composition == nullptr ? 0 : static_cast<int>(composition->images.size());
    const int selection = count == 0 ? -1 : std::clamp(preferred_selection, 0, count - 1);
    SendMessageW(list, LB_SETCURSEL, selection, 0);
    RefreshCompositionPreview(state);
    UpdateCompositionButtons(state);
}

void RefreshCompositionCandidates(DialogState& state) {
    HWND list = Item(state, IdCompositionCandidate);
    ListView_DeleteAllItems(list);
    UniqueImageList new_image_list(ImageList_Create(
        state.composition_thumbnail_size, state.composition_thumbnail_size,
        ILC_COLOR32, 8, 8));
    ListView_SetImageList(list, new_image_list.get(), LVSIL_NORMAL);
    state.composition_candidate_image_list = std::move(new_image_list);
    ListView_SetIconSpacing(
        list, state.composition_thumbnail_size + 18,
        state.composition_thumbnail_size + 26);
    state.composition_candidates.clear();
    state.composition_candidate_image_indices.clear();
    const auto append = [&](const std::vector<ImageEntry>& images, ConfigOrigin origin) {
        for (const auto& image : images) {
            ImageReference reference{origin, image.source_path};
            const int index = static_cast<int>(state.composition_candidates.size());
            state.composition_candidates.push_back(reference);
            const std::wstring label = CompositionCandidateLabel(state, reference);
            UniqueBitmap thumbnail = MakeThumbnail(
                ResolvedReferencePath(state, reference),
                static_cast<UINT>(state.composition_thumbnail_size));
            const int image_index = state.composition_candidate_image_list.get() == nullptr
                ? -1 : ImageList_Add(
                    state.composition_candidate_image_list.get(), thumbnail.get(), nullptr);
            state.composition_candidate_image_indices.push_back(image_index);
            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
            item.iItem = index;
            item.pszText = const_cast<wchar_t*>(label.c_str());
            item.iImage = image_index;
            item.lParam = static_cast<LPARAM>(index);
            ListView_InsertItem(list, &item);
        }
    };
    append(state.global_working.images, ConfigOrigin::Global);
    if (state.composition_origin == ConfigOrigin::Local) {
        append(state.local_working.images, ConfigOrigin::Local);
    }
    if (!state.composition_candidates.empty()) {
        ListView_SetItemState(
            list, 0, LVIS_SELECTED | LVIS_FOCUSED,
            LVIS_SELECTED | LVIS_FOCUSED);
    }
    state.composition_candidates_valid = true;
    state.composition_candidates_origin = state.composition_origin;
    const std::wstring size_label =
        std::to_wstring(state.composition_thumbnail_size) + L"px";
    SetWindowTextW(Item(state, IdCompositionThumbnailSizeValue), size_label.c_str());
    UpdateCompositionButtons(state);
}

bool CommitCompositionEditor(DialogState& state, bool show_error) {
    ImageComposition* composition = EditingComposition(state);
    if (composition == nullptr) {
        return true;
    }
    const std::wstring name = WindowText(Item(state, IdCompositionName));
    const auto aliases = ParseAliases(WindowText(Item(state, IdCompositionAliases)));
    const bool ruby_enabled = SendMessageW(
        Item(state, IdCompositionRubyEnabled), BM_GETCHECK, 0, 0) == BST_CHECKED;
    double image_margin = 0.0;
    if (name.empty()) {
        if (show_error) {
            ShowTab(state, 2);
            MessageBoxW(state.window, L"合成名を入力してください。", L"mojie", MB_OK | MB_ICONWARNING);
            SetFocus(Item(state, IdCompositionName));
        }
        return false;
    }
    if (aliases.empty()) {
        if (show_error) {
            ShowTab(state, 2);
            MessageBoxW(state.window, L"置換文字列を1つ以上入力してください。", L"mojie", MB_OK | MB_ICONWARNING);
            SetFocus(Item(state, IdCompositionAliases));
        }
        return false;
    }
    if (composition->images.empty()) {
        if (show_error) {
            ShowTab(state, 2);
            MessageBoxW(state.window, L"合成する画像を1つ以上追加してください。", L"mojie", MB_OK | MB_ICONWARNING);
            SetFocus(Item(state, IdCompositionCandidate));
        }
        return false;
    }
    if (!ParseNumber(Item(state, IdCompositionMargin), image_margin) ||
        image_margin < 0.0 || image_margin > 10000.0) {
        if (show_error) {
            ShowTab(state, 2);
            MessageBoxW(
                state.window,
                L"画像間隔には0から10000までの数値を入力してください。",
                L"mojie", MB_OK | MB_ICONWARNING);
            SetFocus(Item(state, IdCompositionMargin));
        }
        return false;
    }
    composition->name = name;
    composition->match_texts = aliases;
    composition->ruby_enabled = ruby_enabled;
    composition->image_margin = image_margin;
    HWND list = Item(state, IdCompositionList);
    ListView_SetItemText(
        list, state.editing_composition, 2, const_cast<wchar_t*>(composition->name.c_str()));
    const std::wstring summary = JoinAliases(composition->match_texts);
    ListView_SetItemText(
        list, state.editing_composition, 3, const_cast<wchar_t*>(summary.c_str()));
    const std::wstring margin_summary = FormatNumber(composition->image_margin);
    ListView_SetItemText(
        list, state.editing_composition, 4,
        const_cast<wchar_t*>(margin_summary.c_str()));
    ListView_SetItemText(
        list, state.editing_composition, 5,
        const_cast<wchar_t*>(composition->ruby_enabled ? L"表示" : L"非表示"));
    return true;
}

void LoadCompositionEditor(DialogState& state, int index) {
    state.editing_composition = index;
    const ImageComposition* composition = EditingComposition(state);
    SetWindowTextW(Item(state, IdCompositionName),
        composition == nullptr ? L"" : composition->name.c_str());
    std::wstring multiline;
    if (composition != nullptr) {
        for (const auto& alias : composition->match_texts) {
            if (!multiline.empty()) {
                multiline += L"\r\n";
            }
            multiline += alias;
        }
    }
    SetWindowTextW(Item(state, IdCompositionAliases), multiline.c_str());
    SendMessageW(
        Item(state, IdCompositionRubyEnabled), BM_SETCHECK,
        composition != nullptr && composition->ruby_enabled
            ? BST_CHECKED : BST_UNCHECKED,
        0);
    const std::wstring margin_text = composition == nullptr
        ? L"0" : FormatNumber(composition->image_margin);
    SetWindowTextW(Item(state, IdCompositionMargin), margin_text.c_str());
    if (!state.composition_candidates_valid ||
        state.composition_candidates_origin != state.composition_origin) {
        RefreshCompositionCandidates(state);
    }
    RefreshCompositionImages(state);
}

void RefreshCompositions(DialogState& state, int preferred_selection = 0) {
    HWND list = Item(state, IdCompositionList);
    state.refreshing_composition_list = true;
    ListView_DeleteAllItems(list);
    const auto& compositions = ActiveCompositions(state);
    for (std::size_t index = 0; index < compositions.size(); ++index) {
        const auto& composition = compositions[index];
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(index);
        item.pszText = const_cast<wchar_t*>(L"");
        item.lParam = static_cast<LPARAM>(index);
        const int row = ListView_InsertItem(list, &item);
        SetOriginCell(state, list, row, state.composition_origin);
        ListView_SetItemText(list, row, 2, const_cast<wchar_t*>(composition.name.c_str()));
        const std::wstring summary = JoinAliases(composition.match_texts);
        ListView_SetItemText(list, row, 3, const_cast<wchar_t*>(summary.c_str()));
        const std::wstring margin_summary = FormatNumber(composition.image_margin);
        ListView_SetItemText(
            list, row, 4, const_cast<wchar_t*>(margin_summary.c_str()));
        ListView_SetItemText(
            list, row, 5,
            const_cast<wchar_t*>(composition.ruby_enabled ? L"表示" : L"非表示"));
        ListView_SetCheckState(list, row, composition.enabled ? TRUE : FALSE);
    }
    const int selection = compositions.empty() ? -1 :
        std::clamp(preferred_selection, 0, static_cast<int>(compositions.size()) - 1);
    if (selection >= 0) {
        ListView_SetItemState(list, selection, LVIS_SELECTED | LVIS_FOCUSED,
            LVIS_SELECTED | LVIS_FOCUSED);
    }
    state.refreshing_composition_list = false;
    LoadCompositionEditor(state, selection);
}

bool CommitImageEditor(DialogState& state, bool show_error) {
    ImageEntry* image = ImageAtRow(state, state.editing_image);
    if (image == nullptr) {
        return true;
    }
    const auto aliases = ParseAliases(WindowText(Item(state, IdAliases)));
    if (aliases.empty()) {
        if (show_error) {
            ShowTab(state, 1);
            MessageBoxW(state.window, L"置換文字列を1つ以上入力してください。", L"mojie", MB_OK | MB_ICONWARNING);
            SetFocus(Item(state, IdAliases));
        }
        return false;
    }

    const bool ruby_enabled =
        SendMessageW(Item(state, IdRubyEnabled), BM_GETCHECK, 0, 0) == BST_CHECKED;
    const std::wstring ruby_override = WindowText(Item(state, IdRubyOverride));
    const bool invalid_ruby = ruby_override.find_first_of(L"<>\r\n") !=
            std::wstring::npos ||
        std::any_of(ruby_override.begin(), ruby_override.end(), [](wchar_t value) {
            return value <= 0x1f || value == 0x7f || value == 0x85 ||
                value == 0x2028 || value == 0x2029;
        });
    if (invalid_ruby) {
        if (show_error) {
            ShowTab(state, 1);
            MessageBoxW(
                state.window,
                L"ふりがな指定は1行で入力し、'<' と '>' は使用しないでください。",
                L"mojie", MB_OK | MB_ICONWARNING);
            SetFocus(Item(state, IdRubyOverride));
        }
        return false;
    }

    image->match_texts = aliases;
    image->ruby_enabled = ruby_enabled;
    image->ruby_text_override = ruby_override;
    HWND list = Item(state, IdImageList);
    const std::wstring ruby_summary = !image->ruby_enabled
        ? L"（非表示）"
        : (image->ruby_text_override.empty()
            ? L"（置換文字列）" : image->ruby_text_override);
    ListView_SetItemText(
        list, state.editing_image, 4,
        const_cast<wchar_t*>(ruby_summary.c_str()));
    return true;
}

void EnableImageEditor(DialogState& state, bool enabled) {
    EnableWindow(Item(state, IdAliases), enabled);
    EnableWindow(Item(state, IdRubyEnabled), enabled);
    const bool ruby_enabled = enabled &&
        SendMessageW(Item(state, IdRubyEnabled), BM_GETCHECK, 0, 0) == BST_CHECKED;
    EnableWindow(Item(state, IdRubyOverride), ruby_enabled);
    EnableWindow(Item(state, IdOpenFolder), enabled);
    const bool local_image = state.editing_image >= 0 &&
        static_cast<std::size_t>(state.editing_image) < state.image_rows.size() &&
        state.image_rows[static_cast<std::size_t>(state.editing_image)].origin == ConfigOrigin::Local;
    EnableWindow(Item(state, IdRemoveImage), enabled && (!local_image || LocalAvailable(state)));
}

PreviewResult DecodePreview(const fs::path& path, UINT maximum_width, UINT maximum_height) {
    PreviewResult result;
    if (maximum_width == 0 || maximum_height == 0) {
        return result;
    }

    ScopedComInitialization com;
    if (!com.available()) {
        return result;
    }

    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    HRESULT status = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(status)) {
        return result;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    status = factory->CreateDecoderFromFilename(
        path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(status)) {
        return result;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    status = decoder->GetFrame(0, &frame);
    if (FAILED(status) || FAILED(frame->GetSize(&result.source_width, &result.source_height)) ||
        result.source_width == 0 || result.source_height == 0) {
        result.source_width = 0;
        result.source_height = 0;
        return result;
    }

    const double scale = std::min({
        1.0,
        static_cast<double>(maximum_width) / static_cast<double>(result.source_width),
        static_cast<double>(maximum_height) / static_cast<double>(result.source_height),
    });
    const UINT width = std::max(
        1U, static_cast<UINT>(std::floor(static_cast<double>(result.source_width) * scale)));
    const UINT height = std::max(
        1U, static_cast<UINT>(std::floor(static_cast<double>(result.source_height) * scale)));
    if (width > static_cast<UINT>(std::numeric_limits<LONG>::max()) ||
        height > static_cast<UINT>(std::numeric_limits<LONG>::max()) ||
        width > std::numeric_limits<UINT>::max() / 4U) {
        return result;
    }
    const UINT stride = width * 4U;
    if (height > std::numeric_limits<UINT>::max() / stride) {
        return result;
    }
    const UINT buffer_size = stride * height;

    IWICBitmapSource* source = frame.Get();
    ComPtr<IWICBitmapScaler> scaler;
    if (width != result.source_width || height != result.source_height) {
        status = factory->CreateBitmapScaler(&scaler);
        if (FAILED(status) || FAILED(scaler->Initialize(
                frame.Get(), width, height, WICBitmapInterpolationModeFant))) {
            return result;
        }
        source = scaler.Get();
    }

    ComPtr<IWICFormatConverter> converter;
    status = factory->CreateFormatConverter(&converter);
    if (FAILED(status) || FAILED(converter->Initialize(
            source, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
            nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        return result;
    }

    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = static_cast<LONG>(width);
    bitmap_info.bmiHeader.biHeight = -static_cast<LONG>(height);
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    UniqueBitmap bitmap(CreateDIBSection(
        nullptr, &bitmap_info, DIB_RGB_COLORS, &pixels, nullptr, 0));
    if (!bitmap || pixels == nullptr ||
        FAILED(converter->CopyPixels(
            nullptr, stride, buffer_size, static_cast<BYTE*>(pixels)))) {
        return result;
    }

    const COLORREF background = GetSysColor(COLOR_WINDOW);
    const unsigned background_blue = GetBValue(background);
    const unsigned background_green = GetGValue(background);
    const unsigned background_red = GetRValue(background);
    auto* bytes = static_cast<BYTE*>(pixels);
    for (UINT offset = 0; offset < buffer_size; offset += 4U) {
        BYTE* pixel = bytes + offset;
        const unsigned inverse_alpha = 255U - pixel[3];
        pixel[0] = static_cast<BYTE>(std::min(
            255U, static_cast<unsigned>(pixel[0]) +
                      (background_blue * inverse_alpha + 127U) / 255U));
        pixel[1] = static_cast<BYTE>(std::min(
            255U, static_cast<unsigned>(pixel[1]) +
                      (background_green * inverse_alpha + 127U) / 255U));
        pixel[2] = static_cast<BYTE>(std::min(
            255U, static_cast<unsigned>(pixel[2]) +
                      (background_red * inverse_alpha + 127U) / 255U));
        pixel[3] = 255;
    }

    result.bitmap = std::move(bitmap);
    return result;
}

UniqueBitmap MakeThumbnail(const fs::path& path, UINT size) {
    if (size == 0 || size > static_cast<UINT>(std::numeric_limits<LONG>::max())) {
        return {};
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(size);
    info.bmiHeader.biHeight = -static_cast<LONG>(size);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    UniqueBitmap canvas(CreateDIBSection(
        nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0));
    if (!canvas || pixels == nullptr) {
        return {};
    }

    const COLORREF background = GetSysColor(COLOR_WINDOW);
    auto* bytes = static_cast<BYTE*>(pixels);
    for (std::size_t offset = 0; offset < static_cast<std::size_t>(size) * size * 4U;
         offset += 4U) {
        bytes[offset] = GetBValue(background);
        bytes[offset + 1] = GetGValue(background);
        bytes[offset + 2] = GetRValue(background);
        bytes[offset + 3] = 255;
    }

    HDC destination = CreateCompatibleDC(nullptr);
    if (destination == nullptr) {
        return canvas;
    }
    HGDIOBJ previous_destination = SelectObject(destination, canvas.get());
    PreviewResult preview = DecodePreview(path, size - 4U, size - 4U);
    if (preview.bitmap) {
        BITMAP bitmap{};
        GetObjectW(preview.bitmap.get(), sizeof(bitmap), &bitmap);
        HDC source = CreateCompatibleDC(destination);
        if (source != nullptr) {
            HGDIOBJ previous_source = SelectObject(source, preview.bitmap.get());
            BitBlt(
                destination,
                (static_cast<int>(size) - bitmap.bmWidth) / 2,
                (static_cast<int>(size) - bitmap.bmHeight) / 2,
                bitmap.bmWidth, bitmap.bmHeight, source, 0, 0, SRCCOPY);
            SelectObject(source, previous_source);
            DeleteDC(source);
        }
    } else {
        RECT bounds{0, 0, static_cast<LONG>(size), static_cast<LONG>(size)};
        SetBkMode(destination, TRANSPARENT);
        SetTextColor(destination, GetSysColor(COLOR_GRAYTEXT));
        DrawTextW(destination, L"?", -1, &bounds,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(destination, previous_destination);
    DeleteDC(destination);
    return canvas;
}

void RefreshCompositionPreview(DialogState& state) {
    state.composition_preview_bitmaps.clear();
    const ImageComposition* composition = EditingComposition(state);
    if (composition != nullptr) {
        state.composition_preview_bitmaps.reserve(composition->images.size());
        for (const auto& reference : composition->images) {
            PreviewResult preview = DecodePreview(
                ResolvedReferencePath(state, reference), 160, 70);
            state.composition_preview_bitmaps.push_back(std::move(preview.bitmap));
        }
    }
    InvalidateRect(Item(state, IdCompositionPreview), nullptr, TRUE);
}

void ClearImagePreview(DialogState& state, const wchar_t* dimensions) {
    state.preview_bitmap.reset();
    SetWindowTextW(Item(state, IdImageDimensions), dimensions);
    InvalidateRect(Item(state, IdImagePreview), nullptr, TRUE);
}

void UpdateImagePreview(DialogState& state, const fs::path& image_path) {
    RECT bounds{};
    if (!GetClientRect(Item(state, IdImagePreview), &bounds)) {
        ClearImagePreview(state, L"読み込めません");
        return;
    }
    const UINT maximum_width = static_cast<UINT>(std::max(bounds.right - bounds.left, 0L));
    const UINT maximum_height = static_cast<UINT>(std::max(bounds.bottom - bounds.top, 0L));
    PreviewResult preview = DecodePreview(image_path, maximum_width, maximum_height);
    if (preview.source_width == 0 || preview.source_height == 0) {
        ClearImagePreview(state, L"読み込めません");
        return;
    }

    std::wstring dimensions = std::to_wstring(preview.source_width);
    dimensions += L"px x ";
    dimensions += std::to_wstring(preview.source_height);
    dimensions += L"px";
    SetWindowTextW(Item(state, IdImageDimensions), dimensions.c_str());
    state.preview_bitmap = std::move(preview.bitmap);
    InvalidateRect(Item(state, IdImagePreview), nullptr, TRUE);
}

void LoadImageEditor(DialogState& state, int index) {
    state.editing_image = index;
    const ImageEntry* image = ImageAtRow(state, index);
    if (image == nullptr) {
        EnableImageEditor(state, false);
        SetWindowTextW(Item(state, IdAliases), L"");
        SendMessageW(Item(state, IdRubyEnabled), BM_SETCHECK, BST_UNCHECKED, 0);
        SetWindowTextW(Item(state, IdRubyOverride), L"");
        ClearImagePreview(state, L"—");
        return;
    }
    std::wstring aliases;
    for (const auto& alias : image->match_texts) {
        if (!aliases.empty()) {
            aliases += L"\r\n";
        }
        aliases += alias;
    }
    SetWindowTextW(Item(state, IdAliases), aliases.c_str());
    SendMessageW(
        Item(state, IdRubyEnabled), BM_SETCHECK,
        image->ruby_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextW(Item(state, IdRubyOverride), image->ruby_text_override.c_str());
    EnableImageEditor(state, true);
    UpdateImagePreview(state, ResolvedImagePath(state, index));
}

bool CommitGlobalEditor(DialogState& state, bool show_error) {
    const int mode_index = static_cast<int>(
        SendMessageW(Item(state, IdDefaultSizeMode), CB_GETCURSEL, 0, 0));
    ImageSize size;
    size.mode = mode_index == 1 ? SizeMode::Percent :
                mode_index == 2 ? SizeMode::Pixels : SizeMode::LineHeight;
    size.value = 100.0;
    if (size.mode != SizeMode::LineHeight &&
        (!ParseNumber(Item(state, IdDefaultSizeValue), size.value) ||
         size.value <= 0.0 || size.value > 10000.0)) {
        if (show_error) {
            ShowTab(state, 0);
            MessageBoxW(state.window, L"画像サイズには0より大きく10000以下の数値を入力してください。", L"mojie", MB_OK | MB_ICONWARNING);
            SetFocus(Item(state, IdDefaultSizeValue));
        }
        return false;
    }

    ImagePadding padding;
    if (!ParseNumber(Item(state, IdDefaultPaddingX), padding.x) ||
        !ParseNumber(Item(state, IdDefaultPaddingY), padding.y) ||
        padding.x < -10000.0 || padding.x > 10000.0 ||
        padding.y < -10000.0 || padding.y > 10000.0) {
        if (show_error) {
            ShowTab(state, 0);
            MessageBoxW(state.window, L"余白には-10000以上10000以下の数値を入力してください。", L"mojie", MB_OK | MB_ICONWARNING);
        }
        return false;
    }

    double ruby_size = state.global_working.default_ruby.size_percent;
    if (!ParseNumber(Item(state, IdDefaultRubySize), ruby_size) ||
        ruby_size <= 0.0 || ruby_size > 10000.0) {
        if (show_error) {
            ShowTab(state, 0);
            MessageBoxW(state.window, L"ふりがなサイズには0より大きい数値を入力してください。", L"mojie", MB_OK | MB_ICONWARNING);
            SetFocus(Item(state, IdDefaultRubySize));
        }
        return false;
    }

    state.global_working.normalize_width =
        SendMessageW(Item(state, IdNormalizeWidth), BM_GETCHECK, 0, 0) == BST_CHECKED;
    state.global_working.load_global =
        SendMessageW(Item(state, IdLoadGlobal), BM_GETCHECK, 0, 0) == BST_CHECKED;
    state.global_working.load_local =
        SendMessageW(Item(state, IdLoadLocal), BM_GETCHECK, 0, 0) == BST_CHECKED;
    state.global_working.default_size = size;
    state.global_working.default_padding = padding;
    state.global_working.default_ruby.size_percent = ruby_size;
    return true;
}

void UpdateSourceSelection(DialogState& state) {
    const auto& sources = ActiveSources(state);
    const int selection = static_cast<int>(SendMessageW(Item(state, IdSourceList), LB_GETCURSEL, 0, 0));
    const bool directory = selection >= 0 &&
        static_cast<std::size_t>(selection) < sources.size() &&
        sources[static_cast<std::size_t>(selection)].kind == SourceKind::Directory;
    const bool editable = state.source_origin == ConfigOrigin::Global || LocalAvailable(state);
    EnableWindow(Item(state, IdAddFolder), editable);
    EnableWindow(Item(state, IdAddFile), editable);
    EnableWindow(Item(state, IdRescan), editable);
    EnableWindow(Item(state, IdUnregisterMissing), editable);
    EnableWindow(Item(state, IdRemoveSource), selection >= 0);
    EnableWindow(Item(state, IdRecursive), directory);
    SendMessageW(
        Item(state, IdRecursive), BM_SETCHECK,
        directory && sources[static_cast<std::size_t>(selection)].recursive ? BST_CHECKED : BST_UNCHECKED,
        0);
}

void RefreshSources(DialogState& state) {
    HWND list = Item(state, IdSourceList);
    const int old_selection = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    const auto& sources = ActiveSources(state);
    for (const auto& source : sources) {
        std::wstring label = source.kind == SourceKind::Directory ? L"[フォルダー] " : L"[画像] ";
        label += source.path.wstring();
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    const int selection = sources.empty() ? -1 :
        std::min(old_selection < 0 ? 0 : old_selection, static_cast<int>(sources.size()) - 1);
    SendMessageW(list, LB_SETCURSEL, selection, 0);
    UpdateSourceSelection(state);
}

void RefreshImages(DialogState& state, int preferred_selection = 0) {
    HWND list = Item(state, IdImageList);
    state.refreshing_image_list = true;
    HideImageStatusTooltip(state, list);
    ListView_DeleteAllItems(list);
    state.image_rows.clear();
    state.image_statuses.clear();
    const auto append_images = [&](const std::vector<ImageEntry>& images, ConfigOrigin origin) {
      for (std::size_t index = 0; index < images.size(); ++index) {
        const auto& image = images[index];
        const int row_index = static_cast<int>(state.image_rows.size());
        state.image_rows.push_back({origin, index});
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
        item.iItem = row_index;
        item.iSubItem = 0;
        item.pszText = const_cast<wchar_t*>(L"");
        item.iImage = I_IMAGENONE;
        item.lParam = static_cast<LPARAM>(row_index);
        const int row = ListView_InsertItem(list, &item);
        SetOriginCell(state, list, row, origin);
        const std::wstring filename = image.source_path.filename().wstring();
        fs::path resolved_path = image.source_path;
        if (origin == ConfigOrigin::Local && resolved_path.is_relative()) {
            resolved_path = state.local_config_file.parent_path() / resolved_path;
        }
        const RecognitionImageStatus status =
            GetRecognitionImageStatus(image, resolved_path);
        state.image_statuses.push_back(status);
        SetImageStatusCell(state, list, row, status);
        const std::wstring path = resolved_path.wstring();
        ListView_SetItemText(list, row, 3, const_cast<wchar_t*>(filename.c_str()));
        const std::wstring ruby_summary = !image.ruby_enabled
            ? L"（非表示）"
            : (image.ruby_text_override.empty()
                ? L"（置換文字列）" : image.ruby_text_override);
        ListView_SetItemText(
            list, row, 4, const_cast<wchar_t*>(ruby_summary.c_str()));
        ListView_SetItemText(list, row, 5, const_cast<wchar_t*>(path.c_str()));
        ListView_SetCheckState(list, row, image.enabled ? TRUE : FALSE);
      }
    };
    append_images(state.global_working.images, ConfigOrigin::Global);
    append_images(state.local_working.images, ConfigOrigin::Local);
    const int selection = state.image_rows.empty() ? -1 :
        std::clamp(preferred_selection, 0, static_cast<int>(state.image_rows.size()) - 1);
    if (selection >= 0) {
        ListView_SetItemState(list, selection, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list, selection, FALSE);
    }
    state.refreshing_image_list = false;
    LoadImageEditor(state, selection);
}

void OpenSelectedImageFolder(DialogState& state) {
    const ImageEntry* image = ImageAtRow(state, state.editing_image);
    if (image == nullptr) {
        return;
    }
    const fs::path folder = ResolvedImagePath(state, state.editing_image).parent_path();
    std::error_code error;
    if (folder.empty() || !fs::is_directory(folder, error)) {
        MessageBoxW(state.window, L"画像のフォルダーが見つかりません。", L"mojie", MB_OK | MB_ICONWARNING);
        return;
    }

    const std::wstring folder_path = folder.wstring();
    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_FLAG_NO_UI;
    execute.hwnd = state.window;
    execute.lpVerb = L"open";
    execute.lpFile = folder_path.c_str();
    execute.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&execute)) {
        MessageBoxW(state.window, L"画像のフォルダーを開けませんでした。", L"mojie", MB_OK | MB_ICONWARNING);
    }
}

void OpenConfigFile(DialogState& state, const fs::path& file) {
    std::error_code error;
    if (file.empty() || !fs::is_regular_file(file, error)) {
        return;
    }
    const std::wstring path = file.wstring();
    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_FLAG_NO_UI;
    execute.hwnd = state.window;
    execute.lpVerb = L"open";
    execute.lpFile = path.c_str();
    execute.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&execute)) {
        MessageBoxW(state.window, L"設定ファイルを開けませんでした。", L"mojie", MB_OK | MB_ICONWARNING);
    }
}

std::optional<fs::path> PickPath(HWND owner, bool folder) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool uninitialize = initialized == S_OK || initialized == S_FALSE;
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(
            CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog)))) {
        if (uninitialize) {
            CoUninitialize();
        }
        return std::nullopt;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    if (folder) {
        options |= FOS_PICKFOLDERS;
    } else {
        options |= FOS_FILEMUSTEXIST;
        const COMDLG_FILTERSPEC filters[] = {
            {L"対応画像", L"*.png;*.bmp;*.jpg;*.jpeg;*.jpe;*.jfif;*.gif;*.tif;*.tiff;*.ico;*.dds;*.wdp;*.jxr;*.hdp;*.webp;*.heic;*.heif;*.avif"},
            {L"すべてのファイル", L"*.*"},
        };
        dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
        dialog->SetFileTypeIndex(1);
    }
    dialog->SetOptions(options);
    std::optional<fs::path> result;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = fs::path(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    if (uninitialize) {
        CoUninitialize();
    }
    return result;
}

void AppendSourcePath(
    DialogState& state,
    SourceKind kind,
    const fs::path& path) {
    fs::path stored_path = path;
    if (state.source_origin == ConfigOrigin::Local) {
        std::error_code relative_error;
        const fs::path relative = fs::relative(
            stored_path, state.local_config_file.parent_path(), relative_error);
        if (!relative_error && !relative.empty()) {
            const auto first = relative.begin();
            if (first == relative.end() || *first != L"..") {
                stored_path = relative.lexically_normal();
            }
        }
    }
    auto& sources = ActiveSources(state);
    sources.push_back({kind, std::move(stored_path), false});
}

void AddSource(DialogState& state, SourceKind kind) {
    if (state.source_origin == ConfigOrigin::Local && !LocalAvailable(state)) {
        return;
    }
    const auto path = PickPath(state.window, kind == SourceKind::Directory);
    if (!path.has_value()) {
        return;
    }
    if (kind == SourceKind::File) {
        if (!IsSupportedImageFile(*path)) {
            MessageBoxW(
                state.window,
                L"対応していない画像形式です。JPG、PNG、GIF、BMP、TIFF、ICOなどを選択してください。",
                L"mojie", MB_OK | MB_ICONWARNING);
            return;
        }
    }
    AppendSourcePath(state, kind, *path);
    auto& sources = ActiveSources(state);
    RefreshSources(state);
    SendMessageW(Item(state, IdSourceList), LB_SETCURSEL, sources.size() - 1, 0);
    UpdateSourceSelection(state);
}

void AddDroppedSources(DialogState& state, HDROP drop) {
    if (state.source_origin == ConfigOrigin::Local && !LocalAvailable(state)) {
        return;
    }

    const UINT path_count = DragQueryFileW(drop, 0xffffffffU, nullptr, 0);
    std::size_t added_count = 0;
    std::size_t skipped_count = 0;
    for (UINT index = 0; index < path_count; ++index) {
        const UINT length = DragQueryFileW(drop, index, nullptr, 0);
        std::wstring buffer(static_cast<std::size_t>(length) + 1, L'\0');
        if (DragQueryFileW(drop, index, buffer.data(), length + 1) == 0) {
            ++skipped_count;
            continue;
        }
        buffer.resize(length);
        const fs::path path(buffer);
        std::error_code directory_error;
        if (fs::is_directory(path, directory_error) && !directory_error) {
            AppendSourcePath(state, SourceKind::Directory, path);
            ++added_count;
        } else if (IsSupportedImageFile(path)) {
            AppendSourcePath(state, SourceKind::File, path);
            ++added_count;
        } else {
            ++skipped_count;
        }
    }

    if (added_count != 0) {
        auto& sources = ActiveSources(state);
        RefreshSources(state);
        SendMessageW(Item(state, IdSourceList), LB_SETCURSEL, sources.size() - 1, 0);
        UpdateSourceSelection(state);
    }
    if (skipped_count != 0) {
        std::wostringstream message;
        message << L"フォルダーまたは対応画像として認識できない項目を "
                << skipped_count << L" 件追加しませんでした。";
        MessageBoxW(
            state.window, message.str().c_str(), L"mojie",
            MB_OK | MB_ICONINFORMATION);
    }
}

LRESULT CALLBACK SourceListProcedure(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam,
    UINT_PTR subclass_id,
    DWORD_PTR reference_data) noexcept {
    if (message == WM_DROPFILES) {
        const HDROP drop = reinterpret_cast<HDROP>(wparam);
        try {
            auto* state = reinterpret_cast<DialogState*>(reference_data);
            if (state != nullptr) {
                AddDroppedSources(*state, drop);
            }
        } catch (...) {
            OutputDebugStringW(
                L"mojie: 画像ソースのドロップ処理中に予期しないエラーが発生しました。\n");
        }
        DragFinish(drop);
        return 0;
    }
    if (message == WM_NCDESTROY) {
        DragAcceptFiles(window, FALSE);
        RemoveWindowSubclass(window, SourceListProcedure, subclass_id);
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

void EnableSourceListDrop(HWND source_list, DialogState& state) {
    constexpr UINT_PTR kSourceListSubclass = 1;
    if (SetWindowSubclass(
            source_list, SourceListProcedure, kSourceListSubclass,
            reinterpret_cast<DWORD_PTR>(&state))) {
        DragAcceptFiles(source_list, TRUE);
    }
}

void Rescan(DialogState& state) {
    if (!CommitImageEditor(state, true)) {
        return;
    }
    std::wstring selected_id;
    ConfigOrigin selected_origin = ConfigOrigin::Global;
    const int old_selection = ListView_GetNextItem(Item(state, IdImageList), -1, LVNI_SELECTED);
    if (const ImageEntry* selected_image = ImageAtRow(state, old_selection)) {
        selected_id = selected_image->stable_id;
        selected_origin = state.image_rows[static_cast<std::size_t>(old_selection)].origin;
    }
    bool unregister_missing = SendMessageW(
        Item(state, IdUnregisterMissing), BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (unregister_missing) {
        const std::size_t missing_count = [&]() {
            if (state.source_origin == ConfigOrigin::Global) {
                GlobalConfig preview = state.global_working;
                return ScanAndReconcile(preview).missing_count;
            }
            LocalConfig preview = state.local_working;
            return ScanAndReconcile(preview, state.local_config_file.parent_path()).missing_count;
        }();
        if (missing_count != 0) {
            std::wostringstream message;
            message << L"画像リストにない認識画像を " << missing_count
                    << L" 件登録解除します。\n\n"
                    << L"画像合成の設定は変更しません。続行しますか？";
            unregister_missing = MessageBoxW(
                state.window, message.str().c_str(), L"mojie",
                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
        }
    }
    const ScanResult result = state.source_origin == ConfigOrigin::Global
        ? ScanAndReconcile(state.global_working)
        : ScanAndReconcile(state.local_working, state.local_config_file.parent_path());
    if (unregister_missing) {
        if (state.source_origin == ConfigOrigin::Global) {
            UnregisterMissingImages(state.global_working);
        } else {
            UnregisterMissingImages(state.local_working);
        }
    }
    int selected = 0;
    if (!selected_id.empty()) {
        const auto& images = selected_origin == ConfigOrigin::Global
            ? state.global_working.images : state.local_working.images;
        const auto found = std::find_if(images.begin(), images.end(),
            [&](const ImageEntry& image) { return image.stable_id == selected_id; });
        if (found != images.end()) {
            selected = static_cast<int>(std::distance(images.begin(), found));
            if (selected_origin == ConfigOrigin::Local) {
                selected += static_cast<int>(state.global_working.images.size());
            }
        }
    }
    RefreshImages(state, selected);
    RefreshCompositionCandidates(state);
    RefreshCompositionImages(state);
    if (!result.diagnostics.empty()) {
        std::wostringstream message;
        message << L"再読み込みは完了しましたが、" << result.diagnostics.size()
                << L"件のパスを読み込めませんでした。\n\n"
                << result.diagnostics.front().path.wstring();
        MessageBoxW(state.window, message.str().c_str(), L"mojie", MB_OK | MB_ICONWARNING);
    }
}

void RemoveSelectedImage(DialogState& state) {
    const int row = state.editing_image;
    if (row < 0 || static_cast<std::size_t>(row) >= state.image_rows.size()) {
        return;
    }
    const DialogState::ImageRowReference reference =
        state.image_rows[static_cast<std::size_t>(row)];
    if (reference.origin == ConfigOrigin::Local && !LocalAvailable(state)) {
        return;
    }
    auto& images = reference.origin == ConfigOrigin::Global
        ? state.global_working.images : state.local_working.images;
    if (reference.index >= images.size()) {
        return;
    }
    auto& unregistered = reference.origin == ConfigOrigin::Global
        ? state.global_working.unregistered_images : state.local_working.unregistered_images;
    const fs::path base_directory = reference.origin == ConfigOrigin::Local && LocalPathKnown(state)
        ? state.local_config_file.parent_path() : fs::path{};
    const fs::path source_path = images[reference.index].source_path;
    const std::wstring source_key = NormalizedPathKey(source_path, base_directory);
    const bool already_unregistered = std::any_of(
        unregistered.begin(), unregistered.end(), [&](const fs::path& path) {
            return NormalizedPathKey(path, base_directory) == source_key;
        });
    if (!already_unregistered) {
        unregistered.push_back(source_path);
    }
    images.erase(images.begin() + static_cast<std::ptrdiff_t>(reference.index));
    RefreshImages(state, std::max(0, row - 1));
    RefreshCompositionCandidates(state);
    RefreshCompositionImages(state);
}

void AddComposition(DialogState& state) {
    if (state.composition_origin == ConfigOrigin::Local && !LocalAvailable(state)) {
        return;
    }
    if (!CommitCompositionEditor(state, true)) {
        return;
    }
    ImageComposition composition;
    composition.name = L"新しい画像合成";
    composition.match_texts = {L"置換文字列"};
    composition.origin = state.composition_origin;
    auto& compositions = ActiveCompositions(state);
    compositions.push_back(std::move(composition));
    RefreshCompositions(state, static_cast<int>(compositions.size()) - 1);
    SetFocus(Item(state, IdCompositionName));
    SendMessageW(Item(state, IdCompositionName), EM_SETSEL, 0, -1);
}

void RemoveComposition(DialogState& state) {
    auto& compositions = ActiveCompositions(state);
    const int index = state.editing_composition;
    if (index < 0 || static_cast<std::size_t>(index) >= compositions.size() ||
        (state.composition_origin == ConfigOrigin::Local && !LocalAvailable(state))) {
        return;
    }
    compositions.erase(compositions.begin() + index);
    RefreshCompositions(state, index);
}

void AddCompositionImage(DialogState& state) {
    const int selection = ListView_GetNextItem(
        Item(state, IdCompositionCandidate), -1, LVNI_SELECTED);
    if (selection < 0 || static_cast<std::size_t>(selection) >=
            state.composition_candidates.size()) {
        return;
    }
    const ImageReference& reference =
        state.composition_candidates[static_cast<std::size_t>(selection)];
    const ImageEntry* image = FindConfiguredImage(state, reference);
    if (image == nullptr || !image->present || !image->enabled) {
        MessageBoxW(
            state.window, L"見つからない画像または無効な画像は追加できません。",
            L"mojie", MB_OK | MB_ICONWARNING);
        return;
    }
    ImageComposition* composition = EditingComposition(state);
    if (composition == nullptr) {
        return;
    }
    composition->images.push_back(reference);
    RefreshCompositionImages(state, static_cast<int>(composition->images.size()) - 1);
}

void CancelCompositionCandidateDrag(DialogState& state) {
    if (!state.dragging_composition_candidate) {
        return;
    }
    state.dragging_composition_candidate = false;
    state.dragging_composition_candidate_index = -1;
    state.dragging_composition_editor_index = -1;
    if (GetCapture() == state.window) {
        ReleaseCapture();
    }
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
}

bool CursorOverCompositionImages(const DialogState& state) {
    RECT bounds{};
    if (!GetWindowRect(Item(state, IdCompositionImages), &bounds)) {
        return false;
    }
    POINT cursor{};
    return GetCursorPos(&cursor) && PtInRect(&bounds, cursor);
}

void BeginCompositionCandidateDrag(DialogState& state, int index) {
    if (index < 0 || static_cast<std::size_t>(index) >=
            state.composition_candidates.size() || EditingComposition(state) == nullptr) {
        return;
    }
    const ImageReference& reference =
        state.composition_candidates[static_cast<std::size_t>(index)];
    const ImageEntry* image = FindConfiguredImage(state, reference);
    if (image == nullptr || !image->present || !image->enabled) {
        return;
    }
    state.dragging_composition_candidate = true;
    state.dragging_composition_candidate_index = index;
    state.dragging_composition_origin = state.composition_origin;
    state.dragging_composition_editor_index = state.editing_composition;
    SetCapture(state.window);
    SetCursor(LoadCursorW(nullptr, IDC_HAND));
}

void DropCompositionCandidate(DialogState& state) {
    const bool over_target = CursorOverCompositionImages(state);
    const int index = state.dragging_composition_candidate_index;
    const bool same_editor = state.dragging_composition_origin == state.composition_origin &&
        state.dragging_composition_editor_index == state.editing_composition;
    CancelCompositionCandidateDrag(state);
    if (!same_editor || !over_target || index < 0 || static_cast<std::size_t>(index) >=
            state.composition_candidates.size()) {
        return;
    }
    HWND candidates = Item(state, IdCompositionCandidate);
    ListView_SetItemState(
        candidates, index, LVIS_SELECTED | LVIS_FOCUSED,
        LVIS_SELECTED | LVIS_FOCUSED);
    AddCompositionImage(state);
}

void RemoveCompositionImage(DialogState& state) {
    ImageComposition* composition = EditingComposition(state);
    const int selection = static_cast<int>(SendMessageW(
        Item(state, IdCompositionImages), LB_GETCURSEL, 0, 0));
    if (composition == nullptr || selection < 0 ||
        static_cast<std::size_t>(selection) >= composition->images.size()) {
        return;
    }
    composition->images.erase(composition->images.begin() + selection);
    RefreshCompositionImages(state, selection);
}

void MoveCompositionImage(DialogState& state, int offset) {
    ImageComposition* composition = EditingComposition(state);
    const int selection = static_cast<int>(SendMessageW(
        Item(state, IdCompositionImages), LB_GETCURSEL, 0, 0));
    const int target = selection + offset;
    if (composition == nullptr || selection < 0 || target < 0 ||
        static_cast<std::size_t>(selection) >= composition->images.size() ||
        static_cast<std::size_t>(target) >= composition->images.size()) {
        return;
    }
    std::swap(
        composition->images[static_cast<std::size_t>(selection)],
        composition->images[static_cast<std::size_t>(target)]);
    RefreshCompositionImages(state, target);
}

enum class ConversionFailureChoice {
    Skip,
    Back,
};

ConversionFailureChoice ShowConversionFailureDialog(
    const DialogState& state,
    const Diagnostic& diagnostic) {
    constexpr int kSkipButton = 1001;
    constexpr int kBackButton = 1002;
    const TASKDIALOG_BUTTON buttons[] = {
        {kSkipButton, L"スキップ"},
        {kBackButton, L"戻る"},
    };
    std::wstring content = diagnostic.path.wstring();
    content += L"\n\n";
    content += diagnostic.message;
    TASKDIALOGCONFIG dialog{};
    dialog.cbSize = sizeof(dialog);
    dialog.hwndParent = state.window;
    dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    dialog.pszWindowTitle = L"mojie";
    dialog.pszMainIcon = TD_ERROR_ICON;
    dialog.pszMainInstruction = L"画像をPNGへ変換できませんでした。";
    dialog.pszContent = content.c_str();
    dialog.cButtons = static_cast<UINT>(std::size(buttons));
    dialog.pButtons = buttons;
    dialog.nDefaultButton = kBackButton;
    int pressed_button = kBackButton;
    if (FAILED(TaskDialogIndirect(
            &dialog, &pressed_button, nullptr, nullptr))) {
        MessageBoxW(
            state.window, content.c_str(), L"mojie", MB_OK | MB_ICONERROR);
        return ConversionFailureChoice::Back;
    }
    return pressed_button == kSkipButton
        ? ConversionFailureChoice::Skip
        : ConversionFailureChoice::Back;
}

bool UnregisterSkippedImage(
    DialogState& state,
    const ImageReference& reference) {
    auto& images = reference.origin == ConfigOrigin::Global
        ? state.global_working.images : state.local_working.images;
    auto& unregistered = reference.origin == ConfigOrigin::Global
        ? state.global_working.unregistered_images
        : state.local_working.unregistered_images;
    const fs::path base = reference.origin == ConfigOrigin::Local && LocalPathKnown(state)
        ? state.local_config_file.parent_path() : fs::path{};
    const std::wstring reference_key =
        NormalizedPathKey(reference.source_path);
    const auto found = std::find_if(
        images.begin(), images.end(), [&](const ImageEntry& image) {
            return NormalizedPathKey(image.source_path, base) == reference_key;
        });
    if (found == images.end()) {
        return false;
    }
    const fs::path stored_path = found->source_path;
    const bool already_unregistered = std::any_of(
        unregistered.begin(), unregistered.end(), [&](const fs::path& path) {
            return NormalizedPathKey(path, base) == reference_key;
        });
    if (!already_unregistered) {
        unregistered.push_back(stored_path);
    }
    images.erase(found);
    return true;
}

void SaveAndClose(DialogState& state) {
    if (!CommitImageEditor(state, true) || !CommitCompositionEditor(state, true) ||
        !CommitGlobalEditor(state, true)) {
        return;
    }
    const bool global_changed_externally =
        CaptureFileSignature(state.global_config_file) != state.global_config_signature;
    const bool local_changed_externally = LocalPathKnown(state) &&
        CaptureFileSignature(state.local_config_file) != state.local_config_signature;
    if (global_changed_externally || local_changed_externally) {
        MessageBoxW(
            state.window,
            L"設定画面を開いた後に設定ファイルが外部で変更されました。\n"
            L"外部の変更を失わないよう保存を中止しました。設定画面を開き直してください。",
            L"mojie", MB_OK | MB_ICONWARNING);
        return;
    }
    const ScanResult global_scan = ScanAndReconcile(state.global_working);
    ScanResult local_scan;
    if (LocalAvailable(state)) {
        local_scan = ScanAndReconcile(state.local_working, state.local_config_file.parent_path());
    }
    GlobalConfig effective = MakeEffectiveConfig(
        state.global_working, state.local_working, state.local_config_file.parent_path());
    bool back_requested = false;
    const CacheSyncResult cache = SyncManagedCache(
        effective, state.app_data_path,
        [&](const Diagnostic& diagnostic, const ImageEntry&) {
            const ConversionFailureChoice choice =
                ShowConversionFailureDialog(state, diagnostic);
            if (choice == ConversionFailureChoice::Skip) {
                return CacheSyncFailureAction::SkipImage;
            }
            back_requested = true;
            return CacheSyncFailureAction::Stop;
        });
    bool skipped_any_image = false;
    for (const ImageReference& skipped : cache.skipped_images) {
        skipped_any_image = UnregisterSkippedImage(state, skipped) || skipped_any_image;
    }
    if (skipped_any_image) {
        RefreshImages(state);
        state.composition_candidates_valid = false;
        RefreshCompositionCandidates(state);
        RefreshCompositionImages(state);
    }
    if (back_requested) {
        return;
    }
    if (!cache.diagnostics.empty()) {
        const Diagnostic& diagnostic = cache.diagnostics.front();
        std::wstring message = L"画像を同期できなかったため、設定を保存しませんでした。\n\n";
        message += diagnostic.path.wstring();
        message += L"\n";
        message += diagnostic.message;
        MessageBoxW(state.window, message.c_str(), L"mojie", MB_OK | MB_ICONERROR);
        return;
    }
    std::error_code local_file_error;
    const bool should_save_local = LocalAvailable(state) &&
        (fs::is_regular_file(state.local_config_file, local_file_error) ||
         !state.local_working.sources.empty() || !state.local_working.images.empty() ||
         !state.local_working.compositions.empty());
    const bool local_existed = should_save_local &&
        fs::is_regular_file(state.local_config_file, local_file_error);
    fs::path local_backup;
    std::wstring error;
    if (local_existed) {
        local_backup = state.local_config_file.parent_path() /
            (state.local_config_file.filename().wstring() + L".backup." +
             std::to_wstring(GetCurrentProcessId()));
        std::error_code backup_error;
        fs::copy_file(
            state.local_config_file, local_backup,
            fs::copy_options::overwrite_existing, backup_error);
        if (backup_error) {
            MessageBoxW(
                state.window,
                L"ローカル設定のバックアップを作成できないため、保存を中止しました。",
                L"mojie", MB_OK | MB_ICONERROR);
            return;
        }
    }
    if (should_save_local &&
        !SaveLocalConfigAtomic(state.local_config_file, state.local_working, &error)) {
        std::error_code ignored;
        fs::remove(local_backup, ignored);
        const std::wstring message = L"ローカル設定を保存できませんでした。\n\n" + error;
        MessageBoxW(state.window, message.c_str(), L"mojie", MB_OK | MB_ICONERROR);
        return;
    }
    if (!SaveGlobalConfigAtomic(state.global_config_file, state.global_working, &error)) {
        bool rollback_ok = true;
        if (should_save_local) {
            if (local_existed) {
                rollback_ok = MoveFileExW(
                    local_backup.c_str(), state.local_config_file.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
            } else {
                std::error_code remove_error;
                fs::remove(state.local_config_file, remove_error);
                rollback_ok = !remove_error;
            }
        }
        std::wstring message = L"グローバル設定を保存できませんでした。\n\n" + error;
        if (!rollback_ok) {
            message += L"\n\nローカル設定の復元にも失敗しました。設定ファイルを確認してください。";
        }
        MessageBoxW(state.window, message.c_str(), L"mojie", MB_OK | MB_ICONERROR);
        return;
    }
    if (!local_backup.empty()) {
        std::error_code ignored;
        fs::remove(local_backup, ignored);
    }
    *state.global_output = state.global_working;
    *state.local_output = state.local_working;
    state.accepted = true;
    state.finished = true;

    if (!global_scan.diagnostics.empty() || !local_scan.diagnostics.empty() || !cache.diagnostics.empty()) {
        std::wostringstream message;
        message << L"設定を保存しましたが、読み込めない画像が "
                << (global_scan.diagnostics.size() + local_scan.diagnostics.size() +
                    cache.diagnostics.size()) << L" 件あります。";
        MessageBoxW(state.window, message.str().c_str(), L"mojie", MB_OK | MB_ICONWARNING);
    } else if (cache.copied_count > 0 || cache.removed_count > 0) {
        MessageBoxW(
            state.window,
            L"画像を同期しました。AviUtl2に反映されない場合は再起動してください。",
            L"mojie", MB_OK | MB_ICONINFORMATION);
    }
    DestroyWindow(state.window);
}

void CreateControls(DialogState& state) {
    InitializeOriginImages(state);

    HWND tab = AddControl(
        state, 0, WC_TABCONTROLW, L"", WS_CLIPSIBLINGS | WS_TABSTOP,
        12, 10, 935, 598, IdTab);
    TCITEMW tab_item{};
    tab_item.mask = TCIF_TEXT;
    tab_item.pszText = const_cast<wchar_t*>(L"全体設定");
    TabCtrl_InsertItem(tab, 0, &tab_item);
    tab_item.pszText = const_cast<wchar_t*>(L"画像設定");
    TabCtrl_InsertItem(tab, 1, &tab_item);
    tab_item.pszText = const_cast<wchar_t*>(L"画像合成");
    TabCtrl_InsertItem(tab, 2, &tab_item);

    auto* global = &state.global_controls;
    auto* images = &state.image_controls;
    auto* compositions = &state.composition_controls;

    AddControl(state, 0, WC_BUTTONW, L"全般", BS_GROUPBOX,
               30, 46, 890, 112, 0, global);
    AddControl(state, 0, WC_BUTTONW, L"半角・全角を区別しない", BS_AUTOCHECKBOX | WS_TABSTOP,
               50, 72, 260, 22, IdNormalizeWidth, global);
    SendMessageW(
        Item(state, IdNormalizeWidth), BM_SETCHECK,
        state.global_working.normalize_width ? BST_CHECKED : BST_UNCHECKED, 0);
    AddControl(state, 0, WC_BUTTONW, L"グローバル設定を読み込む", BS_AUTOCHECKBOX | WS_TABSTOP,
               50, 108, 260, 22, IdLoadGlobal, global);
    SendMessageW(Item(state, IdLoadGlobal), BM_SETCHECK,
                 state.global_working.load_global ? BST_CHECKED : BST_UNCHECKED, 0);
    AddControl(state, 0, WC_BUTTONW, L"ローカル設定を読み込む", BS_AUTOCHECKBOX | WS_TABSTOP,
               340, 108, 260, 22, IdLoadLocal, global);
    SendMessageW(Item(state, IdLoadLocal), BM_SETCHECK,
                 state.global_working.load_local ? BST_CHECKED : BST_UNCHECKED, 0);

    AddControl(state, 0, WC_BUTTONW, L"デフォルト値の設定", BS_GROUPBOX,
               30, 170, 890, 196, 0, global);
    AddControl(state, 0, WC_STATICW, L"画像サイズ", 0,
               52, 200, 110, 20, 0, global);
    HWND default_mode = AddControl(
        state, 0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP,
        170, 196, 190, 150, IdDefaultSizeMode, global);
    SendMessageW(default_mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"行の高さ（既定）"));
    SendMessageW(default_mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"割合（%）"));
    SendMessageW(default_mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"固定（px）"));
    const int default_mode_index = state.global_working.default_size.mode == SizeMode::Percent ? 1 :
        state.global_working.default_size.mode == SizeMode::Pixels ? 2 : 0;
    SendMessageW(default_mode, CB_SETCURSEL, default_mode_index, 0);
    AddControl(state, WS_EX_CLIENTEDGE, WC_EDITW,
               FormatNumber(state.global_working.default_size.value).c_str(), ES_AUTOHSCROLL | WS_TABSTOP,
               375, 196, 110, 24, IdDefaultSizeValue, global);
    EnableWindow(Item(state, IdDefaultSizeValue), default_mode_index != 0);
    AddControl(state, 0, WC_STATICW, L"横余白", 0,
               52, 246, 90, 20, 0, global);
    AddControl(state, WS_EX_CLIENTEDGE, WC_EDITW,
               FormatNumber(state.global_working.default_padding.x).c_str(), ES_AUTOHSCROLL | WS_TABSTOP,
               170, 242, 120, 24, IdDefaultPaddingX, global);
    AddControl(state, 0, WC_STATICW, L"縦余白", 0,
               330, 246, 90, 20, 0, global);
    AddControl(state, WS_EX_CLIENTEDGE, WC_EDITW,
               FormatNumber(state.global_working.default_padding.y).c_str(), ES_AUTOHSCROLL | WS_TABSTOP,
               420, 242, 120, 24, IdDefaultPaddingY, global);

    AddControl(state, 0, WC_STATICW, L"ふりがなサイズ（%）", 0,
               52, 296, 150, 20, 0, global);
    AddControl(state, WS_EX_CLIENTEDGE, WC_EDITW,
               FormatNumber(state.global_working.default_ruby.size_percent).c_str(), ES_AUTOHSCROLL | WS_TABSTOP,
               205, 292, 120, 24, IdDefaultRubySize, global);

    AddControl(state, 0, WC_BUTTONW, L"設定の管理", BS_GROUPBOX,
               30, 464, 890, 112, 0, global);
    AddControl(state, 0, WC_STATICW, L"グローバル", 0,
               50, 491, 85, 20, 0, global);
    std::error_code global_path_error;
    const bool global_file_exists =
        fs::is_regular_file(state.global_config_file, global_path_error);
    const std::wstring global_path = global_file_exists
        ? state.global_config_file.wstring() : L"未作成";
    AddControl(state, 0, WC_STATICW, global_path.c_str(), SS_PATHELLIPSIS,
               140, 491, 630, 20, IdGlobalConfigPath, global);
    AddControl(state, 0, WC_BUTTONW, L"ファイルを開く", BS_PUSHBUTTON | WS_TABSTOP,
               790, 486, 110, 26, IdOpenGlobalConfig, global);
    EnableWindow(Item(state, IdOpenGlobalConfig), global_file_exists);
    AddControl(state, 0, WC_STATICW, L"ローカル", 0,
               50, 532, 85, 20, 0, global);
    std::error_code local_path_error;
    const bool local_file_exists = LocalPathKnown(state) &&
        fs::is_regular_file(state.local_config_file, local_path_error);
    const std::wstring local_path = local_file_exists
        ? state.local_config_file.wstring()
        : LocalPathKnown(state)
            ? (state.local_config_editable ? L"未作成" : L"読み込めません")
            : L"利用できません（プロジェクトが未保存です）";
    AddControl(state, 0, WC_STATICW, local_path.c_str(), SS_PATHELLIPSIS,
               140, 532, 630, 20, IdLocalConfigPath, global);
    AddControl(state, 0, WC_BUTTONW, L"ファイルを開く", BS_PUSHBUTTON | WS_TABSTOP,
               790, 527, 110, 26, IdOpenLocalConfig, global);
    EnableWindow(Item(state, IdOpenLocalConfig), local_file_exists);

    AddControl(state, 0, WC_BUTTONW, L"画像ソース", BS_GROUPBOX,
               30, 46, 890, 174, 0, images);
    HWND source_tab = AddControl(
        state, 0, WC_TABCONTROLW, L"", WS_CLIPSIBLINGS | WS_TABSTOP,
        45, 68, 860, 28, IdSourceTab, images);
    TabCtrl_SetImageList(source_tab, state.origin_image_list.get());
    TCITEMW source_tab_item{};
    source_tab_item.mask = TCIF_TEXT | TCIF_IMAGE;
    source_tab_item.pszText = const_cast<wchar_t*>(L"グローバル設定");
    source_tab_item.iImage = state.global_origin_image;
    TabCtrl_InsertItem(source_tab, 0, &source_tab_item);
    source_tab_item.pszText = const_cast<wchar_t*>(
        LocalAvailable(state) ? L"ローカル設定" :
        LocalPathKnown(state) ? L"ローカル設定（読み込みエラー）" :
                                L"ローカル設定（利用不可）");
    source_tab_item.iImage = state.local_origin_image;
    TabCtrl_InsertItem(source_tab, 1, &source_tab_item);
    HWND source_list = AddControl(
        state, WS_EX_CLIENTEDGE, WC_LISTBOXW, L"",
        LBS_NOTIFY | WS_VSCROLL | WS_TABSTOP,
        58, 100, 550, 88, IdSourceList, images);
    EnableSourceListDrop(source_list, state);
    AddControl(state, 0, WC_BUTTONW, L"フォルダー追加...", BS_PUSHBUTTON | WS_TABSTOP,
               620, 100, 125, 26, IdAddFolder, images);
    AddControl(state, 0, WC_BUTTONW, L"画像追加...", BS_PUSHBUTTON | WS_TABSTOP,
               620, 132, 125, 26, IdAddFile, images);
    AddControl(state, 0, WC_BUTTONW, L"登録解除", BS_PUSHBUTTON | WS_TABSTOP,
               620, 164, 125, 26, IdRemoveSource, images);
    AddControl(state, 0, WC_BUTTONW, L"サブフォルダーを含める", BS_AUTOCHECKBOX | WS_TABSTOP,
               755, 166, 145, 23, IdRecursive, images);
    AddControl(state, 0, WC_BUTTONW, L"画像リストを再読み込み", BS_PUSHBUTTON | WS_TABSTOP,
                755, 100, 145, 26, IdRescan, images);
    AddControl(state, 0, WC_BUTTONW, L"リストにない画像を登録解除",
               BS_AUTOCHECKBOX | WS_TABSTOP,
               755, 132, 165, 23, IdUnregisterMissing, images);
    // The source controls are siblings rather than children of this nested
    // tab. Keep the selector itself header-sized and above the surrounding
    // group box so the tab labels are not painted over by sibling controls.
    SetWindowPos(
        source_tab, HWND_TOP, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    AddControl(state, 0, WC_BUTTONW, L"認識画像", BS_GROUPBOX,
               30, 232, 890, 348, 0, images);
    HWND image_list = AddControl(
        state, WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP,
        45, 258, 520, 306, IdImageList, images);
    ListView_SetExtendedListViewStyle(
        image_list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES |
            LVS_EX_DOUBLEBUFFER | LVS_EX_SUBITEMIMAGES);
    ListView_SetImageList(image_list, state.origin_image_list.get(), LVSIL_SMALL);
    state.image_status_tooltip = CreateWindowExW(
        WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        state.window, nullptr, state.instance, nullptr);
    if (state.image_status_tooltip != nullptr) {
        TOOLINFOW tool = ImageStatusTooltipTool(image_list);
        tool.lpszText = const_cast<wchar_t*>(L"");
        if (SendMessageW(
                state.image_status_tooltip, TTM_ADDTOOLW, 0,
                reinterpret_cast<LPARAM>(&tool)) == FALSE ||
            !SetWindowSubclass(
                image_list, ImageStatusListSubclass,
                kImageStatusListSubclassId,
                reinterpret_cast<DWORD_PTR>(&state))) {
            DestroyWindow(state.image_status_tooltip);
            state.image_status_tooltip = nullptr;
        }
    }
    struct ColumnDefinition {
        const wchar_t* title;
        int width;
        int format;
    };
    constexpr ColumnDefinition columns[] = {
        {L"有効", 44, LVCFMT_CENTER},
        {L"", 28, LVCFMT_CENTER},
        {L"状態", 44, LVCFMT_CENTER},
        {L"ファイル名", 130, LVCFMT_LEFT},
        {L"ふりがな指定", 130, LVCFMT_LEFT},
        {L"パス", 330, LVCFMT_LEFT},
    };
    for (int column_index = 0; column_index < static_cast<int>(std::size(columns)); ++column_index) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        column.pszText = const_cast<wchar_t*>(columns[column_index].title);
        column.cx = columns[column_index].width;
        column.fmt = columns[column_index].format;
        ListView_InsertColumn(image_list, column_index, &column);
    }
    // State-image checkboxes can only live in logical column 0. Reorder the
    // columns so the origin and status remain visually leftmost while
    // preserving native checkbox behavior in the following "有効" column.
    int column_order[] = {1, 2, 0, 3, 4, 5};
    ListView_SetColumnOrderArray(image_list, static_cast<int>(std::size(column_order)), column_order);

    AddControl(state, WS_EX_CLIENTEDGE, WC_STATICW, L"",
               SS_OWNERDRAW,
               580, 258, 325, 102, IdImagePreview, images);
    AddControl(state, 0, WC_STATICW, L"", SS_CENTER,
               580, 364, 110, 20, IdImageDimensions, images);
    AddControl(state, 0, WC_BUTTONW, L"フォルダーを開く", BS_PUSHBUTTON | WS_TABSTOP,
               700, 361, 100, 26, IdOpenFolder, images);
    AddControl(state, 0, WC_BUTTONW, L"登録解除", BS_PUSHBUTTON | WS_TABSTOP,
               810, 361, 95, 26, IdRemoveImage, images);
    AddControl(state, 0, WC_STATICW, L"置換文字列（1行に1つ）", 0,
               580, 389, 220, 20, 0, images);
    AddControl(state, WS_EX_CLIENTEDGE, WC_EDITW, L"",
               ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_TABSTOP,
               580, 411, 325, 70, IdAliases, images);
    AddControl(state, 0, WC_BUTTONW, L"ふりがなを表示", BS_AUTOCHECKBOX | WS_TABSTOP,
               580, 488, 160, 22, IdRubyEnabled, images);
    AddControl(state, 0, WC_STATICW, L"ふりがな指定（空欄は置換文字列）", 0,
               580, 513, 250, 20, 0, images);
    AddControl(state, WS_EX_CLIENTEDGE, WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP,
               580, 534, 325, 24, IdRubyOverride, images);

    AddControl(state, 0, WC_BUTTONW, L"画像合成", BS_GROUPBOX,
               30, 46, 890, 534, 0, compositions);
    HWND composition_tab = AddControl(
        state, 0, WC_TABCONTROLW, L"", WS_CLIPSIBLINGS | WS_TABSTOP,
        45, 68, 860, 28, IdCompositionTab, compositions);
    TabCtrl_SetImageList(composition_tab, state.origin_image_list.get());
    TCITEMW composition_tab_item{};
    composition_tab_item.mask = TCIF_TEXT | TCIF_IMAGE;
    composition_tab_item.pszText = const_cast<wchar_t*>(L"グローバル設定");
    composition_tab_item.iImage = state.global_origin_image;
    TabCtrl_InsertItem(composition_tab, 0, &composition_tab_item);
    composition_tab_item.pszText = const_cast<wchar_t*>(
        LocalAvailable(state) ? L"ローカル設定" :
        LocalPathKnown(state) ? L"ローカル設定（読み込みエラー）" :
                                L"ローカル設定（利用不可）");
    composition_tab_item.iImage = state.local_origin_image;
    TabCtrl_InsertItem(composition_tab, 1, &composition_tab_item);
    SetWindowPos(composition_tab, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    HWND composition_list = AddControl(
        state, WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP,
        48, 104, 430, 216, IdCompositionList, compositions);
    ListView_SetExtendedListViewStyle(
        composition_list,
        LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES |
            LVS_EX_DOUBLEBUFFER | LVS_EX_SUBITEMIMAGES);
    ListView_SetImageList(
        composition_list, state.origin_image_list.get(), LVSIL_SMALL);
    constexpr ColumnDefinition composition_columns[] = {
        {L"有効", 48, LVCFMT_CENTER},
        {L"設定", 52, LVCFMT_CENTER},
        {L"名前", 120, LVCFMT_LEFT},
        {L"置換文字列", 150, LVCFMT_LEFT},
        {L"画像間隔(px)", 88, LVCFMT_RIGHT},
        {L"ふりがな", 65, LVCFMT_CENTER},
    };
    for (int column_index = 0;
         column_index < static_cast<int>(std::size(composition_columns)); ++column_index) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        column.pszText = const_cast<wchar_t*>(composition_columns[column_index].title);
        column.cx = composition_columns[column_index].width;
        column.fmt = composition_columns[column_index].format;
        ListView_InsertColumn(composition_list, column_index, &column);
    }
    int composition_column_order[] = {1, 0, 2, 3, 4, 5};
    ListView_SetColumnOrderArray(
        composition_list, static_cast<int>(std::size(composition_column_order)),
        composition_column_order);
    AddControl(state, 0, WC_BUTTONW, L"追加", BS_PUSHBUTTON | WS_TABSTOP,
               48, 326, 95, 28, IdAddComposition, compositions);
    AddControl(state, 0, WC_BUTTONW, L"削除", BS_PUSHBUTTON | WS_TABSTOP,
               151, 326, 95, 28, IdRemoveComposition, compositions);

    AddControl(state, 0, WC_STATICW, L"追加する画像（ダブルクリックまたはドラッグ）", 0,
               48, 365, 220, 20, 0, compositions);
    AddControl(state, 0, WC_STATICW, L"サイズ", 0,
               270, 365, 42, 20, 0, compositions);
    HWND thumbnail_slider = AddControl(
        state, 0, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP,
        310, 358, 112, 30, IdCompositionThumbnailSize, compositions);
    SendMessageW(thumbnail_slider, TBM_SETRANGE, TRUE, MAKELPARAM(32, 96));
    SendMessageW(thumbnail_slider, TBM_SETPOS, TRUE, state.composition_thumbnail_size);
    AddControl(state, 0, WC_STATICW, L"48px", SS_CENTER,
               424, 365, 52, 20, IdCompositionThumbnailSizeValue, compositions);
    HWND candidate_list = AddControl(
        state, WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        LVS_ICON | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_AUTOARRANGE | WS_TABSTOP,
        48, 387, 430, 177, IdCompositionCandidate, compositions);
    ListView_SetExtendedListViewStyle(
        candidate_list, LVS_EX_DOUBLEBUFFER | LVS_EX_BORDERSELECT);
    state.composition_candidate_image_list.reset(
        ImageList_Create(48, 48, ILC_COLOR32, 8, 8));
    ListView_SetImageList(
        candidate_list, state.composition_candidate_image_list.get(), LVSIL_NORMAL);
    ListView_SetIconSpacing(candidate_list, 66, 74);

    AddControl(state, 0, WC_STATICW, L"合成プレビュー", 0,
               500, 104, 100, 20, 0, compositions);
    AddControl(state, 0, WC_BUTTONW, L"ふりがなを表示",
               BS_AUTOCHECKBOX | WS_TABSTOP,
               600, 101, 118, 22, IdCompositionRubyEnabled, compositions);
    AddControl(state, 0, WC_STATICW, L"画像間隔(px)", 0,
               720, 104, 95, 20, 0, compositions);
    AddControl(state, WS_EX_CLIENTEDGE, WC_EDITW, L"0",
               ES_AUTOHSCROLL | WS_TABSTOP,
               817, 100, 78, 24, IdCompositionMargin, compositions);
    AddControl(state, WS_EX_CLIENTEDGE, WC_STATICW, L"", SS_OWNERDRAW,
               500, 126, 395, 86, IdCompositionPreview, compositions);
    AddControl(state, 0, WC_STATICW, L"名前", 0,
               500, 220, 100, 20, 0, compositions);
    AddControl(state, WS_EX_CLIENTEDGE, WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP,
               500, 242, 395, 24, IdCompositionName, compositions);
    AddControl(state, 0, WC_STATICW, L"置換文字列（1行に1つ）", 0,
               500, 274, 240, 20, 0, compositions);
    AddControl(state, WS_EX_CLIENTEDGE, WC_EDITW, L"",
               ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_TABSTOP,
               500, 296, 395, 66, IdCompositionAliases, compositions);
    AddControl(state, 0, WC_STATICW, L"合成する画像（上から順）", 0,
               500, 370, 240, 20, 0, compositions);
    AddControl(state, WS_EX_CLIENTEDGE, WC_LISTBOXW, L"",
               LBS_NOTIFY | WS_VSCROLL | WS_TABSTOP,
               500, 392, 300, 153, IdCompositionImages, compositions);
    AddControl(state, 0, WC_BUTTONW, L"削除", BS_PUSHBUTTON | WS_TABSTOP,
               808, 392, 87, 26, IdRemoveCompositionImage, compositions);
    AddControl(state, 0, WC_BUTTONW, L"上へ", BS_PUSHBUTTON | WS_TABSTOP,
               808, 426, 87, 26, IdMoveCompositionImageUp, compositions);
    AddControl(state, 0, WC_BUTTONW, L"下へ", BS_PUSHBUTTON | WS_TABSTOP,
               808, 460, 87, 26, IdMoveCompositionImageDown, compositions);

    AddControl(state, 0, WC_BUTTONW, L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP,
               735, 620, 95, 28, IdOk);
    AddControl(state, 0, WC_BUTTONW, L"キャンセル", BS_PUSHBUTTON | WS_TABSTOP,
               840, 620, 95, 28, IdCancel);

    ShowTab(state, 0);
}

void ShowSelectedTab(DialogState& state) {
    const int selection = static_cast<int>(
        SendMessageW(Item(state, IdTab), TCM_GETCURSEL, 0, 0));
    ShowTab(state, selection);
}

LRESULT WindowProcedureImpl(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    DialogState* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        state = static_cast<DialogState*>(create->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) {
        return DefWindowProcW(window, message, wparam, lparam);
    }

    switch (message) {
    case WM_CREATE:
        CreateControls(*state);
        ScanAndReconcile(state->global_working);
        if (LocalAvailable(*state)) {
            ScanAndReconcile(state->local_working, state->local_config_file.parent_path());
        }
        RefreshSources(*state);
        RefreshImages(*state);
        RefreshCompositions(*state);
        return 0;
    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(lparam) == Item(*state, IdCompositionThumbnailSize)) {
            const int size = static_cast<int>(SendMessageW(
                Item(*state, IdCompositionThumbnailSize), TBM_GETPOS, 0, 0));
            const std::wstring label = std::to_wstring(size) + L"px";
            SetWindowTextW(Item(*state, IdCompositionThumbnailSizeValue), label.c_str());
            if (LOWORD(wparam) != TB_THUMBTRACK &&
                size != state->composition_thumbnail_size) {
                state->composition_thumbnail_size = size;
                RefreshCompositionCandidates(*state);
            }
            return 0;
        }
        break;
    case WM_COMMAND: {
        const int id = LOWORD(wparam);
        const int notification = HIWORD(wparam);
        if (id == IdAddFolder && notification == BN_CLICKED) {
            AddSource(*state, SourceKind::Directory);
        } else if (id == IdAddFile && notification == BN_CLICKED) {
            AddSource(*state, SourceKind::File);
        } else if (id == IdRemoveSource && notification == BN_CLICKED) {
            const int index = static_cast<int>(SendMessageW(Item(*state, IdSourceList), LB_GETCURSEL, 0, 0));
            auto& sources = ActiveSources(*state);
            if (index >= 0 && static_cast<std::size_t>(index) < sources.size()) {
                sources.erase(sources.begin() + index);
                RefreshSources(*state);
            }
        } else if (id == IdSourceList && notification == LBN_SELCHANGE) {
            UpdateSourceSelection(*state);
        } else if (id == IdRecursive && notification == BN_CLICKED) {
            const int index = static_cast<int>(SendMessageW(Item(*state, IdSourceList), LB_GETCURSEL, 0, 0));
            auto& sources = ActiveSources(*state);
            if (index >= 0 && static_cast<std::size_t>(index) < sources.size() &&
                sources[static_cast<std::size_t>(index)].kind == SourceKind::Directory) {
                sources[static_cast<std::size_t>(index)].recursive =
                    SendMessageW(Item(*state, IdRecursive), BM_GETCHECK, 0, 0) == BST_CHECKED;
            }
        } else if (id == IdRescan && notification == BN_CLICKED) {
            Rescan(*state);
        } else if (id == IdRemoveImage && notification == BN_CLICKED) {
            RemoveSelectedImage(*state);
        } else if (id == IdOpenFolder && notification == BN_CLICKED) {
            OpenSelectedImageFolder(*state);
        } else if (id == IdRubyEnabled && notification == BN_CLICKED) {
            const bool enabled = SendMessageW(
                Item(*state, IdRubyEnabled), BM_GETCHECK, 0, 0) == BST_CHECKED;
            EnableWindow(Item(*state, IdRubyOverride), enabled);
            if (ImageEntry* image = ImageAtRow(*state, state->editing_image)) {
                image->ruby_enabled = enabled;
                const std::wstring summary = !enabled
                    ? L"（非表示）"
                    : (image->ruby_text_override.empty()
                        ? L"（置換文字列）" : image->ruby_text_override);
                ListView_SetItemText(
                    Item(*state, IdImageList), state->editing_image, 4,
                    const_cast<wchar_t*>(summary.c_str()));
            }
        } else if (id == IdOpenGlobalConfig && notification == BN_CLICKED) {
            OpenConfigFile(*state, state->global_config_file);
        } else if (id == IdOpenLocalConfig && notification == BN_CLICKED) {
            OpenConfigFile(*state, state->local_config_file);
        } else if (id == IdDefaultSizeMode && notification == CBN_SELCHANGE) {
            const int mode = static_cast<int>(
                SendMessageW(Item(*state, IdDefaultSizeMode), CB_GETCURSEL, 0, 0));
            EnableWindow(Item(*state, IdDefaultSizeValue), mode != 0);
        } else if (id == IdCompositionMargin && notification == EN_CHANGE) {
            double margin = 0.0;
            if (ImageComposition* composition = EditingComposition(*state);
                composition != nullptr &&
                ParseNumber(Item(*state, IdCompositionMargin), margin) &&
                margin >= 0.0 && margin <= 10000.0) {
                composition->image_margin = margin;
                if (state->editing_composition >= 0) {
                    const std::wstring margin_summary = FormatNumber(margin);
                    ListView_SetItemText(
                        Item(*state, IdCompositionList), state->editing_composition, 4,
                        const_cast<wchar_t*>(margin_summary.c_str()));
                }
                InvalidateRect(Item(*state, IdCompositionPreview), nullptr, TRUE);
            }
        } else if (id == IdCompositionRubyEnabled && notification == BN_CLICKED) {
            if (ImageComposition* composition = EditingComposition(*state)) {
                composition->ruby_enabled = SendMessageW(
                    Item(*state, IdCompositionRubyEnabled), BM_GETCHECK, 0, 0) == BST_CHECKED;
                ListView_SetItemText(
                    Item(*state, IdCompositionList), state->editing_composition, 5,
                    const_cast<wchar_t*>(composition->ruby_enabled ? L"表示" : L"非表示"));
            }
        } else if (id == IdAddComposition && notification == BN_CLICKED) {
            AddComposition(*state);
        } else if (id == IdRemoveComposition && notification == BN_CLICKED) {
            RemoveComposition(*state);
        } else if (id == IdRemoveCompositionImage && notification == BN_CLICKED) {
            RemoveCompositionImage(*state);
        } else if (id == IdMoveCompositionImageUp && notification == BN_CLICKED) {
            MoveCompositionImage(*state, -1);
        } else if (id == IdMoveCompositionImageDown && notification == BN_CLICKED) {
            MoveCompositionImage(*state, 1);
        } else if (id == IdCompositionImages && notification == LBN_SELCHANGE) {
            UpdateCompositionButtons(*state);
        } else if (id == IdOk && notification == BN_CLICKED) {
            SaveAndClose(*state);
        } else if (id == IdCancel && notification == BN_CLICKED) {
            if (state->dragging_composition_candidate) {
                CancelCompositionCandidateDrag(*state);
            } else {
                SendMessageW(window, WM_CLOSE, 0, 0);
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (state->dragging_composition_candidate) {
            if ((wparam & MK_LBUTTON) == 0) {
                CancelCompositionCandidateDrag(*state);
            } else {
                SetCursor(LoadCursorW(
                    nullptr, CursorOverCompositionImages(*state) ? IDC_HAND : IDC_NO));
            }
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (state->dragging_composition_candidate) {
            DropCompositionCandidate(*state);
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE && state->dragging_composition_candidate) {
            CancelCompositionCandidateDrag(*state);
            return 0;
        }
        break;
    case WM_CANCELMODE:
    case WM_CAPTURECHANGED:
        CancelCompositionCandidateDrag(*state);
        return 0;
    case WM_ACTIVATEAPP:
        if (wparam == FALSE) {
            CancelCompositionCandidateDrag(*state);
        }
        break;
    case WM_DRAWITEM: {
        const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (draw == nullptr) {
            return DefWindowProcW(window, message, wparam, lparam);
        }

        if (draw->CtlID == IdCompositionPreview) {
            FillRect(draw->hDC, &draw->rcItem, GetSysColorBrush(COLOR_WINDOW));
            const int count = static_cast<int>(state->composition_preview_bitmaps.size());
            if (count == 0) {
                RECT bounds = draw->rcItem;
                SetBkMode(draw->hDC, TRANSPARENT);
                SetTextColor(draw->hDC, GetSysColor(COLOR_GRAYTEXT));
                DrawTextW(draw->hDC, L"合成する画像を追加してください", -1, &bounds,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            }
            std::vector<SIZE> dimensions;
            dimensions.reserve(state->composition_preview_bitmaps.size());
            double source_width = 0.0;
            int source_height = 1;
            for (const auto& bitmap : state->composition_preview_bitmaps) {
                BITMAP info{};
                SIZE size{48, 48};
                if (bitmap && GetObjectW(bitmap.get(), sizeof(info), &info) != 0) {
                    size = {info.bmWidth, info.bmHeight};
                }
                dimensions.push_back(size);
                source_width += size.cx;
                source_height = std::max(source_height, static_cast<int>(size.cy));
            }
            const ImageComposition* composition = EditingComposition(*state);
            const double margin = composition == nullptr ? 0.0 : composition->image_margin;
            source_width += margin * std::max(0, count - 1);
            const int available_width = std::max(
                1, static_cast<int>(draw->rcItem.right - draw->rcItem.left - 4));
            const int available_height = std::max(
                1, static_cast<int>(draw->rcItem.bottom - draw->rcItem.top - 4));
            const double scale = std::min({
                1.0,
                static_cast<double>(available_width) / std::max(1.0, source_width),
                static_cast<double>(available_height) / source_height});
            const int drawn_width = std::max(1, static_cast<int>(std::lround(source_width * scale)));
            int x = draw->rcItem.left +
                ((draw->rcItem.right - draw->rcItem.left) - drawn_width) / 2;
            const int gap = std::max(0, static_cast<int>(std::lround(margin * scale)));
            SetStretchBltMode(draw->hDC, HALFTONE);
            for (int index = 0; index < count; ++index) {
                const SIZE source_size = dimensions[static_cast<std::size_t>(index)];
                const int width = std::max(1, static_cast<int>(std::lround(source_size.cx * scale)));
                const int height = std::max(1, static_cast<int>(std::lround(source_size.cy * scale)));
                const int y = draw->rcItem.top +
                    ((draw->rcItem.bottom - draw->rcItem.top) - height) / 2;
                const auto& bitmap = state->composition_preview_bitmaps[
                    static_cast<std::size_t>(index)];
                if (bitmap) {
                    HDC source = CreateCompatibleDC(draw->hDC);
                    if (source != nullptr) {
                        HGDIOBJ previous = SelectObject(source, bitmap.get());
                        StretchBlt(
                            draw->hDC, x, y, width, height,
                            source, 0, 0, source_size.cx, source_size.cy, SRCCOPY);
                        if (previous != nullptr && previous != HGDI_ERROR) {
                            SelectObject(source, previous);
                        }
                        DeleteDC(source);
                    }
                } else {
                    RECT missing_bounds{x, y, x + width, y + height};
                    DrawEdge(draw->hDC, &missing_bounds, EDGE_ETCHED, BF_RECT);
                    DrawTextW(draw->hDC, L"?", -1, &missing_bounds,
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
                x += width + gap;
            }
            return TRUE;
        }

        if (draw->CtlID != IdImagePreview) {
            return DefWindowProcW(window, message, wparam, lparam);
        }

        FillRect(draw->hDC, &draw->rcItem, GetSysColorBrush(COLOR_WINDOW));
        if (state->preview_bitmap) {
            BITMAP bitmap{};
            if (GetObjectW(state->preview_bitmap.get(), sizeof(bitmap), &bitmap) != 0) {
                HDC source = CreateCompatibleDC(draw->hDC);
                if (source != nullptr) {
                    HGDIOBJ previous = SelectObject(source, state->preview_bitmap.get());
                    const int x = draw->rcItem.left +
                        ((draw->rcItem.right - draw->rcItem.left) - bitmap.bmWidth) / 2;
                    const int y = draw->rcItem.top +
                        ((draw->rcItem.bottom - draw->rcItem.top) - bitmap.bmHeight) / 2;
                    BitBlt(
                        draw->hDC, x, y, bitmap.bmWidth, bitmap.bmHeight,
                        source, 0, 0, SRCCOPY);
                    if (previous != nullptr && previous != HGDI_ERROR) {
                        SelectObject(source, previous);
                    }
                    DeleteDC(source);
                }
            }
        } else {
            RECT text_bounds = draw->rcItem;
            SetBkMode(draw->hDC, TRANSPARENT);
            SetTextColor(draw->hDC, GetSysColor(COLOR_GRAYTEXT));
            DrawTextW(
                draw->hDC, L"プレビューなし", -1, &text_bounds,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        return TRUE;
    }
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<const NMHDR*>(lparam);
        if (header != nullptr && header->idFrom == IdTab && header->code == TCN_SELCHANGE) {
            CancelCompositionCandidateDrag(*state);
            ShowSelectedTab(*state);
        } else if (header != nullptr && header->idFrom == IdSourceTab &&
                   header->code == TCN_SELCHANGE) {
            int selection = TabCtrl_GetCurSel(Item(*state, IdSourceTab));
            if (selection == 1 && !LocalAvailable(*state)) {
                selection = 0;
                TabCtrl_SetCurSel(Item(*state, IdSourceTab), 0);
            }
            state->source_origin = selection == 1 ? ConfigOrigin::Local : ConfigOrigin::Global;
            RefreshSources(*state);
        } else if (header != nullptr && header->idFrom == IdCompositionTab &&
                   header->code == TCN_SELCHANGE) {
            CancelCompositionCandidateDrag(*state);
            int selection = TabCtrl_GetCurSel(Item(*state, IdCompositionTab));
            const int previous = state->composition_origin == ConfigOrigin::Local ? 1 : 0;
            if ((selection == 1 && !LocalAvailable(*state)) ||
                !CommitCompositionEditor(*state, true)) {
                selection = previous;
                TabCtrl_SetCurSel(Item(*state, IdCompositionTab), selection);
            } else {
                state->composition_origin = selection == 1
                    ? ConfigOrigin::Local : ConfigOrigin::Global;
                RefreshCompositions(*state);
            }
        } else if (header != nullptr && header->idFrom == IdImageList &&
                   header->code == LVN_ITEMCHANGED && !state->refreshing_image_list) {
            const auto* changed = reinterpret_cast<const NMLISTVIEW*>(lparam);
            if (changed->iItem >= 0 &&
                static_cast<std::size_t>(changed->iItem) < state->image_rows.size() &&
                (changed->uChanged & LVIF_STATE) != 0) {
                const UINT old_check = changed->uOldState & LVIS_STATEIMAGEMASK;
                const UINT new_check = changed->uNewState & LVIS_STATEIMAGEMASK;
                if (old_check != new_check && new_check != 0) {
                    if (ImageEntry* image = ImageAtRow(*state, changed->iItem)) {
                        image->enabled =
                            ListView_GetCheckState(Item(*state, IdImageList), changed->iItem) != FALSE;
                        RefreshCompositionCandidates(*state);
                        RefreshCompositionImages(*state);
                    }
                }
                const bool became_selected =
                    (changed->uOldState & LVIS_SELECTED) == 0 &&
                    (changed->uNewState & LVIS_SELECTED) != 0;
                if (became_selected && changed->iItem != state->editing_image) {
                    const int old = state->editing_image;
                    if (!CommitImageEditor(*state, true)) {
                        state->refreshing_image_list = true;
                        ListView_SetItemState(
                            Item(*state, IdImageList), changed->iItem, 0, LVIS_SELECTED | LVIS_FOCUSED);
                        if (old >= 0) {
                            ListView_SetItemState(
                                Item(*state, IdImageList), old, LVIS_SELECTED | LVIS_FOCUSED,
                                LVIS_SELECTED | LVIS_FOCUSED);
                        }
                        state->refreshing_image_list = false;
                    } else {
                        LoadImageEditor(*state, changed->iItem);
                    }
                }
            }
        } else if (header != nullptr && header->idFrom == IdCompositionCandidate &&
                   header->code == LVN_BEGINDRAG) {
            const auto* drag = reinterpret_cast<const NMLISTVIEW*>(lparam);
            BeginCompositionCandidateDrag(*state, drag->iItem);
        } else if (header != nullptr && header->idFrom == IdCompositionCandidate &&
                   header->code == NM_DBLCLK) {
            AddCompositionImage(*state);
        } else if (header != nullptr && header->idFrom == IdCompositionCandidate &&
                   header->code == LVN_ITEMCHANGED) {
            UpdateCompositionButtons(*state);
        } else if (header != nullptr && header->idFrom == IdCompositionList &&
                   header->code == LVN_ITEMCHANGED && !state->refreshing_composition_list) {
            const auto* changed = reinterpret_cast<const NMLISTVIEW*>(lparam);
            auto& compositions = ActiveCompositions(*state);
            if (changed->iItem >= 0 &&
                static_cast<std::size_t>(changed->iItem) < compositions.size() &&
                (changed->uChanged & LVIF_STATE) != 0) {
                const UINT old_check = changed->uOldState & LVIS_STATEIMAGEMASK;
                const UINT new_check = changed->uNewState & LVIS_STATEIMAGEMASK;
                if (old_check != new_check && new_check != 0) {
                    compositions[static_cast<std::size_t>(changed->iItem)].enabled =
                        ListView_GetCheckState(
                            Item(*state, IdCompositionList), changed->iItem) != FALSE;
                }
                const bool became_selected =
                    (changed->uOldState & LVIS_SELECTED) == 0 &&
                    (changed->uNewState & LVIS_SELECTED) != 0;
                if (became_selected && changed->iItem != state->editing_composition) {
                    const int old = state->editing_composition;
                    if (!CommitCompositionEditor(*state, true)) {
                        state->refreshing_composition_list = true;
                        ListView_SetItemState(
                            Item(*state, IdCompositionList), changed->iItem, 0,
                            LVIS_SELECTED | LVIS_FOCUSED);
                        if (old >= 0) {
                            ListView_SetItemState(
                                Item(*state, IdCompositionList), old,
                                LVIS_SELECTED | LVIS_FOCUSED,
                                LVIS_SELECTED | LVIS_FOCUSED);
                        }
                        state->refreshing_composition_list = false;
                    } else {
                        LoadCompositionEditor(*state, changed->iItem);
                    }
                }
            }
        }
        return 0;
    }
    case WM_CLOSE:
        CancelCompositionCandidateDrag(*state);
        state->finished = true;
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        CancelCompositionCandidateDrag(*state);
        state->finished = true;
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return DefWindowProcW(window, message, wparam, lparam);
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK WindowProcedure(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    try {
        return WindowProcedureImpl(window, message, wparam, lparam);
    } catch (...) {
        OutputDebugStringW(L"mojie: 設定画面の処理中に予期しないエラーが発生しました。\n");
        auto* state = reinterpret_cast<DialogState*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (state != nullptr) {
            state->accepted = false;
            state->finished = true;
        }
        if (message == WM_NCCREATE) {
            return FALSE;
        }
        if (message == WM_CREATE) {
            return -1;
        }
        if (message != WM_NCDESTROY && IsWindow(window)) {
            DestroyWindow(window);
        }
        return 0;
    }
}

bool RegisterDialogClass(HINSTANCE instance) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = WindowProcedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = kWindowClass;
    return RegisterClassExW(&window_class) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

} // namespace

bool ShowConfigDialog(
    HWND owner,
    HINSTANCE instance,
    GlobalConfig& global_config,
    LocalConfig& local_config,
    const fs::path& global_config_file,
    const fs::path& local_config_file,
    bool local_config_editable,
    const fs::path& app_data_path) {
    INITCOMMONCONTROLSEX common_controls{};
    common_controls.dwSize = sizeof(common_controls);
    common_controls.dwICC =
        ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES |
        ICC_WIN95_CLASSES;
    if (!InitCommonControlsEx(&common_controls)) {
        return false;
    }
    if (!RegisterDialogClass(instance)) {
        return false;
    }
    DialogState state;
    state.owner = owner;
    state.instance = instance;
    state.global_working = global_config;
    state.local_working = local_config;
    state.global_output = &global_config;
    state.local_output = &local_config;
    state.global_config_file = global_config_file;
    state.local_config_file = local_config_file;
    state.local_config_editable = local_config_editable;
    state.global_config_signature = CaptureFileSignature(global_config_file);
    state.local_config_signature = CaptureFileSignature(local_config_file);
    state.app_data_path = app_data_path;

    HWND window = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
        kWindowClass, L"mojie 設定",
        WS_CAPTION | WS_SYSMENU | WS_POPUP,
        CW_USEDEFAULT, CW_USEDEFAULT, 965, 700,
        owner, nullptr, instance, &state);
    if (window == nullptr) {
        return false;
    }
    RECT bounds{};
    GetWindowRect(window, &bounds);
    RECT owner_bounds{};
    if (owner != nullptr && GetWindowRect(owner, &owner_bounds)) {
        SetWindowPos(
            window, nullptr,
            owner_bounds.left + ((owner_bounds.right - owner_bounds.left) - (bounds.right - bounds.left)) / 2,
            owner_bounds.top + ((owner_bounds.bottom - owner_bounds.top) - (bounds.bottom - bounds.top)) / 2,
            0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }

    const BOOL owner_was_enabled = owner != nullptr ? IsWindowEnabled(owner) : FALSE;
    if (owner_was_enabled) {
        EnableWindow(owner, FALSE);
    }
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    MSG message{};
    BOOL message_status = TRUE;
    while (!state.finished && (message_status = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (!state.finished && IsWindow(window)) {
        // GetMessage() returned WM_QUIT or failed. Destroy the HWND while the
        // stack-owned DialogState is still alive so no later callback can use it.
        DestroyWindow(window);
    }
    if (owner_was_enabled && IsWindow(owner)) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    if (message_status == 0) {
        PostQuitMessage(static_cast<int>(message.wParam));
    }
    return state.accepted;
}

} // namespace mojie
