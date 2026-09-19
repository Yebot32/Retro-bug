/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * AArch32-at-EL0 capability probe. Answers a question the research left unverified: on an arm64
 * Android build that still advertises `abilist32=armeabi-v7a`, can a 32-bit ARM *process* actually
 * be executed, or is the ABI list a packaging leftover? docs/research/01 treated hardware AArch32 as
 * a fair-weather path and docs/10 AD-1 ruled it out for v1 on that reasoning; this measures it.
 *
 * Built with `-static` for armv7 so the result cannot be attributed to a missing loader or library:
 * if the kernel can start this ELF, AArch32 EL0 is live; if it refuses, it is not, regardless of what
 * the system properties claim.
 *
 * Read the outcome as: this tells us about *this device*, not the fleet. ARMv9 clusters differ by
 * core — Cortex-A715/A510 drop AArch32 at EL0 while Cortex-X3 keeps it — so a big.LITTLE part can
 * still start 32-bit code on one core. Mixed results across devices are expected and are the reason
 * AD-1 does not build on it.
 */
#include <stdio.h>
#include <sys/auxv.h>
#include <unistd.h>

int main(void) {
  unsigned long hwcap = getauxval(AT_HWCAP);
  printf("RB_AARCH32_PROBE alive pid=%d hwcap=0x%lx "
         "swp=%d half=%d thumb=%d vfp=%d neon=%d vfpv3=%d idiva=%d idivet=%d\n",
         (int)getpid(), hwcap,
         (hwcap >> 6) & 1u,   /* HWCAP_SWP  */
         (hwcap >> 7) & 1u,   /* HWCAP_HALF */
         (hwcap >> 8) & 1u,   /* HWCAP_THUMB (always 1 in practice) */
         (hwcap >> 12) & 1u,  /* HWCAP_VFP  */
         (hwcap >> 13) & 1u,  /* HWCAP_NEON */
         (hwcap >> 18) & 1u,  /* HWCAP_VFPv3 */
         (hwcap >> 26) & 1u,  /* HWCAP_IDIVA */
         (hwcap >> 27) & 1u); /* HWCAP_IDIVT */
  return 0;
}
