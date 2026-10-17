// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// See thunks_sles.h. Methods run on guest threads with the GGL held (except where `unlocked` drops it around a host
// call); callbacks run on the host's audio threads without it. The maps below are shared by both, so they have their
// own mutex (a leaf: nothing is called while it is held).
#include "thunks_sles.h"

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <dlfcn.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

namespace rb::launcher {

namespace {

std::string Hex(std::uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%x", v);
  return b;
}

/// The interface kinds the guest can call methods on. `kNone`: a known IID with no guest vtable (OutputMix, effects,
/// 3D, ...): creating an object with it works, `GetInterface` on it answers FEATURE_UNSUPPORTED.
enum Kind : int {
  kNone = -1,
  kObject,
  kEngine,
  kPlay,
  kPrefetchStatus,
  kPlaybackRate,
  kSeek,
  kVolume,
  kBufferQueue,
  kSimpleBufferQueue,
  kMuteSolo,
  kEffectSend,
  kAndroidConfiguration,
  kKinds
};

/// One method: a ThunkCaller signature whose first letter after ':' is `self` (passed as a handle), or a hand-written
/// one (`special`), or one this layer does not offer (`unsupported`: FEATURE_UNSUPPORTED, named once).
struct MethodDesc {
  const char* name;
  const char* spec;  ///< null: special or unsupported
  bool unsupported = false;
};
#define SPECIAL(n) MethodDesc{n, nullptr, false}
#define UNSUP(n) MethodDesc{n, nullptr, true}

struct KindDesc {
  const char* name;
  std::vector<MethodDesc> methods;
};

const std::array<KindDesc, kKinds>& Kinds() {
  static const std::array<KindDesc, kKinds> k = {{
      {"SLObjectItf",
       {{"Realize", "u:hu"}, {"Resume", "u:hu"}, {"GetState", "u:hp"}, SPECIAL("GetInterface"),
        SPECIAL("RegisterCallback"), {"AbortAsyncOperation", "v:h"}, SPECIAL("Destroy"), {"SetPriority", "u:hiu"},
        {"GetPriority", "u:hpp"}, UNSUP("SetLossOfControlInterfaces")}},
      {"SLEngineItf",
       {UNSUP("CreateLEDDevice"), UNSUP("CreateVibraDevice"), SPECIAL("CreateAudioPlayer"),
        UNSUP("CreateAudioRecorder"), UNSUP("CreateMidiPlayer"), UNSUP("CreateListener"), UNSUP("Create3DGroup"),
        SPECIAL("CreateOutputMix"), UNSUP("CreateMetadataExtractor"), UNSUP("CreateExtensionObject"),
        {"QueryNumSupportedInterfaces", "u:hup"}, UNSUP("QuerySupportedInterfaces"),
        {"QueryNumSupportedExtensions", "u:hp"}, {"QuerySupportedExtension", "u:hupp"},
        {"IsExtensionSupported", "u:hpp"}}},
      {"SLPlayItf",
       {{"SetPlayState", "u:hu"}, {"GetPlayState", "u:hp"}, {"GetDuration", "u:hp"}, {"GetPosition", "u:hp"},
        SPECIAL("RegisterCallback"), {"SetCallbackEventsMask", "u:hu"}, {"GetCallbackEventsMask", "u:hp"},
        {"SetMarkerPosition", "u:hu"}, {"ClearMarkerPosition", "u:h"}, {"GetMarkerPosition", "u:hp"},
        {"SetPositionUpdatePeriod", "u:hu"}, {"GetPositionUpdatePeriod", "u:hp"}}},
      {"SLPrefetchStatusItf",
       {{"GetPrefetchStatus", "u:hp"}, {"GetFillLevel", "u:hp"}, SPECIAL("RegisterCallback"),
        {"SetCallbackEventsMask", "u:hu"}, {"GetCallbackEventsMask", "u:hp"}, {"SetFillUpdatePeriod", "u:hi"},
        {"GetFillUpdatePeriod", "u:hp"}}},
      {"SLPlaybackRateItf",
       {{"SetRate", "u:hi"}, {"GetRate", "u:hp"}, {"SetPropertyConstraints", "u:hu"}, {"GetProperties", "u:hp"},
        {"GetCapabilitiesOfRate", "u:hip"}, {"GetRateRange", "u:hupppp"}}},
      {"SLSeekItf", {{"SetPosition", "u:huu"}, {"SetLoop", "u:huuu"}, {"GetLoop", "u:hppp"}}},
      {"SLVolumeItf",
       {{"SetVolumeLevel", "u:hi"}, {"GetVolumeLevel", "u:hp"}, {"GetMaxVolumeLevel", "u:hp"}, {"SetMute", "u:hu"},
        {"GetMute", "u:hp"}, {"EnableStereoPosition", "u:hu"}, {"IsEnabledStereoPosition", "u:hp"},
        {"SetStereoPosition", "u:hi"}, {"GetStereoPosition", "u:hp"}}},
      {"SLBufferQueueItf",
       {{"Enqueue", "u:hpu"}, {"Clear", "u:h"}, {"GetState", "u:hp"}, SPECIAL("RegisterCallback")}},
      {"SLAndroidSimpleBufferQueueItf",
       {{"Enqueue", "u:hpu"}, {"Clear", "u:h"}, {"GetState", "u:hp"}, SPECIAL("RegisterCallback")}},
      {"SLMuteSoloItf",
       {{"SetChannelMute", "u:huu"}, {"GetChannelMute", "u:hup"}, {"SetChannelSolo", "u:huu"},
        {"GetChannelSolo", "u:hup"}, {"GetNumChannels", "u:hp"}}},
      {"SLEffectSendItf",
       {UNSUP("EnableEffectSend"), UNSUP("IsEnabled"), {"SetDirectLevel", "u:hi"}, {"GetDirectLevel", "u:hp"},
        UNSUP("SetSendLevel"), UNSUP("GetSendLevel")}},
      {"SLAndroidConfigurationItf",
       {{"SetConfiguration", "u:hppu"}, {"GetConfiguration", "u:hppp"}, UNSUP("AcquireJavaProxy"),
        {"ReleaseJavaProxy", "u:hu"}}},
  }};
  return k;
}
#undef SPECIAL
#undef UNSUP

struct IidName {
  const char* name;
  Kind kind;
};
/// Every SL_IID_* of OpenSLES.h and OpenSLES_Android.h, with the guest vtable each answers (if any).
const std::vector<IidName>& IidNames() {
  static const std::vector<IidName> k = {
      {"SL_IID_NULL", kNone}, {"SL_IID_OBJECT", kNone}, {"SL_IID_ENGINE", kEngine},
      {"SL_IID_ENGINECAPABILITIES", kNone}, {"SL_IID_PLAY", kPlay}, {"SL_IID_PREFETCHSTATUS", kPrefetchStatus},
      {"SL_IID_PLAYBACKRATE", kPlaybackRate}, {"SL_IID_SEEK", kSeek}, {"SL_IID_VOLUME", kVolume},
      {"SL_IID_BUFFERQUEUE", kBufferQueue}, {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE", kSimpleBufferQueue},
      {"SL_IID_MUTESOLO", kMuteSolo}, {"SL_IID_EFFECTSEND", kEffectSend},
      {"SL_IID_ANDROIDCONFIGURATION", kAndroidConfiguration}, {"SL_IID_OUTPUTMIX", kNone},
      {"SL_IID_3DCOMMIT", kNone}, {"SL_IID_3DDOPPLER", kNone}, {"SL_IID_3DGROUPING", kNone},
      {"SL_IID_3DLOCATION", kNone}, {"SL_IID_3DMACROSCOPIC", kNone}, {"SL_IID_3DSOURCE", kNone},
      {"SL_IID_ANDROIDACOUSTICECHOCANCELLATION", kNone}, {"SL_IID_ANDROIDAUTOMATICGAINCONTROL", kNone},
      {"SL_IID_ANDROIDBUFFERQUEUESOURCE", kNone}, {"SL_IID_ANDROIDEFFECT", kNone},
      {"SL_IID_ANDROIDEFFECTCAPABILITIES", kNone}, {"SL_IID_ANDROIDEFFECTSEND", kNone},
      {"SL_IID_ANDROIDNOISESUPPRESSION", kNone}, {"SL_IID_AUDIODECODERCAPABILITIES", kNone},
      {"SL_IID_AUDIOENCODER", kNone}, {"SL_IID_AUDIOENCODERCAPABILITIES", kNone},
      {"SL_IID_AUDIOIODEVICECAPABILITIES", kNone}, {"SL_IID_BASSBOOST", kNone}, {"SL_IID_DEVICEVOLUME", kNone},
      {"SL_IID_DYNAMICINTERFACEMANAGEMENT", kNone}, {"SL_IID_DYNAMICSOURCE", kNone},
      {"SL_IID_ENVIRONMENTALREVERB", kNone}, {"SL_IID_EQUALIZER", kNone}, {"SL_IID_LED", kNone},
      {"SL_IID_METADATAEXTRACTION", kNone}, {"SL_IID_METADATATRAVERSAL", kNone}, {"SL_IID_MIDIMESSAGE", kNone},
      {"SL_IID_MIDIMUTESOLO", kNone}, {"SL_IID_MIDITEMPO", kNone}, {"SL_IID_MIDITIME", kNone},
      {"SL_IID_PITCH", kNone}, {"SL_IID_PRESETREVERB", kNone}, {"SL_IID_RATEPITCH", kNone},
      {"SL_IID_RECORD", kNone}, {"SL_IID_THREADSYNC", kNone}, {"SL_IID_VIBRA", kNone},
      {"SL_IID_VIRTUALIZER", kNone}, {"SL_IID_VISUALIZATION", kNone},
  };
  return k;
}

}  // namespace

struct Sles::Impl {
  struct Obj;
  struct Itf {
    Kind kind = kNone;
    void* host = nullptr;  ///< the host interface (a pointer to a pointer to its vtable)
    std::uint32_t handle = 0;
    std::uint32_t cell = 0;
    Obj* obj = nullptr;
  };
  struct Callback {
    Impl* impl;
    std::uint32_t fn, ctx, self;
  };
  struct Obj {
    SLObjectItf host = nullptr;
    std::map<int, std::unique_ptr<Itf>> itfs;  ///< by kind; kObject is the object's own cell
    std::vector<std::unique_ptr<Callback>> callbacks;  ///< freed only after the host Destroy (no callback after it)
    std::vector<std::string> strings;                   ///< URIs and MIME types handed to the host
  };
  struct Iid {
    std::string name;
    Kind kind;
    SLInterfaceID host;
    std::uint32_t cell = 0;  ///< the guest SL_IID_x variable, once resolved
  };

  SlesHost host;
  void* lib = nullptr;
  bool lib_tried = false;
  std::vector<Iid> iids;
  std::array<std::uint32_t, kKinds> vtables{};
  std::uint32_t bump = 0, bump_end = 0;
  std::vector<std::uint32_t> free_cells;

  mutable std::mutex mu;  ///< guards the three below; a leaf
  std::map<std::uint32_t, Itf*> by_cell;
  std::map<void*, std::uint32_t> by_host;
  std::map<Obj*, std::unique_ptr<Obj>> objs;
  std::set<std::string> said;

  explicit Impl(SlesHost h) : host(std::move(h)) {}

  void Say(const std::string& key, const std::string& text) {
    {
      std::lock_guard<std::mutex> lk(mu);
      if (!said.insert(key).second) return;
    }
    if (host.say) host.say("sles: " + text);
  }

  bool Load() {
    if (lib_tried) return lib != nullptr;
    lib_tried = true;
    lib = dlopen("libOpenSLES.so", RTLD_NOW);
    if (lib == nullptr) return false;
    for (const IidName& n : IidNames()) {
      auto* sym = static_cast<const SLInterfaceID*>(dlsym(lib, n.name));
      if (sym != nullptr && *sym != nullptr) iids.push_back(Iid{n.name, n.kind, *sym});
    }
    return true;
  }

  /// `bytes` of guest memory from a private bump area (never freed: IIDs, vtables, cells).
  std::uint32_t Bump(std::uint32_t bytes) {
    bytes = (bytes + 7u) & ~7u;
    if (bump == 0 || bump + bytes > bump_end) {
      const std::uint32_t at = host.alloc(0x1000);
      if (at == 0) return 0;
      bump = at;
      bump_end = at + 0x1000;
    }
    const std::uint32_t at = bump;
    bump += bytes;
    return at;
  }

  std::uint32_t U32(std::uint32_t at) const {
    std::uint32_t v = 0;
    host.mem->Read(at, &v, 4);
    return v;
  }
  bool ReadOk(std::uint32_t at, void* dst, std::size_t n) const { return at != 0 && host.mem->Read(at, dst, n); }
  bool Put(std::uint32_t at, std::uint32_t v) { return at != 0 && host.mem->Write(at, &v, 4); }
  /// Argument `i` of a guest call (r0..r3, then the stack).
  std::uint32_t Arg(const exec::MachineState& s, unsigned i) const {
    return i < 4 ? s.r[i] : U32(s.r[13] + 4u * (i - 4u));
  }
  bool CString(std::uint32_t at, std::string* out) const {
    out->clear();
    for (std::uint32_t i = 0; i < 4096; ++i) {
      char c = 0;
      if (!host.mem->Read(at + i, &c, 1)) return false;
      if (c == '\0') return true;
      out->push_back(c);
    }
    return false;
  }

  /// The host IID with the guest GUID at `at`, or null (and `*kind` kNone).
  const Iid* FindIid(std::uint32_t at) const {
    unsigned char g[sizeof(SLInterfaceID_)];
    if (!ReadOk(at, g, sizeof g)) return nullptr;
    for (const Iid& i : iids) {
      if (std::memcmp(g, i.host, sizeof g) == 0) return &i;
    }
    return nullptr;
  }
  std::string GuidText(std::uint32_t at) const {
    SLInterfaceID_ g{};
    if (!ReadOk(at, &g, sizeof g)) return "an unreadable IID at " + Hex(at);
    char b[48];
    std::snprintf(b, sizeof b, "IID %08x-%04x-%04x", static_cast<unsigned>(g.time_low),
                  static_cast<unsigned>(g.time_mid), static_cast<unsigned>(g.time_hi_and_version));
    return b;
  }

  Itf* Find(std::uint32_t cell) {
    std::lock_guard<std::mutex> lk(mu);
    const auto it = by_cell.find(cell);
    return it == by_cell.end() ? nullptr : it->second;
  }
  std::uint32_t GuestOf(const void* host_itf) {
    std::lock_guard<std::mutex> lk(mu);
    const auto it = by_host.find(const_cast<void*>(host_itf));
    return it == by_host.end() ? 0u : it->second;
  }

  /// A guest cell for `host_itf` of `kind` on `o` (made once per kind).
  std::uint32_t AddItf(Obj* o, Kind kind, void* host_itf) {
    if (const auto it = o->itfs.find(kind); it != o->itfs.end()) return it->second->cell;
    const std::uint32_t vt = Vtable(kind);
    if (vt == 0) return 0;
    std::uint32_t cell = 0;
    {
      std::lock_guard<std::mutex> lk(mu);
      if (!free_cells.empty()) {
        cell = free_cells.back();
        free_cells.pop_back();
      }
    }
    if (cell == 0) cell = Bump(4);
    if (cell == 0 || !Put(cell, vt)) return 0;
    auto itf = std::make_unique<Itf>();
    itf->kind = kind;
    itf->host = host_itf;
    itf->handle = host.handles->Put(host_itf);
    itf->cell = cell;
    itf->obj = o;
    {
      std::lock_guard<std::mutex> lk(mu);
      by_cell[cell] = itf.get();
      by_host[host_itf] = cell;
    }
    o->itfs[kind] = std::move(itf);
    return cell;
  }

  /// A new guest object for `host_obj`: its object cell.
  std::uint32_t NewObject(SLObjectItf host_obj, std::vector<std::string> strings = {}) {
    auto o = std::make_unique<Obj>();
    o->host = host_obj;
    o->strings = std::move(strings);
    Obj* raw = o.get();
    {
      std::lock_guard<std::mutex> lk(mu);
      objs[raw] = std::move(o);
    }
    const std::uint32_t cell = AddItf(raw, kObject, const_cast<void*>(static_cast<const void*>(host_obj)));
    if (cell == 0) {
      (*host_obj)->Destroy(host_obj);
      std::lock_guard<std::mutex> lk(mu);
      objs.erase(raw);
    }
    return cell;
  }

  /// The guest vtable for `kind`: one slot per method, bound on first use.
  std::uint32_t Vtable(Kind kind) {
    if (kind < 0 || kind >= kKinds) return 0;
    if (vtables[kind] != 0) return vtables[kind];
    const KindDesc& d = Kinds()[kind];
    const std::uint32_t vt = Bump(static_cast<std::uint32_t>(4 * d.methods.size()));
    if (vt == 0) return 0;
    for (std::size_t i = 0; i < d.methods.size(); ++i) {
      const unsigned index = static_cast<unsigned>(i);
      const std::uint32_t slot =
          host.bind(std::string{d.name} + "::" + d.methods[i].name,
                    [this, kind, index](exec::MachineState& s, std::string* why) { return Method(kind, index, s, why); });
      if (!Put(vt + 4u * index, slot)) return 0;
    }
    vtables[kind] = vt;
    return vt;
  }

  // ---- methods
  bool Method(Kind kind, unsigned index, exec::MachineState& s, std::string* why) {
    const MethodDesc& m = Kinds()[kind].methods[index];
    const std::string what = std::string{Kinds()[kind].name} + "::" + m.name;
    Itf* itf = Find(s.r[0]);
    if (itf == nullptr || itf->kind != kind) {
      Say(what + ":self", what + " on " + Hex(s.r[0]) + ", which is no live interface of that kind (destroyed?)");
      s.r[0] = SL_RESULT_PRECONDITIONS_VIOLATED;
      return true;
    }
    if (m.unsupported) {
      Say(what, what + " is not offered to the guest: FEATURE_UNSUPPORTED");
      s.r[0] = SL_RESULT_FEATURE_UNSUPPORTED;
      return true;
    }
    if (m.spec == nullptr) return Special(kind, m.name, *itf, s, why);
    void* fn = (*static_cast<void* const* const*>(itf->host))[index];
    exec::MachineState call = s;
    call.r[0] = itf->handle;
    // No pointer, at most four words: nothing in guest memory is read or written, so the GGL can go (the host may
    // wait for an audio thread that waits for the GGL).
    const char* args = std::strchr(m.spec, ':') + 1;
    const bool release = std::strchr(args, 'p') == nullptr && std::strlen(args) <= 4;
    bool ok = false;
    std::string e;
    if (release) {
      host.unlocked([&] { ok = abi::ThunkCaller::Call(m.spec, fn, call, *host.mem, *host.handles, &e); });
    } else {
      ok = abi::ThunkCaller::Call(m.spec, fn, call, *host.mem, *host.handles, &e);
    }
    if (!ok) {
      Say(what + ":refused", what + " refused: " + e);
      s.r[0] = SL_RESULT_PARAMETER_INVALID;
      return true;
    }
    s.r[0] = call.r[0];
    s.r[1] = call.r[1];
    return true;
  }

  bool Special(Kind kind, const std::string& name, Itf& itf, exec::MachineState& s, std::string* why) {
    if (name == "GetInterface") return GetInterface(itf, s);
    if (name == "Destroy") return Destroy(itf, s);
    if (name == "RegisterCallback") return RegisterCallback(kind, itf, s);
    if (name == "CreateOutputMix") return CreateOutputMix(itf, s);
    if (name == "CreateAudioPlayer") return CreateAudioPlayer(itf, s);
    *why = "no special for " + name;
    return false;
  }

  bool GetInterface(Itf& self, exec::MachineState& s) {
    const std::uint32_t giid = s.r[1], out = s.r[2];
    const Iid* iid = FindIid(giid);
    if (iid == nullptr || iid->kind == kNone || iid->kind == kObject) {
      const std::string n = iid ? iid->name : GuidText(giid);
      Say("GetInterface:" + n, "GetInterface(" + n + ") has no guest interface: FEATURE_UNSUPPORTED");
      s.r[0] = SL_RESULT_FEATURE_UNSUPPORTED;
      return true;
    }
    Obj* o = self.obj;
    if (const auto it = o->itfs.find(iid->kind); it != o->itfs.end()) {
      s.r[0] = Put(out, it->second->cell) ? SL_RESULT_SUCCESS : SL_RESULT_PARAMETER_INVALID;
      return true;
    }
    void* host_itf = nullptr;
    const SLresult r = (*o->host)->GetInterface(o->host, iid->host, &host_itf);
    if (r != SL_RESULT_SUCCESS || host_itf == nullptr) {
      s.r[0] = r == SL_RESULT_SUCCESS ? SL_RESULT_INTERNAL_ERROR : r;
      return true;
    }
    const std::uint32_t cell = AddItf(o, iid->kind, host_itf);
    s.r[0] = cell != 0 && Put(out, cell) ? SL_RESULT_SUCCESS : SL_RESULT_MEMORY_FAILURE;
    return true;
  }

  bool Destroy(Itf& self, exec::MachineState&) {
    Obj* o = self.obj;
    SLObjectItf h = o->host;
    host.unlocked([&] { (*h)->Destroy(h); });  // waits out a running callback, which may want the GGL
    std::unique_ptr<Obj> gone;
    {
      std::lock_guard<std::mutex> lk(mu);
      for (auto& [k, itf] : o->itfs) {
        by_cell.erase(itf->cell);
        by_host.erase(itf->host);
        free_cells.push_back(itf->cell);
      }
      const auto it = objs.find(o);
      if (it != objs.end()) {
        gone = std::move(it->second);
        objs.erase(it);
      }
    }
    for (auto& [k, itf] : o->itfs) host.handles->Forget(itf->host);
    return true;  // `gone` frees the callbacks: the host promised none runs after Destroy
  }

  // One line the first time audio flows: a game's log then shows the whole path worked, not only that nothing failed.
  std::atomic<bool> first_callback{false};
  void FirstCallback() {
    if (!first_callback.exchange(true)) Say("callback", "first buffer-queue callback into the guest: audio is flowing");
  }

  // ---- callbacks (host audio threads)
  static void BqTramp(SLBufferQueueItf, void* ctx) {
    auto* c = static_cast<Callback*>(ctx);
    c->impl->FirstCallback();
    c->impl->host.call_guest(c->fn, {c->self, c->ctx});
  }
  static void SimpleBqTramp(SLAndroidSimpleBufferQueueItf, void* ctx) {
    auto* c = static_cast<Callback*>(ctx);
    c->impl->FirstCallback();
    c->impl->host.call_guest(c->fn, {c->self, c->ctx});
  }
  static void PlayTramp(SLPlayItf, void* ctx, SLuint32 event) {
    auto* c = static_cast<Callback*>(ctx);
    c->impl->host.call_guest(c->fn, {c->self, c->ctx, event});
  }
  static void PrefetchTramp(SLPrefetchStatusItf, void* ctx, SLuint32 event) {
    auto* c = static_cast<Callback*>(ctx);
    c->impl->host.call_guest(c->fn, {c->self, c->ctx, event});
  }
  static void ObjectTramp(SLObjectItf, const void* ctx, SLuint32 event, SLresult result, SLuint32 param,
                          void* itf) {
    auto* c = static_cast<Callback*>(const_cast<void*>(ctx));
    c->impl->host.call_guest(c->fn, {c->self, c->ctx, event, result, param, itf ? c->impl->GuestOf(itf) : 0u});
  }

  bool RegisterCallback(Kind kind, Itf& self, exec::MachineState& s) {
    const std::uint32_t fn = s.r[1], ctx = s.r[2];
    std::unique_ptr<Callback> c;
    if (fn != 0) c = std::make_unique<Callback>(Callback{this, fn, ctx, self.cell});
    void* hc = c.get();
    SLresult r = SL_RESULT_FEATURE_UNSUPPORTED;
    switch (kind) {
      case kBufferQueue: {
        auto h = static_cast<SLBufferQueueItf>(self.host);
        r = (*h)->RegisterCallback(h, c ? &BqTramp : nullptr, hc);
        break;
      }
      case kSimpleBufferQueue: {
        auto h = static_cast<SLAndroidSimpleBufferQueueItf>(self.host);
        r = (*h)->RegisterCallback(h, c ? &SimpleBqTramp : nullptr, hc);
        break;
      }
      case kPlay: {
        auto h = static_cast<SLPlayItf>(self.host);
        r = (*h)->RegisterCallback(h, c ? &PlayTramp : nullptr, hc);
        break;
      }
      case kPrefetchStatus: {
        auto h = static_cast<SLPrefetchStatusItf>(self.host);
        r = (*h)->RegisterCallback(h, c ? &PrefetchTramp : nullptr, hc);
        break;
      }
      case kObject: {
        auto h = static_cast<SLObjectItf>(self.host);
        r = (*h)->RegisterCallback(h, c ? &ObjectTramp : nullptr, hc);
        break;
      }
      default:
        break;
    }
    // Kept even when replaced: the host may be inside the old one right now. Freed with the object.
    if (r == SL_RESULT_SUCCESS && c) self.obj->callbacks.push_back(std::move(c));
    s.r[0] = r;
    return true;
  }

  // ---- object creation
  /// The guest's interface-ID and required arrays as the host's. An IID this device does not know is dropped when
  /// optional and fails the creation (FEATURE_UNSUPPORTED) when required.
  SLresult Ids(std::uint32_t n, std::uint32_t ids, std::uint32_t req, std::vector<SLInterfaceID>* out,
               std::vector<SLboolean>* out_req, const char* what) {
    if (n > 64) return SL_RESULT_PARAMETER_INVALID;
    for (std::uint32_t i = 0; i < n; ++i) {
      const std::uint32_t g = U32(ids + 4u * i);
      const SLboolean r = req != 0 ? U32(req + 4u * i) : SL_BOOLEAN_FALSE;
      const Iid* iid = FindIid(g);
      if (iid == nullptr) {
        Say(std::string{what} + GuidText(g), std::string{what} + " asked for " + GuidText(g) +
                                                  ", which this device does not know" + (r ? ": refused" : ": dropped"));
        if (r) return SL_RESULT_FEATURE_UNSUPPORTED;
        continue;
      }
      out->push_back(iid->host);
      out_req->push_back(r ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE);
    }
    return SL_RESULT_SUCCESS;
  }

  struct Data {
    union {
      SLuint32 type;
      SLDataLocator_AndroidSimpleBufferQueue asbq;
      SLDataLocator_BufferQueue bq;
      SLDataLocator_AndroidFD fd;
      SLDataLocator_URI uri;
      SLDataLocator_OutputMix mix;
      SLDataLocator_Address addr;
    } loc{};
    union {
      SLuint32 type;
      SLDataFormat_PCM pcm;
      SLAndroidDataFormat_PCM_EX pcmex;
      SLDataFormat_MIME mime;
    } fmt{};
    SLDataSource src{};  // also used as a sink (same layout)
  };

  /// A guest SLDataSource/SLDataSink at `at` as the host's, in `d` (strings appended to `strings`).
  SLresult Translate(std::uint32_t at, Data* d, std::vector<std::string>* strings, const char* what) {
    const std::uint32_t gloc = U32(at), gfmt = U32(at + 4);
    if (at == 0 || gloc == 0) return SL_RESULT_PARAMETER_INVALID;
    const std::uint32_t type = U32(gloc);
    switch (type) {
      case SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE:
      case SL_DATALOCATOR_BUFFERQUEUE:
        d->loc.bq.locatorType = type;
        d->loc.bq.numBuffers = U32(gloc + 4);
        break;
      case SL_DATALOCATOR_ANDROIDFD: {
        // {u32 type; i32 fd; i64 offset; i64 length}: the same offsets on armeabi and arm64. Guest fds are host fds.
        d->loc.fd.locatorType = type;
        d->loc.fd.fd = static_cast<SLint32>(U32(gloc + 4));
        std::int64_t off = 0, len = 0;
        host.mem->Read(gloc + 8, &off, 8);
        host.mem->Read(gloc + 16, &len, 8);
        d->loc.fd.offset = off;
        d->loc.fd.length = len;
        break;
      }
      case SL_DATALOCATOR_URI: {
        std::string u;
        if (!CString(U32(gloc + 4), &u)) return SL_RESULT_PARAMETER_INVALID;
        strings->push_back(std::move(u));
        d->loc.uri.locatorType = type;
        d->loc.uri.URI = reinterpret_cast<SLchar*>(strings->back().data());
        break;
      }
      case SL_DATALOCATOR_OUTPUTMIX: {
        Itf* mix = Find(U32(gloc + 4));
        if (mix == nullptr || mix->kind != kObject) {
          Say(std::string{what} + "mix", std::string{what} + ": the output mix " + Hex(U32(gloc + 4)) +
                                             " is no live object");
          return SL_RESULT_PARAMETER_INVALID;
        }
        d->loc.mix.locatorType = type;
        d->loc.mix.outputMix = static_cast<SLObjectItf>(mix->host);
        break;
      }
      case SL_DATALOCATOR_ADDRESS: {
        const std::uint32_t p = U32(gloc + 4), n = U32(gloc + 8);
        if (n == 0 || !host.mem->IsMapped(p) || !host.mem->IsMapped(p + n - 1)) return SL_RESULT_PARAMETER_INVALID;
        d->loc.addr.locatorType = type;
        d->loc.addr.pAddress = host.mem->Host(p);
        d->loc.addr.length = n;
        break;
      }
      default:
        Say(std::string{what} + "loc" + Hex(type),
            std::string{what} + ": data locator " + Hex(type) + " is not translated: FEATURE_UNSUPPORTED");
        return SL_RESULT_FEATURE_UNSUPPORTED;
    }
    d->src.pLocator = &d->loc;
    d->src.pFormat = nullptr;
    if (gfmt == 0) return SL_RESULT_SUCCESS;
    const std::uint32_t ftype = U32(gfmt);
    switch (ftype) {
      case SL_DATAFORMAT_PCM:
        if (!ReadOk(gfmt, &d->fmt.pcm, sizeof d->fmt.pcm)) return SL_RESULT_PARAMETER_INVALID;
        break;
      case SL_ANDROID_DATAFORMAT_PCM_EX:
        if (!ReadOk(gfmt, &d->fmt.pcmex, sizeof d->fmt.pcmex)) return SL_RESULT_PARAMETER_INVALID;
        break;
      case SL_DATAFORMAT_MIME: {
        std::string m;
        const std::uint32_t gm = U32(gfmt + 4);
        if (gm != 0 && !CString(gm, &m)) return SL_RESULT_PARAMETER_INVALID;
        strings->push_back(std::move(m));
        d->fmt.mime.formatType = ftype;
        d->fmt.mime.mimeType = gm != 0 ? reinterpret_cast<SLchar*>(strings->back().data()) : nullptr;
        d->fmt.mime.containerType = U32(gfmt + 8);
        break;
      }
      default:
        Say(std::string{what} + "fmt" + Hex(ftype),
            std::string{what} + ": data format " + Hex(ftype) + " is not translated: FEATURE_UNSUPPORTED");
        return SL_RESULT_FEATURE_UNSUPPORTED;
    }
    d->src.pFormat = &d->fmt;
    return SL_RESULT_SUCCESS;
  }

  bool CreateOutputMix(Itf& self, exec::MachineState& s) {
    const std::uint32_t out = Arg(s, 1), n = Arg(s, 2), ids = Arg(s, 3), req = Arg(s, 4);
    std::vector<SLInterfaceID> hids;
    std::vector<SLboolean> hreq;
    SLresult r = Ids(n, ids, req, &hids, &hreq, "CreateOutputMix");
    SLObjectItf made = nullptr;
    auto e = static_cast<SLEngineItf>(self.host);
    if (r == SL_RESULT_SUCCESS) {
      r = (*e)->CreateOutputMix(e, &made, static_cast<SLuint32>(hids.size()), hids.data(), hreq.data());
    }
    s.r[0] = Finish(r, made, out, {});
    return true;
  }

  bool CreateAudioPlayer(Itf& self, exec::MachineState& s) {
    const std::uint32_t out = Arg(s, 1), gsrc = Arg(s, 2), gsnk = Arg(s, 3), n = Arg(s, 4), ids = Arg(s, 5),
                        req = Arg(s, 6);
    std::vector<std::string> strings;
    strings.reserve(4);  // pointers into these are handed to the host: no reallocation
    Data src, snk;
    std::vector<SLInterfaceID> hids;
    std::vector<SLboolean> hreq;
    SLresult r = Translate(gsrc, &src, &strings, "CreateAudioPlayer source");
    if (r == SL_RESULT_SUCCESS) r = Translate(gsnk, &snk, &strings, "CreateAudioPlayer sink");
    if (r == SL_RESULT_SUCCESS) r = Ids(n, ids, req, &hids, &hreq, "CreateAudioPlayer");
    SLObjectItf made = nullptr;
    auto e = static_cast<SLEngineItf>(self.host);
    if (r == SL_RESULT_SUCCESS) {
      SLDataSink sink{snk.src.pLocator, snk.src.pFormat};
      r = (*e)->CreateAudioPlayer(e, &made, &src.src, &sink, static_cast<SLuint32>(hids.size()), hids.data(),
                                  hreq.data());
    }
    {
      const std::string k = Hex(src.loc.type) + "/" + (src.src.pFormat ? Hex(src.fmt.type) : std::string{"-"}) + " -> " + Hex(r);
      Say("player " + k, "CreateAudioPlayer: source locator/format " + k);
    }
    s.r[0] = Finish(r, made, out, std::move(strings));
    return true;
  }

  /// A created host object's guest cell written to `out`; the host object is destroyed when that cannot be done.
  SLresult Finish(SLresult r, SLObjectItf made, std::uint32_t out, std::vector<std::string> strings) {
    if (r != SL_RESULT_SUCCESS || made == nullptr) return r == SL_RESULT_SUCCESS ? SL_RESULT_INTERNAL_ERROR : r;
    const std::uint32_t cell = NewObject(made, std::move(strings));
    if (cell == 0) return SL_RESULT_MEMORY_FAILURE;
    if (!Put(out, cell)) {
      // The guest can never name the object: destroy it here rather than leave a live host object behind.
      if (Itf* itf = Find(cell)) {
        exec::MachineState unused{};
        Destroy(*itf, unused);
      }
      Say("out " + Hex(out), "an object was made but its out-pointer " + Hex(out) +
                                 " is not writable guest memory: destroyed, PARAMETER_INVALID");
      return SL_RESULT_PARAMETER_INVALID;
    }
    return SL_RESULT_SUCCESS;
  }

  bool CreateEngine(exec::MachineState& s, std::string* why) {
    using Fn = SLresult (*)(SLObjectItf*, SLuint32, const SLEngineOption*, SLuint32, const SLInterfaceID*,
                            const SLboolean*);
    auto fn = reinterpret_cast<Fn>(dlsym(lib, "slCreateEngine"));
    if (fn == nullptr) {
      *why = "this device's libOpenSLES has no slCreateEngine";
      return false;
    }
    const std::uint32_t out = Arg(s, 0), nopt = Arg(s, 1), gopt = Arg(s, 2), n = Arg(s, 3), ids = Arg(s, 4),
                        req = Arg(s, 5);
    std::vector<SLEngineOption> opts;
    SLresult r = nopt > 16 ? SL_RESULT_PARAMETER_INVALID : SL_RESULT_SUCCESS;
    for (std::uint32_t i = 0; r == SL_RESULT_SUCCESS && i < nopt; ++i) {
      opts.push_back(SLEngineOption{U32(gopt + 8u * i), U32(gopt + 8u * i + 4u)});
    }
    std::vector<SLInterfaceID> hids;
    std::vector<SLboolean> hreq;
    if (r == SL_RESULT_SUCCESS) r = Ids(n, ids, req, &hids, &hreq, "slCreateEngine");
    SLObjectItf made = nullptr;
    if (r == SL_RESULT_SUCCESS) {
      r = fn(&made, static_cast<SLuint32>(opts.size()), opts.empty() ? nullptr : opts.data(),
             static_cast<SLuint32>(hids.size()), hids.data(), hreq.data());
    }
    Say("engine " + Hex(r), "slCreateEngine -> " + Hex(r));
    s.r[0] = Finish(r, made, out, {});
    return true;
  }
};

Sles::Sles(SlesHost host) : impl_{std::make_unique<Impl>(std::move(host))} {}
Sles::~Sles() = default;

std::optional<std::uint32_t> Sles::Resolve(const std::string& name) {
  Impl& m = *impl_;
  if (name.rfind("sl", 0) != 0 && name.rfind("SL_IID_", 0) != 0) return std::nullopt;
  if (!m.Load()) return std::nullopt;
  if (name == "slCreateEngine") {
    return m.host.bind(name, [&m](exec::MachineState& s, std::string* why) { return m.CreateEngine(s, why); });
  }
  if (name == "slQueryNumSupportedEngineInterfaces") {
    void* fn = dlsym(m.lib, name.c_str());
    if (fn == nullptr) return std::nullopt;
    return m.host.bind(name, [&m, fn](exec::MachineState& s, std::string* why) {
      return abi::ThunkCaller::Call("u:p", fn, s, *m.host.mem, *m.host.handles, why);
    });
  }
  if (name == "slQuerySupportedEngineInterfaces") {
    return m.host.bind(name, [&m](exec::MachineState& s, std::string*) {
      m.Say("slQuerySupportedEngineInterfaces", "slQuerySupportedEngineInterfaces is not offered: FEATURE_UNSUPPORTED");
      s.r[0] = SL_RESULT_FEATURE_UNSUPPORTED;
      return true;
    });
  }
  for (Impl::Iid& i : m.iids) {
    if (i.name != name) continue;
    if (i.cell == 0) {
      // The variable (one word) and the GUID it points at, both guest memory.
      const std::uint32_t at = m.Bump(4 + sizeof(SLInterfaceID_));
      if (at == 0 || !m.host.mem->Write(at + 4, i.host, sizeof(SLInterfaceID_)) || !m.Put(at, at + 4)) {
        return std::nullopt;
      }
      i.cell = at;
    }
    return i.cell;
  }
  return std::nullopt;
}

std::size_t Sles::live_objects() const {
  std::lock_guard<std::mutex> lk(impl_->mu);
  return impl_->objs.size();
}

}  // namespace rb::launcher
