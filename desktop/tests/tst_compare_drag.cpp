// Regression test for the CompareStage divider.
//
// Two defects this pins down, both found by driving the real component with
// real mouse events (the headless `--sweep` shots could not see either):
//
//  1. The grab area rode on the divider. A MouseArea that moves with `pos`
//     reports `mouse.x` relative to itself, so every press divided a ~0..28px
//     offset by the stage width and collapsed `pos` to the clamp floor (0.02).
//     The divider then ignored the cursor entirely for the rest of the drag.
//  2. The clip seam (`width * pos`) and the 1px rule (`width * pos - 0.5`) were
//     rounded independently, so at fractional positions they landed on adjacent
//     columns and the divider read as two parallel lines mid-drag.
//
// The component is loaded from the same sources the app ships (not a copy), so
// the assertions fail if either defect returns.
#include "theme.h"

#include <QGuiApplication>
#include <QImageReader>
#include <QMouseEvent>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTest>

namespace {

constexpr double kRailWidth = 76.0;   // shell rail; the stage starts after it

class TstCompareDrag : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void pressOnRuleKeepsPos();
    void dragTracksCursorRight();
    void dragTracksCursorLeft();
    void posSurvivesRelease();
    void pressAwayFromRuleIsInert();
    void seamAndRuleShareOneColumn();
    void clampsAtEdges();
    void cleanupTestCase();

private:
    QQuickItem* stage() const { return m_stage; }
    // Divider scene x for the current `pos`, as the app's shell lays it out.
    double ruleSceneX() const {
        return kRailWidth + std::round(m_stage->width() * m_stage->property("pos").toDouble());
    }
    double pos() const { return m_stage->property("pos").toDouble(); }
    void send(QEvent::Type type, double sceneX, double sceneY, Qt::MouseButton b) {
        const QPointF scene(sceneX, sceneY);
        QMouseEvent e(type, m_window->contentItem()->mapFromScene(scene), scene,
                      b, b, Qt::NoModifier);
        QGuiApplication::sendEvent(m_window, &e);
    }
    void dragTo(double sceneX) {
        send(QEvent::MouseMove, sceneX, m_window->height() / 2.0, Qt::LeftButton);
    }

    QQmlApplicationEngine m_engine;
    QQuickWindow* m_window = nullptr;
    QQuickItem* m_stage = nullptr;
};

void TstCompareDrag::initTestCase() {
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    // The compare stage decodes the fixture SVGs at full size.
    QImageReader::setAllocationLimit(0);
    m_engine.rootContext()->setContextProperty(QStringLiteral("Theme"), new Theme(&m_engine));
    m_engine.rootContext()->setContextProperty(QStringLiteral("FIXTURE_DIR"),
                                               QStringLiteral(FIXTURE_DIR));
    m_engine.load(QUrl(QStringLiteral("qrc:/tests/tst_stage.qml")));
    QVERIFY2(!m_engine.rootObjects().isEmpty(), "tst_stage.qml failed to load");
    m_window = qobject_cast<QQuickWindow*>(m_engine.rootObjects().first());
    QVERIFY(m_window);
    m_window->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_window));
    m_stage = m_window->findChild<QQuickItem*>(QStringLiteral("compareStage"));
    QVERIFY2(m_stage, "compareStage not found");
}

void TstCompareDrag::cleanupTestCase() { m_window->close(); }

// Pressing exactly on the rule must not move it. Before the fix this snapped
// `pos` to 0.02.
void TstCompareDrag::pressOnRuleKeepsPos() {
    m_stage->setProperty("pos", 0.62);
    QTest::qWait(50);
    const double before = pos();
    send(QEvent::MouseButtonPress, ruleSceneX(), m_window->height() / 2.0, Qt::LeftButton);
    QVERIFY2(qAbs(pos() - before) < 0.01,
             qPrintable(QStringLiteral("press moved pos: %1 -> %2").arg(before).arg(pos())));
    send(QEvent::MouseButtonRelease, ruleSceneX(), m_window->height() / 2.0, Qt::LeftButton);
}

// Mid-drag, `pos` must equal the cursor position. Before the fix it stayed at
// the clamp floor while the cursor travelled.
void TstCompareDrag::dragTracksCursorRight() {
    m_stage->setProperty("pos", 0.3);
    QTest::qWait(50);
    send(QEvent::MouseButtonPress, ruleSceneX(), m_window->height() / 2.0, Qt::LeftButton);
    for (int i = 1; i <= 6; ++i) {
        const double target = ruleSceneX() + i * 30.0;
        dragTo(target);
        const double expect = (target - kRailWidth) / m_stage->width();
        QVERIFY2(qAbs(pos() - expect) < 0.005,
                 qPrintable(QStringLiteral("step %1: pos=%2 expected=%3")
                                .arg(i).arg(pos()).arg(expect)));
    }
    send(QEvent::MouseButtonRelease, m_window->width() - 10.0, m_window->height() / 2.0,
         Qt::LeftButton);
}

// Dragging back past the grab point must also track (no hysteresis / snapping).
void TstCompareDrag::dragTracksCursorLeft() {
    m_stage->setProperty("pos", 0.7);
    QTest::qWait(50);
    const double grab = ruleSceneX();
    send(QEvent::MouseButtonPress, grab, m_window->height() / 2.0, Qt::LeftButton);
    for (int i = 1; i <= 5; ++i) {
        const double target = grab - i * 90.0;
        dragTo(target);
        const double expect = (target - kRailWidth) / m_stage->width();
        QVERIFY2(qAbs(pos() - expect) < 0.005,
                 qPrintable(QStringLiteral("step %1: pos=%2 expected=%3")
                                .arg(i).arg(pos()).arg(expect)));
    }
    send(QEvent::MouseButtonRelease, kRailWidth + 20.0, m_window->height() / 2.0, Qt::LeftButton);
}

void TstCompareDrag::posSurvivesRelease() {
    m_stage->setProperty("pos", 0.5);
    QTest::qWait(50);
    send(QEvent::MouseButtonPress, ruleSceneX(), m_window->height() / 2.0, Qt::LeftButton);
    dragTo(ruleSceneX() + 120.0);
    const double held = pos();
    send(QEvent::MouseButtonRelease, ruleSceneX() + 120.0, m_window->height() / 2.0,
         Qt::LeftButton);
    QTest::qWait(50);
    QCOMPARE(pos(), held);
}

// The grab area spans the stage; a press away from the rule must not drag it.
void TstCompareDrag::pressAwayFromRuleIsInert() {
    m_stage->setProperty("pos", 0.5);
    QTest::qWait(50);
    const double before = pos();
    const double y = m_window->height() / 2.0;
    const double far = kRailWidth + m_stage->width() - 60.0;
    send(QEvent::MouseButtonPress, far, y, Qt::LeftButton);
    send(QEvent::MouseMove, far + 40.0, y, Qt::LeftButton);
    send(QEvent::MouseButtonRelease, far + 40.0, y, Qt::LeftButton);
    QCOMPARE(pos(), before);
}

// The clip seam, the rule and the handle must all sit on one rounded column,
// otherwise the divider paints as two lines at fractional positions.
void TstCompareDrag::seamAndRuleShareOneColumn() {
    for (double p : {0.3000, 0.3050, 0.3100, 0.6200, 0.6237, 0.7777}) {
        m_stage->setProperty("pos", p);
        QTest::qWait(30);
        const int px = std::round(m_stage->width() * p);
        QCOMPARE(m_stage->property("dividerPx").toInt(), px);

        QQuickItem* clip = m_stage->findChild<QQuickItem*>(QStringLiteral("afterClip"));
        QQuickItem* rule = m_stage->findChild<QQuickItem*>(QStringLiteral("dividerRule"));
        QVERIFY2(clip, "afterClip not found");
        QVERIFY2(rule, "dividerRule not found");
        QCOMPARE(int(clip->x()), px);
        QCOMPARE(int(rule->x()), px);
        QCOMPARE(int(rule->width()), 1);
    }
}

// Clamps: dragging to the stage edges pins pos at 0.02 / 0.98. Targets stay
// inside the stage because synthetic moves are not delivered once the point
// leaves the item (a real drag keeps its grab).
void TstCompareDrag::clampsAtEdges() {
    m_stage->setProperty("pos", 0.5);
    QTest::qWait(50);
    const double y = m_window->height() / 2.0;
    send(QEvent::MouseButtonPress, ruleSceneX(), y, Qt::LeftButton);
    dragTo(kRailWidth + 1.0);
    QCOMPARE(pos(), 0.02);
    dragTo(kRailWidth + m_stage->width() - 1.0);
    QCOMPARE(pos(), 0.98);
    send(QEvent::MouseButtonRelease, kRailWidth + m_stage->width() - 1.0, y, Qt::LeftButton);
}

} // namespace

QTEST_MAIN(TstCompareDrag)
#include "tst_compare_drag.moc"
