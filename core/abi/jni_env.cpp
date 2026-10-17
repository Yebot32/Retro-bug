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
#include "rb/abi/jni_args.h"

#include <cstdio>
#include <cstring>
#include <utility>

namespace rb::abi {

namespace {

/// The guest registers at the `Call<T>Method` slot a host method is running for, per host thread (M2: two
/// guest threads can be inside host methods at once). `env` scopes it to one environment.
struct CallingFrame {
  const JniEnv* env = nullptr;
  const exec::MachineState* state = nullptr;
};
thread_local CallingFrame tls_calling;

/// Hex digits for a guest word — what every "0x" in a refusal is followed by. The file used to print
/// `"0x" + Hex32(addr)`, a decimal number wearing a hex prefix, in every handle it named.
std::string Hex32(std::uint32_t v) {
  char buf[9];
  std::snprintf(buf, sizeof buf, "%x", v);
  return buf;
}

/// Reads a NUL-terminated guest string, refusing rather than running off the window. A guest that
/// passes a non-NUL-terminated buffer is a guest that would have made the *host* read past its own
/// allocation, and the host has no such page to fault on — the failure would be silent.
bool ReadGuestString(const mem::GuestMemory& memory, GuestAddr at, std::string& out, std::string& err) {
  out.clear();
  if (at == 0 || at >= memory.window_bytes()) {
    err = "JNI string argument 0x" + Hex32(at) + " is outside the guest window";
    return false;
  }
  constexpr unsigned kMax = 4096;  ///< enough for any real message, small enough to stop a runaway
  for (unsigned i = 0; i < kMax; ++i) {
    const GuestAddr where = at + i;
    if (where >= memory.window_bytes()) {
      err = "JNI string at 0x" + Hex32(at) + " ran to the end of the window without a NUL";
      return false;
    }
    char c = 0;
    if (!memory.Read(where, &c, 1)) {
      err = "JNI string at 0x" + Hex32(at) + " is not readable at offset " + std::to_string(i);
      return false;
    }
    if (c == '\0') return true;
    out.push_back(c);
  }
  err = "JNI string at 0x" + Hex32(at) + " exceeded " + std::to_string(kMax) + " bytes";
  return false;
}

/// Writes a 32-bit guest word. Everything in the table and the arena is one of these or a byte array.
bool PutWord(mem::GuestMemory& memory, GuestAddr at, std::uint32_t v, std::string& err) {
  if (!memory.Write(at, &v, 4)) {
    err = "could not write 0x" + Hex32(v) + " to guest address 0x" + Hex32(at);
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

const exec::MachineState* JniEnv::calling_state() const {
  return tls_calling.env == this ? tls_calling.state : nullptr;
}

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
    err_ = "could not map the JNIEnv region at 0x" + Hex32(where) + ": " + merr;
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
    const GuestAddr handle = Arg(s, 1);
    if (!CheckHandle(handle, length, serr)) return false;
    // `isCopy` is an out-parameter the guest may pass NULL for; writing to it unconditionally would be
    // a guest dereferencing address 0. The value written is the truth, not a claim: the pointer handed
    // back below aims at a copy in the arena, which is what `JNI_TRUE` promises. The write must stay
    // one byte — `isCopy` points at a `jboolean`, and the 4-byte word a first draft used here overran
    // three bytes into whatever the guest declared beside it.
    const GuestAddr is_copy = Arg(s, 2);
    if (is_copy != 0) {
      const std::uint8_t one = 1;
      if (!memory_.Write(is_copy, &one, 1)) {
        if (serr != nullptr) *serr = "GetStringUTFChars could not write isCopy";
        return false;
      }
    }
    const GuestAddr copy = MakeCharsCopy(handle, length);
    if (copy == 0) {
      if (serr != nullptr) *serr = err_.empty() ? "GetStringUTFChars failed" : err_;
      return false;
    }
    s.r[0] = copy + jni::kStringHeaderBytes;
    return true;
  });

  // Still a no-op, and still bound rather than left to the refusal path: "cleared" is the true answer for
  // an environment that cannot have a pending exception — see the note on the exception family below.
  const auto noop = [](exec::MachineState& s, std::string*) {
    s.r[0] = 0;
    return true;
  };

  // The local-reference lifecycle, split the way JNI defines it: `DeleteLocalRef` frees a `jstring`,
  // `ReleaseStringUTFChars` frees the chars copy its matching `GetStringUTFChars` handed out, and neither
  // is the other. The first draft freed nothing (a make-per-frame arena walk-off in tens of frames,
  // whose failure surfaced as a bare "NewStringUTF failed" far from the loop that caused it); the
  // revision that fixed that made *both* entries free the string — restoring the exhaustion fix and
  // breaking the ABI, because the ordinary Get → Release → Delete sequence was then refused as a stale
  // handle on the second call and the whole guest call faulted. Returned blocks go to a sorted,
  // coalesced free list that both allocators serve from, so a complete make/use/free cycle at any frame
  // rate stays at one string's size.
  abi_.Bind(jni::kReleaseStringUTFChars, [this](exec::MachineState& s, std::string* serr) {
    // `(JNIEnv*, jstring, const char* chars)` — the third argument is the chars copy the matching
    // `GetStringUTFChars` returned, and it is the only thing this call frees. The `jstring` stays valid
    // until `DeleteLocalRef`, which is JNI's own rule and the one the previous revision broke.
    const GuestAddr chars = Arg(s, 2);
    // The guest hands back the pointer `GetStringUTFChars` returned — the copy's *body*, a header's
    // width past the block handle. Anything at or below that width cannot be one (a null, a bare
    // handle, a guest's own arithmetic), and anything else is classified by the allocation record
    // inside `ReleaseBlock`.
    if (chars <= jni::kStringHeaderBytes) {
      if (serr != nullptr) {
        *serr = "ReleaseStringUTFChars with chars=0x" + Hex32(chars) +
                " — not a pointer this environment handed out";
      }
      return false;
    }
    if (!ReleaseBlock(chars - jni::kStringHeaderBytes, jni::kCharsMagic, "chars buffer", serr)) {
      return false;
    }
    s.r[0] = 0;
    return true;
  });
  abi_.Bind(jni::kDeleteLocalRef, [this](exec::MachineState& s, std::string* serr) {
    // `(JNIEnv*, jobject)`. The local references here are the `jstring`s and `byte[]`s this environment
    // hands out, plus the class-model objects — chars copies are not references — so anything else
    // arriving here is refused by name rather than silently ignored: a guest deleting a reference it
    // never got is a bug worth a line in the log. `DeleteLocalRef(NULL)` is legal and means nothing to
    // do; `ReleaseBlock` owns that rule.
    const GuestAddr ref = Arg(s, 1);
    if (IsArrayRef(ref)) {
      if (arrays_.erase(ref) == 0) {
        if (serr != nullptr) {
          *serr = "DeleteLocalRef(0x" + Hex32(ref) + "): not a live byte[] (already deleted, or never made)";
        }
        return false;
      }
      ++arrays_freed_;
      s.r[0] = 0;
      return true;
    }
    // A class handle or a bound object is permanent in this model — nothing is ever collected, so the
    // reference a guest drops was never what kept it alive. Accepting the delete is the truthful answer.
    if (ref != 0 && (IsClassRef(ref) || ClassOf(ref) != 0)) {
      s.r[0] = 0;
      return true;
    }
    if (!ReleaseBlock(ref, jni::kStringMagic, "string", serr)) return false;
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

  // ---- the host class model. See the header: objects are what the launcher bound, methods are what
  // it defined, and everything else refuses with the object or the method named.
  abi_.Bind(jni::kGetObjectClass, [this](exec::MachineState& s, std::string* serr) {
    const GuestAddr obj = Arg(s, 1);
    const GuestAddr cls = ClassOf(obj);
    if (cls == 0) {
      if (serr != nullptr) {
        *serr = "GetObjectClass(0x" + Hex32(obj) + "): not an object the host bound — the class model "
                "answers only for objects the launcher defined (C09)";
      }
      return false;
    }
    s.r[0] = cls;
    return true;
  });
  abi_.Bind(jni::kGetMethodID, [this](exec::MachineState& s, std::string* serr) {
    // `(env, jclass, const char* name, const char* sig)`.
    const GuestAddr cls = Arg(s, 1);
    if (!IsClassRef(cls) || (cls - jni::kClassRefBase) / 4u >= classes_.size()) {
      if (serr != nullptr) *serr = "GetMethodID: 0x" + Hex32(cls) + " is not a class this environment made";
      return false;
    }
    std::string name, sig, why;
    if (!ReadGuestString(memory_, Arg(s, 2), name, why) || !ReadGuestString(memory_, Arg(s, 3), sig, why)) {
      if (serr != nullptr) *serr = "GetMethodID: " + why;
      return false;
    }
    for (std::size_t i = 0; i < methods_.size(); ++i) {
      const HostMethodRec& m = methods_[i];
      if (m.cls == cls && m.name == name && m.sig == sig) {
        s.r[0] = jni::kMethodIdBase + static_cast<GuestAddr>(i) * 4u;
        return true;
      }
    }
    // JNI would return NULL with NoSuchMethodError pending; this environment cannot represent a pending
    // exception (see the exception family below), so the honest form is a refusal naming the method.
    if (serr != nullptr) {
      *serr = "GetMethodID: " + classes_[(cls - jni::kClassRefBase) / 4u].name + "." + name + sig +
              " has no host implementation — the next method this app needs";
    }
    return false;
  });
  struct Family {
    std::uint32_t base;
    char kind;
  };
  for (const Family f : {Family{jni::kCallObjectMethod, 'L'}, Family{jni::kCallBooleanMethod, 'Z'},
                         Family{jni::kCallIntMethod, 'I'}, Family{jni::kCallVoidMethod, 'V'}}) {
    abi_.Bind(f.base + 0u, [this, f](exec::MachineState& s, std::string* e) {
      return CallHost(s, f.kind, ArgForm::kVariadic, e);
    });
    abi_.Bind(f.base + 1u, [this, f](exec::MachineState& s, std::string* e) {
      return CallHost(s, f.kind, ArgForm::kVaList, e);
    });
    abi_.Bind(f.base + 2u, [this, f](exec::MachineState& s, std::string* e) {
      return CallHost(s, f.kind, ArgForm::kJvalues, e);
    });
  }
  // Global and local references to a permanent object are the object: nothing is collected, so a new
  // reference is the same token. Strings and arrays are refused — their lifetime ends at
  // `DeleteLocalRef`, and a global that outlived it would be a stale handle the guest believed in.
  const auto same_ref = [this](exec::MachineState& s, std::string* serr, const char* what) {
    const GuestAddr ref = Arg(s, 1);
    if (ref != 0 && !IsClassRef(ref) && ClassOf(ref) == 0) {
      if (serr != nullptr) {
        *serr = std::string{what} + "(0x" + Hex32(ref) + "): only class-model objects and classes have "
                "references here; a jstring or byte[] ends at DeleteLocalRef";
      }
      return false;
    }
    s.r[0] = ref;
    return true;
  };
  abi_.Bind(jni::kNewGlobalRef, [same_ref](exec::MachineState& s, std::string* e) {
    return same_ref(s, e, "NewGlobalRef");
  });
  abi_.Bind(jni::kNewLocalRef, [same_ref](exec::MachineState& s, std::string* e) {
    return same_ref(s, e, "NewLocalRef");
  });
  abi_.Bind(jni::kDeleteGlobalRef, [same_ref](exec::MachineState& s, std::string* e) {
    if (!same_ref(s, e, "DeleteGlobalRef")) return false;
    s.r[0] = 0;
    return true;
  });
  abi_.Bind(jni::kIsSameObject, [](exec::MachineState& s, std::string*) {
    s.r[0] = s.r[1] == s.r[2] ? 1u : 0u;  // one token per object here, so identity is equality
    return true;
  });
  abi_.Bind(jni::kGetArrayLength, [this](exec::MachineState& s, std::string* serr) {
    const auto it = arrays_.find(Arg(s, 1));
    if (it == arrays_.end()) {
      if (serr != nullptr) *serr = "GetArrayLength(0x" + Hex32(Arg(s, 1)) + "): not a live byte[] of this environment";
      return false;
    }
    s.r[0] = static_cast<std::uint32_t>(it->second->size());
    return true;
  });
  abi_.Bind(jni::kGetByteArrayRegion, [this](exec::MachineState& s, std::string* serr) {
    // `(env, array, jsize start, jsize len, jbyte* buf)`: `buf` is the first stacked word.
    const auto it = arrays_.find(Arg(s, 1));
    if (it == arrays_.end()) {
      if (serr != nullptr) *serr = "GetByteArrayRegion(0x" + Hex32(Arg(s, 1)) + "): not a live byte[] of this environment";
      return false;
    }
    const auto start = static_cast<std::int32_t>(s.r[2]);
    const auto len = static_cast<std::int32_t>(s.r[3]);
    std::uint32_t buf = 0;
    if (!memory_.Read(s.r[13], &buf, 4)) {
      if (serr != nullptr) *serr = "GetByteArrayRegion: the stacked buf argument is not readable";
      return false;
    }
    const auto size = static_cast<std::int64_t>(it->second->size());
    if (start < 0 || len < 0 || static_cast<std::int64_t>(start) + len > size) {
      // JNI throws ArrayIndexOutOfBoundsException here; with no exceptions to raise, refuse by name.
      if (serr != nullptr) {
        *serr = "GetByteArrayRegion: [" + std::to_string(start) + ", +" + std::to_string(len) +
                ") is outside a byte[" + std::to_string(size) + "]";
      }
      return false;
    }
    if (len != 0 && !memory_.Write(buf, it->second->data() + start, static_cast<std::size_t>(len))) {
      if (serr != nullptr) {
        *serr = "GetByteArrayRegion: could not write " + std::to_string(len) + " bytes to guest 0x" + Hex32(buf);
      }
      return false;
    }
    s.r[0] = 0;
    return true;
  });
  abi_.Bind(jni::kNewByteArray, [this](exec::MachineState& s, std::string* serr) {
    const auto len = static_cast<std::int32_t>(s.r[1]);
    // A host-side array, so a guest that asks for gigabytes is refused by name rather than allocated.
    if (len < 0 || len > (64 << 20)) {
      if (serr != nullptr) *serr = "NewByteArray(" + std::to_string(len) + "): not a length this environment makes";
      return false;
    }
    const GuestAddr h = NewHostByteArray(std::vector<std::uint8_t>(static_cast<std::size_t>(len), 0));
    if (h == 0) {
      if (serr != nullptr) *serr = "NewByteArray: the array handle range is exhausted";
      return false;
    }
    s.r[0] = h;
    return true;
  });
  abi_.Bind(jni::kSetByteArrayRegion, [this](exec::MachineState& s, std::string* serr) {
    // `(env, array, jsize start, jsize len, const jbyte* buf)`: `buf` is the first stacked word.
    const auto it = arrays_.find(Arg(s, 1));
    if (it == arrays_.end()) {
      if (serr != nullptr) *serr = "SetByteArrayRegion(0x" + Hex32(Arg(s, 1)) + "): not a live byte[] of this environment";
      return false;
    }
    const auto start = static_cast<std::int32_t>(s.r[2]);
    const auto len = static_cast<std::int32_t>(s.r[3]);
    std::uint32_t buf = 0;
    if (!memory_.Read(s.r[13], &buf, 4)) {
      if (serr != nullptr) *serr = "SetByteArrayRegion: the stacked buf argument is not readable";
      return false;
    }
    const auto size = static_cast<std::int64_t>(it->second->size());
    if (start < 0 || len < 0 || static_cast<std::int64_t>(start) + len > size) {
      if (serr != nullptr) {
        *serr = "SetByteArrayRegion: [" + std::to_string(start) + ", +" + std::to_string(len) +
                ") is outside a byte[" + std::to_string(size) + "]";
      }
      return false;
    }
    if (len != 0 && !memory_.Read(buf, it->second->data() + start, static_cast<std::size_t>(len))) {
      if (serr != nullptr) {
        *serr = "SetByteArrayRegion: could not read " + std::to_string(len) + " bytes from guest 0x" + Hex32(buf);
      }
      return false;
    }
    s.r[0] = 0;
    return true;
  });
  // The elements pair: a guest-visible *copy* in the arena (so the `isCopy=1` written is the truth), tied
  // to its array by the record. Release modes are JNI's: 0 copies back and frees, JNI_COMMIT (1) copies
  // back and keeps the buffer, JNI_ABORT (2) frees without copying.
  abi_.Bind(jni::kGetByteArrayElements, [this](exec::MachineState& s, std::string* serr) {
    const GuestAddr arr = Arg(s, 1);
    const auto it = arrays_.find(arr);
    if (it == arrays_.end()) {
      if (serr != nullptr) *serr = "GetByteArrayElements(0x" + Hex32(arr) + "): not a live byte[] of this environment";
      return false;
    }
    const auto n = static_cast<std::uint32_t>(it->second->size());
    const unsigned total = (jni::kStringHeaderBytes + n + 3u) & ~3u;
    const GuestAddr block = AllocBlock(total);
    std::string werr;
    if (block == 0) {
      if (serr != nullptr) *serr = "GetByteArrayElements: " + err_;
      return false;
    }
    if (!PutWord(memory_, block, jni::kElementsMagic, werr) || !PutWord(memory_, block + 4u, n, werr)) {
      ReturnSpace(block, total);
      if (serr != nullptr) *serr = "GetByteArrayElements: " + werr;
      return false;
    }
    live_.emplace(block, LiveBlock{total, n, jni::kElementsMagic});
    if (n != 0 && !memory_.Write(block + jni::kStringHeaderBytes, it->second->data(), n)) {
      ReleaseInternal(block);
      if (serr != nullptr) *serr = "GetByteArrayElements: could not write the copy at 0x" + Hex32(block);
      return false;
    }
    const GuestAddr is_copy = Arg(s, 2);
    const std::uint8_t one = 1;
    if (is_copy != 0 && !memory_.Write(is_copy, &one, 1)) {
      ReleaseInternal(block);
      if (serr != nullptr) *serr = "GetByteArrayElements could not write isCopy";
      return false;
    }
    elements_[block] = arr;
    ++elements_made_;
    s.r[0] = block + jni::kStringHeaderBytes;
    return true;
  });
  abi_.Bind(jni::kReleaseByteArrayElements, [this](exec::MachineState& s, std::string* serr) {
    const GuestAddr arr = Arg(s, 1);
    const GuestAddr elems = Arg(s, 2);
    const std::uint32_t mode = Arg(s, 3);
    const GuestAddr block = elems > jni::kStringHeaderBytes ? elems - jni::kStringHeaderBytes : 0;
    const auto el = elements_.find(block);
    if (el == elements_.end() || el->second != arr) {
      if (serr != nullptr) {
        *serr = "ReleaseByteArrayElements(0x" + Hex32(arr) + ", 0x" + Hex32(elems) +
                "): not elements this environment handed out for that array";
      }
      return false;
    }
    if (mode > 2u) {
      if (serr != nullptr) *serr = "ReleaseByteArrayElements: mode " + std::to_string(mode) + " is not 0, JNI_COMMIT or JNI_ABORT";
      return false;
    }
    if (mode != 2u) {  // 0 and JNI_COMMIT copy back
      const auto it = arrays_.find(arr);
      if (it == arrays_.end()) {
        if (serr != nullptr) *serr = "ReleaseByteArrayElements: the array 0x" + Hex32(arr) + " was deleted before its elements";
        return false;
      }
      if (!it->second->empty() &&
          !memory_.Read(block + jni::kStringHeaderBytes, it->second->data(), it->second->size())) {
        if (serr != nullptr) *serr = "ReleaseByteArrayElements: the copy at 0x" + Hex32(elems) + " is unreadable";
        return false;
      }
    }
    if (mode != 1u) {  // 0 and JNI_ABORT free
      if (!ReleaseBlock(block, jni::kElementsMagic, "byte[] elements", serr)) return false;
      elements_.erase(el);
    }
    s.r[0] = 0;
    return true;
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

  // The JNIEnv table is an engine service, not foreign host code: its functions mutate this environment's
  // arena, records and host class model, which the GGL protects, and none of them waits on another guest
  // thread. So they run with the GGL held — including the host methods `CallHost` dispatches to, which
  // reach the same records through `NewHostByteArray`/`StringText`/`HostArray`. A nested guest call one
  // of them makes (a host method calling back into the guest) may still release the lock at a slice
  // boundary or a blocking syscall; nothing here keeps an invariant across that call except the
  // `methods_` record, which only setup (`DefineHostMethod`) grows.
  for (std::uint32_t slot = 0; slot < jni::kTableEntries; ++slot) {
    abi_.SetSlotLock(slot, AbiContext::SlotLock::kHoldsGgl);
  }

  ok_ = true;
  if (err != nullptr) err->clear();
}

bool JniEnv::ReleaseBlock(GuestAddr handle, std::uint32_t want_magic, const char* what,
                          std::string* err) {
  // `DeleteLocalRef(NULL)` is legal in JNI and means nothing to do — not a refusal. (The release slot
  // pre-checks its own null argument before reaching here: a null *chars* pointer is not legal.)
  if (handle == 0) return true;

  if (handle < arena_at_ || handle >= arena_next_ || (handle & 3u) != 0) {
    if (err != nullptr) {
      *err = "release of 0x" + Hex32(handle) +
             " which is not a word-aligned handle inside this instance's string arena (0x" +
             Hex32(arena_at_) + "..0x" + Hex32(arena_next_) + ")";
    }
    return false;
  }
  // The allocation record decides what this handle is, not the header in guest memory. A missing
  // record is a stale handle — what a double release or a use-after-release looks like — and the
  // wording says which case is most likely.
  const auto it = live_.find(handle);
  if (it == live_.end()) {
    if (err != nullptr) {
      *err = "release of 0x" + Hex32(handle) +
             " which does not carry a live RetroBug " + what +
             " header (already released, or never one of this environment's)";
    }
    return false;
  }
  if (it->second.magic != want_magic) {
    const char* actual = it->second.magic == jni::kStringMagic   ? "jstring"
                         : it->second.magic == jni::kElementsMagic ? "byte[] elements"
                                                                   : "chars buffer";
    if (err != nullptr) {
      *err = "release of 0x" + Hex32(handle) + " which is a live RetroBug " + actual +
             ", not a " + what;
    }
    return false;
  }
  // The header words are guest-writable, so they are a claim; the record is the fact. A scribbled
  // length used to be able to sit inside every range check and still hand the free list a block
  // overlapping its neighbour — two live strings on one block. Disagreement refuses by name.
  std::uint32_t magic = 0, length = 0;
  if (!memory_.Read(handle, &magic, 4) || magic != it->second.magic ||
      !memory_.Read(handle + 4u, &length, 4) || length != it->second.length) {
    if (err != nullptr) {
      *err = "release of 0x" + Hex32(handle) +
             ": something other than this arena wrote its header (declares " + std::to_string(length) +
             " bytes where the record says " + std::to_string(it->second.length) + ")";
    }
    return false;
  }

  // Kill the handle *before* returning the space. A `jstring` a guest kept past its release then
  // fails the record check instead of being read as whatever the next string wrote there — the
  // difference between a named refusal and a plausible string made of another string's bytes.
  std::string werr;
  if (!PutWord(memory_, handle, 0u, werr)) {
    if (err != nullptr) *err = werr;
    return false;
  }
  const unsigned bytes = it->second.total;
  const std::uint32_t kind = it->second.magic;
  live_.erase(it);
  ReturnSpace(handle, bytes);
  if (kind == jni::kStringMagic) {
    ++strings_freed_;
  } else if (kind == jni::kElementsMagic) {
    ++elements_released_;
  } else {
    ++chars_released_;
  }
  return true;
}

void JniEnv::ReturnSpace(GuestAddr handle, unsigned bytes) {
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
}

void JniEnv::ReleaseInternal(GuestAddr handle) {
  const auto it = live_.find(handle);
  if (it == live_.end()) return;  // a make whose header never landed: the caller returns raw space
  const unsigned bytes = it->second.total;
  live_.erase(it);
  std::string ignored;
  PutWord(memory_, handle, 0u, ignored);
  ReturnSpace(handle, bytes);
}

GuestAddr JniEnv::AllocBlock(unsigned total) {
  // First fit over the blocks releases handed back, so a guest that makes and frees strings in a loop
  // reuses one slot instead of walking the arena off its end. Always splitting — even into a remainder
  // too small to satisfy another allocation — is what keeps a *large* freed block from being consumed by
  // a small string: the remainder would otherwise leave the free list and the arena would shrink by the
  // difference on every iteration, which is the leak the free list exists to remove. A sliver costs four
  // bytes of list entry and is merged by the coalescing in `ReleaseBlock`.
  for (std::size_t i = 0; i < free_.size(); ++i) {
    if (free_[i].bytes < total) continue;
    const GuestAddr handle = free_[i].at;
    if (free_[i].bytes == total) {
      free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(i));
    } else {
      free_[i].at += total;
      free_[i].bytes -= total;
    }
    return handle;
  }
  if (arena_next_ > arena_end_ || total > arena_end_ - arena_next_) {
    err_ = "the JNIEnv string arena is full (" + std::to_string(arena_end_ - arena_at_) +
           " bytes, wanted " + std::to_string(total) + ", " + std::to_string(free_.size()) +
           " free block(s) too small)";
    return 0;
  }
  const GuestAddr handle = arena_next_;
  arena_next_ += total;
  return handle;
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
  const GuestAddr handle = AllocBlock(total);
  if (handle == 0) return 0;
  const auto text_bytes = static_cast<std::uint32_t>(text.size());
  std::string werr;
  if (!PutWord(memory_, handle, jni::kStringMagic, werr) ||
      !PutWord(memory_, handle + 4u, text_bytes, werr)) {
    // No record can describe a block whose header never landed, so the space goes back raw — this
    // used to leak on the bump pointer when the header write failed.
    ReturnSpace(handle, total);
    err_ = werr;
    return 0;
  }
  live_.emplace(handle, LiveBlock{total, text_bytes, jni::kStringMagic});
  if (!memory_.Write(handle + jni::kStringHeaderBytes, text.data(), text.size() + 1u)) {
    ReleaseInternal(handle);
    err_ = "could not write the string body at 0x" + Hex32(handle);
    return 0;
  }
  last_handle_ = handle;
  ++utf_calls_;
  ++strings_made_;
  last_string_ = text;
  return handle;
}

GuestAddr JniEnv::MakeCharsCopy(GuestAddr string_handle, std::uint32_t length) {
  const unsigned total = (jni::kStringHeaderBytes + length + 1u + 3u) & ~3u;
  const GuestAddr handle = AllocBlock(total);
  if (handle == 0) return 0;
  std::string werr;
  if (!PutWord(memory_, handle, jni::kCharsMagic, werr) ||
      !PutWord(memory_, handle + 4u, length, werr)) {
    ReturnSpace(handle, total);  // header never landed: no record could describe this block
    err_ = werr;
    return 0;
  }
  live_.emplace(handle, LiveBlock{total, length, jni::kCharsMagic});
  std::string body(length + 1u, '\0');
  std::string why;
  if (!memory_.Read(string_handle + jni::kStringHeaderBytes, body.data(), length + 1u)) {
    why = "could not read the string body at 0x" + Hex32(string_handle);
  } else if (!memory_.Write(handle + jni::kStringHeaderBytes, body.data(), length + 1u)) {
    why = "could not write the chars copy at 0x" + Hex32(handle);
  }
  if (!why.empty()) {
    ReleaseInternal(handle);  // uncounted: a copy that failed to be made must not appear released
    err_ = why;
    return 0;
  }
  ++chars_made_;
  return handle;
}

bool JniEnv::CheckHandle(GuestAddr handle, std::uint32_t& length_out, std::string* err) const {
  const auto complain = [&](const std::string& what) {
    if (err != nullptr) *err = what;
    return false;
  };
  if (handle < arena_at_ || handle >= arena_next_ || (handle & 3u) != 0) {
    return complain("JNI string handle 0x" + Hex32(handle) +
                    " is not in this instance's string arena (0x" + Hex32(arena_at_) + "..0x" +
                    Hex32(arena_next_) + ")");
  }
  // The record is the authority: a handle with no record is stale or never ours, whatever guest
  // memory at that address now happens to contain.
  const auto it = live_.find(handle);
  if (it == live_.end() || it->second.magic != jni::kStringMagic) {
    return complain("JNI string handle 0x" + Hex32(handle) +
                    " does not carry a RetroBug string header");
  }
  // And the header must *agree* with the record. The header is guest-writable memory, so a
  // disagreement is a scribble or a collision worth a named refusal — and the length reported below
  // comes from the record either way, so a guest cannot steer a later read or copy with a number it
  // wrote itself.
  std::uint32_t magic = 0, length = 0;
  if (!memory_.Read(handle, &magic, 4) || magic != jni::kStringMagic ||
      !memory_.Read(handle + 4u, &length, 4)) {
    return complain("JNI string handle 0x" + Hex32(handle) +
                    " does not carry a RetroBug string header");
  }
  if (length != it->second.length) {
    return complain("JNI string handle 0x" + Hex32(handle) +
                    ": something other than this arena wrote its header (declares " +
                    std::to_string(length) + " bytes where the record says " +
                    std::to_string(it->second.length) + ")");
  }
  length_out = it->second.length;
  return true;
}

GuestAddr JniEnv::DefineHostClass(const std::string& jni_name, std::string* err) {
  if (memory_.window_bytes() > jni::kClassRefBase) {
    if (err != nullptr) {
      *err = "the guest window reaches 0x" + Hex32(jni::kClassRefBase) +
             ", so class-model handles would be guest addresses; refusing to define " + jni_name;
    }
    return 0;
  }
  for (std::size_t i = 0; i < classes_.size(); ++i) {
    if (classes_[i].name == jni_name) return jni::kClassRefBase + static_cast<GuestAddr>(i) * 4u;
  }
  classes_.push_back(HostClassRec{jni_name});
  return jni::kClassRefBase + static_cast<GuestAddr>(classes_.size() - 1) * 4u;
}

bool JniEnv::DefineHostMethod(GuestAddr cls, const std::string& name, const std::string& sig,
                              HostMethod fn, std::string* err) {
  const auto fail = [&](const std::string& why) {
    if (err != nullptr) *err = "DefineHostMethod " + name + sig + ": " + why;
    return false;
  };
  if (!IsClassRef(cls) || (cls - jni::kClassRefBase) / 4u >= classes_.size()) return fail("unknown class");
  for (const HostMethodRec& m : methods_) {
    if (m.cls == cls && m.name == name && m.sig == sig) return fail("already defined");
  }
  // `(params)ret`, with every reference — `Lpkg/Name;` or any `[...` array — reduced to L, because the
  // calling convention only cares that it is one word.
  HostMethodRec rec{cls, name, sig, {}, 'V', std::move(fn), 0, 0};
  if (!jni::ParseSignature(sig, &rec.params, &rec.ret)) return fail("unreadable JNI method signature '" + sig + "'");
  methods_.push_back(std::move(rec));
  return true;
}

bool JniEnv::BindHostObject(GuestAddr obj, GuestAddr cls, std::string* err) {
  if (!IsClassRef(cls) || (cls - jni::kClassRefBase) / 4u >= classes_.size()) {
    if (err != nullptr) *err = "BindHostObject: 0x" + Hex32(cls) + " is not a class this environment made";
    return false;
  }
  // 0 is null in JNI, and a handle-shaped value would be mistaken for the handle.
  if (obj == 0 || obj >= jni::kClassRefBase) {
    if (err != nullptr) *err = "BindHostObject: 0x" + Hex32(obj) + " cannot stand for an object";
    return false;
  }
  bound_[obj] = cls;
  return true;
}

GuestAddr JniEnv::ClassOf(GuestAddr obj) const {
  const auto it = bound_.find(obj);
  return it == bound_.end() ? 0 : it->second;
}

GuestAddr JniEnv::NewHostByteArray(std::vector<std::uint8_t> bytes) {
  if (next_array_ >= jni::kHandleSpan / 4u) return 0;
  const GuestAddr h = jni::kArrayRefBase + next_array_++ * 4u;
  arrays_.emplace(h, std::make_shared<std::vector<std::uint8_t>>(std::move(bytes)));
  ++arrays_made_;
  return h;
}

bool JniEnv::StringText(GuestAddr jstring, std::string& out, std::string* err) const {
  std::uint32_t length = 0;
  if (!CheckHandle(jstring, length, err)) return false;
  out.assign(length, '\0');
  if (length != 0 && !memory_.Read(jstring + jni::kStringHeaderBytes, out.data(), length)) {
    if (err != nullptr) *err = "could not read the string body at 0x" + Hex32(jstring);
    return false;
  }
  return true;
}

std::vector<JniEnv::HostMethodUse> JniEnv::host_method_uses() const {
  std::vector<HostMethodUse> out;
  for (const HostMethodRec& m : methods_) {
    out.push_back(HostMethodUse{classes_[(m.cls - jni::kClassRefBase) / 4u].name, m.name, m.sig, m.calls,
                                m.refused});
  }
  return out;
}

bool JniEnv::CallHost(exec::MachineState& s, char family, ArgForm form, std::string* err) {
  // `(env, jobject, jmethodID, ...)` in every form; the forms differ only in where the rest is.
  const GuestAddr obj = Arg(s, 1);
  const GuestAddr mid = Arg(s, 2);
  const auto fail = [&](const std::string& why) {
    if (err != nullptr) *err = why;
    return false;
  };
  if (mid < jni::kMethodIdBase || mid >= jni::kMethodIdBase + jni::kHandleSpan || (mid & 3u) != 0 ||
      (mid - jni::kMethodIdBase) / 4u >= methods_.size()) {
    return fail("Call*Method: 0x" + Hex32(mid) + " is not a method ID this environment handed out");
  }
  HostMethodRec& m = methods_[(mid - jni::kMethodIdBase) / 4u];
  const std::string& cname = classes_[(m.cls - jni::kClassRefBase) / 4u].name;
  const std::string who = cname + "." + m.name + m.sig;
  if (ClassOf(obj) != m.cls) {
    return fail("Call*Method " + who + " on 0x" + Hex32(obj) + ", which is not a bound instance of " + cname);
  }
  // The slot family is the return type the guest's compiler chose; a method whose signature says
  // otherwise would hand back a value in the wrong shape.
  if (m.ret != family) {
    return fail(std::string{"Call"} + family + "Method used for " + who + ", which returns " + m.ret);
  }

  // Argument decoding. The A form is an array of 8-byte `jvalue`s; the V form is AAPCS's `va_list`, one
  // pointer to the stacked values (r3); the variadic form continues in r3 then the stack. In both of
  // the last two, `...` promoted F to double and Z/B/C/S to int, and 8-byte values sit 8-aligned —
  // in the variadic form that alignment also skips r3, since a pair cannot start on an odd register.
  std::vector<std::uint64_t> args;
  std::string derr;
  const jni::ArgForm jform = form == ArgForm::kVariadic ? jni::ArgForm::kVariadic
                             : form == ArgForm::kVaList ? jni::ArgForm::kVaList
                                                        : jni::ArgForm::kJvalues;
  if (!jni::DecodeArgs(s, memory_, jform, m.params, 3, &args, &derr)) return fail("Call*Method " + who + ": " + derr);

  std::uint64_t ret = 0;
  std::string why;
  ++m.calls;
  // Per host thread, and saved/restored, so a host method on one thread never sees another thread's caller,
  // and a host method that calls back into the guest (which calls another host method) gets its own back.
  const CallingFrame saved = tls_calling;
  tls_calling = CallingFrame{this, &s};
  const bool answered = m.fn(*this, args, ret, &why);
  tls_calling = saved;
  if (!answered) {
    ++m.refused;
    return fail(who + ": " + why);
  }
  if (family == 'Z') {
    s.r[0] = static_cast<std::uint32_t>(ret & 0xffu);
  } else if (family != 'V') {
    s.r[0] = static_cast<std::uint32_t>(ret);
  }
  return true;
}

void JniEnv::Audit(diag::Auditor& audit) const {
  bool sorted = true, coalesced = true, in_bounds = true;
  std::size_t free_total = 0;
  for (std::size_t i = 0; i < free_.size(); ++i) {
    const FreeBlock& b = free_[i];
    if (b.at < arena_at_ || b.at + b.bytes > arena_end_ || (b.at & 3u) != 0) in_bounds = false;
    if (i != 0) {
      if (b.at < free_[i - 1].at) sorted = false;
      // Adjacent holes are one hole: if two entries touch, the coalescing in `ReturnSpace` was
      // skipped somewhere and the list can sprout slivers. Overlap is the worse failure.
      if (free_[i - 1].at + free_[i - 1].bytes >= b.at) coalesced = false;
    }
    free_total += b.bytes;
  }
  audit.Check("jni/arena-free-list", sorted && coalesced && in_bounds,
              "sorted=" + std::to_string(sorted) + " coalesced=" + std::to_string(coalesced) +
                  " in_bounds=" + std::to_string(in_bounds));

  bool blocks_ok = true;
  std::size_t live_total = 0;
  for (const auto& [handle, block] : live_) {
    if (handle < arena_at_ || (handle & 3u) != 0 ||
        static_cast<std::uint64_t>(handle) + block.total > arena_end_) {
      blocks_ok = false;
    }
    live_total += block.total;
  }
  const std::size_t arena_total = arena_end_ - arena_at_;
  audit.Check("jni/arena-accounting",
              blocks_ok && arena_next_ <= arena_end_ && live_total + free_total <= arena_total,
              std::to_string(live_.size()) + " live (" + std::to_string(live_total) + " B) + " +
                  std::to_string(free_.size()) + " free (" + std::to_string(free_total) +
                  " B) vs arena " + std::to_string(arena_total) + " B, bump at " +
                  std::to_string(arena_next_ - arena_at_));
  audit.Info("jni/arena", std::to_string(arena_total) + " bytes, " + std::to_string(strings_made_) +
                             " string(s) made, " + std::to_string(chars_made_) + " copy/copies");
  std::size_t array_bytes = 0;
  for (const auto& [handle, bytes] : arrays_) array_bytes += bytes->size();
  audit.Check("jni/array-accounting", arrays_made_ - arrays_freed_ == arrays_.size(),
              std::to_string(arrays_made_) + " made - " + std::to_string(arrays_freed_) + " freed vs " +
                  std::to_string(arrays_.size()) + " live (" + std::to_string(array_bytes) + " B on the host)");
  audit.Info("jni/class-model", std::to_string(classes_.size()) + " class(es), " +
                                    std::to_string(methods_.size()) + " method(s), " +
                                    std::to_string(bound_.size()) + " bound object(s)");
}

}  // namespace rb::abi
