/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * C04 `rb::codecache` — who owns which compiled block, and what happens when there is no room left.
 *
 * Two decisions shape this file, and both come from somewhere specific.
 *
 * **Every key carries `instance_id`.** `docs/research/05` §2.9 names the hole directly: a code cache
 * looked up by guest PC alone aliases one app's compiled block onto another's, which on T1 is not a
 * performance bug but a cross-instance one — the isolated-per-app property the brief makes absolute
 * ("no isolation property is ever global or shared across apps") would be broken by a hash table that
 * forgot a field. So `BlockKey` is a struct with the instance in it, and the test that two instances at
 * the same `pc` get two slots is the cheapest way to make forgetting it expensive. `image_id` and the
 * mode are in the key for the same kind of reason: two modules can map the same guest address, and an
 * ARM/Thumb distinction that is not in the key is how a block of Thumb bytes gets served to an ARM-mode
 * fetch — which does not fault, it just decodes something else.
 *
 * **This file decides what to drop; it does not touch an instruction cache.** The host-cache discipline
 * (`dc civac`, `ic iallu`, `isb`) belongs to whichever back end emitted the code — today that is
 * Dynarmic, whose `Jit::InvalidateCacheRange` is what `JitExecutor::Invalidate` calls (`30` §Step 9).
 * So `FlushInstance` and `Forget` *return the guest ranges* the caller has to invalidate rather than
 * invalidating anything: policy here, maintenance there, and the split is visible in the signature
 * instead of being a paragraph of intent.
 *
 * The memory itself is behind an injected `Allocator` because what kind of memory it is — anonymous
 * `RWX`, a `memfd` with two mappings, or none at all — is a platform answer the launcher obtains by
 * probing (AD-16, U-13), not something engine code should assume. `kInterpreted` is a real backend
 * rather than a null pointer: Tier I has to run with no cache at all on iOS and on any OEM where the
 * probe says no, and a cache that *fails* in that configuration would be a fallback that cannot be
 * taken. Reserve is writable and Commit makes it executable-and-not-writable, never both: that is the
 * one W^X statement this component can make in portable code, and the test asserts the transition.
 *
 * See docs/20 §C04.
 */
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "rb/diag/diag.h"
#include "rb/exec/machine_state.h"
#include "rb/isa/front_end.h"

namespace rb::codecache {

using InstanceId = std::uint32_t;
using ImageId = std::uint32_t;

/// Which kind of executable memory the platform probe settled on. Named here rather than inferred from
/// an allocator's behaviour because the report has to say which one was *asked* for as well as which one
/// is in use, and a silent downgrade is the failure this component's C17 field exists to make visible.
enum class Kind : std::uint8_t {
  kAnonRwx,     ///< anonymous mapping, executable: the Android path U-13 measured as available.
  kMemfdDual,   ///< a memfd with a writable and an executable mapping over the same pages.
  kInterpreted, ///< no host code at all: Tier I, iOS, or an OEM where the probe said no.
  kCount,
};

[[nodiscard]] const char* Name(Kind);

/// The identity of one compiled block. Four fields, all of them load-bearing — see the header.
struct BlockKey {
  InstanceId instance = 0;
  ImageId image = 0;
  exec::GuestAddr pc = 0;
  isa::Mode mode = isa::Mode::Thumb;

  [[nodiscard]] bool operator==(const BlockKey&) const = default;
  [[nodiscard]] bool operator<(const BlockKey& o) const {
    // The mode goes through an `int` because `std::tie` binds references and a cast is a temporary; the
    // comparison is still by that exact four-field order, which is what the equality below relies on.
    const int m = static_cast<int>(mode), om = static_cast<int>(o.mode);
    return std::tie(instance, image, pc, m) < std::tie(o.instance, o.image, o.pc, om);
  }
  [[nodiscard]] std::string Describe() const;
};

/// A piece of the cache's address space, as an offset into whatever the allocator owns. Core never
/// dereferences it; it is a token the back end hands to its own code emitter.
struct HostRange {
  std::uintptr_t at = 0;
  std::size_t bytes = 0;
  [[nodiscard]] bool empty() const { return at == 0 || bytes == 0; }
  [[nodiscard]] bool operator==(const HostRange&) const = default;
  /// Ordered so a test or a report can put ranges in a set. Not used by the policy itself, which looks
  /// keys up by `BlockKey` and never by address.
  [[nodiscard]] bool operator<(const HostRange& o) const {
    return std::tie(at, bytes) < std::tie(o.at, o.bytes);
  }
};

/// The platform's side of the seam. Implementations live with the platform (M1's Android and iOS glue);
/// this repository contains the policy and a test implementation, because the policy is what needs the
/// tests and the policy must not be able to reach `mmap`.
class Allocator {
 public:
  virtual ~Allocator() = default;
  [[nodiscard]] virtual Kind kind() const = 0;

  /// Space to emit into. Writable, and not executable: a block is never both once it is committed.
  /// False means "no room", which is what turns the cache full and starts eviction.
  virtual bool Reserve(std::size_t bytes, HostRange* out, std::string* err) = 0;
  /// Hand back the tail `used` describes and make it executable. A back end that emitted 40 bytes of
  /// the 256 it was given must say so, or the cache accountancy silently lies.
  virtual bool Commit(const HostRange& used, std::string* err) = 0;
  virtual void Release(const HostRange& whole) = 0;
};

/// What one `Acquire` produced. `kNoRoom` and `kDuplicate` are kept apart on purpose: the first means
/// "evict something and try again", the second means "you already have this block", and a caller that saw
/// them as one answer would evict a live block to make room for a copy of itself. `kNoRoom` is not an
/// error to report to a user either way.
struct Slot {
  enum class State { kSpace, kInterpreted, kDuplicate, kNoRoom } state = State::kNoRoom;
  BlockKey key;
  HostRange range;  ///< valid when `state == kSpace`
};

/// Per-instance bounds, because a cache shared without accounting is one app evicting another — and the
/// other direction, an app that compiles a lot, is a memory problem that has to land somewhere.
struct Limits {
  std::size_t bytes_per_instance = 2u << 20;
  std::size_t blocks_per_instance = 4096;
};

class Cache {
 public:
  /// `asked` is what the launcher wanted, which may differ from `allocator->kind()` after a probe
  /// downgrade; both are reported, because the difference is the finding.
  Cache(std::unique_ptr<Allocator> allocator, Kind asked, Limits limits = {});

  [[nodiscard]] Kind backend() const { return allocator_->kind(); }
  [[nodiscard]] Kind requested() const { return asked_; }
  [[nodiscard]] const Limits& limits() const { return limits_; }

  /// Claim space for roughly `bytes` of compiled code. A key already in the cache gives back
  /// `kDuplicate` rather than a second piece of space, so the caller cannot mistake "already compiled"
  /// for "no room" and evict the block it is asking about.
  Slot Acquire(const BlockKey& key, std::size_t bytes);

  /// Finish a block: the first `used_bytes` of what the slot was reserved become the block. Returns
  /// false, with a reason, when the slot was never valid or the range does not fit what was reserved — a
  /// `used.bytes` larger than what was handed out is a back-end bug, and silently clamping it is how an
  /// accountancy lie starts. The committed range is readable from `Find`, which is also how a caller knows
  /// the block is there at all.
  bool Commit(const Slot&, std::size_t used_bytes, std::string* err);

  /// The entry for a key, if any. Not `const`, because a lookup refreshes the block's recency, and an
  /// eviction policy that ignored lookups would evict the block a hot loop is sitting in.
  [[nodiscard]] bool Find(const BlockKey&, HostRange* out);

  /// Drop one block — the AD-7 path: a guest wrote over it. Returns the guest range to invalidate, or
  /// zeros when there was nothing there, which is a real answer and not a failure.
  exec::GuestAddr Forget(const BlockKey&);

  /// Drop every block of one instance. `docs/research/05` §2.9's hole is closed here as well as in the
  /// key: this cannot touch another instance's entries, and the test says so by looking.
  std::vector<exec::GuestAddr> FlushInstance(InstanceId);

  /// Make room by evicting this instance's least-recently-used block. Only this instance's blocks are
  /// candidates: an app that compiled too much must not be able to slow another one down.
  bool EvictOne(InstanceId);

  [[nodiscard]] std::size_t blocks() const { return entries_.size(); }
  [[nodiscard]] std::size_t bytes_in_use(InstanceId) const;
  [[nodiscard]] std::size_t blocks_of(InstanceId) const;
  [[nodiscard]] unsigned evictions() const { return evictions_; }
  /// Blocks asked for per instance, in first-seen order — the same shape as C07's tally, and for the same
  /// reason: "the cache filled up" is only actionable with a number attached to it.
  [[nodiscard]] std::vector<std::pair<InstanceId, unsigned>> instances_touched() const;
  /// Which instances have asked for space, in first-seen order — the set `FlushInstance` has to be called
  /// for, which is why it is reachable rather than something a caller keeps alongside.
  [[nodiscard]] std::vector<InstanceId> Instances() const;

  /// C17's `code-cache-backend` echo: what was asked for, what is in use, and whether the difference is
  /// a downgrade. Recorded as `inert` when a non-interpreted backend was asked for and something weaker
  /// is active, because that is precisely the substitution the recorder exists to make loud.
  void Echo(diag::Recorder&, const std::string& stage, const std::string& evidence) const;

 private:
  struct Entry {
    HostRange reserved;  ///< what the allocator handed out, and what `Release` takes back
    HostRange used;      ///< the committed prefix of it, which is what is accounted
    std::uint64_t use = 0;  ///< monotonic stamp, for eviction order
  };

  void NoteTouch(InstanceId instance);

  std::unique_ptr<Allocator> allocator_;
  Kind asked_;
  Limits limits_;
  std::map<BlockKey, Entry> entries_;
  std::vector<BlockKey> order_;  ///< insertion order, so eviction is deterministic and testable
  std::uint64_t clock_ = 0;
  unsigned evictions_ = 0;
  std::vector<std::pair<InstanceId, unsigned>> touched_;
};

}  // namespace rb::codecache
