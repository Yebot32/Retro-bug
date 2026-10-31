// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// rb_thunk_tests (M4 Task 1): the generic AAPCS32-softfp -> AAPCS64 thunk caller and the handle table. Each row
// builds the guest's registers and stacked words by hand, as a guest caller leaves them at a host slot, calls a
// real host function through `ThunkCaller::Call` with a signature string, and checks what the host function
// received and what the guest gets back in r0/r1.
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "rb/abi/thunk.h"
#include "rb/mem/guest_memory.h"

namespace {

using namespace rb;

int checks = 0, failures = 0;

void fail(const char* what, int line, const std::string& detail = {}) {
  ++failures;
  std::printf("FAIL  %s  (line %d)%s%s\n", what, line, detail.empty() ? "" : "  ", detail.c_str());
}
#define CHECK(...)                                   \
  do {                                               \
    ++checks;                                        \
    if (!(__VA_ARGS__)) fail(#__VA_ARGS__, __LINE__); \
  } while (0)
#define CHECK_CTX(cond, detail)                \
  do {                                         \
    ++checks;                                  \
    if (!(cond)) fail(#cond, __LINE__, detail); \
  } while (0)

constexpr std::uint32_t kStack = 0x10000u;  // the guest sp: stacked argument words start here

std::unique_ptr<mem::GuestMemory> Window() {
  mem::AddressModel model;
  model.kind = mem::AddressModel::kBiased;
  std::string err;
  auto m = mem::GuestMemory::Create(std::size_t{1} << 20, model, &err);
  if (m) m->Map(kStack, 0x2000, mem::kRead | mem::kWrite, &err);
  if (m) m->Map(0x20000, 0x1000, mem::kRead | mem::kWrite, &err);
  return m;
}

/// A guest state with words `w` laid out as AAPCS32 passes them: r0..r3, then the stack at sp.
exec::MachineState State(mem::GuestMemory& m, std::initializer_list<std::uint32_t> w) {
  exec::MachineState s{};
  s.r[13] = kStack;
  unsigned i = 0;
  for (std::uint32_t v : w) {
    if (i < 4) {
      s.r[i] = v;
    } else {
      m.Write(kStack + 4u * (i - 4u), &v, 4);
    }
    ++i;
  }
  return s;
}

std::uint32_t Lo(double d) {
  std::uint64_t b;
  std::memcpy(&b, &d, 8);
  return static_cast<std::uint32_t>(b);
}
std::uint32_t Hi(double d) {
  std::uint64_t b;
  std::memcpy(&b, &d, 8);
  return static_cast<std::uint32_t>(b >> 32);
}
std::uint32_t Bits(float f) {
  std::uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}

// ---- host functions the rows call
struct Seen {
  std::int64_t i[8] = {};
  double d[4] = {};
  float f[4] = {};
  const void* p[2] = {};
} seen;

int six_ints(int a, int b, int c, int d, int e, int f) { return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f; }
void mixed(int a, double b, int c, long long d, float e, double f) {
  seen.i[0] = a;
  seen.d[0] = b;
  seen.i[1] = c;
  seen.i[2] = d;
  seen.f[0] = e;
  seen.d[1] = f;
}
float fmul(float a, float b) { return a * b; }
double dhalf(double a) { return a / 2; }
long long lneg(long long a) { return -a; }
int strlen_of(const char* s) { return s == nullptr ? -1 : static_cast<int>(std::strlen(s)); }
void record_ptr(const void* p) { seen.p[0] = p; }
/// 64-bit parameters (size_t, ssize_t, GLsizeiptr) see the extension: u zero-extended, i sign-extended.
unsigned long long widen(unsigned long long u, long long i) {
  seen.i[0] = static_cast<std::int64_t>(u);
  seen.i[1] = i;
  return 0;
}
int dummy_object = 7;
void* make_handle() { return &dummy_object; }
int read_handle(const int* p) { return p == nullptr ? -1 : *p; }
int nine_ints(int a, int b, int c, int d, int e, int f, int g, int h, int i) {
  return a + b + c + d + e + f + g + h + 100 * i;
}

void ints_beyond_the_registers() {
  auto m = Window();
  abi::HandleTable h;
  exec::MachineState s = State(*m, {1, 2, 3, 4, 5, static_cast<std::uint32_t>(-6)});
  std::string e;
  CHECK_CTX(abi::ThunkCaller::Call("i:iiiiii", reinterpret_cast<void*>(&six_ints), s, *m, h, &e), e);
  CHECK_CTX(static_cast<std::int32_t>(s.r[0]) == 1 + 4 + 9 + 16 + 25 - 36, std::to_string(s.r[0]));
  exec::MachineState s9 = State(*m, {1, 1, 1, 1, 1, 1, 1, 1, 7});
  CHECK_CTX(abi::ThunkCaller::Call("i:iiiiiiiii", reinterpret_cast<void*>(&nine_ints), s9, *m, h, &e), e);
  CHECK_CTX(s9.r[0] == 708u, "the ninth integer goes to the host stack: " + std::to_string(s9.r[0]));
}

/// AAPCS32: a double or int64 takes an even register pair (r2:r3 here), never splits, and on the stack an
/// 8-aligned slot; softfp passes floats in core words.
void doubles_and_int64_are_aligned() {
  auto m = Window();
  abi::HandleTable h;
  // (int a, double b, int c, long long d, float e, double f):
  //   a r0 | pad r1 | b r2:r3 | c stack0 | pad stack1 | d stack2:3 | e stack4 | pad stack5 | f stack6:7
  const double b = 1.25, f = -3.5;
  const long long d = 0x1234'5678'9abc'def0LL;
  exec::MachineState s = State(*m, {11, 0xdead, Lo(b), Hi(b), 22, 0xdead, static_cast<std::uint32_t>(d),
                                    static_cast<std::uint32_t>(d >> 32), Bits(0.75f), 0xdead, Lo(f), Hi(f)});
  std::string e;
  seen = {};
  CHECK_CTX(abi::ThunkCaller::Call("v:idilfd", reinterpret_cast<void*>(&mixed), s, *m, h, &e), e);
  CHECK(seen.i[0] == 11 && seen.d[0] == b && seen.i[1] == 22 && seen.i[2] == d && seen.f[0] == 0.75f &&
        seen.d[1] == f);
}

void float_and_double_results_come_back_softfp() {
  auto m = Window();
  abi::HandleTable h;
  std::string e;
  exec::MachineState s = State(*m, {Bits(1.5f), Bits(4.0f)});
  CHECK_CTX(abi::ThunkCaller::Call("f:ff", reinterpret_cast<void*>(&fmul), s, *m, h, &e), e);
  CHECK_CTX(s.r[0] == Bits(6.0f), "r0 0x" + std::to_string(s.r[0]));
  exec::MachineState sd = State(*m, {Lo(9.0), Hi(9.0)});
  CHECK_CTX(abi::ThunkCaller::Call("d:d", reinterpret_cast<void*>(&dhalf), sd, *m, h, &e), e);
  CHECK(sd.r[0] == Lo(4.5) && sd.r[1] == Hi(4.5));
  const long long v = 0x0000'0001'0000'0002LL;
  exec::MachineState sl = State(*m, {static_cast<std::uint32_t>(v), static_cast<std::uint32_t>(v >> 32)});
  CHECK_CTX(abi::ThunkCaller::Call("l:l", reinterpret_cast<void*>(&lneg), sl, *m, h, &e), e);
  CHECK(sl.r[0] == static_cast<std::uint32_t>(-v) && sl.r[1] == static_cast<std::uint32_t>((-v) >> 32));
}

void unsigned_is_zero_extended_signed_is_sign_extended() {
  auto m = Window();
  abi::HandleTable h;
  std::string e;
  exec::MachineState s = State(*m, {0xffff'fff0u, 0xffff'fff0u});
  seen = {};
  CHECK_CTX(abi::ThunkCaller::Call("v:ui", reinterpret_cast<void*>(&widen), s, *m, h, &e), e);
  CHECK_CTX(seen.i[0] == 0xffff'fff0LL && seen.i[1] == -16, std::to_string(seen.i[0]) + " " + std::to_string(seen.i[1]));
}

void pointers_are_translated_and_checked() {
  auto m = Window();
  abi::HandleTable h;
  std::string e;
  const char text[] = "guest!";
  m->Write(0x20000, text, sizeof text);
  exec::MachineState s = State(*m, {0x20000});
  CHECK_CTX(abi::ThunkCaller::Call("i:p", reinterpret_cast<void*>(&strlen_of), s, *m, h, &e), e);
  CHECK(s.r[0] == 6u);
  exec::MachineState n = State(*m, {0});
  CHECK_CTX(abi::ThunkCaller::Call("i:p", reinterpret_cast<void*>(&strlen_of), n, *m, h, &e), e);
  CHECK(static_cast<std::int32_t>(n.r[0]) == -1);
  exec::MachineState bad = State(*m, {0x80000});  // inside the window, never mapped
  e.clear();
  CHECK(!abi::ThunkCaller::Call("i:p", reinterpret_cast<void*>(&strlen_of), bad, *m, h, &e) &&
        e.find("0x80000") != std::string::npos);
  // `o`: GL's pointer-or-offset. Guest memory is translated; a small value (a buffer offset) is passed as is.
  exec::MachineState off = State(*m, {0x40});
  seen = {};
  CHECK_CTX(abi::ThunkCaller::Call("v:o", reinterpret_cast<void*>(&record_ptr), off, *m, h, &e), e);
  CHECK(seen.p[0] == reinterpret_cast<const void*>(std::uintptr_t{0x40}));
  exec::MachineState on = State(*m, {0x20000});
  CHECK_CTX(abi::ThunkCaller::Call("v:o", reinterpret_cast<void*>(&record_ptr), on, *m, h, &e), e);
  CHECK(seen.p[0] == m->Host(0x20000));
}

/// A host callee that reads guest bytes (a GL driver handed a shader string or a vertex array) must accept a
/// page the memory model serves out of line: `Host` refuses that page by design, and the thunk layer used to
/// refuse it too — the shape a 16 KiB host page gives every file-backed module page at a 4 KiB-aligned base.
void out_of_line_pointer_args_are_dereferenceable() {
  // A window with 16 KiB granules whatever the host's own page size: the shape that leaves a file page
  // out of line must be reproducible on a 4 KiB runner as well as a 16 KiB one.
  unsigned native_shift = 12;
  while ((std::size_t{1} << native_shift) < static_cast<std::size_t>(sysconf(_SC_PAGESIZE))) ++native_shift;
  mem::AddressModel model;
  model.kind = mem::AddressModel::kBiased;
  model.host_page_shift = native_shift > 14 ? native_shift : 14;
  std::string created;
  auto m = mem::GuestMemory::Create(std::size_t{1} << 20, model, &created);
  if (!m) {
    fail("16 KiB-granule window creation failed", __LINE__, created);
    return;
  }
  CHECK(m->Map(kStack, 0x2000, mem::kRead | mem::kWrite, &created));
  abi::HandleTable h;
  std::string e;
  int fd = -1;
#if defined(SYS_memfd_create)
  fd = static_cast<int>(syscall(SYS_memfd_create, "rb-thunk", 0u));
#endif
  if (fd < 0) {
    std::printf("# no memfd (%s): out-of-line pointer argument not exercised\n", std::strerror(errno));
    return;
  }
  const char text[] = "offscreen";
  CHECK(::pwrite(fd, text, sizeof text, 0) == static_cast<ssize_t>(sizeof text));
  // Guest 0x1000 with file offset 0 cannot be served in place: the granule base would need a negative file
  // offset, so the page goes out of line on every host.
  CHECK(m->MapFile(0x1000, mem::kGuestPage, mem::kRead, fd, 0, false, &e));
  ::close(fd);
  CHECK(m->IsMapped(0x1000));
  CHECK(m->Host(0x1000) == nullptr);
  exec::MachineState s = State(*m, {0x1000});
  CHECK_CTX(abi::ThunkCaller::Call("i:p", reinterpret_cast<void*>(&strlen_of), s, *m, h, &e), e);
  CHECK_CTX(s.r[0] == 9u, std::to_string(s.r[0]));
  // The offset-or-pointer form resolves it the same way (or it would be passed as a buffer offset).
  exec::MachineState o = State(*m, {0x1000});
  seen = {};
  CHECK_CTX(abi::ThunkCaller::Call("v:o", reinterpret_cast<void*>(&record_ptr), o, *m, h, &e), e);
  CHECK(seen.p[0] != nullptr && std::strcmp(static_cast<const char*>(seen.p[0]), "offscreen") == 0);
}

void host_pointers_become_handles() {
  auto m = Window();
  abi::HandleTable h;
  std::string e;
  exec::MachineState s = State(*m, {});
  CHECK_CTX(abi::ThunkCaller::Call("H:", reinterpret_cast<void*>(&make_handle), s, *m, h, &e), e);
  const std::uint32_t handle = s.r[0];
  CHECK_CTX(handle != 0 && handle != static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&dummy_object)),
            "handle 0x" + std::to_string(handle));
  exec::MachineState again = State(*m, {});
  abi::ThunkCaller::Call("H:", reinterpret_cast<void*>(&make_handle), again, *m, h, &e);
  CHECK(again.r[0] == handle);  // one host object, one handle
  exec::MachineState use = State(*m, {handle});
  CHECK_CTX(abi::ThunkCaller::Call("i:h", reinterpret_cast<void*>(&read_handle), use, *m, h, &e), e);
  CHECK(use.r[0] == 7u);
  exec::MachineState null = State(*m, {0});
  CHECK_CTX(abi::ThunkCaller::Call("i:h", reinterpret_cast<void*>(&read_handle), null, *m, h, &e), e);
  CHECK(static_cast<std::int32_t>(null.r[0]) == -1);
  exec::MachineState forged = State(*m, {handle + 4});
  e.clear();
  CHECK(!abi::ThunkCaller::Call("i:h", reinterpret_cast<void*>(&read_handle), forged, *m, h, &e) &&
        e.find("handle") != std::string::npos);
  h.Forget(&dummy_object);
  exec::MachineState gone = State(*m, {handle});
  CHECK(!abi::ThunkCaller::Call("i:h", reinterpret_cast<void*>(&read_handle), gone, *m, h, &e));
}

void a_bad_signature_is_refused_by_name() {
  auto m = Window();
  abi::HandleTable h;
  std::string e;
  exec::MachineState s = State(*m, {});
  CHECK(!abi::ThunkCaller::Call("i:q", reinterpret_cast<void*>(&six_ints), s, *m, h, &e) &&
        e.find("'q'") != std::string::npos);
  CHECK(!abi::ThunkCaller::Call("i:fffffffff", reinterpret_cast<void*>(&six_ints), s, *m, h, &e));
}

}  // namespace

int main() {
  ints_beyond_the_registers();
  doubles_and_int64_are_aligned();
  float_and_double_results_come_back_softfp();
  unsigned_is_zero_extended_signed_is_sign_extended();
  pointers_are_translated_and_checked();
  out_of_line_pointer_args_are_dereferenceable();
  host_pointers_become_handles();
  a_bad_signature_is_refused_by_name();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
