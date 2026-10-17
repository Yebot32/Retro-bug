// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// rb_sles_tests (M4 Task 4): the OpenSL ES thunks (android/jni/thunks_sles.cpp) against this device's real
// libOpenSLES. Each row plays the guest's part by hand, as an armeabi caller would: it reads an object's guest cell
// to find its vtable, loads a method's slot address from it, and "calls" that slot with r0..r3 and stacked words; a
// guest callback is a fake guest address the test answers. Run on a device (tools/run_engine_tests.sh).
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rb/abi/thunk.h"
#include "rb/mem/guest_memory.h"
#include "thunks_sles.h"

namespace {

using namespace rb;

int checks = 0, failures = 0;

void fail(const char* what, int line, const std::string& detail = {}) {
  ++failures;
  std::printf("FAIL  %s  (line %d)%s%s\n", what, line, detail.empty() ? "" : "  ", detail.c_str());
}
#define CHECK(...)                                   \
  do {                                               \
    ++checks;                                        \
    if (!(__VA_ARGS__)) fail(#__VA_ARGS__, __LINE__); \
  } while (0)
#define CHECK_CTX(cond, detail)                \
  do {                                         \
    ++checks;                                  \
    if (!(cond)) fail(#cond, __LINE__, detail); \
  } while (0)

constexpr std::uint32_t kStack = 0x10000u;       // the guest sp: stacked argument words start here
constexpr std::uint32_t kScratch = 0x20000u;     // the test's own guest structs and out-params
constexpr std::uint32_t kHeap = 0x100000u;       // what SlesHost::alloc hands out
constexpr std::uint32_t kSlotBase = 0x7000'0000u;  // fake slot addresses (never guest memory)
constexpr std::uint32_t kBqCallback = 0x6000'0001u;  // fake guest functions
constexpr std::uint32_t kPlayCallback = 0x6000'0011u;

// Method indices (the vtable order of OpenSLES.h).
enum : unsigned { kRealize = 0, kGetState = 2, kGetInterface = 3, kDestroy = 6 };
enum : unsigned { kCreateAudioPlayer = 2, kCreateOutputMix = 7 };
enum : unsigned { kSetPlayState = 0, kGetPlayState = 1, kPlayRegisterCallback = 4, kSetCallbackEventsMask = 5 };
enum : unsigned { kEnqueue = 0, kClear = 1, kBqGetState = 2, kBqRegisterCallback = 3 };
enum : unsigned { kSetVolumeLevel = 0, kGetVolumeLevel = 1 };

struct Rig {
  std::unique_ptr<mem::GuestMemory> m;
  abi::HandleTable handles;
  std::vector<launcher::SlesHost::Method> methods;
  std::vector<std::string> method_names;
  std::uint32_t heap = kHeap;
  std::mutex mu;
  std::condition_variable cv;
  std::map<std::uint32_t, std::function<void(const std::vector<std::uint32_t>&)>> guest_fns;
  std::vector<std::string> said;
  std::unique_ptr<launcher::Sles> sles;

  Rig() {
    mem::AddressModel model;
    model.kind = mem::AddressModel::kBiased;
    std::string err;
    m = mem::GuestMemory::Create(std::size_t{16} << 20, model, &err);
    m->Map(kStack, 0x4000, mem::kRead | mem::kWrite, &err);
    m->Map(kScratch, 0x10000, mem::kRead | mem::kWrite, &err);
    launcher::SlesHost h;
    h.mem = m.get();
    h.handles = &handles;
    h.alloc = [this](std::size_t bytes) -> std::uint32_t {
      const std::uint32_t len = static_cast<std::uint32_t>((bytes + 0xfff) & ~std::size_t{0xfff});
      std::string e;
      if (!m->Map(heap, len, mem::kRead | mem::kWrite, &e)) return 0;
      const std::uint32_t at = heap;
      heap += len;
      return at;
    };
    h.bind = [this](const std::string& name, launcher::SlesHost::Method fn) -> std::uint32_t {
      for (std::size_t i = 0; i < method_names.size(); ++i) {
        if (method_names[i] == name) return kSlotBase + 4u * static_cast<std::uint32_t>(i);
      }
      methods.push_back(std::move(fn));
      method_names.push_back(name);
      return kSlotBase + 4u * static_cast<std::uint32_t>(methods.size() - 1);
    };
    h.call_guest = [this](std::uint32_t fn, const std::vector<std::uint32_t>& args) {
      std::function<void(const std::vector<std::uint32_t>&)> f;
      {
        std::lock_guard<std::mutex> lk(mu);
        const auto it = guest_fns.find(fn);
        if (it != guest_fns.end()) f = it->second;
      }
      if (f) f(args);
    };
    h.unlocked = [](const std::function<void()>& f) { f(); };
    h.say = [this](const std::string& s) {
      std::lock_guard<std::mutex> lk(mu);
      said.push_back(s);
    };
    sles = std::make_unique<launcher::Sles>(std::move(h));
  }

  std::uint32_t U32(std::uint32_t at) const {
    std::uint32_t v = 0;
    m->Read(at, &v, 4);
    return v;
  }
  void Put(std::uint32_t at, std::uint32_t v) { m->Write(at, &v, 4); }

  /// Calls the slot at `addr` with `args` laid out as AAPCS32 passes them; returns r0, or 0xdead when `addr` is no
  /// slot this rig bound.
  std::uint32_t Call(std::uint32_t addr, std::initializer_list<std::uint32_t> args, std::string* why = nullptr) {
    if (addr < kSlotBase || (addr - kSlotBase) / 4u >= methods.size()) return 0xdeadu;
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
    std::string e;
    if (!methods[(addr - kSlotBase) / 4u](s, &e)) {
      if (why) *why = e;
      return 0xbadu;
    }
    return s.r[0];
  }
  /// The guest's `(*itf)->Method(itf, ...)`: method `k` of the interface at guest cell `itf`.
  std::uint32_t Method(std::uint32_t itf, unsigned k, std::initializer_list<std::uint32_t> rest) {
    const std::uint32_t vtable = U32(itf);
    const std::uint32_t entry = U32(vtable + 4u * k);
    std::vector<std::uint32_t> a{itf};
    a.insert(a.end(), rest.begin(), rest.end());
    exec::MachineState s{};
    s.r[13] = kStack;
    for (std::size_t i = 0; i < a.size(); ++i) {
      if (i < 4) {
        s.r[i] = a[i];
      } else {
        Put(kStack + 4u * static_cast<std::uint32_t>(i - 4), a[i]);
      }
    }
    if (entry < kSlotBase || (entry - kSlotBase) / 4u >= methods.size()) return 0xdeadu;
    std::string e;
    if (!methods[(entry - kSlotBase) / 4u](s, &e)) return 0xbadu;
    return s.r[0];
  }
  /// The guest SLInterfaceID value for `name` (what `SL_IID_x` holds), 0 when unresolved.
  std::uint32_t Iid(const char* name) {
    const auto cell = sles->Resolve(name);
    return cell ? U32(*cell) : 0u;
  }
};

/// `slCreateEngine` + Realize + the engine interface: returns the engine object and its SLEngineItf (guest cells).
bool MakeEngine(Rig& r, std::uint32_t* obj, std::uint32_t* engine) {
  const auto create = r.sles->Resolve("slCreateEngine");
  if (!create) return false;
  const std::uint32_t out = kScratch;
  r.Put(out, 0);
  if (r.Call(*create, {out, 0, 0, 0, 0, 0}) != SL_RESULT_SUCCESS) return false;
  *obj = r.U32(out);
  if (*obj == 0 || r.Method(*obj, kRealize, {SL_BOOLEAN_FALSE}) != SL_RESULT_SUCCESS) return false;
  r.Put(out + 4, 0);
  if (r.Method(*obj, kGetInterface, {r.Iid("SL_IID_ENGINE"), out + 4}) != SL_RESULT_SUCCESS) return false;
  *engine = r.U32(out + 4);
  return *engine != 0;
}

void iids_are_guest_copies_of_the_host_guids() {
  Rig r;
  const auto cell = r.sles->Resolve("SL_IID_ENGINE");
  CHECK(cell.has_value());
  if (!cell) return;
  const std::uint32_t guid = r.U32(*cell);
  CHECK(guid != 0);
  unsigned char bytes[sizeof(SLInterfaceID_)] = {};
  CHECK(r.m->Read(guid, bytes, sizeof bytes));
  CHECK(std::memcmp(bytes, SL_IID_ENGINE, sizeof bytes) == 0);
  // The same name answers the same cell; another name answers another GUID.
  CHECK(r.sles->Resolve("SL_IID_ENGINE") == cell);
  CHECK(r.Iid("SL_IID_PLAY") != 0 && r.Iid("SL_IID_PLAY") != guid);
  // Not an OpenSL ES name: not this module's to answer.
  CHECK(!r.sles->Resolve("glClear").has_value());
}

void an_engine_is_created_realized_and_destroyed() {
  Rig r;
  std::uint32_t obj = 0, engine = 0;
  CHECK(MakeEngine(r, &obj, &engine));
  if (engine == 0) return;
  // State round-trip through a guest out-param.
  r.Put(kScratch + 8, 0);
  CHECK(r.Method(obj, kGetState, {kScratch + 8}) == SL_RESULT_SUCCESS);
  CHECK(r.U32(kScratch + 8) == SL_OBJECT_STATE_REALIZED);
  // Asking twice answers the same interface cell.
  r.Put(kScratch + 12, 0);
  CHECK(r.Method(obj, kGetInterface, {r.Iid("SL_IID_ENGINE"), kScratch + 12}) == SL_RESULT_SUCCESS);
  CHECK(r.U32(kScratch + 12) == engine);
  CHECK(r.sles->live_objects() == 1);
  CHECK(r.Method(obj, kDestroy, {}) == 0 || true);  // void method: r0 is whatever it was
  CHECK(r.sles->live_objects() == 0);
  // A destroyed object's methods are refused by name, not run on freed host memory.
  CHECK(r.Method(obj, kGetState, {kScratch + 8}) != SL_RESULT_SUCCESS);
}

void an_unknown_interface_is_unsupported_and_named() {
  Rig r;
  std::uint32_t obj = 0, engine = 0;
  CHECK(MakeEngine(r, &obj, &engine));
  if (engine == 0) return;
  // A GUID the host does not know (a guest-made one): FEATURE_UNSUPPORTED, and said.
  const std::uint32_t bogus = kScratch + 0x100;
  const unsigned char junk[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  r.m->Write(bogus, junk, 16);
  r.Put(kScratch + 16, 0x1234);
  CHECK(r.Method(obj, kGetInterface, {bogus, kScratch + 16}) == SL_RESULT_FEATURE_UNSUPPORTED);
  CHECK(r.U32(kScratch + 16) == 0x1234);
  bool named = false;
  for (const std::string& line : r.said) named = named || line.find("GetInterface(IID") != std::string::npos;
  CHECK(named);
  r.Method(obj, kDestroy, {});
}

/// The row the plan names: engine, output mix, a PCM buffer-queue player; three buffers enqueued; the guest callback
/// runs three times on the audio thread; GetState's counts match; volume and play state round-trip.
void a_pcm_buffer_queue_player_calls_the_guest_back() {
  Rig r;
  std::uint32_t eobj = 0, engine = 0;
  CHECK(MakeEngine(r, &eobj, &engine));
  if (engine == 0) return;
  const std::uint32_t o = kScratch + 0x200;  // out-params and structs
  // Output mix.
  r.Put(o, 0);
  CHECK(r.Method(engine, kCreateOutputMix, {o, 0, 0, 0}) == SL_RESULT_SUCCESS);
  const std::uint32_t mix = r.U32(o);
  CHECK(mix != 0);
  CHECK(r.Method(mix, kRealize, {SL_BOOLEAN_FALSE}) == SL_RESULT_SUCCESS);
  // Source: simple buffer queue (2 buffers) + PCM 16-bit stereo 44.1 kHz. Sink: the output mix.
  const std::uint32_t loc = o + 0x10, fmt = o + 0x20, src = o + 0x40, sloc = o + 0x50, snk = o + 0x60;
  r.Put(loc, SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE);
  r.Put(loc + 4, 2);
  const std::uint32_t pcm[7] = {SL_DATAFORMAT_PCM, 2, SL_SAMPLINGRATE_44_1, SL_PCMSAMPLEFORMAT_FIXED_16,
                                SL_PCMSAMPLEFORMAT_FIXED_16, SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT,
                                SL_BYTEORDER_LITTLEENDIAN};
  r.m->Write(fmt, pcm, sizeof pcm);
  r.Put(src, loc);
  r.Put(src + 4, fmt);
  r.Put(sloc, SL_DATALOCATOR_OUTPUTMIX);
  r.Put(sloc + 4, mix);
  r.Put(snk, sloc);
  r.Put(snk + 4, 0);
  // Interfaces: the buffer queue and volume, both required.
  const std::uint32_t ids = o + 0x70, req = o + 0x80;
  r.Put(ids, r.Iid("SL_IID_ANDROIDSIMPLEBUFFERQUEUE"));
  r.Put(ids + 4, r.Iid("SL_IID_VOLUME"));
  r.Put(req, SL_BOOLEAN_TRUE);
  r.Put(req + 4, SL_BOOLEAN_TRUE);
  r.Put(o + 4, 0);
  // CreateAudioPlayer(self, pPlayer, src, snk, numInterfaces, ids, req): 7 words, three on the stack.
  CHECK(r.Method(engine, kCreateAudioPlayer, {o + 4, src, snk, 2, ids, req}) == SL_RESULT_SUCCESS);
  const std::uint32_t player = r.U32(o + 4);
  CHECK(player != 0);
  if (player == 0) return;
  CHECK(r.Method(player, kRealize, {SL_BOOLEAN_FALSE}) == SL_RESULT_SUCCESS);
  const std::uint32_t itfs = o + 0x90;
  CHECK(r.Method(player, kGetInterface, {r.Iid("SL_IID_PLAY"), itfs}) == SL_RESULT_SUCCESS);
  CHECK(r.Method(player, kGetInterface, {r.Iid("SL_IID_ANDROIDSIMPLEBUFFERQUEUE"), itfs + 4}) == SL_RESULT_SUCCESS);
  CHECK(r.Method(player, kGetInterface, {r.Iid("SL_IID_VOLUME"), itfs + 8}) == SL_RESULT_SUCCESS);
  const std::uint32_t play = r.U32(itfs), bq = r.U32(itfs + 4), vol = r.U32(itfs + 8);
  CHECK(play != 0 && bq != 0 && vol != 0);
  if (play == 0 || bq == 0 || vol == 0) return;

  // The guest callback: counts calls, notes the arguments and that it ran on another (the audio) thread.
  std::atomic<int> calls{0};
  std::atomic<bool> right_args{true}, other_thread{true};
  const std::thread::id me = std::this_thread::get_id();
  const std::uint32_t guest_ctx = 0xC0FFEEu;
  {
    std::lock_guard<std::mutex> lk(r.mu);
    r.guest_fns[kBqCallback] = [&](const std::vector<std::uint32_t>& a) {
      if (a.size() != 2 || a[0] != bq || a[1] != guest_ctx) right_args = false;
      if (std::this_thread::get_id() == me) other_thread = false;
      {
        std::lock_guard<std::mutex> lk2(r.mu);
        ++calls;
      }
      r.cv.notify_all();
    };
  }
  CHECK(r.Method(bq, kBqRegisterCallback, {kBqCallback, guest_ctx}) == SL_RESULT_SUCCESS);
  // Three 4 KiB buffers of silence in guest memory (the host reads them where they are).
  const std::uint32_t bufs = kScratch + 0x1000;
  std::vector<std::uint8_t> zero(0x1000, 0);
  for (int i = 0; i < 3; ++i) r.m->Write(bufs + 0x1000u * i, zero.data(), zero.size());
  // A 2-buffer queue takes two; the third is refused (BUFFER_INSUFFICIENT) until one plays: enqueue two, play, and
  // enqueue the third from the test when the first callback has run.
  CHECK(r.Method(bq, kEnqueue, {bufs, 0x1000}) == SL_RESULT_SUCCESS);
  CHECK(r.Method(bq, kEnqueue, {bufs + 0x1000, 0x1000}) == SL_RESULT_SUCCESS);
  r.Put(o + 0xa0, 0xffff);
  r.Put(o + 0xa4, 0xffff);
  CHECK(r.Method(bq, kBqGetState, {o + 0xa0}) == SL_RESULT_SUCCESS);
  CHECK(r.U32(o + 0xa0) == 2);  // count
  CHECK(r.U32(o + 0xa4) == 0);  // index
  CHECK(r.Method(play, kSetPlayState, {SL_PLAYSTATE_PLAYING}) == SL_RESULT_SUCCESS);
  {
    std::unique_lock<std::mutex> lk(r.mu);
    r.cv.wait_for(lk, std::chrono::seconds(3), [&] { return calls.load() >= 1; });
  }
  CHECK(r.Method(bq, kEnqueue, {bufs + 0x2000, 0x1000}) == SL_RESULT_SUCCESS);
  {
    std::unique_lock<std::mutex> lk(r.mu);
    r.cv.wait_for(lk, std::chrono::seconds(3), [&] { return calls.load() >= 3; });
  }
  CHECK_CTX(calls.load() == 3, "callbacks: " + std::to_string(calls.load()));
  CHECK(right_args.load());
  CHECK(other_thread.load());
  CHECK(r.Method(bq, kBqGetState, {o + 0xa0}) == SL_RESULT_SUCCESS);
  CHECK(r.U32(o + 0xa0) == 0);  // nothing left queued
  CHECK(r.U32(o + 0xa4) == 3);  // three played
  // Play state and volume round-trip (SLmillibel is 16 bits: the out-param is written 2 bytes wide).
  r.Put(o + 0xb0, 0);
  CHECK(r.Method(play, kGetPlayState, {o + 0xb0}) == SL_RESULT_SUCCESS);
  CHECK(r.U32(o + 0xb0) == SL_PLAYSTATE_PLAYING);
  CHECK(r.Method(vol, kSetVolumeLevel, {static_cast<std::uint32_t>(-300)}) == SL_RESULT_SUCCESS);
  r.Put(o + 0xb4, 0xAAAA0000u);
  CHECK(r.Method(vol, kGetVolumeLevel, {o + 0xb4}) == SL_RESULT_SUCCESS);
  CHECK(r.U32(o + 0xb4) == (0xAAAA0000u | 0xFED4u));  // -300 as int16, upper half untouched
  CHECK(r.Method(play, kSetPlayState, {SL_PLAYSTATE_STOPPED}) == SL_RESULT_SUCCESS);
  CHECK(r.Method(bq, kClear, {}) == SL_RESULT_SUCCESS);
  CHECK(r.sles->live_objects() == 3);
  r.Method(player, kDestroy, {});
  r.Method(mix, kDestroy, {});
  r.Method(eobj, kDestroy, {});
  CHECK(r.sles->live_objects() == 0);
  CHECK(r.handles.size() == 0);  // every host interface handle was forgotten
}

/// A play-event callback carries the event word as a third argument.
void a_play_callback_gets_its_event() {
  Rig r;
  std::uint32_t eobj = 0, engine = 0;
  CHECK(MakeEngine(r, &eobj, &engine));
  if (engine == 0) return;
  const std::uint32_t o = kScratch + 0x200;
  r.Put(o, 0);
  CHECK(r.Method(engine, kCreateOutputMix, {o, 0, 0, 0}) == SL_RESULT_SUCCESS);
  const std::uint32_t mix = r.U32(o);
  r.Method(mix, kRealize, {SL_BOOLEAN_FALSE});
  const std::uint32_t loc = o + 0x10, fmt = o + 0x20, src = o + 0x40, sloc = o + 0x50, snk = o + 0x60;
  r.Put(loc, SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE);
  r.Put(loc + 4, 1);
  const std::uint32_t pcm[7] = {SL_DATAFORMAT_PCM, 1, SL_SAMPLINGRATE_22_05, SL_PCMSAMPLEFORMAT_FIXED_16,
                                SL_PCMSAMPLEFORMAT_FIXED_16, SL_SPEAKER_FRONT_CENTER, SL_BYTEORDER_LITTLEENDIAN};
  r.m->Write(fmt, pcm, sizeof pcm);
  r.Put(src, loc);
  r.Put(src + 4, fmt);
  r.Put(sloc, SL_DATALOCATOR_OUTPUTMIX);
  r.Put(sloc + 4, mix);
  r.Put(snk, sloc);
  r.Put(snk + 4, 0);
  const std::uint32_t ids = o + 0x70, req = o + 0x80;
  r.Put(ids, r.Iid("SL_IID_BUFFERQUEUE"));  // the plain buffer queue answers the same locator on Android
  r.Put(req, SL_BOOLEAN_TRUE);
  r.Put(o + 4, 0);
  CHECK(r.Method(engine, kCreateAudioPlayer, {o + 4, src, snk, 1, ids, req}) == SL_RESULT_SUCCESS);
  const std::uint32_t player = r.U32(o + 4);
  if (player == 0) return;
  r.Method(player, kRealize, {SL_BOOLEAN_FALSE});
  const std::uint32_t itfs = o + 0x90;
  CHECK(r.Method(player, kGetInterface, {r.Iid("SL_IID_PLAY"), itfs}) == SL_RESULT_SUCCESS);
  CHECK(r.Method(player, kGetInterface, {r.Iid("SL_IID_BUFFERQUEUE"), itfs + 4}) == SL_RESULT_SUCCESS);
  const std::uint32_t play = r.U32(itfs), bq = r.U32(itfs + 4);
  std::atomic<std::uint32_t> event{0};
  std::atomic<std::uint32_t> caller{0};
  {
    std::lock_guard<std::mutex> lk(r.mu);
    r.guest_fns[kPlayCallback] = [&](const std::vector<std::uint32_t>& a) {
      if (a.size() == 3) {
        caller = a[0];
        event = a[2];
      }
      r.cv.notify_all();
    };
  }
  CHECK(r.Method(play, kPlayRegisterCallback, {kPlayCallback, 7}) == SL_RESULT_SUCCESS);
  CHECK(r.Method(play, kSetCallbackEventsMask, {SL_PLAYEVENT_HEADATEND | SL_PLAYEVENT_HEADMOVING}) ==
        SL_RESULT_SUCCESS);
  std::vector<std::uint8_t> zero(0x800, 0);
  r.m->Write(kScratch + 0x1000, zero.data(), zero.size());
  CHECK(r.Method(bq, kEnqueue, {kScratch + 0x1000, 0x800}) == SL_RESULT_SUCCESS);
  r.Method(play, kSetPlayState, {SL_PLAYSTATE_PLAYING});
  {
    std::unique_lock<std::mutex> lk(r.mu);
    r.cv.wait_for(lk, std::chrono::seconds(3), [&] { return event.load() != 0; });
  }
  CHECK_CTX((event.load() & (SL_PLAYEVENT_HEADATEND | SL_PLAYEVENT_HEADMOVING)) != 0,
            "event " + std::to_string(event.load()));
  CHECK(caller.load() == play);
  r.Method(play, kSetPlayState, {SL_PLAYSTATE_STOPPED});
  r.Method(player, kDestroy, {});
  r.Method(mix, kDestroy, {});
  r.Method(eobj, kDestroy, {});
  CHECK(r.sles->live_objects() == 0);
}

}  // namespace

int main() {
  iids_are_guest_copies_of_the_host_guids();
  an_engine_is_created_realized_and_destroyed();
  an_unknown_interface_is_unsupported_and_named();
  a_pcm_buffer_queue_player_calls_the_guest_back();
  a_play_callback_gets_its_event();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
