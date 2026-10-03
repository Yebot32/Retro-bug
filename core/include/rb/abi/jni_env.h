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
#include <string>

#include "rb/abi/abi.h"
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
inline constexpr std::uint32_t kDeleteLocalRef = 23;
inline constexpr std::uint32_t kGetObjectClass = 31;
inline constexpr std::uint32_t kNewStringUTF = 167;
inline constexpr std::uint32_t kGetStringUTFLength = 168;
inline constexpr std::uint32_t kGetStringUTFChars = 169;
inline constexpr std::uint32_t kReleaseStringUTFChars = 170;
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
inline constexpr std::uint32_t kStringMagic = 0x5242'5374u;  ///< 'RBS t'
inline constexpr std::uint32_t kStringHeaderBytes = 8;       ///< magic, then the byte length
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
  /// Bytes of the arena the bump pointer has claimed. A guest that releases keeps this flat however many
  /// strings it makes; a guest that does not grows it until the refusal below fires. It is the difference
  /// `a_released_string_does_not_grow_the_arena` measures, and the reason it exists as an accessor rather
  /// than an inference from `strings_made()`.
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

 private:
  /// The `NewStringUTF` implementation: copies the guest's NUL-terminated bytes and returns a handle.
  GuestAddr MakeString(GuestAddr chars_at);

  /// One block a release handed back, in arena coordinates. Kept sorted by address and coalesced, because
  /// a list of adjacent slivers can refuse an allocation the space as a whole would hold.
  struct FreeBlock {
    GuestAddr at = 0;
    unsigned bytes = 0;
  };

  /// Returns `handle`'s block to the free list — the local-reference lifecycle's other half. False, with a
  /// named reason, when the handle is not a *live* string of this instance's: a stale handle, a foreign
  /// pointer, or a double release. `handle == 0` is legal and means nothing to do, which is what JNI says
  /// about `DeleteLocalRef(NULL)`.
  bool Free(GuestAddr handle, std::string* err);

  /// Validates a handle the guest handed back: inside this instance's arena, and carrying the magic
  /// this environment writes. A `jstring` is opaque to the guest, so anything arriving here is either
  /// one of ours, a corrupted copy, or a guest pointer to something else entirely — and dereferencing
  /// the last of those as a string would read arbitrary guest memory and call it text.
  [[nodiscard]] bool CheckHandle(GuestAddr handle, std::uint32_t& length_out, std::string* err) const;

  mem::GuestMemory& memory_;
  AbiContext& abi_;
  GuestAddr table_at_ = 0;
  GuestAddr env_at_ = 0;
  GuestAddr arena_at_ = 0;
  GuestAddr arena_next_ = 0;
  GuestAddr arena_end_ = 0;
  /// Sorted, coalesced blocks freed by `ReleaseStringUTFChars`/`DeleteLocalRef` and available to the next
  /// `MakeString`. Empty for a guest that never releases, which is exactly the behaviour this replaced.
  std::vector<FreeBlock> free_;
  bool ok_ = false;
  std::string err_;
  std::string last_string_;
  unsigned utf_calls_ = 0;
  unsigned strings_made_ = 0;
  unsigned strings_freed_ = 0;
  GuestAddr last_handle_ = 0;
};

}  // namespace rb::abi
