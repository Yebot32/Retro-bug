// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// See silent_sles.h. Guest ABI notes:
//   * an interface pointer is a guest word whose value is a guest vtable address;
//   * `self` is that same guest word, so each interface cell is also the host's key;
//   * method slots are ordinary host slots, resolved once when the vtable is built.
//
// Only the interfaces a silent OpenAL/DLGE startup reaches are exposed. Anything else answers
// FEATURE_UNSUPPORTED, which is the same answer a real device gives for an interface it lacks.
#include "rb/audio/silent_sles.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "rb/mem/guest_memory.h"

namespace rb::audio {
namespace {

using Method = std::function<bool(exec::MachineState&, std::string*)>;

constexpr std::uint32_t kSuccess = 0;
constexpr std::uint32_t kParameterInvalid = 2;
constexpr std::uint32_t kPreconditionsViolated = 1;
constexpr std::uint32_t kFeatureUnsupported = 12;
constexpr std::uint32_t kObjectStateRealized = 1;
constexpr std::uint32_t kPlayStateStopped = 1;

enum class Facet : unsigned { Object, Engine, Play, BufferQueue, SimpleBufferQueue, Count };
enum class ObjKind : unsigned { Engine, OutputMix, Player };
enum class Iid : unsigned { Engine, Play, BufferQueue, SimpleBufferQueue, Count };

struct FacetDesc {
  const char* name;
  const char* const* methods;
  unsigned count;
};

const char* const kObjectMethods[] = {
    "Realize",          "Resume",       "GetState",         "GetInterface", "RegisterCallback",
    "AbortAsyncOperation", "Destroy",   "SetPriority",      "GetPriority",  "SetLossOfControlInterfaces",
};
const char* const kEngineMethods[] = {
    "CreateLEDDevice",          "CreateVibraDevice",       "CreateAudioPlayer",      "CreateAudioRecorder",
    "CreateMidiPlayer",         "CreateListener",          "Create3DGroup",          "CreateOutputMix",
    "CreateMetadataExtractor",  "CreateExtensionObject",   "QueryNumSupportedInterfaces",
    "QuerySupportedInterfaces", "QueryNumSupportedExtensions", "QuerySupportedExtension",
    "IsExtensionSupported",
};
const char* const kPlayMethods[] = {
    "SetPlayState",   "GetPlayState",   "GetDuration",  "GetPosition",   "RegisterCallback",
    "SetCallbackEventsMask", "GetCallbackEventsMask", "SetMarkerPosition", "ClearMarkerPosition",
    "GetMarkerPosition", "SetPositionUpdatePeriod", "GetPositionUpdatePeriod",
};
const char* const kBufferQueueMethods[] = {"Enqueue", "Clear", "GetState", "RegisterCallback"};
const char* const kSimpleBufferQueueMethods[] = {"Enqueue", "Clear", "GetState", "RegisterCallback"};

const FacetDesc& Desc(Facet f) {
  static const FacetDesc k[] = {
      {"Object", kObjectMethods, 10},
      {"Engine", kEngineMethods, 15},
      {"Play", kPlayMethods, 12},
      {"BufferQueue", kBufferQueueMethods, 4},
      {"SimpleBufferQueue", kSimpleBufferQueueMethods, 4},
  };
  return k[static_cast<unsigned>(f)];
}

std::string HexString(std::uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%x", v);
  return b;
}

}  // namespace

struct SilentSles::Impl {
  struct Obj {
    ObjKind kind = ObjKind::Engine;
    bool realized = false;
    std::uint32_t object_cell = 0;
    std::uint32_t play_state = kPlayStateStopped;
    std::uint32_t queued = 0;
    std::map<Facet, std::uint32_t> cells;
  };
  struct Itf {
    Obj* obj = nullptr;
    Facet facet = Facet::Object;
  };

  SilentSlesHost host;

  std::array<std::uint32_t, static_cast<unsigned>(Facet::Count)> vtables{};
  std::array<std::uint32_t, static_cast<unsigned>(Iid::Count)> iid_objects{};
  std::map<std::uint32_t, Iid> iid_at;  ///< guest GUID object address -> kind
  std::map<std::uint32_t, std::unique_ptr<Itf>> itfs;
  std::map<std::uint32_t, std::unique_ptr<Obj>> objects;
  std::set<std::string> said;

  std::uint32_t bump = 0;
  std::uint32_t bump_end = 0;

  explicit Impl(SilentSlesHost h) : host(std::move(h)) {
    RegisterInterfaceIds();
    BindEntryPoints();
    // Build every vtable before the guest runs: resolving a name while dispatching a host slot
    // would re-enter the bridge's slot table.
    for (unsigned i = 0; i < static_cast<unsigned>(Facet::Count); ++i) Vtable(static_cast<Facet>(i));
    Say("backend", "sles: silent backend installed (OpenSL object model; audio output is muted)");
  }

  void Say(const std::string& key, const std::string& text) {
    if (!said.insert(key).second) return;
    if (host.log) host.log(text);
  }

  std::uint32_t Alloc(std::uint32_t bytes) {
    bytes = (bytes + 7u) & ~7u;
    if (bump == 0 || bump + bytes > bump_end) {
      const std::uint32_t page_bytes = bytes > 0x1000u ? (bytes + 0xfffu) & ~0xfffu : 0x1000u;
      const std::uint32_t at = host.alloc ? host.alloc(page_bytes) : 0;
      if (at == 0) {
        Say("alloc", "sles: guest memory unavailable");
        return 0;
      }
      bump = at;
      bump_end = at + page_bytes;
    }
    const std::uint32_t at = bump;
    bump += bytes;
    return at;
  }

  bool WriteU32(std::uint32_t at, std::uint32_t value) {
    return at != 0 && host.mem != nullptr && host.mem->Write(at, &value, 4);
  }

  std::uint32_t Bind(const std::string& name, Method fn) {
    const std::uint32_t slot = host.bind ? host.bind(name, fn) : 0;
    if (slot == 0) Say("bind:" + name, "sles: could not bind " + name);
    return slot;
  }

  // ---- symbols

  void RegisterInterfaceIds() {
    struct IdName {
      const char* name;
      Iid id;
    };
    const IdName ids[] = {
        {"SL_IID_ENGINE", Iid::Engine},
        {"SL_IID_PLAY", Iid::Play},
        {"SL_IID_BUFFERQUEUE", Iid::BufferQueue},
        {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE", Iid::SimpleBufferQueue},
    };
    const std::uint32_t cells = Alloc(static_cast<std::uint32_t>(sizeof ids / sizeof ids[0]) * 4u);
    const std::uint32_t objects = Alloc(static_cast<std::uint32_t>(sizeof ids / sizeof ids[0]) * 16u);
    if (cells == 0 || objects == 0) return;
    for (std::size_t i = 0; i < sizeof ids / sizeof ids[0]; ++i) {
      const std::uint32_t cell = cells + static_cast<std::uint32_t>(i * 4);
      const std::uint32_t guid = objects + static_cast<std::uint32_t>(i * 16);
      std::array<unsigned char, 16> bytes{};
      for (std::size_t b = 0; b < bytes.size(); ++b) {
        bytes[b] = static_cast<unsigned char>(0x40u + static_cast<unsigned>(i) * 0x11u + b);
      }
      // A recognizable version-4 style GUID. Only identity matters here; no host compares it.
      bytes[6] = 0x40;
      bytes[8] = 0x80;
      if (host.mem != nullptr) host.mem->Write(guid, bytes.data(), bytes.size());
      WriteU32(cell, guid);
      if (host.data_symbol) host.data_symbol(ids[i].name, cell);
      iid_at[guid] = ids[i].id;
      iid_objects[static_cast<unsigned>(ids[i].id)] = guid;
    }
  }

  void BindEntryPoints() {
    Bind("slCreateEngine", [this](exec::MachineState& s, std::string*) {
      const std::uint32_t engine = NewObject(ObjKind::Engine);
      if (engine == 0 || !WriteU32(s.r[0], engine)) {
        s.r[0] = kParameterInvalid;
        return true;
      }
      Say("engine", "sles: slCreateEngine -> 0 (silent engine created)");
      s.r[0] = kSuccess;
      return true;
    });
    Bind("slQueryNumSupportedEngineInterfaces", [this](exec::MachineState& s, std::string*) {
      if (!WriteU32(s.r[1], 0)) {
        s.r[0] = kParameterInvalid;
        return true;
      }
      s.r[0] = kSuccess;
      return true;
    });
    Bind("slQuerySupportedEngineInterfaces", [](exec::MachineState& s, std::string*) {
      s.r[0] = kSuccess;
      return true;
    });
  }

  // ---- guest cells and vtables

  std::uint32_t Vtable(Facet facet) {
    const unsigned index = static_cast<unsigned>(facet);
    if (vtables[index] != 0) return vtables[index];
    const FacetDesc& d = Desc(facet);
    const std::uint32_t at = Alloc(static_cast<std::uint32_t>(d.count * 4u));
    if (at == 0) return 0;
    for (unsigned i = 0; i < d.count; ++i) {
      const std::string name = std::string{"sles:silent:"} + d.name + "::" + d.methods[i];
      const std::uint32_t slot =
          Bind(name, [this, facet, i](exec::MachineState& s, std::string* why) { return Call(facet, i, s, why); });
      if (slot == 0 || !WriteU32(at + i * 4u, slot)) return 0;
    }
    vtables[index] = at;
    return at;
  }

  std::uint32_t NewInterface(Obj* obj, Facet facet) {
    if (const auto it = obj->cells.find(facet); it != obj->cells.end()) return it->second;
    const std::uint32_t vt = Vtable(facet);
    const std::uint32_t cell = vt == 0 ? 0 : Alloc(4);
    if (cell == 0 || !WriteU32(cell, vt)) return 0;
    auto itf = std::make_unique<Itf>();
    itf->obj = obj;
    itf->facet = facet;
    itfs[cell] = std::move(itf);
    obj->cells[facet] = cell;
    return cell;
  }

  std::uint32_t NewObject(ObjKind kind) {
    auto obj = std::make_unique<Obj>();
    obj->kind = kind;
    Obj* raw = obj.get();
    const std::uint32_t cell = NewInterface(raw, Facet::Object);
    if (cell == 0) return 0;
    raw->object_cell = cell;
    objects[cell] = std::move(obj);
    return cell;
  }

  Itf* Find(std::uint32_t cell) {
    const auto it = itfs.find(cell);
    return it == itfs.end() ? nullptr : it->second.get();
  }

  bool WriteOut(std::uint32_t at, std::uint32_t value) { return WriteU32(at, value); }

  bool GetInterface(Itf& self, exec::MachineState& s) {
    const auto id = iid_at.find(s.r[1]);
    if (id == iid_at.end()) {
      Say("iid:" + HexString(s.r[1]), "sles: GetInterface on unknown IID " + HexString(s.r[1]) +
                                         ": FEATURE_UNSUPPORTED");
      s.r[0] = kFeatureUnsupported;
      return true;
    }
    Facet facet = Facet::Count;
    switch (id->second) {
      case Iid::Engine:
        if (self.obj->kind == ObjKind::Engine) facet = Facet::Engine;
        break;
      case Iid::Play:
        if (self.obj->kind == ObjKind::Player) facet = Facet::Play;
        break;
      case Iid::BufferQueue:
        if (self.obj->kind == ObjKind::Player) facet = Facet::BufferQueue;
        break;
      case Iid::SimpleBufferQueue:
        if (self.obj->kind == ObjKind::Player) facet = Facet::SimpleBufferQueue;
        break;
      case Iid::Count:
        break;
    }
    if (facet == Facet::Count) {
      Say("iface:" + HexString(s.r[1]), "sles: this object does not offer IID " + HexString(s.r[1]) +
                                            ": FEATURE_UNSUPPORTED");
      s.r[0] = kFeatureUnsupported;
      return true;
    }
    const std::uint32_t cell = NewInterface(self.obj, facet);
    if (cell == 0 || !WriteOut(s.r[2], cell)) {
      s.r[0] = kParameterInvalid;
      return true;
    }
    if (facet == Facet::Engine) Say("engine-iface", "sles: engine interface handed to the guest");
    s.r[0] = kSuccess;
    return true;
  }

  bool Destroy(Itf& self) {
    Obj* obj = self.obj;
    std::vector<std::uint32_t> cells;
    for (const auto& [facet, cell] : obj->cells) cells.push_back(cell);
    for (const std::uint32_t cell : cells) itfs.erase(cell);
    objects.erase(obj->object_cell);
    return true;
  }

  bool Call(Facet facet, unsigned index, exec::MachineState& s, std::string*) {
    Itf* itf = Find(s.r[0]);
    if (itf == nullptr || itf->facet != facet) {
      Say("dead:" + HexString(s.r[0]), "sles: method on a dead interface " + HexString(s.r[0]));
      s.r[0] = kPreconditionsViolated;
      return true;
    }
    s.r[0] = kSuccess;
    switch (facet) {
      case Facet::Object:
        switch (index) {
          case 0:  // Realize
            itf->obj->realized = true;
            if (itf->obj->kind == ObjKind::OutputMix) Say("mix-realize", "sles: output mix realized");
            if (itf->obj->kind == ObjKind::Player) Say("player-realize", "sles: audio player realized");
            return true;
          case 1:  // Resume
          case 5:  // AbortAsyncOperation
          case 7:  // SetPriority
          case 9:  // SetLossOfControlInterfaces
            return true;
          case 2:  // GetState
            if (!WriteOut(s.r[1], kObjectStateRealized)) s.r[0] = kParameterInvalid;
            return true;
          case 3:  // GetInterface
            return GetInterface(*itf, s);
          case 4:  // RegisterCallback
            return true;
          case 6:  // Destroy
            return Destroy(*itf);
          case 8:  // GetPriority
            if (!WriteOut(s.r[1], 0)) s.r[0] = kParameterInvalid;
            return true;
          default:
            break;
        }
        break;
      case Facet::Engine:
        switch (index) {
          case 2: {  // CreateAudioPlayer
            const std::uint32_t player = NewObject(ObjKind::Player);
            if (player == 0 || !WriteOut(s.r[1], player)) {
              s.r[0] = kParameterInvalid;
              return true;
            }
            Say("player", "sles: CreateAudioPlayer -> object (silent; buffer callbacks are not delivered)");
            return true;
          }
          case 7: {  // CreateOutputMix
            const std::uint32_t mix = NewObject(ObjKind::OutputMix);
            if (mix == 0 || !WriteOut(s.r[1], mix)) {
              s.r[0] = kParameterInvalid;
              return true;
            }
            Say("mix", "sles: CreateOutputMix -> object");
            return true;
          }
          case 10:  // QueryNumSupportedInterfaces
          case 12: {  // QueryNumSupportedExtensions
            if (!WriteOut(s.r[1], 0)) s.r[0] = kParameterInvalid;
            return true;
          }
          case 13:  // QuerySupportedExtension
            s.r[0] = kSuccess;
            return true;
          case 14:  // IsExtensionSupported
            if (!WriteOut(s.r[2], 0)) s.r[0] = kParameterInvalid;
            return true;
          default:
            s.r[0] = kFeatureUnsupported;
            return true;
        }
      case Facet::Play:
        switch (index) {
          case 0:  // SetPlayState
            itf->obj->play_state = s.r[1];
            return true;
          case 1:  // GetPlayState
            if (!WriteOut(s.r[1], itf->obj->play_state)) s.r[0] = kParameterInvalid;
            return true;
          case 2:  // GetDuration
          case 3:  // GetPosition
          case 9:  // GetMarkerPosition
            if (!WriteOut(s.r[1], 0)) s.r[0] = kParameterInvalid;
            return true;
          case 4:  // RegisterCallback
          case 5:  // SetCallbackEventsMask
          case 7:  // SetMarkerPosition
          case 8:  // ClearMarkerPosition
          case 10:  // SetPositionUpdatePeriod
            return true;
          case 6:  // GetCallbackEventsMask
            if (!WriteOut(s.r[1], 0)) s.r[0] = kParameterInvalid;
            return true;
          case 11:  // GetPositionUpdatePeriod
            if (!WriteOut(s.r[1], 1000)) s.r[0] = kParameterInvalid;
            return true;
          default:
            break;
        }
        break;
      case Facet::BufferQueue:
      case Facet::SimpleBufferQueue:
        switch (index) {
          case 0:  // Enqueue
            ++itf->obj->queued;
            Say(itf->facet == Facet::BufferQueue ? "enqueue-bq" : "enqueue-simple",
                "sles: buffer enqueued (silent; no audio callback)");
            return true;
          case 1:  // Clear
            itf->obj->queued = 0;
            return true;
          case 2:  // GetState
            if (!WriteOut(s.r[1], itf->obj->queued)) s.r[0] = kParameterInvalid;
            return true;
          case 3:  // RegisterCallback
            return true;
          default:
            break;
        }
        break;
      case Facet::Count:
        break;
    }
    s.r[0] = kFeatureUnsupported;
    return true;
  }
};

SilentSles::SilentSles(SilentSlesHost host) : impl_(std::make_unique<Impl>(std::move(host))) {}

SilentSles::~SilentSles() = default;

}  // namespace rb::audio
