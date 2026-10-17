// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
#include "rb/abi/jni_args.h"

#include <cstdio>
#include <cstring>

namespace rb::abi::jni {
using exec::GuestAddr;
namespace {

std::string Hex(std::uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%x", v);
  return buf;
}

}  // namespace

bool ParseSignature(const std::string& sig, std::vector<char>* params, char* ret) {
  // `(params)ret`, with every reference — `Lpkg/Name;` or any `[...` array — reduced to L, because the calling
  // convention only cares that it is one word.
  std::size_t i = 0;
  if (sig.empty() || sig[i++] != '(') return false;
  const auto one = [&](char& kind) -> bool {
    if (i >= sig.size()) return false;
    char c = sig[i];
    if (c == '[') {
      while (i < sig.size() && sig[i] == '[') ++i;
      if (i >= sig.size()) return false;
      c = sig[i];
      if (c == 'L') {
        const std::size_t semi = sig.find(';', i);
        if (semi == std::string::npos) return false;
        i = semi + 1;
      } else {
        if (std::strchr("ZBCSIJFD", c) == nullptr || c == '\0') return false;
        ++i;
      }
      kind = 'L';
      return true;
    }
    if (c == 'L') {
      const std::size_t semi = sig.find(';', i);
      if (semi == std::string::npos) return false;
      i = semi + 1;
      kind = 'L';
      return true;
    }
    if (std::strchr("ZBCSIJFD", c) == nullptr || c == '\0') return false;
    ++i;
    kind = c;
    return true;
  };
  std::vector<char> ps;
  while (i < sig.size() && sig[i] != ')') {
    char k = 0;
    if (!one(k)) return false;
    ps.push_back(k);
  }
  if (i >= sig.size()) return false;
  ++i;
  char r = 0;
  if (i < sig.size() && sig[i] == 'V') {
    r = 'V';
    ++i;
  } else if (!one(r)) {
    return false;
  }
  if (i != sig.size()) return false;
  if (params != nullptr) *params = std::move(ps);
  if (ret != nullptr) *ret = r;
  return true;
}

bool ArgWord(const exec::MachineState& s, const mem::GuestMemory& m, unsigned index, std::uint32_t* out) {
  if (index < 4) {
    *out = s.r[index];
    return true;
  }
  return m.Read(s.r[13] + 4u * (index - 4u), out, 4);
}

bool DecodeArgs(const exec::MachineState& s, const mem::GuestMemory& m, ArgForm form, const std::vector<char>& params,
                unsigned fixed, std::vector<std::uint64_t>* out, std::string* err) {
  const auto fail = [&](const std::string& why) {
    if (err != nullptr) *err = why;
    return false;
  };
  out->clear();
  // Register-and-stack forms walk a cursor over the AAPCS32 argument area: core registers r[next..3] first, then
  // the stack. Pointer forms walk guest memory from the pointer.
  const bool in_registers = form == ArgForm::kVariadic || form == ArgForm::kFixed;
  unsigned next_reg = fixed;                                       // the next free core register (in_registers)
  GuestAddr cursor = s.r[13] + 4u * (fixed > 4 ? fixed - 4 : 0u);  // the next stack word (in_registers)
  if (!in_registers) {
    std::uint32_t p = 0;
    if (!ArgWord(s, m, fixed, &p)) return fail("the argument pointer (word " + std::to_string(fixed) + ") is unreadable");
    cursor = p;
  }
  for (const char k : params) {
    std::uint64_t v = 0;
    if (form == ArgForm::kJvalues) {
      const std::size_t n = (k == 'Z' || k == 'B') ? 1 : (k == 'C' || k == 'S') ? 2 : (k == 'J' || k == 'D') ? 8 : 4;
      if (!m.Read(cursor, &v, n)) return fail("jvalue array at 0x" + Hex(cursor) + " is unreadable");
      cursor += 8;
    } else {
      const bool promoted_float = k == 'F' && form != ArgForm::kFixed;  // `...` passes F as a double
      const bool wide = k == 'J' || k == 'D' || promoted_float;
      if (wide) {
        if (in_registers && next_reg % 2 != 0) ++next_reg;  // a doubleword starts on an even register
        if (in_registers && next_reg < 4) {
          v = static_cast<std::uint64_t>(s.r[next_reg]) | (static_cast<std::uint64_t>(s.r[next_reg + 1]) << 32);
          next_reg += 2;
        } else {
          if (in_registers) next_reg = 4;  // once one argument went to the stack, no later one is in a register
          cursor = (cursor + 7u) & ~7u;
          if (!m.Read(cursor, &v, 8)) return fail("argument at 0x" + Hex(cursor) + " is unreadable");
          cursor += 8;
        }
        if (promoted_float) {
          double d = 0;
          std::memcpy(&d, &v, 8);
          const float f = static_cast<float>(d);
          std::uint32_t bits = 0;
          std::memcpy(&bits, &f, 4);
          v = bits;
        }
      } else if (in_registers && next_reg < 4) {
        v = s.r[next_reg++];
      } else {
        std::uint32_t w = 0;
        if (!m.Read(cursor, &w, 4)) return fail("argument at 0x" + Hex(cursor) + " is unreadable");
        v = w;
        cursor += 4;
      }
    }
    // Narrow kinds keep only their own bits, so a caller's stale upper bytes never reach the host.
    if (k == 'Z') v &= 0xffu;
    if (k == 'B') v = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int8_t>(v)));
    if (k == 'C') v &= 0xffffu;
    if (k == 'S') v = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int16_t>(v)));
    if (k == 'I' || k == 'F' || k == 'L') v &= 0xffff'ffffu;
    out->push_back(v);
  }
  return true;
}

}  // namespace rb::abi::jni
