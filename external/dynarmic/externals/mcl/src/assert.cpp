// This file is part of the mcl project.
// Copyright (c) 2022 merryhime
// SPDX-License-Identifier: MIT

#include "mcl/assert.hpp"

#include <cstdio>
#include <exception>
#include <string>

#include <fmt/format.h>

namespace mcl::detail {

namespace {

void (*g_assert_hook)(const char*) = nullptr;

}  // namespace

void set_assert_hook(void (*hook)(const char*))
{
    g_assert_hook = hook;
}

[[noreturn]] void assert_terminate_impl(const char* expr_str, fmt::string_view msg, fmt::format_args args)
{
    // RetroBug (patches/dynarmic/0012): a tool that translates code it cannot run may ask Dynarmic to
    // translate a byte range that is data, and the front end or an emitter answers with this assertion.
    // A hook may take the message and throw instead, so the caller can refuse that one block and keep
    // going; the emulator never installs one, so a real engine assertion still terminates the process.
    const std::string text = fmt::format("assertion failed: {}\nMessage:{}", expr_str,
                                         fmt::vformat(msg, args));
    if (g_assert_hook != nullptr) {
        g_assert_hook(text.c_str());
    }
    fmt::print(stderr, "assertion failed: {}\nMessage:", expr_str);
    fmt::vprint(stderr, msg, args);
    std::fflush(stderr);
    std::terminate();
}

}  // namespace mcl::detail
