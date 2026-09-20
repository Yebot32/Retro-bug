    .syntax unified
    .arch armv8-a
    .thumb

/*
 * Hardware ground truth for the ARMv7 exclusive-access pair, one function per question.
 *
 * Each takes r0 = address, r1 = value to store, and returns STREX's `Rd` in r0: 0 means the store
 * passed, 1 means it failed. Every sequence is single-threaded and every write in it is issued by this
 * same core, which is the case RetroBug exercises today and the case an emulator's memory model has to
 * get right before it can be trusted with two.
 *
 * f2 is the one that settles a design question rather than confirming an obvious one: after an LDREXH,
 * does a *plain* STRH by the same core to the same halfword invalidate the reservation? Address
 * comparison says no, value comparison says yes, and the two are not interchangeable -- a store that
 * fails when hardware lets it through makes the guest retry forever in a loop it cannot exit.
 */

                .thumb_func
                .global f1_plain_pair
f1_plain_pair:                        @ ldrexh then strexh, nothing between: the baseline
                ldrexh   r3, [r0]
                strexh   r2, r1, [r0]
                mov      r0, r2
                bx       lr

                .thumb_func
                .global f2_plain_store_between
f2_plain_store_between:               @ same core writes the halfword with a plain STRH in the middle
                mov      r3, #0x5a
                strh     r3, [r0]
                ldrexh   r3, [r0]
                mov      r2, #0x77
                strh     r2, [r0]
                strexh   r4, r1, [r0]
                mov      r0, r4
                bx       lr

                .thumb_func
                .global f3_clrex_between
f3_clrex_between:                     @ CLREX drops the marker, so the store must fail
                ldrexh   r3, [r0]
                mov      r2, #0x77
                strh     r2, [r0]
                clrex
                strexh   r4, r1, [r0]
                mov      r0, r4
                bx       lr

                .thumb_func
                .global f4_no_prior_load
f4_no_prior_load:                     @ a store with no reservation at all
                clrex
                strexh   r4, r1, [r0]
                mov      r0, r4
                bx       lr

                .thumb_func
                .global f5_other_address
f5_other_address:                     @ reserved one halfword, stored the next
                ldrexh   r3, [r0]
                add      r0, r0, #2
                strexh   r4, r1, [r0]
                mov      r0, r4
                bx       lr

                .thumb_func
                .global f6_acquire_pair
f6_acquire_pair:                      @ LDAEXH, the form bionic's mutexes actually use
                ldaexh   r3, [r0]
                strexh   r2, r1, [r0]
                mov      r0, r2
                bx       lr

                .thumb_func
                .global f7_acquire_plain_store_between
f7_acquire_plain_store_between:       @ LDAEXH, then a plain STRH by this core, then STREXH
                ldaexh   r3, [r0]
                mov      r2, #0x77
                strh     r2, [r0]
                strexh   r4, r1, [r0]
                mov      r0, r4
                bx       lr

                .thumb_func
                .global f8_byte_pair
f8_byte_pair:
                ldrexb   r3, [r0]
                mov      r2, r1
                strexb   r4, r2, [r0]
                mov      r0, r4
                bx       lr

                .thumb_func
                .global f9_double_pair
f9_double_pair:                       @ the 64-bit pair, which is also the LDAEXD/STLEXD shape
                ldrexd   r3, r4, [r0]
                mov      r6, r1
                mov      r7, r1
                strexd   r5, r6, r7, [r0]
                mov      r0, r5
                bx       lr

                .thumb_func
                .global f10_word_pair
f10_word_pair:
                ldrex    r3, [r0]
                strex    r2, r1, [r0]
                mov      r0, r2
                bx       lr

                .thumb_func
                .global f11_acquire_word
f11_acquire_word:                     @ LDAEX, and the loaded value handed back in r1
                ldaex    r3, [r0]
                strex    r2, r3, [r0]                   @ rewrite what was just read
                mov      r0, r2
                bx       lr

                .thumb_func
                .global f12_ordinary_write_then_strexh
f12_ordinary_write_then_strexh:       @ the reservation survives a plain write *through a different
                mov      r3, #0x11                      @ register pointing at the same halfword
                strh     r3, [r0]
                ldrexh   r4, [r0]
                mov      r5, #0x22
                strh     r5, [r0]
                strexh   r6, r1, [r0]
                mov      r0, r6
                bx       lr
