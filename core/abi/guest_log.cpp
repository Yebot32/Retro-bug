// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// See rb/abi/guest_log.h.

#include "rb/abi/guest_log.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace rb::abi {

std::uint32_t GuestVarargs::Next32() {
  if (reg_free) {
    reg_free = false;
    return reg;
  }
  std::uint32_t v = 0;
  if (!mem->Read(at, &v, 4)) ok = false;
  at += 4;
  return v;
}

std::uint64_t GuestVarargs::Next64() {
  reg_free = false;
  at = (at + 7u) & ~7u;
  std::uint64_t v = 0;
  if (!mem->Read(at, &v, 8)) ok = false;
  at += 8;
  return v;
}

std::string GuestStringOrEmpty(const mem::GuestMemory& mem, std::uint32_t at) {
  std::string out;
  if (at == 0 || at >= mem.window_bytes()) return out;
  for (unsigned i = 0; i < 256; ++i) {
    char c = 0;
    if (!mem.Read(at + i, &c, 1) || c == '\0') break;
    out.push_back(c);
  }
  return out;
}

std::string GuestFormat(const mem::GuestMemory& mem, std::uint32_t fmt_at, GuestVarargs& va) {
  const std::string fmt = GuestStringOrEmpty(mem, fmt_at);
  std::string out;
  for (std::size_t i = 0; i < fmt.size(); ++i) {
    if (fmt[i] != '%') {
      out.push_back(fmt[i]);
      continue;
    }
    std::string spec = "%";
    std::size_t j = i + 1;
    while (j < fmt.size() && std::strchr("-+ #0", fmt[j]) != nullptr) spec.push_back(fmt[j++]);
    if (j < fmt.size() && fmt[j] == '*') {
      spec += std::to_string(static_cast<std::int32_t>(va.Next32()));
      ++j;
    } else {
      while (j < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[j]))) spec.push_back(fmt[j++]);
    }
    if (j < fmt.size() && fmt[j] == '.') {
      spec.push_back(fmt[j++]);
      if (j < fmt.size() && fmt[j] == '*') {
        spec += std::to_string(static_cast<std::int32_t>(va.Next32()));
        ++j;
      } else {
        while (j < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[j]))) spec.push_back(fmt[j++]);
      }
    }
    bool wide = false;
    while (j < fmt.size() && std::strchr("hlLqjzt", fmt[j]) != nullptr) {
      if (fmt[j] == 'l' && j + 1 < fmt.size() && fmt[j + 1] == 'l') {
        wide = true;
        ++j;
      } else if (fmt[j] == 'q' || fmt[j] == 'j' || fmt[j] == 'L') {
        wide = true;
      }
      ++j;  // h, hh, l, z, t are 32-bit on this ABI and promote to int
    }
    if (j >= fmt.size()) {
      out += fmt.substr(i);
      break;
    }
    const char c = fmt[j];
    char buf[512];
    buf[0] = 0;
    if (c == '%') {
      out.push_back('%');
    } else if (std::strchr("diuoxX", c) != nullptr) {
      const std::uint64_t raw = wide ? va.Next64() : va.Next32();
      const std::string s = spec + "ll" + c;
      if (c == 'd' || c == 'i') {
        const long long v = wide ? static_cast<long long>(raw)
                                 : static_cast<long long>(static_cast<std::int32_t>(raw));
        std::snprintf(buf, sizeof buf, s.c_str(), v);
      } else {
        std::snprintf(buf, sizeof buf, s.c_str(), static_cast<unsigned long long>(raw));
      }
      out += buf;
    } else if (c == 'c') {
      std::snprintf(buf, sizeof buf, (spec + "c").c_str(), static_cast<int>(va.Next32() & 0xffu));
      out += buf;
    } else if (c == 'p') {
      std::snprintf(buf, sizeof buf, "0x%x", va.Next32());
      out += buf;
    } else if (std::strchr("fFeEgGaA", c) != nullptr) {
      const std::uint64_t bits = va.Next64();  // `...` promoted float to double
      double d = 0;
      std::memcpy(&d, &bits, 8);
      std::snprintf(buf, sizeof buf, (spec + c).c_str(), d);
      out += buf;
    } else if (c == 's') {
      const std::uint32_t p = va.Next32();
      const std::string s = p == 0 ? std::string{"(null)"} : GuestStringOrEmpty(mem, p);
      std::snprintf(buf, sizeof buf, (spec + "s").c_str(), s.c_str());
      out += buf;
    } else if (c == 'n') {
      va.Next32();
    } else {
      out += fmt.substr(i, j - i + 1);
    }
    i = j;
  }
  return out;
}

std::string PrioName(int prio) {
  static const char* const kPrio[] = {"?", "?", "V", "D", "I", "W", "E", "F", "S"};
  return (prio >= 0 && prio <= 8) ? kPrio[prio] : "?";
}

}  // namespace rb::abi
