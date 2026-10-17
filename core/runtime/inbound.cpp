// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
#include "rb/runtime/inbound.h"

#include <cstdio>
#include <cstring>

#include "rb/abi/jni_args.h"

extern "C" char rb_inbound_pool[];  // inbound_aarch64.S: kPoolSize entries of 8 bytes

namespace rb::runtime {
namespace {

std::atomic<Inbound*> g_current{nullptr};

const char* ReasonText(exec::StopReason r) {
  switch (r) {
    case exec::StopReason::Budget: return "budget";
    case exec::StopReason::Fault: return "fault";
    case exec::StopReason::HostCall: return "host call";
    case exec::StopReason::Syscall: return "syscall";
    case exec::StopReason::Returned: return "returned";
    case exec::StopReason::Internal: return "internal";
    default: return "exited";
  }
}

bool ValidShorty(const std::string& s) {
  if (s.empty() || std::strchr("VZBCSIJFDL", s[0]) == nullptr) return false;
  for (std::size_t i = 1; i < s.size(); ++i) {
    if (s[i] == 'V' || std::strchr("ZBCSIJFDL", s[i]) == nullptr) return false;
  }
  return true;
}

}  // namespace

std::uint64_t InboundEntry(std::uint32_t index, CapturedArgs* a) {
  Inbound* in = Inbound::current();
  if (in == nullptr || index >= in->bound_.load(std::memory_order_acquire)) {
    std::fprintf(stderr, "RetroBug: inbound trampoline %u reached with no binding (no live Inbound, or unbound)\n", index);
    return 0;
  }
  return in->Dispatch(in->pool_[index], *a);
}

Inbound::Inbound(GuestProcess& p, abi::ForwardingJni& fwd, abi::HostJava& java, int tier)
    : p_{p}, fwd_{fwd}, java_{java}, tier_{tier}, pool_{new InboundTarget[kPoolSize]} {
  g_current.store(this, std::memory_order_release);
}

Inbound::~Inbound() {
  Inbound* self = this;
  g_current.compare_exchange_strong(self, nullptr, std::memory_order_acq_rel);
}

Inbound* Inbound::current() { return g_current.load(std::memory_order_acquire); }

void* Inbound::BindPool(const InboundTarget& t, std::string* err) {
  if (!ValidShorty(t.shorty)) {
    if (err != nullptr) *err = "'" + t.shorty + "' is not a JNI shorty (" + t.name + ")";
    return nullptr;
  }
  std::lock_guard<std::mutex> lk(mu_);
  const std::size_t i = bound_.load(std::memory_order_relaxed);
  if (i >= kPoolSize) {
    if (err != nullptr) *err = "the inbound trampoline pool is exhausted (" + std::to_string(kPoolSize) + ")";
    return nullptr;
  }
  pool_[i] = t;
  bound_.store(i + 1, std::memory_order_release);
  return rb_inbound_pool + 8 * i;
}

void Inbound::Fail(const InboundTarget& t, const std::string& why) {
  const std::string message = "RetroBug: guest native " + t.name + " stopped: " + why;
  std::fprintf(stderr, "%s\n", message.c_str());
  const abi::HostRef cls = java_.FindClass("java/lang/Error");
  if (cls == 0) return;
  java_.ThrowNew(cls, message);
  java_.DeleteLocalRef(cls);
}

std::uint64_t Inbound::Dispatch(const InboundTarget& t, const CapturedArgs& a) {
  using abi::Value32;
  const char ret = t.shorty.empty() ? 'V' : t.shorty[0];
  fwd_.SyncShadows();  // Java may have written a shadowed direct buffer since the guest last ran (M4)
  fwd_.PushFrame();
  std::string e;
  std::vector<Value32> args;
  args.push_back(Value32::Ptr(fwd_.env()));
  args.push_back(Value32::Ptr(fwd_.ToGuestLocal(static_cast<abi::HostRef>(a.x[1]), &e)));
  // AAPCS64: integers and references in x2.. (after env and this/class), FP values in d0..; past the eighth of
  // either kind they go to the stack, one 8-byte slot each, in argument order — so both kinds draw from one cursor.
  unsigned gr = 2, fr = 0;
  const std::uint64_t* stack = a.stack;
  const auto next_int = [&]() -> std::uint64_t { return gr < 8 ? a.x[gr++] : *stack++; };
  const auto next_fp = [&]() -> std::uint64_t { return fr < 8 ? a.d[fr++] : *stack++; };
  for (std::size_t k = 1; k < t.shorty.size() && e.empty(); ++k) {
    switch (t.shorty[k]) {
      case 'Z': args.push_back(Value32::U32(static_cast<std::uint32_t>(next_int() & 0xffu))); break;
      case 'B': args.push_back(Value32::S32(static_cast<std::int8_t>(next_int()))); break;
      case 'C': args.push_back(Value32::U32(static_cast<std::uint32_t>(next_int() & 0xffffu))); break;
      case 'S': args.push_back(Value32::S32(static_cast<std::int16_t>(next_int()))); break;
      case 'I': args.push_back(Value32::S32(static_cast<std::int32_t>(next_int()))); break;
      case 'J': args.push_back(Value32::I64(static_cast<std::int64_t>(next_int()))); break;
      case 'F': {
        const auto bits = static_cast<std::uint32_t>(next_fp());
        float f = 0;
        std::memcpy(&f, &bits, 4);
        args.push_back(Value32::F32(f));
        break;
      }
      case 'D': {
        const std::uint64_t bits = next_fp();
        double d = 0;
        std::memcpy(&d, &bits, 8);
        args.push_back(Value32::F64(d));
        break;
      }
      case 'L': args.push_back(Value32::Ptr(fwd_.ToGuestLocal(static_cast<abi::HostRef>(next_int()), &e))); break;
      default: e = std::string{"unknown shorty kind '"} + t.shorty[k] + "'"; break;
    }
  }
  if (!e.empty()) {
    fwd_.PopFrame(0);
    Fail(t, e);
    return 0;
  }
  fwd_.ArgumentsIn();
  exec::RunResult rr{};
  exec::MachineState last{};
  const char shorty[2] = {ret, '\0'};
  const abi::Ret32 r = p_.abi().CallGuestToCompletion(t.fn, args, abi::Shorty::For(shorty),
                                                      GuestProcess::kThreadTurnBudget, &rr, &last);
  if (rr.reason != exec::StopReason::Returned) {
    fwd_.PopFrame(0);
    if (rr.reason == exec::StopReason::Exited && p_.monitor().group_exited() && on_exit_) {
      on_exit_(static_cast<int>(p_.abi().exit_status()));
    }
    // Where the guest was: without it a fault inside a game's library names no instruction to look at.
    char where[48];
    std::snprintf(where, sizeof where, " (pc 0x%x, lr 0x%x)", static_cast<unsigned>(last.pc),
                  static_cast<unsigned>(last.r[14]));
    Fail(t, std::string{ReasonText(rr.reason)} + (rr.detail.empty() ? "" : ": " + rr.detail) +
                (rr.reason == exec::StopReason::Exited ? "" : where));
    return 0;
  }
  fwd_.SyncShadows();  // the guest's writes to a shadowed direct buffer reach Java before it reads them
  std::string pe;
  const abi::HostRef result = fwd_.PopFrame(ret == 'L' ? r.w() : 0, &pe);
  if (!pe.empty()) {
    Fail(t, "its return value: " + pe);
    return 0;
  }
  switch (ret) {
    case 'V': return 0;
    case 'L': return static_cast<std::uint64_t>(result);
    case 'Z': return r.w() & 0xffu;
    case 'B': return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int8_t>(r.w())));
    case 'C': return r.w() & 0xffffu;
    case 'S': return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int16_t>(r.w())));
    case 'I':
    case 'F': return r.w();  // a jfloat's bits: the capture copies x0 to d0, so s0 holds them
    default: return r.bits;  // J, D
  }
}

std::string ShortyOf(const std::string& sig) {
  std::vector<char> params;
  char ret = 0;
  if (!abi::jni::ParseSignature(sig, &params, &ret)) return {};
  std::string s(1, ret);
  s.append(params.begin(), params.end());
  return s;
}

}  // namespace rb::runtime

extern "C" std::uint64_t rb_inbound_entry(std::uint32_t index, rb::runtime::CapturedArgs* a) {
  return rb::runtime::InboundEntry(index, a);
}
