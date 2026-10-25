#!/usr/bin/env bash
# RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
#
# The layering gate core/README.md has always promised: `core/` is the platform-agnostic engine, so it
# may not include an Android/JNI/GL/Mach header, and it may not call a host interface that exists on one
# platform and not another. Everything of that kind belongs in `platform/` behind rb/platform/host.h.
#
# Usage: tools/check_layering.sh [--report]
#   default   FAIL on both rules (S0b is done: core/ has no platform-specific host calls left)
#   --report  fail on headers only, report the syscall rule
#
# Comments and string literals are stripped before the syscall scan: a message that *names*
# `sched_getaffinity` is documentation, not a call, and a gate that flags its own explanations gets
# switched off within a week. The strip is line-based and deliberately simple; it can only make the gate
# more permissive, never fire on prose.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STRICT=1
[ "${1:-}" = "--report" ] && STRICT=0

fail=0
reported=0

# 1. Headers: no platform SDK in core/. `core/tests` is excluded on purpose — a test may exercise the
#    host's real GL to compare against the thunks; `core/include/rb/platform/` is the seam itself.
mapfile -t HEADER_HITS < <(grep -rnE '#include *<(android/|jni\.h|GLES[0-9]*/|EGL/|TargetConditionals\.h|mach/|os/proc\.h)' \
  "$ROOT/core" --include=*.cpp --include=*.h --include=*.hpp 2>/dev/null \
  | grep -v "/core/tests/" | grep -v "/core/include/rb/platform/" || true)
if [ ${#HEADER_HITS[@]} -gt 0 ]; then
  echo "layering: core/ includes a platform SDK header:"
  printf '  %s\n' "${HEADER_HITS[@]}"
  fail=1
fi

# 2. Host interfaces that exist on one platform only. mmap/mprotect/munmap/clock_gettime/open are
#    deliberately absent: they exist with the same meaning on Linux and Darwin, and wrapping them would
#    hide which semantics the engine relies on.
PATTERN='(^|[^A-Za-z0-9_])(syscall|memfd_create|gettid|prlimit|sysinfo|sched_getaffinity|sched_getparam|sched_getscheduler|sched_get_priority_min|sched_get_priority_max|epoll_create1|epoll_ctl|epoll_wait|eventfd|statx|getdents64|ppoll|pselect6|renameat2|dlopen|pthread_threadid_np)[[:space:]]*\('

SYSCALL_HITS=""
while IFS= read -r f; do
  case "$f" in
    */core/tests/*|*/core/include/rb/platform/*) continue ;;
  esac
  # Strip `//` comments and "..." strings, then look for a call. Line numbers survive the strip.
  hit=$(sed -E 's@//.*$@@; s@"[^"]*"@@g' "$f" | grep -nE "$PATTERN" || true)
  if [ -n "$hit" ]; then
    SYSCALL_HITS+=$(printf '%s\n' "$hit" | sed "s@^@${f#${ROOT}/}:@")$'\n'
  fi
done < <(find "$ROOT/core" -name '*.cpp' -o -name '*.h' | sort)

if [ -n "${SYSCALL_HITS//[$'\n']/}" ]; then
  if [ "$STRICT" = 1 ]; then
    echo "layering: core/ calls a platform-specific host interface (move it behind rb::platform):"
    printf '%s' "$SYSCALL_HITS" | sed '/^$/d' | sed 's/^/  /'
    fail=1
  else
    echo "layering: reported (not failing yet — S0b moves these behind rb::platform):"
    printf '%s' "$SYSCALL_HITS" | sed '/^$/d' | sed 's/^/  /'
    reported=1
  fi
fi

if [ "$fail" = 0 ]; then
  if [ "$reported" = 1 ]; then
    echo "layering: no header violations (syscall findings reported above)"
  else
    echo "layering: clean"
  fi
fi
exit "$fail"
