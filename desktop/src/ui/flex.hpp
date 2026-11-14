// ui/flex.hpp — Zero-allocation C++23 immediate-mode Flexbox layout solver.
// Pure math over flat reusable node/scratch vectors in Context; works both
// inside live ImGui frames (snapped via snapPx/snapPt) and headlessly without
// an ImGuiContext.
#pragma once

#include "ui/geist.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <vector>

namespace upix::ui::flex {

inline constexpr float kUnbounded = 1.0e9f;
inline constexpr float kUnboundedThreshold = 0.5e9f;

enum class Direction : uint8_t { Row, Column };
enum class Justify : uint8_t { Start, Center, End, SpaceBetween, SpaceAround, SpaceEvenly };
enum class Align : uint8_t { Start, Center, End, Stretch };
enum class AlignSelf : uint8_t { Auto, Start, Center, End, Stretch };
enum class Wrap : uint8_t { NoWrap, Wrap };

/// Pixel rectangle with helpers matching controls.hpp (p0, p1, vec4).
struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    constexpr ImVec2 p0() const { return ImVec2(x, y); }
    constexpr ImVec2 p1() const { return ImVec2(x + w, y + h); }
    constexpr ImVec2 size() const { return ImVec2(w, h); }
    constexpr ImVec2 center() const { return ImVec2(x + w * 0.5f, y + h * 0.5f); }
    constexpr float right() const { return x + w; }
    constexpr float bottom() const { return y + h; }
    constexpr float midX() const { return x + w * 0.5f; }
    constexpr float midY() const { return y + h * 0.5f; }
    constexpr ImVec4 vec4() const { return ImVec4(x, y, x + w, y + h); }
    constexpr bool empty() const { return w <= 0.0f || h <= 0.0f; }

    constexpr Rect inset(float l, float t, float r, float b) const {
        return Rect{x + l, y + t, std::max(0.0f, w - l - r), std::max(0.0f, h - t - b)};
    }
    constexpr Rect inset(float hPad, float vPad) const { return inset(hPad, vPad, hPad, vPad); }
    constexpr Rect inset(float all) const { return inset(all, all, all, all); }
};

/// LTRB spacing for padding and margins.
struct Edges {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    static constexpr Edges all(float v) { return {v, v, v, v}; }
    static constexpr Edges xy(float h, float v) { return {h, v, h, v}; }
    static constexpr Edges only(float l, float t, float r, float b) { return {l, t, r, b}; }
};

/// Single-axis sizing specification with optional min/max bounds and flex weights.
struct Len {
    enum class Kind : uint8_t { Auto, Fixed, Grow, Shrink, Fraction };

    Kind kind = Kind::Auto;
    float size = 0.0f;
    float growWeight = 0.0f;
    float shrinkWeight = 0.0f;
    float minVal = 0.0f;
    float maxVal = kUnbounded;

    constexpr Len min(float v) const {
        Len copy = *this;
        copy.minVal = std::max(0.0f, v);
        if (copy.maxVal < copy.minVal) copy.maxVal = copy.minVal;
        return copy;
    }

    constexpr Len max(float v) const {
        Len copy = *this;
        copy.maxVal = std::max(copy.minVal, v);
        return copy;
    }

    constexpr Len clamp(float lo, float hi) const {
        Len copy = *this;
        copy.minVal = std::max(0.0f, lo);
        copy.maxVal = std::max(copy.minVal, hi);
        return copy;
    }

    constexpr Len basis(float px) const {
        Len copy = *this;
        copy.size = std::max(0.0f, px);
        return copy;
    }

    constexpr Len grow(float w = 1.0f) const {
        Len copy = *this;
        copy.growWeight = std::max(0.0f, w);
        return copy;
    }

    constexpr Len shrink(float w = 1.0f) const {
        Len copy = *this;
        copy.shrinkWeight = std::max(0.0f, w);
        return copy;
    }
};

/// Content/intrinsic sizing (grows only if explicit .grow() is attached).
inline constexpr Len Auto(float intrinsicPx = 0.0f) {
    return Len{Len::Kind::Auto, std::max(0.0f, intrinsicPx), 0.0f, 0.0f, 0.0f, kUnbounded};
}

/// Alias for Auto(intrinsicPx) matching intrinsic-fit naming.
inline constexpr Len Fit(float intrinsicPx = 0.0f) {
    return Auto(intrinsicPx);
}

/// Rigid fixed size in logical pixels.
inline constexpr Len Fixed(float px) {
    return Len{Len::Kind::Fixed, std::max(0.0f, px), 0.0f, 0.0f, 0.0f, kUnbounded};
}

/// Flexible item that expands to share free space (and shrinks if overflowing).
inline constexpr Len Grow(float weight = 1.0f, float basisPx = 0.0f) {
    return Len{Len::Kind::Grow, std::max(0.0f, basisPx), std::max(0.0f, weight), 1.0f, 0.0f, kUnbounded};
}

/// Shrinkable item starting at basisPx that yields space on overflow.
inline constexpr Len Shrink(float basisPx, float weight = 1.0f) {
    return Len{Len::Kind::Shrink, std::max(0.0f, basisPx), 0.0f, std::max(0.0f, weight), 0.0f, kUnbounded};
}

/// Fraction (0..1) of the parent's inner size along that axis.
inline constexpr Len Fraction(float frac01) {
    return Len{Len::Kind::Fraction, std::clamp(frac01, 0.0f, 1.0f), 0.0f, 0.0f, 0.0f, kUnbounded};
}

/// Full flex specification combining basis, grow weight, and shrink weight.
inline constexpr Len Flex(float basisPx, float growWeight, float shrinkWeight = 1.0f) {
    const Len::Kind kind = (growWeight > 0.0f)
        ? Len::Kind::Grow
        : (shrinkWeight > 0.0f ? Len::Kind::Shrink : Len::Kind::Auto);
    return Len{kind, std::max(0.0f, basisPx), std::max(0.0f, growWeight),
               std::max(0.0f, shrinkWeight), 0.0f, kUnbounded};
}

/// Intrinsic size returned by custom measure hooks (e.g. wrapped text height).
struct IntrinsicSize {
    float w = 0.0f;
    float h = 0.0f;
};

using MeasureFn = IntrinsicSize (*)(float availW, float availH, const void* userData);

/// Properties for a Row or Column flex container.
struct ContainerStyle {
    Direction dir = Direction::Row;
    Edges padding = {};
    Edges margin = {};
    float gap = 0.0f;
    float rowGap = -1.0f; // < 0 inherits `gap`
    Justify justify = Justify::Start;
    Align align = Align::Stretch;
    AlignSelf alignSelf = AlignSelf::Auto;
    Wrap wrap = Wrap::NoWrap;
    Len width = Auto();
    Len height = Auto();
    bool visible = true;
};

/// Properties for a leaf slot added with designated initializers.
struct SlotStyle {
    Len width = Auto();
    Len height = Auto();
    Edges margin = {};
    AlignSelf alignSelf = AlignSelf::Auto;
    bool visible = true;
};

/// Opaque handle to a declared flex container or leaf slot.
struct Slot {
    int32_t id = -1;
    constexpr bool valid() const { return id >= 0; }
    explicit constexpr operator bool() const { return id >= 0; }
};

namespace detail {

inline constexpr bool isDefinite(float v) {
    return v < kUnboundedThreshold;
}

inline constexpr float clampLen(float v, const Len& len) {
    return std::clamp(v, len.minVal, len.maxVal);
}

inline constexpr Align resolveAlign(Align parentAlign, AlignSelf self) {
    switch (self) {
    case AlignSelf::Start: return Align::Start;
    case AlignSelf::Center: return Align::Center;
    case AlignSelf::End: return Align::End;
    case AlignSelf::Stretch: return Align::Stretch;
    case AlignSelf::Auto:
    default: return parentAlign;
    }
}

inline constexpr bool stretchesCross(const Len& crossSpec, Align effAlign) {
    if (crossSpec.kind == Len::Kind::Grow) return true;
    return crossSpec.kind == Len::Kind::Auto && effAlign == Align::Stretch;
}

struct MeasureHook {
    static constexpr size_t kInlineBytes = 32;
    MeasureFn fn = nullptr;
    const void* userData = nullptr;
    alignas(std::max_align_t) unsigned char storage[kInlineBytes] = {};

    IntrinsicSize invoke(float availW, float availH) const {
        if (!fn) return {};
        return fn(availW, availH, userData ? userData : static_cast<const void*>(storage));
    }
};

struct Node {
    bool isContainer = false;
    bool visible = true;
    Direction dir = Direction::Row;
    Justify justify = Justify::Start;
    Align align = Align::Stretch;
    AlignSelf alignSelf = AlignSelf::Auto;
    Wrap wrap = Wrap::NoWrap;

    Edges padding = {};
    Edges margin = {};
    float gap = 0.0f;
    float rowGap = -1.0f;
    Len width = Auto();
    Len height = Auto();

    MeasureHook measure = {};

    int32_t parent = -1;
    int32_t firstChild = -1;
    int32_t lastChild = -1;
    int32_t nextSibling = -1;
    int32_t childCount = 0;

    // 1-entry measurement cache to keep nested measure+layout strictly O(N).
    bool hasCache = false;
    float cacheAvailW = -1.0f;
    float cacheAvailH = -1.0f;
    float cacheOutW = 0.0f;
    float cacheOutH = 0.0f;

    // Per-solve scratch and output rect.
    float mainSize = 0.0f;
    float crossSize = 0.0f;
    bool frozen = false;
    Rect rect = {};
};

struct LineInfo {
    int32_t scratchOffset = 0;
    int32_t count = 0;
    float mainBasisSum = 0.0f;
    float crossSize = 0.0f;
};

} // namespace detail

/// Reusable flat arena holding all nodes and scratch buffers across frames.
/// Calling reset() preserves vector capacities so steady-state frames perform
/// zero heap allocations.
struct Context {
    std::vector<detail::Node> nodes;
    std::vector<int32_t> scratchItems;
    std::vector<detail::LineInfo> scratchLines;

    void reserve(size_t nodeCount) {
        nodes.reserve(nodeCount);
        scratchItems.reserve(nodeCount);
        scratchLines.reserve(std::max<size_t>(16, nodeCount / 4));
    }

    void reset() {
        nodes.clear();
        scratchItems.clear();
        scratchLines.clear();
    }

    size_t nodeCount() const { return nodes.size(); }
    size_t nodeCapacity() const { return nodes.capacity(); }
    size_t scratchItemCapacity() const { return scratchItems.capacity(); }
    size_t scratchLineCapacity() const { return scratchLines.capacity(); }
};

/// Default thread-local arena for immediate-mode ImGui views.
inline Context& threadLocalContext() {
    thread_local Context ctx;
    return ctx;
}

class Layout;

/// Builder handle for a declared Row or Column container inside a Layout.
class Container {
public:
    constexpr Container() = default;
    constexpr Container(Layout* owner, int32_t id) : owner_(owner), id_(id) {}

    constexpr Slot handle() const { return Slot{id_}; }
    constexpr operator Slot() const { return Slot{id_}; }
    constexpr bool valid() const { return owner_ != nullptr && id_ >= 0; }

    /// Append a leaf slot using a designated SlotStyle.
    Slot slot(const SlotStyle& style);

    /// Append a leaf slot with explicit width/height specs.
    Slot slot(Len w, Len h, AlignSelf alignSelf = AlignSelf::Auto,
              Edges margin = {}, bool visible = true);

    /// Append a leaf slot backed by a stateless MeasureFn + userData pointer.
    Slot measured(MeasureFn fn, const void* userData, Len w = Auto(), Len h = Auto(),
                  AlignSelf alignSelf = AlignSelf::Auto, Edges margin = {}, bool visible = true);

    /// Append a leaf slot backed by a small trivially-copyable lambda (<= 32 B, zero-alloc).
    template <typename Fn>
        requires(sizeof(Fn) <= detail::MeasureHook::kInlineBytes &&
                 alignof(Fn) <= alignof(std::max_align_t) &&
                 std::is_trivially_copyable_v<Fn>)
    Slot measured(Fn fn, Len w = Auto(), Len h = Auto(),
                  AlignSelf alignSelf = AlignSelf::Auto, Edges margin = {}, bool visible = true);

    /// Convenience: fixed-size item along parent's main axis.
    Slot fixed(float mainPx, Len cross = Auto(), AlignSelf alignSelf = AlignSelf::Auto);
    /// Convenience: grow item along parent's main axis.
    Slot grow(float weight = 1.0f, Len cross = Auto(), AlignSelf alignSelf = AlignSelf::Auto);
    /// Convenience: shrinkable item along parent's main axis.
    Slot shrink(float basisPx, float weight = 1.0f, Len cross = Auto(), AlignSelf alignSelf = AlignSelf::Auto);
    /// Convenience: invisible flexible spacer along parent's main axis.
    Slot spacer(float weight = 1.0f);

    /// Append a nested Row container.
    Container row(ContainerStyle style = {});
    /// Append a nested Column container.
    Container col(ContainerStyle style = {});

    Rect rect() const;
    ImVec2 p0() const { return rect().p0(); }
    ImVec2 p1() const { return rect().p1(); }
    ImVec2 size() const { return rect().size(); }
    ImVec4 vec4() const { return rect().vec4(); }

private:
    Layout* owner_ = nullptr;
    int32_t id_ = -1;
};

/// Two-pass immediate-mode Flexbox builder & solver.
/// Pass 1: declare root + nested containers/slots.
/// Pass 2: call solve(...) and read pixel-snapped rects for controls.
class Layout {
public:
    /// Construct using a caller-owned Context (resets node list, keeps capacity).
    explicit Layout(Context& ctx) : ctx_(ctx) { ctx_.reset(); }

    /// Construct using the thread-local Context (resets node list, keeps capacity).
    Layout() : ctx_(threadLocalContext()) { ctx_.reset(); }

    /// Declare the root container (only once per Layout; replaces prior root if recalled).
    Container root(ContainerStyle style = {}) {
        ctx_.reset();
        rootId_ = allocNode(true, style);
        return Container(this, rootId_);
    }

    Container row(ContainerStyle style = {}) {
        style.dir = Direction::Row;
        return root(style);
    }

    Container col(ContainerStyle style = {}) {
        style.dir = Direction::Column;
        return root(style);
    }

    /// Solve layout into definite (or partially unbounded) bounds.
    /// Any axis >= kUnboundedThreshold is shrink-wrapped to content.
    Rect solve(Rect bounds, bool snapToDpi = true) {
        if (rootId_ < 0 || rootId_ >= static_cast<int32_t>(ctx_.nodes.size())) return {};
        auto& r = ctx_.nodes[static_cast<size_t>(rootId_)];
        if (!r.visible) {
            zeroTree(rootId_);
            return {};
        }

        const float availW = detail::isDefinite(bounds.w) ? std::max(0.0f, bounds.w) : kUnbounded;
        const float availH = detail::isDefinite(bounds.h) ? std::max(0.0f, bounds.h) : kUnbounded;

        float rootW = resolveRootAxis(r.width, availW);
        float rootH = resolveRootAxis(r.height, availH);
        if (!detail::isDefinite(rootW) || !detail::isDefinite(rootH)) {
            const IntrinsicSize measured = measureNode(rootId_, rootW, rootH);
            if (!detail::isDefinite(rootW)) rootW = detail::clampLen(measured.w, r.width);
            if (!detail::isDefinite(rootH)) rootH = detail::clampLen(measured.h, r.height);
        }

        const float x = bounds.x + r.margin.left;
        const float y = bounds.y + r.margin.top;
        const float innerW = std::max(0.0f, rootW - r.margin.left - r.margin.right);
        const float innerH = std::max(0.0f, rootH - r.margin.top - r.margin.bottom);
        layoutContainer(rootId_, x, y, innerW, innerH);

        if (snapToDpi) {
            snapTree(rootId_);
        }
        return r.rect;
    }

    Rect solve(ImVec2 p0, ImVec2 p1, bool snapToDpi = true) {
        return solve(Rect{p0.x, p0.y, std::max(0.0f, p1.x - p0.x), std::max(0.0f, p1.y - p0.y)}, snapToDpi);
    }

    /// Solve with fixed width and shrink-wrapped height (ideal for scrollable cards/pages).
    Rect solveWidth(ImVec2 origin, float width, bool snapToDpi = true) {
        return solve(Rect{origin.x, origin.y, std::max(0.0f, width), kUnbounded}, snapToDpi);
    }

    /// Solve with shrink-wrapped width and height up to optional max bounds.
    Rect solveAuto(ImVec2 origin = ImVec2(0.0f, 0.0f), bool snapToDpi = true) {
        return solve(Rect{origin.x, origin.y, kUnbounded, kUnbounded}, snapToDpi);
    }

    Rect rect(Slot s) const {
        if (!s.valid() || static_cast<size_t>(s.id) >= ctx_.nodes.size()) return {};
        return ctx_.nodes[static_cast<size_t>(s.id)].rect;
    }

    ImVec2 p0(Slot s) const { return rect(s).p0(); }
    ImVec2 p1(Slot s) const { return rect(s).p1(); }
    ImVec2 size(Slot s) const { return rect(s).size(); }
    ImVec4 vec4(Slot s) const { return rect(s).vec4(); }

    bool isVisible(Slot s) const {
        if (!s.valid() || static_cast<size_t>(s.id) >= ctx_.nodes.size()) return false;
        const auto& n = ctx_.nodes[static_cast<size_t>(s.id)];
        return n.visible && !n.rect.empty();
    }

    const Context& context() const { return ctx_; }

private:
    friend class Container;

    Context& ctx_;
    int32_t rootId_ = -1;

    static const Len& mainSpec(const detail::Node& n, Direction parentDir) {
        return parentDir == Direction::Row ? n.width : n.height;
    }
    static const Len& crossSpec(const detail::Node& n, Direction parentDir) {
        return parentDir == Direction::Row ? n.height : n.width;
    }
    static float mainMargin(const detail::Node& n, Direction parentDir) {
        return parentDir == Direction::Row ? (n.margin.left + n.margin.right) : (n.margin.top + n.margin.bottom);
    }
    static float crossMargin(const detail::Node& n, Direction parentDir) {
        return parentDir == Direction::Row ? (n.margin.top + n.margin.bottom) : (n.margin.left + n.margin.right);
    }
    static float mainPad(const detail::Node& c) {
        return c.dir == Direction::Row ? (c.padding.left + c.padding.right) : (c.padding.top + c.padding.bottom);
    }
    static float crossPad(const detail::Node& c) {
        return c.dir == Direction::Row ? (c.padding.top + c.padding.bottom) : (c.padding.left + c.padding.right);
    }
    static float effectiveRowGap(const detail::Node& c) {
        return c.rowGap >= 0.0f ? c.rowGap : c.gap;
    }

    int32_t allocNode(bool isContainer, const ContainerStyle& style) {
        const int32_t id = static_cast<int32_t>(ctx_.nodes.size());
        detail::Node n;
        n.isContainer = isContainer;
        n.visible = style.visible;
        n.dir = style.dir;
        n.justify = style.justify;
        n.align = style.align;
        n.alignSelf = style.alignSelf;
        n.wrap = style.wrap;
        n.padding = style.padding;
        n.margin = style.margin;
        n.gap = std::max(0.0f, style.gap);
        n.rowGap = style.rowGap;
        n.width = style.width;
        n.height = style.height;
        ctx_.nodes.push_back(n);
        return id;
    }

    int32_t appendChild(int32_t parentId, bool isContainer, const ContainerStyle& style) {
        if (parentId < 0 || static_cast<size_t>(parentId) >= ctx_.nodes.size()) return -1;
        const int32_t childId = allocNode(isContainer, style);
        auto& parent = ctx_.nodes[static_cast<size_t>(parentId)];
        ctx_.nodes[static_cast<size_t>(childId)].parent = parentId;
        if (parent.lastChild < 0) {
            parent.firstChild = childId;
            parent.lastChild = childId;
        } else {
            ctx_.nodes[static_cast<size_t>(parent.lastChild)].nextSibling = childId;
            parent.lastChild = childId;
        }
        ++parent.childCount;
        return childId;
    }

    float resolveRootAxis(const Len& spec, float avail) {
        if (spec.kind == Len::Kind::Fixed) {
            return detail::clampLen(spec.size, spec);
        }
        if (detail::isDefinite(avail)) {
            if (spec.kind == Len::Kind::Fraction) {
                return detail::clampLen(avail * spec.size, spec);
            }
            return detail::clampLen(avail, spec);
        }
        return kUnbounded;
    }

    static float resolveFixedOrFrac(const Len& spec, float avail) {
        if (spec.kind == Len::Kind::Fixed) return detail::clampLen(spec.size, spec);
        if (spec.kind == Len::Kind::Fraction) {
            return detail::isDefinite(avail) ? detail::clampLen(avail * spec.size, spec) : spec.minVal;
        }
        return -1.0f;
    }

    IntrinsicSize measureNode(int32_t id, float availW, float availH) {
        auto& n = ctx_.nodes[static_cast<size_t>(id)];
        if (!n.visible) return {};

        if (n.hasCache && std::fabs(n.cacheAvailW - availW) < 1e-4f && std::fabs(n.cacheAvailH - availH) < 1e-4f) {
            return {n.cacheOutW, n.cacheOutH};
        }

        const float fixedW = resolveFixedOrFrac(n.width, availW);
        const float fixedH = resolveFixedOrFrac(n.height, availH);
        const float childAvailW = fixedW >= 0.0f ? fixedW : availW;
        const float childAvailH = fixedH >= 0.0f ? fixedH : availH;

        float outW = fixedW;
        float outH = fixedH;

        if (!n.isContainer) {
            IntrinsicSize hook = n.measure.fn
                ? n.measure.invoke(std::max(0.0f, childAvailW - n.margin.left - n.margin.right),
                                   std::max(0.0f, childAvailH - n.margin.top - n.margin.bottom))
                : IntrinsicSize{};
            if (outW < 0.0f) {
                const float baseW = std::max(n.width.size, hook.w);
                outW = detail::clampLen(baseW, n.width);
            }
            if (outH < 0.0f) {
                const float baseH = std::max(n.height.size, hook.h);
                outH = detail::clampLen(baseH, n.height);
            }
        } else if (outW < 0.0f || outH < 0.0f) {
            const IntrinsicSize content = measureContainer(id, childAvailW, childAvailH);
            if (outW < 0.0f) outW = detail::clampLen(std::max(n.width.size, content.w), n.width);
            if (outH < 0.0f) outH = detail::clampLen(std::max(n.height.size, content.h), n.height);
        }

        auto& refreshed = ctx_.nodes[static_cast<size_t>(id)];
        refreshed.hasCache = true;
        refreshed.cacheAvailW = availW;
        refreshed.cacheAvailH = availH;
        refreshed.cacheOutW = outW;
        refreshed.cacheOutH = outH;
        return {outW, outH};
    }

    IntrinsicSize measureContainer(int32_t id, float availW, float availH) {
        const auto n = ctx_.nodes[static_cast<size_t>(id)];
        const bool isRow = (n.dir == Direction::Row);
        const float padM = mainPad(n);
        const float padC = crossPad(n);
        const float availMain = isRow ? availW : availH;
        const float availCross = isRow ? availH : availW;
        const float innerMain = detail::isDefinite(availMain) ? std::max(0.0f, availMain - padM) : kUnbounded;
        const float innerCross = detail::isDefinite(availCross) ? std::max(0.0f, availCross - padC) : kUnbounded;
        const float rGap = effectiveRowGap(n);

        // If wrapping on a Row/Column with a definite main size, simulate line breaks and
        // flex main sizing per line so wrapped children measure their heights accurately.
        if (n.wrap == Wrap::Wrap && detail::isDefinite(innerMain)) {
            const size_t itemsBase = ctx_.scratchItems.size();
            const size_t linesBase = ctx_.scratchLines.size();
            buildLines(id, innerMain, innerCross);

            float maxLineMain = 0.0f;
            float totalCross = 0.0f;
            const size_t lineCount = ctx_.scratchLines.size() - linesBase;
            for (size_t li = 0; li < lineCount; ++li) {
                const detail::LineInfo line = ctx_.scratchLines[linesBase + li];
                resolveLineMain(line, n.gap, innerMain);
                const float lineCross = resolveLineCross(id, line, innerCross);
                float lineUsedMain = line.count > 1 ? n.gap * static_cast<float>(line.count - 1) : 0.0f;
                for (int32_t k = 0; k < line.count; ++k) {
                    const auto& ch = ctx_.nodes[static_cast<size_t>(ctx_.scratchItems[static_cast<size_t>(line.scratchOffset + k)])];
                    lineUsedMain += ch.mainSize + mainMargin(ch, n.dir);
                }
                maxLineMain = std::max(maxLineMain, lineUsedMain);
                totalCross += lineCross;
            }
            if (lineCount > 1) totalCross += rGap * static_cast<float>(lineCount - 1);

            ctx_.scratchItems.resize(itemsBase);
            ctx_.scratchLines.resize(linesBase);
            return isRow ? IntrinsicSize{maxLineMain + padM, totalCross + padC}
                         : IntrinsicSize{totalCross + padC, maxLineMain + padM};
        }

        // Single-line (NoWrap or unbounded main):
        // If main is definite and any child stretches/flexes, resolve main sizes first so
        // children with height: Auto (like wrapped Column or MeasureFn) get exact width!
        const size_t itemsBase = ctx_.scratchItems.size();
        const size_t linesBase = ctx_.scratchLines.size();
        buildSingleLine(id, innerMain, innerCross);

        float sumMain = 0.0f;
        float maxCross = 0.0f;
        if (ctx_.scratchLines.size() > linesBase) {
            const detail::LineInfo line = ctx_.scratchLines[linesBase];
            if (detail::isDefinite(innerMain)) {
                resolveLineMain(line, n.gap, innerMain);
                maxCross = resolveLineCross(id, line, innerCross);
                for (int32_t k = 0; k < line.count; ++k) {
                    const auto& ch = ctx_.nodes[static_cast<size_t>(ctx_.scratchItems[static_cast<size_t>(line.scratchOffset + k)])];
                    sumMain += ch.mainSize + mainMargin(ch, n.dir);
                }
            } else {
                for (int32_t k = 0; k < line.count; ++k) {
                    const int32_t cid = ctx_.scratchItems[static_cast<size_t>(line.scratchOffset + k)];
                    const auto& ch = ctx_.nodes[static_cast<size_t>(cid)];
                    sumMain += ch.mainSize + mainMargin(ch, n.dir);
                    maxCross = std::max(maxCross, ch.crossSize + crossMargin(ch, n.dir));
                }
            }
            if (line.count > 1) sumMain += n.gap * static_cast<float>(line.count - 1);
        }

        ctx_.scratchItems.resize(itemsBase);
        ctx_.scratchLines.resize(linesBase);
        return isRow ? IntrinsicSize{sumMain + padM, maxCross + padC}
                     : IntrinsicSize{maxCross + padC, sumMain + padM};
    }

    float childBasisMain(int32_t cid, Direction parentDir, float availMain, float availCross) {
        const auto ch = ctx_.nodes[static_cast<size_t>(cid)];
        const Len& ms = mainSpec(ch, parentDir);
        if (ms.kind == Len::Kind::Fixed) return detail::clampLen(ms.size, ms);
        if (ms.kind == Len::Kind::Fraction) {
            return detail::isDefinite(availMain) ? detail::clampLen(availMain * ms.size, ms) : ms.minVal;
        }
        if (ms.kind == Len::Kind::Grow) {
            return detail::clampLen(ms.size, ms);
        }
        if (ms.kind == Len::Kind::Shrink && (ms.size > 0.0f || (!ch.isContainer && !ch.measure.fn))) {
            return detail::clampLen(ms.size, ms);
        }
        // Auto (or Shrink with basis 0 on a container/measured node): measure intrinsic.
        const float mW = parentDir == Direction::Row ? kUnbounded : availCross;
        const float mH = parentDir == Direction::Row ? availCross : kUnbounded;
        const IntrinsicSize sz = measureNode(cid, mW, mH);
        const float raw = (parentDir == Direction::Row) ? sz.w : sz.h;
        return detail::clampLen(raw, ms);
    }

    float childBasisCross(int32_t cid, Direction parentDir, float resolvedMain, float availCross) {
        const auto ch = ctx_.nodes[static_cast<size_t>(cid)];
        const Len& cs = crossSpec(ch, parentDir);
        if (cs.kind == Len::Kind::Fixed) return detail::clampLen(cs.size, cs);
        if (cs.kind == Len::Kind::Fraction) {
            return detail::isDefinite(availCross) ? detail::clampLen(availCross * cs.size, cs) : cs.minVal;
        }
        const float mW = parentDir == Direction::Row ? resolvedMain : availCross;
        const float mH = parentDir == Direction::Row ? availCross : resolvedMain;
        const IntrinsicSize sz = measureNode(cid, mW, mH);
        const float raw = (parentDir == Direction::Row) ? sz.h : sz.w;
        return detail::clampLen(raw, cs);
    }

    void buildSingleLine(int32_t containerId, float innerMain, float innerCross) {
        const auto parent = ctx_.nodes[static_cast<size_t>(containerId)];
        const int32_t offset = static_cast<int32_t>(ctx_.scratchItems.size());
        int32_t count = 0;
        float sum = 0.0f;
        for (int32_t cid = parent.firstChild; cid != -1;) {
            const int32_t next = ctx_.nodes[static_cast<size_t>(cid)].nextSibling;
            if (!ctx_.nodes[static_cast<size_t>(cid)].visible) {
                ctx_.nodes[static_cast<size_t>(cid)].rect = {};
                cid = next;
                continue;
            }
            const float availC = std::max(0.0f, innerCross - crossMargin(ctx_.nodes[static_cast<size_t>(cid)], parent.dir));
            const float basisM = childBasisMain(cid, parent.dir, innerMain, detail::isDefinite(innerCross) ? availC : kUnbounded);
            ctx_.nodes[static_cast<size_t>(cid)].mainSize = basisM;
            ctx_.nodes[static_cast<size_t>(cid)].crossSize =
                detail::isDefinite(innerMain) ? 0.0f : childBasisCross(cid, parent.dir, basisM, detail::isDefinite(innerCross) ? availC : kUnbounded);
            sum += basisM + mainMargin(ctx_.nodes[static_cast<size_t>(cid)], parent.dir);
            ctx_.scratchItems.push_back(cid);
            ++count;
            cid = next;
        }
        if (count > 0) {
            ctx_.scratchLines.push_back(detail::LineInfo{offset, count, sum, 0.0f});
        }
    }

    void buildLines(int32_t containerId, float innerMain, float innerCross) {
        const auto parent = ctx_.nodes[static_cast<size_t>(containerId)];
        int32_t lineOffset = static_cast<int32_t>(ctx_.scratchItems.size());
        int32_t lineCount = 0;
        float lineSum = 0.0f;

        for (int32_t cid = parent.firstChild; cid != -1;) {
            const int32_t next = ctx_.nodes[static_cast<size_t>(cid)].nextSibling;
            if (!ctx_.nodes[static_cast<size_t>(cid)].visible) {
                ctx_.nodes[static_cast<size_t>(cid)].rect = {};
                cid = next;
                continue;
            }
            const float availC = std::max(0.0f, innerCross - crossMargin(ctx_.nodes[static_cast<size_t>(cid)], parent.dir));
            const float basisM = childBasisMain(cid, parent.dir, innerMain, detail::isDefinite(innerCross) ? availC : kUnbounded);
            const float outerM = basisM + mainMargin(ctx_.nodes[static_cast<size_t>(cid)], parent.dir);
            ctx_.nodes[static_cast<size_t>(cid)].mainSize = basisM;

            const float gapIfAdded = (lineCount > 0) ? parent.gap : 0.0f;
            if (lineCount > 0 && lineSum + gapIfAdded + outerM > innerMain + 1e-3f) {
                ctx_.scratchLines.push_back(detail::LineInfo{lineOffset, lineCount, lineSum, 0.0f});
                lineOffset = static_cast<int32_t>(ctx_.scratchItems.size());
                lineCount = 0;
                lineSum = 0.0f;
            }
            if (lineCount > 0) lineSum += parent.gap;
            lineSum += outerM;
            ctx_.scratchItems.push_back(cid);
            ++lineCount;
            cid = next;
        }
        if (lineCount > 0) {
            ctx_.scratchLines.push_back(detail::LineInfo{lineOffset, lineCount, lineSum, 0.0f});
        }
    }

    void resolveLineMain(detail::LineInfo line, float gap, float innerMain) {
        if (line.count <= 0 || !detail::isDefinite(innerMain)) return;
        const int32_t parentId = ctx_.nodes[static_cast<size_t>(ctx_.scratchItems[static_cast<size_t>(line.scratchOffset)])].parent;
        const Direction dir = ctx_.nodes[static_cast<size_t>(parentId)].dir;
        const float gapsTotal = (line.count > 1) ? gap * static_cast<float>(line.count - 1) : 0.0f;

        float initialUsed = gapsTotal;
        for (int32_t k = 0; k < line.count; ++k) {
            auto& ch = ctx_.nodes[static_cast<size_t>(ctx_.scratchItems[static_cast<size_t>(line.scratchOffset + k)])];
            ch.frozen = false;
            initialUsed += ch.mainSize + mainMargin(ch, dir);
        }

        const float freeSpace = innerMain - initialUsed;
        if (std::fabs(freeSpace) <= 1e-4f) return;

        if (freeSpace > 0.0f) {
            distributeGrow(line, dir, gapsTotal, innerMain);
        } else {
            distributeShrink(line, dir, gapsTotal, innerMain);
        }
    }

    void distributeGrow(detail::LineInfo line, Direction dir, float gapsTotal, float innerMain) {
        for (int32_t k = 0; k < line.count; ++k) {
            auto& ch = ctx_.nodes[static_cast<size_t>(ctx_.scratchItems[static_cast<size_t>(line.scratchOffset + k)])];
            if (mainSpec(ch, dir).growWeight <= 0.0f) ch.frozen = true;
        }
        for (int32_t iter = 0; iter < line.count; ++iter) {
            float totalWeight = 0.0f;
            float used = gapsTotal;
            for (int32_t k = 0; k < line.count; ++k) {
                const auto& ch = ctx_.nodes[static_cast<size_t>(ctx_.scratchItems[static_cast<size_t>(line.scratchOffset + k)])];
                used += ch.mainSize + mainMargin(ch, dir);
                if (!ch.frozen) totalWeight += mainSpec(ch, dir).growWeight;
            }
            const float rem = innerMain - used;
            if (totalWeight <= 0.0f || rem <= 1e-4f) break;

            bool clampedAny = false;
            for (int32_t k = 0; k < line.count; ++k) {
                auto& ch = ctx_.nodes[static_cast<size_t>(ctx_.scratchItems[static_cast<size_t>(line.scratchOffset + k)])];
                if (ch.frozen) continue;
                const Len& ms = mainSpec(ch, dir);
                const float next = ch.mainSize + rem * (ms.growWeight / totalWeight);
                const float clamped = detail::clampLen(next, ms);
                if (std::fabs(clamped - next) > 1e-4f) {
                    ch.mainSize = clamped;
                    ch.frozen = true;
                    clampedAny = true;
                }
            }
            if (!clampedAny) {
                for (int32_t k = 0; k < line.count; ++k) {
                    auto& ch = ctx_.nodes[static_cast<size_t>(ctx_.scratchItems[static_cast<size_t>(line.scratchOffset + k)])];
                    if (ch.frozen) continue;
                    ch.mainSize = detail::clampLen(
                        ch.mainSize + rem * (mainSpec(ch, dir).growWeight / totalWeight),
                        mainSpec(ch, dir));
                }
                break;
            }
        }
    }

    void distributeShrink(detail::LineInfo line, Direction dir, float gapsTotal, float innerMain) {
        for (int32_t k = 0; k < line.count; ++k) {
            auto& ch = ctx_.nodes[static_cast<size_t>(ctx_.scratchItems[static_cast<size_t>(line.scratchOffset + k)])];
            const Len& ms = mainSpec(ch, dir);
            if (ms.shrinkWeight <= 0.0f || ch.mainSize <= ms.minVal + 1e-4f) {
                ch.frozen = true;
            }
        }
        for (int32_t iter = 0; iter < line.count; ++iter) {
            float totalScaled = 0.0f;
            float totalWeightOnly = 0.0f;
            float used = gapsTotal;
            for (int32_t k = 0; k < line.count; ++k) {
                const auto& ch = ctx_.nodes[static_cast<size_t>(ctx_.scratchItems[static_cast<size_t>(line.scratchOffset + k)])];
                used += ch.mainSize + mainMargin(ch, dir);
                if (!ch.frozen) {
                    const float w = mainSpec(ch, dir).shrinkWeight;
                    totalScaled += w * std::max(0.0f, ch.mainSize);
                    totalWeightOnly += w;
                }
            }
            const float overflow = used - innerMain;
            if (overflow <= 1e-4f || totalWeightOnly <= 0.0f) break;
            const bool useScaled = totalScaled > 1e-4f;
            const float denom = useScaled ? totalScaled : totalWeightOnly;

            bool clampedAny = false;
            for (int32_t k = 0; k < line.count; ++k) {
                auto& ch = ctx_.nodes[static_cast<size_t>(ctx_.scratchItems[static_cast<size_t>(line.scratchOffset + k)])];
                if (ch.frozen) continue;
                const Len& ms = mainSpec(ch, dir);
                const float factor = useScaled ? (ms.shrinkWeight * std::max(0.0f, ch.mainSize)) : ms.shrinkWeight;
                const float next = ch.mainSize - overflow * (factor / denom);
                const float clamped = detail::clampLen(next, ms);
                if (std::fabs(clamped - next) > 1e-4f) {
                    ch.mainSize = clamped;
                    ch.frozen = true;
                    clampedAny = true;
                }
            }
            if (!clampedAny) {
                for (int32_t k = 0; k < line.count; ++k) {
                    auto& ch = ctx_.nodes[static_cast<size_t>(ctx_.scratchItems[static_cast<size_t>(line.scratchOffset + k)])];
                    if (ch.frozen) continue;
                    const Len& ms = mainSpec(ch, dir);
                    const float factor = useScaled ? (ms.shrinkWeight * std::max(0.0f, ch.mainSize)) : ms.shrinkWeight;
                    ch.mainSize = detail::clampLen(ch.mainSize - overflow * (factor / denom), ms);
                }
                break;
            }
        }
    }

    float resolveLineCross(int32_t containerId, detail::LineInfo line, float innerCross) {
        const auto parent = ctx_.nodes[static_cast<size_t>(containerId)];
        float maxCross = 0.0f;
        for (int32_t k = 0; k < line.count; ++k) {
            const int32_t cid = ctx_.scratchItems[static_cast<size_t>(line.scratchOffset + k)];
            const auto& ch = ctx_.nodes[static_cast<size_t>(cid)];
            const float availC = detail::isDefinite(innerCross)
                ? std::max(0.0f, innerCross - crossMargin(ch, parent.dir))
                : kUnbounded;
            const float c = childBasisCross(cid, parent.dir, ch.mainSize, availC);
            ctx_.nodes[static_cast<size_t>(cid)].crossSize = c;
            maxCross = std::max(maxCross, c + crossMargin(ctx_.nodes[static_cast<size_t>(cid)], parent.dir));
        }
        return maxCross;
    }

    void layoutContainer(int32_t id, float x, float y, float w, float h) {
        auto& self = ctx_.nodes[static_cast<size_t>(id)];
        self.rect = Rect{x, y, w, h};

        const bool isRow = (self.dir == Direction::Row);
        const float padM = mainPad(self);
        const float padC = crossPad(self);
        const float innerMain = std::max(0.0f, (isRow ? w : h) - padM);
        const float innerCross = std::max(0.0f, (isRow ? h : w) - padC);
        const float mainOrigin = (isRow ? (x + self.padding.left) : (y + self.padding.top));
        const float crossOrigin = (isRow ? (y + self.padding.top) : (x + self.padding.left));
        const float gap = self.gap;
        const float rGap = effectiveRowGap(self);
        const Direction dir = self.dir;
        const Justify justify = self.justify;
        const Align align = self.align;
        const Wrap wrap = self.wrap;

        const size_t itemsBase = ctx_.scratchItems.size();
        const size_t linesBase = ctx_.scratchLines.size();

        if (wrap == Wrap::Wrap) {
            buildLines(id, innerMain, innerCross);
        } else {
            buildSingleLine(id, innerMain, innerCross);
        }

        const size_t lineCount = ctx_.scratchLines.size() - linesBase;
        for (size_t li = 0; li < lineCount; ++li) {
            const detail::LineInfo line = ctx_.scratchLines[linesBase + li];
            resolveLineMain(line, gap, innerMain);
            const float resolvedCross = resolveLineCross(id, line, innerCross);
            ctx_.scratchLines[linesBase + li].crossSize = resolvedCross;
        }
        if (lineCount == 1) {
            ctx_.scratchLines[linesBase].crossSize = innerCross;
        }

        float crossCursor = crossOrigin;
        for (size_t li = 0; li < lineCount; ++li) {
            if (li > 0) crossCursor += rGap;
            const detail::LineInfo line = ctx_.scratchLines[linesBase + li];

            // Compute main-axis start offset and inter-item step according to justify.
            float usedNoGap = 0.0f;
            for (int32_t k = 0; k < line.count; ++k) {
                const auto& ch = ctx_.nodes[static_cast<size_t>(ctx_.scratchItems[static_cast<size_t>(line.scratchOffset + k)])];
                usedNoGap += ch.mainSize + mainMargin(ch, dir);
            }
            const float minGaps = line.count > 1 ? gap * static_cast<float>(line.count - 1) : 0.0f;
            const float freeSpace = std::max(0.0f, innerMain - usedNoGap - minGaps);

            float mainCursor = mainOrigin;
            float itemGap = gap;
            switch (justify) {
            case Justify::Start:
                break;
            case Justify::Center:
                mainCursor += freeSpace * 0.5f;
                break;
            case Justify::End:
                mainCursor += freeSpace;
                break;
            case Justify::SpaceBetween:
                if (line.count > 1) itemGap = gap + freeSpace / static_cast<float>(line.count - 1);
                break;
            case Justify::SpaceAround:
                if (line.count == 1) {
                    mainCursor += freeSpace * 0.5f;
                } else if (line.count > 1) {
                    const float half = freeSpace / static_cast<float>(line.count * 2);
                    mainCursor += half;
                    itemGap = gap + half * 2.0f;
                }
                break;
            case Justify::SpaceEvenly:
                if (line.count >= 1) {
                    const float slot = freeSpace / static_cast<float>(line.count + 1);
                    mainCursor += slot;
                    itemGap = gap + slot;
                }
                break;
            }

            for (int32_t k = 0; k < line.count; ++k) {
                if (k > 0) mainCursor += itemGap;
                const int32_t cid = ctx_.scratchItems[static_cast<size_t>(line.scratchOffset + k)];
                const auto chCopy = ctx_.nodes[static_cast<size_t>(cid)];
                const Len& cs = crossSpec(chCopy, dir);
                const Align effAlign = detail::resolveAlign(align, chCopy.alignSelf);
                const float availChildCross = std::max(0.0f, line.crossSize - crossMargin(chCopy, dir));

                float childCross = chCopy.crossSize;
                if (detail::stretchesCross(cs, effAlign)) {
                    childCross = detail::clampLen(availChildCross, cs);
                }

                float crossOffset = 0.0f;
                switch (effAlign) {
                case Align::Start:
                case Align::Stretch:
                    crossOffset = 0.0f;
                    break;
                case Align::Center:
                    crossOffset = (availChildCross - childCross) * 0.5f;
                    break;
                case Align::End:
                    crossOffset = availChildCross - childCross;
                    break;
                }

                const float cx = isRow
                    ? (mainCursor + chCopy.margin.left)
                    : (crossCursor + chCopy.margin.left + crossOffset);
                const float cy = isRow
                    ? (crossCursor + chCopy.margin.top + crossOffset)
                    : (mainCursor + chCopy.margin.top);
                const float cw = isRow ? chCopy.mainSize : childCross;
                const float chH = isRow ? childCross : chCopy.mainSize;

                if (chCopy.isContainer) {
                    layoutContainer(cid, cx, cy, cw, chH);
                } else {
                    ctx_.nodes[static_cast<size_t>(cid)].rect = Rect{cx, cy, cw, chH};
                }

                mainCursor += chCopy.mainSize + mainMargin(chCopy, dir);
            }

            crossCursor += line.crossSize;
        }

        ctx_.scratchItems.resize(itemsBase);
        ctx_.scratchLines.resize(linesBase);
    }

    void zeroTree(int32_t id) {
        if (id < 0 || static_cast<size_t>(id) >= ctx_.nodes.size()) return;
        auto& n = ctx_.nodes[static_cast<size_t>(id)];
        n.rect = {};
        for (int32_t cid = n.firstChild; cid != -1; cid = ctx_.nodes[static_cast<size_t>(cid)].nextSibling) {
            zeroTree(cid);
        }
    }

    void snapTree(int32_t id) {
        if (id < 0 || static_cast<size_t>(id) >= ctx_.nodes.size()) return;
        auto& n = ctx_.nodes[static_cast<size_t>(id)];
        if (n.visible) {
            const ImVec2 a = snapPt(n.rect.x, n.rect.y);
            const ImVec2 b = snapPt(n.rect.x + n.rect.w, n.rect.y + n.rect.h);
            n.rect = Rect{a.x, a.y, std::max(0.0f, b.x - a.x), std::max(0.0f, b.y - a.y)};
        } else {
            n.rect = {};
        }
        for (int32_t cid = n.firstChild; cid != -1; cid = ctx_.nodes[static_cast<size_t>(cid)].nextSibling) {
            snapTree(cid);
        }
    }
};

inline Slot Container::slot(const SlotStyle& style) {
    if (!valid()) return {};
    ContainerStyle cs;
    cs.width = style.width;
    cs.height = style.height;
    cs.margin = style.margin;
    cs.alignSelf = style.alignSelf;
    cs.visible = style.visible;
    return Slot{owner_->appendChild(id_, false, cs)};
}

inline Slot Container::slot(Len w, Len h, AlignSelf alignSelf, Edges margin, bool visible) {
    return slot(SlotStyle{w, h, margin, alignSelf, visible});
}

inline Slot Container::measured(MeasureFn fn, const void* userData, Len w, Len h,
                                AlignSelf alignSelf, Edges margin, bool visible) {
    const Slot s = slot(SlotStyle{w, h, margin, alignSelf, visible});
    if (s.valid()) {
        auto& node = owner_->ctx_.nodes[static_cast<size_t>(s.id)];
        node.measure.fn = fn;
        node.measure.userData = userData;
    }
    return s;
}

template <typename Fn>
    requires(sizeof(Fn) <= detail::MeasureHook::kInlineBytes &&
             alignof(Fn) <= alignof(std::max_align_t) &&
             std::is_trivially_copyable_v<Fn>)
inline Slot Container::measured(Fn fn, Len w, Len h,
                                AlignSelf alignSelf, Edges margin, bool visible) {
    const Slot s = slot(SlotStyle{w, h, margin, alignSelf, visible});
    if (s.valid()) {
        auto& node = owner_->ctx_.nodes[static_cast<size_t>(s.id)];
        ::new (static_cast<void*>(node.measure.storage)) Fn(fn);
        node.measure.userData = nullptr;
        node.measure.fn = [](float availW, float availH, const void* raw) -> IntrinsicSize {
            const Fn* callable = std::launder(static_cast<const Fn*>(raw));
            return (*callable)(availW, availH);
        };
    }
    return s;
}

inline Slot Container::fixed(float mainPx, Len cross, AlignSelf alignSelf) {
    if (!valid()) return {};
    const Direction dir = owner_->ctx_.nodes[static_cast<size_t>(id_)].dir;
    return dir == Direction::Row
        ? slot(Fixed(mainPx), cross, alignSelf)
        : slot(cross, Fixed(mainPx), alignSelf);
}

inline Slot Container::grow(float weight, Len cross, AlignSelf alignSelf) {
    if (!valid()) return {};
    const Direction dir = owner_->ctx_.nodes[static_cast<size_t>(id_)].dir;
    return dir == Direction::Row
        ? slot(Grow(weight), cross, alignSelf)
        : slot(cross, Grow(weight), alignSelf);
}

inline Slot Container::shrink(float basisPx, float weight, Len cross, AlignSelf alignSelf) {
    if (!valid()) return {};
    const Direction dir = owner_->ctx_.nodes[static_cast<size_t>(id_)].dir;
    return dir == Direction::Row
        ? slot(Shrink(basisPx, weight), cross, alignSelf)
        : slot(cross, Shrink(basisPx, weight), alignSelf);
}

inline Slot Container::spacer(float weight) {
    return grow(weight, Fixed(0.0f));
}

inline Container Container::row(ContainerStyle style) {
    if (!valid()) return {};
    style.dir = Direction::Row;
    return Container(owner_, owner_->appendChild(id_, true, style));
}

inline Container Container::col(ContainerStyle style) {
    if (!valid()) return {};
    style.dir = Direction::Column;
    return Container(owner_, owner_->appendChild(id_, true, style));
}

inline Rect Container::rect() const {
    return owner_ ? owner_->rect(Slot{id_}) : Rect{};
}

} // namespace upix::ui::flex
