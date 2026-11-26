// ScriptHook VI - header-only mod-menu toolkit (C++17). Game-independent.
//
//   Menu      pages of items (button, toggle, int/float slider, choice, submenu),
//             cursor with wrap-around, a scrolling window, open/close/back.
//   Layout    Menu::Layout(style) -> DrawList: rectangles and text in normalised
//             screen space (0..1, origin top-left). Pure, so it is unit-tested.
//   Backend   draws a DrawList. GTA VI: shvi::vi::EngineBackend (vi_ui.hpp)
//             calls three engine functions by name; menus don't change when
//             the drawing route behind those names does.
//   Input     InputQueue: pass InputQueue::OnKey to api->keyboard_register. Keys
//             arrive on the window thread; drain them with Pop() in your script.
//
// Typical script loop:
//   for (;;) {
//       for (ui::Action a; (a = input.Pop()) != ui::Action::None;) menu.Handle(a);
//       ui::Submit(backend, menu.Layout(style));
//       shvi::wait(0);
//   }
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace shvi::ui {

struct Color {
    std::uint8_t r = 0, g = 0, b = 0, a = 255;
    friend bool operator==(const Color& x, const Color& y) {
        return x.r == y.r && x.g == y.g && x.b == y.b && x.a == y.a;
    }
    friend bool operator!=(const Color& x, const Color& y) { return !(x == y); }
};

enum class Align { Left, Center, Right };

// x/y: top-left for rectangles; for text, the anchor (left edge, centre or
// right edge, per align) and the top of the line.
struct DrawCmd {
    enum Kind { Rect, Text } kind = Rect;
    float x = 0, y = 0, w = 0, h = 0;
    Color color{};
    std::string text;
    float scale = 0.35f;
    int font = 0;
    Align align = Align::Left;
};
using DrawList = std::vector<DrawCmd>;

class Backend {
public:
    virtual ~Backend() = default;
    virtual void Draw(const DrawCmd& cmd) = 0;
};

inline void Submit(Backend& b, const DrawList& list) {
    for (const auto& c : list) b.Draw(c);
}

// Faces the GTA VI overlay draws with (UI_DRAW_TEXT's font argument): Segoe UI, Segoe UI Bold,
// Bahnschrift. Any other value draws with the regular face.
namespace font {
inline constexpr int Regular = 0, Bold = 1, Display = 2;
}

// Text width without a renderer: advance widths of printable ASCII for the three faces, in thousandths of
// the face's line height (the unit the overlay sizes text by). Other bytes count as an average glyph.
namespace detail {
inline constexpr std::uint16_t kAdvance[3][95] = {
    {206,214,295,444,405,615,602,173,227,227,314,514,163,301,163,293,405,405,405,405,405,405,405,405,405,405,163,163,514,514,514,337,718,485,431,465,527,380,367,516,534,200,268,436,354,675,562,567,421,567,450,399,394,517,467,702,443,416,429,227,285,227,514,312,202,383,442,347,443,393,235,443,425,182,182,374,182,648,425,441,442,443,261,319,255,425,360,543,345,364,340,227,180,227,514},
    {207,246,371,445,432,652,639,220,278,278,342,532,204,304,204,333,432,432,432,432,432,432,432,432,432,432,204,204,532,532,532,329,717,529,482,469,554,400,391,535,576,238,335,488,384,720,594,570,462,570,491,421,441,544,501,756,493,456,456,278,328,278,532,312,236,405,466,361,465,407,288,465,453,214,214,420,214,689,455,460,466,465,299,331,293,455,407,599,415,405,360,278,245,278,532},
    {269,271,378,624,577,687,652,178,339,339,430,528,227,479,227,378,552,333,517,529,569,544,513,503,562,513,227,227,424,459,429,432,890,647,646,619,656,598,559,646,679,275,505,642,573,779,700,646,623,657,652,616,481,655,587,847,567,503,544,294,378,294,503,436,294,532,544,498,539,536,317,539,559,251,268,529,290,856,559,545,544,539,427,519,326,556,500,773,517,483,495,360,275,360,514},
};
}  // namespace detail

struct Style {
    float x = 0.715f, y = 0.085f, width = 0.255f;
    float headerHeight = 0.068f, subHeight = 0.030f, itemHeight = 0.033f, footerHeight = 0.032f;
    float accentHeight = 0.004f;  // the stripe under the header
    float padX = 0.009f;
    int maxVisible = 12;
    int font = font::Regular, boldFont = font::Bold, titleFont = font::Display;
    float titleScale = 0.62f, subScale = 0.25f, itemScale = 0.31f, tagScale = 0.20f, hintScale = 0.24f;
    // How the backend sizes text and the screen's shape, for fitting labels: a line of `scale` is
    // scale * textHeight of the screen height tall (the overlay's kTextScale), and `aspect` is width / height.
    float textHeight = 0.06f, aspect = 16.0f / 9.0f;
    std::string rootCaption = "MAIN MENU";  // the sub-header on the root page (deeper pages show the path)
    Color accent{236, 72, 153, 255};
    Color header{14, 14, 20, 240}, sub{0, 0, 0, 225}, background{8, 8, 12, 200}, highlight{236, 72, 153, 235};
    Color section{0, 0, 0, 215}, footer{0, 0, 0, 230}, scrollTrack{255, 255, 255, 28};
    Color text{240, 240, 245, 255}, textDim{125, 125, 135, 255}, textSelected{255, 255, 255, 255}, title{255, 255, 255, 255};
    Color subText{190, 190, 200, 255}, sectionText{236, 72, 153, 255}, hint{165, 165, 175, 255};
    Color switchOn{52, 211, 153, 255}, switchOff{70, 70, 82, 255}, knob{245, 245, 245, 255};
};

// Width of `s` at `scale` in face `font`, in screen widths (0..1).
inline float TextWidth(const std::string& s, float scale, int face, const Style& st) {
    const auto& adv = detail::kAdvance[face >= 0 && face <= 2 ? face : 0];
    std::uint32_t sum = 0;
    for (unsigned char ch : s) {
        if (ch >= 32 && ch <= 126) sum += adv[ch - 32];
        else if (ch >= 0xC0 || ch < 0x80) sum += 450;  // a UTF-8 lead byte or control: about one glyph
    }
    return static_cast<float>(sum) / 1000.0f * scale * st.textHeight / st.aspect;
}
// `s` cut to fit `maxW` (screen widths), ending in "..." when cut. Cuts on UTF-8 boundaries.
inline std::string FitText(const std::string& s, float maxW, float scale, int face, const Style& st) {
    if (maxW <= 0.0f) return {};
    if (TextWidth(s, scale, face, st) <= maxW) return s;
    const float dots = TextWidth("...", scale, face, st);
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        std::size_t len = 1;
        unsigned char ch = static_cast<unsigned char>(s[i]);
        if (ch >= 0xF0) len = 4;
        else if (ch >= 0xE0) len = 3;
        else if (ch >= 0xC0) len = 2;
        std::string next = out + s.substr(i, len);
        if (TextWidth(next, scale, face, st) + dots > maxW) break;
        out = std::move(next);
        i += len;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out + "...";
}

enum class Action { None, Open, Up, Down, Left, Right, Select, Back };

class Page;

struct Item {
    enum Kind { Button, Toggle, Int, Float, Choice, Submenu, Section } kind = Button;
    std::string label;
    std::function<void()> onSelect;  // Button; called after a value change for the others
    bool* flag = nullptr;
    int* ival = nullptr;
    int imin = 0, imax = 0, istep = 1;
    float* fval = nullptr;
    float fmin = 0, fmax = 0, fstep = 0.1f;
    int precision = 2;
    std::vector<std::string> options;
    Page* page = nullptr;
    // Presentation, set with Page::Tag / Hint / Dim right after adding the item.
    std::string tag;          // a short badge drawn before the value ("EXP", "12/40")
    Color tagColor{90, 90, 100, 230};
    std::string hint;         // shown in the footer while the item is selected
    bool dim = false;         // drawn greyed out (still selectable)
    bool Selectable() const { return kind != Section; }
};

class Page {
public:
    explicit Page(std::string title) : title_(std::move(title)) {}
    const std::string& Title() const { return title_; }
    const std::vector<Item>& Items() const { return items_; }

    // A heading row between items: drawn, never selected.
    Page& Section(std::string title) {
        Item i;
        i.kind = Item::Section;
        i.label = std::move(title);
        items_.push_back(std::move(i));
        return *this;
    }
    // Presentation of the item added last.
    Page& Tag(std::string text, Color c) {
        if (!items_.empty()) { items_.back().tag = std::move(text); items_.back().tagColor = c; }
        return *this;
    }
    Page& Hint(std::string text) {
        if (!items_.empty()) items_.back().hint = std::move(text);
        return *this;
    }
    Page& Dim(bool d = true) {
        if (!items_.empty()) items_.back().dim = d;
        return *this;
    }

    Page& Button(std::string label, std::function<void()> fn) {
        Item i;
        i.kind = Item::Button;
        i.label = std::move(label);
        i.onSelect = std::move(fn);
        items_.push_back(std::move(i));
        return *this;
    }
    Page& Toggle(std::string label, bool* v, std::function<void()> changed = nullptr) {
        Item i;
        i.kind = Item::Toggle;
        i.label = std::move(label);
        i.flag = v;
        i.onSelect = std::move(changed);
        items_.push_back(std::move(i));
        return *this;
    }
    Page& Int(std::string label, int* v, int mn, int mx, int step = 1, std::function<void()> changed = nullptr) {
        Item i;
        i.kind = Item::Int;
        i.label = std::move(label);
        i.ival = v;
        i.imin = mn;
        i.imax = mx;
        i.istep = step;
        i.onSelect = std::move(changed);
        items_.push_back(std::move(i));
        return *this;
    }
    Page& Float(std::string label, float* v, float mn, float mx, float step = 0.1f, int precision = 2,
                std::function<void()> changed = nullptr) {
        Item i;
        i.kind = Item::Float;
        i.label = std::move(label);
        i.fval = v;
        i.fmin = mn;
        i.fmax = mx;
        i.fstep = step;
        i.precision = precision;
        i.onSelect = std::move(changed);
        items_.push_back(std::move(i));
        return *this;
    }
    Page& Choice(std::string label, int* index, std::vector<std::string> options,
                 std::function<void()> changed = nullptr) {
        Item i;
        i.kind = Item::Choice;
        i.label = std::move(label);
        i.ival = index;
        i.options = std::move(options);
        i.onSelect = std::move(changed);
        items_.push_back(std::move(i));
        return *this;
    }
    Page& Submenu(std::string label, Page& target) {
        Item i;
        i.kind = Item::Submenu;
        i.label = std::move(label);
        i.page = &target;
        items_.push_back(std::move(i));
        return *this;
    }

private:
    std::string title_;
    std::vector<Item> items_;
};

// The value an item shows on the right. Toggles show ON/OFF (drawn as a switch by Layout); sliders and
// choices get arrows when `selected`, since only then do Left/Right change them.
inline std::string ValueText(const Item& i, bool selected = false) {
    char buf[64];
    std::string v;
    switch (i.kind) {
    case Item::Toggle: return i.flag && *i.flag ? "ON" : "OFF";
    case Item::Int:
        std::snprintf(buf, sizeof(buf), "%d", i.ival ? *i.ival : 0);
        v = buf;
        break;
    case Item::Float:
        std::snprintf(buf, sizeof(buf), "%.*f", i.precision, i.fval ? static_cast<double>(*i.fval) : 0.0);
        v = buf;
        break;
    case Item::Choice:
        if (!i.ival || i.options.empty()) return "";
        v = i.options[static_cast<std::size_t>(std::clamp(*i.ival, 0, int(i.options.size()) - 1))];
        break;
    case Item::Submenu: return ">";
    default: return "";
    }
    return selected ? "<  " + v + "  >" : v;
}

class Menu {
public:
    explicit Menu(std::string title) {
        pages_.push_back(std::make_unique<Page>(std::move(title)));
        stack_.push_back({pages_.front().get(), 0});
    }
    Page& Root() { return *pages_.front(); }
    Page& AddPage(std::string title) {
        pages_.push_back(std::make_unique<Page>(std::move(title)));
        return *pages_.back();
    }

    bool IsOpen() const { return open_; }
    void SetOpen(bool open) { open_ = open; }
    const Page& Current() const { return *stack_.back().page; }
    int Cursor() const { return stack_.back().cursor; }
    // First row shown. The window moves only as far as needed to keep the cursor in view (and the
    // section heading right above it, when there is one).
    int ScrollTop(const Style& st) const {
        const Frame& f = stack_.back();
        int n = static_cast<int>(f.page->Items().size());
        int vis = std::max(1, st.maxVisible);
        int top = std::clamp(f.top, 0, std::max(0, n - vis));
        int want = f.cursor;
        if (want > 0 && !f.page->Items()[static_cast<std::size_t>(want - 1)].Selectable()) --want;
        if (want < top) top = want;
        if (f.cursor >= top + vis) top = f.cursor - vis + 1;
        f.top = top;
        return top;
    }
    // "k / n" over selectable items: position of the cursor and how many there are.
    std::pair<int, int> Position() const {
        const auto& items = Current().Items();
        int k = 0, n = 0;
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            if (!items[static_cast<std::size_t>(i)].Selectable()) continue;
            ++n;
            if (i <= Cursor()) k = n;
        }
        return {n ? k : 0, n};
    }
    // Where the user is: the cursor on each page from the root down. RestorePath re-enters the same submenus
    // in a menu built the same way, so a plugin can rebuild its menu without losing the user's place.
    std::vector<int> Path() const {
        std::vector<int> p;
        for (const Frame& f : stack_) p.push_back(f.cursor);
        return p;
    }
    void RestorePath(const std::vector<int>& p) {
        stack_.resize(1);
        for (std::size_t i = 0; i < p.size(); ++i) {
            Frame& f = stack_.back();
            const auto& items = f.page->Items();
            if (items.empty()) break;
            f.cursor = Settle(*f.page, std::clamp(p[i], 0, static_cast<int>(items.size()) - 1), +1);
            const Item& it = items[static_cast<std::size_t>(f.cursor)];
            if (i + 1 < p.size() && it.kind == Item::Submenu && it.page) stack_.push_back({it.page, 0});
            else break;
        }
    }
    // Page titles from the root to the current page.
    std::string Breadcrumb(const char* sep = "  /  ") const {
        std::string s;
        for (const Frame& f : stack_) s += (s.empty() ? "" : sep) + f.page->Title();
        return s;
    }

    void Handle(Action a) {
        if (a == Action::Open) {
            open_ = !open_;
            return;
        }
        if (!open_ || a == Action::None) return;
        Frame& f = stack_.back();
        const auto& items = f.page->Items();
        int n = static_cast<int>(items.size());
        if (a == Action::Back) {
            if (stack_.size() > 1) stack_.pop_back();
            else open_ = false;
            return;
        }
        if (n == 0) return;
        f.cursor = Settle(*f.page, std::clamp(f.cursor, 0, n - 1), +1);
        const Item& it = items[static_cast<std::size_t>(f.cursor)];
        switch (a) {
        case Action::Up: f.cursor = Settle(*f.page, (f.cursor + n - 1) % n, -1); break;
        case Action::Down: f.cursor = Settle(*f.page, (f.cursor + 1) % n, +1); break;
        case Action::Left: Adjust(it, -1); break;
        case Action::Right: Adjust(it, +1); break;
        case Action::Select: Activate(it); break;
        default: break;
        }
    }

    DrawList Layout(const Style& st) const {
        DrawList out;
        if (!open_) return out;
        const Page& p = Current();
        const auto& items = p.Items();
        int n = static_cast<int>(items.size());
        int vis = std::max(1, st.maxVisible);
        int top = ScrollTop(st);
        int shown = std::min(vis, n - top);
        const float xr = st.x + st.width - st.padX;  // inner right edge

        auto rect = [&](float x, float y, float w, float h, Color c) {
            DrawCmd d;
            d.kind = DrawCmd::Rect;
            d.x = x; d.y = y; d.w = w; d.h = h; d.color = c;
            out.push_back(std::move(d));
        };
        auto text = [&](std::string s, float x, float y, float scale, int face, Color c, Align al) {
            DrawCmd d;
            d.kind = DrawCmd::Text;
            d.text = std::move(s);
            d.x = x; d.y = y; d.scale = scale; d.font = face; d.color = c; d.align = al;
            out.push_back(std::move(d));
        };
        // top of a text line of `scale` centred in a row at y of height h
        auto mid = [&](float y, float h, float scale) { return y + (h - scale * st.textHeight) * 0.5f; };

        // header: title on a dark band, accent stripe below
        rect(st.x, st.y, st.width, st.headerHeight, st.header);
        text(FitText(p.Title(), st.width - 2 * st.padX, st.titleScale, st.titleFont, st), st.x + st.padX * 1.5f,
             mid(st.y, st.headerHeight, st.titleScale), st.titleScale, st.titleFont, st.title, Align::Left);
        float y = st.y + st.headerHeight;
        rect(st.x, y, st.width, st.accentHeight, st.accent);
        y += st.accentHeight;

        // sub-header: where we are, and the position in the page
        rect(st.x, y, st.width, st.subHeight, st.sub);
        auto [k, total] = Position();
        std::string count = std::to_string(k) + " / " + std::to_string(total);
        float countW = TextWidth(count, st.subScale, st.font, st);
        text(count, xr, mid(y, st.subHeight, st.subScale), st.subScale, st.font, st.subText, Align::Right);
        std::string where = stack_.size() > 1 || st.rootCaption.empty() ? Breadcrumb() : st.rootCaption;
        text(FitText(where, st.width - 3 * st.padX - countW, st.subScale, st.boldFont, st), st.x + st.padX,
             mid(y, st.subHeight, st.subScale), st.subScale, st.boldFont, st.subText, Align::Left);
        y += st.subHeight;

        const float listTop = y;
        const bool scroll = n > vis;
        const float scrollW = scroll ? 0.0025f : 0.0f;
        const float rowW = st.width - scrollW;
        const float xv = st.x + rowW - st.padX;  // right edge of values (left of the scroll bar)
        for (int row = 0; row < shown; ++row) {
            int idx = top + row;
            const Item& it = items[static_cast<std::size_t>(idx)];
            float ry = listTop + static_cast<float>(row) * st.itemHeight;
            if (it.kind == Item::Section) {
                rect(st.x, ry, rowW, st.itemHeight, st.section);
                text(FitText(it.label, rowW - 2 * st.padX, st.subScale, st.boldFont, st), st.x + st.padX,
                     mid(ry, st.itemHeight, st.subScale) + st.itemHeight * 0.08f, st.subScale, st.boldFont, st.sectionText,
                     Align::Left);
                continue;
            }
            bool sel = idx == Cursor();
            rect(st.x, ry, rowW, st.itemHeight, sel ? st.highlight : st.background);
            if (sel) rect(st.x, ry, 0.0022f, st.itemHeight, st.title);  // a bright edge on the selected row
            Color tc = sel ? st.textSelected : (it.dim ? st.textDim : st.text);
            float ty = mid(ry, st.itemHeight, st.itemScale);

            // right side, from the edge inwards: value (or switch), then the tag
            float right = xv;
            if (it.kind == Item::Toggle) {
                const float th = st.itemHeight * 0.46f, tw = th * 2.0f / st.aspect, kh = th * 0.70f, kw = kh / st.aspect;
                const bool on = it.flag && *it.flag;
                const float tx = right - tw, tyy = ry + (st.itemHeight - th) * 0.5f;
                rect(tx, tyy, tw, th, on ? st.switchOn : st.switchOff);
                const float kx = on ? tx + tw - kw - (th - kh) * 0.5f / st.aspect : tx + (th - kh) * 0.5f / st.aspect;
                rect(kx, tyy + (th - kh) * 0.5f, kw, kh, st.knob);
                right = tx - st.padX;
            } else {
                std::string v = ValueText(it, sel);
                if (!v.empty()) {
                    v = FitText(v, rowW * 0.45f, st.itemScale, st.font, st);
                    float vw = TextWidth(v, st.itemScale, st.font, st);
                    Color vc = sel ? st.textSelected : (it.kind == Item::Submenu ? st.textDim : tc);
                    text(v, right, ty, st.itemScale, st.font, vc, Align::Right);
                    right -= vw + st.padX;
                }
            }
            if (!it.tag.empty()) {
                const float tw = TextWidth(it.tag, st.tagScale, st.boldFont, st), padx = 0.0035f;
                const float th = st.itemHeight * 0.56f, tyy = ry + (st.itemHeight - th) * 0.5f;
                const float bx = right - tw - 2 * padx;
                rect(bx, tyy, tw + 2 * padx, th, it.tagColor);
                text(it.tag, bx + padx, mid(tyy, th, st.tagScale), st.tagScale, st.boldFont, st.textSelected, Align::Left);
                right = bx - st.padX * 0.6f;
            }
            const float labelX = st.x + st.padX + (sel ? 0.002f : 0.0f);
            text(FitText(it.label, right - labelX, st.itemScale, sel ? st.boldFont : st.font, st), labelX, ty, st.itemScale,
                 sel ? st.boldFont : st.font, tc, Align::Left);
        }
        float fy = listTop + static_cast<float>(shown) * st.itemHeight;
        if (scroll) {  // track and thumb beside the rows
            float trackH = fy - listTop;
            rect(st.x + rowW, listTop, scrollW, trackH, st.scrollTrack);
            float thumbH = std::max(trackH * static_cast<float>(vis) / static_cast<float>(n), st.itemHeight * 0.5f);
            float thumbY = listTop + (trackH - thumbH) * static_cast<float>(top) / static_cast<float>(std::max(1, n - vis));
            rect(st.x + rowW, thumbY, scrollW, thumbH, st.accent);
        }

        // footer: what the selected item does, or the keys
        rect(st.x, fy, st.width, st.footerHeight, st.footer);
        std::string hint;
        if (Cursor() >= 0 && Cursor() < n) {
            const Item& it = items[static_cast<std::size_t>(Cursor())];
            hint = it.hint;
            if (hint.empty()) {
                switch (it.kind) {
                case Item::Toggle: hint = "Enter  switch on / off"; break;
                case Item::Int: case Item::Float: case Item::Choice: hint = "Left / Right  change"; break;
                case Item::Submenu: hint = "Enter  open      Backspace  back"; break;
                case Item::Button: hint = "Enter  run"; break;
                default: break;
                }
            }
        }
        if (!hint.empty())
            text(FitText(hint, st.width - 2 * st.padX, st.hintScale, st.font, st), st.x + st.padX, mid(fy, st.footerHeight, st.hintScale),
                 st.hintScale, st.font, st.hint, Align::Left);
        return out;
    }

private:
    struct Frame {
        const Page* page;
        int cursor;
        mutable int top = 0;  // scroll position, kept by ScrollTop
    };
    // The nearest selectable index from i in direction dir (wrapping), or i when the page has none.
    static int Settle(const Page& p, int i, int dir) {
        const auto& items = p.Items();
        int n = static_cast<int>(items.size());
        for (int k = 0; k < n; ++k) {
            int j = ((i + dir * k) % n + n) % n;
            if (items[static_cast<std::size_t>(j)].Selectable()) return j;
        }
        return i;
    }

    static void Changed(const Item& it) {
        if (it.onSelect) it.onSelect();
    }
    void Adjust(const Item& it, int dir) {
        switch (it.kind) {
        case Item::Toggle:
            if (it.flag) { *it.flag = !*it.flag; Changed(it); }
            break;
        case Item::Int:
            if (it.ival) { *it.ival = std::clamp(*it.ival + dir * it.istep, it.imin, it.imax); Changed(it); }
            break;
        case Item::Float:
            if (it.fval) { *it.fval = std::clamp(*it.fval + static_cast<float>(dir) * it.fstep, it.fmin, it.fmax); Changed(it); }
            break;
        case Item::Choice:
            if (it.ival && !it.options.empty()) {
                int m = static_cast<int>(it.options.size());
                *it.ival = ((*it.ival + dir) % m + m) % m;
                Changed(it);
            }
            break;
        default: break;
        }
    }
    void Activate(const Item& it) {
        switch (it.kind) {
        case Item::Button: Changed(it); break;
        case Item::Toggle: Adjust(it, +1); break;
        case Item::Submenu:
            if (it.page) stack_.push_back({it.page, it.page->Items().empty() ? 0 : Settle(*it.page, 0, +1)});
            break;
        default: break;  // sliders/choices change with Left/Right
        }
    }

    std::vector<std::unique_ptr<Page>> pages_;
    std::vector<Frame> stack_;
    bool open_ = false;
};

// Virtual-key codes (winuser.h values; spelled out so this header needs no <windows.h>).
// Every platform delivers keys as these codes: on PS5 the runtime maps the pad to them
// (R3+L1 -> F4, d-pad -> arrows, Cross -> Return, Circle -> Back, see platforms/ps5/README.md).
namespace vk {
inline constexpr std::uint32_t Back = 0x08, Return = 0x0D, Left = 0x25, Up = 0x26, Right = 0x27, Down = 0x28;
inline constexpr std::uint32_t Numpad0 = 0x60, Numpad2 = 0x62, Numpad3 = 0x63, Numpad4 = 0x64, Numpad5 = 0x65,
                               Numpad6 = 0x66, Numpad8 = 0x68, Numpad9 = 0x69, F4 = 0x73;
}  // namespace vk

struct KeyMap {
    std::vector<std::uint32_t> open{vk::F4};
    std::vector<std::uint32_t> up{vk::Numpad8, vk::Up}, down{vk::Numpad2, vk::Down};
    std::vector<std::uint32_t> left{vk::Numpad4, vk::Left}, right{vk::Numpad6, vk::Right};
    std::vector<std::uint32_t> select{vk::Numpad5, vk::Return}, back{vk::Numpad0, vk::Back};

    Action Map(std::uint32_t key) const {
        auto has = [key](const std::vector<std::uint32_t>& v) { return std::find(v.begin(), v.end(), key) != v.end(); };
        if (has(open)) return Action::Open;
        if (has(up)) return Action::Up;
        if (has(down)) return Action::Down;
        if (has(left)) return Action::Left;
        if (has(right)) return Action::Right;
        if (has(select)) return Action::Select;
        if (has(back)) return Action::Back;
        return Action::None;
    }
};

// Thread-safe: OnKey runs on the game window's thread, Pop on the script fiber.
class InputQueue {
public:
    explicit InputQueue(KeyMap map = {}) : map_(std::move(map)) {}

    // Signature matches shvi_keyboard_fn: api->keyboard_register(&InputQueue::OnKey, &queue).
    static void OnKey(std::uint32_t key, int isDown, void* user) {
        auto* q = static_cast<InputQueue*>(user);
        if (!q || !isDown) return;
        Action a = q->map_.Map(key);
        if (a == Action::None) return;
        std::lock_guard lock(q->mu_);
        if (q->pending_.size() < 64) q->pending_.push_back(a);  // bounded: a stalled script can't grow it
    }

    Action Pop() {
        std::lock_guard lock(mu_);
        if (pending_.empty()) return Action::None;
        Action a = pending_.front();
        pending_.pop_front();
        return a;
    }

private:
    KeyMap map_;
    std::mutex mu_;
    std::deque<Action> pending_;
};

}  // namespace shvi::ui
