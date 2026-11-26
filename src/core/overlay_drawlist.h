// GENERIC. Double-buffered overlay draw list (T7.1, spec §6.4).
//
// The producer (the game's script thread: fibers and tick callbacks) appends
// to the back frame and publishes it at the end of each tick. The consumer
// (the present thread) reads the newest published frame; if no tick ran since
// the last present it simply redraws the same frame, so nothing flickers when
// presents outpace ticks. A published frame is immutable and shared: the
// consumer keeps it alive for as long as it draws from it.
#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace shvi::overlay {

struct DrawItem {
    enum class Kind : std::uint8_t { Rect, Text } kind = Kind::Rect;
    float x = 0, y = 0, w = 0, h = 0;   // normalized 0..1, top-left (text: anchor per align, top of line)
    std::uint32_t rgba = 0;             // vi::PackRgba layout: r<<24 | g<<16 | b<<8 | a
    float scale = 0.35f;
    int align = 0;                      // 0 left, 1 centre, 2 right
    std::string text;
};
using Frame = std::vector<DrawItem>;

class DrawList {
public:
    DrawList();
    // Producer side (one thread).
    void AddRect(float x, float y, float w, float h, std::uint32_t rgba);
    void AddText(std::string_view text, float x, float y, float scale, std::uint32_t rgba, int align);
    void Publish();  // back -> front, back cleared
    void Clear();    // publish an empty frame and drop pending back items
    // Consumer side (any thread).
    std::shared_ptr<const Frame> Front() const;  // never null
    std::uint64_t Generation() const;            // increments on each Publish/Clear

private:
    void Swap(Frame next);
    Frame back_;
    mutable std::mutex mu_;
    std::shared_ptr<const Frame> front_;
    std::uint64_t gen_ = 0;
};

DrawList& GlobalDrawList();

// End-of-tick decision (core_main OnTick): plugins ran -> Publish, otherwise
// (gate closed or guard latched) -> Clear, so a menu vanishes instead of
// staying frozen on screen.
void FinishTickDrawList(bool pluginsRan);

}  // namespace shvi::overlay
