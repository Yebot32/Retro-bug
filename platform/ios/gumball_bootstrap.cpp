// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
#include "gumball_bootstrap.h"

#include <algorithm>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iterator>

#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <OpenGLES/ES3/gl.h>

#include "rb/abi/jni_forward.h"
#include "rb/host/boot.h"
#include "rb/runtime/guest_process.h"

namespace rb::platform::ios {
namespace {

constexpr char kSurface[] = "com/twentysixdigital/gumballjava/JNIGLSurface";
constexpr char kExportPrefix[] = "Java_com_twentysixdigital_gumballjava_JNIGLSurface_";
constexpr unsigned kMaxSize = 4096;
constexpr std::size_t kMaxFile = 16u << 20;
using Java = java::TableJava;
using Word = abi::Value32;

struct Image {
  unsigned width = 128, height = 128, texture_width = 128, texture_height = 128;
  std::vector<std::uint8_t> pixels;
};

Image Decode(const std::vector<std::uint8_t>& bytes) {
  Image result;
  if (bytes.empty()) return result;
  CFDataRef data = CFDataCreateWithBytesNoCopy(nullptr, bytes.data(), bytes.size(), kCFAllocatorNull);
  if (!data) return result;
  CGImageSourceRef source = CGImageSourceCreateWithData(data, nullptr);
  CFRelease(data);
  if (!source) return result;
  CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
  CFRelease(source);
  if (!image) return result;
  const auto width = CGImageGetWidth(image), height = CGImageGetHeight(image);
  if (width > 0 && height > 0 && width <= kMaxSize && height <= kMaxSize) {
    result.width = static_cast<unsigned>(width);
    result.height = static_cast<unsigned>(height);
    result.texture_width = std::bit_ceil(result.width);
    result.texture_height = std::bit_ceil(result.height);
    result.pixels.resize(std::size_t{result.texture_width} * result.texture_height * 4);
    auto color = CGColorSpaceCreateDeviceRGB();
    auto context = CGBitmapContextCreate(result.pixels.data(), result.texture_width, result.texture_height,
        8, result.texture_width * 4, color, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(color);
    if (context) {
      CGContextDrawImage(context, CGRectMake(0, 0, result.texture_width, result.texture_height), image);
      CGContextRelease(context);
    } else {
      result = {};
    }
  }
  CGImageRelease(image);
  return result;
}

std::string AssetName(const std::u16string& name) {
  const auto path = std::filesystem::path(name).generic_u8string();
  return std::string(path.begin(), path.end());
}

std::vector<std::uint8_t> ReadEntry(loader::ApkArchive& apk, const std::string& name) {
  const auto path = "assets/" + name;
  const auto length = apk.EntrySize(path);
  if (!length || *length > kMaxFile) return {};
  std::string error;
  auto bytes = apk.Read(path, &error);
  return bytes ? std::move(*bytes) : std::vector<std::uint8_t>{};
}

std::filesystem::path SavePath(const std::string& directory, const std::u16string& name) {
  const auto path = std::filesystem::path(name);
  if (path.empty() || path.has_parent_path() || path == "." || path == ".." || name.find(u'\0') != name.npos) return {};
  return std::filesystem::path(directory) / "gumball" / path;
}

Java::Ref ByteArray(Java& java, const std::vector<std::uint8_t>& bytes) {
  const auto length = static_cast<std::int32_t>(bytes.size());
  const auto array = java.NewArray('B', length, 0, 0);
  if (length != 0 && !java.SetArrayRegion('B', array, 0, length, bytes.data())) return 0;
  return array;
}

}  // namespace

GumballBootstrap::GumballBootstrap(host::Boot& boot, Java& java, std::string lib, std::string data_dir)
    : boot_(boot), java_(java), lib_(std::move(lib)), data_dir_(std::move(data_dir)) {}

void GumballBootstrap::InstallJava() {
  java_.DefineClass(kSurface, "java/lang/Object");
  java_.DefineMethod(kSurface, "dataFromFile", "(Ljava/lang/String;)[B", false,
                     [this](Java& j, int, const auto& args) {
    return ByteArray(j, ReadEntry(*apk_, AssetName(j.GetStringChars(args.at(0)))));
  });
  java_.DefineMethod(kSurface, "databankRead", "(Ljava/lang/String;)[B", false,
                     [this](Java& j, int, const auto& args) -> Java::Ref {
    const auto path = SavePath(data_dir_, j.GetStringChars(args.at(0)));
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() < 0 || file.tellg() > static_cast<std::streamoff>(kMaxFile)) return 0;
    const auto length = static_cast<std::size_t>(file.tellg());
    std::vector<std::uint8_t> bytes(length);
    file.seekg(0);
    if (length && !file.read(reinterpret_cast<char*>(bytes.data()), length)) return 0;
    return ByteArray(j, bytes);
  });
  java_.DefineMethod(kSurface, "databankWrite", "(Ljava/lang/String;[B)V", false,
                     [this](Java& j, int, const auto& args) {
    const auto path = SavePath(data_dir_, j.GetStringChars(args.at(0)));
    const auto length = j.GetArrayLength(args.at(1));
    if (path.empty() || length < 0 || static_cast<std::size_t>(length) > kMaxFile) return 0u;
    std::vector<std::uint8_t> bytes(length);
    if (length && !j.GetArrayRegion('B', args.at(1), 0, length, bytes.data())) return 0u;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return 0u;
    auto temp = path;
    temp += ".tmp";
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    if (length) file.write(reinterpret_cast<const char*>(bytes.data()), length);
    file.close();
    if (file) std::filesystem::rename(temp, path, error);
    return 0u;
  });
  java_.DefineMethod(kSurface, "queueTexture", "(Ljava/lang/String;ZZ)V", false,
                     [this](Java& j, int, const auto& args) {
    if (args.at(0) == 0) return 0u;
    Texture request{j.GetStringChars(args.at(0)), args.at(1) != 0};
    if (args.at(2) != 0) {
      textures_.push_back(std::move(request));
    } else {
      std::string error;
      if (!DeliverTexture(request, &error)) j.SetPending("java/lang/IllegalStateException", error);
    }
    return 0u;
  });
  for (const auto& [name, signature] : {
       std::pair{"showAlert", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V"},
       {"soundCommand", "(Ljava/lang/String;I)V"}, {"textCommand", "(IILjava/lang/String;IIIII)V"},
       {"openBrowser", "(Ljava/lang/String;)V"}, {"storeBuy", "(Ljava/lang/String;Ljava/lang/String;)V"},
       {"triggerShareInterface", "(I)V"}, {"thumbnailCommand", "(III)V"}, {"setThumbnailImage", "(II[B)V"},
       {"chartboostShowInterstitial", "()V"}, {"chartboostShowMoreGames", "()V"}, {"pickPhoto", "()V"}, {"takePhoto", "()V"}}) {
    java_.DefineMethod(kSurface, name, signature, false, [](Java&, int, const auto&) { return 0u; });
  }
}

bool GumballBootstrap::Call(const char* method, const char* shorty, const abi::Args32& args, std::string* error) {
  const auto* module = boot_.module(lib_);
  const auto entry = module ? module->FindExport(std::string(kExportPrefix) + method) : std::nullopt;
  if (!entry) { *error = std::string("Gumball has no native ") + method; return false; }
  auto& guest = *boot_.process();
  runtime::Ggl::Held held(guest.ggl());
  auto* jni = boot_.jni();
  jni->PushFrame();
  const auto receiver = surface_;
  abi::Args32 placed{Word::Ptr(jni->env()), Word::Ptr(receiver)};
  placed.insert(placed.end(), args.begin(), args.end());
  exec::RunResult result{};
  if (receiver) guest.abi().CallGuestToCompletion(entry->tagged(), placed, abi::Shorty::For(shorty),
                                                 runtime::GuestProcess::kThreadTurnBudget, &result);
  jni->PopFrame(0);
  if (!receiver) return false;
  if (result.reason != exec::StopReason::Returned) {
    *error = std::string("Gumball ") + method + " stopped: " + result.detail;
    return false;
  }
  if (java_.ExceptionCheck()) {
    *error = std::string("Gumball ") + method + ": " + java_.PendingClass() + ": " + java_.PendingMessage();
    return false;
  }
  return true;
}

bool GumballBootstrap::DeliverTexture(const Texture& texture, std::string* error) {
  const auto name = AssetName(texture.name);
  Image bitmap;
  unsigned scale = 1;
  if (name.find("@2x") == std::string::npos) {
    const auto dot = name.rfind('.');
    const auto high = dot == std::string::npos ? name + "@2x" : name.substr(0, dot) + "@2x" + name.substr(dot);
    bitmap = Decode(ReadEntry(*apk_, high));
    if (!bitmap.pixels.empty()) scale = 2;
  }
  if (bitmap.pixels.empty()) bitmap = Decode(ReadEntry(*apk_, name));
  if (bitmap.pixels.empty()) {
    boot_.Live("gumball: unreadable texture " + name + ", using the Java 128x128 fallback");
    bitmap.pixels.resize(128 * 128 * 4);
  }
  auto& guest = *boot_.process();
  runtime::Ggl::Held held(guest.ggl());
  auto* jni = boot_.jni();
  jni->PushFrame();
  const auto str = jni->ToGuestLocal(java_.NewString(texture.name), error);
  bool ok = false;
  const auto width = Word::U32(bitmap.width / scale), height = Word::U32(bitmap.height / scale);
  if (str && texture.raw) {
    const auto bytes = jni->ToGuestLocal(ByteArray(java_, bitmap.pixels), error);
    if (bytes) ok = Call("jniTextureReadyRaw", "VIILLIIII", {Word::Ptr(str), Word::Ptr(bytes),
        Word::U32(bitmap.texture_width), Word::U32(bitmap.texture_height), width, height}, error);
  } else if (str) {
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, bitmap.texture_width, bitmap.texture_height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, bitmap.pixels.data());
    ok = Call("jniTextureReady", "VIILIII", {Word::Ptr(str), Word::U32(id), width, height}, error);
  }
  jni->PopFrame(0);
  return ok;
}

bool GumballBootstrap::Start(const std::string& apk, int width, int height, std::string* error) {
  if (!apk_) {
    apk_ = loader::ApkArchive::Open(apk, error);
    if (!apk_) return false;
    InstallJava();
    const auto local = java_.NewInstance(kSurface);
    // This engine caches the surface passed to jniSurfaceChanged and reuses it on later JNI calls.
    surface_ = boot_.jni()->ToGuestGlobal(local, error);
    java_.DeleteLocalRef(local);
    if (surface_ == 0) return false;
  }
  width_ = width;
  if (!Call("jniClearTextures", "VII", {}, error) ||
      !Call("jniSurfaceChanged", "VIIII", {Word::S32(width), Word::S32(height)}, error) ||
      !Call(started_ ? "jniReloadTextures" : "jniInit", started_ ? "VII" : "VIII",
            started_ ? abi::Args32{} : abi::Args32{Word::S32(0)}, error)) return false;
  started_ = true;
  Resume();
  boot_.Live("gumball: JNI surface and GLES1 initialized");
  return true;
}

void GumballBootstrap::Resume() { last_frame_ = {}; }

bool GumballBootstrap::Render(std::string* error) {
  const auto now = std::chrono::steady_clock::now();
  const float dt = last_frame_.time_since_epoch().count() == 0 ? 0.f :
      std::min(0.25f, std::chrono::duration<float>(now - last_frame_).count());
  last_frame_ = now;
  if (!textures_.empty()) {
    auto texture = std::move(textures_.front());
    textures_.pop_front();
    if (!DeliverTexture(texture, error)) return false;
  }
  return Call("jniRun", "VIIF", {Word::F32(dt)}, error) &&
         Call("jniAcceleration", "VIIFFF", {Word::F32(0), Word::F32(0), Word::F32(0)}, error);
}

void GumballBootstrap::Touch(int kind, int id, float x, float y) {
  if (kind == 0 && finger_ == -1) finger_ = id;
  if (id != finger_) return;
  if (kind >= 2) finger_ = -1;
  const int action = kind == 0 ? 1 : kind == 1 ? 3 : kind == 2 ? 2 : 4;
  std::string error;
  if (!Call("jniTouch", "VIIIII", {Word::S32(static_cast<int>(y)), Word::S32(width_ - static_cast<int>(x)),
                                     Word::S32(action)}, &error)) boot_.Live(error);
}

}  // namespace rb::platform::ios
