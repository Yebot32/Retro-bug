/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * C08/C09 seam — the fake `JNIEnv`'s guest-visible objects and its host-call bindings.
 *
 * See rb/abi/jni_env.h for what the three guest objects are and why the table indices are measured.
 * This file is the part that touches guest memory, so the recurring rule is: **nothing here may hand a
 * host pointer to the guest, and nothing here may trust a guest pointer.** Every address that arrives
 * from r0..r3 is bounds-checked against the guest window before it is read, and every address returned
 * is a guest address.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rb/abi/jni_env.h"

#include <cstring>
#include <utility>

namespace rb::abi {

namespace {

/// Reads a NUL-terminated guest string, refusing rather than running off the window. A guest that
/// passes a non-NUL-terminated buffer is a guest that would have made the *host* read past its own
/// allocation, and the host has no such page to fault on — the failure would be silent.
bool ReadGuestString(const mem::GuestMemory& memory, GuestAddr at, std::string& out, std::string& err) {
  out.clear();
  if (at == 0 || at >= memory.window_bytes()) {
    err = "JNI string argument 0x" + std::to_string(at) + " is outside the guest window";
    return false;
  }
  constexpr unsigned kMax = 4096;  ///< enough for any real message, small enough to stop a runaway
  for (unsigned i = 0; i < kMax; ++i) {
    const GuestAddr where = at + i;
    if (where >= memory.window_bytes()) {
      err = "JNI string at 0x" + std::to_string(at) + " ran to the end of the window without a NUL";
      return false;
    }
    char c = 0;
    if (!memory.Read(where, &c, 1)) {
      err = "JNI string at 0x" + std::to_string(at) + " is not readable at offset " + std::to_string(i);
      return false;
    }
    if (c == '\0') return true;
    out.push_back(c);
  }
  err = "JNI string at 0x" + std::to_string(at) + " exceeded " + std::to_string(kMax) + " bytes";
  return false;
}

/// Writes a 32-bit guest word. Everything in the table and the arena is one of these or a byte array.
bool PutWord(mem::GuestMemory& memory, GuestAddr at, std::uint32_t v, std::string& err) {
  if (!memory.Write(at, &v, 4)) {
    err = "could not write 0x" + std::to_string(v) + " to guest address " + std::to_string(at);
    return false;
  }
  return true;
}

/// Reads the argument at `index` out of the AAPCS register window, and refuses a read of anything past
/// r3 rather than inventing a stack walk: none of the JNI functions implemented here takes more than
/// three arguments, and a fourth would mean this table needs an argument the environment does not
/// model — which is a decision, not something to guess at from memory offsets.
std::uint32_t Arg(const exec::MachineState& state, unsigned index) {
  return index < 4 ? state.r[index] : 0u;
}

}  // namespace

JniEnv::JniEnv(mem::GuestMemory& memory, AbiContext& abi, GuestAddr where, unsigned arena_bytes,
               std::string* err)
    : memory_{memory}, abi_{abi} {
  // One mapping for all three objects: the table, the `env` word, then the arena. Rounding to whole
  // guest pages is `GuestMemory`'s requirement, and mapping once rather than three times keeps the
  // layout one arithmetic expression instead of three that have to agree.
  const unsigned table_bytes = jni::kTableEntries * 4u;
  const unsigned need = table_bytes + 8u + arena_bytes;  // +8: the env word, padded to keep the arena
                                                        // word-aligned for whatever follows it
  unsigned pages = (need + mem::kGuestPage - 1) / mem::kGuestPage;
  if (pages == 0) pages = 1;
  std::string merr;
  if (!memory_.Map(where, pages * mem::kGuestPage, mem::kRead | mem::kWrite, &merr)) {
    err_ = "could not map the JNIEnv region at 0x" + std::to_string(where) + ": " + merr;
    if (err != nullptr) *err = err_;
    return;
  }
  table_at_ = where;
  env_at_ = where + table_bytes;
  arena_at_ = env_at_ + 8u;
  arena_next_ = arena_at_;
  arena_end_ = arena_at_ + arena_bytes;

  // Every entry gets the slot for its own index, implemented or not. Filling it before binding means
  // a table word is never zero, so a guest that reaches an unimplemented entry lands on a refusal that
  // names the index instead of on address 0.
  for (std::uint32_t i = 0; i < jni::kTableEntries; ++i) {
    if (!PutWord(memory_, table_at_ + i * 4u, abi_.SlotFor(i), merr)) {
      err_ = merr;
      if (err != nullptr) *err = err_;
      return;
    }
  }
  if (!PutWord(memory_, env_at_, table_at_, merr)) {
    err_ = merr;
    if (err != nullptr) *err = err_;
    return;
  }

  // ---- the implemented entries
  //
  // Each one reads its arguments from the AAPCS registers and writes its result to r0, which is the
  // whole of the contract: the bridge has already placed the guest's arguments, and restores PC from
  // LR on the way out. From the guest's side a host slot *is* a callee, so the softfp rules apply in this
  // direction too: a slot that eventually takes a `jdouble` — the `Get*Field`/`Call*Method` families
  // reached through `jvalue`, and `NewObjectV` — must read it from an even/odd core pair, and one that
  // returns a `jfloat`/`jdouble` writes r0 / r0:r1 rather than s0/d0. None of the slots bound below does
  // either yet, which is why the note is here and not in a test.
  abi_.Bind(jni::kGetVersion, [](exec::MachineState& s, std::string*) {
    s.r[0] = jni::kVersion;
    return true;
  });

  abi_.Bind(jni::kNewStringUTF, [this](exec::MachineState& s, std::string* serr) {
    const GuestAddr handle = MakeString(Arg(s, 1));
    if (handle == 0) {
      if (serr != nullptr) *serr = err_.empty() ? "NewStringUTF failed" : err_;
      return false;
    }
    s.r[0] = handle;
    return true;
  });

  abi_.Bind(jni::kGetStringUTFLength, [this](exec::MachineState& s, std::string* serr) {
    std::uint32_t length = 0;
    if (!CheckHandle(Arg(s, 1), length, serr)) return false;
    s.r[0] = length;  // `jsize`, so r0 is the whole result
    return true;
  });

  abi_.Bind(jni::kGetStringUTFChars, [this](exec::MachineState& s, std::string* serr) {
    std::uint32_t length = 0;
    if (!CheckHandle(Arg(s, 1), length, serr)) return false;
    // `isCopy` is an out-parameter the guest may pass NULL for; writing to it unconditionally would be
    // a guest dereferencing address 0, and the copy semantics do not matter to anything yet.
    const GuestAddr is_copy = Arg(s, 2);
    if (is_copy != 0) {
      // `isCopy` points at a `jboolean` — one byte. Writing the 4-byte word a first draft used here
      // overran three bytes into whatever the guest declared beside it, and a pointer one to three
      // bytes before the end of a page turned a successful call into a fault.
      const std::uint8_t one = 1;
      if (!memory_.Write(is_copy, &one, 1)) {
        if (serr != nullptr) *serr = "GetStringUTFChars could not write isCopy";
        return false;
      }
    }
    s.r[0] = Arg(s, 1) + jni::kStringHeaderBytes;
    return true;
  });

  // Still a no-op, and still bound rather than left to the refusal path: "cleared" is the true answer for
  // an environment that cannot have a pending exception — see the note on the exception family below.
  const auto noop = [](exec::MachineState& s, std::string*) {
    s.r[0] = 0;
    return true;
  };

  // The local-reference lifecycle, which JNI defines and the first draft did not implement: strings were
  // bump-allocated and freed only with the instance, so a guest that makes one string per frame walked the
  // arena off its end in tens of frames and the failure surfaced as a bare "NewStringUTF failed" — far
  // from the loop that caused it. Both entries now return the block, and `MakeString` reuses it.
  abi_.Bind(jni::kReleaseStringUTFChars, [this](exec::MachineState& s, std::string* serr) {
    // `(JNIEnv*, jstring, const char* chars)` — the *handle* is the second argument. The chars pointer is
    // an address inside the same block, so validating it separately would be checking our own arithmetic.
    if (!Free(Arg(s, 1), serr)) return false;
    s.r[0] = 0;
    return true;
  });
  abi_.Bind(jni::kDeleteLocalRef, [this](exec::MachineState& s, std::string* serr) {
    // `(JNIEnv*, jobject)`. In this environment the only local references that exist are the strings it
    // hands out, so anything else arriving here is refused by name rather than silently ignored — a guest
    // deleting a reference it never got is a bug worth a line in the log.
    if (!Free(Arg(s, 1), serr)) return false;
    s.r[0] = 0;
    return true;
  });
  abi_.Bind(jni::kExceptionClear, noop);

  // A guest that reaches one of these has asked for something RetroBug does not model. Refusing with
  // the index named is what turns "the app showed a blank screen" into a line in the log that says
  // which JNI function is missing, and it is the shape every remaining entry takes.
  const auto refused = [](exec::MachineState&, std::string* serr, const char* what) {
    if (serr != nullptr) *serr = what;
    return false;
  };
  abi_.Bind(jni::kFindClass, [refused](exec::MachineState& s, std::string* e) {
    (void)s;
    return refused(s, e, "FindClass: RetroBug has no guest class loader yet (C11)");
  });
  abi_.Bind(jni::kGetObjectClass, [refused](exec::MachineState& s, std::string* e) {
    (void)s;
    return refused(s, e, "GetObjectClass: needs the guest class model (C09)");
  });

  // The exception family answers rather than refuses, because "no exception" is the *true* answer on
  // this environment: every function it binds either cannot throw or refuses loudly, so a pending
  // exception is unrepresentable. Binding ExceptionOccurred to the refusal path made the defensive
  // style — `(*env)->ExceptionOccurred(env)` after every call — kill the whole guest call on its
  // first check, and the fault it produced named C09 work that was never the guest's problem.
  abi_.Bind(jni::kExceptionOccurred, [](exec::MachineState& s, std::string*) {
    s.r[0] = 0;  ///< no `jthrowable` is ever pending here
    return true;
  });
  abi_.Bind(jni::kExceptionCheck, [](exec::MachineState& s, std::string*) {
    s.r[0] = 0;  ///< JNI_FALSE: nothing to check
    return true;
  });
  abi_.Bind(jni::kExceptionDescribe, noop);

  ok_ = true;
  if (err != nullptr) err->clear();
}

bool JniEnv::Free(GuestAddr handle, std::string* err) {
  // `DeleteLocalRef(NULL)` is legal in JNI and means nothing to do — not a refusal. It is also what a
  // guest reaches when a slot it called handed it a null reference back.
  if (handle == 0) return true;

  if (handle < arena_at_ || handle >= arena_next_ || (handle & 3u) != 0) {
    if (err != nullptr) {
      *err = "release of 0x" + std::to_string(handle) +
             " which is not a word-aligned handle inside this instance's string arena (0x" +
             std::to_string(arena_at_) + "..0x" + std::to_string(arena_next_) + ")";
    }
    return false;
  }
  std::uint32_t magic = 0, length = 0;
  if (!memory_.Read(handle, &magic, 4) || magic != jni::kStringMagic ||
      !memory_.Read(handle + 4u, &length, 4)) {
    // The same check `CheckHandle` makes, for the same reason, and the wording says which case is most
    // likely: a stale handle is what a double release or a use-after-release looks like.
    if (err != nullptr) {
      *err = "release of 0x" + std::to_string(handle) +
             " which does not carry a live RetroBug string header (already released, or never one of "
             "this environment's)";
    }
    return false;
  }
  const unsigned bytes = (jni::kStringHeaderBytes + length + 1u + 3u) & ~3u;

  // Kill the handle *before* returning the space. A `jstring` a guest kept past its release then fails the
  // magic check instead of being read as whatever the next string wrote there — the difference between a
  // named refusal and a plausible string made of another string's bytes.
  std::string werr;
  if (!PutWord(memory_, handle, 0u, werr)) {
    if (err != nullptr) *err = werr;
    return false;
  }

  const FreeBlock block{handle, bytes};
  std::size_t at = 0;
  while (at < free_.size() && free_[at].at < block.at) ++at;
  free_.insert(free_.begin() + static_cast<std::ptrdiff_t>(at), block);
  // Adjacent holes are one hole: without this the list accumulates slivers and eventually refuses an
  // allocation that the space as a whole holds.
  for (std::size_t i = 0; i + 1 < free_.size();) {
    if (free_[i].at + free_[i].bytes == free_[i + 1].at) {
      free_[i].bytes += free_[i + 1].bytes;
      free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(i + 1));
    } else {
      ++i;
    }
  }
  // Give a block that reaches the bump pointer straight back to it. This is what makes a strict
  // make-then-release loop cost *nothing* rather than merely being reusable: the arena stays at one
  // string's size instead of growing to the high-water mark of every size the guest ever asked for.
  while (!free_.empty() && free_.back().at + free_.back().bytes == arena_next_) {
    arena_next_ = free_.back().at;
    free_.pop_back();
  }
  ++strings_freed_;
  return true;
}

GuestAddr JniEnv::MakeString(GuestAddr chars_at) {
  std::string text;
  std::string why;
  if (!ReadGuestString(memory_, chars_at, text, why)) {
    err_ = why;
    return 0;
  }
  // Word-aligned, so a released block and a fresh one describe the same generous size.
  const unsigned total =
      (jni::kStringHeaderBytes + static_cast<unsigned>(text.size()) + 1u + 3u) & ~3u;
  // First fit over the blocks releases handed back, so a guest that makes and frees strings in a loop
  // reuses one slot instead of walking the arena off its end. Always splitting — even into a remainder too
  // small to satisfy another allocation — is what keeps a *large* freed block from being consumed by a
  // small string: the remainder would otherwise leave the free list and the arena would shrink by the
  // difference on every iteration, which is the leak this change exists to remove. A sliver costs four
  // bytes of list entry and is merged by the coalescing in `Free`.
  GuestAddr handle = 0;
  for (std::size_t i = 0; i < free_.size(); ++i) {
    if (free_[i].bytes < total) continue;
    handle = free_[i].at;
    if (free_[i].bytes == total) {
      free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(i));
    } else {
      free_[i].at += total;
      free_[i].bytes -= total;
    }
    break;
  }
  if (handle == 0) {
    if (arena_next_ > arena_end_ || total > arena_end_ - arena_next_) {
      err_ = "the JNIEnv string arena is full (" + std::to_string(arena_end_ - arena_at_) +
             " bytes, wanted " + std::to_string(total) + ", " + std::to_string(free_.size()) +
             " free block(s) too small)";
      return 0;
    }
    handle = arena_next_;
    arena_next_ += total;
  }
  std::string werr;
  if (!PutWord(memory_, handle, jni::kStringMagic, werr) ||
      !PutWord(memory_, handle + 4u, static_cast<std::uint32_t>(text.size()), werr)) {
    err_ = werr;
    return 0;
  }
  if (!memory_.Write(handle + jni::kStringHeaderBytes, text.data(), text.size() + 1u)) {
    err_ = "could not write the string body at 0x" + std::to_string(handle);
    return 0;
  }
  last_handle_ = handle;
  ++utf_calls_;
  ++strings_made_;
  last_string_ = text;
  return handle;
}

bool JniEnv::CheckHandle(GuestAddr handle, std::uint32_t& length_out, std::string* err) const {
  const auto complain = [&](const std::string& what) {
    if (err != nullptr) *err = what;
    return false;
  };
  if (handle < arena_at_ || handle >= arena_next_ || (handle & 3u) != 0) {
    return complain("JNI string handle 0x" + std::to_string(handle) +
                    " is not in this instance's string arena (0x" + std::to_string(arena_at_) + "..0x" +
                    std::to_string(arena_next_) + ")");
  }
  std::uint32_t magic = 0;
  if (!memory_.Read(handle, &magic, 4)) return complain("JNI string handle is not readable");
  if (magic != jni::kStringMagic) {
    return complain("JNI string handle 0x" + std::to_string(handle) +
                    " does not carry a RetroBug string header");
  }
  if (!memory_.Read(handle + 4u, &length_out, 4)) {
    return complain("JNI string handle has no readable length word");
  }
  if (length_out > arena_end_ - handle - jni::kStringHeaderBytes) {
    // The one check that used to return bare `false`: every other refusal here names itself, and a
    // corrupted length word arriving as a slot-number-only fault cost the reason that found it.
    return complain("JNI string handle 0x" + std::to_string(handle) +
                    " declares " + std::to_string(length_out) +
                    " bytes, which runs past this instance's arena");
  }
  return true;
}

}  // namespace rb::abi
