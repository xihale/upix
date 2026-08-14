# Layer 3: Protocol / Role Pruning Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the dead CLI `client` subcommand and reconcile stale documentation that still describes two already-deleted vestigial stubs (`crypto::xor_encrypt` and the C++ `MDNSDiscovery` stub), leaving the codebase honest about what exists.

**Architecture:** Single, self-contained deletion of the CLI `client process` subcommand (a one-shot raw-TCP job submitter that duplicates the Android JNI path). No protocol, coordinator, or share-layer changes — the protocol symbols (`MSG_CLIENT_SUBMIT`, `ClientTaskSubmit`, etc.) and the coordinator's `handle_client` stay live because the Android app still drives them. The other two Layer-3 targets (xor_encrypt, C++ MDNSDiscovery) were already deleted from C++ in commit `8c11549`; this plan only cleans up the docs that still describe them, and removes the now-stale `.sh` test lines that invoke `client process`.

**Tech Stack:** C++23 (CLI, xmake build), Markdown docs, bash test scripts.

---

## Pre-Plan Reality Check (read this before executing)

The original Layer-3 design (`docs/decisions/specs/2026-07-28-architecture-simplification-design.md` §4.1–4.3) named three code deletions. Independent verification against the current working tree shows the situation has changed:

| Target | Spec said | Reality (verified 2026-07-28) | Action in this plan |
|---|---|---|---|
| CLI `client` subcommand | delete (§4.1) | **Still present** (`cmd_client.cpp`, 186 lines) | **Task 1: delete it** |
| C++ `MDNSDiscovery` stub | delete (§4.2), keep `MDNSBroadcaster` | **C++ stub already gone** (commit `8c11549`); only `MDNSBroadcaster` remains in C++ (which the spec wanted to keep anyway) | **Task 2: doc cleanup only** |
| `crypto::xor_encrypt` | delete (§4.3) | **Already gone** (commit `8c11549`; 0 callers ever) | **Task 3: doc cleanup only** |

Spec §4.1 also floated *optionally* deleting the coordinator's `MSG_CLIENT_SUBMIT` handling under the assumption "Android 不走 coordinator 的 handle_client". That assumption is **false**: `apps/app/src/main/cpp/jni_bridge.cpp:638` sends `MSG_CLIENT_SUBMIT` to the coordinator, which `coordinator.cpp:240` and `:401` serve. **The coordinator handler stays.** Only the CLI client goes.

This plan therefore does one real deletion + honest doc reconciliation. If you expected three deletions, this is why there's one.

---

## File Structure

**Code deletion (Task 1):**
- Delete entirely: `cli/src/cmd_client.cpp`
- Surgical edit: `cli/xmake.lua` (drop the file from `add_files`)
- Surgical edit: `cli/src/cli_app.cpp` (drop the `client` subcommand registration block)
- Surgical edit: `cli/src/cli_app.hpp` (drop forward-decl + function decl)
- Surgical edit: `cli/src/args.hpp` (drop `args::Client` struct)

**Test-script fix (Task 1, same task):**
- Surgical edit: `scripts/quick_test.sh` (remove the `client process` invocation)
- Surgical edit: `scripts/test_distributed.sh` (remove the `client process` invocation)

**Doc reconciliation (Tasks 2 & 3):**
- Edit: `docs/PONYTAIL_AUDIT.md` — xor_encrypt entry now describes deleted code; mark done or remove
- Edit: `docs/ARCHITECTURE.md` — stale `crypto::xor_encrypt` references
- Edit: `docs/TECHNICAL_REPORT.md` — stale `crypto::xor_encrypt` references
- Edit: `README.md` — usage example for `client process`
- Edit: `docs/ARCHITECTURE.md`, `docs/OVERVIEW.md`, `docs/DATA_FLOW.md` — `client` role mentions
- Edit: spec file itself — mark §4.2/§4.3 as already-executed (so future readers aren't misled)

**Explicitly NOT touched (do not delete — verified still live):**
- `share/core/distributed/protocol.{hpp,cpp}`: `MSG_CLIENT_SUBMIT`, `MSG_CLIENT_RESPONSE`, `ClientTaskSubmit`, `ClientTaskResponse`, `MSG_AUTH_REQUEST/RESPONSE`, `AuthRequest`, `AuthResponse` — used by `coordinator.cpp`, `worker.cpp`, `jni_bridge.cpp` (4 hits).
- `share/core/distributed/coordinator.cpp`: `handle_client` and the `MSG_CLIENT_SUBMIT` branches at lines 240–345 and 401–484 — still serve the Android JNI client path.
- `share/utils/mdns.{hpp,cpp}`: `MDNSBroadcaster` — spec §4.2 explicitly wants this kept; it's the live PC-side broadcaster (`cmd_server.cpp`).
- `cli/src/common.hpp` `MAX_NETWORK_PAYLOAD` constant — **still used by `cli/src/cmd_process.cpp:128`** (the serve-daemon response guard). An early draft of this plan wrongly assumed only `cmd_client.cpp` used it; that was incorrect. It stays. (Note for a future layer: `MAX_NETWORK_PAYLOAD` is independently redefined in `worker.cpp:53` and `coordinator.cpp:19` — pre-existing dedup smell, out of Layer 3's scope.)

---

## A/B Verification Baseline

Layer 3 is CLI-only. There is **no CLI unit-test suite** (`cli/tests/` contains only an `output/` dir). The verification signal is therefore:

1. **Build success** — `cd cli && xmake` must compile cleanly with zero errors and zero new warnings after the deletion. This is the authoritative check: if any consumer of `cmd_client` / `args::Client` remains, the build breaks.
2. **`--help` snapshot** — `crossscale --help` must list `server`, `worker`, `process`, `serve`, `benchmark`, `model`, `perf`, `completion` but **not** `client`. (`process`/`serve`/`server`/`worker` are the live roles.)
3. **Grep cleanliness** — after Task 1, `grep -rn "cmd_client\|args::Client" cli/` returns nothing.

Capture the baseline before starting:

- [ ] **Step 0: Capture baseline build + help**

```bash
cd cli && xmake 2>&1 | tail -5
# Record: builds clean (exit 0).
cd .. && ./cli/build/linux/x86_64/release/crossscale-cli --help 2>&1 | grep -E "^  [a-z]" | sort
# Record the subcommand list (it currently includes 'client').
```

---

### Task 1: Delete the CLI `client` subcommand

**Files:**
- Delete: `cli/src/cmd_client.cpp` (entire file, 186 lines)
- Modify: `cli/xmake.lua:16` (remove one line from `add_files`)
- Modify: `cli/src/cli_app.cpp:159-171` (remove the client registration block)
- Modify: `cli/src/cli_app.hpp:24,34` (remove forward-decl + function decl)
- Modify: `cli/src/args.hpp:78-86` (remove `args::Client` struct)
- Modify: `scripts/quick_test.sh:18-19` (remove the `client process` invocation)
- Modify: `scripts/test_distributed.sh:26` (remove the `client process` invocation)

- [ ] **Step 1: Read the exact current content of each edit site**

Read these files at the listed line ranges so the edits match the working tree verbatim (line numbers in this plan are from the verified scan; confirm they still match before editing):
- `cli/xmake.lua` around line 16
- `cli/src/cli_app.cpp` lines 155–175
- `cli/src/cli_app.hpp` lines 20–40
- `cli/src/args.hpp` lines 75–90
- `scripts/quick_test.sh` lines 15–25
- `scripts/test_distributed.sh` lines 22–30

- [ ] **Step 2: Delete `cmd_client.cpp`**

```bash
git rm cli/src/cmd_client.cpp
```

- [ ] **Step 3: Remove the file from `cli/xmake.lua`**

In `cli/xmake.lua`, find the `add_files(...)` list that includes `"src/cmd_client.cpp",` and delete that single line. Leave every other entry untouched.

- [ ] **Step 4: Remove the client registration block from `cli/src/cli_app.cpp`**

Delete the entire contiguous block (comment line + `static args::Client client_args;` + the `add_subcommand("client", ...)` tree + the `cproc->callback(...)`). After deletion, the surrounding `server`/`worker` registration blocks must remain intact and the file must still compile.

- [ ] **Step 5: Remove the declarations from `cli/src/cli_app.hpp`**

Delete both lines (they are the only `Client`/`cmd_client` references in this file):
- the `namespace args { struct Client; }` forward declaration (line ~24)
- the `int cmd_client(const args::Client& a);` function declaration (line ~34)

- [ ] **Step 6: Remove the `args::Client` struct from `cli/src/args.hpp`**

Delete the whole struct (comment + `struct Client { ... };`, lines ~78–86). No other code references `args::Client` (verified by grep).

- [ ] **Step 7: Remove the `client process` invocations from the test scripts**

In `scripts/quick_test.sh`, remove the two lines under the `# run client` comment that invoke `"$CLI" client process ...`.

In `scripts/test_distributed.sh`, remove the single `time $CLI client process ...` line.

(Do not delete the scripts wholesale — they also test `server`/`worker`/`process` flows that still matter. Only the client line goes.)

- [ ] **Step 8: Verify grep cleanliness**

Run:
```bash
grep -rn "cmd_client\|args::Client" cli/ scripts/ 2>/dev/null; echo "exit=$?"
```
Expected: `exit=1` (no matches). If any match remains, you missed a reference — go fix it before building.

- [ ] **Step 9: Build and confirm clean compile**

```bash
cd cli && xmake 2>&1 | tail -20
```
Expected: clean build, exit 0, **no new warnings** about undefined `cmd_client` / `args::Client`. If the build fails with an undefined reference, a consumer was missed — re-grep and fix.

- [ ] **Step 10: Verify `--help` no longer lists `client`**

```bash
cd .. && ./cli/build/linux/x86_64/release/crossscale-cli --help 2>&1 | grep -E "^  [a-z]" | sort
```
Expected: the subcommand list matches the baseline **minus** `client`. The `process` / `server` / `worker` / `serve` roles are still present.

- [ ] **Step 11: Commit**

```bash
git add cli/xmake.lua cli/src/cli_app.cpp cli/src/cli_app.hpp cli/src/args.hpp scripts/quick_test.sh scripts/test_distributed.sh
git commit -m "refactor(cli): drop dead client subcommand (one-shot TCP submitter)

The 'client process' subcommand is a PC-side raw-TCP one-shot image
submitter that duplicates the Android JNI distributed path
(jni_bridge.cpp nativeProcessImageDistributed -> MSG_CLIENT_SUBMIT).
Android is the real client; the CLI subcommand is unused.

Removed:
- cli/src/cmd_client.cpp (entire file)
- client subcommand registration (cli_app.cpp)
- args::Client struct (args.hpp), cmd_client decl (cli_app.hpp)
- xmake.lua add_files entry
- client process invocations in scripts/quick_test.sh + test_distributed.sh

Protocol symbols (MSG_CLIENT_SUBMIT/RESPONSE, ClientTaskSubmit/Response)
and coordinator handle_client stay live: Android JNI still drives them.

A/B: cli builds clean (0 warnings); --help lists roles minus client;
grep cmd_client/args::Client clean."
```

---

### Task 2: Reconcile mDNS docs + README client usage (C++ stub already gone)

**Files:**
- Edit: `docs/ARCHITECTURE.md` (mDNS references that imply a C++ MDNSDiscovery stub)
- Edit: `docs/TECHNICAL_REPORT.md` (same)
- Edit: `docs/OVERVIEW.md` (mDNS mentions)
- Edit: `docs/DATA_FLOW.md` (mDNS mentions)
- Edit: `docs/decisions/specs/2026-07-28-architecture-simplification-design.md` §4.2 — mark as already-executed
- Edit: `README.md:99-102` — the "Client-side offloading" usage example invokes the now-deleted `client process` subcommand; this is **user-facing** (code-quality reviewer flagged it as the highest-impact stale doc). Remove or replace it.

**Context:** Spec §4.2 said "delete the C++ MDNSDiscovery stub, keep MDNSBroadcaster." Verification shows the C++ stub was already deleted in commit `8c11549`; only `MDNSBroadcaster` (which the spec wanted to keep) remains in C++. The Android `MDNSDiscovery.kt` (platform-native NsdManager) is the only "MDNSDiscovery" that exists, and it stays. So §4.2 is effectively a no-op for code; only docs that describe the removed stub need updating to avoid misleading future readers. Separately, the Task 1 code-quality review flagged `README.md:102` (`./crossscale-cli client process ...`) as a user-facing stale example — folded into this doc-reconciliation task.

- [ ] **Step 1: Find every stale mDNS/MDNSDiscovery doc reference + README client example**

```bash
command grep -rn "MDNSDiscovery\|mdns.*stub\|mDNS.*stub\|MDNSBroadcaster" docs/ README.md 2>/dev/null
command grep -n "client process\|Client-side offloading" README.md 2>/dev/null
```
(Use `command grep` — the shell has a `grep` function wrapper that breaks alternation patterns.) Record the hits. For the mDNS hits, determine whether each describes the **C++** stub (stale) or the **Android** class (still real) or `MDNSBroadcaster` (still real). Only C++-stub descriptions need editing.

- [ ] **Step 2: Update each stale doc reference**

For each mDNS hit from Step 1 that describes a C++ `MDNSDiscovery` stub as if it exists: rewrite to past tense / "removed", or delete the sentence if it adds no value. Preserve any accurate description of `MDNSBroadcaster` (PC, live) and `MDNSDiscovery.kt` (Android, live).

For the README "Client-side offloading" block (lines ~99-102): the `client process` command no longer exists. The honest options are: (a) delete the whole block, or (b) replace it with a note that distributed offloading is done from the Android app (the real client path) and the CLI's role is `server`/`worker`. Default to (b) — it preserves the user's mental model of what the CLI does without advertising a dead command. Read the surrounding README section first to match tone/format.

Concrete example for the spec §4.2: append a one-line note at the end of §4.2:
```
> **Status (2026-07-28):** the C++ `MDNSDiscovery` stub was already removed in commit `8c11549`. Only `MDNSBroadcaster` (kept, as intended) and the Android `MDNSDiscovery.kt` remain. This section is now doc-only; no further code action.
```

For `ARCHITECTURE.md` / `TECHNICAL_REPORT.md` / `OVERVIEW.md` / `DATA_FLOW.md`: edit in place — do not leave sentences claiming a C++ discovery stub exists.

- [ ] **Step 3: Verify no doc still claims a live C++ MDNSDiscovery stub, and README no longer advertises client**

```bash
command grep -rn "MDNSDiscovery" docs/ 2>/dev/null | command grep -iv "\.kt\|android\|kotlin"
command grep -n "client process" README.md 2>/dev/null; echo "exit=$?"
```
Expected: first command — either no matches, or only matches that explicitly say "removed" / past tense. Second command — exit=1 (no matches in README).

- [ ] **Step 4: Commit**

```bash
git add docs/ README.md
git commit -m "docs: reconcile mDNS + README (C++ MDNSDiscovery stub, client subcommand)

Two doc-only reconciliations after Layer 3 Task 1:
- Spec §4.2 targeted the C++ MDNSDiscovery stub for deletion, but it was
  already removed in commit 8c11549 (alongside xor_encrypt). Only
  MDNSBroadcaster (PC, live) and Android MDNSDiscovery.kt remain.
  Updated ARCHITECTURE/TECHNICAL_REPORT/OVERVIEW/DATA_FLOW + spec §4.2.
- README 'Client-side offloading' example invoked the now-deleted
  'client process' subcommand (removed in prior commit). Rewrote to
  reflect that distributed offloading is driven from the Android app;
  CLI roles are server/worker. No code change."
```

---

### Task 3: Reconcile xor_encrypt docs (already gone)

**Files:**
- Edit: `docs/PONYTAIL_AUDIT.md:56` (xor_encrypt audit entry — code already deleted)
- Edit: `docs/ARCHITECTURE.md:173,323` (stale `crypto::xor_encrypt` references)
- Edit: `docs/TECHNICAL_REPORT.md:182,475` (same)
- Edit: `docs/decisions/specs/2026-07-28-architecture-simplification-design.md` §4.3 — mark as already-executed

**Context:** Spec §4.3 said "delete `crypto::xor_encrypt`/`xor_decrypt` (0 callers, fake security)." Verification confirms it was already deleted in commit `8c11549`; current `share/utils/crypto.{hpp,cpp}` expose only `sha256_hex` and `generate_uuid`. So §4.3 is a no-op for code; only docs need updating.

- [ ] **Step 1: Find every stale xor_encrypt doc reference**

```bash
grep -rn "xor_encrypt\|xor_decrypt" docs/ README.md 2>/dev/null
```
Record the hits.

- [ ] **Step 2: Update each stale reference**

- `docs/PONYTAIL_AUDIT.md:56` — the line "`xor_encrypt` / `xor_decrypt` (`crypto.cpp:186-196`) — 0 调用": mark as ✅ done / removed, or delete the line if the audit is meant to track open items.
- `docs/ARCHITECTURE.md` and `docs/TECHNICAL_REPORT.md` lines mentioning "`crypto::xor_encrypt` 存在但未用于传输" / "形同虚设": rewrite to past tense ("was removed") or delete.
- Spec §4.3: append a status note:
```
> **Status (2026-07-28):** already executed in commit `8c11549`. `crypto.{hpp,cpp}` now expose only `sha256_hex` and `generate_uuid`. Doc-only reconciliation in this layer.
```

- [ ] **Step 3: Verify grep cleanliness**

```bash
grep -rn "xor_encrypt\|xor_decrypt" docs/ 2>/dev/null
```
Expected: only matches that say "removed" / past tense, or none.

- [ ] **Step 4: Commit**

```bash
git add docs/
git commit -m "docs: reconcile xor_encrypt docs — already removed in 8c11549

Spec §4.3 targeted crypto::xor_encrypt/xor_decrypt for deletion, but they
were already removed in commit 8c11549 (0 callers ever, fake security).
share/utils/crypto.{hpp,cpp} now expose only sha256_hex + generate_uuid.

Updated PONYTAIL_AUDIT/ARCHITECTURE/TECHNICAL_REPORT + spec §4.3 to
reflect reality. No code change."
```

---

## ~~Task 4: Drop unused `MAX_NETWORK_PAYLOAD`~~ — DROPPED (false premise)

An early draft of this plan included an optional Task 4 to delete `MAX_NETWORK_PAYLOAD` from `cli/src/common.hpp` on the assumption that only `cmd_client.cpp` used it. **Code review during Task 1 execution disproved this:** `cli/src/cmd_process.cpp:128` (serve-daemon response guard) also consumes it. The constant is NOT dead after Task 1 and must stay. Task 4 is removed. (Separately, the constant is independently redefined in `worker.cpp:53` and `coordinator.cpp:19` — a real dedup smell, but a pre-existing one out of Layer 3's scope.)

---

## Self-Review

**1. Spec coverage (§4.1–4.3):**
- §4.1 (delete CLI `client`): Task 1. ✅
- §4.1 optional (delete coordinator `handle_client`): **deliberately NOT done** — verified false assumption (Android uses it); documented in Pre-Plan Reality Check. ✅ (correctly declined)
- §4.2 (delete C++ MDNSDiscovery stub): already executed in `8c11549`; Task 2 reconciles docs + marks spec done. ✅
- §4.3 (delete xor_encrypt): already executed in `8c11549`; Task 3 reconciles docs + marks spec done. ✅
- §4.4 (protocol format unchanged, optional `MSG_IMAGE_TILE` audit): **out of scope for Layer 3** — not in the three original targets; defer to a future layer if desired. (Noted here for completeness, no task.)
- Task 4 (drop `MAX_NETWORK_PAYLOAD`): **DROPPED** — false premise (still used by `cmd_process.cpp:128`); see the dropped-task section above.

**2. Placeholder scan:** No "TBD"/"implement later"/"similar to Task N". Every code step shows the exact file/line and the exact action (delete this line / delete this block / delete this file). Doc steps use grep to find exact lines then describe the edit; this is intentional because doc line numbers drift, but the *action* is fully specified (rewrite to past tense or delete; append the verbatim status note).

**3. Type/name consistency:** The only types referenced are `args::Client` (Task 1), `cmd_client` (Task 1), `MAX_NETWORK_PAYLOAD` (Task 4) — each appears consistently. Protocol symbols named in "Explicitly NOT touched" are spelled exactly as in `protocol.hpp` (`MSG_CLIENT_SUBMIT`, `ClientTaskSubmit`, etc.).

**4. Verification rigor:** Task 1 has a hard gate — the build *will* break if any consumer is missed, so `xmake` is the authoritative A/B. `--help` and grep are secondary confirmations. Tasks 2/3 have grep gates. No claim of "done" without running a command.

---

## Execution Handoff

Plan complete and saved to `docs/decisions/plans/2026-07-28-layer3-protocol-role-pruning.md`. Two execution options:

**1. Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration.

**2. Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints.

Which approach?
