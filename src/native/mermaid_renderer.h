#pragma once
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <functional>
#include <memory>
#include <string>

namespace mm {
struct MermaidImage {
    bool complete = false, dark = false;
    float width = 480, height = 100;
    std::wstring error;
    std::shared_ptr<Gdiplus::Bitmap> bitmap;
};
// UI/STA-thread API. A single lazy WebView renders queued diagrams offscreen;
// document text and navigation continue through the native renderer.
class MermaidRenderer {
public:
    explicit MermaidRenderer(std::wstring profileDirectory = {});
    ~MermaidRenderer();
    MermaidRenderer(const MermaidRenderer&) = delete;
    MermaidRenderer& operator=(const MermaidRenderer&) = delete;
    std::shared_ptr<MermaidImage> Render(const std::string& source, bool dark, std::function<void()> changed);
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
