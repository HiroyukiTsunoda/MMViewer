#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "viewer.h"
#include "diagram.h"
#include "mermaid_renderer.h"
#include "md4c.h"
#include <windowsx.h>
#include <uxtheme.h>
#include <gdiplus.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <list>
#include <map>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mm {
namespace {
using namespace Gdiplus;
std::wstring Wide(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (!n) n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring result(n, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), result.data(), n);
    return result;
}
std::wstring Lower(std::wstring value) {
    for (auto& c : value) c = static_cast<wchar_t>(std::towlower(c));
    return value;
}
std::wstring Entity(std::string_view value) {
    if (value.size() > 3 && value[0] == '&' && value[1] == '#') {
        try {
            size_t consumed = 0;
            auto body = std::string(value.substr(2, value.size() - 3));
            int base = 10;
            if (!body.empty() && (body[0] == 'x' || body[0] == 'X')) { base = 16; body.erase(0, 1); }
            auto cp = std::stoul(body, &consumed, base);
            if (consumed == body.size() && cp > 0 && cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff)) {
                if (cp <= 0xffff) return std::wstring(1, static_cast<wchar_t>(cp));
                cp -= 0x10000;
                return { static_cast<wchar_t>(0xd800 + (cp >> 10)), static_cast<wchar_t>(0xdc00 + (cp & 1023)) };
            }
        } catch (...) { }
    }
    static const std::map<std::string_view, std::wstring> known = {
        {"&amp;", L"&"}, {"&lt;", L"<"}, {"&gt;", L">"}, {"&quot;", L"\""},
        {"&apos;", L"'"}, {"&nbsp;", L"\u00a0"}, {"&copy;", L"\u00a9"}, {"&reg;", L"\u00ae"},
        {"&hellip;", L"\u2026"}, {"&mdash;", L"\u2014"}, {"&ndash;", L"\u2013"},
        {"&rarr;", L"\u2192"}, {"&larr;", L"\u2190"}, {"&times;", L"\u00d7"}
    };
    auto found = known.find(value);
    return found == known.end() ? Wide(value) : found->second;
}
std::wstring Anchor(std::wstring text) {
    text = Lower(std::move(text));
    std::wstring result;
    for (wchar_t c : text) {
        if (std::iswspace(c)) result += L'-';
        else if (c == L'-' || c == L'_' || std::iswalnum(c) || c >= 128) result += c;
    }
    return result;
}
std::wstring DecodeReference(std::wstring value) {
    // Percent escaping is normally UTF-8. Decode consecutive escaped bytes together.
    auto hex = [](wchar_t c) { if (c >= L'0' && c <= L'9') return c - L'0'; if (c >= L'a' && c <= L'f') return c - L'a' + 10; if (c >= L'A' && c <= L'F') return c - L'A' + 10; return -1; };
    std::wstring output;
    for (size_t i = 0; i < value.size();) {
        if (value[i] == L'%' && i + 2 < value.size() && hex(value[i + 1]) >= 0 && hex(value[i + 2]) >= 0) {
            std::string bytes;
            while (i + 2 < value.size() && value[i] == L'%' && hex(value[i + 1]) >= 0 && hex(value[i + 2]) >= 0) {
                bytes += static_cast<char>(hex(value[i + 1]) * 16 + hex(value[i + 2])); i += 3;
            }
            output += Wide(bytes);
        } else output += value[i++];
    }
    return output;
}
bool IsLocalRegularImage(const std::filesystem::path& path) {
    const auto native = path.native();
    if (native.empty() || native.starts_with(L"\\\\") || native.starts_with(L"//")) return false;
    auto root = path.root_path().native();
    if (root.empty() || GetDriveTypeW(root.c_str()) == DRIVE_REMOTE) return false;
    std::filesystem::path part;
    for (const auto& component : path) {
        part /= component;
        if (!part.has_root_directory()) continue;
        auto attributes = GetFileAttributesW(part.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    }
    auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    return !ec && size > 0 && size <= 20u * 1024u * 1024u;
}
std::unique_ptr<Bitmap> OpenLocalImage(const std::filesystem::path& file) {
    // The path can be replaced while its viewport raster is not retained.
    // Apply the same file and decoded-size limits to every decoder lifetime.
    if (!IsLocalRegularImage(file)) return {};
    auto extension = Lower(file.extension().native());
    if (extension != L".png" && extension != L".jpg" && extension != L".jpeg" && extension != L".gif" && extension != L".bmp") return {};
    auto bitmap = std::make_unique<Bitmap>(file.c_str(), FALSE);
    if (bitmap->GetLastStatus() != Ok) return {};
    UINT width = bitmap->GetWidth(), height = bitmap->GetHeight();
    if (!width || !height || width > 16384 || height > 16384 || static_cast<uint64_t>(width) * height > 40000000) return {};
    return bitmap;
}
void CopyText(HWND hwnd, const std::wstring& text) {
    if (!OpenClipboard(hwnd)) return;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (memory) {
        if (void* p = GlobalLock(memory)) {
            memcpy(p, text.c_str(), (text.size() + 1) * sizeof(wchar_t)); GlobalUnlock(memory);
            EmptyClipboard(); if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
        } else GlobalFree(memory);
    }
    CloseClipboard();
}
}

struct NativeDocumentView::Impl {
    enum class Kind { Paragraph, Heading, Quote, Code, Rule, Table, Image, Diagram };
    enum Style : unsigned { Bold = 1, Italic = 2, Code = 4, Strike = 8, Keyword = 16, String = 32, Comment = 64, Number = 128 };
    struct Run { std::wstring text, link; unsigned style = 0; size_t offset = 0; };
    struct Piece { std::wstring text, link; unsigned style = 0; size_t offset = 0; float x = 0, y = 0, width = 0, height = 0, size = 16; };
    struct Cell { std::vector<Run> runs; bool heading = false; };
    struct Block {
        Kind kind = Kind::Paragraph;
        std::vector<Run> runs;
        std::vector<std::vector<Cell>> rows;
        std::vector<float> rowEdges;
        std::vector<size_t> rowPieceStarts; // First piece of each table row, in rowEdges order.
        std::vector<Piece> pieces;
        std::wstring anchor, reference, alternate, language;
        std::wstring imagePath;
        std::string diagramSource;
        std::shared_ptr<Diagram> diagram;
        std::shared_ptr<MermaidImage> officialDiagram;
        std::unique_ptr<Bitmap> image;
        int heading = 0, quote = 0, indent = 0;
        float x = 0, y = 0, width = 0, height = 0, imageWidth = 0, imageHeight = 0;
        bool imageAttempted = false, imageValid = false, markerOnly = false;
    };
    struct CachedDocument {
        std::string source;
        std::wstring path, plain, query;
        std::vector<Block> blocks;
        std::vector<std::pair<size_t, size_t>> matches;
        int fontSize = 16, layoutWidth = -1;
        bool sourceMode = false, wide = false, dirty = true, laidOut = false;
        float contentHeight = 0, contentWidth = 0, dpi = 1;
        size_t bytes = 0, visibleBegin = 0, visibleEnd = 0;
    };
    // This bounds retained allocation estimates, excluding allocator bookkeeping.
    static constexpr size_t CacheBytes = 64 * 1024 * 1024, CacheCount = 8;
    std::list<CachedDocument> documentCache;
    size_t cacheBytes = 0, parseCount = 0, layoutCount = 0, cacheHits = 0, bufferAllocations = 0;
    bool cacheable = true;
    int laidOutFont = 16;
    bool laidOutWide = false;
    HDC backDC = nullptr;
    HBITMAP backBitmap = nullptr;
    HGDIOBJ backOriginal = nullptr;
    int backWidth = 0, backHeight = 0;
    size_t visibleBegin = 0, visibleEnd = 0;
    NativeDocumentView* owner;
    std::unique_ptr<MermaidRenderer> mermaid;
    bool refreshMermaidTheme = false;
    std::shared_ptr<int> mermaidLifetime=std::make_shared<int>(0);
    HWND hwnd = nullptr;
    std::string source;
    std::wstring path, plain, query;
    // Lowercased body for repeated case-insensitive searches. Built on the
    // first search after the body changes, released with the query, never cached.
    std::wstring plainLower;
    bool plainLowered = false;
    std::vector<Block> blocks;
    std::vector<std::pair<size_t, size_t>> matches;
    int activeMatch = -1, fontSize = 16, vertical = 0, horizontal = 0;
    int wheelRemainder = 0, zoomRemainder = 0;
    size_t selectionAnchor = 0, selectionFocus = 0;
    bool selecting = false;
    bool dark = false, sourceMode = false, wide = false, dirty = true, laidOut = false;
    bool scrollbarsDirty = true;
    float contentHeight = 0, contentWidth = 0, dpi = 1;
    int layoutWidth = -1;
    double pendingRatio = -1;
    std::wstring pendingAnchor;
    Color background{255, 255, 255, 255}, foreground{255, 38, 43, 52}, muted{255, 100, 112, 128};
    Color panel{255, 245, 247, 250}, border{255, 218, 224, 231}, linkColor{255, 38, 104, 194};
    std::map<unsigned long, std::unique_ptr<Font>> fonts;
    std::unordered_map<uint64_t, float> glyphWidths;
    struct WordKey { std::wstring text; float size = 0; unsigned style = 0; bool operator==(const WordKey&) const = default; };
    struct WordKeyHash {
        size_t operator()(const WordKey& key) const {
            size_t hash = std::hash<std::wstring>{}(key.text);
            hash ^= std::hash<float>{}(key.size) + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
            hash ^= key.style + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
            return hash;
        }
    };
    std::unordered_map<WordKey, float, WordKeyHash> wordWidths;
    bool liveResize = false;
    std::unique_ptr<StringFormat> textFormat;
    size_t current = static_cast<size_t>(-1);
    int quoteDepth = 0, listDepth = 0;
    std::vector<int> listNumbers;
    unsigned spanStyle = 0;
    std::vector<unsigned> styleStack;
    std::vector<std::wstring> linkStack;
    std::wstring currentLink;
    bool inCode = false, inTable = false, inImage = false;
    std::wstring imageReference, imageAlternate;

    explicit Impl(NativeDocumentView* view) : owner(view) {}
    ~Impl() { mermaidLifetime.reset();mermaid.reset();ReleaseBuffer(); }
    bool RenderedDiagram(const Block& block) const {
        return block.officialDiagram ? block.officialDiagram->error.empty() : block.diagram && block.diagram->valid;
    }
    void RequestDiagram(Block& block) {
        if(!mermaid||block.kind!=Kind::Diagram)return;
        if(block.officialDiagram&&block.officialDiagram->dark==dark)return;
        std::weak_ptr<int> lifetime=mermaidLifetime;
        block.officialDiagram=mermaid->Render(block.diagramSource,dark,[this,lifetime]{
            if(lifetime.expired()||!hwnd)return;
            PostMessageW(hwnd,WM_APP+52,0,0);
        });
        dirty=true;
    }
    void ReindexText() {
        plain.clear();plainLower.clear();plainLowered=false;
        auto add=[&](auto& runs){for(auto& run:runs){run.offset=plain.size();plain+=run.text;}plain+=L'\n';};
        for(auto& block:blocks){if(block.kind!=Kind::Diagram||!RenderedDiagram(block))add(block.runs);for(auto& row:block.rows)for(auto& cell:row)add(cell.runs);}
        selectionAnchor=selectionFocus=0;FindMatches();
    }
    void ReleaseBuffer() {
        if (backDC && backOriginal) SelectObject(backDC, backOriginal);
        if (backBitmap) DeleteObject(backBitmap);
        if (backDC) DeleteDC(backDC);
        backDC = nullptr; backBitmap = nullptr; backOriginal = nullptr;
        backWidth = backHeight = 0;
    }
    bool EnsureBuffer(HDC dc, int width, int height) {
        if (backDC && backWidth == width && backHeight == height) return true;
        ReleaseBuffer();
        backDC = CreateCompatibleDC(dc);
        if (backDC) backBitmap = CreateCompatibleBitmap(dc, width, height);
        if (!backBitmap) { ReleaseBuffer(); return false; }
        backOriginal = SelectObject(backDC, backBitmap);
        if (!backOriginal || backOriginal == HGDI_ERROR) { backOriginal = nullptr; ReleaseBuffer(); return false; }
        backWidth = width; backHeight = height; ++bufferAllocations; return true;
    }
    void ReleaseImages() {
        for (size_t i = visibleBegin; i < std::min(visibleEnd, blocks.size()); ++i) blocks[i].image.reset();
        visibleBegin = visibleEnd = 0;
    }
    size_t RetainedBytes() const {
        size_t bytes = sizeof(CachedDocument) + source.capacity() + 1 +
            (path.capacity() + plain.capacity() + query.capacity() + 3) * sizeof(wchar_t) + blocks.capacity() * sizeof(Block) +
            matches.capacity() * sizeof(std::pair<size_t, size_t>);
        auto strings = [](const auto& value) { return (value.capacity() + 1) * sizeof(typename std::decay_t<decltype(value)>::value_type); };
        auto runsBytes = [&](const auto& runs) {
            size_t total = runs.capacity() * sizeof(Run);
            for (const auto& run : runs) total += strings(run.text) + strings(run.link);
            return total;
        };
        for (const auto& block : blocks) {
            bytes += runsBytes(block.runs) + block.rows.capacity() * sizeof(std::vector<Cell>) +
                block.rowEdges.capacity() * sizeof(float) + block.rowPieceStarts.capacity() * sizeof(size_t) + block.pieces.capacity() * sizeof(Piece);
            for (const auto& row : block.rows) {
                bytes += row.capacity() * sizeof(Cell);
                for (const auto& cell : row) bytes += runsBytes(cell.runs);
            }
            for (const auto& piece : block.pieces) bytes += strings(piece.text) + strings(piece.link);
            bytes += strings(block.anchor) + strings(block.reference) + strings(block.alternate) +
                strings(block.language) + strings(block.imagePath) + strings(block.diagramSource);
            if (block.diagram) bytes += DiagramMemoryUsage(*block.diagram);
            if(block.officialDiagram){bytes+=sizeof(MermaidImage)+strings(block.officialDiagram->error);if(auto image=block.officialDiagram->bitmap)bytes+=size_t(image->GetWidth())*image->GetHeight()*4;}
            if (block.image) bytes += sizeof(Bitmap) + static_cast<size_t>(block.image->GetWidth()) * block.image->GetHeight() * 4;
        }
        return bytes;
    }
    void RemoveCached(const std::wstring& documentPath) {
        for (auto it = documentCache.begin(); it != documentCache.end();) {
            if (it->path == documentPath) { cacheBytes -= it->bytes; it = documentCache.erase(it); }
            else ++it;
        }
    }
    void SaveCached() {
        if (!cacheable || path.empty() || !laidOut) return;
        if(std::any_of(blocks.begin(),blocks.end(),[](const auto& block){return block.officialDiagram&&!block.officialDiagram->complete;}))return;
        auto bytes = RetainedBytes();
        RemoveCached(path);
        // Prefer retaining the parsed text if a large raster would exceed the
        // budget. Ordinary visible images travel with the cached document.
        if (bytes > CacheBytes) { ReleaseImages(); bytes = RetainedBytes(); }
        if (bytes > CacheBytes) return;
        while (!documentCache.empty() && (documentCache.size() >= CacheCount || cacheBytes + bytes > CacheBytes)) {
            cacheBytes -= documentCache.back().bytes; documentCache.pop_back();
        }
        CachedDocument cached;
        cached.source = std::move(source); cached.path = std::move(path); cached.plain = std::move(plain); cached.blocks = std::move(blocks);
        cached.query = query; cached.matches = std::move(matches);
        cached.fontSize = laidOutFont; cached.sourceMode = sourceMode; cached.wide = laidOutWide;
        cached.layoutWidth = layoutWidth; cached.dpi = dpi; cached.contentWidth = contentWidth; cached.contentHeight = contentHeight;
        cached.dirty = dirty; cached.laidOut = laidOut; cached.bytes = bytes;
        cached.visibleBegin = visibleBegin; cached.visibleEnd = visibleEnd;
        visibleBegin = visibleEnd = 0;
        documentCache.push_front(std::move(cached)); cacheBytes += bytes;
    }
    void ReplaceDocument(const std::string& documentSource, const std::wstring& documentPath, int pixels, bool mode) {
        pixels = std::clamp(pixels, 10, 40);
        if (cacheable && source == documentSource && path == documentPath && sourceMode == mode) {
            owner->SetFontSize(pixels); return;
        }
        // Extract the destination before storing the departing tab, so capacity
        // eviction cannot discard the very document being selected.
        auto found = std::find_if(documentCache.begin(), documentCache.end(), [&](const auto& item) {
            return item.path == documentPath && item.sourceMode == mode && item.source == documentSource;
        });
        std::unique_ptr<CachedDocument> restored;
        if (found != documentCache.end()) {
            cacheBytes -= found->bytes; restored = std::make_unique<CachedDocument>(std::move(*found)); documentCache.erase(found);
        }
        SaveCached();
        fontSize = pixels; sourceMode = mode; cacheable = true;
        vertical = horizontal = 0; pendingRatio = -1; pendingAnchor.clear(); scrollbarsDirty = true;
        selectionAnchor = selectionFocus = 0;
        if (selecting && hwnd && GetCapture() == hwnd) ReleaseCapture();
        selecting = false;
        if (restored) {
            refreshMermaidTheme = true;
            source = std::move(restored->source); path = std::move(restored->path); plain = std::move(restored->plain); blocks = std::move(restored->blocks);
            std::wstring().swap(plainLower); plainLowered = false; // The lowercase copy belonged to the departing document.
            layoutWidth = restored->layoutWidth; dpi = restored->dpi; contentWidth = restored->contentWidth; contentHeight = restored->contentHeight;
            laidOutFont = restored->fontSize; laidOutWide = restored->wide;
            visibleBegin = restored->visibleBegin; visibleEnd = restored->visibleEnd;
            laidOut = restored->laidOut; dirty = restored->dirty || fontSize != laidOutFont || wide != laidOutWide;
            ++cacheHits;
            if (restored->query == query) { matches = std::move(restored->matches); activeMatch = -1; }
            else FindMatches();
        } else {
            std::string(documentSource).swap(source); path = documentPath; Parse();
        }
        if (hwnd) InvalidateRect(hwnd, nullptr, FALSE);
    }
    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        auto self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wp, lp);
        switch (message) {
        case WM_APP+52:
            self->pendingRatio=self->owner->ScrollRatio();self->ReindexText();self->dirty=true;
            InvalidateRect(window,nullptr,FALSE);return 0;
        case WM_PAINT: self->Paint(); return 0;
        case WM_PRINTCLIENT: self->Paint(reinterpret_cast<HDC>(wp)); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_SIZE:
            if (!LOWORD(lp) || !HIWORD(lp)) self->ReleaseBuffer();
            self->scrollbarsDirty = true;
            InvalidateRect(window, nullptr, FALSE); return 0;
        case WM_DPICHANGED_AFTERPARENT: self->dirty = true; InvalidateRect(window, nullptr, FALSE); return 0;
        case WM_VSCROLL: self->ScrollBar(true, LOWORD(wp)); return 0;
        case WM_HSCROLL: self->ScrollBar(false, LOWORD(wp)); return 0;
        case WM_MOUSEWHEEL: {
            if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) {
                self->zoomRemainder += GET_WHEEL_DELTA_WPARAM(wp);
                int delta = self->zoomRemainder / WHEEL_DELTA; self->zoomRemainder %= WHEEL_DELTA;
                if (delta && self->owner->onZoom) self->owner->onZoom(delta);
            } else {
                self->wheelRemainder += GET_WHEEL_DELTA_WPARAM(wp);
                int delta = self->wheelRemainder / WHEEL_DELTA; self->wheelRemainder %= WHEEL_DELTA;
                if (delta) self->ScrollBy((GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) ? -delta * 64 : 0, (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) ? 0 : -delta * static_cast<int>(self->fontSize * self->dpi * 4));
            }
            return 0;
        }
        case WM_MOUSEHWHEEL: self->ScrollBy(GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * 64, 0); return 0;
        case WM_LBUTTONDOWN:
            SetFocus(window); self->selectionAnchor = self->selectionFocus = self->HitOffset(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            self->selecting = true; SetCapture(window); InvalidateRect(window, nullptr, FALSE); return 0;
        case WM_MOUSEMOVE:
            if (self->selecting) {
                RECT area{}; GetClientRect(window, &area);
                int y = GET_Y_LPARAM(lp);
                if (y < 0) self->ScrollBy(0, -24); else if (y > area.bottom) self->ScrollBy(0, 24);
                self->selectionFocus = self->HitOffset(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP: {
            if (self->selecting) { self->selectionFocus = self->HitOffset(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); self->selecting = false; ReleaseCapture(); }
            auto target = self->HitLink(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (!target.empty() && self->selectionAnchor == self->selectionFocus) {
                if (target.front() == L'#') self->owner->ScrollToAnchor(target.substr(1));
                else if (self->owner->onLink) self->owner->onLink(target);
            }
            return 0;
        }
        case WM_CAPTURECHANGED: self->selecting = false; return 0;
        case WM_SETCURSOR: {
            POINT point; GetCursorPos(&point); ScreenToClient(window, &point);
            SetCursor(LoadCursorW(nullptr, self->HitLink(point.x, point.y).empty() ? IDC_ARROW : IDC_HAND)); return TRUE;
        }
        case WM_KEYDOWN:
            if ((GetKeyState(VK_CONTROL) & 0x8000) && wp == 'C') { self->CopySelection(); return 0; }
            if ((GetKeyState(VK_CONTROL) & 0x8000) && wp == 'A') { self->selectionAnchor = 0; self->selectionFocus = self->plain.size(); InvalidateRect(window, nullptr, FALSE); return 0; }
            if (wp == VK_DOWN) { self->ScrollBy(0, 40); return 0; }
            if (wp == VK_UP) { self->ScrollBy(0, -40); return 0; }
            if (wp == VK_HOME || wp == VK_END || wp == VK_NEXT || wp == VK_PRIOR) { self->Navigate(wp); return 0; }
            break;
        case WM_CONTEXTMENU: {
            HMENU menu = CreatePopupMenu(); AppendMenuW(menu, MF_STRING, 1, self->selectionAnchor == self->selectionFocus ? L"Markdown をコピー" : L"選択範囲をコピー");
            POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; if (p.x == -1 && p.y == -1) { p = {16,16}; ClientToScreen(window, &p); }
            int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, window, nullptr);
            DestroyMenu(menu); if (command == 1) self->CopySelection(); return 0;
        }
        case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS;
        case WM_NCDESTROY: self->hwnd = nullptr; SetWindowLongPtrW(window, GWLP_USERDATA, 0); break;
        }
        return DefWindowProcW(window, message, wp, lp);
    }
    Font& GetFont(float size, unsigned style) {
        // Keyed to 0.01 px: the caches outlive a zoom, and a table's 0.94 scaled
        // body at one font size must not stand in for the body at another.
        unsigned long key = static_cast<unsigned long>(size * 100) * 16 + (style & 15);
        auto found = fonts.find(key);
        if (found == fonts.end()) {
            // Zooming adds sizes without discarding the others; a full cache
            // starts over. Callers finish with a font before asking for the next.
            if (fonts.size() >= 64) fonts.clear();
            found = fonts.emplace(key, nullptr).first;
            auto& font = found->second;
            int flags = FontStyleRegular;
            if (style & Bold) flags |= FontStyleBold;
            if (style & Italic) flags |= FontStyleItalic;
            if (style & Strike) flags |= FontStyleStrikeout;
            font = std::make_unique<Font>((style & Code) ? L"Consolas" : L"Yu Gothic UI", size, flags, UnitPixel);
            if (font->GetLastStatus() != Ok) font = std::make_unique<Font>(L"Segoe UI", size, flags, UnitPixel);
        }
        return *found->second;
    }
    StringFormat& Format() {
        if (!textFormat) {
            textFormat = std::make_unique<StringFormat>(StringFormat::GenericTypographic());
            textFormat->SetFormatFlags(StringFormatFlagsMeasureTrailingSpaces | StringFormatFlagsNoWrap | StringFormatFlagsNoClip);
        }
        return *textFormat;
    }
    float Measure(Graphics& g, const std::wstring& text, float size, unsigned style) {
        if (text.empty()) return 0;
        // Single characters and short words repeat throughout a document. Both
        // caches key on the exact text, size and font style, so they survive a
        // zoom; a full cache starts over instead of measuring everything again.
        uint64_t glyphKey = 0; WordKey wordKey;
        if (text.size() == 1) {
            glyphKey = (static_cast<uint64_t>(size * 100) << 20) | (static_cast<uint64_t>(style & 15) << 16) | static_cast<unsigned short>(text[0]);
            auto cached = glyphWidths.find(glyphKey);
            if (cached != glyphWidths.end()) return cached->second;
        } else if (text.size() <= 16) {
            wordKey = {text, size, style & 15};
            auto cached = wordWidths.find(wordKey);
            if (cached != wordWidths.end()) return cached->second;
        }
        RectF bounds; g.MeasureString(text.c_str(), static_cast<INT>(text.size()), &GetFont(size, style), PointF(0, 0), &Format(), &bounds);
        float width = std::max(0.0f, bounds.Width);
        if (text.size() == 1) { if (glyphWidths.size() >= 32768) glyphWidths.clear(); glyphWidths[glyphKey] = width; }
        else if (text.size() <= 16) { if (wordWidths.size() >= 32768) wordWidths.clear(); wordWidths.emplace(std::move(wordKey), width); }
        return width;
    }
    Block& Add(Kind kind) {
        blocks.emplace_back(); auto& block = blocks.back(); block.kind = kind; block.quote = quoteDepth; block.indent = listDepth;
        current = blocks.size() - 1; return block;
    }
    std::vector<Run>& Runs() {
        if (current >= blocks.size()) Add(Kind::Paragraph);
        auto& block = blocks[current];
        if (inTable) {
            if (block.rows.empty()) block.rows.emplace_back();
            if (block.rows.back().empty()) block.rows.back().emplace_back();
            return block.rows.back().back().runs;
        }
        return block.runs;
    }
    void AddText(std::wstring text) {
        if (inImage) { imageAlternate += text; return; }
        auto& runs = Runs();
        if (!runs.empty() && runs.back().style == spanStyle && runs.back().link == currentLink) runs.back().text += text;
        else runs.push_back({std::move(text), currentLink, spanStyle});
    }
    static int EnterBlock(MD_BLOCKTYPE type, void* detail, void* userdata) {
        auto& s = *static_cast<Impl*>(userdata);
        switch (type) {
        case MD_BLOCK_H: s.Add(Kind::Heading).heading = static_cast<MD_BLOCK_H_DETAIL*>(detail)->level; break;
        case MD_BLOCK_P:
            if (s.current >= s.blocks.size() || (!s.blocks[s.current].runs.empty() && !s.blocks[s.current].markerOnly)) s.Add(s.quoteDepth ? Kind::Quote : Kind::Paragraph);
            s.blocks[s.current].markerOnly = false; break;
        case MD_BLOCK_QUOTE: ++s.quoteDepth; break;
        case MD_BLOCK_UL: ++s.listDepth; s.listNumbers.push_back(-1); break;
        case MD_BLOCK_OL: ++s.listDepth; s.listNumbers.push_back(static_cast<int>(static_cast<MD_BLOCK_OL_DETAIL*>(detail)->start)); break;
        case MD_BLOCK_LI: {
            s.Add(Kind::Paragraph);
            auto item = static_cast<MD_BLOCK_LI_DETAIL*>(detail);
            std::wstring prefix;
            if (item->is_task) prefix = (item->task_mark == 'x' || item->task_mark == 'X') ? L"\u2611  " : L"\u2610  ";
            else if (!s.listNumbers.empty() && s.listNumbers.back() >= 0) prefix = std::to_wstring(s.listNumbers.back()++) + L".  ";
            else prefix = L"\u2022  ";
            s.AddText(prefix); s.blocks[s.current].markerOnly = true; break;
        }
        case MD_BLOCK_HR: s.Add(Kind::Rule); s.current = static_cast<size_t>(-1); break;
        case MD_BLOCK_CODE: {
            auto* info = static_cast<MD_BLOCK_CODE_DETAIL*>(detail);
            auto language = Wide({info->lang.text ? info->lang.text : "", info->lang.size});
            auto& block = s.Add(Lower(language) == L"mermaid" ? Kind::Diagram : Kind::Code); block.language = language;
            s.inCode = true; s.spanStyle = Code; break;
        }
        case MD_BLOCK_HTML: s.Add(Kind::Code); s.inCode = true; s.spanStyle = Code; break;
        case MD_BLOCK_TABLE: s.Add(Kind::Table); s.inTable = true; break;
        case MD_BLOCK_TR: s.blocks[s.current].rows.emplace_back(); break;
        case MD_BLOCK_TH: case MD_BLOCK_TD: s.blocks[s.current].rows.back().push_back({{}, type == MD_BLOCK_TH}); break;
        default: break;
        }
        return 0;
    }
    static int LeaveBlock(MD_BLOCKTYPE type, void*, void* userdata) {
        auto& s = *static_cast<Impl*>(userdata);
        switch (type) {
        case MD_BLOCK_QUOTE: --s.quoteDepth; break;
        case MD_BLOCK_UL: case MD_BLOCK_OL: --s.listDepth; if (!s.listNumbers.empty()) s.listNumbers.pop_back(); break;
        case MD_BLOCK_CODE: case MD_BLOCK_HTML: s.inCode = false; s.spanStyle = 0; s.current = static_cast<size_t>(-1); break;
        case MD_BLOCK_TABLE: s.inTable = false; s.current = static_cast<size_t>(-1); break;
        case MD_BLOCK_P: case MD_BLOCK_H: case MD_BLOCK_LI: s.current = static_cast<size_t>(-1); break;
        default: break;
        }
        return 0;
    }
    static int EnterSpan(MD_SPANTYPE type, void* detail, void* userdata) {
        auto& s = *static_cast<Impl*>(userdata); s.styleStack.push_back(s.spanStyle); s.linkStack.push_back(s.currentLink);
        switch (type) {
        case MD_SPAN_EM: s.spanStyle |= Italic; break;
        case MD_SPAN_STRONG: s.spanStyle |= Bold; break;
        case MD_SPAN_CODE: s.spanStyle |= Code; break;
        case MD_SPAN_DEL: s.spanStyle |= Strike; break;
        case MD_SPAN_A: { auto& href = static_cast<MD_SPAN_A_DETAIL*>(detail)->href; s.currentLink = Wide({href.text ? href.text : "", href.size}); break; }
        case MD_SPAN_IMG: { auto& src = static_cast<MD_SPAN_IMG_DETAIL*>(detail)->src; s.imageReference = Wide({src.text ? src.text : "", src.size}); s.imageAlternate.clear(); s.inImage = true; break; }
        default: break;
        }
        return 0;
    }
    static int LeaveSpan(MD_SPANTYPE type, void*, void* userdata) {
        auto& s = *static_cast<Impl*>(userdata);
        if (type == MD_SPAN_IMG) {
            s.inImage = false;
            if (s.inTable) s.AddText(L"[画像: " + s.imageAlternate + L"]");
            else {
                auto& block = s.Add(Kind::Image); block.reference = std::move(s.imageReference); block.alternate = std::move(s.imageAlternate);
                s.current = static_cast<size_t>(-1);
            }
        }
        if (!s.styleStack.empty()) { s.spanStyle = s.styleStack.back(); s.styleStack.pop_back(); }
        if (!s.linkStack.empty()) { s.currentLink = std::move(s.linkStack.back()); s.linkStack.pop_back(); }
        return 0;
    }
    static int Text(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE count, void* userdata) {
        auto& s = *static_cast<Impl*>(userdata);
        if (s.inCode && s.current < s.blocks.size() && s.blocks[s.current].kind == Kind::Diagram) s.blocks[s.current].diagramSource.append(text, count);
        if (type == MD_TEXT_SOFTBR) s.AddText(L" ");
        else if (type == MD_TEXT_BR) s.AddText(L"\n");
        else if (type == MD_TEXT_NULLCHAR) s.AddText(L"\ufffd");
        else if (type == MD_TEXT_ENTITY) s.AddText(Entity({text, count}));
        else s.AddText(Wide({text, count}));
        return 0;
    }
    void HighlightCode(Block& block) {
        auto language = Lower(block.language);
        static const std::wstring languages = L"|c|cpp|c++|csharp|cs|java|js|javascript|ts|typescript|python|py|json|jsonc|yaml|yml|sh|bash|powershell|ps1|sql|go|rust|rs|kotlin|swift|";
        if (language.empty() || languages.find(L"|" + language + L"|") == std::wstring::npos) return;
        bool hashComments = language == L"python" || language == L"py" || language == L"yaml" || language == L"yml" || language == L"sh" || language == L"bash" || language == L"powershell" || language == L"ps1";
        std::wstring code;
        { size_t total = 0; for (const auto& run : block.runs) total += run.text.size(); code.reserve(total); }
        for (const auto& run : block.runs) code += run.text;
        static const std::unordered_set<std::wstring_view> keywords = {
            L"if", L"else", L"for", L"while", L"do", L"switch", L"case", L"default", L"break", L"continue", L"return", L"class", L"struct", L"enum",
            L"namespace", L"using", L"import", L"from", L"as", L"export", L"const", L"let", L"var", L"static", L"public", L"private", L"protected",
            L"internal", L"void", L"int", L"float", L"double", L"bool", L"string", L"char", L"long", L"short", L"unsigned", L"auto", L"new", L"delete",
            L"this", L"base", L"super", L"try", L"catch", L"finally", L"throw", L"throws", L"async", L"await", L"yield", L"function", L"def", L"lambda",
            L"with", L"in", L"is", L"not", L"and", L"or", L"true", L"false", L"null", L"none", L"self", L"pass", L"raise", L"except", L"interface",
            L"extends", L"implements", L"package", L"type", L"func", L"fn", L"pub", L"impl", L"mut", L"match", L"use", L"mod", L"select", L"where",
            L"join", L"insert", L"update", L"create", L"table", L"values", L"order", L"by", L"group", L"having", L"limit", L"distinct"};
        std::vector<Run> colored;
        for (size_t at = 0; at < code.size();) {
            size_t end = at + 1; unsigned style = Code;
            wchar_t c = code[at];
            if (c == L'"' || c == L'\'' || c == L'`') {
                style |= String;
                while (end < code.size()) { if (code[end] == L'\\' && end + 1 < code.size()) { end += 2; continue; } if (code[end++] == c) break; }
            } else if ((hashComments && c == L'#') || (c == L'/' && end < code.size() && code[end] == L'/') || (language == L"sql" && c == L'-' && end < code.size() && code[end] == L'-')) {
                style |= Comment; while (end < code.size() && code[end] != L'\n') ++end;
            } else if (c == L'/' && end < code.size() && code[end] == L'*') {
                style |= Comment; end = code.find(L"*/", end + 1); end = end == std::wstring::npos ? code.size() : end + 2;
            } else if (std::iswdigit(c)) {
                style |= Number; while (end < code.size() && (std::iswalnum(code[end]) || code[end] == L'.' || code[end] == L'_')) ++end;
            } else if (std::iswalpha(c) || c == L'_') {
                while (end < code.size() && (std::iswalnum(code[end]) || code[end] == L'_')) ++end;
                if (end - at < 32) { // Keywords are short; the lowered word stays on the stack.
                    wchar_t word[32]; for (size_t i = at; i < end; ++i) word[i - at] = static_cast<wchar_t>(std::towlower(code[i]));
                    if (keywords.contains(std::wstring_view(word, end - at))) style |= Keyword;
                }
            }
            if (!colored.empty() && colored.back().style == style) colored.back().text.append(code, at, end - at);
            else colored.push_back({code.substr(at, end - at), {}, style});
            at = end;
        }
        block.runs = std::move(colored);
    }
    void Parse() {
        ++parseCount; ReleaseImages();
        std::vector<Block>().swap(blocks); std::wstring().swap(plain); std::wstring().swap(plainLower); plainLowered = false;
        selectionAnchor = selectionFocus = 0; current = static_cast<size_t>(-1); quoteDepth = 0; listDepth = 0; listNumbers.clear();
        spanStyle = 0; styleStack.clear(); linkStack.clear(); currentLink.clear(); inCode = inTable = inImage = false;
        if (sourceMode) { auto& block = Add(Kind::Code); block.language = L"Markdown"; block.runs.push_back({Wide(source), {}, Code}); }
        else if (!source.empty()) {
            MD_PARSER parser{}; parser.abi_version = 0;
            parser.flags = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS | MD_FLAG_PERMISSIVEAUTOLINKS;
            parser.enter_block = EnterBlock; parser.leave_block = LeaveBlock; parser.enter_span = EnterSpan; parser.leave_span = LeaveSpan; parser.text = Text;
            if (source.size() > std::numeric_limits<MD_SIZE>::max() || md_parse(source.data(), static_cast<MD_SIZE>(source.size()), &parser, this) != 0) {
                blocks.clear(); auto& block = Add(Kind::Code); block.runs.push_back({Wide(source), {}, Code});
            }
        }
        std::map<std::wstring, int> anchors;
        auto indexRuns = [this](std::vector<Run>& runs) { for (auto& run : runs) { run.offset = plain.size(); plain += run.text; } plain += L'\n'; };
        for (auto& block : blocks) {
            if (block.kind == Kind::Code && !sourceMode) HighlightCode(block);
            if (block.kind == Kind::Diagram) {if(mermaid)RequestDiagram(block);else block.diagram=ParseDiagram(block.diagramSource);}
            if (block.kind != Kind::Diagram || !RenderedDiagram(block)) indexRuns(block.runs);
            if (block.kind == Kind::Heading) {
                std::wstring text; for (const auto& run : block.runs) text += run.text;
                auto name = Anchor(text); int occurrence = anchors[name]++; block.anchor = name + (occurrence ? L"-" + std::to_wstring(occurrence) : L"");
            }
            for (auto& row : block.rows) for (auto& cell : row) indexRuns(cell.runs);
        }
        // Ordinary scratch buffers can be reused. A large alternate string in a
        // table must not remain outside the bounded document cache indefinitely.
        if (imageAlternate.capacity() > 8192) std::wstring().swap(imageAlternate);
        else imageAlternate.clear();
        if (imageReference.capacity() > 8192) std::wstring().swap(imageReference);
        else imageReference.clear();
        FindMatches(); dirty = true; laidOut = false;
    }
    float LayoutRuns(Graphics& g, Block& block, const std::vector<Run>& runs, float left, float top, float width, float size, unsigned extraStyle, bool wrap) {
        const float lineHeight = std::ceil(size * 1.65f);
        float x = left, y = top, furthest = left;
        for (const auto& run : runs) {
            size_t at = 0;
            while (at < run.text.size()) {
                if (run.text[at] == L'\r') { ++at; continue; }
                if (run.text[at] == L'\n') { furthest = std::max(furthest, x); x = left; y += lineHeight; ++at; continue; }
                // Keep Latin words together; CJK wraps between glyphs. Avoid splitting UTF-16 surrogate pairs.
                size_t end = at + 1;
                wchar_t first = run.text[at];
                if (first >= 0xd800 && first <= 0xdbff && end < run.text.size()) ++end;
                else if (first < 0x2e80 && !std::iswspace(first)) {
                    while (end < run.text.size() && end - at < 256 && run.text[end] < 0x2e80 && !std::iswspace(run.text[end])) ++end;
                } else if (std::iswspace(first) && first != L'\n') {
                    while (end < run.text.size() && end - at < 256 && (run.text[end] == L' ' || run.text[end] == L'\t')) ++end;
                }
                std::wstring token = run.text.substr(at, end - at);
                if (token.find(L'\t') != std::wstring::npos) { for (auto& c : token) if (c == L'\t') c = L' '; }
                unsigned style = run.style | extraStyle;
                float tokenWidth = Measure(g, token, size, style);
                if (wrap && tokenWidth <= width && x > left && x + tokenWidth > left + width && !std::iswspace(first)) { furthest = std::max(furthest, x); x = left; y += lineHeight; }
                if (wrap && tokenWidth > width && token.size() > 1) {
                    // A long URL or identifier still needs a bounded visual line.
                    end = at + 1;
                    if (first >= 0xd800 && first <= 0xdbff && end < run.text.size()) ++end;
                    token = run.text.substr(at, end - at); tokenWidth = Measure(g, token, size, style);
                }
                if (wrap && x > left && x + tokenWidth > left + width) { furthest = std::max(furthest, x); x = left; y += lineHeight; }
                Piece p{ token, run.link, style, run.offset + at, x, y, tokenWidth, lineHeight, size };
                if (!block.pieces.empty()) {
                    auto& previous = block.pieces.back();
                    if (previous.text.size() + p.text.size() <= 512 && previous.style == p.style && previous.link == p.link && previous.size == p.size && previous.y == p.y && previous.offset + previous.text.size() == p.offset && std::abs(previous.x + previous.width - p.x) < 0.1f) {
                        previous.text += p.text; previous.width += p.width;
                    } else block.pieces.push_back(std::move(p));
                } else block.pieces.push_back(std::move(p));
                x += tokenWidth; at = end;
            }
        }
        block.width = std::max(block.width, std::max(furthest, x) - block.x);
        return y + lineHeight - top;
    }
    void LoadImage(Block& block) {
        if (block.imageAttempted) return; block.imageAttempted = true;
        try {
            auto reference = DecodeReference(block.reference);
            if (reference.empty() || reference.find(L'\0') != std::wstring::npos || reference.starts_with(L"\\") || reference.starts_with(L"//")) return;
            if (reference.find(L':') != std::wstring::npos && !(reference.size() > 2 && reference[1] == L':' && std::iswalpha(reference[0]) && reference.find(L':', 2) == std::wstring::npos)) return;
            std::filesystem::path file(reference);
            if (file.is_relative()) file = std::filesystem::path(path).parent_path() / file;
            file = std::filesystem::absolute(file).lexically_normal();
            auto bitmap = OpenLocalImage(file);
            if (!bitmap) return;
            UINT w = bitmap->GetWidth(), h = bitmap->GetHeight();
            block.imageWidth = static_cast<float>(w); block.imageHeight = static_cast<float>(h); block.imagePath = file.native(); block.imageValid = true;
        } catch (...) { }
    }
    void LoadImageRaster(Block& block) {
        if (!block.imageValid || block.image) return;
        try {
            auto original = OpenLocalImage(block.imagePath);
            if (original) {
                float width = static_cast<float>(original->GetWidth()), height = static_cast<float>(original->GetHeight());
                if (width != block.imageWidth || height != block.imageHeight) {
                    block.imageWidth = width; block.imageHeight = height;
                    dirty = true; return; // Reflow before choosing the raster dimensions.
                }
                int rasterWidth = std::max(1, static_cast<int>(std::ceil(block.width)));
                int rasterHeight = std::max(1, static_cast<int>(std::ceil(block.height)));
                if (static_cast<uint64_t>(rasterWidth) * rasterHeight <= 16000000) {
                    auto raster = std::make_unique<Bitmap>(rasterWidth, rasterHeight, PixelFormat32bppPARGB);
                    if (raster->GetLastStatus() == Ok) {
                        Graphics cached(raster.get()); cached.SetInterpolationMode(InterpolationModeHighQualityBicubic);
                        if (cached.Clear(Color(0, 0, 0, 0)) == Ok && cached.DrawImage(original.get(), 0, 0, rasterWidth, rasterHeight) == Ok) {
                            block.image = std::move(raster); return;
                        }
                    }
                }
            }
        } catch (...) { }
        // Show the normal image error panel instead of drawing with stale size
        // information or retrying a failed decoder on every idle repaint.
        block.imageValid = false; block.image.reset(); dirty = true;
    }
    void PrepareVisibleImages(Graphics& g) {
        // A changed image can move additional images into the viewport. Bound
        // reflow work per paint; any newly exposed remainder gets the next paint.
        for (int pass = 0; pass < 2; ++pass) {
            RECT client{}; GetClientRect(hwnd, &client);
            for (size_t i = visibleBegin; i < std::min(visibleEnd, blocks.size()); ++i)
                if (blocks[i].y + blocks[i].height < vertical || blocks[i].y > vertical + client.bottom) blocks[i].image.reset();
            auto first = std::lower_bound(blocks.begin(), blocks.end(), static_cast<float>(vertical),
                [](const Block& block, float y) { return block.y + block.height < y; });
            visibleBegin = visibleEnd = static_cast<size_t>(first - blocks.begin());
            for (auto it = first; it != blocks.end() && it->y <= vertical + client.bottom; ++it) {
                ++visibleEnd;
                if (it->kind == Kind::Image) LoadImageRaster(*it);
            }
            if (!dirty) return;
            EnsureLayout(g);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    void EnsureLayout(Graphics& g) {
        if(mermaid&&refreshMermaidTheme){
            refreshMermaidTheme=false;
            for(auto& block:blocks)RequestDiagram(block);
            ReindexText();
        }
        RECT client{}; GetClientRect(hwnd, &client);
        int clientWidth = std::max(1L, client.right);
        float currentDpi = GetDpiForWindow(hwnd) / 96.0f;
        if (!dirty && clientWidth == layoutWidth && currentDpi == dpi) { ApplyPendingScroll(client); return; }
        // While the user drags a window edge the previous layout stays on
        // screen; one relayout follows when the drag ends (SetLiveResize).
        if (liveResize && laidOut && !dirty && currentDpi == dpi) { ApplyPendingScroll(client); return; }
        if (dpi != currentDpi) dpi = currentDpi; // Font and width caches key on the scaled size.
        ++layoutCount; ReleaseImages(); scrollbarsDirty = true;
        laidOutFont = fontSize; laidOutWide = wide;
        layoutWidth = clientWidth; dirty = false;
        const float margin = 28 * dpi;
        const float available = std::max(80.0f * dpi, clientWidth - margin * 2);
        const float baseWidth = wide ? available : std::min(available, 1000.0f * dpi);
        const float left = std::max(margin, (clientWidth - baseWidth) / 2);
        const float body = fontSize * dpi;
        float y = margin;
        float documentWidth = baseWidth;
        for (auto& block : blocks) {
            block.pieces.clear(); block.rowEdges.clear(); block.rowPieceStarts.clear();
            block.x = left + block.indent * 18 * dpi + block.quote * 15 * dpi;
            block.y = y; block.width = std::max(60 * dpi, baseWidth - (block.x - left)); block.height = 0;
            float inset = 0, size = body, top = y;
            if (block.kind == Kind::Heading) {
                static const float factors[] = {1, 2, 1.55f, 1.3f, 1.12f, 1, 1};
                size *= factors[std::clamp(block.heading, 1, 6)];
                top += 8 * dpi;
                block.height = LayoutRuns(g, block, block.runs, block.x, top, block.width, size, Bold, true) + 18 * dpi;
            } else if (block.kind == Kind::Rule) block.height = 18 * dpi;
            else if (block.kind == Kind::Table) {
                size *= 0.94f;
                size_t columns = 0; for (const auto& row : block.rows) columns = std::max(columns, row.size());
                float columnWidth = std::max(125 * dpi, block.width / std::max<size_t>(1, columns));
                block.width = columnWidth * columns; block.rowEdges.push_back(y);
                float rowY = y;
                for (auto& row : block.rows) {
                    block.rowPieceStarts.push_back(block.pieces.size());
                    float rowHeight = size * 1.65f + 16 * dpi;
                    for (size_t c = 0; c < row.size(); ++c) {
                        float h = LayoutRuns(g, block, row[c].runs, block.x + c * columnWidth + 10 * dpi, rowY + 8 * dpi, columnWidth - 20 * dpi, size, row[c].heading ? Bold : 0, true) + 16 * dpi;
                        rowHeight = std::max(rowHeight, h);
                    }
                    rowY += rowHeight; block.rowEdges.push_back(rowY);
                }
                block.height = rowY - y;
            } else if (block.kind == Kind::Image) {
                LoadImage(block);
                if (block.imageValid) {
                    float scale = std::min(1.0f, block.width / block.imageWidth);
                    block.width = block.imageWidth * scale; block.height = block.imageHeight * scale;
                } else {
                    std::wstring explanation = L"[画像] " + (block.alternate.empty() ? block.reference : block.alternate);
                    auto lower = Lower(block.reference);
                    if (lower.starts_with(L"http://") || lower.starts_with(L"https://")) explanation += L"  — 外部画像";
                    else explanation += L"  — 読み込めない画像形式またはパス";
                    std::vector<Run> alt = {{explanation, (lower.starts_with(L"http://") || lower.starts_with(L"https://")) ? block.reference : L"", 0, plain.size()}};
                    block.height = LayoutRuns(g, block, alt, block.x + 12 * dpi, y + 10 * dpi, block.width - 24 * dpi, body * 0.92f, 0, true) + 20 * dpi;
                }
            } else if (block.kind == Kind::Diagram && RenderedDiagram(block)) {
                float scale = body / 16.0f;
                block.width = std::max(block.width, (block.officialDiagram?block.officialDiagram->width:block.diagram->width) * scale + 24 * dpi);
                block.height = (block.officialDiagram?block.officialDiagram->height:block.diagram->height) * scale + 24 * dpi;
            } else {
                bool code = block.kind == Kind::Code || block.kind == Kind::Diagram;
                inset = code ? 16 * dpi : 0;
                if (code) { size *= 0.9f; top += 14 * dpi; }
                if (block.kind == Kind::Diagram) {
                    std::wstring message = L"図を表示できません: " + (block.officialDiagram?block.officialDiagram->error:((block.diagram && !block.diagram->error.empty()) ? block.diagram->error : L"未対応の構文"));
                    std::vector<Run> error = {{message, {}, Bold, plain.size()}};
                    top += LayoutRuns(g, block, error, block.x + inset, top, block.width - inset * 2, body * 0.92f, 0, true) + 8 * dpi;
                }
                float originalWidth = block.width;
                block.height = top - y + LayoutRuns(g, block, block.runs, block.x + inset, top, block.width - inset * 2, size, code ? Code : 0, !code) + (code ? 14 * dpi : 0);
                if (code && block.width > originalWidth) block.width += inset;
            }
            documentWidth = std::max(documentWidth, block.x - left + block.width);
            y += block.height + ((block.indent && block.kind == Kind::Paragraph) ? 3 : 15) * dpi;
        }
        // Diagrams, tables and unwrapped code can exceed the text column.
        // Center their actual combined extent; when it exceeds the viewport,
        // keep just the ordinary left margin. Apply the same offset to text
        // pieces so painting, selection, links and search share coordinates.
        const float documentLeft = std::max(margin, (clientWidth - documentWidth) / 2);
        const float shift = documentLeft - left;
        if (shift != 0) for (auto& block : blocks) {
            block.x += shift;
            for (auto& piece : block.pieces) piece.x += shift;
        }
        contentWidth = std::max(static_cast<float>(clientWidth), documentLeft + documentWidth + margin);
        contentHeight = y + margin;
        laidOut = true;
        ApplyPendingScroll(client);
    }
    void ApplyPendingScroll(const RECT& client) {
        if (!scrollbarsDirty && pendingRatio < 0 && pendingAnchor.empty()) return;
        if (pendingRatio >= 0) { vertical = static_cast<int>(pendingRatio * std::max(0.0f, contentHeight - client.bottom)); pendingRatio = -1; }
        if (!pendingAnchor.empty()) { for (const auto& block : blocks) if (block.anchor == pendingAnchor) { vertical = static_cast<int>(block.y - 16 * dpi); break; } pendingAnchor.clear(); }
        UpdateScrollbars();
    }
    void UpdateScrollbars() {
        if (!hwnd) return;
        scrollbarsDirty = false;
        RECT r{}; GetClientRect(hwnd, &r);
        vertical = std::clamp(vertical, 0, std::max(0, static_cast<int>(contentHeight) - static_cast<int>(r.bottom)));
        horizontal = std::clamp(horizontal, 0, std::max(0, static_cast<int>(contentWidth) - static_cast<int>(r.right)));
        SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
        info.nMin = 0; info.nMax = std::max(0, static_cast<int>(contentHeight) - 1); info.nPage = std::max(0L, r.bottom); info.nPos = vertical; SetScrollInfo(hwnd, SB_VERT, &info, TRUE);
        info.nMax = std::max(0, static_cast<int>(contentWidth) - 1); info.nPage = std::max(0L, r.right); info.nPos = horizontal; SetScrollInfo(hwnd, SB_HORZ, &info, TRUE);
    }
    void Navigate(WPARAM key) {
        // Key events may arrive before WM_PAINT after a tab switch, zoom or
        // resize. Resolve the new layout and restored position first so the
        // next paint cannot undo navigation or use the previous document size.
        HDC dc = GetDC(hwnd);if (!dc) return;
        {
            Graphics g(dc);
            for (int attempt = 0; attempt < 4; ++attempt) {
                RECT before{}, after{};GetClientRect(hwnd, &before);
                EnsureLayout(g);GetClientRect(hwnd, &after);
                if (before.right == after.right && before.bottom == after.bottom) break;
            }
        }
        ReleaseDC(hwnd, dc);
        if (key == VK_HOME) ScrollBy(0, -vertical);
        else if (key == VK_END) ScrollBy(0, std::max(0, static_cast<int>(contentHeight) - vertical));
        else {
            RECT client{};GetClientRect(hwnd, &client);
            ScrollBy(0, (key == VK_NEXT ? 1 : -1) * std::max(20L, client.bottom - 48));
        }
    }
    void ScrollBy(int x, int y) {
        vertical += y; horizontal += x; UpdateScrollbars();
        InvalidateRect(hwnd, nullptr, FALSE); if (owner->onScroll) owner->onScroll();
    }
    void ScrollBar(bool isVertical, int code) {
        SCROLLINFO info{sizeof(info), SIF_ALL}; GetScrollInfo(hwnd, isVertical ? SB_VERT : SB_HORZ, &info);
        int value = isVertical ? vertical : horizontal;
        switch (code) {
        case SB_LINEUP: value -= 32; break;
        case SB_LINEDOWN: value += 32; break;
        case SB_PAGEUP: value -= static_cast<int>(info.nPage); break;
        case SB_PAGEDOWN: value += static_cast<int>(info.nPage); break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: value = info.nTrackPos; break;
        case SB_TOP: value = 0; break;
        case SB_BOTTOM: value = info.nMax; break;
        default: return;
        }
        ScrollBy(isVertical ? 0 : value - horizontal, isVertical ? value - vertical : 0);
    }
    void Paint(HDC external = nullptr) {
        PAINTSTRUCT ps{}; HDC dc = external ? external : BeginPaint(hwnd, &ps); RECT client{}; GetClientRect(hwnd, &client);
        if (client.right > 0 && client.bottom > 0) {
            bool buffered = EnsureBuffer(dc, client.right, client.bottom);
            HDC memory = buffered ? backDC : dc;
            {
                Graphics g(memory); g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit); g.SetSmoothingMode(SmoothingModeAntiAlias); g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
                EnsureLayout(g); PrepareVisibleImages(g); g.Clear(background); g.TranslateTransform(-static_cast<REAL>(horizontal), -static_cast<REAL>(vertical));
                SolidBrush textBrush(foreground), linkBrush(linkColor), panelBrush(panel), mutedBrush(muted);
                SolidBrush keywordBrush(dark ? Color(255, 192, 152, 245) : Color(255, 128, 56, 169));
                SolidBrush stringBrush(dark ? Color(255, 164, 206, 143) : Color(255, 39, 125, 63));
                SolidBrush commentBrush(dark ? Color(255, 127, 145, 155) : Color(255, 108, 122, 130));
                SolidBrush numberBrush(dark ? Color(255, 239, 187, 120) : Color(255, 174, 98, 30));
                Pen line(border, dpi), quotePen(linkColor, 3 * dpi);
                if (blocks.empty()) {
                    auto& font = GetFont(22 * dpi, 0); std::wstring welcome = L"Markdown を開いて読み始める";
                    g.DrawString(welcome.c_str(), -1, &font, PointF(32 * dpi, 36 * dpi), &textBrush);
                    auto& helperFont = GetFont(14 * dpi, 0);
                    g.DrawString(L"ファイルやフォルダーを開くか、ここへドロップしてください。", -1, &helperFont, PointF(34 * dpi, 86 * dpi), &mutedBrush);
                }
                auto firstBlock = std::lower_bound(blocks.begin(), blocks.end(), static_cast<float>(vertical),
                    [](const Block& block, float y) { return block.y + block.height < y; });
                visibleBegin = visibleEnd = static_cast<size_t>(firstBlock - blocks.begin());
                for (auto it = firstBlock; it != blocks.end() && it->y <= vertical + client.bottom; ++it) {
                    auto& block = *it; ++visibleEnd;
                    RectF bounds(block.x, block.y, block.width, block.height);
                    bool panelBlock = block.kind == Kind::Code || block.kind == Kind::Diagram || (block.kind == Kind::Image && !block.imageValid);
                    if (panelBlock) { g.FillRectangle(&panelBrush, bounds); g.DrawRectangle(&line, bounds); }
                    if (block.quote) g.DrawLine(&quotePen, block.x - 10 * dpi, block.y, block.x - 10 * dpi, block.y + block.height);
                    if (block.kind == Kind::Rule) g.DrawLine(&line, block.x, block.y + 8 * dpi, block.x + block.width, block.y + 8 * dpi);
                    if (block.kind == Kind::Heading && block.heading <= 2) g.DrawLine(&line, block.x, block.y + block.height, block.x + block.width, block.y + block.height);
                    if (block.kind == Kind::Table && block.rowEdges.size() > 1) {
                        if (!block.rows.empty()) { RectF header(block.x, block.y, block.width, block.rowEdges[1] - block.y); g.FillRectangle(&panelBrush, header); }
                        size_t columns = 0; for (const auto& row : block.rows) columns = std::max(columns, row.size());
                        g.DrawRectangle(&line, bounds);
                        for (float edge : block.rowEdges) g.DrawLine(&line, block.x, edge, block.x + block.width, edge);
                        for (size_t c = 1; c < columns; ++c) { float x = block.x + block.width * c / columns; g.DrawLine(&line, x, block.y, x, block.y + block.height); }
                    }
                    if (block.kind == Kind::Image && block.imageValid) {
                        if (block.image) g.DrawImage(block.image.get(), bounds);
                    }
                    if (block.kind == Kind::Diagram && RenderedDiagram(block)) {
                        if(block.officialDiagram){
                            auto& result=*block.officialDiagram;const float scale=fontSize*dpi/16.0f;
                            if(result.bitmap)g.DrawImage(result.bitmap.get(),RectF(block.x+12*dpi,block.y+12*dpi,result.width*scale,result.height*scale));
                            else{const wchar_t* text=L"Mermaidを描画中…";g.DrawString(text,-1,&GetFont(fontSize*dpi,0),PointF(block.x+12*dpi,block.y+12*dpi),&textBrush);}
                        }else{
                            DiagramTheme theme{panel, foreground, muted, dark ? Color(255, 44, 57, 75) : Color(255, 232, 240, 254), linkColor};
                            DrawDiagram(g, *block.diagram, block.x + 12 * dpi, block.y + 12 * dpi, fontSize * dpi / 16.0f, theme);
                        }
                    }
                    // Table cells restart at the row top; other block pieces are
                    // ordered vertically, including a source view with many lines.
                    auto firstPiece = block.pieces.begin(), endPiece = block.pieces.end();
                    if (block.kind == Kind::Table) {
                        // Rows follow each other and rowEdges[i] is the top of row
                        // i, so only the rows crossing the client area are visited.
                        if (block.rowPieceStarts.size() + 1 == block.rowEdges.size()) {
                            auto rowAt = [&](float y) { // Index of the first row whose bottom edge reaches y.
                                return static_cast<size_t>(std::lower_bound(block.rowEdges.begin() + 1, block.rowEdges.end(), y) - block.rowEdges.begin()) - 1; };
                            auto pieceAt = [&](size_t row) { return block.pieces.begin() + static_cast<ptrdiff_t>(row < block.rowPieceStarts.size() ? block.rowPieceStarts[row] : block.pieces.size()); };
                            firstPiece = pieceAt(rowAt(static_cast<float>(vertical)));
                            endPiece = pieceAt(rowAt(static_cast<float>(vertical + client.bottom)) + 1);
                        }
                    } else firstPiece = std::lower_bound(block.pieces.begin(), block.pieces.end(), static_cast<float>(vertical),
                        [](const Piece& piece, float y) { return piece.y + piece.height < y; });
                    for (auto pieceIt = firstPiece; pieceIt < endPiece; ++pieceIt) {
                        const auto& piece = *pieceIt;
                        if (block.kind != Kind::Table && piece.y > vertical + client.bottom) break;
                        if (piece.y + piece.height < vertical || piece.y > vertical + client.bottom) continue;
                        if (piece.x + piece.width < horizontal || piece.x > horizontal + client.right) continue;
                        RectF pieceBounds(piece.x, piece.y + 2 * dpi, piece.width, piece.height - 3 * dpi);
                        if ((piece.style & Code) && !panelBlock) g.FillRectangle(&panelBrush, pieceBounds);
                        auto begin = std::lower_bound(matches.begin(), matches.end(), piece.offset, [](const auto& match, size_t at) { return match.first + match.second <= at; });
                        for (auto it = begin; it != matches.end() && it->first < piece.offset + piece.text.size(); ++it) {
                            size_t start = std::max(piece.offset, it->first) - piece.offset;
                            size_t end = std::min(piece.offset + piece.text.size(), it->first + it->second) - piece.offset;
                            if (start >= end) continue;
                            float x = piece.x + Measure(g, piece.text.substr(0, start), piece.size, piece.style);
                            float width = Measure(g, piece.text.substr(start, end - start), piece.size, piece.style);
                            bool active = std::distance(matches.begin(), it) == activeMatch;
                            SolidBrush highlight(active ? Color(235, 230, 155, 42) : (dark ? Color(180, 119, 95, 27) : Color(255, 255, 235, 137)));
                            g.FillRectangle(&highlight, RectF(x, piece.y, width, piece.height));
                        }
                        size_t selectedStart = std::min(selectionAnchor, selectionFocus), selectedEnd = std::max(selectionAnchor, selectionFocus);
                        if (selectedStart < selectedEnd && selectedStart < piece.offset + piece.text.size() && selectedEnd > piece.offset) {
                            size_t start = std::max(piece.offset, selectedStart) - piece.offset;
                            size_t end = std::min(piece.offset + piece.text.size(), selectedEnd) - piece.offset;
                            float x = piece.x + Measure(g, piece.text.substr(0, start), piece.size, piece.style);
                            float width = Measure(g, piece.text.substr(start, end - start), piece.size, piece.style);
                            SolidBrush selection(dark ? Color(255, 47, 77, 117) : Color(255, 184, 215, 252));
                            g.FillRectangle(&selection, RectF(x, piece.y, width, piece.height));
                        }
                        auto* brush = piece.link.empty() ? &textBrush : &linkBrush;
                        if (piece.link.empty()) {
                            if (piece.style & Keyword) brush = &keywordBrush;
                            else if (piece.style & String) brush = &stringBrush;
                            else if (piece.style & Comment) brush = &commentBrush;
                            else if (piece.style & Number) brush = &numberBrush;
                        }
                        g.DrawString(piece.text.c_str(), static_cast<INT>(piece.text.size()), &GetFont(piece.size, piece.style), PointF(piece.x, piece.y), &Format(), brush);
                        if (!piece.link.empty()) { Pen underline(linkColor, dpi); g.DrawLine(&underline, piece.x, piece.y + piece.size * 1.35f, piece.x + piece.width, piece.y + piece.size * 1.35f); }
                    }
                }
            }
            if (buffered) BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
        }
        if (!external) EndPaint(hwnd, &ps);
    }
    std::wstring HitLink(int x, int y) const {
        float px = static_cast<float>(x + horizontal), py = static_cast<float>(y + vertical);
        // Blocks follow each other vertically; only those around py are visited.
        auto first = std::lower_bound(blocks.begin(), blocks.end(), py, [](const Block& block, float y) { return block.y + block.height < y; });
        for (auto it = first; it != blocks.end() && it->y <= py; ++it) {
            for (const auto& piece : it->pieces) if (!piece.link.empty() && px >= piece.x && px <= piece.x + piece.width && py >= piece.y && py <= piece.y + piece.height) return piece.link;
        }
        return {};
    }
    size_t HitOffset(int x, int y) {
        float px = static_cast<float>(x + horizontal), py = static_cast<float>(y + vertical);
        const Piece* nearest = nullptr; float distance = std::numeric_limits<float>::max();
        size_t nearestBlock = 0;
        auto visit = [&](size_t index) {
            for (const auto& piece : blocks[index].pieces) {
                float dy = py < piece.y ? piece.y - py : (py > piece.y + piece.height ? py - piece.y - piece.height : 0);
                float dx = px < piece.x ? piece.x - px : (px > piece.x + piece.width ? px - piece.x - piece.width : 0);
                float d = dy * 10000 + dx;
                // Ties resolve to the earliest block and piece, exactly as a
                // full first-to-last scan with a strict comparison would.
                if (d < distance || (d == distance && nearest && index < nearestBlock)) { distance = d; nearest = &piece; nearestBlock = index; }
            }
        };
        // Blocks are laid out top to bottom without overlapping, and pieces
        // stay inside their block. Search outward from the block under the
        // point; once a block's vertical gap alone exceeds the best distance,
        // no later block in that direction can win.
        const size_t start = static_cast<size_t>(std::lower_bound(blocks.begin(), blocks.end(), py,
            [](const Block& block, float at) { return block.y + block.height < at; }) - blocks.begin());
        for (size_t index = start; index < blocks.size(); ++index) {
            if (blocks[index].y > py && (blocks[index].y - py) * 10000 > distance) break;
            visit(index);
        }
        for (size_t index = start; index > 0;) {
            --index;
            const float bottom = blocks[index].y + blocks[index].height;
            if (py > bottom && (py - bottom) * 10000 > distance) break;
            visit(index);
        }
        if (!nearest) return 0;
        if (px <= nearest->x) return std::min(plain.size(), nearest->offset);
        if (px >= nearest->x + nearest->width) return std::min(plain.size(), nearest->offset + nearest->text.size());
        HDC dc = GetDC(hwnd); size_t offset = 0;
        {
            Graphics g(dc); g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
            size_t low = 0, high = nearest->text.size();
            while (low < high) {
                size_t middle = (low + high) / 2;
                if (Measure(g, nearest->text.substr(0, middle), nearest->size, nearest->style) < px - nearest->x) low = middle + 1;
                else high = middle;
            }
            offset = low;
            if (offset > 0 && offset < nearest->text.size() && nearest->text[offset] >= 0xdc00 && nearest->text[offset] <= 0xdfff) --offset;
        }
        ReleaseDC(hwnd, dc); return std::min(plain.size(), nearest->offset + offset);
    }
    std::wstring SelectedText() const {
        size_t start = std::min(selectionAnchor, selectionFocus), end = std::min(plain.size(), std::max(selectionAnchor, selectionFocus));
        return start < end ? plain.substr(start, end - start) : std::wstring();
    }
    void CopySelection() {
        const auto selected = SelectedText();
        CopyText(hwnd, selected.empty() ? Wide(source) : selected);
    }
    void FindMatches() {
        std::vector<std::pair<size_t, size_t>>().swap(matches); activeMatch = -1;
        if (query.empty()) { std::wstring().swap(plainLower); plainLowered = false; return; }
        // Every keystroke in the search box used to lowercase a copy of the
        // whole body; the copy is now reused until the body or query goes away.
        if (!plainLowered) { plainLower = Lower(plain); plainLowered = true; }
        const auto needle = Lower(query);
        size_t at = 0;
        while ((at = plainLower.find(needle, at)) != std::wstring::npos) { matches.emplace_back(at, needle.size()); at += std::max<size_t>(1, needle.size()); }
    }
    void RevealMatch() {
        if (activeMatch < 0 || activeMatch >= static_cast<int>(matches.size()) || !hwnd) return;
        HDC dc = GetDC(hwnd); { Graphics g(dc); EnsureLayout(g); } ReleaseDC(hwnd, dc);
        auto match = matches[activeMatch];
        for (const auto& block : blocks) for (const auto& piece : block.pieces) {
            if (match.first >= piece.offset && match.first < piece.offset + piece.text.size()) {
                vertical = std::max(0, static_cast<int>(piece.y - 40 * dpi));
                RECT r{}; GetClientRect(hwnd, &r);
                if (piece.x < horizontal || piece.x + piece.width > horizontal + r.right) horizontal = std::max(0, static_cast<int>(piece.x - 24 * dpi));
                UpdateScrollbars(); InvalidateRect(hwnd, nullptr, FALSE); return;
            }
        }
    }
};

NativeDocumentView::NativeDocumentView() : impl_(std::make_unique<Impl>(this)) {}
NativeDocumentView::~NativeDocumentView() { if (impl_->hwnd) DestroyWindow(impl_->hwnd); }
void NativeDocumentView::EnableOfficialMermaid(const std::wstring& profileDirectory) {
    if(impl_->mermaid)return;impl_->mermaid=std::make_unique<MermaidRenderer>(profileDirectory);
    impl_->documentCache.clear();impl_->cacheBytes=0;impl_->Parse();
    if(impl_->hwnd)InvalidateRect(impl_->hwnd,nullptr,FALSE);
}
HWND NativeDocumentView::Create(HWND parent, int id) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW type{sizeof(type)}; type.lpfnWndProc = Impl::WndProc; type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = L"MMviewer.NativeDocument"; type.hCursor = LoadCursorW(nullptr, IDC_ARROW); type.style = CS_DBLCLKS;
        registered = RegisterClassExW(&type) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    if (!registered) return nullptr;
    return CreateWindowExW(0, L"MMviewer.NativeDocument", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), impl_.get());
}
void NativeDocumentView::SetDocument(const std::string& source, const std::wstring& path) {
    impl_->ReplaceDocument(source, path, impl_->fontSize, impl_->sourceMode);
}
void NativeDocumentView::SetDocument(const std::string& source, const std::wstring& path, int fontPixels, bool sourceMode) {
    impl_->ReplaceDocument(source, path, fontPixels, sourceMode);
}
void NativeDocumentView::InvalidateDocumentCache(const std::wstring& path) {
    impl_->RemoveCached(path);
    if (impl_->path == path) impl_->cacheable = false;
}
NativeDocumentView::PerformanceStats NativeDocumentView::GetPerformanceStats() const {
    PerformanceStats stats{impl_->parseCount, impl_->layoutCount, impl_->cacheHits, impl_->documentCache.size(), impl_->cacheBytes, impl_->bufferAllocations};
    for(const auto& block:impl_->blocks)if(const auto& diagram=block.officialDiagram){
        if(!diagram->complete)++stats.pendingDiagrams;
        else if(diagram->bitmap)++stats.renderedDiagrams;
        else ++stats.failedDiagrams;
    }
    return stats;
}
void NativeDocumentView::SetTheme(bool dark) {
    if(impl_->dark!=dark)impl_->refreshMermaidTheme=true;
    impl_->dark = dark;
    impl_->background = dark ? Gdiplus::Color(255, 24, 28, 35) : Gdiplus::Color(255, 255, 255, 255);
    impl_->foreground = dark ? Gdiplus::Color(255, 223, 230, 239) : Gdiplus::Color(255, 38, 43, 52);
    impl_->muted = dark ? Gdiplus::Color(255, 159, 172, 191) : Gdiplus::Color(255, 100, 112, 128);
    impl_->panel = dark ? Gdiplus::Color(255, 31, 37, 47) : Gdiplus::Color(255, 245, 247, 250);
    impl_->border = dark ? Gdiplus::Color(255, 64, 76, 94) : Gdiplus::Color(255, 218, 224, 231);
    impl_->linkColor = dark ? Gdiplus::Color(255, 112, 174, 255) : Gdiplus::Color(255, 38, 104, 194);
    if (impl_->hwnd) {
        SetWindowTheme(impl_->hwnd, dark ? L"DarkMode_Explorer" : nullptr, dark ? L"ScrollBar" : nullptr);
        RedrawWindow(impl_->hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME);
    }
}
void NativeDocumentView::SetFontSize(int pixels) {
    pixels = std::clamp(pixels, 10, 40); if (impl_->fontSize == pixels) return;
    impl_->pendingRatio = ScrollRatio(); impl_->fontSize = pixels; impl_->dirty = true;
    if (impl_->hwnd) InvalidateRect(impl_->hwnd, nullptr, FALSE);
}
int NativeDocumentView::FontSize() const { return impl_->fontSize; }
void NativeDocumentView::SetLiveResize(bool active) {
    if (impl_->liveResize == active) return;
    impl_->liveResize = active;
    if (!active && impl_->hwnd) InvalidateRect(impl_->hwnd, nullptr, FALSE);
}
void NativeDocumentView::SetSourceMode(bool sourceMode) { if (sourceMode == impl_->sourceMode) return; impl_->pendingRatio = ScrollRatio(); impl_->sourceMode = sourceMode; impl_->Parse(); if (impl_->hwnd) InvalidateRect(impl_->hwnd, nullptr, FALSE); }
void NativeDocumentView::SetWide(bool wide) { if (wide == impl_->wide) return; impl_->pendingRatio = ScrollRatio(); impl_->wide = wide; impl_->dirty = true; if (impl_->hwnd) InvalidateRect(impl_->hwnd, nullptr, FALSE); }
double NativeDocumentView::ScrollRatio() const {
    if (impl_->pendingRatio >= 0) return impl_->pendingRatio;
    if (!impl_->hwnd || !impl_->laidOut) return 0;
    RECT r{}; GetClientRect(impl_->hwnd, &r); double maximum = impl_->contentHeight - r.bottom;
    return maximum > 0 ? std::clamp(impl_->vertical / maximum, 0.0, 1.0) : 0;
}
void NativeDocumentView::SetScrollRatio(double ratio) {
    impl_->pendingRatio = std::isfinite(ratio) ? std::clamp(ratio, 0.0, 1.0) : 0;
    if (impl_->hwnd) InvalidateRect(impl_->hwnd, nullptr, FALSE);
}
void NativeDocumentView::ScrollToAnchor(const std::wstring& anchor) {
    impl_->pendingAnchor = Lower(DecodeReference(anchor.starts_with(L"#") ? anchor.substr(1) : anchor));
    if (impl_->pendingAnchor.empty()) impl_->pendingRatio = 0;
    if (impl_->hwnd) InvalidateRect(impl_->hwnd, nullptr, FALSE);
}
void NativeDocumentView::Search(const std::wstring& query, int direction) {
    if (impl_->query != query) { impl_->query = query; impl_->FindMatches(); }
    if (!impl_->matches.empty()) {
        if (impl_->activeMatch < 0) impl_->activeMatch = direction < 0 ? static_cast<int>(impl_->matches.size()) - 1 : 0;
        else if (direction) impl_->activeMatch = (impl_->activeMatch + (direction > 0 ? 1 : -1) + static_cast<int>(impl_->matches.size())) % static_cast<int>(impl_->matches.size());
        impl_->RevealMatch();
    }
    if (impl_->hwnd) InvalidateRect(impl_->hwnd, nullptr, FALSE);
}
std::pair<int, int> NativeDocumentView::SearchResult() const { return { impl_->activeMatch + 1, static_cast<int>(impl_->matches.size()) }; }
std::wstring NativeDocumentView::SelectedText() const { return impl_->SelectedText(); }
HWND NativeDocumentView::Handle() const { return impl_->hwnd; }
}
