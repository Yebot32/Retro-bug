/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * C06 `rb::linker`: the dependency chain, one global symbol scope, and guest constructors.
 *
 * The fixture is two purpose-built libraries (`tests/corpus/link_leaf.c`, `link_mid.c`) rather than a
 * real system library, because a chain with libc in it cannot distinguish "the linker resolved the
 * graph" from "one big library happened to export everything", and because the ordering claim needs a
 * value a guest *wrote* — which needs a constructor that does nothing else.
 *
 * What the lane refuses to do is assert a linker-internal bookkeeping fact and call it a result. The
 * load order, the `order()` vector and the `Lookup` table are all checked, but the claim that closes
 * the component is that after `RunInitializers`, guest memory holds what only a correctly-ordered,
 * correctly-resolved run of two guest functions could have put there.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rb/exec/interp.h"
#include "rb/linker/guest_linker.h"
#include "rb/loader/guest_elf.h"
#include "rb/mem/guest_memory.h"

#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#ifndef RB_LINK_LEAF_PATH
#define RB_LINK_LEAF_PATH "/data/local/tmp/librbleaf.so"
#endif
#ifndef RB_LINK_MID_PATH
#define RB_LINK_MID_PATH "/data/local/tmp/librbmid.so"
#endif

namespace {

using namespace rb;
using namespace rb::mem;
namespace exec = rb::exec;
namespace isa = rb::isa;
namespace loader = rb::loader;
namespace lnk = rb::linker;

int failures = 0;
int checks = 0;

void fail(const char* what, int line, const std::string& detail = {}) {
  ++failures;
  std::printf("FAIL  %s  (line %d)%s%s\n", what, line, detail.empty() ? "" : "  ", detail.c_str());
}

#define CHECK_CTX(cond, detail)                                  \
  do {                                                           \
    ++checks;                                                    \
    if (!(cond)) {                                               \
      fail(#cond, __LINE__, std::string{detail});               \
    }                                                            \
  } while (0)
#define CHECK(cond) CHECK_CTX(cond, {})

std::string Hex(std::uint64_t v) {
  static const char* d = "0123456789abcdef";
  std::string s;
  for (int i = 60; i >= 0; i -= 4) s += d[(v >> i) & 0xf];
  return s;
}

std::vector<std::uint8_t> read_file(const char* path) {
  std::ifstream f(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

unsigned HostPageShift() {
  long ps = sysconf(_SC_PAGESIZE);
  unsigned shift = 12;
  while ((1ul << shift) < static_cast<unsigned long>(ps)) ++shift;
  return shift;
}

constexpr exec::GuestAddr kStackTop = 0x0010'0000u;
constexpr exec::GuestAddr kReturnAddress = 0x00c0'0000u;

/// Runs one guest function to completion and reports whether it returned. `argc/argv/envp` stay zero —
/// the constructors under test take the arguments the ABI offers and use none of them, and inventing a
/// guest argv block would be this lane writing the answer it claims to check.
bool run_guest(loader::CodeAddress fn, exec::InterpExecutor& engine, std::string* why) {
  if (!fn) {
    if (why) *why = "the address to run is null";
    return false;
  }
  exec::MachineState s{};
  s.pc = fn.addr;
  s.r[13] = kStackTop;
  s.r[14] = kReturnAddress;  // the sentinel ends the Run; a real return, not a fault
  s.mode = fn.thumb ? isa::Mode::Thumb : isa::Mode::Arm;
  s.cpsr = fn.thumb ? (1u << 5) : 0u;
  for (int guard = 0; guard < 10000; ++guard) {
    const exec::RunResult r = engine.Run(s, 200);
    if (r.reason == exec::StopReason::Returned) return true;
    if (r.reason == exec::StopReason::Budget) continue;
    if (why) *why = "stopped as reason=" + std::to_string(static_cast<int>(r.reason)) + " " + r.detail;
    return false;
  }
  if (why) *why = "never reached the return sentinel";
  return false;
}

/// The biased address of a data symbol, or 0. `Symbol::value` is `st_value` plus the load bias, which is
/// the guest address the object lives at; `FindExport` answers with a `CodeAddress` meant for branching,
/// so it is the wrong tool for a variable.
exec::GuestAddr data_symbol(const loader::GuestModule& m, const char* name) {
  for (const auto& s : m.exports) {
    if (s.defined && s.name == name && !s.is_function) return s.value;
  }
  return 0;
}

unsigned read_word(mem::GuestMemory& mem, exec::GuestAddr at) {
  unsigned v = 0;
  if (!mem.Read(at, &v, 4)) return 0xdead'ffffu;
  return v;
}

struct Fixture {
  std::vector<std::uint8_t> leaf;
  std::vector<std::uint8_t> mid;
};

std::optional<Fixture> load_fixture() {
  Fixture f;
  f.leaf = read_file(RB_LINK_LEAF_PATH);
  f.mid = read_file(RB_LINK_MID_PATH);
  if (f.leaf.empty() || f.mid.empty()) {
    fail("read the two guest libraries", __LINE__,
         std::string{"leaf "} + std::to_string(f.leaf.size()) + " bytes, mid " +
             std::to_string(f.mid.size()) + " bytes — pushed from tools/run_engine_tests.sh?");
    return std::nullopt;
  }
  if (!loader::LooksLikeAArch32Elf(f.leaf.data(), f.leaf.size()) ||
      !loader::LooksLikeAArch32Elf(f.mid.data(), f.mid.size())) {
    fail("the fixture libraries are not AArch32 ELF", __LINE__);
    return std::nullopt;
  }
  return f;
}

/// The fixture bytes are captured **by value**: this returns a callback that outlives the call it was
/// built in, so a `[&]` here leaves the linker reading a dead `Fixture` — which presented as the chain
/// loading successfully with its dependency missing, an impossible result that took a run to trace back
/// to the capture list rather than to the linker.
lnk::ImageProvider provider_for(const Fixture& f, bool include_leaf) {
  return [f, include_leaf](const std::string& soname) -> std::optional<std::vector<std::uint8_t>> {
    if (soname == "librbleaf.so") return include_leaf ? std::optional{f.leaf} : std::nullopt;
    if (soname == "librbmid.so") return std::optional{f.mid};
    return std::nullopt;
  };
}

// ------------------------------------------------------------------ the chain

/// A dependency that cannot be placed has to stop the load and name itself. This is the same shape as
/// every refusal in this project: the alternative — bind it to nothing and continue — is a guest branch
/// to address 0 at a moment far from the cause. The pair matters: the positive case alone would also
/// pass if the linker ignored `DT_NEEDED` entirely and let the load fail later for an unrelated reason.
void a_missing_dependency_refuses_naming_it() {
  std::string err;
  auto fx = load_fixture();
  if (!fx) return;
  AddressModel model;
  model.kind = AddressModel::kBiased;
  model.host_page_shift = HostPageShift();
  auto mem = GuestMemory::Create(64u << 20, model, &err);
  if (!mem) {
    fail("guest memory", __LINE__, err);
    return;
  }

  lnk::GuestLinker linker{*mem, provider_for(*fx, /*include_leaf=*/false)};
  const auto mod = linker.LinkNamed("librbmid.so", &err);
  CHECK_CTX(!mod.has_value(), "the chain loaded with its leaf missing; it should refuse");
  CHECK_CTX(err.find("librbleaf.so") != std::string::npos,
            "the refusal does not name the missing dependency: " + err);
}

void the_chain_loads_dependencies_first() {
  std::string err;
  auto fx = load_fixture();
  if (!fx) return;
  AddressModel model;
  model.kind = AddressModel::kBiased;
  model.host_page_shift = HostPageShift();
  auto mem = GuestMemory::Create(64u << 20, model, &err);
  if (!mem) {
    fail("guest memory", __LINE__, err);
    return;
  }

  lnk::GuestLinker linker{*mem, provider_for(*fx, true)};
  const auto mid = linker.LinkNamed("librbmid.so", &err);
  CHECK_CTX(mid.has_value(), "linking the chain failed: " + err);
  if (!mid) return;

  CHECK_CTX(linker.order().size() == 2u,
            "expected two modules in the scope, got " + std::to_string(linker.order().size()));
  if (linker.order().size() != 2u) return;
  CHECK_CTX(linker.order()[0]->soname == "librbleaf.so",
            "the leaf is not first in load order: " + linker.order()[0]->soname);
  CHECK_CTX(linker.order()[1]->soname == "librbmid.so",
            "the dependent is not last: " + linker.order()[1]->soname);
  // Distinct placements: one instance of a shared dependency, not two copies of it. Two mappings would
  // leave the two modules disagreeing about a global they both touch, which is the bug `dlopen`-style
  // loaders are famous for.
  CHECK_CTX(linker.order()[0]->base != linker.order()[1]->base, "both modules landed at the same base");

  // The pre-scan and the loader must agree, or the chain is being built from one reading of the image
  // while the relocations come from another.
  const auto peek_mid = lnk::PeekNeeded(fx->mid, &err);
  CHECK_CTX(peek_mid == (*mid)->needed,
            "PeekNeeded said " + std::to_string(peek_mid.size()) + " dependencies, the loader recorded " +
                std::to_string((*mid)->needed.size()));
  CHECK_CTX(!peek_mid.empty() && peek_mid[0] == "librbleaf.so", "the pre-scan missed DT_NEEDED");
  std::string leaf_err;
  const auto peek_leaf = lnk::PeekNeeded(fx->leaf, &leaf_err);
  CHECK_CTX(peek_leaf.empty(), "the leaf reports a dependency: " +
                                   (peek_leaf.empty() ? std::string{} : peek_leaf[0]));

  // Re-linking the same soname reuses the instance rather than mapping a second copy.
  const auto again = linker.LinkNamed("librbmid.so", &err);
  CHECK_CTX(again.has_value() && *again == *mid, "a second LinkNamed mapped a duplicate instance");
  CHECK_CTX(linker.order().size() == 2u, "the scope grew on re-link: " +
                                             std::to_string(linker.order().size()));
}

// ------------------------------------------------------------------ the scope

void a_symbol_resolves_from_the_wider_scope() {
  std::string err;
  auto fx = load_fixture();
  if (!fx) return;
  AddressModel model;
  model.kind = AddressModel::kBiased;
  model.host_page_shift = HostPageShift();
  auto mem = GuestMemory::Create(64u << 20, model, &err);
  if (!mem) {
    fail("guest memory", __LINE__, err);
    return;
  }
  lnk::GuestLinker linker{*mem, provider_for(*fx, true)};
  const auto mid = linker.LinkNamed("librbmid.so", &err);
  if (!mid) {
    fail("link the chain", __LINE__, err);
    return;
  }

  const auto leaf_add = linker.Lookup("leaf_add");
  CHECK_CTX(leaf_add.has_value(), "leaf_add is not in the global scope");
  if (!leaf_add) return;
  const auto in_leaf = linker.order()[0]->FindExport("leaf_add");
  const auto in_mid = (*mid)->FindExport("leaf_add");
  CHECK_CTX(in_leaf.has_value() && *leaf_add == in_leaf->tagged(),
            "Lookup did not return the definition from the module that has it");
  CHECK_CTX(!in_mid.has_value(),
            "the dependent claims to define a symbol it only imports, so scope order proves nothing");
  // The Thumb tag has to survive the lookup: masking it enters Thumb code in ARM state, which decodes
  // different semantics instead of faulting. `docs/30` has the bug this guard exists to prevent.
  CHECK_CTX(in_leaf.has_value() && (*leaf_add & 1u) == (in_leaf->thumb ? 1u : 0u),
            "the ISA tag was lost resolving leaf_add");
}

// ------------------------------------------------------------------ constructors

/**
 * The claim this whole component has been building toward: the emulator ran a guest constructor, in
 * dependency order, with cross-module references resolved.
 *
 * The evidence is three words of guest memory that no ordering mistake can produce together.
 * `g_leaf_seen_by_mid` is what `mid_init` observed of `g_leaf_ran` *while it ran*, so a linker that ran
 * the dependent first leaves 0 there; a linker that never called one of them leaves its marker absent;
 * and `g_mid_ran` carries the result of a call through the other module's PLT (0xB22B22 + 5), so a
 * symbol resolved to the wrong address or entered in the wrong ISA state ends the run with a fault
 * rather than a number.
 */
void guest_constructors_run_in_dependency_order() {
  std::string err;
  auto fx = load_fixture();
  if (!fx) return;
  AddressModel model;
  model.kind = AddressModel::kBiased;
  model.host_page_shift = HostPageShift();
  auto mem = GuestMemory::Create(64u << 20, model, &err);
  if (!mem) {
    fail("guest memory", __LINE__, err);
    return;
  }
  if (!mem->Map(kStackTop - (64u << 10), 64u << 10, kRead | kWrite, &err)) {
    fail("map a guest stack", __LINE__, err);
    return;
  }
  if (!mem->Map(kReturnAddress, kGuestPage, kRead | kWrite, &err)) {
    fail("map the return sentinel", __LINE__, err);
    return;
  }

  lnk::GuestLinker linker{*mem, provider_for(*fx, true)};
  const auto mid = linker.LinkNamed("librbmid.so", &err);
  if (!mid) {
    fail("link the chain", __LINE__, err);
    return;
  }

  const auto* leaf = linker.order()[0];
  const auto leaf_ran_at = data_symbol(*leaf, "g_leaf_ran");
  const auto mid_ran_at = data_symbol(**mid, "g_mid_ran");
  const auto seen_at = data_symbol(**mid, "g_leaf_seen_by_mid");
  CHECK_CTX(leaf_ran_at && mid_ran_at && seen_at,
            "a constructor marker is not an exported data symbol: leaf=0x" + Hex(leaf_ran_at) +
                " mid=0x" + Hex(mid_ran_at) + " seen=0x" + Hex(seen_at));
  if (!leaf_ran_at || !mid_ran_at || !seen_at) return;

  // Nothing has run yet, so the markers have to be absent. Without this row, a stale value from some
  // earlier write would let the assertions below pass on a linker that called nothing at all.
  CHECK_CTX(read_word(*mem, leaf_ran_at) == 0u && read_word(*mem, mid_ran_at) == 0u,
            "a constructor marker was already set before RunInitializers");

  exec::InterpExecutor engine{*mem, kReturnAddress};
  std::vector<std::uint32_t> ran;
  // The linker composes its own "constructor N did not return" message over `err`, so the reason
  // `run_guest` produced has to be kept somewhere the checks can print — without this, a red row here
  // says only that something did not return.
  std::string last_why;
  const lnk::ExecFn exec_fn = [&](loader::CodeAddress fn) {
    std::string why;
    if (!run_guest(fn, engine, &why)) {
      last_why = "at " + std::to_string(fn.addr) + " (thumb=" + std::to_string(fn.thumb) + "): " + why;
      return false;
    }
    ran.push_back(fn.addr);
    return true;
  };
  const bool ok = linker.RunInitializers(exec_fn, &err);
  if (!ok) {
    // Named rather than left as a red row: the address, both modules' bases, and the four halfwords the
    // guest actually sees at the entry point together say whether this is a bad address, a segment
    // placed at the wrong offset, or a decoder refusing real bytes. Without them the failure is a
    // mystery for whoever reads it next.
    std::string at;
    const auto dump = [&](loader::CodeAddress fn) {
      unsigned short h[4] = {0, 0, 0, 0};
      mem->Read(fn.addr, h, 8);
      return " @" + std::to_string(fn.addr) + " thumb=" + std::to_string(fn.thumb) + " bytes=" +
             std::to_string(h[0]) + "," + std::to_string(h[1]) + "," + std::to_string(h[2]) + "," +
             std::to_string(h[3]);
    };
    for (auto* m : linker.order()) {
      at += " [" + m->soname + " base=" + std::to_string(m->base) + " extent=" +
            std::to_string(m->extent) + "]";
      for (const auto& fn : m->init_array) at += dump(fn);
    }
    std::printf("  [ctor] %s | %s%s\n", err.c_str(), last_why.c_str(), at.c_str());
  }
  CHECK_CTX(ok, "RunInitializers failed: " + err + ((ok || last_why.empty()) ? "" : " | " + last_why));
  CHECK_CTX(ran.size() == 2u, "ran " + std::to_string(ran.size()) +
                                  " constructors; each module has exactly one, so this is the count "
                                  "the loader recorded for two .init_array entries");

  CHECK_CTX(read_word(*mem, leaf_ran_at) == 0xA11A11u,
            "g_leaf_ran = 0x" + Hex(read_word(*mem, leaf_ran_at)) + ", the leaf constructor did not run");
  CHECK_CTX(read_word(*mem, seen_at) == 0xA11A11u,
            "g_leaf_seen_by_mid = 0x" + Hex(read_word(*mem, seen_at)) +
                " — mid's constructor ran before the leaf's, or the leaf's did not run at all");
  CHECK_CTX(read_word(*mem, mid_ran_at) == 0xB22B27u,
            "g_mid_ran = 0x" + Hex(read_word(*mem, mid_ran_at)) +
                ", expected 0xB22B22 + leaf_add(2,3) through the other module's PLT");
}

/// A cross-module *call* after the chain is linked, which is the state an app's JNI method runs in.
void a_linked_call_reaches_the_other_module() {
  std::string err;
  auto fx = load_fixture();
  if (!fx) return;
  AddressModel model;
  model.kind = AddressModel::kBiased;
  model.host_page_shift = HostPageShift();
  auto mem = GuestMemory::Create(64u << 20, model, &err);
  if (!mem) {
    fail("guest memory", __LINE__, err);
    return;
  }
  if (!mem->Map(kStackTop - (64u << 10), 64u << 10, kRead | kWrite, &err) ||
      !mem->Map(kReturnAddress, kGuestPage, kRead | kWrite, &err)) {
    fail("map the guest stack and sentinel", __LINE__, err);
    return;
  }
  lnk::GuestLinker linker{*mem, provider_for(*fx, true)};
  const auto mid = linker.LinkNamed("librbmid.so", &err);
  if (!mid) {
    fail("link the chain", __LINE__, err);
    return;
  }
  const auto fn = (*mid)->FindExport("mid_call");
  CHECK_CTX(fn.has_value(), "mid_call is not exported by the dependent");
  if (!fn) return;

  exec::MachineState s{};
  s.pc = fn->addr;
  s.r[0] = 7;
  s.r[13] = kStackTop;
  s.r[14] = kReturnAddress;
  s.mode = fn->thumb ? isa::Mode::Thumb : isa::Mode::Arm;
  s.cpsr = fn->thumb ? (1u << 5) : 0u;
  exec::InterpExecutor engine{*mem, kReturnAddress};
  bool returned = false;
  for (int guard = 0; guard < 100; ++guard) {
    const exec::RunResult r = engine.Run(s, 500);
    if (r.reason == exec::StopReason::Returned) {
      returned = true;
      break;
    }
    if (r.reason != exec::StopReason::Budget) {
      err = r.detail;
      break;
    }
  }
  // mid_call(7) = leaf_add(7,7) + 1 = 15. The value crossing two modules in one call is the point: a
  // GOT slot left at zero by an unresolved import would have branched to address 0 instead.
  CHECK_CTX(returned, "mid_call did not return: " + err);
  CHECK_CTX(s.r[0] == 15u, "mid_call(7) = " + Hex(s.r[0]) + ", expected 15");
}

}  // namespace

int main() {
  a_missing_dependency_refuses_naming_it();
  the_chain_loads_dependencies_first();
  a_symbol_resolves_from_the_wider_scope();
  guest_constructors_run_in_dependency_order();
  a_linked_call_reaches_the_other_module();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
