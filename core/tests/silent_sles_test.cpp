// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The silent OpenSL backend behind the iOS host (platform/ios/silent_sles.cpp), driven exactly as
// an armeabi OpenAL caller drives it: load the object cell, load its vtable entry, call that host
// slot with self first. No OpenSLES headers or device library are needed; the contract under test is
// the guest ABI, not Android's implementation.
//
// Rows:
//   * four IID data imports answer guest pointers to distinct GUID objects;
//   * slCreateEngine -> Realize -> GetInterface(SL_IID_ENGINE) -> CreateOutputMix -> Realize and
//     CreateAudioPlayer -> Realize succeed (KitKat's startup stops when any of these fails);
//   * the player offers PLAY and both buffer-queue interfaces, state round-trips, and enqueued
//     buffers are counted and dropped without a callback;
//   * an unknown IID is FEATURE_UNSUPPORTED and leaves the out-cell untouched;
//   * Destroy invalidates every interface of that object instead of using freed host state.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "rb/audio/silent_sles.h"
#include "rb/exec/machine_state.h"
#include "rb/mem/guest_memory.h"

namespace {

using namespace rb;

int checks = 0;
int failures = 0;

void fail(const char* what, int line, const std::string& detail = {}) {
  ++failures;
  std::fprintf(stderr, "FAIL  %s  (line %d)%s%s\n", what, line, detail.empty() ? "" : "  ", detail.c_str());
}
#define CHECK(...)                                   \
  do {                                               \
    ++checks;                                        \
    if (!(__VA_ARGS__)) fail(#__VA_ARGS__, __LINE__); \
  } while (0)

constexpr std::uint32_t kStack = 0x10000u;
constexpr std::uint32_t kScratch = 0x20000u;
constexpr std::uint32_t kHeap = 0x100000u;
constexpr std::uint32_t kSlotBase = 0x7000'0000u;
constexpr std::uint32_t kSuccess = 0;
constexpr std::uint32_t kPreconditions = 1;
constexpr std::uint32_t kFeatureUnsupported = 12;

enum : unsigned { kRealize = 0, kGetState = 2, kGetInterface = 3, kDestroy = 6 };
enum : unsigned { kCreateAudioPlayer = 2, kCreateOutputMix = 7 };
enum : unsigned { kSetPlayState = 0, kGetPlayState = 1 };
enum : unsigned { kEnqueue = 0, kClear = 1, kBqGetState = 2 };

struct Rig {
  std::unique_ptr<mem::GuestMemory> mem;
  std::uint32_t heap = kHeap;
  std::vector<audio::SilentSlesHost::Method> methods;
  std::map<std::string, std::uint32_t> names;
  std::map<std::string, std::uint32_t> data;
  std::vector<std::string> said;
  std::unique_ptr<audio::SilentSles> sles;

  Rig() {
    mem::AddressModel model;
    model.kind = mem::AddressModel::kBiased;
    std::string err;
    mem = mem::GuestMemory::Create(std::size_t{16} << 20, model, &err);
    mem->Map(kStack, 0x4000, mem::kRead | mem::kWrite, &err);
    mem->Map(kScratch, 0x10000, mem::kRead | mem::kWrite, &err);

    audio::SilentSlesHost h;
    h.mem = mem.get();
    h.alloc = [this](std::size_t bytes) -> std::uint32_t {
      const std::uint32_t len = static_cast<std::uint32_t>((bytes + 0xfff) & ~std::size_t{0xfff});
      std::string e;
      if (!mem->Map(heap, len, mem::kRead | mem::kWrite, &e)) return 0;
      const std::uint32_t at = heap;
      heap += len;
      return at;
    };
    h.bind = [this](const std::string& name, audio::SilentSlesHost::Method fn) -> std::uint32_t {
      if (const auto it = names.find(name); it != names.end()) return it->second;
      methods.push_back(std::move(fn));
      const std::uint32_t slot = kSlotBase + 4u * static_cast<std::uint32_t>(methods.size() - 1);
      names.emplace(name, slot);
      return slot;
    };
    h.data_symbol = [this](const std::string& name, std::uint32_t at) { data[name] = at; };
    h.log = [this](const std::string& line) { said.push_back(line); };
    sles = std::make_unique<audio::SilentSles>(std::move(h));
  }

  std::uint32_t U32(std::uint32_t at) const {
    std::uint32_t v = 0;
    mem->Read(at, &v, 4);
    return v;
  }
  void Put(std::uint32_t at, std::uint32_t v) { mem->Write(at, &v, 4); }

  std::uint32_t Call(std::uint32_t slot, std::initializer_list<std::uint32_t> args) {
    if (slot < kSlotBase || (slot - kSlotBase) / 4u >= methods.size()) return 0xdeadu;
    exec::MachineState s{};
    s.r[13] = kStack;
    unsigned i = 0;
    for (std::uint32_t v : args) {
      if (i < 4) {
        s.r[i] = v;
      } else {
        Put(kStack + 4u * (i - 4u), v);
      }
      ++i;
    }
    std::string err;
    if (!methods[(slot - kSlotBase) / 4u](s, &err)) return 0xbadu;
    return s.r[0];
  }

  std::uint32_t Method(std::uint32_t itf, unsigned index, std::initializer_list<std::uint32_t> rest) {
    const std::uint32_t vtable = U32(itf);
    const std::uint32_t entry = U32(vtable + 4u * index);
    std::vector<std::uint32_t> args{itf};
    args.insert(args.end(), rest.begin(), rest.end());
    exec::MachineState s{};
    s.r[13] = kStack;
    for (std::size_t i = 0; i < args.size(); ++i) {
      if (i < 4) {
        s.r[i] = args[i];
      } else {
        Put(kStack + 4u * static_cast<std::uint32_t>(i - 4), args[i]);
      }
    }
    if (entry < kSlotBase || (entry - kSlotBase) / 4u >= methods.size()) return 0xdeadu;
    std::string err;
    if (!methods[(entry - kSlotBase) / 4u](s, &err)) return 0xbadu;
    return s.r[0];
  }

  std::uint32_t Iid(const std::string& name) {
    const auto it = data.find(name);
    return it == data.end() ? 0u : U32(it->second);
  }
};

void iids_are_guest_pointers_to_distinct_guids() {
  Rig r;
  const char* const names[] = {"SL_IID_ENGINE", "SL_IID_PLAY", "SL_IID_BUFFERQUEUE",
                               "SL_IID_ANDROIDSIMPLEBUFFERQUEUE"};
  std::vector<std::uint32_t> guids;
  for (const char* name : names) {
    const auto cell = r.data.find(name);
    CHECK(cell != r.data.end());
    if (cell == r.data.end()) continue;
    const std::uint32_t guid = r.U32(cell->second);
    CHECK(guid != 0);
    unsigned char bytes[16] = {};
    CHECK(r.mem->Read(guid, bytes, sizeof bytes));
    bool any = false;
    for (const unsigned char b : bytes) any = any || b != 0;
    CHECK(any);
    guids.push_back(guid);
  }
  for (std::size_t i = 0; i < guids.size(); ++i) {
    for (std::size_t j = i + 1; j < guids.size(); ++j) CHECK(guids[i] != guids[j]);
  }
}

void openal_startup_contract_succeeds() {
  Rig r;
  CHECK(r.names.count("slCreateEngine") == 1);
  if (r.names.count("slCreateEngine") == 0) return;

  CHECK(r.Call(r.names["slCreateEngine"], {kScratch, 0, 0, 0, 0, 0}) == kSuccess);
  const std::uint32_t engine = r.U32(kScratch);
  CHECK(engine != 0);
  if (engine == 0) return;
  CHECK(r.Method(engine, kRealize, {0}) == kSuccess);
  CHECK(r.Method(engine, kGetInterface, {r.Iid("SL_IID_ENGINE"), kScratch + 4}) == kSuccess);
  const std::uint32_t engine_itf = r.U32(kScratch + 4);
  CHECK(engine_itf != 0);
  if (engine_itf == 0) return;

  // The two allocations OpenAL makes before it accepts the device.
  CHECK(r.Method(engine_itf, kCreateOutputMix, {kScratch + 8, 0, 0, 0}) == kSuccess);
  const std::uint32_t mix = r.U32(kScratch + 8);
  CHECK(mix != 0);
  if (mix != 0) CHECK(r.Method(mix, kRealize, {0}) == kSuccess);

  CHECK(r.Method(engine_itf, kCreateAudioPlayer, {kScratch + 12, kScratch + 0x200, kScratch + 0x300, 0}) ==
        kSuccess);
  const std::uint32_t player = r.U32(kScratch + 12);
  CHECK(player != 0);
  if (player == 0) return;
  CHECK(r.Method(player, kRealize, {0}) == kSuccess);

  CHECK(r.Method(player, kGetInterface, {r.Iid("SL_IID_PLAY"), kScratch + 16}) == kSuccess);
  CHECK(r.Method(player, kGetInterface, {r.Iid("SL_IID_BUFFERQUEUE"), kScratch + 20}) == kSuccess);
  CHECK(r.Method(player, kGetInterface, {r.Iid("SL_IID_ANDROIDSIMPLEBUFFERQUEUE"), kScratch + 24}) == kSuccess);
  const std::uint32_t play = r.U32(kScratch + 16);
  const std::uint32_t bq = r.U32(kScratch + 20);
  CHECK(play != 0 && bq != 0);
  if (play == 0 || bq == 0) return;

  CHECK(r.Method(play, kSetPlayState, {2}) == kSuccess);  // SL_PLAYSTATE_PLAYING
  CHECK(r.Method(play, kGetPlayState, {kScratch + 28}) == kSuccess);
  CHECK(r.U32(kScratch + 28) == 2);
  CHECK(r.Method(bq, kEnqueue, {kScratch + 0x400, 256}) == kSuccess);
  CHECK(r.Method(bq, kBqGetState, {kScratch + 32}) == kSuccess);
  CHECK(r.U32(kScratch + 32) == 1);
  CHECK(r.Method(bq, kClear, {}) == kSuccess);
  CHECK(r.Method(bq, kBqGetState, {kScratch + 36}) == kSuccess);
  CHECK(r.U32(kScratch + 36) == 0);

  bool named_mix = false, named_player = false, named_engine = false;
  for (const std::string& line : r.said) {
    named_mix = named_mix || line.find("CreateOutputMix") != std::string::npos;
    named_player = named_player || line.find("CreateAudioPlayer") != std::string::npos;
    named_engine = named_engine || line.find("slCreateEngine") != std::string::npos;
  }
  CHECK(named_mix && named_player && named_engine);
}

void unknown_interface_is_named_and_does_not_write_out() {
  Rig r;
  r.Call(r.names["slCreateEngine"], {kScratch, 0, 0, 0, 0, 0});
  const std::uint32_t player_out = kScratch + 8;
  // Build an engine, then use it only to reach the player (keeps the test independent of layout).
  r.Method(r.U32(kScratch), kRealize, {0});
  r.Method(r.U32(kScratch), kGetInterface, {r.Iid("SL_IID_ENGINE"), kScratch + 4});
  const std::uint32_t engine_itf = r.U32(kScratch + 4);
  r.Method(engine_itf, kCreateAudioPlayer, {player_out, 0, 0, 0});
  const std::uint32_t player = r.U32(player_out);
  CHECK(player != 0);
  if (player == 0) return;

  const std::uint32_t bogus = kScratch + 0x800;
  const unsigned char junk[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  r.mem->Write(bogus, junk, sizeof junk);
  r.Put(kScratch + 12, 0x1234);
  CHECK(r.Method(player, kGetInterface, {bogus, kScratch + 12}) == kFeatureUnsupported);
  CHECK(r.U32(kScratch + 12) == 0x1234);

  // Destroy removes every interface derived from the object, not only the one called.
  CHECK(r.Method(player, kGetInterface, {r.Iid("SL_IID_PLAY"), kScratch + 16}) == kSuccess);
  const std::uint32_t play = r.U32(kScratch + 16);
  CHECK(r.Method(player, kDestroy, {}) == kSuccess);
  CHECK(r.Method(play, kGetPlayState, {kScratch + 20}) == kPreconditions);
  bool named = false;
  for (const std::string& line : r.said) named = named || line.find("dead interface") != std::string::npos;
  CHECK(named);
}

}  // namespace

int main() {
  iids_are_guest_pointers_to_distinct_guids();
  openal_startup_contract_succeeds();
  unknown_interface_is_named_and_does_not_write_out();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
