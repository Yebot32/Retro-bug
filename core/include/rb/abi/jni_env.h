/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * C08/C09 seam — a `JNIEnv` the guest can actually call, made of guest memory and host-call slots.
 *
 * What a fake `JNIEnv` physically is, from the guest's own instructions rather than from a header:
 *
 *     ldr   r2, [r0]             ; r2 = *env          -> the function table
 *     ldr.w r2, [r2, #0x29c]     ; r2 = table->NewStringUTF
 *     blx   r2                   ; r0 = env, r1 = the char*
 *
 * so `env` is a guest word holding a guest address, and that address is a table of guest addresses.
 * Three objects in guest memory, one of which (the table) is filled by a loop, and the *entries* are
 * not code at all: they are `AbiContext` slots, which the executor recognises before it translates,
 * so no page has to be executable for a host function to be callable.
 *
 * Two decisions worth stating because they were the tempting alternatives:
 *
 *   - **Every** table entry gets a slot, not just the implemented ones. An uninitialised entry would
 *     be a guest branch to address 0 — a fault with nothing to say about itself. A bound-but-refusing
 *     entry names the index, which is the difference between "this app needs GetStringRegion" and a
 *     crash in an unrelated place.
 *   - The indices are transcribed from a measurement, not from the JNI specification.
 *     `tests/corpus/jni-offsets.txt` is `offsetof` over the NDK's own `jni.h`, and `NewStringUTF`'s
 *     167 is independently confirmed by the `#0x29c` above (668 / 4). Those four reserved NULL slots at
 *     the head of the table are the reason counting function declarations gives 163 and the table says
 *     167 — an off-by-four that would have the guest calling `ReleaseStringChars` when it meant
 *     `NewStringUTF`, successfully, with nonsense results.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include "rb/abi/abi.h"
#include "rb/diag/audit.h"
#include "rb/mem/guest_memory.h"

#include <vector>

namespace rb::abi {

/// JNI function-table indices. See this file's header for how they were measured; `kNewStringUTF` is
/// the one with a second, independent confirmation from the fixture's own encoding.
namespace jni {
inline constexpr std::uint32_t kGetVersion = 4;
inline constexpr std::uint32_t kFindClass = 6;
inline constexpr std::uint32_t kExceptionOccurred = 15;
inline constexpr std::uint32_t kExceptionDescribe = 16;
inline constexpr std::uint32_t kExceptionClear = 17;
inline constexpr std::uint32_t kNewGlobalRef = 21;
inline constexpr std::uint32_t kDeleteGlobalRef = 22;
inline constexpr std::uint32_t kDeleteLocalRef = 23;
inline constexpr std::uint32_t kIsSameObject = 24;
inline constexpr std::uint32_t kNewLocalRef = 25;
inline constexpr std::uint32_t kGetObjectClass = 31;
inline constexpr std::uint32_t kGetMethodID = 33;
/// The instance-call families, each a `(variadic, V, A)` triple in that order — the standard table's
/// layout, confirmed at two measured points: KA3D's inline `_JNIEnv::CallObjectMethod` calls
/// `[table, #0x8c]` (35, the V form) and the offsets file has CallIntMethod at 49.
inline constexpr std::uint32_t kCallObjectMethod = 34;
inline constexpr std::uint32_t kCallBooleanMethod = 37;
inline constexpr std::uint32_t kCallIntMethod = 49;
inline constexpr std::uint32_t kCallVoidMethod = 61;
inline constexpr std::uint32_t kNewStringUTF = 167;
inline constexpr std::uint32_t kGetStringUTFLength = 168;
inline constexpr std::uint32_t kGetStringUTFChars = 169;
inline constexpr std::uint32_t kReleaseStringUTFChars = 170;
inline constexpr std::uint32_t kGetArrayLength = 171;
/// `(env, jsize length)` -> a zeroed host-side byte[]. Gumball's menu reaches it on the first tap.
inline constexpr std::uint32_t kNewByteArray = 176;
/// `(env, array, jboolean* isCopy)` -> a guest pointer to a copy of the bytes, and its release
/// `(env, array, elems, mode)`. Gumball 2013's jniInit reaches 184 on the first file it reads.
inline constexpr std::uint32_t kGetByteArrayElements = 184;
inline constexpr std::uint32_t kReleaseByteArrayElements = 192;
/// `(env, array, start, len, buf)` — the fifth argument is the first stacked word. KA3D's
/// `io::FileInputStream` reaches it as `[table, #0x320]` (800 / 4).
inline constexpr std::uint32_t kGetByteArrayRegion = 200;
/// `(env, array, start, len, const jbyte* buf)` — the copy the other way; `buf` is stacked too.
inline constexpr std::uint32_t kSetByteArrayRegion = 208;
inline constexpr std::uint32_t kExceptionCheck = 228;
inline constexpr std::uint32_t kGetObjectRefType = 232;

/// The table length: the highest index defined, plus one. Entries the guest can reach but this
/// environment does not implement are refused by index rather than left blank.
inline constexpr std::uint32_t kTableEntries = 233;

/// JNI 1.6, which is what ART's own `GetVersion` returns on the devices RetroBug targets.
/// `[UNVERIFIED]` beyond the SM-X710: a real `JNI_VERSION_1_6` response is assumed here, and a guest
/// that branches on the version would be the way that assumption gets tested.
inline constexpr std::uint32_t kVersion = 0x0001'0006u;

/// A `jstring` this environment makes is a guest address into its own arena, and the guest never
/// dereferences it as a struct — it only hands it back to another table entry. The layout is therefore
/// ours to choose, and the only rule is that it round-trips through `GetStringUTFChars`.
///
/// Two block kinds share one shape — `{magic, byte length, bytes…}`, word-aligned — told apart by the
/// magic: a `jstring` (`kStringMagic`) and a `GetStringUTFChars` copy of its bytes (`kCharsMagic`).
/// They are separate blocks because JNI gives them separate lifetimes: `ReleaseStringUTFChars` frees
/// the chars buffer, `DeleteLocalRef` frees the string, and either order is legal. One block with one
/// lifetime could not represent that — the revision that tried had `ReleaseStringUTFChars` free the
/// *jstring*, so the legal Release-then-Delete (or Release-then-use) sequence was refused as a stale
/// handle and faulted the whole guest call.
inline constexpr std::uint32_t kStringMagic = 0x5242'5374u;  ///< 'RBSt': a jstring block
inline constexpr std::uint32_t kCharsMagic = 0x5242'5363u;   ///< 'RBSc': a chars-copy block
/// 'RBSe': a `Get<T>ArrayElements` copy — guest bytes standing for a host-side array until released.
inline constexpr std::uint32_t kElementsMagic = 0x5242'5365u;
inline constexpr std::uint32_t kStringHeaderBytes = 8;       ///< magic, then the byte length

/// Handles for what the host class model hands out. None is ever a guest pointer to data: the guest
/// only passes them back to table entries, so they are tokens past the end of the guest window (the
/// launcher's is 64 MiB) and a guest that dereferences one faults by name instead of reading something
/// that looks like an object. `DefineHostClass` refuses on a window large enough to reach
/// `kClassRefBase`, where that stops being true. Arrays get a serial that is never
/// reused, so a deleted array's handle stays stale rather than becoming the next array.
inline constexpr std::uint32_t kClassRefBase = 0xF000'0000u;
inline constexpr std::uint32_t kMethodIdBase = 0xF100'0000u;
inline constexpr std::uint32_t kArrayRefBase = 0xF200'0000u;
inline constexpr std::uint32_t kHandleSpan = 0x0100'0000u;  ///< each kind's range, in bytes
}  // namespace jni

/**
 * One guest instance's `JNIEnv`: the table, the `env` pointer, and the arena its strings live in.
 *
 * Per-instance rather than shared, because a `jstring` handle is a pointer into *this* arena and C08's
 * rule is that no isolation property is ever global. Two instances sharing one arena would let one
 * app's handle be dereferenced by another's, which is a memory-safety bug wearing an ABI hat.
 */
class JniEnv {
 public:
  /// Maps and owns guest memory from `where`: `jni::kTableEntries` words of table, one word for `env`,
  /// then `arena_bytes` of string storage. `where` must be page-aligned; the total is rounded up to
  /// whole guest pages.
  JniEnv(mem::GuestMemory& memory, AbiContext& abi, GuestAddr where, unsigned arena_bytes,
         std::string* err);

  /// The value to place in r0 when calling a guest JNI method.
  [[nodiscard]] GuestAddr env() const { return env_at_; }
  [[nodiscard]] bool ok() const { return ok_; }
  [[nodiscard]] const std::string& error() const { return err_; }

  /// Diagnostics that make the bridge's other half assertable: the last string the host *received*,
  /// read out of guest memory. A test that only checked the return value could pass with an empty
  /// buffer, so this is what proves the bytes crossed.
  [[nodiscard]] const std::string& last_string() const { return last_string_; }
  [[nodiscard]] unsigned new_string_utf_calls() const { return utf_calls_; }
  [[nodiscard]] unsigned strings_made() const { return strings_made_; }
  [[nodiscard]] unsigned strings_freed() const { return strings_freed_; }
  [[nodiscard]] unsigned chars_copies_made() const { return chars_made_; }
  [[nodiscard]] unsigned chars_copies_released() const { return chars_released_; }
  /// Bytes of the arena the bump pointer has claimed. A guest that completes JNI's lifecycle — every
  /// `jstring` deleted, every chars copy released — keeps this flat however many strings it makes; a
  /// guest that leaks either grows it until the refusal below fires. It is the difference
  /// `a_completed_lifecycle_does_not_grow_the_arena` measures, and the reason it exists as an accessor
  /// rather than an inference from `strings_made()`.
  [[nodiscard]] unsigned arena_used() const { return arena_next_ - arena_at_; }
  /// Bytes returned by releases and not yet handed out again.
  [[nodiscard]] unsigned free_bytes() const {
    unsigned n = 0;
    for (const FreeBlock& b : free_) n += b.bytes;
    return n;
  }
  /// The handle of the most recent string this environment made. A `jstring` is opaque to the guest,
  /// so a test that wants to hand one back has to get the same value the guest got — and reconstructing
  /// it from the arena base would be assuming the allocation order rather than observing it.
  [[nodiscard]] GuestAddr last_handle() const { return last_handle_; }

  /// Walks the free list and the allocation record and reports named invariants into the self-audit:
  /// the list sorted, coalesced, in-bounds and non-overlapping, every live block wholly inside the
  /// arena, and the accounting adding up. See `rb/diag/audit.h` for the rules this follows. Const and
  /// side-effect-free.
  void Audit(diag::Auditor& audit) const;

  // ---- the host class model (C09, the part a game's own callbacks need)
  //
  // There is no JavaVM, so a Java object here is whatever the host says it is: a guest value the
  // launcher *binds* to a class it defines, with methods implemented on the host. `GetObjectClass`,
  // `GetMethodID` and the `Call<T>Method{,V,A}` families answer for exactly those, and refuse by name
  // for everything else — an unbound object, a method nobody defined — so the next thing a game needs
  // arrives as a line in the report rather than as a NULL it would dereference.

  /// A host-implemented Java method. `args` holds the call's arguments decoded per the signature: a
  /// reference as its handle, Z/B/C/S/I as the value, F as its float bits, J/D as 64 bits. `ret` takes
  /// the result in its low bits (an object as a handle this environment made, or 0 for null). False,
  /// with `err` set, refuses the guest call by name.
  using HostMethod = std::function<bool(JniEnv& env, const std::vector<std::uint64_t>& args,
                                        std::uint64_t& ret, std::string* err)>;

  /// Defines a class by its JNI name (`com/rovio/ka3d/MyRenderer`) and returns its handle, or 0 when
  /// the window is large enough that handles could be guest addresses (see `kClassRefBase`).
  GuestAddr DefineHostClass(const std::string& jni_name, std::string* err = nullptr);
  /// Adds `name` with JNI signature `sig` (`(Ljava/lang/String;)[B`) to `cls`. False on an unknown class,
  /// a duplicate, or a signature this decoder cannot read.
  bool DefineHostMethod(GuestAddr cls, const std::string& name, const std::string& sig, HostMethod fn,
                        std::string* err = nullptr);
  /// Makes the guest value `obj` an instance of `cls`. The value is the guest's own (the launcher's
  /// stand-in page), so it is only ever compared, never read through.
  bool BindHostObject(GuestAddr obj, GuestAddr cls, std::string* err = nullptr);

  /// Makes a `byte[]` whose bytes live on the host and returns its handle; the guest reaches them only
  /// through `GetArrayLength`/`GetByteArrayRegion`, which copy. 0 when the handle space is exhausted.
  GuestAddr NewHostByteArray(std::vector<std::uint8_t> bytes);
  /// The text of a live `jstring` this environment made — what a host method reads its String
  /// arguments with. Same validation as `GetStringUTFChars`.
  bool StringText(GuestAddr jstring, std::string& out, std::string* err) const;

  /// One defined method and how often the guest reached it, for the report.
  struct HostMethodUse {
    std::string cls, name, sig;
    unsigned calls = 0;
    unsigned refused = 0;
  };
  [[nodiscard]] std::vector<HostMethodUse> host_method_uses() const;
  /// The guest's registers at the `Call<T>Method` slot, valid only while a host method runs (null
  /// otherwise) on the calling host thread — what a host method needs to say *who* called it, e.g. a stack scan for return
  /// addresses when the arguments alone do not explain the request.
  [[nodiscard]] const exec::MachineState* calling_state() const;
  /// The bytes of a live `byte[]` this environment made, or null — what a host method reads a `[B`
  /// argument through.
  [[nodiscard]] const std::vector<std::uint8_t>* HostArray(GuestAddr handle) const {
    const auto it = arrays_.find(handle);
    return it == arrays_.end() ? nullptr : &it->second;
  }
  [[nodiscard]] unsigned elements_made() const { return elements_made_; }
  [[nodiscard]] unsigned elements_released() const { return elements_released_; }
  [[nodiscard]] unsigned arrays_made() const { return arrays_made_; }
  [[nodiscard]] unsigned arrays_freed() const { return arrays_freed_; }
  [[nodiscard]] std::size_t arrays_live() const { return arrays_.size(); }

 private:
  struct HostClassRec {
    std::string name;
  };
  struct HostMethodRec {
    GuestAddr cls = 0;
    std::string name, sig;
    std::vector<char> params;  ///< one kind letter per parameter: Z B C S I F J D, L for any reference
    char ret = 'V';            ///< the same alphabet, plus V
    HostMethod fn;
    unsigned calls = 0;
    unsigned refused = 0;
  };
  /// How the arguments after `(env, obj, mid)` reach a `Call<T>Method` slot.
  enum class ArgForm { kVariadic, kVaList, kJvalues };
  /// The body every `Call<T>Method{,V,A}` slot shares: validates the object and method, decodes the
  /// arguments for `form`, checks the method's return kind is one `family` (a JNI kind letter, with L
  /// meaning any reference) may return, and calls it.
  bool CallHost(exec::MachineState& s, char family, ArgForm form, std::string* err);
  /// Which class a guest value is an instance of, or 0 for an unbound value.
  [[nodiscard]] GuestAddr ClassOf(GuestAddr obj) const;
  [[nodiscard]] static bool IsClassRef(GuestAddr h) {
    return h >= jni::kClassRefBase && h < jni::kClassRefBase + jni::kHandleSpan;
  }
  [[nodiscard]] static bool IsArrayRef(GuestAddr h) {
    return h >= jni::kArrayRefBase && h < jni::kArrayRefBase + jni::kHandleSpan;
  }

  std::vector<HostClassRec> classes_;    ///< handle = kClassRefBase + 4 * index
  std::vector<HostMethodRec> methods_;   ///< id = kMethodIdBase + 4 * index
  std::map<GuestAddr, GuestAddr> bound_;  ///< guest value -> class handle
  std::map<GuestAddr, std::vector<std::uint8_t>> arrays_;  ///< live arrays by handle
  /// Live `GetByteArrayElements` blocks: block handle -> the array it copies. The release checks both
  /// halves, so elements handed back against the wrong array are refused by name.
  std::map<GuestAddr, GuestAddr> elements_;
  unsigned elements_made_ = 0;
  unsigned elements_released_ = 0;
  std::uint32_t next_array_ = 0;
  unsigned arrays_made_ = 0;
  unsigned arrays_freed_ = 0;

  /// The `NewStringUTF` implementation: copies the guest's NUL-terminated bytes and returns a handle.
  GuestAddr MakeString(GuestAddr chars_at);

  /// The `GetStringUTFChars` implementation: allocates a `kCharsMagic` block holding a copy of
  /// `string_handle`'s bytes and returns the block's handle. A copy — not a pointer into the string
  /// block — so a later `DeleteLocalRef` of the string cannot pull the bytes out from under the guest,
  /// and so the `isCopy=1` the slot writes is the truth rather than a claim.
  GuestAddr MakeCharsCopy(GuestAddr string_handle, std::uint32_t length);

  /// One block the free list can hand back, in arena coordinates. Kept sorted by address and coalesced,
  /// because a list of adjacent slivers can refuse an allocation the space as a whole would hold.
  struct FreeBlock {
    GuestAddr at = 0;
    unsigned bytes = 0;
  };

  /// What this environment knows about a block it allocated — the authority for size and kind. The
  /// header words in guest memory are guest-writable, so they are a *claim*: trusting the length word
  /// let an in-range scribble make a release insert a free block overlapping its neighbour (every
  /// range check passed; only the record can catch that). Reads and releases compare the header against
  /// this record and refuse on disagreement instead of reading it as truth.
  struct LiveBlock {
    unsigned total = 0;        ///< whole block, header included, word-aligned
    std::uint32_t length = 0;  ///< payload bytes the block was allocated to hold
    std::uint32_t magic = 0;   ///< which kind it is: `kStringMagic` or `kCharsMagic`
  };

  /// First fit over the free list, then the bump pointer. `total` must already be word-aligned. Zero on
  /// exhaustion, with `err_` naming the arena, the wanted size and the free list's state.
  GuestAddr AllocBlock(unsigned total);

  /// Inserts `[handle, handle + bytes)` into the free list, coalescing and rolling the bump pointer
  /// back where it can. No validation, no counters: the callers are the release path (after it has
  /// checked everything) and the make paths' failure rollbacks — a block whose header never landed has
  /// no record to hand it back through, and a header that failed to write cannot be re-read.
  void ReturnSpace(GuestAddr handle, unsigned bytes);

  /// The make paths' rollback for a *registered* block: erases the record, kills the header, returns
  /// the space. Deliberately uncounted — a copy that failed to be made must not appear in
  /// `chars_copies_released()`.
  void ReleaseInternal(GuestAddr handle);

  /// Returns `handle`'s block to the free list, requiring the record's kind to be `want_magic` so a
  /// chars copy and a `jstring` cannot be freed through each other's slot. False, with a named reason,
  /// when the handle has no record (a stale handle, a foreign pointer, a double release), is the wrong
  /// kind, or whose header disagrees with the record. `handle == 0` is legal and means nothing to do,
  /// which is what JNI says about `DeleteLocalRef(NULL)`; the release slot pre-checks its own null
  /// before reaching here, because a null *chars* argument is not legal. `what` names the expected
  /// kind in the refusal.
  bool ReleaseBlock(GuestAddr handle, std::uint32_t want_magic, const char* what, std::string* err);

  /// Validates a handle the guest handed back: inside this instance's arena, holding a live block of
  /// this environment's per its allocation record, and carrying a header that agrees with that record.
  /// A `jstring` is opaque to the guest, so anything arriving here is either one of ours, a corrupted
  /// copy, or a guest pointer to something else entirely — and dereferencing the last of those as a
  /// string would read arbitrary guest memory and call it text. The record, not the guest-writable
  /// header, is what decides the payload length reported.
  [[nodiscard]] bool CheckHandle(GuestAddr handle, std::uint32_t& length_out, std::string* err) const;

  mem::GuestMemory& memory_;
  AbiContext& abi_;
  GuestAddr table_at_ = 0;
  GuestAddr env_at_ = 0;
  GuestAddr arena_at_ = 0;
  GuestAddr arena_next_ = 0;
  GuestAddr arena_end_ = 0;
  /// Sorted, coalesced blocks freed by `ReleaseStringUTFChars`/`DeleteLocalRef` and available to the
  /// next `MakeString` or `MakeCharsCopy`. Empty for a guest that never releases, which is exactly the
  /// behaviour the original bump-only arena replaced.
  std::vector<FreeBlock> free_;
  /// Every live block this environment allocated: `handle -> {total, length, kind}`. See `LiveBlock`.
  std::map<GuestAddr, LiveBlock> live_;
  bool ok_ = false;
  std::string err_;
  std::string last_string_;
  unsigned utf_calls_ = 0;
  unsigned strings_made_ = 0;
  unsigned strings_freed_ = 0;
  unsigned chars_made_ = 0;
  unsigned chars_released_ = 0;
  GuestAddr last_handle_ = 0;
};

}  // namespace rb::abi
