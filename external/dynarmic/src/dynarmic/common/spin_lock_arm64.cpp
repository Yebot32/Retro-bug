/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <mutex>
#if defined(__APPLE__)
#include <thread>
#endif

#include <oaknut/code_block.hpp>
#include <oaknut/oaknut.hpp>

#include "dynarmic/backend/arm64/abi.h"
#include "dynarmic/common/spin_lock.h"

namespace Dynarmic {

using Backend::Arm64::Wscratch0;
using Backend::Arm64::Wscratch1;
using namespace oaknut::util;

void EmitSpinLockLock(oaknut::CodeGenerator& code, oaknut::XReg ptr) {
    oaknut::Label start, loop;

    code.MOV(Wscratch1, 1);
    code.SEVL();
    code.l(start);
    code.WFE();
    code.l(loop);
    code.LDAXR(Wscratch0, ptr);
    code.CBNZ(Wscratch0, start);
    code.STXR(Wscratch0, Wscratch1, ptr);
    code.CBNZ(Wscratch0, loop);
}

void EmitSpinLockUnlock(oaknut::CodeGenerator& code, oaknut::XReg ptr) {
    code.STLR(WZR, ptr);
}

#if defined(__APPLE__)

/**
 * RetroBug (patches/dynarmic/0009): an atomic test-and-set instead of a lock that emits itself.
 *
 * This lock used to generate its own code the first time it was taken. iOS refuses a launch without a
 * JIT grant the writable code page that costs, and the AOT tier's first call takes this lock through
 * `ExclusiveMonitor::ClearProcessor` before any translated block runs — so the interpreter-only path
 * died with SIGBUS at that very call (KitKat, `__init_tcb`, builds 22-36; the crash report's frames
 * were CrashSignal ← SpinLock::Lock ← ClearProcessor ← AotExecutor::SyncIn). The atomic has the same
 * mutual exclusion and needs no executable memory, which is the property the AOT tier is built on.
 *
 * Keep the generated form where it works: Android's shipping tier is the JIT, and its WFE spin is
 * measurably cheaper than a yield loop.
 */
void SpinLock::Lock() {
    while (__atomic_exchange_n(&storage, 1, __ATOMIC_ACQUIRE) != 0) {
        while (__atomic_load_n(&storage, __ATOMIC_RELAXED) != 0) {
            std::this_thread::yield();
        }
    }
}

void SpinLock::Unlock() {
    __atomic_store_n(&storage, 0, __ATOMIC_RELEASE);
}

#else

namespace {

struct SpinLockImpl {
    SpinLockImpl();

    void Initialize();

    oaknut::CodeBlock mem;
    oaknut::CodeGenerator code;

    void (*lock)(volatile int*);
    void (*unlock)(volatile int*);
};

std::once_flag flag;
SpinLockImpl impl;

SpinLockImpl::SpinLockImpl()
        : mem{4096}
        , code{mem.ptr(), mem.ptr()} {}

void SpinLockImpl::Initialize() {
    mem.unprotect();

    lock = code.xptr<void (*)(volatile int*)>();
    EmitSpinLockLock(code, X0);
    code.RET();

    unlock = code.xptr<void (*)(volatile int*)>();
    EmitSpinLockUnlock(code, X0);
    code.RET();

    mem.protect();
    mem.invalidate_all();
}

}  // namespace

void SpinLock::Lock() {
    std::call_once(flag, &SpinLockImpl::Initialize, impl);
    impl.lock(&storage);
}

void SpinLock::Unlock() {
    std::call_once(flag, &SpinLockImpl::Initialize, impl);
    impl.unlock(&storage);
}

#endif

}  // namespace Dynarmic
