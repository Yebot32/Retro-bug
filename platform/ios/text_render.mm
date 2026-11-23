// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// See text_render.h. CoreText draws the line; the bitmap context is ARGB_8888 with the bytes laid out
// little-endian, which is what Android's Bitmap.copyPixelsToBuffer(nativeOrder) hands the guest.

#include "text_render.h"

#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <cmath>

namespace rb::platform::ios {

bool RenderText(const std::string& utf8, float font_size, int max_w, int max_h, int& w, int& h,
                std::vector<std::uint8_t>& pixels) {
  @autoreleasepool {
    NSString* string = [NSString stringWithUTF8String:utf8.c_str()];
    if (string == nil || string.length == 0) string = @" ";
    CTFontRef font = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, font_size > 1 ? font_size : 12, nullptr);
    if (font == nullptr) return false;
    CGColorRef white = CGColorCreateGenericRGB(1, 1, 1, 1);
    NSDictionary* attrs = @{
      (__bridge id)kCTFontAttributeName : (__bridge id)font,
      (__bridge id)kCTForegroundColorAttributeName : (__bridge id)white,
    };
    NSAttributedString* attributed = [[NSAttributedString alloc] initWithString:string attributes:attrs];
    CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)attributed);
    CGFloat ascent = 0, descent = 0, leading = 0;
    const double line_width = CTLineGetTypographicBounds(line, &ascent, &descent, &leading);
    int out_w = std::max(1, static_cast<int>(std::ceil(line_width)) + 2);
    int out_h = std::max(1, static_cast<int>(std::ceil(ascent + descent + leading)) + 2);
    if (max_w > 0) out_w = std::min(out_w, max_w);
    if (max_h > 0) out_h = std::min(out_h, max_h);
    pixels.assign(static_cast<std::size_t>(out_w) * static_cast<std::size_t>(out_h) * 4, 0);
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(pixels.data(), out_w, out_h, 8, out_w * 4, space,
                                             kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    const bool ok = ctx != nullptr;
    if (ctx != nullptr) {
      CGContextSetTextMatrix(ctx, CGAffineTransformIdentity);
      CGContextSetShouldAntialias(ctx, true);
      CGContextSetShouldSmoothFonts(ctx, false);
      CGContextSetTextPosition(ctx, 1, 1 + descent);
      CTLineDraw(line, ctx);
      CGContextRelease(ctx);
    }
    CGColorSpaceRelease(space);
    CFRelease(line);
    CGColorRelease(white);
    CFRelease(font);
    if (!ok) return false;
    w = out_w;
    h = out_h;
    return true;
  }
}

}  // namespace rb::platform::ios
