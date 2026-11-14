#include "ui/controls.hpp"
#include "ui/flex.hpp"
#include "ui/geist.hpp"
#include "ui/motion.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#define CHECK(expr)                                                                 \
    do {                                                                            \
        if (!(expr)) {                                                              \
            std::fprintf(stderr, "CHECK failed (%s:%d): %s\n", __FILE__, __LINE__,  \
                         #expr);                                                    \
            std::exit(1);                                                           \
        }                                                                           \
    } while (0)

int main() {
    using namespace upix::ui;

    // 1. Geist Light & Dark tokens sanity
    geist::mode() = geist::Mode::Dark;
    CHECK(geist::tokens().bg == hex(0x000000));
    CHECK(geist::tokens().wellBg == hex(0x000000));
    CHECK(geist::tokens().lowest == hex(0x000000));
    CHECK(geist::tokens().panelBg == hex(0x0A0A0A));
    CHECK(geist::tokens().surface == hex(0x111111));
    CHECK(geist::tokens().popupBg == hex(0x111111));
    CHECK(geist::tokens().recessed == hex(0x141414));
    CHECK(geist::tokens().outline == hex(0x1F1F1F));
    CHECK(geist::tokens().outlineStrong == hex(0x333333));
    CHECK(geist::tokens().ctaDisabledBg == hex(0x141414));
    CHECK(geist::tokens().ctaDisabledFg == hex(0x666666));
    CHECK(geist::tokens().segTrack == hex(0x000000));
    CHECK(geist::tokens().segThumb == hex(0x1F1F1F));
    CHECK(geist::tokens().segBorder == hex(0x242424));
    CHECK(geist::tokens().segFg == hex(0xFFFFFF));
    CHECK(geist::tokens().segFgMuted == hex(0x888888));
    CHECK(geist::tokens().segThumbRim == rgba(255, 255, 255, 31));
    CHECK(geist::tokens().dropzoneBg == hex(0x070707));
    CHECK(geist::tokens().dropzoneBorder == hex(0x262626));
    CHECK(geist::tokens().switchKnobOff == hex(0xD4D4D4));
    CHECK(geist::tokens().tagBg == hex(0x1F1F1F));
    CHECK(geist::tokens().accent == hex(0x3291FF));
    CHECK(geist::tokens().regMark == hex(0x404040));
    CHECK(geist::tokens().error == hex(0xE5484D));
    CHECK(geist::tokens().errorHover == hex(0xFF6169));
    CHECK(geist::tokens().dangerBg == rgba(229, 72, 77, 36));
    CHECK(geist::tokens().dangerBgHover == rgba(229, 72, 77, 61));
    CHECK(geist::tokens().dangerBgPress == rgba(229, 72, 77, 82));
    CHECK(geist::tokens().veilAlpha == 0.82f);
    CHECK(geist::tokens().scrimAlpha == 0.72f);
    CHECK(geist::tokens().curtainHaloAlpha == 0.72f);
    geist::mode() = geist::Mode::Light;
    CHECK(geist::tokens().bg == hex(0xFAFAFA));
    CHECK(geist::tokens().wellBg == hex(0xFAFAFA));
    CHECK(geist::tokens().panelBg == hex(0xFFFFFF));
    CHECK(geist::tokens().surface == hex(0xFFFFFF));
    CHECK(geist::tokens().recessed == hex(0xF4F4F4));
    CHECK(geist::tokens().outline == hex(0xEAEAEA));
    CHECK(geist::tokens().outlineStrong == hex(0xD4D4D4));
    CHECK(geist::tokens().ctaDisabledBg == hex(0xF3F3F3));
    CHECK(geist::tokens().ctaDisabledFg == hex(0x999999));
    CHECK(geist::tokens().segTrack == hex(0xF5F5F5));
    CHECK(geist::tokens().segBorder == hex(0xE6E6E6));
    CHECK(geist::tokens().segThumb == hex(0xFFFFFF));
    CHECK(geist::tokens().segThumbRim == rgba(0, 0, 0, 15));
    CHECK(geist::tokens().dropzoneBg == hex(0xFFFFFF));
    CHECK(geist::tokens().dropzoneBorder == hex(0xDCDCDC));
    CHECK(geist::tokens().switchKnobOff == hex(0xFFFFFF));
    CHECK(geist::tokens().tagBg == hex(0xEBEBEB));
    CHECK(geist::tokens().regMark == hex(0xD4D4D4));
    CHECK(geist::tokens().error == hex(0xEE0000));
    CHECK(geist::tokens().errorHover == hex(0xC50000));
    CHECK(geist::tokens().dangerBg == rgba(238, 0, 0, 15));
    CHECK(geist::tokens().dangerBgHover == rgba(238, 0, 0, 31));
    CHECK(geist::tokens().dangerBgPress == rgba(238, 0, 0, 46));
    CHECK(geist::tokens().accent == hex(0x0070F3));
    CHECK(geist::tokens().veilAlpha == 0.82f);
    CHECK(geist::tokens().scrimAlpha == 0.48f);
    CHECK(geist::tokens().curtainHaloAlpha == 0.72f);
    CHECK(geist::tokens().checkerA != geist::tokens().checkerB);
    geist::mode() = geist::Mode::Dark;

    static_assert(geist::type::PAGE_TITLE == 28.0f);
    static_assert(geist::type::SUBHEADING == 23.0f);
    static_assert(geist::type::BODY == 18.0f);
    static_assert(geist::type::BODY_SMALL == 15.0f);
    static_assert(geist::type::LABEL == 16.0f);
    static_assert(geist::type::MONO == 15.0f);
    static_assert(geist::type::EYEBROW == 13.0f);
    static_assert(ctl::geom::SEG_PAD == 4.0f && ctl::geom::SEG_TRACK_R == 8.0f && ctl::geom::SEG_PILL_R == 4.0f);
    static_assert(ctl::geom::SEG_H == 42.0f && ctl::geom::SEG_H_COMPACT == 38.0f && ctl::geom::TOPBAR_H == 52.0f);
    static_assert(ctl::geom::TOGGLE_W == 32.0f && ctl::geom::TOGGLE_H == 18.0f);
    static_assert(ctl::geom::TOGGLE_KNOB == 14.0f && ctl::geom::TOGGLE_TRAVEL == 14.0f);
    static_assert(ctl::geom::SLIDER_TRACK == 4.0f && ctl::geom::SLIDER_KNOB == 14.0f);

    // 2. Wayland DPI physical-pixel snapping
    currentDpiScale() = 1.25f;
    CHECK(std::fabs(hairlinePx() - 0.8f) < 1e-4f);
    CHECK(std::fabs(snapPx(10.3f) * 1.25f - std::round(10.3f * 1.25f)) < 1e-4f);
    currentDpiScale() = 1.0f;
    CHECK(std::fabs(hairlinePx() - 1.0f) < 1e-5f);

    // 3. Magnetic 5-tick quality slider math (0/25/50/75/100% within 2.2%)
    CHECK(ctl::quantizeSliderValue(0.015f, 1, 100) == 1);
    CHECK(ctl::quantizeSliderValue(0.240f, 1, 100) == 26); // 0.25 * 99 + 1 = 25.75 -> 26
    CHECK(ctl::quantizeSliderValue(0.510f, 0, 100) == 50);
    CHECK(ctl::quantizeSliderValue(0.735f, 0, 100) == 75);
    CHECK(ctl::quantizeSliderValue(0.985f, 0, 100) == 100);
    CHECK(ctl::quantizeSliderValue(0.600f, 0, 100) == 60);

    // 4. Zero entrance motion & spring settling in deterministic headless clock
    motion::beginHeadlessMotion(1);
    motion::reduceMotion() = false;

    const ImGuiID pillId = 1001;
    const float first = motion::followSpring(pillId, 180.0f, motion::selectionParams());
    CHECK(std::fabs(first - 180.0f) < 1e-4f);
    for (int f = 0; f < 5; ++f) {
        motion::stepMotionFrame(1.0f / 60.0f);
        const float steady = motion::followSpring(pillId, 180.0f, motion::selectionParams());
        CHECK(std::fabs(steady - 180.0f) < 1e-4f);
    }

    float cur = 180.0f;
    for (int f = 0; f < 20; ++f) {
        motion::stepMotionFrame(1.0f / 60.0f);
        cur = motion::followSpring(pillId, 240.0f, motion::selectionParams(250.0f));
    }
    CHECK(std::fabs(cur - 240.0f) < 0.25f);

    // 5. reduceMotion snaps instantaneously
    motion::reduceMotion() = true;
    motion::stepMotionFrame(1.0f / 60.0f);
    CHECK(std::fabs(motion::followSpring(pillId, 60.0f, motion::selectionParams()) - 60.0f) < 1e-5f);
    CHECK(std::fabs(motion::approach(2002, 0.25f, motion::BASE, 1.0f) - 0.25f) < 1e-5f);
    motion::reduceMotion() = false;

    // 6. Bounded slot eviction removes dormant entries after >600 idle frames
    motion::resetMotionState();
    for (ImGuiID i = 1; i <= 500; ++i) {
        motion::followSpring(5000 + i, 10.0f, motion::PRESS);
        motion::approach(8000 + i, 1.0f, motion::FAST, 1.0f);
    }
    CHECK(motion::activeMotionSlots() == 1000);
    for (int f = 0; f < 750; ++f) {
        motion::stepMotionFrame(1.0f / 60.0f);
    }
    motion::followSpring(999999, 1.0f, motion::PRESS);
    motion::approach(999998, 1.0f, motion::FAST, 1.0f);
    CHECK(motion::activeMotionSlots() == 2);

    motion::endHeadlessMotion();

    // 7. Flexbox layout solver — headless, zero-alloc, DPI-aware
    auto near = [](float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; };
    flex::Context fxCtx;

    // 7a. Row & Column basic distribution + LTRB padding + gap
    {
        flex::Layout fx(fxCtx);
        auto r = fx.row({.padding = flex::Edges::only(10.0f, 8.0f, 14.0f, 12.0f), .gap = 6.0f});
        auto a = r.slot(flex::Fixed(50.0f), flex::Fixed(20.0f));
        auto b = r.slot(flex::Grow(1.0f), flex::Fixed(20.0f));
        auto c = r.slot(flex::Grow(2.0f), flex::Fixed(20.0f));
        fx.solve(flex::Rect{100.0f, 50.0f, 300.0f, 80.0f}, false);
        // Inner width = 300 - 10 - 14 = 276; gaps = 12; remaining after 50 = 214 -> 1:2 = 71.333 / 142.667
        const flex::Rect ra = fx.rect(a);
        const flex::Rect rb = fx.rect(b);
        const flex::Rect rc = fx.rect(c);
        CHECK(near(ra.x, 110.0f) && near(ra.y, 58.0f) && near(ra.w, 50.0f) && near(ra.h, 20.0f));
        CHECK(near(rb.x, 166.0f) && near(rb.w, 214.0f / 3.0f));
        CHECK(near(rc.x, rb.right() + 6.0f) && near(rc.w, 428.0f / 3.0f));
        CHECK(near(rc.right(), 100.0f + 300.0f - 14.0f));
        // ImVec4 / p0 / p1 helpers match rect
        const ImVec4 v4 = fx.vec4(a);
        CHECK(near(v4.x, ra.p0().x) && near(v4.y, ra.p0().y) && near(v4.z, ra.p1().x) && near(v4.w, ra.p1().y));
    }

    // 7b. Column distribution with Fixed + Grow weights + min/max clamping freeze loop
    {
        flex::Layout fx(fxCtx);
        auto c = fx.col({.padding = flex::Edges::all(12.0f), .gap = 8.0f});
        auto s0 = c.slot(flex::Auto(), flex::Fixed(40.0f));
        auto s1 = c.slot(flex::Auto(), flex::Grow(1.0f).max(60.0f));
        auto s2 = c.slot(flex::Auto(), flex::Grow(1.0f));
        fx.solve(flex::Rect{0.0f, 0.0f, 200.0f, 300.0f}, false);
        // Inner H = 300 - 24 = 276; gaps = 16; remaining after 40 = 220 -> s1 caps at 60, s2 gets 160
        CHECK(near(fx.rect(s0).h, 40.0f) && near(fx.rect(s0).w, 176.0f));
        CHECK(near(fx.rect(s1).h, 60.0f));
        CHECK(near(fx.rect(s2).h, 160.0f));
        CHECK(near(fx.rect(s2).bottom(), 288.0f));
    }

    // 7c. Shrink overflow redistribution (weighted + min-clamp freeze redistribution)
    {
        flex::Layout fx(fxCtx);
        auto r = fx.row({.gap = 10.0f});
        auto a = r.slot(flex::Shrink(200.0f, 1.0f).min(150.0f), flex::Fixed(30.0f));
        auto b = r.slot(flex::Shrink(200.0f, 1.0f).min(40.0f), flex::Fixed(30.0f));
        fx.solve(flex::Rect{0.0f, 0.0f, 270.0f, 30.0f}, false);
        // Inner = 270, gap = 10 -> available for items = 260 (overflow 140).
        // `a` clamps at min 150 (absorbs 50), `b` absorbs remaining 90 -> 110.
        CHECK(near(fx.rect(a).w, 150.0f));
        CHECK(near(fx.rect(b).w, 110.0f));
        CHECK(near(fx.rect(b).right(), 270.0f));

        // Unequal shrink weights with equal basis
        flex::Layout fx2(fxCtx);
        auto r2 = fx2.row({});
        auto x = r2.slot(flex::Shrink(200.0f, 2.0f), flex::Fixed(20.0f));
        auto y = r2.slot(flex::Shrink(200.0f, 1.0f), flex::Fixed(20.0f));
        fx2.solve(flex::Rect{0.0f, 0.0f, 250.0f, 20.0f}, false);
        // Overflow 150 split 2:1 -> x shrinks by 100 (to 100), y shrinks by 50 (to 150)
        CHECK(near(fx2.rect(x).w, 100.0f));
        CHECK(near(fx2.rect(y).w, 150.0f));
    }

    // 7d. Justify modes (Start, Center, End, SpaceBetween, SpaceAround, SpaceEvenly)
    {
        auto testJustify = [&](flex::Justify j, float gap, float& x0, float& x1) {
            flex::Layout fx(fxCtx);
            auto r = fx.row({.gap = gap, .justify = j});
            auto s0 = r.slot(flex::Fixed(30.0f), flex::Fixed(20.0f));
            auto s1 = r.slot(flex::Fixed(50.0f), flex::Fixed(20.0f));
            fx.solve(flex::Rect{0.0f, 0.0f, 200.0f, 40.0f}, false);
            x0 = fx.rect(s0).x;
            x1 = fx.rect(s1).x;
        };
        float x0 = 0.0f, x1 = 0.0f;
        testJustify(flex::Justify::Start, 10.0f, x0, x1);
        CHECK(near(x0, 0.0f) && near(x1, 40.0f));

        testJustify(flex::Justify::Center, 10.0f, x0, x1);
        // Used = 30+10+50 = 90 -> free = 110 -> offset = 55
        CHECK(near(x0, 55.0f) && near(x1, 95.0f));

        testJustify(flex::Justify::End, 10.0f, x0, x1);
        CHECK(near(x0, 110.0f) && near(x1, 150.0f));

        testJustify(flex::Justify::SpaceBetween, 10.0f, x0, x1);
        CHECK(near(x0, 0.0f) && near(x1, 150.0f));

        testJustify(flex::Justify::SpaceEvenly, 0.0f, x0, x1);
        // Free = 120 across 3 gaps = 40 each -> x0=40, x1=40+30+40=110
        CHECK(near(x0, 40.0f) && near(x1, 110.0f));

        // Single-item SpaceBetween stays at start
        flex::Layout fxSingle(fxCtx);
        auto rs = fxSingle.row({.justify = flex::Justify::SpaceBetween});
        auto single = rs.slot(flex::Fixed(40.0f), flex::Fixed(20.0f));
        fxSingle.solve(flex::Rect{0.0f, 0.0f, 200.0f, 40.0f}, false);
        CHECK(near(fxSingle.rect(single).x, 0.0f));
    }

    // 7e. Align modes (Start, Center, End, Stretch) & AlignSelf overrides & margins
    {
        flex::Layout fx(fxCtx);
        auto r = fx.row({.padding = flex::Edges::xy(10.0f, 10.0f), .gap = 8.0f, .align = flex::Align::Center});
        auto sDef = r.slot(flex::Fixed(20.0f), flex::Fixed(20.0f));
        auto sStart = r.slot(flex::Fixed(20.0f), flex::Fixed(20.0f), flex::AlignSelf::Start);
        auto sEnd = r.slot(flex::Fixed(20.0f), flex::Fixed(20.0f), flex::AlignSelf::End);
        auto sStretch = r.slot(flex::Fixed(20.0f), flex::Auto(), flex::AlignSelf::Stretch);
        auto sMargin = r.slot(flex::Fixed(20.0f), flex::Fixed(20.0f), flex::AlignSelf::Start,
                              flex::Edges::only(4.0f, 6.0f, 0.0f, 0.0f));
        fx.solve(flex::Rect{0.0f, 0.0f, 300.0f, 80.0f}, false);
        // Inner Y = [10, 70], innerH = 60
        CHECK(near(fx.rect(sDef).y, 30.0f) && near(fx.rect(sDef).h, 20.0f));
        CHECK(near(fx.rect(sStart).y, 10.0f));
        CHECK(near(fx.rect(sEnd).y, 50.0f));
        CHECK(near(fx.rect(sStretch).y, 10.0f) && near(fx.rect(sStretch).h, 60.0f));
        CHECK(near(fx.rect(sMargin).x, fx.rect(sStretch).right() + 8.0f + 4.0f));
        CHECK(near(fx.rect(sMargin).y, 10.0f + 6.0f));
    }

    // 7f. Nested containers, auto-sized card pre-solve height, and hidden slot (zero phantom gap)
    {
        flex::Layout fx(fxCtx);
        auto page = fx.col({.padding = flex::Edges::all(16.0f), .gap = 12.0f});
        auto card = page.col({.padding = flex::Edges::all(12.0f), .gap = 8.0f});

        auto row1 = card.row({.gap = 8.0f, .align = flex::Align::Center});
        auto r1Icon = row1.slot(flex::Fixed(22.0f), flex::Fixed(22.0f));
        auto r1Title = row1.slot(flex::Grow(1.0f), flex::Fixed(18.0f));
        auto r1Badge = row1.slot(flex::Fixed(40.0f), flex::Fixed(20.0f));

        auto hiddenDivider = card.slot(flex::Auto(), flex::Fixed(1.0f), flex::AlignSelf::Auto, {}, false);
        auto visibleDivider = card.slot(flex::Auto(), flex::Fixed(1.0f));

        auto row2 = card.row({.gap = 6.0f});
        auto r2Left = row2.slot(flex::Grow(1.0f), flex::Fixed(24.0f));
        auto r2Right = row2.slot(flex::Fixed(64.0f), flex::Fixed(24.0f));

        const flex::Rect pageRect = fx.solveWidth(ImVec2(0.0f, 0.0f), 400.0f, false);
        // Page inner W = 368 -> Card inner W = 344.
        // Card H = 12 + 22 (row1) + 8 + 1 (visibleDivider, hidden skipped!) + 8 + 24 (row2) + 12 = 87.
        // Page H = 16 + 87 + 16 = 119.
        CHECK(near(card.rect().w, 368.0f));
        CHECK(near(card.rect().h, 87.0f));
        CHECK(near(pageRect.h, 119.0f));
        CHECK(!fx.isVisible(hiddenDivider) && fx.rect(hiddenDivider).empty());
        CHECK(near(fx.rect(visibleDivider).y, row1.rect().bottom() + 8.0f));
        CHECK(near(fx.rect(r1Icon).w, 22.0f));
        CHECK(near(fx.rect(r1Title).w, 344.0f - 22.0f - 40.0f - 16.0f));
        CHECK(near(fx.rect(r1Badge).right(), card.rect().right() - 12.0f));
        CHECK(near(fx.rect(r2Left).w, 344.0f - 64.0f - 6.0f));
        CHECK(near(fx.rect(r2Right).right(), card.rect().right() - 12.0f));
    }

    // 7g. Horizontal Wrap + measured() callback (wrapped text height)
    {
        flex::Layout fx(fxCtx);
        auto col = fx.col({.padding = flex::Edges::all(10.0f), .gap = 10.0f});
        auto chips = col.row({.gap = 8.0f, .rowGap = 6.0f, .wrap = flex::Wrap::Wrap});
        flex::Slot chip[4];
        for (int i = 0; i < 4; ++i) {
            chip[i] = chips.slot(flex::Fixed(80.0f), flex::Fixed(24.0f));
        }
        // Measured text node wraps to 2 lines when width < 250, 1 line when >= 250.
        const float lineH = 16.0f;
        auto textSlot = col.measured(
            [lineH](float availW, float /*availH*/) -> flex::IntrinsicSize {
                const int lines = (availW < 250.0f) ? 2 : 1;
                return {std::min(availW, 250.0f), lineH * static_cast<float>(lines)};
            },
            flex::Grow(1.0f), flex::Auto());

        const flex::Rect r = fx.solveWidth(ImVec2(0.0f, 0.0f), 220.0f, false);
        // Inner W = 200 -> 2 chips per line (80+8+80=168 <= 200, 3rd chip 256 > 200 wraps!)
        // Chips H = 24 + 6 + 24 = 54. Text availW = 200 < 250 -> 2 lines -> 32.
        // Total H = 10 + 54 + 10 + 32 + 10 = 116.
        CHECK(near(fx.rect(chip[0]).x, 10.0f) && near(fx.rect(chip[0]).y, 10.0f));
        CHECK(near(fx.rect(chip[1]).x, 98.0f) && near(fx.rect(chip[1]).y, 10.0f));
        CHECK(near(fx.rect(chip[2]).x, 10.0f) && near(fx.rect(chip[2]).y, 40.0f));
        CHECK(near(fx.rect(chip[3]).x, 98.0f) && near(fx.rect(chip[3]).y, 40.0f));
        CHECK(near(fx.rect(textSlot).h, 32.0f));
        CHECK(near(r.h, 116.0f));
    }

    // 7h. Zero-allocation steady-state frame reuse
    {
        flex::Context reuseCtx;
        auto buildAndSolve = [&]() {
            flex::Layout fx(reuseCtx);
            auto root = fx.col({.padding = flex::Edges::all(16.0f), .gap = 10.0f});
            auto top = root.row({.gap = 8.0f, .wrap = flex::Wrap::Wrap});
            for (int i = 0; i < 6; ++i) top.slot(flex::Fixed(70.0f), flex::Fixed(24.0f));
            auto mid = root.row({.gap = 12.0f});
            mid.slot(flex::Grow(1.0f), flex::Fixed(40.0f));
            mid.slot(flex::Fixed(100.0f), flex::Fixed(40.0f));
            fx.solve(flex::Rect{0.0f, 0.0f, 360.0f, 200.0f}, true);
        };
        buildAndSolve(); // warm-up frame 1
        const size_t capNodes = reuseCtx.nodeCapacity();
        const size_t capItems = reuseCtx.scratchItemCapacity();
        const size_t capLines = reuseCtx.scratchLineCapacity();
        for (int frame = 0; frame < 500; ++frame) {
            buildAndSolve();
            CHECK(reuseCtx.nodeCapacity() == capNodes);
            CHECK(reuseCtx.scratchItemCapacity() == capItems);
            CHECK(reuseCtx.scratchLineCapacity() == capLines);
        }
    }

    // 7i. DPI snapping at 1.25x & 1.5x (exact physical grid alignment, seamless adjacent edges)
    {
        currentDpiScale() = 1.25f;
        flex::Layout fx(fxCtx);
        auto r = fx.row({.gap = 0.0f});
        auto c0 = r.slot(flex::Grow(1.0f), flex::Fixed(20.0f));
        auto c1 = r.slot(flex::Grow(1.0f), flex::Fixed(20.0f));
        auto c2 = r.slot(flex::Grow(1.0f), flex::Fixed(20.0f));
        fx.solve(flex::Rect{0.0f, 0.0f, 100.0f, 20.0f}, true);
        const flex::Rect r0 = fx.rect(c0);
        const flex::Rect r1 = fx.rect(c1);
        const flex::Rect r2 = fx.rect(c2);
        auto onGrid125 = [&](float v) { return near(v * 1.25f, std::round(v * 1.25f), 1e-4f); };
        CHECK(onGrid125(r0.x) && onGrid125(r0.right()));
        CHECK(onGrid125(r1.x) && onGrid125(r1.right()));
        CHECK(onGrid125(r2.x) && onGrid125(r2.right()));
        CHECK(near(r0.right(), r1.x, 1e-5f));
        CHECK(near(r1.right(), r2.x, 1e-5f));
        CHECK(near(r2.right(), 100.0f, 1e-5f));
        currentDpiScale() = 1.0f;
    }

    // 8. UTF-8 non-ASCII detection for eyebrowHeader CJK vs ASCII typography
    CHECK(hasNonAsciiUtf8("模型"));
    CHECK(hasNonAsciiUtf8("推理引擎"));
    CHECK(hasNonAsciiUtf8("Upix 模型"));
    CHECK(ctl::hasNonAsciiUtf8("导出设置"));
    CHECK(!hasNonAsciiUtf8("REAL-ESRGAN V3"));
    CHECK(!hasNonAsciiUtf8("Settings 123"));
    CHECK(!hasNonAsciiUtf8(""));
    CHECK(!hasNonAsciiUtf8(nullptr));
    static_assert(static_cast<int>(ctl::Finish::Capsule) >= 0);

    std::printf("test_gui_math_and_motion: PASS\n");
    return 0;
}
