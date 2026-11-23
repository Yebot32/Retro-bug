// RetroBug iOS adapter — the game icon for the launcher grid. SPDX-License-Identifier: Apache-2.0
//
// Resolve AndroidManifest.xml's application icon through resources.arsc and return the raster variant
// to UIKit without altering its alpha channel. Older APKs without a resource table use a name fallback.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "apk_identity.h"
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
  const auto stem = base.substr(0, base.find_last_of('.'));
  if (stem == "ic_launcher" || stem == "app_icon" || stem == "appicon") return 3;
  if (stem == "icon" || stem == "launcher_icon") return 2;
  if (stem == "ic_launcher_round") return 1;
  return 0;
}

bool IsRaster(const std::string& path) {
  return !EndsWithNoCase(path, ".9.png") &&
      (EndsWithNoCase(path, ".png") || EndsWithNoCase(path, ".webp") ||
       EndsWithNoCase(path, ".jpg") || EndsWithNoCase(path, ".jpeg"));
}

std::optional<std::vector<std::uint8_t>> ReadBounded(rb::loader::ZipReader& zip,
                                                     const std::string& name, std::uint32_t limit) {
  const auto size = zip.UncompressedSize(name);
  if (!size || *size == 0 || *size > limit) return std::nullopt;
  std::string why;
  return zip.Read(name, &why);
}

std::optional<std::vector<std::uint8_t>> FindIcon(rb::loader::ZipReader& zip) {
  constexpr auto kIconLimit = 12u << 20;
  const auto manifest = ReadBounded(zip, "AndroidManifest.xml", 2u << 20);
  const auto identity = manifest ? rb::platform::ios::ReadApkIdentity(*manifest) : rb::platform::ios::ApkIdentity{};
  if (identity.icon || identity.round_icon) {
    if (const auto table = ReadBounded(zip, "resources.arsc", 32u << 20)) {
      bool resolved = false;
      for (const auto id : {identity.icon, identity.round_icon}) {
        if (id == 0) continue;
        auto variants = rb::platform::ios::ReadApkDrawables(*table, id);
        resolved |= !variants.empty();
        const auto density = [](unsigned dpi) { return dpi == 0 || dpi >= 0xfffe ? 160u : dpi; };
        std::stable_sort(variants.begin(), variants.end(), [&](const auto& a, const auto& b) {
          return density(a.density) > density(b.density);
        });
        for (const auto& variant : variants) {
          if (IsRaster(variant.path)) {
            if (auto bytes = ReadBounded(zip, variant.path, kIconLimit)) return bytes;
          }
        }
      }
      // XML-only adaptive or vector drawables cannot be decoded directly by UIKit.
      if (resolved) return std::nullopt;
    }
  }
  std::string best;
  int best_score = 0;
  std::uint32_t best_size = 0;
  for (const std::string& name : zip.Names()) {
    if (!(name.starts_with("res/drawable") || name.starts_with("res/mipmap")) || !IsRaster(name)) continue;
    const int score = IconScore(LowerBase(name));
    if (score == 0) continue;
    const auto size = zip.UncompressedSize(name);
    if (!size || *size == 0 || *size > kIconLimit) continue;
    if (score > best_score || (score == best_score && *size > best_size)) {
      best = name;
      best_score = score;
      best_size = *size;
    }
  }
  return best.empty() ? std::nullopt : ReadBounded(zip, best, kIconLimit);
}

}  // namespace

extern "C" unsigned char* rb_ios_apk_icon(const char* apk_path, unsigned int* len) {
  if (len != nullptr) *len = 0;
  if (apk_path == nullptr) return nullptr;
  std::string why;
  const std::unique_ptr<rb::loader::ZipReader> zip = rb::loader::ZipReader::OpenFile(apk_path, &why);
  if (zip == nullptr) return nullptr;

  const auto bytes = FindIcon(*zip);
  if (!bytes || bytes->empty()) return nullptr;
  auto* out = static_cast<unsigned char*>(std::malloc(bytes->size()));
  if (out == nullptr) return nullptr;
  std::memcpy(out, bytes->data(), bytes->size());
  if (len != nullptr) *len = static_cast<unsigned int>(bytes->size());
  return out;
}

extern "C" void rb_ios_free_bytes(void* bytes) { std::free(bytes); }
