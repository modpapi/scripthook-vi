// GENERIC. See overlay_drawlist.h.
#include "overlay_drawlist.h"

namespace shvi::overlay {

DrawList::DrawList() : front_(std::make_shared<const Frame>()) {}

void DrawList::AddRect(float x, float y, float w, float h, std::uint32_t rgba) {
    DrawItem it;
    it.kind = DrawItem::Kind::Rect;
    it.x = x, it.y = y, it.w = w, it.h = h;
    it.rgba = rgba;
    back_.push_back(std::move(it));
}

void DrawList::AddText(std::string_view text, float x, float y, float scale, std::uint32_t rgba, int align) {
    DrawItem it;
    it.kind = DrawItem::Kind::Text;
    it.x = x, it.y = y;
    it.rgba = rgba;
    it.scale = scale;
    it.align = align;
    it.text.assign(text);
    back_.push_back(std::move(it));
}

void DrawList::Swap(Frame next) {
    auto f = std::make_shared<const Frame>(std::move(next));
    std::shared_ptr<const Frame> old;
    {
        std::lock_guard<std::mutex> lock(mu_);
        old = std::move(front_);
        front_ = std::move(f);
        ++gen_;
    }
    // `old` is released here, outside the lock.
}

void DrawList::Publish() {
    Frame next;
    next.swap(back_);
    Swap(std::move(next));
}

void DrawList::Clear() {
    back_.clear();
    Swap(Frame{});
}

std::shared_ptr<const Frame> DrawList::Front() const {
    std::lock_guard<std::mutex> lock(mu_);
    return front_;
}

std::uint64_t DrawList::Generation() const {
    std::lock_guard<std::mutex> lock(mu_);
    return gen_;
}

DrawList& GlobalDrawList() {
    static DrawList d;
    return d;
}

void FinishTickDrawList(bool pluginsRan) {
    if (pluginsRan) GlobalDrawList().Publish();
    else GlobalDrawList().Clear();
}

}  // namespace shvi::overlay
