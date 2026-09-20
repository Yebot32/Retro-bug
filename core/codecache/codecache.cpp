/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 */
#include "rb/codecache/codecache.h"

#include <algorithm>
#include <cstdio>

namespace rb::codecache {
namespace {

std::string Hex(std::uint64_t v) {
  char buf[19];
  std::snprintf(buf, sizeof buf, "%llx", static_cast<unsigned long long>(v));
  return buf;
}

}  // namespace

const char* Name(Kind k) {
  switch (k) {
    case Kind::kAnonRwx: return "AnonRwx";
    case Kind::kMemfdDual: return "MemfdDualAlias";
    case Kind::kInterpreted: return "Interpreted";
    case Kind::kCount: break;
  }
  return "unknown";
}

std::string BlockKey::Describe() const {
  return "inst" + std::to_string(instance) + "/img" + std::to_string(image) + "@0x" + Hex(pc) +
         (mode == isa::Mode::Thumb ? "t" : "a");
}

Cache::Cache(std::unique_ptr<Allocator> allocator, Kind asked, Limits limits)
    : allocator_(std::move(allocator)), asked_(asked), limits_(limits) {}

std::size_t Cache::bytes_in_use(InstanceId instance) const {
  std::size_t total = 0;
  for (const auto& [key, entry] : entries_) {
    if (key.instance == instance) total += entry.used.bytes;
  }
  return total;
}

std::size_t Cache::blocks_of(InstanceId instance) const {
  std::size_t n = 0;
  for (const auto& [key, entry] : entries_) {
    (void)entry;
    if (key.instance == instance) ++n;
  }
  return n;
}

std::vector<std::pair<InstanceId, unsigned>> Cache::instances_touched() const { return touched_; }

void Cache::NoteTouch(InstanceId instance) {
  for (auto& [id, count] : touched_) {
    if (id == instance) {
      ++count;
      return;
    }
  }
  touched_.emplace_back(instance, 1u);
}

Slot Cache::Acquire(const BlockKey& key, std::size_t bytes) {
  Slot s{};
  s.key = key;
  NoteTouch(key.instance);

  // The interpreted backend has nothing to reserve. This is the branch iOS and any OEM where U-13 came
  // back "no" runs on, so it is a success with a different shape rather than a failure — a cache that
  // returned `kNoRoom` here would send a caller into an eviction loop over an empty cache.
  if (allocator_->kind() == Kind::kInterpreted) {
    s.state = Slot::State::kInterpreted;
    return s;
  }
  if (entries_.count(key) != 0) {
    s.state = Slot::State::kDuplicate;
    return s;
  }

  HostRange reserved{};
  std::string err;
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (bytes_in_use(key.instance) + bytes > limits_.bytes_per_instance ||
        blocks_of(key.instance) >= limits_.blocks_per_instance) {
      // Room is not there before asking, so this evicts rather than failing and then evicting: the
      // allocator's own "no room" answer is the *second* chance, for a backend whose ceiling is not the
      // one configured here.
      if (!EvictOne(key.instance) && attempt == 0) {
        s.state = Slot::State::kNoRoom;
        return s;
      }
    }
    if (allocator_->Reserve(bytes, &reserved, &err)) break;
    if (!EvictOne(key.instance)) {
      s.state = Slot::State::kNoRoom;
      return s;
    }
    reserved = HostRange{};
  }
  if (reserved.empty()) {
    s.state = Slot::State::kNoRoom;
    return s;
  }
  // `used` stays empty until `Commit`: an entry that has space but no executable code must not be
  // findable (a lookup that returns a block which is still writable is the W^X rule broken through the
  // front door) and must not be an eviction victim (releasing it would pull memory out from under a back
  // end that is mid-write). Both of those had a test before this line got it right, which is the only
  // reason the mistake was visible at all.
  entries_.emplace(key, Entry{reserved, HostRange{}, ++clock_});
  order_.push_back(key);
  s.state = Slot::State::kSpace;
  s.range = reserved;
  return s;
}

bool Cache::Commit(const Slot& slot, std::size_t used_bytes, std::string* err) {
  if (slot.state != Slot::State::kSpace) {
    if (err) *err = "Commit on a slot that is not space (" + slot.key.Describe() + ")";
    return false;
  }
  auto found = entries_.find(slot.key);
  if (found == entries_.end()) {
    if (err) *err = "Commit of an evicted block: " + slot.key.Describe();
    return false;
  }
  if (used_bytes == 0 || used_bytes > found->second.reserved.bytes) {
    // Refused, not clamped. A back end that emitted more than it was given has overwritten something it
    // does not own, and an accountancy that quietly agrees with the number is how that becomes invisible.
    if (err) {
      *err = "block wants " + std::to_string(used_bytes) + " bytes of a " +
             std::to_string(found->second.reserved.bytes) + " byte reservation: " +
             slot.key.Describe();
    }
    return false;
  }
  HostRange used{found->second.reserved.at, used_bytes};
  if (!allocator_->Commit(used, err)) {
    // Leave the entry out of the cache rather than holding a range that is half-written and mapped
    // writable: the caller has nothing to look up, and a block that is not executable must never be
    // found by a lookup.
    allocator_->Release(found->second.reserved);
    const BlockKey key = slot.key;
    entries_.erase(found);
    order_.erase(std::remove(order_.begin(), order_.end(), key), order_.end());
    if (err && err->empty()) *err = "the allocator refused to commit " + key.Describe();
    return false;
  }
  found->second.used = used;
  return true;
}

bool Cache::Find(const BlockKey& key, HostRange* out) {
  auto found = entries_.find(key);
  if (found == entries_.end() || found->second.used.empty()) return false;
  // Touching the entry on a lookup is what makes eviction least-*recently*-used rather than
  // first-in-first-out: a hot loop's block is found thousands of times, and evicting it to make room for
  // a cold one is the behaviour that makes a small cache look broken on real code.
  found->second.use = ++clock_;
  if (out != nullptr) *out = found->second.used;
  return true;
}

exec::GuestAddr Cache::Forget(const BlockKey& key) {
  auto found = entries_.find(key);
  if (found == entries_.end()) return 0;
  allocator_->Release(found->second.reserved);
  entries_.erase(found);
  order_.erase(std::remove(order_.begin(), order_.end(), key), order_.end());
  return key.pc;
}

std::vector<exec::GuestAddr> Cache::FlushInstance(InstanceId instance) {
  std::vector<exec::GuestAddr> out;
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (it->first.instance != instance) {
      ++it;  // another app's code: not this call's business, at any cost to the free space
      continue;
    }
    allocator_->Release(it->second.reserved);
    out.push_back(it->first.pc);
    it = entries_.erase(it);
  }
  order_.erase(std::remove_if(order_.begin(), order_.end(),
                              [&](const BlockKey& k) { return k.instance != instance; }),
               order_.end());
  return out;
}

bool Cache::EvictOne(InstanceId instance) {
  auto best = entries_.end();
  for (auto it = entries_.begin(); it != entries_.end(); ++it) {
    if (it->first.instance != instance) continue;
    // `used.empty()` means reserved and not yet committed: a block some other thread's back end is
    // writing right now. Evicting it would release memory underneath an emitter that is about to
    // `Commit` into it, and the entry would then be gone with the write already landed — a use-after-free
    // whose symptom is one app executing another app's code.
    if (it->second.used.empty()) continue;
    if (best == entries_.end() || it->second.use < best->second.use) best = it;
  }
  if (best == entries_.end()) return false;
  allocator_->Release(best->second.reserved);
  const BlockKey key = best->first;
  entries_.erase(best);
  order_.erase(std::remove(order_.begin(), order_.end(), key), order_.end());
  ++evictions_;
  return true;
}

std::vector<InstanceId> Cache::Instances() const {
  std::vector<InstanceId> out;
  for (const auto& [id, count] : touched_) {
    (void)count;
    out.push_back(id);
  }
  return out;
}

void Cache::Echo(diag::Recorder& rec, const std::string& stage, const std::string& evidence) const {
  diag::Entry e{};
  e.field = diag::Field::kCodeCacheBackend;
  e.stage = stage;
  e.requested = Name(asked_);
  e.active = Name(allocator_->kind());
  // `inert` is the right word for "asked for a code cache, got the interpreter": the request was made
  // and not honoured, which is the case the recorder was built for. Equal sides are `active`.
  e.outcome = asked_ == allocator_->kind() ? diag::Outcome::kActive : diag::Outcome::kInert;
  e.evidence = e.outcome == diag::Outcome::kActive
                   ? evidence
                   : "downgraded from " + std::string{Name(asked_)} + ": " + evidence;
  rec.Record(e);
}

}  // namespace rb::codecache
