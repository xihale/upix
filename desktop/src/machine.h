// Decoder for upix-cli machine output. The CLI's `--json` modes write one
// compact JSON object per '\n'-terminated line on stdout (human logs move to
// stderr); this module turns those lines into the state Desk renders.
//
// Streamed events (JSON-lines, keyed by "t"):
//   {"t":"progress","pct":42,"done":5,"total":12,"eta":3}
//   {"t":"current","file":"a.png"}
//   {"t":"saved","path":"/out/x.png"}
//   {"t":"note","message":"2560x1680"}
//   {"t":"download","name":"Model-x4","pct":42}
//   {"t":"task","pct":57}
//   {"t":"pairing","device":"Pixel 9 Pro","fingerprint":"EC61…"}
//   {"t":"card","ips":["192.0.2.4"],"port":9527,"fingerprint":"EC61…",
//    "qr":"{\"v\":2,…}"}   ← v2 invite payload; image://qr renders it directly
// One-shot documents (no "t"): model list/list-remote/import, daemon status.
#pragma once

#include <QString>
#include <QVariantMap>

namespace machine {

// Parse one CLI output line into an event. Returns an empty map when the line
// is not a machine event (human log lines on stderr) — callers keep those as
// diagnostic text (failure detail, host log pane).
QVariantMap parseEvent(const QString& line);

// Compose the daemon tooltip label from a `daemon status --json` document.
// Same prose the CLI used to print, now assembled where it is displayed.
QString daemonLabel(const QVariantMap& status);

// Stage done-note: "完成 · <message>".
QString doneNote(const QString& message);

}  // namespace machine
