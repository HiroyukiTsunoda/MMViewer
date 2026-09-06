#pragma once
#include <windows.h>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace mm {
class NativeDocumentView {
public:
    NativeDocumentView();
    ~NativeDocumentView();
    NativeDocumentView(const NativeDocumentView&) = delete;
    NativeDocumentView& operator=(const NativeDocumentView&) = delete;
    HWND Create(HWND parent, int id);
    void SetDocument(const std::string& source, const std::wstring& path);
    void SetDocument(const std::string& source, const std::wstring& path, int fontPixels, bool sourceMode);
    void InvalidateDocumentCache(const std::wstring& path);
    struct PerformanceStats {
        size_t parses, layouts, cacheHits, cachedDocuments, cachedBytes, bufferAllocations;
    };
    PerformanceStats GetPerformanceStats() const;
    void SetTheme(bool dark);
    void SetFontSize(int pixels);
    int FontSize() const;
    // Between WM_ENTERSIZEMOVE and WM_EXITSIZEMOVE the current layout stays; the width change lays out once at the end.
    void SetLiveResize(bool active);
    void SetSourceMode(bool sourceMode);
    void SetWide(bool wide);
    double ScrollRatio() const;
    void SetScrollRatio(double ratio);
    void ScrollToAnchor(const std::wstring& anchor);
    void Search(const std::wstring& query, int direction);
    std::pair<int, int> SearchResult() const;
    // Text of the current mouse selection in reading order; empty when nothing is selected.
    std::wstring SelectedText() const;
    HWND Handle() const;
    std::function<void(int)> onZoom;
    std::function<void(const std::wstring&)> onLink;
    std::function<void()> onScroll;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
