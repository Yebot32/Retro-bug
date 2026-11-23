/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * C02 `rb::exec` — the code-map trace: which guest addresses were reached as block starts at run time.
 *
 * AOT translation is offline, so its input is a code map made from facts in the file (exports, exidx,
 * relocations, veneers — AOT task 2). Those sources cannot see a start that only a computed branch
 * names: an indirect `blx` target, a jump-table case, a function pointer. This recorder is the runtime
 * half that fills the gap. A diagnostic run opens a file, every executor reports the PC of each block
 * it starts, and `rb_aotgen --trace` merges the result into the map.
 *
 * **Off unless asked for.** `OnBlockEntry` is on the block-entry path of both tiers (Tier J's
 * `PreCodeReadHook`, Tier I's translate miss), so the disabled cost is one relaxed load of `enabled`
 * and a branch. Everything else — the `unordered_set` dedupe, the mutex, the buffered file — is inside
 * that branch and only exists on a tracing run. A disabled run writes no file, takes no lock, and
 * produces the same guest behaviour as one built without this module.
 *
 * **The file.** A 16-byte header, then fixed 10-byte records, little-endian:
 *
 *     offset  size  field
 *     0       8     magic "RBCMTRC\0"
 *     8       4     version (1)
 *     12      4     record size (10)
 *     ...     ...   records: u64 pc, u8 thumb (0/1), u8 source
 *
 * `pc` is the guest address the executor was about to translate (the ARM PC, with the Thumb bit in the
 * separate `thumb` byte, as `LocationDescriptor::PC()` gives it). `source` says how the address was
 * observed; `kSourceBlockEntry` is currently the only producer. The recorder dedupes on `(pc, thumb)`
 * and appends each start once. Callers turn PCs into `.text`-relative offsets with the image's load
 * bias; the file itself is module-agnostic, which is what lets one run cover a process with several
 * libraries.
 *
 * **Lifetime.** `Start` opens and truncates the file, `Stop` flushes and closes it, and `Start` is
 * called from the boot before any guest code runs (so Tier I's block cache cannot hide a start behind
 * a hit from before tracing began). `Stop` is idempotent and also runs at process exit. A path is
 * normally configured with `RB_CODEMAP`; `StartFromEnvironment` also reads Android's
 * `debug.retrobug.codemap` system property when the variable is unset, because an installed app
 * process does not inherit a shell environment.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace rb::exec::trace {

/// The on-disk header and record sizes. The tool duplicates these three numbers rather than including
/// this header: `tools/aotgen` is deliberately independent of `core/` (AOT task 2's design).
inline constexpr char kFileMagic[8] = {'R', 'B', 'C', 'M', 'T', 'R', 'C', '\0'};
inline constexpr std::uint32_t kFileVersion = 1;
inline constexpr std::uint32_t kFileHeaderBytes = 16;
inline constexpr std::uint32_t kFileRecordBytes = 10;

/// How a record's address was observed. The field is in the file format so a later producer can add a
/// kind without a version bump; the value is advisory to the tool, which only needs `pc` and `thumb`.
enum : std::uint8_t {
  /// An executor began a block at this address. Both tiers report this, so the same set arrives
  /// whether the run was Tier I or Tier J.
  kSourceBlockEntry = 1,
};

/**
 * Open `path` for a trace and write the header. An existing file is truncated: a trace is one run's
 * observation, and appending a second header to a previous run's records would make the file
 * unparseable. Records themselves are only ever appended, in the order they are first seen.
 *
 * `err` is cleared on success and set to a reason (with `strerror` where a syscall failed) otherwise.
 * Calling `Start` while a trace is open closes it first.
 */
bool Start(const std::string& path, std::string* err);

/**
 * `Start` from `RB_CODEMAP`, or from the Android `debug.retrobug.codemap` property when the variable
 * is unset. No path configured is not an error: it returns false with `err` empty, which is the normal
 * launch. A configured path that cannot be opened is an error (the caller reports it and continues —
 * a diagnostic must not stop the app).
 */
bool StartFromEnvironment(std::string* err);

/// Flush and close the trace. Idempotent; safe to call when nothing was started.
void Stop();

[[nodiscard]] bool enabled() noexcept;

/// Records written so far, i.e. distinct block starts, not calls. Zero when disabled.
[[nodiscard]] std::uint64_t record_count();

/// The open file's path, or empty when no trace is open. For the boot's one log line.
[[nodiscard]] std::string path();

namespace detail {

/// The hot-path flag. External linkage only so `OnBlockEntry` can inline the disabled test; nothing
/// outside this module should touch it.
extern std::atomic<bool> enabled;

/// The slow half: dedupe, serialise and append. Called only after `enabled` was read as true.
void RecordBlockEntry(std::uint32_t pc, bool thumb);

}  // namespace detail

/**
 * Report a block start.
 *
 * The whole disabled path is the relaxed load below and a branch — no call, no lock, no allocation.
 * A diagnostic run pays for the mutex and the `unordered_set` in `RecordBlockEntry`. `pc` is the guest
 * address about to be translated (even; the Thumb bit is `thumb`), which is exactly the unit
 * `CodeMapEntry::off` is formed from once a load bias is subtracted.
 */
inline void OnBlockEntry(std::uint32_t pc, bool thumb) {
  if (!detail::enabled.load(std::memory_order_relaxed)) return;
  detail::RecordBlockEntry(pc, thumb);
}

}  // namespace rb::exec::trace
