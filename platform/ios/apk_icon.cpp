// RetroBug iOS adapter — the game icon for the launcher grid. SPDX-License-Identifier: Apache-2.0
//
// The Android launcher asks PackageManager for a game's icon; iOS has no package manager, so the same
// answer is read out of the APK with the engine's zip reader: the entry whose name says launcher image,
// preferring the largest (density buckets scale up), skipping adaptive-icon layers that are not a picture
// on their own and nine-patch sheets. The stored bytes go out unchanged; UIImage decodes PNG, JPEG and
// the WebP newer games ship.

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "rb/loader/zip.h"
#include "rb_ios.h"

namespace {

bool EndsWithNoCase(const std::string& s, const char* suffix) {
  const std::size_t n = std::char_traits<char>::length(suffix);
  if (s.size() < n) return false;
  for (std::size_t i = 0; i < n; ++i) {
    const char a = s[s.size() - n + i];
    if (std::tolower(static_cast<unsigned char>(a)) !=
        std::tolower(static_cast<unsigned char>(suffix[i]))) {
      return false;
    }
  }
  return true;
}

/// The file name, lowercased: the directory says nothing about what the entry is.
std::string LowerBase(const std::string& path) {
  std::string base = path.substr(path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/') + 1);
  for (char& c : base) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return base;
}

int IconScore(const std::string& base) {
  int score = 0;
  if (base.find("ic_launcher") != std::string::npos) {
    score = 3;
  } else if (base.find("launcher") != std::string::npos) {
    score = 2;
  } else if (base.find("icon") != std::string::npos || base.find("logo") != std::string::npos) {
    score = 1;
  }
  if (score == 0) return 0;
  // An adaptive icon's foreground and background are layers, not a picture; a splash is not the icon.
  if (base.find("foreground") != std::string::npos || base.find("background") != std::string::npos ||
      base.find("splash") != std::string::npos) {
    return 0;
  }
  return score;
}

}  // namespace

extern "C" unsigned char* rb_ios_apk_icon(const char* apk_path, unsigned int* len) {
  if (len != nullptr) *len = 0;
  if (apk_path == nullptr) return nullptr;
  std::string why;
  const std::unique_ptr<rb::loader::ZipReader> zip = rb::loader::ZipReader::OpenFile(apk_path, &why);
  if (zip == nullptr) return nullptr;

  std::string best;
  int best_score = 0;
  std::uint32_t best_size = 0;
  for (const std::string& name : zip->Names()) {
    const std::string base = LowerBase(name);
    if (!EndsWithNoCase(base, ".png") && !EndsWithNoCase(base, ".webp") && !EndsWithNoCase(base, ".jpg") &&
        !EndsWithNoCase(base, ".jpeg")) {
      continue;
    }
    if (EndsWithNoCase(base, ".9.png")) continue;
    const int score = IconScore(base);
    if (score == 0) continue;
    const std::optional<std::uint32_t> size = zip->UncompressedSize(name);
    if (!size || *size == 0) continue;
    if (score > best_score || (score == best_score && *size > best_size)) {
      best = name;
      best_score = score;
      best_size = *size;
    }
  }
  if (best.empty() || best_size > (12u << 20)) return nullptr;
  const std::optional<std::vector<std::uint8_t>> bytes = zip->Read(best, &why);
  if (!bytes || bytes->empty()) return nullptr;
  auto* out = static_cast<unsigned char*>(std::malloc(bytes->size()));
  if (out == nullptr) return nullptr;
  std::memcpy(out, bytes->data(), bytes->size());
  if (len != nullptr) *len = static_cast<unsigned int>(bytes->size());
  return out;
}

extern "C" void rb_ios_free_bytes(void* bytes) { std::free(bytes); }
