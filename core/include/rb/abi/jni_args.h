// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The JNI argument conventions both guest JNIEnvs share: the host-class model (`JniEnv`, the launcher's) and the
// forwarding bridge (`ForwardingJni`, M3). A JNI method signature reduced to kind letters, and the Java arguments of
// a `Call*Method`-shaped entry read out of an AAPCS32 softfp guest call in each of its three forms.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rb/exec/machine_state.h"
#include "rb/mem/guest_memory.h"

namespace rb::abi::jni {

/// Where a `Call*Method`-shaped entry's Java arguments are. kVariadic: in the remaining core registers then the
/// stack, with `...`'s promotions (F as a double, Z/B/C/S as int; 8-byte values 8-aligned, an even register pair
/// or the stack). kVaList: AAPCS32's `va_list`, one pointer to such stacked values. kJvalues: a `jvalue*` array of
/// 8-byte slots. kFixed: ordinary (non-variadic) parameters — like kVariadic but F stays a 4-byte float.
enum class ArgForm { kVariadic, kVaList, kJvalues, kFixed };

/// `(params)ret` reduced to one kind letter per parameter (Z B C S I J F D, L for any reference or array) and the
/// return kind (the same alphabet plus V). False on anything that is not a well-formed JNI method signature.
bool ParseSignature(const std::string& sig, std::vector<char>* params, char* ret);

/// The 32-bit argument word `index` of the guest call in `s` (r0-r3, then the stack at sp). False if unreadable.
bool ArgWord(const exec::MachineState& s, const mem::GuestMemory& m, unsigned index, std::uint32_t* out);

/// The Java arguments, one per `params` letter, after `fixed` leading argument words (3 for `(env, obj, mid, ...)`,
/// 4 for the nonvirtual `(env, obj, cls, mid, ...)`). For kVaList/kJvalues the pointer is argument word `fixed`.
/// Each value is returned in its own bits: Z/C zero-extended, B/S sign-extended, I/F/L as 32 bits, J/D as 64.
bool DecodeArgs(const exec::MachineState& s, const mem::GuestMemory& m, ArgForm form, const std::vector<char>& params,
                unsigned fixed, std::vector<std::uint64_t>* out, std::string* err);

}  // namespace rb::abi::jni
