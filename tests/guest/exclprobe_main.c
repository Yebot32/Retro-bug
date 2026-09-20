/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * Prints what real ARMv7 hardware says for each exclusive-access sequence in `exclprobe.s`, so the two
 * tiers can be held to a measurement rather than to the same reading of the manual twice.
 *
 * Each row reports `Rd` (0 = the store passed, 1 = it failed) and the halfword/word afterwards, because
 * `Rd` alone cannot tell "the store failed" from "the store passed and wrote the same value that was
 * already there" -- which is precisely the confusion a value-comparing memory model would introduce.
 */
#include <stdint.h>
#include <stdio.h>

uint16_t _hw;
uint32_t _ww;
uint32_t _dd[2];

extern unsigned f1_plain_pair(void*, unsigned);
extern unsigned f2_plain_store_between(void*, unsigned);
extern unsigned f3_clrex_between(void*, unsigned);
extern unsigned f4_no_prior_load(void*, unsigned);
extern unsigned f5_other_address(void*, unsigned);
extern unsigned f6_acquire_pair(void*, unsigned);
extern unsigned f7_acquire_plain_store_between(void*, unsigned);
extern unsigned f8_byte_pair(void*, unsigned);
extern unsigned f9_double_pair(void*, unsigned);
extern unsigned f10_word_pair(void*, unsigned);
extern unsigned f11_acquire_word(void*, unsigned);
extern unsigned f12_ordinary_write_then_strexh(void*, unsigned);

static void half(const char* what, unsigned (*fn)(void*, unsigned), uint16_t seed) {
  _hw = seed;
  unsigned rd = fn(&_hw, 0x1234u);
  printf("%-34s rd=%u after=%04x\n", what, rd, _hw);
}

static void word(const char* what, unsigned (*fn)(void*, unsigned), uint32_t seed) {
  _ww = seed;
  unsigned rd = fn(&_ww, 0x89abcdefu);
  printf("%-34s rd=%u after=%08x\n", what, rd, _ww);
}

int main(void) {
  half("f1_plain_pair", f1_plain_pair, 0x0001);
  half("f2_plain_store_between", f2_plain_store_between, 0x0002);
  half("f3_clrex_between", f3_clrex_between, 0x0003);
  half("f4_no_prior_load", f4_no_prior_load, 0x0004);
  half("f5_other_address", f5_other_address, 0x0005);
  half("f6_acquire_pair", f6_acquire_pair, 0x0006);
  half("f7_acquire_plain_store_between", f7_acquire_plain_store_between, 0x0007);
  half("f12_ordinary_write_then_strexh", f12_ordinary_write_then_strexh, 0x0008);

  _hw = 0x0009;
  printf("f8_byte_pair                         rd=%u after=%04x\n", f8_byte_pair(&_hw, 0x1234u), _hw);

  word("f10_word_pair", f10_word_pair, 0x0000000au);
  word("f11_acquire_word", f11_acquire_word, 0x0000000bu);

  _dd[0] = 0x11111111u;
  _dd[1] = 0x22222222u;
  printf("f9_double_pair                       rd=%u after=%08x %08x\n", f9_double_pair(_dd, 0x33333333u),
         _dd[0], _dd[1]);
  return 0;
}
