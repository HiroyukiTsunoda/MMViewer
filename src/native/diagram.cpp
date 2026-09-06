#include "diagram.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace mm {
namespace {
using namespace Gdiplus;
constexpr float pi = 3.14159265359f;
std::string Trim(std::string s) {
    auto a = s.find_first_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
bool Starts(const std::string& s, const std::string& p) { return s.rfind(p, 0) == 0; }
std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    if (!n) throw std::runtime_error("UTF-8として読み取れない図です。");
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), out.data(), n);
    return out;
}
void Replace(std::string& s, const std::string& a, const std::string& b) {
    for (size_t p = 0; (p = s.find(a, p)) != std::string::npos; p += b.size()) s.replace(p, a.size(), b);
}
std::wstring Label(std::string s) {
    s = Trim(s);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    if (s.size() >= 2 && s.front() == '`' && s.back() == '`') s = s.substr(1, s.size() - 2);
    Replace(s, "<br/>", "\n"); Replace(s, "<br />", "\n"); Replace(s, "<br>", "\n");
    Replace(s, "\\n", "\n"); Replace(s, "\\\"", "\""); Replace(s, "&quot;", "\""); Replace(s, "&lt;", "<");
    Replace(s, "&gt;", ">"); Replace(s, "&amp;", "&"); Replace(s, "&#35;", "#");
    return Wide(s);
}
// One parse or draw shares a measuring context. Creating a bitmap, graphics
// and font per uncached character dominated Japanese labels, where nearly
// every character is new. GDI+ objects never live in static or thread_local
// storage here, so none can outlive GdiplusShutdown.
struct Measurer {
    Bitmap bitmap; Graphics graphics; Font font; StringFormat format;
    Measurer() : bitmap(1,1,PixelFormat32bppARGB), graphics(&bitmap), font(L"Yu Gothic UI",16,FontStyleRegular,UnitPixel),
        format(StringFormat::GenericTypographic()) {
        format.SetFormatFlags(StringFormatFlagsNoWrap|StringFormatFlagsMeasureTrailingSpaces);
    }
    Measurer(const Measurer&) = delete;
    Measurer& operator=(const Measurer&) = delete;
    bool Width(wchar_t c,float& width) {
        RectF bounds;
        if(graphics.MeasureString(&c,1,&font,PointF(0,0),&format,&bounds)!=Ok)return false;
        width=std::max(1.0f,bounds.Width+.5f);return true;
    }
};
thread_local Measurer* activeMeasurer = nullptr;
struct MeasureScope {
    Measurer measurer; Measurer* previous;
    MeasureScope() : previous(activeMeasurer) { activeMeasurer = &measurer; }
    ~MeasureScope() { activeMeasurer = previous; }
    MeasureScope(const MeasureScope&) = delete;
    MeasureScope& operator=(const MeasureScope&) = delete;
};
float CharWidth(wchar_t c) {
    // Match the actual document font; an average Latin width makes words such
    // as DocumentManager wrap despite apparently sufficient node padding.
    thread_local std::unordered_map<wchar_t,float> widths;
    if(auto it=widths.find(c);it!=widths.end())return it->second;
    float width=16.0f;bool measured;
    if(activeMeasurer)measured=activeMeasurer->Width(c,width);
    else{Measurer local;measured=local.Width(c,width);}
    if(measured)widths[c]=width; // A failed measurement must not pin its fallback width for the thread.
    return width;
}
std::pair<float, float> TextSize(const std::wstring& s) {
    float maxW = 0, w = 0, h = 22;
    for (wchar_t c : s) {
        if (c == L'\n') { maxW = std::max(maxW, w); w = 0; h += 22; }
        else w += CharWidth(c);
    }
    return { std::max(20.0f, std::max(maxW, w)), h };
}
std::wstring Wrap(std::wstring s, float width = 260) {
    std::wstring out; float w = 0;
    for (wchar_t c : s) {
        if (c == L'\n') { out += c; w = 0; continue; }
        if (w + CharWidth(c) > width) { out += L'\n'; w = 0; }
        out += c; w += CharWidth(c);
    }
    return out;
}
std::vector<std::string> Statements(const std::string& source) {
    std::vector<std::string> out; std::string current; std::vector<char> brackets;
    bool quote = false, escaped = false, sequence = false;
    for (size_t i = 0; i < source.size(); ++i) {
        char c = source[i];
        if (!quote && c == '%' && i + 1 < source.size() && source[i + 1] == '%' && (!sequence || Trim(current).empty())) {
            if (i + 2 < source.size() && source[i + 2] == '{')
                throw std::runtime_error("Mermaidの設定ディレクティブは未対応です。");
            while (i < source.size() && source[i] != '\n') ++i;
            c = '\n';
        }
        if (sequence) {
            if(c=='\n'||c==';'){if(!Trim(current).empty())out.push_back(Trim(current));current.clear();}
            else current+=c;
            continue;
        }
        if (c == '"' && !escaped) quote = !quote;
        if (!quote) {
            if (c == '[' || c == '(' || c == '{') brackets.push_back(c);
            if (c == ']' || c == ')' || c == '}') {
                if (!brackets.empty()) brackets.pop_back();
            }
            if ((c == '\n' || c == ';') && brackets.empty()) {
                if (!Trim(current).empty()) {out.push_back(Trim(current));if(out.size()==1&&out.back()=="sequenceDiagram")sequence=true;}
                current.clear(); escaped = false; continue;
            }
        }
        current += c;
        escaped = c == '\\' && !escaped;
    }
    if (quote || !brackets.empty()) throw std::runtime_error("引用符またはノードの括弧が閉じられていません。");
    if (!Trim(current).empty()) out.push_back(Trim(current));
    return out;
}
enum class Shape { Rect, Rounded, Stadium, Diamond, Circle, DoubleCircle, Hexagon, Subroutine, Cylinder, Parallelogram, Trapezoid };
struct Style { std::optional<Color> fill, stroke, text; std::optional<float> width; std::optional<bool> dashed; };
struct Node { std::string id, className; std::wstring label; Shape shape = Shape::Rect; RectF box; int group = -1, rank = 0; Style style; };
struct Edge {
    int from = 0, to = 0;
    std::wstring label;
    bool arrow = true, startArrow = false, dotted = false, thick = false;
    Style style;
    PointF a,b,c1,c2,labelPoint;
    float labelWidth=0,labelHeight=0; // Measured at layout; drawing uses them for visibility.
    std::vector<PointF> route;
};
struct Group { std::wstring label; int parent = -1; RectF box; };
struct Participant { std::string id; std::wstring label; bool actor = false; float x = 0; };
enum class EventKind { Message, Note, Activate, Deactivate, Block, Else, End, Spacer };
struct Event { EventKind kind; int from = 0, to = 0; std::wstring label; std::string arrow, blockType; int side = 0, depth = 0, end = -1, number = 0; float y = 0, h = 54; };
struct Activation { int participant; float y, bottom; int depth; };
std::optional<Color> ParseColor(std::string value) {
    value = Trim(value);
    static const std::map<std::string, unsigned> colors = {
        {"white",0xffffff},{"black",0x000000},{"red",0xff0000},{"green",0x008000},{"blue",0x0000ff},
        {"yellow",0xffff00},{"orange",0xffa500},{"purple",0x800080},{"pink",0xffc0cb},{"gray",0x808080},
        {"grey",0x808080},{"lightgray",0xd3d3d3},{"lightgrey",0xd3d3d3},{"lightblue",0xadd8e6},
        {"lightgreen",0x90ee90},{"darkgreen",0x006400},{"navy",0x000080},{"teal",0x008080},{"cyan",0x00ffff},
        {"magenta",0xff00ff},{"brown",0xa52a2a},{"indigo",0x4b0082},{"violet",0xee82ee},{"beige",0xf5f5dc}
    };
    if (value == "none" || value == "transparent") return Color(0, 0, 0, 0);
    unsigned rgb = 0;
    if (!value.empty() && value.front() == '#') {
        std::string hex = value.substr(1);
        if (hex.size() == 3) { std::string h; for (char c : hex) { h += c; h += c; } hex = h; }
        if (hex.size() != 6 || !std::all_of(hex.begin(), hex.end(), [](unsigned char c) { return std::isxdigit(c) != 0; }))
            throw std::runtime_error("未対応の色指定です: " + value);
        rgb = std::stoul(hex, nullptr, 16);
    } else {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        auto it = colors.find(value); if (it == colors.end()) throw std::runtime_error("未対応の色指定です: " + value);
        rgb = it->second;
    }
    return Color(255, (BYTE)(rgb >> 16), (BYTE)(rgb >> 8), (BYTE)rgb);
}
Style ParseStyle(const std::string& source) {
    Style result; std::istringstream stream(source); std::string token;
    while (std::getline(stream, token, ',')) {
        size_t colon = token.find(':');
        if (colon == std::string::npos) throw std::runtime_error("スタイル指定を読み取れません: " + token);
        std::string name = Trim(token.substr(0, colon)), value = Trim(token.substr(colon + 1));
        if (name == "fill") result.fill = ParseColor(value);
        else if (name == "stroke") result.stroke = ParseColor(value);
        else if (name == "color") result.text = ParseColor(value);
        else if (name == "stroke-width") {
            static const std::regex widthPattern(R"([0-9]+(?:\.[0-9]+)?(?:px)?)");
            if (!std::regex_match(value, widthPattern)) throw std::runtime_error("線幅を読み取れません。");
            result.width = std::clamp(std::stof(value), 0.0f, 12.0f);
        } else if (name == "stroke-dasharray") result.dashed = value != "0" && value != "none";
        else throw std::runtime_error("未対応のスタイルです: " + name);
    }
    return result;
}
void MergeStyle(Style& a, const Style& b) {
    if (b.fill) a.fill = b.fill; if (b.stroke) a.stroke = b.stroke; if (b.text) a.text = b.text;
    if (b.width) a.width = b.width; if (b.dashed) a.dashed = b.dashed;
}
}

struct Diagram::Data {
    bool sequence = false, horizontal = false, reverse = false, autonumber = false;
    int sequenceDepth = 0;
    std::wstring title;
    std::vector<Node> nodes;
    std::vector<Edge> edges;
    std::vector<Group> groups;
    std::vector<Participant> participants;
    std::vector<Event> events;
    std::vector<Activation> activations;
};

namespace {
template<class Char>
std::size_t StringAllocationBytes(const std::basic_string<Char>& value) {
    // Inline short-string storage is already included in the owning object.
    const auto object = reinterpret_cast<std::uintptr_t>(&value);
    const auto buffer = reinterpret_cast<std::uintptr_t>(value.data());
    if (buffer >= object && buffer - object < sizeof(value)) return 0;
    return (value.capacity() + 1) * sizeof(Char);
}

class FlowParser {
    Diagram::Data& d;
    std::unordered_map<std::string, int> ids;
    std::map<std::string, Style> classes;
    std::vector<int> groupStack;
    int NodeIndex(const std::string& id) {
        auto it = ids.find(id); if (it != ids.end()) return it->second;
        if (d.nodes.size() >= 500) throw std::runtime_error("ノード数が上限の500個を超えています。");
        int i = (int)d.nodes.size(); ids[id] = i;
        Node n; n.id = id; n.label = Wide(id); n.group = groupStack.empty() ? -1 : groupStack.back();
        d.nodes.push_back(n); return i;
    }
    static void Space(const std::string& s, size_t& p) { while (p < s.size() && std::isspace((unsigned char)s[p])) ++p; }
    int ReadNode(const std::string& s, size_t& p) {
        Space(s, p); size_t start = p;
        while (p < s.size()) {
            unsigned char c = (unsigned char)s[p];
            if (std::isalnum(c) || c == '_' || c >= 128 || (c == '-' && p + 1 < s.size() && std::isalnum((unsigned char)s[p + 1]))) ++p;
            else break;
        }
        if (p == start) throw std::runtime_error("ノードIDを読み取れません: " + s.substr(p, 60));
        int index = NodeIndex(s.substr(start, p - start)); Space(s, p);
        struct Delimiter { const char* open; const char* close; Shape shape; };
        static const Delimiter delimiters[] = {
            {"(((",")))",Shape::DoubleCircle},{"([","])",Shape::Stadium},{"[[","]]",Shape::Subroutine},
            {"[(",")]",Shape::Cylinder},{"((","))",Shape::Circle},{"{{","}}",Shape::Hexagon},
            {"[/","/]",Shape::Parallelogram},{"[/","\\]",Shape::Trapezoid},
            {"[","]",Shape::Rect},{"(",")",Shape::Rounded},{"{","}",Shape::Diamond}
        };
        for (const auto& delimiter : delimiters) {
            std::string open = delimiter.open, close = delimiter.close;
            if (s.compare(p, open.size(), open) != 0) continue;
            size_t begin = p + open.size(), end = begin; bool quote = false, escape = false;
            for (; end < s.size(); ++end) {
                char c = s[end]; if (c == '"' && !escape) quote = !quote;
                if (!quote && s.compare(end, close.size(), close) == 0) break;
                escape = c == '\\' && !escape;
            }
            if (end == s.size()) {
                if (open == "[/") continue;
                throw std::runtime_error("ノードの括弧が閉じられていません。");
            }
            d.nodes[index].shape = delimiter.shape;
            d.nodes[index].label = Wrap(Label(s.substr(begin, end - begin)));
            p = end + close.size(); break;
        }
        Space(s, p);
        if (s.compare(p, 3, ":::") == 0) {
            p += 3; size_t begin = p;
            while (p < s.size() && (std::isalnum((unsigned char)s[p]) || s[p] == '_')) ++p;
            if (p == begin) throw std::runtime_error("class名がありません。");
            d.nodes[index].className = s.substr(begin, p - begin);
        }
        if (!groupStack.empty()) d.nodes[index].group = groupStack.back();
        return index;
    }
    Edge ReadEdge(const std::string& s, size_t& p) {
        Space(s, p); Edge e;
        static const std::vector<std::string> operators = { "<-->", "<==>", "-.->", "-.-", "==>", "===", "-->", "---", "--", "-." };
        std::string op;
        for (const auto& candidate : operators) if (s.compare(p, candidate.size(), candidate) == 0) { op = candidate; p += op.size(); break; }
        if (op.empty()) throw std::runtime_error("未対応の接続構文です: " + s.substr(p, 50));
        if (op == "--" || op == "-.") {
            std::string close = op == "--" ? "-->" : ".->";
            size_t end = s.find(close, p);
            if (end == std::string::npos) throw std::runtime_error("接続のラベルが閉じられていません。");
            e.label = Wrap(Label(s.substr(p, end - p)), 200); p = end + close.size();
            e.dotted = op == "-.";
        } else {
            e.arrow = op.find('>') != std::string::npos; e.startArrow = op.front() == '<';
            e.dotted = op.find('.') != std::string::npos; e.thick = op.find('=') != std::string::npos;
        }
        Space(s, p);
        if (p < s.size() && s[p] == '|') {
            size_t end = s.find('|', p + 1); if (end == std::string::npos) throw std::runtime_error("接続ラベルの | が閉じられていません。");
            e.label = Wrap(Label(s.substr(p + 1, end - p - 1)), 200); p = end + 1;
        }
        return e;
    }
public:
    explicit FlowParser(Diagram::Data& data) : d(data) {}
    void Parse(const std::vector<std::string>& statements) {
        for (size_t line = 1; line < statements.size(); ++line) {
            const auto& s = statements[line];
            if (Starts(s, "subgraph ")) {
                std::string label = Trim(s.substr(9)); size_t open = label.find('[');
                if (open != std::string::npos && label.back() == ']') label = label.substr(open + 1, label.size() - open - 2);
                Group g; g.label = Label(label); g.parent = groupStack.empty() ? -1 : groupStack.back();
                groupStack.push_back((int)d.groups.size()); d.groups.push_back(g); continue;
            }
            if (s == "end") {
                if (groupStack.empty()) throw std::runtime_error("対応するsubgraphのないendです。");
                groupStack.pop_back(); continue;
            }
            if (Starts(s, "direction ")) throw std::runtime_error("subgraph内のdirection指定は未対応です。");
            if (Starts(s, "classDef ")) {
                size_t split = s.find(' ', 9); if (split == std::string::npos) throw std::runtime_error("classDefのスタイルがありません。");
                std::istringstream names(s.substr(9, split - 9)); std::string name;
                Style style = ParseStyle(s.substr(split + 1)); while (std::getline(names, name, ',')) classes[Trim(name)] = style;
                continue;
            }
            if (Starts(s, "style ")) {
                size_t split = s.find(' ', 6); if (split == std::string::npos) throw std::runtime_error("styleの指定がありません。");
                auto it = ids.find(s.substr(6, split - 6)); if (it == ids.end()) throw std::runtime_error("styleの対象ノードがありません。");
                MergeStyle(d.nodes[it->second].style, ParseStyle(s.substr(split + 1))); continue;
            }
            if (Starts(s, "class ")) {
                size_t split = s.find(' ', 6); if (split == std::string::npos) throw std::runtime_error("class名がありません。");
                std::istringstream names(s.substr(6, split - 6)); std::string name;
                while (std::getline(names, name, ',')) d.nodes[NodeIndex(Trim(name))].className = Trim(s.substr(split + 1));
                continue;
            }
            if (Starts(s, "linkStyle ")) {
                size_t split = s.find(' ', 10); if (split == std::string::npos) throw std::runtime_error("linkStyleが不正です。");
                std::string targets = s.substr(10, split - 10); Style st = ParseStyle(s.substr(split + 1));
                if (targets == "default") for (auto& e : d.edges) e.style = st;
                else {
                    std::istringstream indices(targets); std::string token;
                    while (std::getline(indices, token, ',')) {
                        if (token.empty() || !std::all_of(token.begin(), token.end(), [](unsigned char c) { return std::isdigit(c); })) throw std::runtime_error("linkStyleの接続番号が不正です。");
                        size_t n = std::stoul(token); if (n >= d.edges.size()) throw std::runtime_error("linkStyleの接続番号が範囲外です。");
                        d.edges[n].style = st;
                    }
                }
                continue;
            }
            if (Starts(s, "click ")) throw std::runtime_error("clickによる操作は未対応です。");
            size_t p = 0; int from = ReadNode(s, p); Space(s, p);
            while (p < s.size()) {
                Edge edge = ReadEdge(s, p); edge.from = from; edge.to = ReadNode(s, p); from = edge.to;
                if (d.edges.size() >= 500) throw std::runtime_error("接続数が上限の500本を超えています。");
                d.edges.push_back(edge); Space(s, p);
            }
        }
        if (!groupStack.empty()) throw std::runtime_error("subgraphに対応するendがありません。");
        if (d.nodes.empty()) throw std::runtime_error("図にノードがありません。");
        for (auto& n : d.nodes) {
            Style original = n.style;
            auto base = classes.find("default"); if (base != classes.end()) n.style = base->second;
            if (!n.className.empty()) {
                auto it = classes.find(n.className); if (it == classes.end()) throw std::runtime_error("定義されていないclassです: " + n.className);
                MergeStyle(n.style, it->second);
            }
            if (original.fill || original.stroke || original.text || original.width || original.dashed) MergeStyle(n.style, original);
        }
    }
};

void LayoutFlow(Diagram& diagram) {
    auto& d = *diagram.data; size_t n = d.nodes.size();
    std::vector<std::vector<int>> adjacency(n); std::vector<int> indegree(n, 0), order;
    for (const auto& e : d.edges) if (e.from != e.to) adjacency[e.from].push_back(e.to);
    // Break only DFS back edges; keep every original edge for drawing.
    std::vector<int> visited(n); std::set<std::pair<int,int>> backEdges;
    std::function<void(int)> visit = [&](int from) {
        visited[from] = 1;
        for (int to : adjacency[from]) {
            if (visited[to] == 1) backEdges.emplace(from, to);
            else if (!visited[to]) visit(to);
        }
        visited[from] = 2;
    };
    for (int i = 0; i < (int)n; ++i) if (!visited[i]) visit(i);
    for (int from = 0; from < (int)n; ++from) for (int to : adjacency[from]) if (!backEdges.count({from,to})) ++indegree[to];
    for (int i = 0; i < (int)n; ++i) if (!indegree[i]) order.push_back(i);
    for (size_t i = 0; i < order.size(); ++i) for (int to : adjacency[order[i]]) {
        if (backEdges.count({order[i],to})) continue;
        d.nodes[to].rank = std::max(d.nodes[to].rank, d.nodes[order[i]].rank + 1);
        if (--indegree[to] == 0) order.push_back(to);
    }
    int maxRank = 0;
    for (auto& node : d.nodes) {
        auto [w,h] = TextSize(node.label); w += 40; h += 26;
        if (node.shape == Shape::Diamond) { w = std::max(100.0f,w * 1.3f); h = std::max(78.0f,h * 1.6f); }
        if (node.shape == Shape::Circle || node.shape == Shape::DoubleCircle) w = h = std::max(w,h) + 10;
        if (node.shape == Shape::Cylinder) h += 18;
        if (node.shape == Shape::Hexagon || node.shape == Shape::Parallelogram) w += 26;
        node.box = RectF(0,0,std::max(80.0f,w),std::max(48.0f,h)); maxRank = std::max(maxRank,node.rank);
    }
    std::vector<std::vector<int>> ranks(maxRank+1);
    for (int i = 0; i < (int)n; ++i) ranks[d.nodes[i].rank].push_back(i);
    std::vector<float> primary(maxRank+1,0), secondary(maxRank+1,0), gap(maxRank+1,108);
    for (const auto& e:d.edges) if(!e.label.empty()) {
        auto [w,h]=TextSize(e.label);
        gap[d.nodes[e.from].rank]=std::max(gap[d.nodes[e.from].rank],(d.horizontal?w:h)+38);
    }
    float totalPrimary = 0, totalSecondary = 0;
    for (int r = 0; r <= maxRank; ++r) {
        // Stable grouping reduces group-box overlap while retaining source order.
        std::stable_sort(ranks[r].begin(),ranks[r].end(),[&](int a,int b){return d.nodes[a].group < d.nodes[b].group;});
        for (int i : ranks[r]) {
            auto box=d.nodes[i].box; primary[r]=std::max(primary[r],d.horizontal?box.Width:box.Height);
            secondary[r]+=(d.horizontal?box.Height:box.Width)+64;
        }
        secondary[r]-=64; totalSecondary=std::max(totalSecondary,secondary[r]);
        totalPrimary += primary[r] + gap[r];
    }
    totalPrimary -= gap[maxRank]; float p = 56;
    for (int r = 0; r <= maxRank; ++r) {
        float s = 56 + (totalSecondary-secondary[r])/2;
        for (int i : ranks[r]) {
            auto& box=d.nodes[i].box; float dimP=d.horizontal?box.Width:box.Height;
            float posP = d.reverse ? 56+totalPrimary-(p-56)-primary[r]+(primary[r]-dimP)/2 : p+(primary[r]-dimP)/2;
            if (d.horizontal) { box.X=posP;box.Y=s; s+=box.Height+64; }
            else { box.X=s;box.Y=posP; s+=box.Width+64; }
        }
        p += primary[r]+gap[r];
    }
    for (int g = (int)d.groups.size()-1; g >= 0; --g) {
        float l=1e9f,t=1e9f,r=-1e9f,b=-1e9f;
        auto add=[&](const RectF& box){l=std::min(l,box.X);t=std::min(t,box.Y);r=std::max(r,box.GetRight());b=std::max(b,box.GetBottom());};
        for (const auto& node:d.nodes) if(node.group==g) add(node.box);
        for (const auto& child:d.groups) if(child.parent==g) add(child.box);
        if(r<l) throw std::runtime_error("空のsubgraphは表示できません。");
        d.groups[g].box=RectF(l-22,t-38,r-l+44,b-t+60);
    }
    diagram.width=(d.horizontal?totalPrimary:totalSecondary)+112;
    diagram.height=(d.horizontal?totalSecondary:totalPrimary)+112;
    for (const auto& g:d.groups) { diagram.width=std::max(diagram.width,g.box.GetRight()+28);diagram.height=std::max(diagram.height,g.box.GetBottom()+28); }
    // Route feedback and rank-skipping connections outside every node. Record
    // exact geometry once so drawing and reported bounds cannot drift apart.
    float outerX=0,outerY=0,innerX=1e9f,innerY=1e9f;
    for(const auto& node:d.nodes){outerX=std::max(outerX,node.box.GetRight());outerY=std::max(outerY,node.box.GetBottom());innerX=std::min(innerX,node.box.X);innerY=std::min(innerY,node.box.Y);}
    int externalRoute=0;
    for(auto& e:d.edges) {
        const auto& from=d.nodes[e.from];const auto& to=d.nodes[e.to];
        PointF fc{from.box.X+from.box.Width/2,from.box.Y+from.box.Height/2};
        PointF tc{to.box.X+to.box.Width/2,to.box.Y+to.box.Height/2};float sign=d.reverse?-1.0f:1.0f;
        if(e.from==e.to) {
            e.a={from.box.GetRight(),fc.Y-8};e.b={from.box.GetRight(),fc.Y+12};
            e.c1={e.a.X+65,e.a.Y-65};e.c2={e.b.X+65,e.b.Y+65};
        } else if(to.rank<=from.rank||to.rank>from.rank+1) {
            float margin=48+(externalRoute++%12)*18.0f;
            if(d.horizontal) {
                float side=fc.Y<(innerY+outerY)/2?innerY-margin:outerY+margin;
                e.a={fc.X+sign*from.box.Width/2,fc.Y};e.b={tc.X-sign*to.box.Width/2,tc.Y};
                e.route={e.a,{e.a.X+sign*28,e.a.Y},{e.a.X+sign*28,side},{e.b.X-sign*28,side},{e.b.X-sign*28,e.b.Y},e.b};
                e.labelPoint={(e.a.X+e.b.X)/2,side};
            } else {
                float side=fc.X<(innerX+outerX)/2?innerX-margin:outerX+margin;
                e.a={fc.X,fc.Y+sign*from.box.Height/2};e.b={tc.X,tc.Y-sign*to.box.Height/2};
                e.route={e.a,{e.a.X,e.a.Y+sign*28},{side,e.a.Y+sign*28},{side,e.b.Y-sign*28},{e.b.X,e.b.Y-sign*28},e.b};
                e.labelPoint={side,(e.a.Y+e.b.Y)/2};
            }
            e.c1=e.route[1];e.c2=e.route[e.route.size()-2];
        } else if(d.horizontal) {
            e.a={fc.X+sign*from.box.Width/2,fc.Y};e.b={tc.X-sign*to.box.Width/2,tc.Y};float mid=(e.a.X+e.b.X)/2;
            e.c1={mid,e.a.Y};e.c2={mid,e.b.Y};
        } else {
            e.a={fc.X,fc.Y+sign*from.box.Height/2};e.b={tc.X,tc.Y-sign*to.box.Height/2};float mid=(e.a.Y+e.b.Y)/2;
            e.c1={e.a.X,mid};e.c2={e.b.X,mid};
        }
        if(e.route.empty())e.labelPoint={(e.a.X+3*e.c1.X+3*e.c2.X+e.b.X)/8,(e.a.Y+3*e.c1.Y+3*e.c2.Y+e.b.Y)/8};
    }
    float left=0,top=0,right=diagram.width-24,bottom=diagram.height-24;
    auto include=[&](const RectF& r){left=std::min(left,r.X);top=std::min(top,r.Y);right=std::max(right,r.GetRight());bottom=std::max(bottom,r.GetBottom());};
    for(const auto& group:d.groups)include(group.box);
    for(auto& e:d.edges) {
        for(auto point:{e.a,e.b,e.c1,e.c2})include(RectF(point.X-8,point.Y-8,16,16));
        for(auto point:e.route)include(RectF(point.X-8,point.Y-8,16,16));
        if(!e.label.empty()){auto[w,h]=TextSize(e.label);e.labelWidth=w;e.labelHeight=h;include(RectF(e.labelPoint.X-w/2-8,e.labelPoint.Y-h/2-5,w+16,h+10));}
    }
    float dx=left<24?24-left:0,dy=top<24?24-top:0;
    for(auto& node:d.nodes){node.box.X+=dx;node.box.Y+=dy;}
    for(auto& group:d.groups){group.box.X+=dx;group.box.Y+=dy;}
    for(auto& e:d.edges)for(auto point:{&e.a,&e.b,&e.c1,&e.c2,&e.labelPoint}){point->X+=dx;point->Y+=dy;}
    for(auto& e:d.edges)for(auto& point:e.route){point.X+=dx;point.Y+=dy;}
    diagram.width=right+dx+24;diagram.height=bottom+dy+24;
}

void ParseSequence(Diagram& diagram,const std::vector<std::string>& statements) {
    auto& d=*diagram.data; d.sequence=true; std::unordered_map<std::string,int> ids;
    auto participant=[&](std::string id)->int {
        id=Trim(id);if(id.empty()) throw std::runtime_error("参加者IDがありません。");
        static const std::regex idPattern(R"([^\s:,+;<>]+)"); // Compiled once, not per message endpoint.
        if(!std::regex_match(id,idPattern)) throw std::runtime_error("参加者IDが不正です: "+id);
        auto it=ids.find(id);if(it!=ids.end())return it->second;
        if(d.participants.size()>=80)throw std::runtime_error("参加者数が上限の80人を超えています。");
        int i=(int)d.participants.size();ids[id]=i;d.participants.push_back({id,Label(id),false,0});return i;
    };
    std::vector<int> blocks; int number=0;
    for(size_t i=1;i<statements.size();++i) {
        auto s=statements[i];Event e{EventKind::Message};
        if(Starts(s,"participant ")||Starts(s,"actor ")) {
            bool actor=Starts(s,"actor ");auto text=Trim(s.substr(actor?6:12));size_t as=text.find(" as ");
            int p=participant(as==std::string::npos?text:text.substr(0,as));
            d.participants[p].actor=actor;if(as!=std::string::npos)d.participants[p].label=Wrap(Label(text.substr(as+4)),180);continue;
        }
        if(s=="autonumber") {d.autonumber=true;continue;}
        if(Starts(s,"title ")||Starts(s,"title:")) {d.title=Label(s.substr(6));continue;}
        if(Starts(s,"activate ")||Starts(s,"deactivate ")) {
            bool active=Starts(s,"activate ");e.kind=active?EventKind::Activate:EventKind::Deactivate;
            e.from=participant(s.substr(active?9:11));e.h=0;d.events.push_back(e);continue;
        }
        if(Starts(s,"Note ")||Starts(s,"note ")) {
            auto colon=s.find(':');if(colon==std::string::npos)throw std::runtime_error("Noteに本文がありません。");
            auto target=Trim(s.substr(5,colon-5));e.kind=EventKind::Note;e.label=Wrap(Label(s.substr(colon+1)),280);
            if(Starts(target,"left of ")) {e.side=-1;e.from=e.to=participant(target.substr(8));}
            else if(Starts(target,"right of ")) {e.side=1;e.from=e.to=participant(target.substr(9));}
            else if(Starts(target,"over ")) {auto rest=target.substr(5);auto comma=rest.find(',');e.from=participant(rest.substr(0,comma));e.to=comma==std::string::npos?e.from:participant(rest.substr(comma+1));}
            else throw std::runtime_error("未対応のNoteの位置です。");
            e.h=TextSize(e.label).second+38;d.events.push_back(e);continue;
        }
        bool block=false;
        for(const auto& name:{"alt","opt","loop","par","critical","break","rect"}) {
            std::string prefix=name;
            if(s==prefix||Starts(s,prefix+" ")) {
                if(prefix=="rect") throw std::runtime_error("sequenceのrect背景色指定は未対応です。");
                e.kind=EventKind::Block;e.blockType=prefix;e.label=Label(s.substr(prefix.size()));e.depth=(int)blocks.size();e.h=46;
                d.sequenceDepth=std::max(d.sequenceDepth,e.depth);
                blocks.push_back((int)d.events.size());d.events.push_back(e);block=true;break;
            }
        }
        if(block)continue;
        if(s=="else"||Starts(s,"else ")||s=="and"||Starts(s,"and ")||Starts(s,"option ")) {
            if(blocks.empty())throw std::runtime_error("ブロック外のelse / andです。");
            e.kind=EventKind::Else;e.from=blocks.back();e.depth=(int)blocks.size()-1;e.label=Label(s.substr(s.find(' ')==std::string::npos?s.size():s.find(' ')+1));e.h=46;
            d.events.push_back(e);continue;
        }
        if(s=="end") {
            if(blocks.empty())throw std::runtime_error("対応するブロックのないendです。");
            e.kind=EventKind::End;e.h=24;e.from=blocks.back();d.events[blocks.back()].end=(int)d.events.size();blocks.pop_back();d.events.push_back(e);continue;
        }
        if(Starts(s,"%%"))continue;
        size_t colon=s.find(':');if(colon==std::string::npos)throw std::runtime_error("未対応のsequence構文です: "+s.substr(0,80));
        auto routing=Trim(s.substr(0,colon));size_t arrowAt=std::string::npos;std::string arrow;
        for(const auto& token:{"-->>","->>","-->","--x","--)","->","-x","-)"}) {
            auto pos=routing.find(token);if(pos!=std::string::npos&&(arrowAt==std::string::npos||pos<arrowAt)) {arrowAt=pos;arrow=token;}
        }
        if(arrowAt==std::string::npos)throw std::runtime_error("未対応のメッセージ矢印です。");
        e.from=participant(routing.substr(0,arrowAt));auto recipient=Trim(routing.substr(arrowAt+arrow.size()));
        char activation=0;if(!recipient.empty()&&(recipient[0]=='+'||recipient[0]=='-')){activation=recipient[0];recipient=Trim(recipient.substr(1));}
        e.to=participant(recipient);e.arrow=arrow;e.label=Wrap(Label(s.substr(colon+1)),e.from==e.to?220.0f:460.0f);e.h=TextSize(e.label).second+36;
        if(e.from==e.to)e.h+=22;
        e.number=++number;d.events.push_back(e);
        if(activation) {Event a{activation=='+'?EventKind::Activate:EventKind::Deactivate};a.from=activation=='+'?e.to:e.from;a.h=0;d.events.push_back(a);}
        if(d.events.size()>2000)throw std::runtime_error("sequenceの行数が上限の2000行を超えています。");
    }
    if(!blocks.empty())throw std::runtime_error("sequenceのブロックを閉じるendがありません。");
    if(d.participants.empty())throw std::runtime_error("図に参加者がありません。");
    float gap=200, left=std::max(100.0f,40+d.sequenceDepth*12.0f);
    for(const auto& e:d.events) {
        if(e.kind==EventKind::Message&&e.from!=e.to)gap=std::max(gap,(TextSize(e.label).first+52)/std::abs(e.from-e.to));
        if(e.kind==EventKind::Note&&e.side<0&&e.from==0)left=std::max(left,TextSize(e.label).first+66);
        if(e.kind==EventKind::Note&&e.side==0&&e.from==0&&e.to==0)left=std::max(left,TextSize(e.label).first/2+10);
    }
    for(size_t i=0;i<d.participants.size();++i)d.participants[i].x=left+70+(float)i*gap;
    float y=d.title.empty()?122.0f:160.0f;
    std::vector<std::vector<int>> activations(d.participants.size());
    for(auto& e:d.events) {
        e.y=y;
        if(e.kind==EventKind::Activate) {
            auto& stack=activations[e.from];stack.push_back((int)d.activations.size());
            d.activations.push_back({e.from,y-12,0,(int)stack.size()-1});
        } else if(e.kind==EventKind::Deactivate) {
            auto& stack=activations[e.from];if(stack.empty())throw std::runtime_error("有効なactivateのないdeactivateです。");
            d.activations[stack.back()].bottom=y-12;stack.pop_back();
        }
        y+=e.h;
    }
    for(const auto& stack:activations)for(int i:stack)d.activations[i].bottom=y;
    bool anyActor=std::any_of(d.participants.begin(),d.participants.end(),[](const auto& p){return p.actor;});
    diagram.width=d.participants.back().x+180+d.sequenceDepth*12.0f;diagram.height=y+(anyActor?140:100);
    for(const auto& e:d.events)if(e.kind==EventKind::Note&&e.side>0)diagram.width=std::max(diagram.width,d.participants[e.from].x+TextSize(e.label).first+70);
    for(const auto& e:d.events)if(e.kind==EventKind::Message&&e.from==e.to)diagram.width=std::max(diagram.width,d.participants[e.from].x+266);
}

// Fonts and the string format live for one DrawDiagram call. A flowchart with
// hundreds of nodes otherwise created a font per label on every repaint.
struct DrawContext {
    std::map<unsigned,std::unique_ptr<Font>> fonts; StringFormat format;
    // The device area in diagram coordinates. Elements entirely outside it are
    // not drawn; bounds are generous, so a partly visible element always is.
    RectF visible; bool clip=false;
    bool Shows(const RectF& r,float margin=8) const {
        return !clip||(r.X-margin<visible.GetRight()&&r.GetRight()+margin>visible.X&&r.Y-margin<visible.GetBottom()&&r.GetBottom()+margin>visible.Y);
    }
    bool Shows(float x1,float y1,float x2,float y2,float margin=8) const {return Shows(RectF(std::min(x1,x2),std::min(y1,y2),std::abs(x2-x1),std::abs(y2-y1)),margin);}
    DrawContext() : format(StringFormat::GenericTypographic()) {
        format.SetLineAlignment(StringAlignmentCenter);
        format.SetFormatFlags(StringFormatFlagsNoClip|StringFormatFlagsNoWrap);format.SetTrimming(StringTrimmingNone);
    }
    Font& GetFont(float size,FontStyle style) {
        auto& font=fonts[static_cast<unsigned>(size*10)*16+static_cast<unsigned>(style&15)];
        if(!font)font=std::make_unique<Font>(L"Yu Gothic UI",size,style,UnitPixel);
        return *font;
    }
};
void Text(Graphics& g,DrawContext& context,const std::wstring& text,const RectF& box,Color color,float size=16,StringAlignment align=StringAlignmentCenter,FontStyle style=FontStyleRegular) {
    SolidBrush brush(color);context.format.SetAlignment(align);
    g.DrawString(text.c_str(),(INT)text.size(),&context.GetFont(size,style),box,&context.format,&brush);
}
void RoundPath(GraphicsPath& path,const RectF& r,float radius) {
    radius=std::min(radius,std::min(r.Width,r.Height)/2);float d=radius*2;
    path.AddArc(r.X,r.Y,d,d,180,90);path.AddArc(r.GetRight()-d,r.Y,d,d,270,90);
    path.AddArc(r.GetRight()-d,r.GetBottom()-d,d,d,0,90);path.AddArc(r.X,r.GetBottom()-d,d,d,90,90);path.CloseFigure();
}
void Arrow(Graphics& g,PointF at,PointF from,Color color,bool filled=true,float size=9) {
    float a=std::atan2(at.Y-from.Y,at.X-from.X);PointF points[]={at,{at.X-size*std::cos(a-.45f),at.Y-size*std::sin(a-.45f)},{at.X-size*std::cos(a+.45f),at.Y-size*std::sin(a+.45f)}};
    if(filled){SolidBrush brush(color);g.FillPolygon(&brush,points,3);}else{Pen pen(color,1.5f);g.DrawLine(&pen,points[0],points[1]);g.DrawLine(&pen,points[0],points[2]);}
}
PointF Center(const RectF& b){return {b.X+b.Width/2,b.Y+b.Height/2};}
void DrawNode(Graphics& g,DrawContext& context,const Node& n,const DiagramTheme& theme) {
    auto r=n.box;GraphicsPath path;auto center=Center(r);
    switch(n.shape) {
    case Shape::Rounded:RoundPath(path,r,9);break;
    case Shape::Stadium:RoundPath(path,r,r.Height/2);break;
    case Shape::Circle:case Shape::DoubleCircle:path.AddEllipse(r);break;
    case Shape::Diamond:{PointF p[]={{center.X,r.Y},{r.GetRight(),center.Y},{center.X,r.GetBottom()},{r.X,center.Y}};path.AddPolygon(p,4);break;}
    case Shape::Hexagon:{PointF p[]={{r.X+18,r.Y},{r.GetRight()-18,r.Y},{r.GetRight(),center.Y},{r.GetRight()-18,r.GetBottom()},{r.X+18,r.GetBottom()},{r.X,center.Y}};path.AddPolygon(p,6);break;}
    case Shape::Parallelogram:{PointF p[]={{r.X+18,r.Y},{r.GetRight(),r.Y},{r.GetRight()-18,r.GetBottom()},{r.X,r.GetBottom()}};path.AddPolygon(p,4);break;}
    case Shape::Trapezoid:{PointF p[]={{r.X+18,r.Y},{r.GetRight()-18,r.Y},{r.GetRight(),r.GetBottom()},{r.X,r.GetBottom()}};path.AddPolygon(p,4);break;}
    case Shape::Cylinder:
        path.AddArc(r.X,r.Y,r.Width,22.0f,180.0f,180.0f);path.AddLine(r.GetRight(),r.Y+11,r.GetRight(),r.GetBottom()-11);
        path.AddArc(r.X,r.GetBottom()-22,r.Width,22.0f,0.0f,180.0f);path.CloseFigure();break;
    default:path.AddRectangle(r);break;
    }
    SolidBrush fill(n.style.fill.value_or(theme.fill));Pen pen(n.style.stroke.value_or(theme.accent),n.style.width.value_or(1.5f));if(n.style.dashed.value_or(false))pen.SetDashStyle(DashStyleDash);
    g.FillPath(&fill,&path);g.DrawPath(&pen,&path);
    if(n.shape==Shape::DoubleCircle)g.DrawEllipse(&pen,r.X+6,r.Y+6,r.Width-12,r.Height-12);
    if(n.shape==Shape::Subroutine){g.DrawLine(&pen,r.X+8,r.Y,r.X+8,r.GetBottom());g.DrawLine(&pen,r.GetRight()-8,r.Y,r.GetRight()-8,r.GetBottom());}
    if(n.shape==Shape::Cylinder){g.DrawArc(&pen,r.X,r.Y,r.Width,22.0f,0.0f,180.0f);r.Y+=10;r.Height-=10;}
    Text(g,context,n.label,RectF(r.X+10,r.Y+5,r.Width-20,r.Height-10),n.style.text.value_or(theme.text));
}
RectF EdgeBounds(const Edge& e) {
    float l=1e9f,t=1e9f,r=-1e9f,b=-1e9f;
    const auto add=[&](PointF p,float pad){l=std::min(l,p.X-pad);t=std::min(t,p.Y-pad);r=std::max(r,p.X+pad);b=std::max(b,p.Y+pad);};
    for(auto p:{e.a,e.b,e.c1,e.c2})add(p,10);for(auto p:e.route)add(p,10);
    if(!e.label.empty()){ // The label's background rectangle, as DrawFlow draws it.
        l=std::min(l,e.labelPoint.X-e.labelWidth/2-5);r=std::max(r,e.labelPoint.X+e.labelWidth/2+5);
        t=std::min(t,e.labelPoint.Y-e.labelHeight/2-3);b=std::max(b,e.labelPoint.Y+e.labelHeight/2+3);
    }
    return RectF(l,t,r-l,b-t);
}
void DrawFlow(Graphics& g,DrawContext& context,const Diagram& diagram,const DiagramTheme& theme) {
    const auto& d=*diagram.data;
    for(const auto& group:d.groups) {
        if(!context.Shows(group.box))continue;
        SolidBrush fill(Color(16,theme.accent.GetR(),theme.accent.GetG(),theme.accent.GetB()));Pen pen(theme.line,1);pen.SetDashStyle(DashStyleDash);
        g.FillRectangle(&fill,group.box);g.DrawRectangle(&pen,group.box);
        Text(g,context,group.label,RectF(group.box.X+10,group.box.Y+4,group.box.Width-20,26),theme.text,14,StringAlignmentNear,FontStyleBold);
    }
    for(const auto& e:d.edges) {
        if(!context.Shows(EdgeBounds(e)))continue;
        Color color=e.style.stroke.value_or(theme.line);Pen pen(color,e.thick?3.0f:e.style.width.value_or(1.5f));if(e.dotted||e.style.dashed.value_or(false))pen.SetDashStyle(DashStyleDash);
        if(e.route.empty())g.DrawBezier(&pen,e.a,e.c1,e.c2,e.b);else g.DrawLines(&pen,e.route.data(),(INT)e.route.size());
        if(e.arrow)Arrow(g,e.b,e.c2,color);if(e.startArrow)Arrow(g,e.a,e.c1,color);
        if(!e.label.empty()) {
            auto[w,h]=TextSize(e.label);RectF r(e.labelPoint.X-w/2-5,e.labelPoint.Y-h/2-3,w+10,h+6);SolidBrush bg(theme.background);g.FillRectangle(&bg,r);
            Text(g,context,e.label,r,e.style.text.value_or(theme.text),14);
        }
    }
    for(const auto& node:d.nodes)if(context.Shows(node.box))DrawNode(g,context,node,theme);
}
void DrawParticipant(Graphics& g,DrawContext& context,const Participant& p,float y,const DiagramTheme& theme) {
    Pen pen(theme.line,1.5f);SolidBrush fill(theme.fill);RectF r(p.x-70,y,140,52);
    if(!p.actor){g.FillRectangle(&fill,r);g.DrawRectangle(&pen,r);Text(g,context,p.label,r,theme.text);}
    else {
        g.DrawEllipse(&pen,p.x-9,y,18.0f,18.0f);g.DrawLine(&pen,p.x,y+18,p.x,y+40);
        g.DrawLine(&pen,p.x-18,y+25,p.x+18,y+25);g.DrawLine(&pen,p.x,y+40,p.x-14,y+54);g.DrawLine(&pen,p.x,y+40,p.x+14,y+54);
        Text(g,context,p.label,RectF(p.x-90,y+57,180,26),theme.text,15);
    }
}
void DrawSequence(Graphics& g,DrawContext& context,const Diagram& diagram,const DiagramTheme& theme) {
    const auto& d=*diagram.data;float top=d.title.empty()?22.0f:60.0f;
    bool anyActor=std::any_of(d.participants.begin(),d.participants.end(),[](const auto& p){return p.actor;});
    float bottom=diagram.height-(anyActor?112:78);
    Pen line(theme.line,1.5f),dashed(theme.line,1);dashed.SetDashStyle(DashStyleDash);
    if(!d.title.empty())Text(g,context,d.title,RectF(10,8,diagram.width-20,36),theme.text,19,StringAlignmentCenter,FontStyleBold);
    for(const auto& p:d.participants){
        if(context.Shows(p.x,top,p.x,bottom))g.DrawLine(&dashed,p.x,top+(p.actor?88:52),p.x,bottom);
        if(context.Shows(RectF(p.x-90,top,180,112)))DrawParticipant(g,context,p,top,theme);
        if(context.Shows(RectF(p.x-90,bottom,180,112)))DrawParticipant(g,context,p,bottom,theme);
    }
    for(const auto& e:d.events)if(e.kind==EventKind::Block&&e.end>=0) {
        float l=d.participants.front().x-88-(d.sequenceDepth-e.depth)*12.0f,r=d.participants.back().x+88+(d.sequenceDepth-e.depth)*12.0f;
        RectF box(l,e.y,r-l,d.events[e.end].y+12-e.y);if(!context.Shows(box))continue;g.DrawRectangle(&line,box);
        std::wstring tag=Wide(e.blockType);auto[w,h]=TextSize(tag);SolidBrush bg(theme.fill);RectF tab(l,e.y,w+22,28);g.FillRectangle(&bg,tab);g.DrawRectangle(&line,tab);Text(g,context,tag,tab,theme.text,14,StringAlignmentCenter,FontStyleBold);
        if(!e.label.empty())Text(g,context,L"["+e.label+L"]",RectF(l+w+32,e.y,r-l-w-42,34),theme.text,14,StringAlignmentNear);
    }
    for(const auto& a:d.activations) {RectF r(d.participants[a.participant].x-6+a.depth*5,a.y,12,std::max(8.0f,a.bottom-a.y));if(!context.Shows(r))continue;SolidBrush fill(theme.fill);g.FillRectangle(&fill,r);g.DrawRectangle(&line,r);}
    for(const auto& e:d.events) {
        if(e.kind==EventKind::Message) {
            float x1=d.participants[e.from].x,x2=d.participants[e.to].x,y=e.y+e.h-14;
            if(!context.Shows(std::min(x1,x2)-12,e.y-30,std::max(std::max(x1,x2),x1+250)+12,e.y+e.h+10))continue;
            std::wstring label=e.label;if(d.autonumber)label=std::to_wstring(e.number)+L". "+label;
            Pen pen(theme.line,1.5f);if(Starts(e.arrow,"--"))pen.SetDashStyle(DashStyleDash);
            PointF end{x2,y},before{x1,y};
            if(e.from==e.to){float right=x1+60;PointF points[]={{x1,y-24},{right,y-24},{right,y},{x1,y}};g.DrawLines(&pen,points,4);before={right,y};Text(g,context,label,RectF(x1+12,e.y,230,e.h-36),theme.text,14,StringAlignmentNear);}
            else {g.DrawLine(&pen,x1,y,x2,y);Text(g,context,label,RectF(std::min(x1,x2)+12,e.y,std::abs(x2-x1)-24,e.h-22),theme.text,14);}
            if(e.arrow.back()=='x'){g.DrawLine(&line,x2-5,y-5,x2+5,y+5);g.DrawLine(&line,x2-5,y+5,x2+5,y-5);}
            else if(e.arrow!="->"&&e.arrow!="-->")Arrow(g,end,before,theme.line,e.arrow.size()>=2&&e.arrow.substr(e.arrow.size()-2)==">>");
        } else if(e.kind==EventKind::Note) {
            auto[w,h]=TextSize(e.label);float x1=d.participants[e.from].x,x2=d.participants[e.to].x;
            float width=e.from==e.to?w+24:std::max(w+24,std::abs(x2-x1)+50),left=(x1+x2-width)/2;
            if(e.side<0)left=x1-width-20;else if(e.side>0)left=x1+20;
            RectF r(left,e.y+5,width,h+22);if(!context.Shows(r))continue;SolidBrush fill(theme.fill);g.FillRectangle(&fill,r);g.DrawRectangle(&line,r);Text(g,context,e.label,r,theme.text,14);
        } else if(e.kind==EventKind::Else) {
            float l=d.participants.front().x-88-(d.sequenceDepth-e.depth)*12.0f,r=d.participants.back().x+88+(d.sequenceDepth-e.depth)*12.0f;
            if(!context.Shows(l,e.y-4,r,e.y+40))continue;
            g.DrawLine(&dashed,l,e.y,r,e.y);Text(g,context,L"["+e.label+L"]",RectF(l+12,e.y+2,r-l-24,36),theme.text,14,StringAlignmentNear);
        }
    }
}
}

std::shared_ptr<Diagram> ParseDiagram(const std::string& source) {
    MeasureScope measuring; // Shared by every label measurement of this parse.
    auto diagram=std::make_shared<Diagram>();
    try {
        if(source.size()>50000)throw std::runtime_error("図のソースが上限の50,000バイトを超えています。");
        auto statements=Statements(source);if(statements.empty())throw std::runtime_error("図のソースが空です。");
        diagram->data=std::make_shared<Diagram::Data>();
        if(statements[0]=="sequenceDiagram")ParseSequence(*diagram,statements);
        else {
            std::smatch match;
            static const std::regex header(R"((?:flowchart|graph)\s+(TD|TB|BT|LR|RL))");
            if(!std::regex_match(statements[0],match,header))
                throw std::runtime_error("この構文は未対応です。flowchart / graph と sequenceDiagram を表示できます。");
            auto direction=match[1].str();diagram->data->horizontal=direction=="LR"||direction=="RL";diagram->data->reverse=direction=="RL"||direction=="BT";
            FlowParser parser(*diagram->data);parser.Parse(statements);LayoutFlow(*diagram);
        }
        if(diagram->width>100000||diagram->height>100000)throw std::runtime_error("図の表示寸法が上限を超えています。");
        diagram->valid=true;
    } catch(const std::exception& e) {
        try{diagram->error=Wide(e.what());}catch(...){diagram->error=L"図の解析中にエラーが発生しました。";}
        diagram->data.reset();diagram->valid=false;diagram->width=0;diagram->height=0;
    }
    return diagram;
}
std::size_t DiagramMemoryUsage(const Diagram& diagram) {
    std::size_t bytes = sizeof(Diagram) + StringAllocationBytes(diagram.error);
    if (!diagram.data) return bytes;
    // Charge the complete shared allocation rather than dividing by use_count:
    // retained ownership keeps all of it alive. Allocator and shared_ptr control
    // block overhead is implementation-specific and excluded from this estimate.
    const auto& data = *diagram.data;
    bytes += sizeof(Diagram::Data) + StringAllocationBytes(data.title);
    bytes += data.nodes.capacity() * sizeof(Node);
    bytes += data.edges.capacity() * sizeof(Edge);
    bytes += data.groups.capacity() * sizeof(Group);
    bytes += data.participants.capacity() * sizeof(Participant);
    bytes += data.events.capacity() * sizeof(Event);
    bytes += data.activations.capacity() * sizeof(Activation);
    for (const auto& node : data.nodes) {
        bytes += StringAllocationBytes(node.id) + StringAllocationBytes(node.className);
        bytes += StringAllocationBytes(node.label);
    }
    for (const auto& edge : data.edges)
        bytes += StringAllocationBytes(edge.label) + edge.route.capacity() * sizeof(Gdiplus::PointF);
    for (const auto& group : data.groups) bytes += StringAllocationBytes(group.label);
    for (const auto& participant : data.participants)
        bytes += StringAllocationBytes(participant.id) + StringAllocationBytes(participant.label);
    for (const auto& event : data.events) {
        bytes += StringAllocationBytes(event.label) + StringAllocationBytes(event.arrow);
        bytes += StringAllocationBytes(event.blockType);
    }
    return bytes;
}
void DrawDiagram(Graphics& graphics,const Diagram& diagram,float x,float y,float scale,const DiagramTheme& theme) {
    if(!diagram.valid||!diagram.data||!std::isfinite(scale)||scale<=0)return;
    MeasureScope measuring;DrawContext context;
    auto state=graphics.Save();graphics.TranslateTransform(x,y);graphics.ScaleTransform(scale,scale);
    // The clip in diagram coordinates: a document view paints its client area,
    // a bitmap the whole diagram. Off-screen elements are then skipped.
    context.clip=graphics.GetVisibleClipBounds(&context.visible)==Ok&&context.visible.Width>0&&context.visible.Height>0;
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);graphics.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    if(diagram.data->sequence)DrawSequence(graphics,context,diagram,theme);else DrawFlow(graphics,context,diagram,theme);
    graphics.Restore(state);
}
}
