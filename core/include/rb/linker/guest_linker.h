/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * C06 `rb::linker` — the guest-side link editor: dependency chains, one global symbol scope, and the
 * order startup work runs in.
 *
 * `docs/10` §8's open choice was "run AOSP's arm32 linker under emulation" versus "RetroBug's own", and
 * the plan resolves M1 by trying the own-loader path first. This is that path, and the shape of it is
 * dictated by what C05 already guarantees rather than by what a linker conventionally exposes:
 *
 *   - **C05 loads one image and knows nothing of its neighbours.** `LoadOptions::resolve` is the seam,
 *     so the linker's whole job is to answer that callback from the modules it has already placed —
 *     which means a dependency has to be *loaded before* the image that needs it, because relocations
 *     are applied inside `LoadFromBytes` and cannot be re-asked later.
 *   - **Loading must stay free of execution and of I/O policy.** So the bytes arrive through a provider
 *     callback (`ImageProvider`), the linker opens no files, and it runs no guest code: initializers are
 *     driven by an injected function, which keeps `rb::linker` from depending on `rb::exec` and keeps
 *     the "what executes at load" question in one named place.
 *   - **Nothing here may bind a symbol to zero.** `LoadOptions::resolve` returning nullopt fails the
 *     load for a non-weak import, and that behaviour is the point — a null in a GOT entry is a guest
 *     branch to address 0 at some later, unrelated moment.
 *
 * `DlopenGuest`/`DlsymGuest` from `docs/20` §C06 are deliberately not implemented yet: they are the
 * host-callable face of this object, and they need the nine host symbols `rb_guest_libc_tests` counted
 * plus a `dlopen` handle identity that survives being called from emulated code. The chain and the scope
 * come first because every other item on the launch list is downstream of them.
 */
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rb/loader/guest_elf.h"
#include "rb/mem/guest_memory.h"

namespace rb::linker {

/// Turns a `DT_NEEDED` soname into the bytes to load. Returning nullopt means "not found", and the
/// linker fails the load naming the soname it could not place.
using ImageProvider =
    std::function<std::optional<std::vector<std::uint8_t>>(const std::string& soname)>;

/// Runs one guest function. `argc`/`argv`/`envp` are the linker's business, not this callback's: it is
/// given an address to enter and told whether it came back.
using ExecFn = std::function<bool(loader::CodeAddress)>;

/// The read-only `DT_NEEDED` names of an image, without mapping it.
///
/// Needed because of the ordering constraint above: the parent's relocations are applied during
/// `LoadFromBytes`, so its dependencies must already be loaded, so their names must be known before the
/// call that resolves them. The linker reads `.dynamic` for this and nothing else — no segments, no
/// relocations, no memory.
///
/// It is also a check on C05 rather than a copy of it: `rb_linker_tests` asserts this agrees with
/// `GuestModule::needed` for the same bytes, so a parser that drifts from the loader's shows up as a
/// failure instead of as a dependency silently never loaded.
std::vector<std::string> PeekNeeded(const std::vector<std::uint8_t>& image, std::string* err);

class GuestLinker {
 public:
  /// `host` answers symbols no guest module exports — the nine names a real `libc.so` asks the host for
  /// (`android_dlopen_ext`, `__loader_shared_globals`, …), and a test's stand-ins. May be null.
  ///
  /// `base` carries the per-image load flags applied to every module in the chain —
  /// `tolerate_unresolved_relocs` (a real bionic still has TLS slots to hand to a linker, so loading it
  /// needs the opt-in) and `resolve_ifunc`. Its `resolve` and `hint` are ignored: the scope is this
  /// object's, and each image has to be placed where the chain can.
  GuestLinker(mem::GuestMemory& memory, ImageProvider provider, loader::SymbolResolver host = {},
              const loader::LoadOptions& base = {})
      : memory_{memory}, provider_{std::move(provider)}, host_{std::move(host)}, base_{base} {
    base_.hint = 0;
    base_.resolve = nullptr;
  }

  /// Load `soname`'s image and, before finishing it, everything in its `DT_NEEDED`. Already-loaded
  /// sonames are reused, which is what makes two libraries depending on the same third one get *one*
  /// instance of it — the reason a plain per-module resolver could not stand in for this.
  ///
  /// Returns the module, or nullopt with `err` naming the stage: a missing provider, an unloadable
  /// image, an unresolvable import, or a `DT_NEEDED` cycle.
  std::optional<loader::GuestModule*> Link(const std::string& soname,
                                           const std::vector<std::uint8_t>& image, std::string* err);

  /// Same, with the bytes coming from the provider — the shape `dlopen` will use.
  std::optional<loader::GuestModule*> LinkNamed(const std::string& soname, std::string* err);

  /// Modules in the order they finished loading, dependencies first. Initializers run in this order,
  /// and so does symbol lookup.
  const std::vector<loader::GuestModule*>& order() const { return order_; }
  std::optional<loader::GuestModule*> Find(const std::string& soname);

  /// The global scope: the first module in load order that defines `symbol` wins.
  ///
  /// Returns the address **with the Thumb tag in bit 0** — what a GOT entry and a `blx` expect — via
  /// `FindExport`, which is also what distinguishes a resolved IFUNC from its resolver. Reading
  /// `exports` directly gets both wrong: `Symbol::value` still points at the resolver of an
  /// unresolved IFUNC, and `CodeAddress::addr` has the ISA bit masked out.
  std::optional<std::uint32_t> Lookup(const std::string& symbol) const;

  /// Run `DT_INIT` then `DT_INIT_ARRAY` for every module in load order — a dependency's constructors
  /// before its dependent's, because the dependent's init code may call into it.
  ///
  /// Stops at the first function that does not return, naming the module and index: a constructor that
  /// faults is a real defect in the image or in the emulator, and continuing would let a half-initialized
  /// library look loaded.
  bool RunInitializers(const ExecFn&, std::string* err);

  /// Names `Lookup` could not answer and the host was asked for, in request order. Non-empty is not
  /// automatically wrong — a `libc.so` genuinely imports `dlopen` — which is why it is reported rather
  /// than asserted on inside the linker.
  const std::vector<std::string>& host_requests() const { return host_requests_; }

 private:
  std::optional<loader::GuestModule*> LinkAt(const std::string& soname,
                                             const std::vector<std::uint8_t>& image, int depth,
                                             std::string* err);

  mem::GuestMemory& memory_;
  ImageProvider provider_;
  loader::SymbolResolver host_;
  loader::LoadOptions base_;
  // `deque`, not `vector`: `order_` holds pointers into here, and a growing `vector` would move the
  // modules out from under them.
  std::deque<loader::GuestModule> modules_;
  std::vector<loader::GuestModule*> order_;
  std::vector<std::string> loading_;  ///< the `DT_NEEDED` path being resolved, for cycle detection
  std::vector<std::string> host_requests_;
};

}  // namespace rb::linker
