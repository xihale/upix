// Desk: the one controller behind the QML. Holds a single QVariantMap
// snapshot (same shape as the web-era Snapshot JSON) that QML renders as a
// pure function; every command is a Q_INVOKABLE that mutates state and runs
// the CLI asynchronously. Never blocks the GUI thread; never spawns itself.
#pragma once
#include <QProcess>
#include <QQmlEngine>
#include <QTimer>
#include <QVariantMap>

class Desk : public QObject {
    Q_OBJECT

    Q_PROPERTY(QVariantMap snap READ snap NOTIFY snapChanged FINAL)

public:
    explicit Desk(QObject* parent = nullptr);

    static Desk* instance() { return g_desk; }

    QVariantMap snap() const { return m_snap; }

    // ---- commands (QML) ----
    Q_INVOKABLE void addPaths(const QStringList& paths);
    Q_INVOKABLE void clearInputs();
    Q_INVOKABLE void removeInput(const QString& path);
    Q_INVOKABLE void start();
    Q_INVOKABLE void cancel();

    Q_INVOKABLE void setModel(const QString& v);
    Q_INVOKABLE void setFormat(const QString& v);
    Q_INVOKABLE void setQuality(int v);
    Q_INVOKABLE void setGpu(bool v);
    Q_INVOKABLE void setJobs(int v);
    Q_INVOKABLE void setAutoTile(bool v);
    Q_INVOKABLE void setTileSize(int v);
    Q_INVOKABLE void setForce(bool v);

    Q_INVOKABLE void refreshModels();
    Q_INVOKABLE void importModels(const QStringList& paths);
    Q_INVOKABLE void download(const QString& name);

    Q_INVOKABLE void daemonAction(const QString& action);

    Q_INVOKABLE void hostStart();
    Q_INVOKABLE void hostStop();
    Q_INVOKABLE void setHostPort(int port);
    Q_INVOKABLE void hostDecide(bool allow);
    Q_INVOKABLE void copyFingerprint();
    Q_INVOKABLE void reveal(const QString& path);
    Q_INVOKABLE void openPath(const QString& path);
    Q_INVOKABLE void pasteClipboard();

    // ---- C++ side ----
    // CLI discovery: UPIX_CLI -> exe dir sibling named exactly `upix-cli`
    // -> PATH (`upix-cli`, then `upix`). NEVER resolves to our own binary —
    // the old fallback returned applicationFilePath() and the app spawned
    // full GUI copies of itself, which took kwin down with it.
    QString findCli() const;

    // Test/fixture support (--shot): replace the snapshot wholesale.
    void applyFakeSnapshot(const QVariantMap& snap);

    // Fixture snapshots for --shot states (empty/picked/running/done/error/
    // models/host/pairing).
    static QVariantMap fakeSnapshot(const QString& state);

signals:
    void snapChanged();
    void requestView(const QString& view);

private:
    // snapshot mutation + 70 ms coalesced notify
    QVariantMap& s() { return m_snap; }
    void touch();                 // schedule emit
    void notifyNow();
    void setNotice(const QString& msg);

    // process helpers — all async, merged channels, line-oriented
    QProcess* spawn(const QString& key, const QStringList& args,
                    const QStringList& env = {});
    void boot();
    void loadPrefs();
    void savePrefs() const;
    void finishJob(int code);
    void refreshDaemonStatus();
    void hostReset();

    QVariantMap m_snap;
    QTimer m_coalesce;

    static inline Desk* g_desk = nullptr;

    // single slot per purpose; a new run replaces the old process
    QHash<QString, QProcess*> m_procs;
};
