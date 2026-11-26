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
    int font = 4;
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

struct Style {
    float x = 0.70f, y = 0.10f, width = 0.24f;
    float headerHeight = 0.060f, itemHeight = 0.034f, footerHeight = 0.030f;
    float padX = 0.008f;
    float textYOffset = 0.10f;  // fraction of a row's height above the text line
    int maxVisible = 10;
    int font = 4;
    float titleScale = 0.70f, itemScale = 0.34f;
    Color header{20, 90, 160, 235}, background{0, 0, 0, 170}, highlight{235, 235, 235, 235};
    Color footer{0, 0, 0, 220};
    Color text{235, 235, 235, 255}, textSelected{10, 10, 10, 255}, title{255, 255, 255, 255};
};

enum class Action { None, Open, Up, Down, Left, Right, Select, Back };

class Page;

struct Item {
    enum Kind { Button, Toggle, Int, Float, Choice, Submenu } kind = Button;
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
};

class Page {
public:
    explicit Page(std::string title) : title_(std::move(title)) {}
    const std::string& Title() const { return title_; }
    const std::vector<Item>& Items() const { return items_; }

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

inline std::string ValueText(const Item& i) {
    char buf[64];
    switch (i.kind) {
    case Item::Toggle: return i.flag && *i.flag ? "ON" : "OFF";
    case Item::Int:
        std::snprintf(buf, sizeof(buf), "< %d >", i.ival ? *i.ival : 0);
        return buf;
    case Item::Float:
        std::snprintf(buf, sizeof(buf), "< %.*f >", i.precision, i.fval ? static_cast<double>(*i.fval) : 0.0);
        return buf;
    case Item::Choice:
        if (!i.ival || i.options.empty()) return "";
        return "< " + i.options[static_cast<std::size_t>(std::clamp(*i.ival, 0, int(i.options.size()) - 1))] + " >";
    case Item::Submenu: return ">>";
    default: return "";
    }
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
    int ScrollTop(const Style& st) const {
        int vis = std::max(1, st.maxVisible);
        return std::max(0, Cursor() - vis + 1);
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
        f.cursor = std::clamp(f.cursor, 0, n - 1);
        const Item& it = items[static_cast<std::size_t>(f.cursor)];
        switch (a) {
        case Action::Up: f.cursor = (f.cursor + n - 1) % n; break;
        case Action::Down: f.cursor = (f.cursor + 1) % n; break;
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

        auto rect = [&](float x, float y, float w, float h, Color c) {
            DrawCmd d;
            d.kind = DrawCmd::Rect;
            d.x = x; d.y = y; d.w = w; d.h = h; d.color = c;
            out.push_back(std::move(d));
        };
        auto text = [&](std::string s, float x, float y, float scale, Color c, Align al) {
            DrawCmd d;
            d.kind = DrawCmd::Text;
            d.text = std::move(s);
            d.x = x; d.y = y; d.scale = scale; d.font = st.font; d.color = c; d.align = al;
            out.push_back(std::move(d));
        };

        rect(st.x, st.y, st.width, st.headerHeight, st.header);
        text(p.Title(), st.x + st.width * 0.5f, st.y + st.headerHeight * st.textYOffset, st.titleScale, st.title,
             Align::Center);
        for (int row = 0; row < shown; ++row) {
            int idx = top + row;
            const Item& it = items[static_cast<std::size_t>(idx)];
            bool sel = idx == Cursor();
            float y = st.y + st.headerHeight + static_cast<float>(row) * st.itemHeight;
            rect(st.x, y, st.width, st.itemHeight, sel ? st.highlight : st.background);
            Color tc = sel ? st.textSelected : st.text;
            float ty = y + st.itemHeight * st.textYOffset;
            text(it.label, st.x + st.padX, ty, st.itemScale, tc, Align::Left);
            std::string v = ValueText(it);
            if (!v.empty()) text(std::move(v), st.x + st.width - st.padX, ty, st.itemScale, tc, Align::Right);
        }
        float fy = st.y + st.headerHeight + static_cast<float>(shown) * st.itemHeight;
        rect(st.x, fy, st.width, st.footerHeight, st.footer);
        text(std::to_string(n ? Cursor() + 1 : 0) + " / " + std::to_string(n), st.x + st.width - st.padX,
             fy + st.footerHeight * st.textYOffset, st.itemScale, st.text, Align::Right);
        return out;
    }

private:
    struct Frame {
        const Page* page;
        int cursor;
    };

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
            if (it.page) stack_.push_back({it.page, 0});
            break;
        default: break;  // sliders/choices change with Left/Right
        }
    }

    std::vector<std::unique_ptr<Page>> pages_;
    std::vector<Frame> stack_;
    bool open_ = false;
};

// Virtual-key codes (winuser.h values; spelled out so this header needs no <windows.h>).
namespace vk {
inline constexpr std::uint32_t Back = 0x08, Return = 0x0D, Left = 0x25, Up = 0x26, Right = 0x27, Down = 0x28;
inline constexpr std::uint32_t Numpad0 = 0x60, Numpad2 = 0x62, Numpad4 = 0x64, Numpad5 = 0x65, Numpad6 = 0x66,
                               Numpad8 = 0x68, F4 = 0x73;
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
