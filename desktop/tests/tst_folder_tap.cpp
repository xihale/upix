// Regression test for the empty-stage tap routing.
//
// Defect this pins down: the drop zone's TapHandler and the 选择文件夹批量处理
// button's own TapHandler are nested (TapHandler does not mask ancestor
// handlers), so one click on the button fired BOTH picker paths — the file
// dialog AND the folder dialog each opened. The zone handler now bails out
// when the release point lands inside the button.
//
// The component is loaded from the same sources the app ships (not a copy),
// so the assertions fail if the double-open returns.
#include "theme.h"

#include <QGuiApplication>
#include <functional>
#include <QMouseEvent>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTest>
#include <QVariantMap>

namespace {

// Minimal Desk stand-in: Workbench only reads `snap` and invokes actions;
// the assertions look at the dialogs, not at Desk effects.
class StubDesk : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap snap READ snap CONSTANT)
public:
    explicit StubDesk(QObject* parent = nullptr) : QObject(parent) {}
    QVariantMap snap() const { return m_snap; }
    Q_INVOKABLE void addPaths(const QStringList&) {}
    Q_INVOKABLE void clearInputs() {}
    Q_INVOKABLE void removeInput(const QString&) {}
    Q_INVOKABLE void openPath(const QString&) {}
    Q_INVOKABLE void reveal(const QString&) {}
    Q_INVOKABLE void download(const QString&) {}
    Q_INVOKABLE void refreshModels() {}
    Q_INVOKABLE void importModels(const QStringList&) {}

private:
    QVariantMap m_snap = QVariantMap{
        {"cliFound", true},
        {"inputs", QVariantList{}},
        {"cover", QString()},
        {"folderCount", 0},
        {"downloading", QString()},
        {"downloadPct", 0},
        {"installedGroups", QVariantList{
             QVariantMap{{"label", QStringLiteral("AnimeSharp")},
                         {"items", QVariantList{
                             QVariantMap{{"m", QVariantMap{{"name", QStringLiteral("AnimeSharp-x2")}, {"scale", 2}, {"size", QStringLiteral("2 MB")}, {"scene", QStringLiteral("动漫")}, {"recommended", false}}}, {"short", QStringLiteral("x2")}, {"remote", false}},
                             QVariantMap{{"m", QVariantMap{{"name", QStringLiteral("AnimeSharp-x4")}, {"scale", 4}, {"size", QStringLiteral("31 MB")}, {"scene", QStringLiteral("动漫")}, {"recommended", true}}}, {"short", QStringLiteral("x4")}, {"remote", false}},
                         }}}},
        },
        {"remoteGroups", QVariantList{
             QVariantMap{{"label", QString()},
                         {"items", QVariantList{
                             QVariantMap{{"m", QVariantMap{{"name", QStringLiteral("NomosUni-SPAN-x2")}, {"scale", 2}, {"size", QStringLiteral("9 MB")}, {"scene", QStringLiteral("照片")}, {"recommended", false}}}, {"short", QString()}, {"remote", true}},
                         }}}},
        },
        {"remoteError", QString()},
        {"modelsDir", QStringLiteral("/home/u/.local/share/upix/models")},
        {"job", QVariantMap{{"phase", QStringLiteral("idle")},
                            {"percent", 0},
                            {"index", 1},
                            {"total", 1},
                            {"outputs", QVariantList{}},
                            {"message", QString()},
                            {"current", QString()},
                            {"tilesDone", 0},
                            {"tilesTotal", 0},
                            {"etaSec", 0}}},
    };
};

class TstFolderTap : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void clickOnFolderButtonOpensFolderDialogOnly();
    void clickOnZoneOpensFileDialogOnly();
    void modelsPageSearchAndFilter();
    void cleanupTestCase();

private:
    QQuickItem* findItem(QQuickItem* root, const QString& objectName) const;
    QObject* findDialog(QQuickItem* root, bool wantFolder) const;
    void sendClick(QQuickWindow* win, double x, double y) const {
        const QPointF pos(x, y);
        QMouseEvent mv(QEvent::MouseMove, pos, pos, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QGuiApplication::sendEvent(win, &mv);
        QTest::qWait(30);
        QMouseEvent press(QEvent::MouseButtonPress, pos, pos,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QGuiApplication::sendEvent(win, &press);
        QTest::qWait(30);
        QMouseEvent release(QEvent::MouseButtonRelease, pos, pos,
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QGuiApplication::sendEvent(win, &release);
        QTest::qWait(120);
    }

    QQmlApplicationEngine m_engine;
    QQuickWindow* m_window = nullptr;
    QQuickItem* m_work = nullptr;
    QObject* m_fileDialog = nullptr;
    QObject* m_folderDialog = nullptr;
    StubDesk m_desk;
};

QQuickItem* TstFolderTap::findItem(QQuickItem* root, const QString& objectName) const {
    if (!root) return nullptr;
    if (root->objectName() == objectName) return root;
    for (QQuickItem* c : root->childItems())
        if (auto* hit = findItem(c, objectName)) return hit;
    return nullptr;
}

// The QML dialogs expose no C++ header in QtQuick.Dialogs, and Qt 6.11 gives
// both the runtime class QFileDialogOptions — so identify them by the
// properties only one of them carries: fileMode (file) vs selectedFolder
// (folder).
QObject* TstFolderTap::findDialog(QQuickItem* root, bool wantFolder) const {
    if (!root) return nullptr;
    for (QObject* c : root->children()) {
        const bool isFile = c->property("fileMode").isValid();
        const bool isFolder = c->property("selectedFolder").isValid();
        const bool startsFile = QString::fromLatin1(c->metaObject()->className())
                                    .startsWith(QLatin1String("QFileDialogOptions"));
        if (startsFile && ((wantFolder && isFolder) || (!wantFolder && isFile)))
            return c;
        if (auto* hit = findDialog(qobject_cast<QQuickItem*>(c), wantFolder)) return hit;
    }
    return nullptr;
}

void TstFolderTap::initTestCase() {
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    m_engine.rootContext()->setContextProperty(QStringLiteral("Theme"), new Theme(&m_engine));
    m_engine.rootContext()->setContextProperty(QStringLiteral("Desk"), &m_desk);
    m_engine.load(QUrl(QStringLiteral("qrc:/tests/tst_folder_tap.qml")));
    QVERIFY2(!m_engine.rootObjects().isEmpty(), "tst_folder_tap.qml failed to load");
    m_window = qobject_cast<QQuickWindow*>(m_engine.rootObjects().first());
    QVERIFY(m_window);
    m_window->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_window));
    m_work = m_window->findChild<QQuickItem*>(QStringLiteral("workbench"));
    QVERIFY2(m_work, "workbench not found");
    m_fileDialog = findDialog(m_work, false);
    m_folderDialog = findDialog(m_work, true);
    QVERIFY2(m_fileDialog, "file dialog not found in Workbench");
    QVERIFY2(m_folderDialog, "folder dialog not found in Workbench");
    QVERIFY(!m_fileDialog->property("visible").toBool());
    QVERIFY(!m_folderDialog->property("visible").toBool());
}

// THE regression: one click on the folder button must open exactly one
// dialog — the folder picker. Before the fix the nested zone handler also
// opened the file dialog.
void TstFolderTap::clickOnFolderButtonOpensFolderDialogOnly() {
    QQuickItem* button = findItem(m_work, QStringLiteral("folderAction"));
    QVERIFY2(button, "folderAction not found");
    const QPointF center = button->mapToItem(nullptr, QPointF(button->width() / 2.0,
                                                              button->height() / 2.0));
    sendClick(m_window, center.x(), center.y());

    QVERIFY2(m_folderDialog->property("visible").toBool(),
             "folder dialog did not open from the folder button");
    QVERIFY2(!m_fileDialog->property("visible").toBool(),
             "file dialog also opened — the zone handler fired for a click "
             "inside the folder button");
    // One click, one dialog: the folder dialog must not have been opened twice
    // (a second open() on an open dialog is a no-op, so this guards the
    // single-fire path only indirectly — the file-dialog assertion above is
    // the real double-open detector).
    QVERIFY(QMetaObject::invokeMethod(m_folderDialog, "close"));
    QTest::qWait(60);
    QVERIFY(!m_folderDialog->property("visible").toBool());
}

// A click on the bare zone (outside the button) still opens the file picker.
void TstFolderTap::clickOnZoneOpensFileDialogOnly() {
    QQuickItem* zone = findItem(m_work, QStringLiteral("emptyZone"));
    QVERIFY2(zone, "emptyZone not found");
    const QPointF center = zone->mapToItem(nullptr, QPointF(zone->width() / 2.0,
                                                            zone->height() / 3.0));
    sendClick(m_window, center.x(), center.y());

    QVERIFY2(m_fileDialog->property("visible").toBool(),
             "file dialog did not open from a zone click");
    QVERIFY2(!m_folderDialog->property("visible").toBool(),
             "folder dialog opened from a zone click");
    QVERIFY(QMetaObject::invokeMethod(m_fileDialog, "close"));
    QTest::qWait(60);
    QVERIFY(!m_fileDialog->property("visible").toBool());
}

// The models page's search query and scene filter must drive the visible
// counts — a broken binding here would show an unfiltered (or empty) page
// while the controls look alive.
void TstFolderTap::modelsPageSearchAndFilter() {
    QQuickItem* page = m_window->findChild<QQuickItem*>(QStringLiteral("modelsPage"));
    QVERIFY(page);
    QCOMPARE(page->property("instCount").toInt(), 2);
    QCOMPARE(page->property("remCount").toInt(), 1);

    page->setProperty("filter", QStringLiteral("动漫"));
    QCOMPARE(page->property("instCount").toInt(), 2);
    QCOMPARE(page->property("remCount").toInt(), 0);
    page->setProperty("filter", QStringLiteral("照片"));
    QCOMPARE(page->property("instCount").toInt(), 0);
    QCOMPARE(page->property("remCount").toInt(), 1);
    page->setProperty("filter", QString());

    page->setProperty("query", QStringLiteral("sharp"));
    QCOMPARE(page->property("instCount").toInt(), 2);   // AnimeSharp family
    page->setProperty("query", QStringLiteral("span"));
    QCOMPARE(page->property("instCount").toInt(), 0);
    QCOMPARE(page->property("remCount").toInt(), 1);
    page->setProperty("query", QString());
    QCOMPARE(page->property("instCount").toInt(), 2);
}

void TstFolderTap::cleanupTestCase() { m_window->close(); }

} // namespace

QTEST_MAIN(TstFolderTap)
#include "tst_folder_tap.moc"
