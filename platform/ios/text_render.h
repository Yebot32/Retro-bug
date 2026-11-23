// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// One line of UTF-8 text rasterised with CoreText, in the pixel format Cocos2d-x's Android text path
// produces: ARGB_8888 ints, little-endian in memory (B, G, R, A per pixel), white glyphs on transparent.
// The guest's nativeInitBitmapDC then applies its own swapAlphaChannel, exactly as on a device.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rb::platform::ios {

/// False when the platform renderer could not produce a context; then w/h are untouched.
bool RenderText(const std::string& utf8, float font_size, int max_w, int max_h, int& w, int& h,
                std::vector<std::uint8_t>& pixels);

}  // namespace rb::platform::ios
