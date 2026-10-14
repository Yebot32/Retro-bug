/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * Guest AArch32 ELF loading (component C05).
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rb/mem/guest_memory.h"

namespace rb::loader {

/// A guest address that holds code.
///
/// The ARM ABI carries the ISA state in bit 0 of every function address: set means Thumb, clear means
/// ARM. Branching to such an address with the bit still in it, or having thrown the bit away, decodes
/// the guest's Thumb instructions as ARM — which does not fault, it executes different semantics. So
/// the flag has to travel with the address rather than being masked out at each use.
struct CodeAddress {
  std::uint32_t addr = 0;  ///< Byte address, bit 0 cleared.
  bool thumb = false;
  explicit operator bool() const { return addr != 0; }
  bool operator==(const CodeAddress&) const = default;

  /**
   * The address as a *function pointer*: bit 0 set for Thumb, clear for ARM.
   *
   * `addr` is the form to execute at; this is the form to hand to anything that will branch to it, and
   * the distinction is not cosmetic. In ARM, bit 0 of a code pointer selects the instruction set — that
   * is what `blx` reads — and it is the same bit a symbol's `st_value` carries (`Symbol::value` keeps it
   * for exactly this reason). A bridge that passes `addr` to a call therefore loses the ISA, which is
   * how `rb::abi` came to enter every guest function in Thumb state: harmless for the Thumb-compiled JNI
   * methods it was first used on, and fatal for bionic's libm, which is mostly A32.
   */
  [[nodiscard]] std::uint32_t tagged() const { return addr | (thumb ? 1u : 0u); }
};

/// One exported or imported dynamic symbol, in guest terms.
struct Symbol {
  std::string name;
  /// st_value plus the load bias, kept exactly as the image states it: for a function that includes
  /// the bit-0 Thumb tag, which is what a data reference to the function has to point at. Use
  /// GuestModule::FindExport to get an address to execute.
  std::uint32_t value = 0;
  std::uint32_t size = 0;
  bool defined = false;      ///< False for undefined symbols the loader must resolve elsewhere.
  bool weak = false;
  bool is_function = false;
  bool thumb = false;        ///< is_function and st_value bit 0 set.
  /**
   * `STT_GNU_IFUNC`: `value` is a *resolver*, and the address a caller must use is whatever that
   * function returns.
   *
   * Not a detail. The device's own 32-bit `libc.so` exports eleven of these and they are
   * `memcpy`, `memmove`, `memset`, `strlen`, `strcmp`, `strcpy`, `stpcpy`, `strcat` and the three `_chk`
   * variants — the routines every app and every other library calls first. Resolving one means *running
   * guest code during load*, which is a capability C05 does not have and C06 will have to, so the flag is
   * here to make that blocker countable rather than discoverable: `rb_guest_libc_tests` asserts on the
   * eleven names instead of guessing at them from a document. Measured with
   * `llvm-readelf --dyn-syms` on `/system/lib/libc.so` from the SM-X710 (Android 16 / SDK 36).
   */
  bool ifunc = false;
};

/// One relocation whose target is the **result of a guest call** rather than an address in the file.
///
/// `where` is the guest address to write the answer into; `resolver` is the function to call, with its
/// Thumb tag attached, and `slot_value` is what `where` holds now — which for `R_ARM_IRELATIVE` is the
/// resolver's own biased address, and is the argument AArch32's REL form passes (there is no addend
/// field, so the slot *is* the addend).
///
/// C05 cannot call it: resolution means running guest code, which needs an executor and, for bionic's
/// arm32 resolvers, a syscall monitor to answer the `openat("/dev/cpu_variant:arm")` they probe with. So
/// the loader writes the resolver into the slot, records the fact here, and leaves the calling to the
/// linker. Measured, both halves: `memcpy` called in that state runs the resolver and it *returns* an
/// implementation pointer in four steps, and `strlen` reaches the `openat` instead — so the execution is
/// not the missing part, only the load-time call.
struct IfuncSlot {
  /**
   * The **guest address** to write the answer into — load bias included, not a `vaddr`.
   *
   * Every other address on this struct is a guest address, and the relocation code that finds this one
   * works in `vaddr`s and adds `base` on the way out, so the two conventions are one field apart from a
   * write that lands 0x2000 below its target and reports total success. That is exactly what it did first:
   * the slots held their resolvers, `FindExport` returned the right answer anyway, and only bionic's own
   * `.iplt` stub — which reads the *slot* — noticed.
   */
  std::uint32_t where = 0;
  CodeAddress resolver;
  std::uint32_t slot_value = 0;
  std::string name;  ///< the exported name for a GOT entry, empty for a symbolless `R_ARM_IRELATIVE`
  /// Filled in by `LoadOptions::resolve_ifunc`: the implementation the resolver chose, or 0 when the slot
  /// is still unresolved. Recorded rather than dropped, because "which of these did we finish" is the
  /// question a launch has to answer, and `where` alone cannot tell a resolved slot from a placeholder.
  std::uint32_t implementation = 0;
};

/// The linker-facing facts about one loaded module.
struct GuestModule {
  std::string path;
  std::string soname;
  std::uint32_t base = 0;      ///< Guest address the image was placed at.
  std::uint32_t extent = 0;    ///< Guest bytes covered, rounded to guest pages.
  CodeAddress entry;           ///< e_entry, already biased by `base`.
  CodeAddress init;            ///< DT_INIT, biased. Absent (addr 0) on NDK images, which use init_array.
  CodeAddress fini;            ///< DT_FINI, biased.
  /**
   * `DT_INIT_ARRAY`'s function pointers, biased and Thumb-tagged, in the order the image lists them.
   *
   * Recorded, **not called**: constructors run after the dependency scope exists (C06), or a module
   * whose init code calls into a library that is not loaded yet gets a null branch instead of a
   * refusal. Empty when the image has neither tag.
   *
   * It lives beside `init` rather than replacing it because the two are independent tags and real
   * images use only the array: `DT_INIT` is zero on every NDK and bionic image measured here, which is
   * why an unrecorded array read as "this module has no startup work" instead of "the loader did not
   * look".
   */
  std::vector<CodeAddress> init_array;
  /// `DT_INIT_ARRAY` slots holding 0 or -1 — old-NDK bracket markers, not functions — skipped the way
  /// bionic's `call_function` skips them and counted so a report can say so.
  std::uint32_t init_array_sentinels = 0;
  std::vector<std::string> needed;
  std::vector<Symbol> exports;
  std::vector<Symbol> imports;
  /// Names of every undefined *weak* dynamic symbol, NOTYPE included (which `imports` omits): references a
  /// device's linker binds to 0 when nothing defines them, so an unanswered one is not a missing import.
  std::vector<std::string> weak_imports;
  std::vector<std::string> unsupported_relocs;  ///< Relocation kinds refused, named for diagnostics.
  /// Relocations whose target has to come from a guest call. Empty for every library built from an NDK
  /// archive so far — `libm.a` and `libz.a` contain no IFUNC — which is why this only shows up once a
  /// *real* system library is loaded.
  std::vector<IfuncSlot> ifunc_slots;
  /// The same refusals as raw `r_type` values, in order. Separate from the strings above because a test
  /// that asserts by *parsing a diagnostic message* is coupled to its formatting: change the printf and
  /// the assertion starts passing for the wrong reason. `rb_guest_libc_tests` counts these.
  std::vector<std::uint32_t> unsupported_reloc_types;

  /// PT_NOTE build-id, if the image carries one. The NDK-built fixture used in tests does **not**,
  /// so nothing here may assume it is present. App Revival's patch identity (docs/10 §12.2) needs a
  /// content hash as the fallback key; that lands with tier-3 patching, which is deliberately held.
  std::vector<std::uint8_t> build_id;

  /// `PT_ARM_EXIDX`, biased: the guest address of the `.ARM.exidx` table and its entry count (8 bytes
  /// each). 0/0 when the image has none or it does not fit the image. What `dl_unwind_find_exidx` answers
  /// for a pc inside [base, base + extent).
  std::uint32_t exidx = 0;
  std::uint32_t exidx_count = 0;

  /// Set once this module's `DT_INIT`/`DT_INIT_ARRAY` have run, and never cleared. bionic guards
  /// constructors per-soinfo the same way: a shared dependency loaded by two dependents (or a retry
  /// after a mid-way failure) must not run its allocator init, static constructors and refcounted
  /// globals a second time in one address space.
  bool constructors_called = false;

  /// An address to branch to, with the Thumb tag split out of it. nullopt when the name is not
  /// defined by this module.
  std::optional<CodeAddress> FindExport(const std::string& name) const;
  std::vector<std::string> ExportNames() const;
};

/// Supplies guest addresses for symbols the module imports. Returning nullopt means "unresolved":
/// the loader records it and, unless the symbol is weak, fails the load rather than binding to zero,
/// because a silent null here becomes a guest jump to address 0 at runtime.
using SymbolResolver = std::function<std::optional<std::uint32_t>(const std::string& name)>;

struct LoadOptions {
  std::uint32_t hint = 0;              ///< Preferred guest load address; 0 lets the loader choose.
  bool apply_relocations = true;
  bool enforce_relro = true;           ///< Strip write from PT_GNU_RELRO after relocation.
  SymbolResolver resolve;
  /**
   * The address `resolve` answers for a name it could *not* resolve, when it answers with a trap rather
   * than nullopt (the launcher binds misses to a loud out-of-window address so a call names itself). A
   * **weak** symbol given this answer is bound to 0 instead, as bionic's linker binds an unresolved weak
   * reference: code tests such a symbol against null before calling it. Angry Birds Go's statically linked
   * `__cxa_guard_acquire` does exactly that with `__google_potentially_blocking_region_begin`, and a
   * non-zero trap there faulted the first guarded static initialiser. 0 (the default) means "no trap".
   */
  std::uint32_t unresolved_trap = 0;
  /**
   * When false — the default — a relocation this loader cannot apply **fails the load**, naming every
   * slot involved.
   *
   * The default used to be the other way round: refusals were collected into
   * `unsupported_relocs` and the load reported success, which reads as a diagnostic affordance and is
   * actually a silent-wrong-value machine. A skipped `R_ARM_TLS_TPOFF32` leaves a thread-pointer offset
   * holding whatever the file had in it, and the guest then dereferences that as if it were an address
   * — no fault anywhere, and the corruption surfaces far from here. `docs/30` §Step 26 is what the
   * permissive default produced on a real `libc.so`: three TLS slots left unfilled in a library that
   * would have been reported as loaded cleanly.
   *
   * Setting this true is a caller saying out loud that it intends to finish the job itself, which is what
   * the guest linker does for IFUNC slots. It does not make the refusals invisible: they are still
   * recorded, and `rb_guest_libc_tests` asserts on the exact multiset either way.
   */
  bool tolerate_unresolved_relocs = false;

  /**
   * Called for each `ifunc_slots` entry **while the image is still writable**, and returning an address
   * writes it into the slot.
   *
   * The timing is not a preference. `docs/30` §Step 27 measured the alternative on a real `libc.so`: all
   * eight of its IFUNC slots are in memory that the RELRO pass has already made read-only by the time
   * `LoadFromBytes` returns, so a caller that tries to resolve afterwards gets its write refused. bionic
   * resolves inside `relocate()`, before `protect_relro()`, for the same reason.
   *
   * This is also the narrow form of the exception C05 needs. The component's stated invariant is that it
   * works on guest memory *without an executor* — that is what makes M0 testable with no syscall layer —
   * and an IFUNC answer requires running guest code. So the loader runs no code itself and takes no
   * dependency on `rb::exec`: it asks a callback the caller owns. Leaving this unset keeps the invariant
   * whole, and slots stay recorded with `implementation == 0` for the linker to finish later.
   *
   * Returning nullopt is not an error; it is "not resolved yet", and the module loads with the resolver
   * still in the slot. That state is measurably not inert, though: calling such a symbol *runs* the
   * resolver and hands the guest its return value as if it were the answer, so a half-resolved library is
   * worse than a refused load. Assert on `implementation != 0` before you trust an IFUNC export.
   */
  std::function<std::optional<std::uint32_t>(const IfuncSlot&)> resolve_ifunc;
};

/// Reads a whole image from `fd` (seekable) and maps it into `mem`.
/// Returns nullopt and fills `err` with a named stage — a load failure must be diagnosable from the
/// log alone, since most of them will be seen on a phone and not at a desk.
std::optional<GuestModule> LoadFromFd(mem::GuestMemory& mem, int fd, const std::string& path,
                                      const LoadOptions&, std::string* err);

std::optional<GuestModule> LoadFromBytes(mem::GuestMemory& mem, const std::vector<std::uint8_t>& image,
                                         const std::string& path, const LoadOptions&, std::string* err);

/// True for \x7fELF with class 1 (32-bit) and machine EM_ARM — the gate the import pipeline uses to
/// decide whether an APK's `lib/armeabi-v7a` entry is loadable here at all.
bool LooksLikeAArch32Elf(const std::uint8_t* data, std::size_t len);

}  // namespace rb::loader
