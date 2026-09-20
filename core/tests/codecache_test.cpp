/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * C04's policy, tested against an allocator that records everything instead of one that calls mmap. That
 * is not a shortcut: the whole content of this component is *which block, of which app, gets evicted
 * when*, and the memory underneath is the platform's business (AD-16) — `Reserve`/`Commit`/`Release` are
 * exactly the three things an Android or iOS allocator will have to do, so a fake that checks their
 * ordering is checking the real contract.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rb/codecache/codecache.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace rb;
namespace cc = rb::codecache;
namespace exec = rb::exec;
namespace isa = rb::isa;
namespace diag = rb::diag;

int failures = 0;
int checks = 0;

void fail(const char* what, int line, const std::string& detail = {}) {
  ++failures;
  std::printf("FAIL  %s  (line %d)%s%s\n", what, line, detail.empty() ? "" : "  ", detail.c_str());
}

#define CHECK_CTX(cond, detail)                      \
  do {                                               \
    ++checks;                                        \
    if (!(cond)) fail(#cond, __LINE__, detail);       \
  } while (0)
#define CHECK(cond) CHECK_CTX(cond, {})

cc::BlockKey Key(std::uint32_t instance, std::uint32_t image, exec::GuestAddr pc,
                 isa::Mode mode = isa::Mode::Thumb) {
  return cc::BlockKey{instance, image, pc, mode};
}

/// The contract every platform backend has to satisfy, enforced here rather than documented: a range is
/// reserved writable, committed at most as large as it was reserved, and released exactly once.
class FakeAllocator : public cc::Allocator {
 public:
  explicit FakeAllocator(cc::Kind kind = cc::Kind::kAnonRwx, std::size_t capacity = 1u << 20)
      : kind_(kind), capacity_(capacity) {}

  cc::Kind kind() const override { return kind_; }

  bool Reserve(std::size_t bytes, cc::HostRange* out, std::string* err) override {
    ++reserves;
    if (bytes == 0 || bytes > capacity_ - used_) {
      if (err) *err = "the allocator is full";
      return false;
    }
    used_ += bytes;
    *out = cc::HostRange{next_++, bytes};  // `at` is a token, and 0 is not a valid one
    open_.insert(*out);
    return true;
  }

  bool Commit(const cc::HostRange& used, std::string* err) override {
    // Matched on the reservation that *contains* it, because `Commit` is deliberately called with the
    // prefix the back end filled in, which is smaller than what was handed out. Comparing the two for
    // equality made every commit fail with "never reserved" and then — because a failed commit removes
    // the entry, which is the right behaviour — emptied the cache under the first test, whose next
    // statement indexed a vector it had just asserted was non-empty. Both halves needed fixing: the
    // assertion says what is wrong, and the code after it must not assume the assertion held.
    auto found = std::find_if(open_.begin(), open_.end(),
                              [&](const cc::HostRange& r) {
                                return r.at == used.at && r.bytes >= used.bytes;
                              });
    if (found == open_.end()) {
      if (err) *err = "Commit of a range that was never reserved";
      return false;
    }
    const cc::HostRange reserved = *found;
    open_.erase(found);
    used_ -= reserved.bytes - used.bytes;  // the tail goes back to the pool
    committed_.insert(used);
    reserved_of_[used.at] = reserved;
    ++commits;
    return true;
  }

  void Release(const cc::HostRange& whole) override {
    ++releases;
    const cc::HostRange reserved = reserved_of_.count(whole.at)
                                       ? reserved_of_[whole.at]
                                       : whole;
    // Releasing something already released is a double free of executable memory, so it is a hard
    // failure here rather than a benign one: the same range could be reserved twice and the second
    // Release would silently succeed.
    if (released_.count(reserved) != 0) ++double_releases;
    released_.insert(reserved);
  }

  std::set<cc::HostRange> committed() const { return committed_; }
  int reserves = 0, commits = 0, releases = 0, double_releases = 0;

 private:
  cc::Kind kind_;
  std::size_t capacity_;
  std::size_t used_ = 0;
  std::uintptr_t next_ = 0x1000;
  std::set<cc::HostRange> open_;
  std::set<cc::HostRange> committed_;
  std::set<cc::HostRange> released_;
  std::map<std::uintptr_t, cc::HostRange> reserved_of_;  // commit prefix -> what was reserved
};

std::unique_ptr<FakeAllocator> Fake(std::size_t capacity = 1u << 20) {
  return std::make_unique<FakeAllocator>(cc::Kind::kAnonRwx, capacity);
}

cc::Cache Make(std::unique_ptr<FakeAllocator> a, cc::Limits l = {},
               cc::Kind asked = cc::Kind::kAnonRwx) {
  return cc::Cache(std::move(a), asked, l);
}

// ------------------------------------------------------- the key

/// The invariant the whole component is named for, from `docs/research/05` §2.9.
void two_instances_at_one_pc_are_two_blocks() {
  auto fake = Fake();
  auto* f = fake.get();
  cc::Cache c = Make(std::move(fake));
  const auto one = c.Acquire(Key(1, 7, 0x2000), 64);
  CHECK_CTX(one.state == cc::Slot::State::kSpace, "first instance");
  std::string err;
  CHECK_CTX(c.Commit(one, 40, &err), err);
  const auto two = c.Acquire(Key(2, 7, 0x2000), 64);
  CHECK_CTX(two.state == cc::Slot::State::kSpace, "second instance, same guest pc");
  CHECK_CTX(two.range.at != one.range.at, "two instances were handed the same host memory");
  CHECK_CTX(c.Commit(two, 40, &err), err);
  CHECK_CTX(c.blocks_of(1) == 1u && c.blocks_of(2) == 1u, "per-instance accounting");

  // And flushing one must leave the other alone — the half that a shared table gets wrong. The size is
  // checked and then used, with the index guarded: the first version of this line segfaulted on
  // `dropped[0]` after a helper bug had emptied the cache, which turned a findable failure into a crash
  // and cost two device cycles to see.
  const auto dropped = c.FlushInstance(1);
  CHECK_CTX(dropped.size() == 1u, std::to_string(dropped.size()));
  CHECK_CTX(!dropped.empty() && dropped[0] == 0x2000u,
            "FlushInstance returns the guest range to invalidate");
  cc::HostRange got{};
  CHECK(!c.Find(Key(1, 7, 0x2000), &got));
  CHECK(c.Find(Key(2, 7, 0x2000), &got));
  CHECK_CTX(f->double_releases == 0, "a range was released twice");
}

/// Address and instance are not enough: two modules can be mapped at the same guest address, and the
/// mode is in the key because getting it wrong is the failure that does not fault — an ARM-mode fetch
/// of Thumb bytes decodes *something*, and the guest runs it.
void image_and_mode_are_part_of_the_identity() {
  auto c = Make(Fake());
  const auto a = c.Acquire(Key(1, 1, 0x3000), 64);
  std::string err;
  CHECK(c.Commit(a, 32, &err));
  CHECK(c.Acquire(Key(1, 2, 0x3000), 64).state == cc::Slot::State::kSpace);
  CHECK(c.Acquire(Key(1, 1, 0x3000, isa::Mode::Arm), 64).state == cc::Slot::State::kSpace);
  // Same key a third time is the case that must not hand out space: a caller that took it for "no room"
  // would evict the block it is asking about.
  CHECK_CTX(c.Acquire(Key(1, 1, 0x3000), 64).state == cc::Slot::State::kDuplicate,
            "a duplicate Acquire offered fresh space");
}

// ------------------------------------------------------- W^X and accountancy

void commit_shrinks_and_release_takes_the_whole_reservation() {
  auto fake = Fake();
  auto* f = fake.get();
  cc::Cache c = Make(std::move(fake));
  const auto slot = c.Acquire(Key(1, 1, 0x4000), 256);
  std::string err;
  CHECK(c.Commit(slot, 100, &err));
  cc::HostRange found{};
  CHECK(c.Find(Key(1, 1, 0x4000), &found));
  CHECK_CTX(found.bytes == 100u, std::to_string(found.bytes) + " accounted; want what was emitted");
  CHECK_CTX(c.bytes_in_use(1) == 100u, std::to_string(c.bytes_in_use(1)));
  CHECK_CTX(f->commits == 1 && f->reserves == 1, "reserve/commit counts");
  // The *reserved* range is what goes back, not the committed prefix — releasing 100 of 256 bytes would
  // strand 156 of them in the allocator forever.
  c.Forget(Key(1, 1, 0x4000));
  CHECK_CTX(f->releases == 1, std::to_string(f->releases));
  CHECK_CTX(f->double_releases == 0, "released twice");
  CHECK(f->committed().count(cc::HostRange{found.at, 100}) == 1u);
}

void an_over_eager_commit_is_refused_not_clamped() {
  auto c = Make(Fake());
  const auto slot = c.Acquire(Key(1, 1, 0x5000), 64);
  std::string err;
  CHECK(!c.Commit(slot, 65, &err));
  CHECK_CTX(err.find("65") != std::string::npos, err);
  CHECK(!c.Find(Key(1, 1, 0x5000), nullptr));
  // A zero-length commit is the same class of lie as an over-long one.
  const auto again = c.Acquire(Key(1, 1, 0x5000), 64);
  err.clear();
  CHECK(!c.Commit(again, 0, &err));
  CHECK_CTX(!err.empty(), "a refused commit said nothing");
}

void a_failed_commit_leaves_nothing_executable_behind() {
  class Broken : public cc::Allocator {
   public:
    cc::Kind kind() const override { return cc::Kind::kAnonRwx; }
    bool Reserve(std::size_t bytes, cc::HostRange* out, std::string*) override {
      *out = cc::HostRange{0x9000, bytes};
      return true;
    }
    bool Commit(const cc::HostRange&, std::string* err) override {
      *err = "mprotect(RX) failed";  // what an OEM sepolicy refusal looks like from here
      return false;
    }
    void Release(const cc::HostRange&) override { ++releases; }
    int releases = 0;
  };
  auto* raw = new Broken();
  cc::Cache c(std::unique_ptr<cc::Allocator>(raw), cc::Kind::kAnonRwx, {});
  const auto slot = c.Acquire(Key(1, 1, 0x6000), 64);
  std::string err;
  CHECK(!c.Commit(slot, 40, &err));
  CHECK_CTX(err.find("mprotect") != std::string::npos, err);
  cc::HostRange found{};
  CHECK_CTX(!c.Find(Key(1, 1, 0x6000), &found),
            "a block whose commit failed is findable, and it is not executable");
  CHECK_CTX(raw->releases == 1, "the failed range was not handed back");
}

// ------------------------------------------------------- limits and eviction

void the_limit_evicts_one_of_its_own_and_not_the_other_apps() {
  cc::Limits l{};
  l.blocks_per_instance = 2;
  auto c = Make(Fake(), l);
  std::string err;
  const auto a = c.Acquire(Key(1, 1, 0x1000), 64);
  CHECK(c.Commit(a, 16, &err));
  const auto b = c.Acquire(Key(1, 1, 0x1008), 64);
  CHECK(c.Commit(b, 16, &err));
  // The oldest is the one that goes, and only within instance 1.
  const auto other = c.Acquire(Key(2, 1, 0x2000), 64);
  CHECK(c.Commit(other, 16, &err));
  const auto d = c.Acquire(Key(1, 1, 0x1010), 64);
  CHECK_CTX(d.state == cc::Slot::State::kSpace, "the third block of a full instance got no room");
  CHECK(c.Commit(d, 16, &err));
  CHECK_CTX(c.blocks_of(1) == 2u && c.blocks_of(2) == 1u,
            "instance 1 has " + std::to_string(c.blocks_of(1)) + ", instance 2 has " +
                std::to_string(c.blocks_of(2)));
  CHECK_CTX(c.evictions() == 1u, std::to_string(c.evictions()));
  cc::HostRange got{};
  CHECK(!c.Find(Key(1, 1, 0x1000), &got));
  CHECK(c.Find(Key(1, 1, 0x1010), &got));
  CHECK(c.Find(Key(2, 1, 0x2000), &got));
}

/// A lookup refreshes recency, or a hot loop's own block is the one evicted next time the cache fills —
/// which is the behaviour that makes a small cache look broken only under real code.
void a_hot_block_survives_and_a_cold_one_does_not() {
  cc::Limits l{};
  l.blocks_per_instance = 2;
  auto c = Make(Fake(), l);
  std::string err;
  const auto hot = c.Acquire(Key(1, 1, 0x1000), 64);
  CHECK(c.Commit(hot, 16, &err));
  const auto cold = c.Acquire(Key(1, 1, 0x1008), 64);
  CHECK(c.Commit(cold, 16, &err));
  cc::HostRange got{};
  CHECK(c.Find(Key(1, 1, 0x1000), &got));  // touched: now the younger of the two
  const auto third = c.Acquire(Key(1, 1, 0x1010), 64);
  CHECK(c.Commit(third, 16, &err));
  CHECK(c.Find(Key(1, 1, 0x1000), &got));
  CHECK(!c.Find(Key(1, 1, 0x1008), &got));
}

/// "No room" has to be reachable, and the way to it is the *allocator* being out rather than the
/// per-instance count: with per-instance limits, an instance that has compiled nothing is by definition
/// not at its own ceiling, so a second instance asking for space would be told "no room" by a policy that
/// only looked at counts. The empty instance cannot evict anything of its own, so it must refuse — and
/// the first instance keeps its block, because refusing is not the same as taking someone else's.
void an_untouchable_full_cache_says_no_room_not_lie() {
  auto c = Make(Fake(64));  // one 64-byte reservation, and that is the whole pool
  std::string err;
  const auto one = c.Acquire(Key(1, 1, 0x7000), 64);
  CHECK_CTX(one.state == cc::Slot::State::kSpace, "the first block should have fitted");
  CHECK(c.Commit(one, 16, &err));
  const auto two = c.Acquire(Key(2, 1, 0x8000), 64);
  CHECK_CTX(two.state == cc::Slot::State::kNoRoom,
            "an empty instance was made to evict: state " +
                std::to_string(static_cast<int>(two.state)));
  CHECK(c.Find(Key(1, 1, 0x7000), nullptr));  // and instance 1 kept its block
  CHECK_CTX(c.evictions() == 0u, std::to_string(c.evictions()));
}

// ------------------------------------------------------- kInterpreted, and the report

/// The fallback has to be a shape rather than a failure, because iOS and every U-13 "no" take this path
/// and Tier I needs no cache at all — a cache that reported full would send a caller into evicting a
/// table it never populated.
void interpreted_backend_hands_out_nothing_and_fails_nobody() {
  auto interp = std::make_unique<FakeAllocator>(cc::Kind::kInterpreted, 0);
  auto* f = interp.get();
  cc::Cache c(std::move(interp), cc::Kind::kAnonRwx, {});
  const auto slot = c.Acquire(Key(1, 1, 0x9000), 64);
  CHECK_CTX(slot.state == cc::Slot::State::kInterpreted,
            std::to_string(static_cast<int>(slot.state)));
  CHECK_CTX(slot.range.empty(), "the interpreted backend handed out memory");
  CHECK_CTX(f->reserves == 0, "Reserve was called on a backend with no memory");
  CHECK_CTX(c.blocks() == 0u, std::to_string(c.blocks()));
  std::string err;
  CHECK(!c.Commit(slot, 16, &err));  // there is nothing to commit, and it says so
  CHECK_CTX(err.find("not space") != std::string::npos, err);
}

/// A reserved-but-uncommitted block is a back end mid-write. Evicting it would `Release` memory under an
/// emitter that is about to commit into it, and the symptom of that is one app executing another's code,
/// so eviction skips them and says "no room" instead.
void an_uncommitted_block_is_never_the_eviction_victim() {
  cc::Limits l{};
  l.blocks_per_instance = 1;
  auto c = Make(Fake(), l);
  std::string err;
  const auto first = c.Acquire(Key(1, 1, 0x1000), 64);  // reserved, deliberately not committed
  CHECK_CTX(c.blocks_of(1) == 1u, "the outstanding slot is not accounted");
  const auto second = c.Acquire(Key(1, 1, 0x2000), 64);
  CHECK_CTX(second.state == cc::Slot::State::kNoRoom,
            "a block being written was evicted to make room");
  CHECK_CTX(c.evictions() == 0u, std::to_string(c.evictions()));
  CHECK(!c.Find(Key(1, 1, 0x1000), nullptr));  // not committed, so not findable
  CHECK(c.Commit(first, 16, &err));
  CHECK(c.Find(Key(1, 1, 0x1000), nullptr));
  // Now it is a legitimate victim, and the second block gets its space.
  const auto third = c.Acquire(Key(1, 1, 0x2000), 64);
  CHECK_CTX(third.state == cc::Slot::State::kSpace, "a committed block was not evictable");
  CHECK_CTX(c.evictions() == 1u, std::to_string(c.evictions()));
}

/// `FlushInstance` is "provably called on every T1 exit path" (C04's review focus), and it can only be
/// proved against a list the cache itself can give. First-seen order, the same convention as C07's
/// tally, so a report read top-down matches the order the launches arrived in.
void the_touched_set_is_what_a_teardown_has_to_flush() {
  auto c = Make(Fake());
  std::string err;
  CHECK(c.Commit(c.Acquire(Key(3, 1, 0x1000), 64), 8, &err));
  CHECK(c.Commit(c.Acquire(Key(1, 1, 0x1000), 64), 8, &err));
  const auto ids = c.Instances();
  CHECK_CTX(ids.size() == 2u, std::to_string(ids.size()));
  CHECK_CTX(ids.size() == 2u && ids[0] == 3u && ids[1] == 1u, "instance order is not first-seen");
  CHECK_CTX(c.instances_touched().size() == 2u, "tally size disagrees with the instance set");
}

void the_backend_choice_is_echoed_with_the_downgrade_named() {
  auto c = Make(Fake(), {}, cc::Kind::kAnonRwx);
  diag::Recorder rec;
  rec.Require(diag::Field::kCodeCacheBackend);
  c.Echo(rec, "code-cache", "probed: anon RWX executed a stub");
  const std::string same = rec.Render();
  CHECK_CTX(same.find("field = code-cache-backend") != std::string::npos, same);
  CHECK_CTX(same.find("requested = AnonRwx") != std::string::npos, same);
  CHECK_CTX(same.find("active = AnonRwx") != std::string::npos, same);
  CHECK_CTX(same.find("outcome = active") != std::string::npos, same);

  // The same cache object, asked for the dual mapping and given the interpreter: that difference is the
  // whole reason the field exists, so it has to be visible in the report and not only in the object.
  auto downgraded = std::make_unique<FakeAllocator>(cc::Kind::kInterpreted, 0);
  cc::Cache c2(std::move(downgraded), cc::Kind::kMemfdDual, {});
  diag::Recorder rec2;
  c2.Echo(rec2, "code-cache", "memfd_create refused: errno 1");
  const std::string text = rec2.Render();
  CHECK_CTX(text.find("requested = MemfdDualAlias") != std::string::npos, text);
  CHECK_CTX(text.find("active = Interpreted") != std::string::npos, text);
  CHECK_CTX(text.find("outcome = inert") != std::string::npos, text);
  CHECK_CTX(text.find("downgraded from MemfdDualAlias") != std::string::npos, text);
}

void the_names_are_stable_and_the_kinds_are_all_distinct() {
  CHECK(std::string{cc::Name(cc::Kind::kAnonRwx)} == "AnonRwx");
  CHECK(std::string{cc::Name(cc::Kind::kMemfdDual)} == "MemfdDualAlias");
  CHECK(std::string{cc::Name(cc::Kind::kInterpreted)} == "Interpreted");
  CHECK(std::string{cc::Name(cc::Kind::kAnonRwx)} != cc::Name(cc::Kind::kMemfdDual));
  CHECK_CTX(Key(1, 2, 0x3000).Describe().find("inst1") != std::string::npos,
            Key(1, 2, 0x3000).Describe());
  CHECK_CTX(Key(1, 2, 0x3000, isa::Mode::Arm).Describe().back() == 'a',
            Key(1, 2, 0x3000, isa::Mode::Arm).Describe());
}

/// Named rather than a bare call list: this lane's first run ended in a `Segmentation fault` with
/// nothing before it, which is the least informative failure there is. Printing the case name as it
/// starts costs one line each and turns a crash into a pointer at the code that caused it.
#define RUN(fn)                                   \
  do {                                            \
    std::fprintf(stderr, "[cc] %s\n", #fn);        \
    std::fflush(stderr);                          \
    fn();                                         \
  } while (0)

}  // namespace

int main() {
  RUN(two_instances_at_one_pc_are_two_blocks);
  RUN(image_and_mode_are_part_of_the_identity);
  RUN(commit_shrinks_and_release_takes_the_whole_reservation);
  RUN(an_over_eager_commit_is_refused_not_clamped);
  RUN(a_failed_commit_leaves_nothing_executable_behind);
  RUN(the_limit_evicts_one_of_its_own_and_not_the_other_apps);
  RUN(a_hot_block_survives_and_a_cold_one_does_not);
  RUN(an_untouchable_full_cache_says_no_room_not_lie);
  RUN(interpreted_backend_hands_out_nothing_and_fails_nobody);
  RUN(an_uncommitted_block_is_never_the_eviction_victim);
  RUN(the_touched_set_is_what_a_teardown_has_to_flush);
  RUN(the_backend_choice_is_echoed_with_the_downgrade_named);
  RUN(the_names_are_stable_and_the_kinds_are_all_distinct);

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
