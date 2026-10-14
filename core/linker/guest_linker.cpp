/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * C06 `rb::linker` — see `guest_linker.h` for what this component is for and the two invariants
 * (dependencies loaded before dependents, nothing bound to zero) that shape every function here.
 */
#include "rb/linker/guest_linker.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#include "rb/loader/elf_defs.h"

namespace rb::linker {
namespace elf = rb::loader::elf;

namespace {

bool CheckHeader(const std::vector<std::uint8_t>& img, const elf::Ehdr32** out) {
  if (img.size() < sizeof(elf::Ehdr32)) return false;
  const auto* h = reinterpret_cast<const elf::Ehdr32*>(img.data());
  if (h->e_ident[0] != 0x7f || h->e_ident[1] != 'E' || h->e_ident[2] != 'L' || h->e_ident[3] != 'F') {
    return false;
  }
  *out = h;
  return true;
}

/// A dynamic symbol name without its `@VERSION` — the form `FindExport` matches on.
std::string BaseName(const std::string& name) { return name.substr(0, name.find('@')); }

std::string Hex(std::uint32_t v) {
  char buf[11];
  std::snprintf(buf, sizeof buf, "0x%x", v);
  return buf;
}

/// `std::string` from a possibly-unterminated, bounded byte run. Returns false if the name does not end
/// inside the image, because then `strtab + off` was not a string at all — a `DT_NEEDED` pointing at the
/// middle of another table would otherwise become a plausible-looking soname.
bool TakeName(const std::vector<std::uint8_t>& img, std::size_t at, std::string* out) {
  if (at >= img.size()) return false;
  std::size_t end = at;
  while (end < img.size() && img[end] != 0) ++end;
  if (end >= img.size()) return false;
  out->assign(reinterpret_cast<const char*>(img.data() + at), end - at);
  return true;
}

}  // namespace

std::vector<std::string> PeekNeeded(const std::vector<std::uint8_t>& image, std::string* err) {
  const auto fail = [&](const std::string& why) -> std::vector<std::string> {
    if (err) *err = why;
    return {};
  };

  const elf::Ehdr32* h = nullptr;
  if (!CheckHeader(image, &h)) return fail("image is not an ELF32 object");

  // The dynamic table is found by program header (it is loaded memory), and read from the file: this is
  // a pre-scan of an image that has not been placed anywhere yet, so there is no guest memory to read
  // and no bias to account for.
  const std::size_t ph_count = h->e_phnum;
  if (h->e_phentsize < sizeof(elf::Phdr32)) return fail("unsupported program header entry size");

  const elf::Phdr32* dyn_ph = nullptr;
  std::vector<elf::Phdr32> loads;
  for (std::size_t i = 0; i < ph_count; ++i) {
    const std::size_t at = h->e_phoff + i * h->e_phentsize;
    if (at + sizeof(elf::Phdr32) > image.size()) return fail("program header table runs past the image");
    const auto* p = reinterpret_cast<const elf::Phdr32*>(image.data() + at);
    if (p->p_type == elf::PT_DYNAMIC && !dyn_ph) dyn_ph = p;
    if (p->p_type == elf::PT_LOAD) loads.push_back(*p);
  }
  if (!dyn_ph) return {};  // a static or executable image has no dependencies to name
  // Widened before the sum: two 32-bit values added in 32 bits wrap mod 2^32, and a crafted
  // `p_offset = 0xFFFFFFF0, p_filesz = 0x100` passed this check and then handed the parser a
  // `Dyn32*` pointing megabytes past the image.
  if (static_cast<std::uint64_t>(dyn_ph->p_offset) + dyn_ph->p_filesz > image.size()) {
    return fail("PT_DYNAMIC runs past the image");
  }

  const auto* dyn = reinterpret_cast<const elf::Dyn32*>(image.data() + dyn_ph->p_offset);
  const std::size_t dyn_count = dyn_ph->p_filesz / sizeof(elf::Dyn32);

  std::vector<std::uint32_t> needed_off;
  std::uint32_t strtab = 0;
  bool have_strtab = false;
  for (std::size_t i = 0; i < dyn_count; ++i) {
    // Stop at DT_NULL: the segment is padded past the terminator, and reading on turns the filler into
    // entries — the same trap `rb_guest_elf_tests` documents for its own parser.
    if (dyn[i].d_tag == elf::DT_NULL) break;
    if (dyn[i].d_tag == elf::DT_NEEDED) needed_off.push_back(static_cast<std::uint32_t>(dyn[i].d_val));
    if (dyn[i].d_tag == elf::DT_STRTAB) {
      strtab = static_cast<std::uint32_t>(dyn[i].d_val);
      have_strtab = true;
    }
  }
  if (needed_off.empty()) return {};
  if (!have_strtab) return fail("DT_NEEDED present with no DT_STRTAB");

  // DT_STRTAB is a vaddr, so it has to be mapped back to the file the way the loader will be.
  std::size_t str_at = 0;
  bool mapped = false;
  for (const auto& p : loads) {
    if (strtab >= p.p_vaddr && strtab < p.p_vaddr + p.p_filesz) {
      str_at = p.p_offset + (strtab - p.p_vaddr);
      mapped = true;
      break;
    }
  }
  if (!mapped) return fail("DT_STRTAB is not inside any PT_LOAD");

  // Names in table order. The loader's own `GuestModule::needed` comes out in the same order, and the
  // two are asserted equal in `rb_linker_tests` — which is what keeps this pre-scan honest rather than
  // letting it drift into a second, quietly-wrong notion of what an image depends on.
  std::vector<std::string> out;
  for (std::uint32_t off : needed_off) {
    std::string name;
    if (!TakeName(image, str_at + off, &name)) return fail("a DT_NEEDED name runs past the image");
    if (name.empty()) return fail("a DT_NEEDED name is the empty string");
    out.push_back(std::move(name));
  }
  return out;
}

std::optional<loader::CodeAddress> UnresolvedIfuncExport(const loader::GuestModule& m,
                                                         const std::string& name) {
  const auto fn = m.FindExport(name);
  if (!fn) return std::nullopt;
  // The resolver is recognised by address, not by the `ifunc` flag alone: when `m` resolved a slot of
  // this name, `FindExport` already answered with the implementation, and that answer must not be
  // resolved a second time.
  const std::string base = BaseName(name);
  for (const loader::Symbol& s : m.exports) {
    if (!s.defined || !s.ifunc || (s.value & ~1u) != fn->addr) continue;
    if (BaseName(s.name) != base) continue;
    return fn;
  }
  return std::nullopt;
}

std::optional<loader::GuestModule*> GuestLinker::Find(const std::string& soname) {
  for (auto& m : modules_) {
    if (m.path == soname || m.soname == soname) return const_cast<loader::GuestModule*>(&m);
  }
  return std::nullopt;
}

std::optional<loader::GuestModule*> GuestLinker::Link(const std::string& soname,
                                                     const std::vector<std::uint8_t>& image,
                                                     std::string* err) {
  return LinkAt(soname, image, 0, err);
}

std::optional<loader::GuestModule*> GuestLinker::LinkNamed(const std::string& soname, std::string* err) {
  auto image = provider_ ? provider_(soname) : std::nullopt;
  if (!image) {
    if (err) *err = "no image for '" + soname + "'";
    return std::nullopt;
  }
  return LinkAt(soname, *image, 0, err);
}

std::optional<loader::GuestModule*> GuestLinker::LinkAt(const std::string& soname,
                                                        const std::vector<std::uint8_t>& image,
                                                        int depth, std::string* err) {
  const auto fail = [&](const std::string& why) -> std::optional<loader::GuestModule*> {
    if (err) *err = why;
    return std::nullopt;
  };

  if (auto existing = Find(soname)) return existing;

  // A `DT_NEEDED` cycle is pathological — two libraries each linking the other with a `NEEDED` entry,
  // which the GNU tools refuse to produce but a hand-edited image can — and the recursion below would
  // grow `modules_` forever. Refusing by name beats hanging, and the depth guard catches whatever the
  // seen-set does not.
  if (std::find(loading_.begin(), loading_.end(), soname) != loading_.end()) {
    std::string chain;
    for (const auto& s : loading_) chain += s + " -> ";
    return fail("dependency cycle at '" + soname + "' (" + chain + soname + ")");
  }
  if (depth > 32) return fail("dependency chain deeper than 32 at '" + soname + "'");

  loading_.push_back(soname);
  struct Pop {
    std::vector<std::string>& v;
    ~Pop() { v.pop_back(); }
  } pop{loading_};

  std::string peek_err;
  const auto deps = PeekNeeded(image, &peek_err);
  // A pre-scan that could not read the dependency list is not "there is none", and continuing past it
  // is precisely the silent under-link this component exists to prevent: the image would be relocated
  // without its dependencies in scope, and the loader's own refusal would then look like a missing
  // symbol rather than a skipped graph. An image with no `PT_DYNAMIC` at all is the exception, and
  // `PeekNeeded` reports it with no error because "no dependencies" is its correct answer.
  if (!peek_err.empty()) {
    return fail("reading the dependencies of '" + soname + "': " + peek_err);
  }
  for (const auto& dep : deps) {
    // Already in the scope beats asking the provider for it. A `vector`-of-modules hit here would not be
    // a performance detail: the provider would hand back a second copy of a library that is loaded, the
    // copy would be placed at a different address, and the two would each answer part of the global
    // scope — so a data symbol the first one initialises would read as uninitialised through the second.
    // That is the same fault as loading `libc.so` twice, which is why bionic looks at its loaded list
    // before it searches the namespace.
    if (auto already = Find(dep)) continue;
    auto dep_image = provider_ ? provider_(dep) : std::nullopt;
    if (!dep_image) return fail("'" + dep + "' is needed by '" + soname + "' but no image was provided");
    auto loaded = LinkAt(dep, *dep_image, depth + 1, err);
    if (!loaded) return fail("while loading '" + dep + "' needed by '" + soname + "': " +
                             (err && !err->empty() ? *err : std::string{"failed"}));
  }

  loader::LoadOptions opts = base_;
  opts.hint = 0;  // the chain places each image itself; a caller's hint means one module
  std::string ifunc_refusal;
  opts.resolve = [&](const std::string& name) -> std::optional<std::uint32_t> {
    if (auto v = Lookup(name)) return v;
    // A refused IFUNC is answered here and not by the host: the scope *does* define the name, and a host
    // stand-in would bind the import to something that is not the function — the same silent wrong
    // answer as binding the resolver, one step removed.
    if (const IfuncExport* refused = RefusedIfunc(name)) {
      if (ifunc_refusal.empty()) ifunc_refusal = refused->refusal;
      return std::nullopt;
    }
    host_requests_.push_back(name);
    return host_ ? host_(name) : std::nullopt;
  };

  auto mod = loader::LoadFromBytes(memory_, image, soname, opts, err);
  // The refusal outranks the loader's own verdict. For a non-weak import the loader has already failed
  // with "unresolved symbol", which is true and unhelpful; for a weak one it bound the import to 0 and
  // succeeded, so the image is unmapped again here — a module whose call to `strcpy` branches to the null
  // page is not loaded either.
  if (!ifunc_refusal.empty()) {
    if (mod) {
      std::string ignored;
      memory_.Unmap(mod->base, mod->extent, &ignored);
    }
    return fail("loading '" + soname + "': " + ifunc_refusal);
  }
  if (!mod) return fail("loading '" + soname + "': " + (err ? *err : std::string{}));

  modules_.push_back(std::move(*mod));
  loader::GuestModule* placed = &modules_.back();
  order_.push_back(placed);
  return placed;
}

std::optional<std::uint32_t> GuestLinker::Lookup(const std::string& symbol) {
  for (auto* m : order_) {
    const auto fn = m->FindExport(symbol);
    if (!fn) continue;
    const auto resolver = UnresolvedIfuncExport(*m, symbol);
    if (!resolver) return fn->tagged();
    // The first definer wins even when its IFUNC is refused: falling through to a later module's
    // definition would bind a different library's `strcpy` than the global scope says is the one.
    const std::uint32_t impl = BindIfuncExport(*m, symbol, *resolver).implementation;
    if (impl == 0) return std::nullopt;
    return impl;
  }
  return std::nullopt;
}

const IfuncExport& GuestLinker::BindIfuncExport(const loader::GuestModule& m, const std::string& name,
                                                loader::CodeAddress resolver) {
  // Keyed by the resolver, so `strcpy@LIBC` and `strcpy` share one answer and one run, and a refusal is
  // not retried: a resolver is guest code, and the same one asked again answers the same way. bionic does
  // re-run resolvers per relocation; caching changes the cost, not the binding.
  for (const IfuncExport& e : ifunc_exports_) {
    if (e.resolver.tagged() == resolver.tagged()) return e;
  }
  IfuncExport e;
  e.name = BaseName(name);
  e.module = m.soname.empty() ? m.path : m.soname;
  e.resolver = resolver;
  const std::string what =
      "'" + e.name + "' is an IFUNC export of '" + e.module + "' (resolver " + Hex(resolver.tagged()) + ")";
  if (!base_.resolve_ifunc) {
    e.refusal = what + " and this linker has no resolve_ifunc hook to run it; binding the resolver would"
                       " make every call return a code pointer instead of doing the work";
  } else {
    loader::IfuncSlot slot;
    slot.where = 0;  // no slot: this is an import being bound, not a relocation of the defining module
    slot.resolver = resolver;
    slot.slot_value = resolver.tagged();
    slot.name = e.name;
    const auto impl = base_.resolve_ifunc(slot);
    if (!impl) {
      e.refusal = what + " and its resolver did not produce an implementation";
    } else if (*impl == 0) {
      e.refusal = what + " and its resolver returned 0, which is not an implementation";
    } else {
      e.implementation = *impl;
    }
  }
  ifunc_exports_.push_back(std::move(e));
  return ifunc_exports_.back();
}

const IfuncExport* GuestLinker::RefusedIfunc(const std::string& name) const {
  const std::string base = BaseName(name);
  for (const IfuncExport& e : ifunc_exports_) {
    if (e.implementation == 0 && e.name == base) return &e;
  }
  return nullptr;
}

bool GuestLinker::RunInitializers(const ExecFn& exec, std::string* err) {
  const auto fail = [&](const std::string& why) {
    if (err) *err = why;
    return false;
  };
  if (!exec) return fail("no executor supplied to run initializers");

  for (auto* m : order_) {
    // `DT_INIT` first, then `DT_INIT_ARRAY` — the order bionic states verbatim in
    // `soinfo::call_constructors` ("DT_INIT should be called before DT_INIT_ARRAY if both are
    // present") and glibc's `call_init` matches. The first draft ran them the other way round with
    // a comment claiming it was bionic's order; the classic image with a `DT_INIT` that sets up
    // state its array entries consume got that setup *after* its consumers. A zero array entry is a
    // gap some linkers leave in the array, not a function to call.
    //
    // Skipped when the module's constructors already ran: a shared dependency is loaded once but is
    // in every dependent's `order_`, and a retry after a mid-way failure would re-run the allocator
    // init and static constructors of everything before the failure. bionic guards per-soinfo with
    // `constructors_called`; `GuestModule` carries the same flag.
    if (m->constructors_called) continue;
    if (m->init && !exec(m->init)) {
      return fail("DT_INIT of '" + m->soname + "' at " + std::to_string(m->init.addr) +
                  " did not return");
    }
    for (std::size_t i = 0; i < m->init_array.size(); ++i) {
      const auto fn = m->init_array[i];
      if (fn.addr == 0) continue;
      if (!exec(fn)) {
        return fail("constructor " + std::to_string(i) + " of '" + m->soname + "' at " +
                    std::to_string(fn.addr) + " did not return");
      }
    }
    m->constructors_called = true;
  }
  return true;
}

}  // namespace rb::linker
