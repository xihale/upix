#include "ui/controls.hpp"
#include "ui/geist.hpp"
#include "ui/motion.hpp"

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
    CHECK(geist::tokens().accent == hex(0x3291FF));
    CHECK(geist::tokens().regMark == hex(0x343440));
    geist::mode() = geist::Mode::Light;
    CHECK(geist::tokens().bg == hex(0xFAFAFA));
    CHECK(geist::tokens().accent == hex(0x0070F3));
    CHECK(geist::tokens().checkerA != geist::tokens().checkerB);
    geist::mode() = geist::Mode::Dark;

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
    std::printf("test_gui_math_and_motion: PASS\n");
    return 0;
}
