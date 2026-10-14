/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * A guest library that imports `strcpy` and `strcat` from the device's own `libc.so` — two of the IFUNC
 * exports libc never calls through its own GOT, so libc's load resolves no slot for them and an importer
 * is bound to whatever the linker does about that. Gumball 2013 was bound to `strcpy`'s *resolver* and
 * copied nothing (2026-10-14); `rb_guest_libc_tests` calls these two and reads the destination back.
 *
 * Built `-fno-builtin` so the compiler emits the calls instead of expanding them, and without fortify so
 * the import is `strcpy` and not `__strcpy_chk`.
 */
#include <string.h>

char* rb_strcpy(char* dst, const char* src) { return strcpy(dst, src); }

char* rb_strcat(char* dst, const char* src) { return strcat(dst, src); }
