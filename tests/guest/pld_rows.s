    .syntax unified

/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * What an AArch32 core actually does with a preload hint, in both ISA states, at three address classes.
 *
 * These are the bytes RetroBug's two tiers execute. The lane at
 * `core/tests/exec_tier_diff_test.cpp` asserts that Tier I treats a preload as a no-op and continues
 * because Tier J does -- which is two implementations agreeing with each other, and this project's own
 * rule is that a tier-vs-tier disagreement is a question, not an answer. Until this file ran on a device
 * there was no hardware evidence about `pld` anywhere in the repo, only a counted occurrence in bionic's
 * disassembly.
 *
 * Each row returns a `uint64_t` in r0:r1 so one call answers two different questions:
 *   - does the *value* prove execution continued past the hint (the two `add`s either side, or the
 *     sentinel surviving the flag row), and
 *   - does the *other* half prove the hint changed nothing it must not (the address register comes back
 *     exactly as it went in).
 * The flag row has no `add` at all on purpose: it sets C with `cmn`, executes a preload, and folds C into
 * the result with `adc`, so a preload that touched the flags is visible as an off-by-one rather than as a
 * crash.
 *
 * ARM rows take r0 = sentinel, r1 = address. Thumb rows take r0 = address because that is the register the
 * 16-bit-adjacent Thumb encoding can address.
 */

                .arm

                .global p1_arm_pld_neg
                .type   p1_arm_pld_neg, %function
p1_arm_pld_neg:                     @ ARM: pld [r1, #-0x400]
                add     r0, r0, #1
                pld     [r1, #-0x400]
                add     r0, r0, #1
                bx      lr
                .size   p1_arm_pld_neg, .-p1_arm_pld_neg

                .global p2_arm_pld_pos
                .type   p2_arm_pld_pos, %function
p2_arm_pld_pos:                     @ ARM: pld [r1, #0x400]
                add     r0, r0, #1
                pld     [r1, #0x400]
                add     r0, r0, #1
                bx      lr
                .size   p2_arm_pld_pos, .-p2_arm_pld_pos

                .global p3_arm_pld_plain
                .type   p3_arm_pld_plain, %function
p3_arm_pld_plain:                   @ ARM: pld [r1], the shape inside a stepping loop
                add     r0, r0, #1
                pld     [r1]
                add     r0, r0, #1
                bx      lr
                .size   p3_arm_pld_plain, .-p3_arm_pld_plain

                .global p4_arm_pld_flags
                .type   p4_arm_pld_flags, %function
p4_arm_pld_flags:                   @ does the hint leave CPSR alone? r0 must come back = sentinel
                cmn     r0, #0      @ carry-out of x+0 is always clear, so this sets C=0 deterministically
                pld     [r1, #0x400]
                adc     r0, r0, #0  @ adds C back in: a preload that set C shows as sentinel+1
                bx      lr
                .size   p4_arm_pld_flags, .-p4_arm_pld_flags

                .thumb
                .thumb_func
                .global t1_thumb_pld_r0
t1_thumb_pld_r0:                    @ Thumb: pld [r0]
                movs    r1, #0xa5
                pld     [r0]
                adds    r1, r1, #1  @ execution reached past the hint
                bx      lr

                .thumb_func
                .global t2_thumb_pld_imm
t2_thumb_pld_imm:                   @ Thumb: pld [r0, #0x100]
                movs    r1, #0xa5
                pld     [r0, #0x100]
                adds    r1, r1, #1
                bx      lr

                .arm
                .global c1_arm_ldr_control
                .type   c1_arm_ldr_control, %function
c1_arm_ldr_control:                 @ the falsifier: a REAL load from the same address must take a fault.
                ldr     r2, [r1, #0x400]
                add     r0, r0, #1
                bx      lr
                .size   c1_arm_ldr_control, .-c1_arm_ldr_control
