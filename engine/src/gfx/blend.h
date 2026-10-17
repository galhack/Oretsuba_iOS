// Pixel operations with the arithmetic of the original's tvpgl.dll kernels
// (docs/re/20_graphics_core.md §3.5). All 32 bpp pixels are BGRA as u32
// 0xAARRGGBB with straight alpha.
#pragma once

#include <cstdint>

#include "gfx/image.h"

namespace ore {

enum BlendMode {
	kBlendCopy = 0,      // opa 255: copy incl. alpha; else constant alpha
	kBlendAlpha = 2,     // source alpha (x opacity)
	kBlendAlphaOver = 3, // straight alpha composition, including destination coverage
};

// blit: draws srcRect of src at (x, y) on dst, clipped to `clip` (and dst).
// hda = keep destination alpha.
void blit(Image &dst, const Image &src, int x, int y, Rect src_rect, int mode, int opacity, bool hda,
          const Rect *clip = nullptr);

// Stretch srcRect to a dw x dh rectangle at (x, y); negative sizes flip.
// filter 2/3 with opacity 255 and mode 0 -> bilinear, else nearest.
void stretch_blit(Image &dst, const Image &src, int x, int y, int dw, int dh, Rect src_rect, int mode, int filter,
                  int opacity, bool hda, const Rect *clip = nullptr);

// Affine blit: maps destination pixels back through the inverse of
// (src -> dst) = [a b; c d] + (tx, ty), nearest sampling of src_rect.
void affine_blit(Image &dst, const Image &src, Rect src_rect, double a, double b, double c, double d, double tx,
                 double ty, int mode, int opacity, bool hda, const Rect *clip = nullptr);

// Plain u32 store over rect (32 bpp) or index store (8 bpp).
void fill_rect(Image &dst, Rect r, uint32_t value);
// ColorRect drawing: mode 0 constant-colour alpha fill (opa 255 = plain
// fill), mode 2 additive, mode 3 subtractive (colour premultiplied by opa).
void color_fill(Image &dst, Rect r, uint32_t rgb, int opacity, int mode);

// Straight copy of a rectangle (alpha included), no blending.
void copy_rect(Image &dst, int x, int y, const Image &src, Rect src_rect);

// Constant-alpha blend of whole images of equal size: dst = a*s + (1-a)*d
// with a in 0..255 (TVP ConstAlphaBlend arithmetic), alpha := 0.
void blend_images(Image &dst, const Image &a, const Image &b, int opa_b);

}  // namespace ore
