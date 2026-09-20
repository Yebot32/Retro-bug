/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * Ground truth for the guest `libc.so` lane (`core/tests/guest_libc_test.cpp`): the answers the device's
 * *own* 32-bit bionic gives for a fixed set of calls, printed as values rather than as decimals so a
 * last-bit difference is visible.
 *
 * Why this file exists at all, rather than the lane asserting from first principles: the point of the lane
 * is "does RetroBug execute real bionic code correctly", and the only authority on what bionic computes is
 * bionic. `strncmp` and `qsort` are not hard to predict, but `strtod` is 3,580 bytes of the trickiest code
 * in the library — correctly-rounded conversion with a table of powers — and an expectation written from
 * what the C standard *seems* to require would eventually be argued with. Here the answer is what silicon
 * returned for these exact bytes.
 *
 * Note what the *input* to the comparison is, too. The lane loads `/system/lib/libc.so` — the device's
 * real file — and this program links against the same file through the ordinary `DT_NEEDED` path. So the
 * two sides are the same machine code, and a difference between them is RetroBug's, not a library
 * revision or a build-config difference. That is the whole reason the lane's binary is a device artifact
 * rather than something built here.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cmp_int(const void* a, const void* b) {
  const int x = *(const int*)a;
  const int y = *(const int*)b;
  return (x > y) - (x < y);
}

/* Offsets rather than pointers: an address means nothing across two processes, and `p - s` means exactly
 * the same thing on both sides. Every string-returning libc call here is reported this way. */
static void show_ptr(const char* what, const char* base, const char* p) {
  printf("%-34s %s%ld\n", what, p ? "+" : "NULL ", p ? (long)(p - base) : 0L);
}

int main(void) {
  /* --- scanning, in registers only: strncmp/strchr/strrchr/strstr/strspn/strcspn ---------- */
  static const char hay[] = "the quick brown fox jumps over the lazy dog";
  static const char needle[] = "over";
  static const char absent[] = "zebra";
  printf("strncmp(hay,\"the quiet\",7)      %d\n", strncmp(hay, "the quiet", 7));
  printf("strncmp(hay,\"the quick\",7)      %d\n", strncmp(hay, "the quick", 7));
  printf("strncmp(hay,needle,0)            %d\n", strncmp(hay, needle, 0));
  printf("strncmp(\"abc\",\"abd\",3)          %d\n", strncmp("abc", "abd", 3));
  printf("strncmp(\"abd\",\"abc\",3)          %d\n", strncmp("abd", "abc", 3));
  show_ptr("strchr(hay,'q')", hay, strchr(hay, 'q'));
  show_ptr("strchr(hay,'z')", hay, strchr(hay, 'z'));
  show_ptr("strchr(hay,0)", hay, strchr(hay, 0));
  show_ptr("strrchr(hay,'o')", hay, strrchr(hay, 'o'));
  show_ptr("strstr(hay,needle)", hay, strstr(hay, needle));
  show_ptr("strstr(hay,absent)", hay, strstr(hay, absent));
  show_ptr("strstr(hay,\"\")", hay, strstr(hay, ""));
  printf("strspn(hay,\"the quick\")         %zu\n", strspn(hay, "the quick"));
  printf("strcspn(hay,needle)              %zu\n", strcspn(hay, needle));
  printf("strspn(\"00123abcXY\",\"0123456789\") %zu\n", strspn("00123abcXY", "0123456789"));

  /* --- conversion: atoi/strtol/strtod, with the end pointer as an offset ------------------ */
  static const char num[] = "  -0x1f4garbage";
  char* end = (char*)num;
  printf("atoi(\"  +42xyz\")                %d\n", atoi("  +42xyz"));
  printf("atoi(\"not a number\")            %d\n", atoi("not a number"));
  printf("atoi(\"9223372036854775808\")     %d\n", atoi("9223372036854775808"));
  printf("strtol(num, &end, 0)             %ld end+%ld\n", strtol(num, &end, 0), (long)(end - num));
  printf("strtol(num, &end, 10)            %ld end+%ld\n", strtol(num, &end, 10), (long)(end - num));
  printf("strtol(num, &end, 16)            %ld end+%ld\n", strtol(num, &end, 16), (long)(end - num));
  static const char oct[] = "-020";
  printf("strtol(oct, &end, 8)             %ld end+%ld\n", strtol(oct, &end, 8), (long)(end - oct));
  /* Every input below is in range, so far as the C standard's defined behaviour goes. Whether a
   * range-error input would additionally touch one of the three TLS relocations the guest `libc.so`
   * carries — through `errno`, say — has not been established, and *which* objects those three slots
   * belong to is an open question this file does not settle. Choosing in-range inputs keeps the rows
   * about the conversion; trying the others belongs in the lane, which reports what happens. */
  static const char dbl[] = "-1.7976931348623157e308xyz";
  const double d = strtod(dbl, &end);
  uint64_t dbits = 0;
  memcpy(&dbits, &d, sizeof dbits);
  printf("strtod(dbl, &end)                0x%016llx end+%ld\n", (unsigned long long)dbits,
         (long)(end - dbl));
  static const char dbl2[] = "  0.1";
  const double d2 = strtod(dbl2, &end);
  memcpy(&dbits, &d2, sizeof dbits);
  printf("strtod(\"  0.1\", &end)            0x%016llx end+%ld\n", (unsigned long long)dbits,
         (long)(end - dbl2));
  static const char dbl3[] = "1e-320";
  const double d3 = strtod(dbl3, &end);
  memcpy(&dbits, &d3, sizeof dbits);
  printf("strtod(\"1e-320\", &end)           0x%016llx end+%ld\n", (unsigned long long)dbits,
         (long)(end - dbl3));
  printf("abs(-2147483647)                 %d\n", abs(-2147483647));
  printf("abs(-1)                          %d\n", abs(-1));

  /* --- a guest callback: qsort calls its comparator, so libc is the caller ---------------- */
  /* The comparator is *guest* code reached from inside emulated libc, which is the interesting part of
   * this row: it exercises the same function-pointer indirection a JNI callback uses, but with the
   * emulator's own state on both sides. */
  int arr[] = {5, 3, 9, 1, -7, 0, 2147483647, -2147483647 - 1, 42};
  qsort(arr, sizeof arr / sizeof arr[0], sizeof arr[0], cmp_int);
  printf("qsort -> ");
  for (unsigned i = 0; i < sizeof arr / sizeof arr[0]; ++i) printf("%d ", arr[i]);
  printf("\n");

  /* --- strtok_r keeps state through a caller-supplied pointer ----------------------------- */
  char tok_buf[] = "one,,two,three,";
  char* save = NULL;
  printf("strtok_r: ");
  for (char* t = strtok_r(tok_buf, ",", &save); t != NULL; t = strtok_r(NULL, ",", &save)) {
    printf("[%s]", t);
  }
  printf("\n");

  /* `snprintf` is in the lane's *reported* group rather than its asserted one, because it is the row most
   * likely to need state RetroBug has not got (stdio's `FILE` machinery and its lock). Capturing the
   * answer anyway costs one line and turns "what would it have to return?" into a lookup the next time
   * someone promotes the row, instead of another device run. */
  char out_buf[64];
  const int n = snprintf(out_buf, sizeof out_buf, "%d/%s/%c", 42, "xy", '!');
  printf("snprintf -> %d \"%s\"\n", n, out_buf);
  return 0;
}
