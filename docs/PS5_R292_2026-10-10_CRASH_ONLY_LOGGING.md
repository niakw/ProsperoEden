# R292 — Crash-only logging when Detailed Logging is OFF (10 October 2026)

## Contract

**Detailed Logging disabled (default):**

- Do not create or append `stderr.log`, `heap.log` or `eden_log.txt`.
- Do not retain normal `boot-trace*.txt` from a successful startup.
- Normal stdout/stderr remain connected to asynchronous pipe readers that discard
  messages without disk writes. This avoids filling the kernel pipe and blocking
  the CPU/GPU threads on slow console storage.
- Keep the independent crash reporter installed: a fatal signal or terminate
  writes `logs/crash-YYYYMMDD-HHMMSS.txt`, with the exception reason,
  register/stack addresses and last game context.
- On the next launch, `Crash::TakeLast` processes and retains the latest five
  reports. It may attach older logs if the *crashed* session had opted in;
  quiet crashes keep only the crash report itself.
- Delete stale ordinary/boot logs from earlier clean sessions, but never
  delete `crash-*` or save files, cached shaders, covers, mods or game settings.
- If the user turns the preference off in the launcher, pipe output switches
  immediately to a non-persistent sink. Switching it on reopens bounded files.

**Detailed Logging enabled:** restore the existing bounded log writer, the
upstream Eden emulation file backend, and boot traces on the next app startup.
Ordinary log caps are unchanged; disable does not require an app restart.

## Why a new pinned backport?

`headless/backports/eden-ps5-bounded-logging.patch` is cached by SHA256.
Changing it in place would hit the old cache-receipt migration, which only
supports one historic flush-policy change; it could mark a build as up to date
without applying our new quiet-code path. Instead this pass uses a separate
`eden-ps5-crash-only-logging.patch` and
`.encore-backport-ps5-crash-only-logging.sha256`, applied **after** the original
bounded patch by `tools/apply-eden-backports.sh`.

Its upstream `Common::Log::FmtLogMessageImpl` now exits before message
formatting when disabled. `FileBackend` does not even create the file in
quiet mode; after the toggle is reenabled, the generation marker reopens a
fresh file instead of writing an old unlinked inode.

## Important limits

- PS5 operating-system/kernel telemetry, system crash dumps and third-party
  runtime logging are **not** owned by Eden Encore and cannot be suppressed
  by this setting.
- Normal `Eden::Report` user-visible errors still display in the launcher.
  A **caught** exception is not automatically a fatal signal/crash report.
- Before persistent config is readable the bootstrap may briefly write an
  emergency boot trace. On a successful quiet startup, Encore closes and
  removes it. An early fatal event must still be diagnosable.
- This prevents persistent normal logs. It does not prove elimination of
  the rendering stutter; GPU/CPU contention and Vulkan presentation require
  a separate hardware A/B.

## Validation

The host core-preflight runs `tools/check-ps5-crash-only-logging.py`,
which exercises quiet/start/write, enable/write, disable/write, re-enable/write
and crash-handler source independence, as well as the existing bounded-pipe
unit test. The native builder must additionally apply the **new** pinned
backport successfully to the real Eden source. Only a PS5 PKG tested on
firmware 13.60 can confirm native filesystem/device behavior.
