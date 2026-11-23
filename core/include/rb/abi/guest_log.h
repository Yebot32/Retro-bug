// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Reading a guest's printf-style log call: the variadic words per AAPCS32 (softfp), and the format
// rendered on the host. Shared by Android's HostSlots and the iOS host slots — the guest's libc/liblog
// imports are the same on both, so the log lines a title writes read the same in either report.
#pragma once

#include <cstdint>
#include <string>

#include "rb/mem/guest_memory.h"

namespace rb::abi {

/// Where a guest printf's variadic arguments live: optionally one core register first (r3 for
/// `__android_log_print(prio, tag, fmt, ...)`), then guest memory — the stack, or a `va_list`'s area.
/// AAPCS puts a 64-bit value 8-aligned and never splits it into r3, which is the one rule that matters.
struct GuestVarargs {
  const mem::GuestMemory* mem = nullptr;
  bool reg_free = false;
  std::uint32_t reg = 0;
  std::uint32_t at = 0;
  bool ok = true;
  std::uint32_t Next32();
  std::uint64_t Next64();
};

/// A guest NUL-terminated string, bounded (256 bytes) and refusal-free.
std::string GuestStringOrEmpty(const mem::GuestMemory& mem, std::uint32_t at);

/// A guest `printf` format rendered on the host: flags, width and precision (including `*`), the h/hh/l/
/// ll/z/j/t/L lengths, and d i u o x X c s p f F e E g G a A %. `%s` reads a bounded guest string; `%n`
/// writes nothing. A conversion it does not know is copied through as text, so the line still says what
/// the guest wrote.
std::string GuestFormat(const mem::GuestMemory& mem, std::uint32_t fmt_at, GuestVarargs& va);

/// liblog's priority as the letter a logcat line starts with (V/D/I/W/E/F/S); "?" for anything else.
std::string PrioName(int prio);

}  // namespace rb::abi
