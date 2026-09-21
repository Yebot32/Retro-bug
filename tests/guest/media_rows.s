/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * The media-instruction blobs RetroBug's differential lane plants, written once and assembled once, so
 * that the on-device oracle and the in-emulator lane cannot drift apart.
 *
 * Every `row_*` here is the identical sequence:
 *
 *     ldr   r2, [r0]        ; operand A, at the address the caller passes in r0
 *     ldr   r3, [r0, #4]    ; operand B
 *     uadd8 r4, r2, r3      ; set GE from the *unsigned* rule, on purpose (see below)
 *     ldr   r2, [r0]
 *     ldr   r3, [r0, #4]
 *     <op>  r4, r2, r3      ; the instruction under test
 *     mrs   r3, cpsr
 *     lsrs  r3, #16
 *     movs  r2, #15
 *     ands  r3, r3, r2      ; r3 = CPSR's four GE bits
 *     mov   r0, r4          ; r0 = the instruction's result
 *     bx    lr
 *
 * The leading `uadd8` is what makes the GE half of a row evidence rather than decoration: for the
 * saturating and halving kinds the expected GE is the value this left behind (ARM says they report
 * nothing), and for the add/subtract kinds the operands are chosen so the leading value differs from the
 * one the instruction under test must produce. A row whose GE was simply never written therefore cannot
 * pass.
 *
 * These are the same halfwords `core/tests/exec_tier_diff_test.cpp` copies into guest memory, and they
 * came out of `llvm-objdump` over this file rather than out of a head — the recipe `docs/30` §Step 9
 * established for `ldaexh`. `-mthumb` on both sides; the ARM-state forms of these instructions are not
 * what the emulator's lanes plant.
 */
    .syntax unified
    .thumb
    .text

    .macro PROLOGUE
    ldr   r2, [r0]
    ldr   r3, [r0, #4]
    uadd8 r4, r2, r3
    ldr   r2, [r0]
    ldr   r3, [r0, #4]
    .endm

    .macro EPILOGUE
    mrs   r3, cpsr
    lsrs  r3, #16
    movs  r2, #15
    ands  r3, r3, r2
    mov   r0, r4
    bx    lr
    .endm

    .thumb_func
    .globl row_uadd8
row_uadd8:
    PROLOGUE
    uadd8  r4, r2, r3
    EPILOGUE

    .thumb_func
    .globl row_usub8
row_usub8:
    PROLOGUE
    usub8  r4, r2, r3
    EPILOGUE

    .thumb_func
    .globl row_sadd8
row_sadd8:
    PROLOGUE
    sadd8  r4, r2, r3
    EPILOGUE

    .thumb_func
    .globl row_ssub8
row_ssub8:
    PROLOGUE
    ssub8  r4, r2, r3
    EPILOGUE

    .thumb_func
    .globl row_uqsub8
row_uqsub8:
    PROLOGUE
    uqsub8 r4, r2, r3
    EPILOGUE

    .thumb_func
    .globl row_uqadd8
row_uqadd8:
    PROLOGUE
    uqadd8 r4, r2, r3
    EPILOGUE

    .thumb_func
    .globl row_qsub8
row_qsub8:
    PROLOGUE
    qsub8  r4, r2, r3
    EPILOGUE

    .thumb_func
    .globl row_uhadd8
row_uhadd8:
    PROLOGUE
    uhadd8 r4, r2, r3
    EPILOGUE

    .thumb_func
    .globl row_uhsub8
row_uhsub8:
    PROLOGUE
    uhsub8 r4, r2, r3
    EPILOGUE

    .thumb_func
    .globl row_shadd8
row_shadd8:
    PROLOGUE
    shadd8 r4, r2, r3
    EPILOGUE

    .thumb_func
    .globl row_uadd16
row_uadd16:
    PROLOGUE
    uadd16 r4, r2, r3
    EPILOGUE

    .thumb_func
    .globl row_usub16
row_usub16:
    PROLOGUE
    usub16 r4, r2, r3
    EPILOGUE

    .thumb_func
    .globl row_sel
row_sel:
    PROLOGUE
    sel    r4, r2, r3
    EPILOGUE
    .end
