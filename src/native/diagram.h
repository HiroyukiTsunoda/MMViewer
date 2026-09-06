#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <cstddef>
#include <memory>
#include <string>

namespace mm {
// All dimensions use the 16 px document font as their baseline.
struct DiagramTheme {
    Gdiplus::Color background, text, line, fill, accent;
};
struct Diagram {
    bool valid = false;
    std::wstring error;
    float width = 0, height = 0;
    struct Data;
    std::shared_ptr<Data> data;
};
std::shared_ptr<Diagram> ParseDiagram(const std::string& source);
// Retained object and container capacity, including the full shared Data object.
// Excludes allocator/control-block overhead and process/thread-wide font caches.
// Call once per distinct Diagram/Data allocation when summing shared documents.
std::size_t DiagramMemoryUsage(const Diagram& diagram);
void DrawDiagram(Gdiplus::Graphics& graphics, const Diagram& diagram,
                 float x, float y, float scale, const DiagramTheme& theme);
}
