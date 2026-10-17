// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// See thunk.h. The host call goes through one "universal" function type per return class: eight integer
// parameters (x0..x7), eight double parameters (v0..v7), then eight more integers, which AAPCS64 puts on the stack
// in order — exactly the slots a callee's ninth and later integer parameters are read from. A float parameter is
// passed as a double whose low 32 bits are the float's bits: the callee reads s<n>, the low half of v<n>.
#include "rb/abi/thunk.h"

#include <cstdio>
#include <cstring>

namespace rb::abi {

namespace {

using U = std::uint64_t;
using UniversalU = U (*)(U, U, U, U, U, U, U, U, double, double, double, double, double, double, double, double, U,
                         U, U, U, U, U, U, U);
using UniversalF = float (*)(U, U, U, U, U, U, U, U, double, double, double, double, double, double, double, double,
                             U, U, U, U, U, U, U, U);
using UniversalD = double (*)(U, U, U, U, U, U, U, U, double, double, double, double, double, double, double, double,
                              U, U, U, U, U, U, U, U);

std::string Hex(std::uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%x", v);
  return b;
}

constexpr unsigned kMaxInts = 16, kMaxFps = 8;

}  // namespace

std::uint32_t HandleTable::Put(void* host) {
  if (host == nullptr) return 0;
  std::lock_guard<std::mutex> lk(mu_);
  if (const auto it = by_host_.find(host); it != by_host_.end()) return it->second;
  const std::uint32_t h = kBase + 4u * static_cast<std::uint32_t>(by_index_.size());
  by_index_.push_back(host);
  by_host_[host] = h;
  return h;
}

bool HandleTable::Get(std::uint32_t handle, void** host) const {
  if (handle == 0) {
    *host = nullptr;
    return true;
  }
  std::lock_guard<std::mutex> lk(mu_);
  if (handle < kBase || (handle & 3u) != 0) return false;
  const std::size_t i = (handle - kBase) / 4u;
  if (i >= by_index_.size() || by_index_[i] == nullptr) return false;
  *host = by_index_[i];
  return true;
}

void HandleTable::Forget(void* host) {
  std::lock_guard<std::mutex> lk(mu_);
  const auto it = by_host_.find(host);
  if (it == by_host_.end()) return;
  by_index_[(it->second - kBase) / 4u] = nullptr;
  by_host_.erase(it);
}

std::size_t HandleTable::size() const {
  std::lock_guard<std::mutex> lk(mu_);
  return by_host_.size();
}

bool ThunkCaller::Call(const char* spec, void* fn, exec::MachineState& s, const mem::GuestMemory& mem,
                       HandleTable& handles, std::string* err) {
  const auto refuse = [&](const std::string& why) {
    if (err != nullptr) *err = std::string{"thunk '"} + spec + "': " + why;
    return false;
  };
  const char* colon = std::strchr(spec, ':');
  if (colon == nullptr || colon != spec + 1) return refuse("not a \"<ret>:<args>\" signature");
  const char ret = spec[0];
  if (std::strchr("viufdlH", ret) == nullptr) return refuse(std::string{"return letter '"} + ret + "' is not one of viufdlH");

  // Guest word n: r0..r3, then the stack at sp.
  bool readable = true;
  const auto word = [&](unsigned n) -> std::uint32_t {
    if (n < 4) return s.r[n];
    std::uint32_t v = 0;
    if (!mem.Read(s.r[13] + 4u * (n - 4u), &v, 4)) readable = false;
    return v;
  };
  U ints[kMaxInts] = {};
  double fps[kMaxFps] = {};
  unsigned ni = 0, nf = 0, n = 0;
  for (const char* a = colon + 1; *a != '\0'; ++a) {
    const char c = *a;
    if (c == 'd' || c == 'l') {
      n += n & 1u;  // an even register pair or an 8-aligned stack slot; never split across r3 and the stack
      const U lo = word(n), hi = word(n + 1);
      n += 2;
      const U v = lo | (hi << 32);
      if (c == 'l') {
        if (ni >= kMaxInts) return refuse("more than 16 integer arguments");
        ints[ni++] = v;
      } else {
        if (nf >= kMaxFps) return refuse("more than 8 floating-point arguments");
        std::memcpy(&fps[nf++], &v, 8);
      }
      continue;
    }
    const std::uint32_t w = word(n++);
    switch (c) {
      case 'f': {
        if (nf >= kMaxFps) return refuse("more than 8 floating-point arguments");
        const U bits = w;  // the float's bits in the low half: the callee reads s<n>
        std::memcpy(&fps[nf++], &bits, 8);
        continue;
      }
      case 'i': ints[ni] = static_cast<U>(static_cast<std::int64_t>(static_cast<std::int32_t>(w))); break;
      case 'u': ints[ni] = w; break;
      case 'p': {
        void* host = w == 0 ? nullptr : mem.Host(w);
        if (w != 0 && host == nullptr) return refuse("pointer argument " + Hex(w) + " is not guest memory");
        ints[ni] = reinterpret_cast<U>(host);
        break;
      }
      case 'o': {
        void* host = w == 0 ? nullptr : mem.Host(w);
        ints[ni] = host != nullptr ? reinterpret_cast<U>(host) : U{w};
        break;
      }
      case 'h': {
        void* host = nullptr;
        if (!handles.Get(w, &host)) return refuse("handle argument " + Hex(w) + " was never issued (or is closed)");
        ints[ni] = reinterpret_cast<U>(host);
        break;
      }
      default: return refuse(std::string{"argument letter '"} + c + "' is not one of iufdlpoh");
    }
    if (++ni > kMaxInts) return refuse("more than 16 integer arguments");
  }
  if (!readable) return refuse("a stacked argument word is not readable guest memory");

#define RB_ARGS                                                                                              \
  ints[0], ints[1], ints[2], ints[3], ints[4], ints[5], ints[6], ints[7], fps[0], fps[1], fps[2], fps[3], fps[4], \
      fps[5], fps[6], fps[7], ints[8], ints[9], ints[10], ints[11], ints[12], ints[13], ints[14], ints[15]
  switch (ret) {
    case 'f': {
      const float r = reinterpret_cast<UniversalF>(fn)(RB_ARGS);
      std::memcpy(&s.r[0], &r, 4);
      break;
    }
    case 'd': {
      const double r = reinterpret_cast<UniversalD>(fn)(RB_ARGS);
      U bits;
      std::memcpy(&bits, &r, 8);
      s.r[0] = static_cast<std::uint32_t>(bits);
      s.r[1] = static_cast<std::uint32_t>(bits >> 32);
      break;
    }
    default: {
      const U r = reinterpret_cast<UniversalU>(fn)(RB_ARGS);
      if (ret == 'l') {
        s.r[0] = static_cast<std::uint32_t>(r);
        s.r[1] = static_cast<std::uint32_t>(r >> 32);
      } else if (ret == 'H') {
        s.r[0] = handles.Put(reinterpret_cast<void*>(r));
      } else {
        s.r[0] = ret == 'v' ? 0u : static_cast<std::uint32_t>(r);
      }
    }
  }
#undef RB_ARGS
  return true;
}

}  // namespace rb::abi
