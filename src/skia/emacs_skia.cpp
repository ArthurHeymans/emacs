/* Minimal Skia C API implementation for Emacs
   Copyright (C) 2024-2026 Free Software Foundation, Inc.

This file is part of GNU Emacs.

GNU Emacs is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or (at
your option) any later version.

GNU Emacs is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU Emacs.  If not, see <https://www.gnu.org/licenses/>. */

#include <config.h>
#include "emacs_skia.h"

/* Skia C++ headers - paths work with both:
   - Nix skia package: -I.../include/skia -> codec/SkCodec.h
   - Source build: -I.../skia -> include/codec/SkCodec.h  */
#include "codec/SkCodec.h"
#include "core/SkCanvas.h"
#include "core/SkColorSpace.h"
#include "core/SkData.h"
#include "core/SkFont.h"
#include "core/SkFontMetrics.h"
#include "core/SkFontMgr.h"
#include "core/SkImage.h"
#include "core/SkPaint.h"
#include "core/SkStream.h"
#include "core/SkSurface.h"
#include "core/SkTypeface.h"

/* Platform-specific font manager for Linux (fontconfig) */
#ifdef HAVE_FONTCONFIG
# include "ports/SkFontMgr_fontconfig.h"
# include "ports/SkFontScanner_FreeType.h"
#endif

#ifdef SK_GL
# include "gpu/GrBackendSurface.h"
# include "gpu/GrDirectContext.h"
# include "gpu/ganesh/SkSurfaceGanesh.h"
# include "gpu/ganesh/gl/GrGLBackendSurface.h"
# include "gpu/ganesh/gl/GrGLDirectContext.h"
# include "gpu/gl/GrGLAssembleInterface.h"
# include "gpu/gl/GrGLInterface.h"
#endif

/* ============================================================
   Internal type wrappers
   ============================================================ */

struct emacs_skia_surface
{
  sk_sp<SkSurface> surface;
  void *pixels; /* For raster surfaces with external pixels */
};

struct emacs_skia_canvas
{
  SkCanvas *canvas; /* Borrowed pointer, owned by surface */
};

struct emacs_skia_paint
{
  SkPaint paint;
};

struct emacs_skia_font
{
  SkFont font;
  sk_sp<SkTypeface> typeface;
};

struct emacs_skia_typeface
{
  sk_sp<SkTypeface> typeface;
};

struct emacs_skia_image
{
  sk_sp<SkImage> image;
};

#ifdef SK_GL
struct emacs_skia_gl_context
{
  sk_sp<GrDirectContext> context;
};
#else
struct emacs_skia_gl_context
{
  int dummy;
};
#endif

/* ============================================================
   Helper functions
   ============================================================ */

static inline SkRect
to_sk_rect (const emacs_skia_rect_t *r)
{
  return SkRect::MakeLTRB (r->left, r->top, r->right, r->bottom);
}

static inline SkIRect
to_sk_irect (const emacs_skia_irect_t *r)
{
  return SkIRect::MakeLTRB (r->left, r->top, r->right, r->bottom);
}

static inline SkPoint
to_sk_point (emacs_skia_point_t p)
{
  return SkPoint::Make (p.x, p.y);
}

static inline SkColor
to_sk_color (emacs_skia_color_t c)
{
  return static_cast<SkColor> (c);
}

static inline SkBlendMode
to_sk_blend_mode (emacs_skia_blend_mode_t mode)
{
  switch (mode)
    {
    case EMACS_SKIA_BLEND_SRC:
      return SkBlendMode::kSrc;
    case EMACS_SKIA_BLEND_SRC_OVER:
      return SkBlendMode::kSrcOver;
    case EMACS_SKIA_BLEND_DST_OVER:
      return SkBlendMode::kDstOver;
    case EMACS_SKIA_BLEND_CLEAR:
      return SkBlendMode::kClear;
    default:
      return SkBlendMode::kSrcOver;
    }
}

static inline SkFontHinting
to_sk_hinting (emacs_skia_hinting_t h)
{
  switch (h)
    {
    case EMACS_SKIA_HINTING_NONE:
      return SkFontHinting::kNone;
    case EMACS_SKIA_HINTING_SLIGHT:
      return SkFontHinting::kSlight;
    case EMACS_SKIA_HINTING_NORMAL:
      return SkFontHinting::kNormal;
    case EMACS_SKIA_HINTING_FULL:
      return SkFontHinting::kFull;
    default:
      return SkFontHinting::kNormal;
    }
}

static inline SkFont::Edging
to_sk_edging (emacs_skia_antialias_t aa)
{
  switch (aa)
    {
    case EMACS_SKIA_ANTIALIAS_NONE:
      return SkFont::Edging::kAlias;
    case EMACS_SKIA_ANTIALIAS_NORMAL:
      return SkFont::Edging::kAntiAlias;
    case EMACS_SKIA_ANTIALIAS_SUBPIXEL:
      return SkFont::Edging::kSubpixelAntiAlias;
    default:
      return SkFont::Edging::kAntiAlias;
    }
}

/* ============================================================
   Global font manager
   ============================================================ */

static sk_sp<SkFontMgr> global_font_mgr;

static sk_sp<SkFontMgr>
get_font_mgr (void)
{
  if (!global_font_mgr)
    {
#ifdef HAVE_FONTCONFIG
      /* Use fontconfig-based font manager on Linux */
      global_font_mgr
	= SkFontMgr_New_FontConfig (nullptr,
				    SkFontScanner_Make_FreeType ());
#else
      /* Fallback to empty font manager (typeface creation will fail)
       */
      global_font_mgr = SkFontMgr::RefEmpty ();
#endif
    }
  return global_font_mgr;
}

/* ============================================================
   Initialization / Cleanup
   ============================================================ */

void
emacs_skia_init (void)
{
  /* Initialize global font manager */
  (void) get_font_mgr ();
}

void
emacs_skia_cleanup (void)
{
  /* Release global font manager */
  global_font_mgr.reset ();
}

/* ============================================================
   GL Context
   ============================================================ */

#ifdef SK_GL
emacs_skia_gl_context_t *
emacs_skia_gl_context_create (emacs_skia_gl_get_proc_fn get_proc,
			      void *ctx)
{
  auto interface = GrGLMakeAssembledInterface (ctx, (GrGLGetProc)
						      get_proc);
  if (!interface)
    {
      return nullptr;
    }

  auto grContext = GrDirectContexts::MakeGL (interface);
  if (!grContext)
    {
      return nullptr;
    }

  auto result = new emacs_skia_gl_context_t;
  result->context = grContext;
  return result;
}

void
emacs_skia_gl_context_destroy (emacs_skia_gl_context_t *ctx)
{
  if (ctx)
    {
      ctx->context->abandonContext ();
      delete ctx;
    }
}

void
emacs_skia_gl_context_flush (emacs_skia_gl_context_t *ctx)
{
  if (ctx && ctx->context)
    {
      ctx->context->flushAndSubmit ();
    }
}
#else
emacs_skia_gl_context_t *
emacs_skia_gl_context_create (emacs_skia_gl_get_proc_fn get_proc,
			      void *ctx)
{
  (void) get_proc;
  (void) ctx;
  return nullptr;
}

void
emacs_skia_gl_context_destroy (emacs_skia_gl_context_t *ctx)
{
  (void) ctx;
}

void
emacs_skia_gl_context_flush (emacs_skia_gl_context_t *ctx)
{
  (void) ctx;
}
#endif

/* ============================================================
   Surface
   ============================================================ */

emacs_skia_surface_t *
emacs_skia_surface_create_raster (int width, int height)
{
  SkImageInfo info = SkImageInfo::MakeN32Premul (width, height);
  auto surface = SkSurfaces::Raster (info);
  if (!surface)
    {
      return nullptr;
    }

  auto result = new emacs_skia_surface_t;
  result->surface = surface;
  result->pixels = nullptr;
  return result;
}

#ifdef SK_GL
emacs_skia_surface_t *
emacs_skia_surface_create_gl (emacs_skia_gl_context_t *ctx, int width,
			      int height, unsigned int framebuffer_id,
			      unsigned int format)
{
  if (!ctx || !ctx->context)
    {
      return nullptr;
    }

  GrGLFramebufferInfo fbInfo;
  fbInfo.fFBOID = framebuffer_id;
  fbInfo.fFormat = format;

  auto backendRT
    = GrBackendRenderTargets::MakeGL (width, height, 0, 0, fbInfo);

  auto surface = SkSurfaces::
    WrapBackendRenderTarget (ctx->context.get (), backendRT,
			     kBottomLeft_GrSurfaceOrigin,
			     kRGBA_8888_SkColorType, nullptr,
			     nullptr);

  if (!surface)
    {
      return nullptr;
    }

  auto result = new emacs_skia_surface_t;
  result->surface = surface;
  result->pixels = nullptr;
  return result;
}
#else
emacs_skia_surface_t *
emacs_skia_surface_create_gl (emacs_skia_gl_context_t *ctx, int width,
			      int height, unsigned int framebuffer_id,
			      unsigned int format)
{
  (void) ctx;
  (void) width;
  (void) height;
  (void) framebuffer_id;
  (void) format;
  return nullptr;
}
#endif

emacs_skia_surface_t *
emacs_skia_surface_create_from_pixels (int width, int height,
				       void *pixels, size_t row_bytes)
{
  SkImageInfo info = SkImageInfo::MakeN32Premul (width, height);
  auto surface = SkSurfaces::WrapPixels (info, pixels, row_bytes);
  if (!surface)
    {
      return nullptr;
    }

  auto result = new emacs_skia_surface_t;
  result->surface = surface;
  result->pixels = pixels;
  return result;
}

void
emacs_skia_surface_destroy (emacs_skia_surface_t *surface)
{
  if (surface)
    {
      delete surface;
    }
}

emacs_skia_canvas_t *
emacs_skia_surface_get_canvas (emacs_skia_surface_t *surface)
{
  if (!surface || !surface->surface)
    {
      return nullptr;
    }

  static emacs_skia_canvas_t canvas_wrapper;
  canvas_wrapper.canvas = surface->surface->getCanvas ();
  return &canvas_wrapper;
}

void *
emacs_skia_surface_get_pixels (emacs_skia_surface_t *surface)
{
  if (!surface || !surface->surface)
    {
      return nullptr;
    }
  SkPixmap pixmap;
  if (surface->surface->peekPixels (&pixmap))
    {
      return const_cast<void *> (pixmap.addr ());
    }
  return surface->pixels;
}

void
emacs_skia_surface_flush (emacs_skia_surface_t *surface)
{
  if (surface && surface->surface)
    {
      /* For raster surfaces (which we currently use), pixels are
	 directly accessible and no flush is needed.  For GPU
	 surfaces, we would need to call
	 GrDirectContext::flushAndSubmit() instead.  */
#ifdef SK_GL
      if (surface->context)
	surface->context->flushAndSubmit ();
#endif
    }
}

int
emacs_skia_surface_get_width (emacs_skia_surface_t *surface)
{
  return surface ? surface->surface->width () : 0;
}

int
emacs_skia_surface_get_height (emacs_skia_surface_t *surface)
{
  return surface ? surface->surface->height () : 0;
}

emacs_skia_image_t *
emacs_skia_surface_make_image_snapshot (emacs_skia_surface_t *surface)
{
  if (!surface || !surface->surface)
    return nullptr;

  sk_sp<SkImage> image = surface->surface->makeImageSnapshot ();
  if (!image)
    return nullptr;

  auto *result = new emacs_skia_image_t;
  result->image = image;
  return result;
}

emacs_skia_image_t *
emacs_skia_surface_make_image_snapshot_rect (
  emacs_skia_surface_t *surface, const emacs_skia_irect_t *rect)
{
  if (!surface || !surface->surface || !rect)
    return nullptr;

  SkIRect sk_rect = SkIRect::MakeLTRB (rect->left, rect->top,
				       rect->right, rect->bottom);
  sk_sp<SkImage> image
    = surface->surface->makeImageSnapshot (sk_rect);
  if (!image)
    return nullptr;

  auto *result = new emacs_skia_image_t;
  result->image = image;
  return result;
}

/* ============================================================
   Canvas
   ============================================================ */

void
emacs_skia_canvas_save (emacs_skia_canvas_t *canvas)
{
  if (canvas && canvas->canvas)
    {
      canvas->canvas->save ();
    }
}

void
emacs_skia_canvas_restore (emacs_skia_canvas_t *canvas)
{
  if (canvas && canvas->canvas)
    {
      canvas->canvas->restore ();
    }
}

int
emacs_skia_canvas_get_save_count (emacs_skia_canvas_t *canvas)
{
  return canvas && canvas->canvas ? canvas->canvas->getSaveCount ()
				  : 0;
}

void
emacs_skia_canvas_restore_to_count (emacs_skia_canvas_t *canvas,
				    int count)
{
  if (canvas && canvas->canvas)
    {
      canvas->canvas->restoreToCount (count);
    }
}

void
emacs_skia_canvas_clear (emacs_skia_canvas_t *canvas,
			 emacs_skia_color_t color)
{
  if (canvas && canvas->canvas)
    {
      canvas->canvas->clear (to_sk_color (color));
    }
}

void
emacs_skia_canvas_clip_rect (emacs_skia_canvas_t *canvas,
			     const emacs_skia_rect_t *rect)
{
  if (canvas && canvas->canvas && rect)
    {
      canvas->canvas->clipRect (to_sk_rect (rect));
    }
}

void
emacs_skia_canvas_clip_irect (emacs_skia_canvas_t *canvas,
			      const emacs_skia_irect_t *rect)
{
  if (canvas && canvas->canvas && rect)
    {
      canvas->canvas->clipIRect (to_sk_irect (rect));
    }
}

void
emacs_skia_canvas_translate (emacs_skia_canvas_t *canvas, float dx,
			     float dy)
{
  if (canvas && canvas->canvas)
    {
      canvas->canvas->translate (dx, dy);
    }
}

void
emacs_skia_canvas_scale (emacs_skia_canvas_t *canvas, float sx,
			 float sy)
{
  if (canvas && canvas->canvas)
    {
      canvas->canvas->scale (sx, sy);
    }
}

void
emacs_skia_canvas_draw_rect (emacs_skia_canvas_t *canvas,
			     const emacs_skia_rect_t *rect,
			     emacs_skia_paint_t *paint)
{
  if (canvas && canvas->canvas && rect && paint)
    {
      canvas->canvas->drawRect (to_sk_rect (rect), paint->paint);
    }
}

void
emacs_skia_canvas_draw_irect (emacs_skia_canvas_t *canvas,
			      const emacs_skia_irect_t *rect,
			      emacs_skia_paint_t *paint)
{
  if (canvas && canvas->canvas && rect && paint)
    {
      canvas->canvas->drawIRect (to_sk_irect (rect), paint->paint);
    }
}

void
emacs_skia_canvas_draw_line (emacs_skia_canvas_t *canvas, float x0,
			     float y0, float x1, float y1,
			     emacs_skia_paint_t *paint)
{
  if (canvas && canvas->canvas && paint)
    {
      canvas->canvas->drawLine (x0, y0, x1, y1, paint->paint);
    }
}

void
emacs_skia_canvas_draw_glyphs (emacs_skia_canvas_t *canvas, int count,
			       const emacs_skia_glyph_t *glyphs,
			       const emacs_skia_point_t *positions,
			       emacs_skia_point_t origin,
			       emacs_skia_font_t *font,
			       emacs_skia_paint_t *paint)
{
  if (!canvas || !canvas->canvas || !glyphs || !positions || !font
      || !paint)
    {
      return;
    }

  /* Convert positions to SkPoint array */
  std::vector<SkPoint> sk_positions (count);
  for (int i = 0; i < count; i++)
    {
      sk_positions[i] = to_sk_point (positions[i]);
    }

  /* New Skia API uses SkSpan instead of raw pointers */
  SkSpan<const SkGlyphID> glyph_span (glyphs, count);
  SkSpan<const SkPoint> pos_span (sk_positions.data (), count);
  canvas->canvas->drawGlyphs (glyph_span, pos_span,
			      to_sk_point (origin), font->font,
			      paint->paint);
}

void
emacs_skia_canvas_draw_image (emacs_skia_canvas_t *canvas,
			      emacs_skia_image_t *image, float x,
			      float y, emacs_skia_paint_t *paint)
{
  if (!canvas || !canvas->canvas || !image || !image->image)
    {
      return;
    }

  SkSamplingOptions sampling (SkFilterMode::kLinear);
  canvas->canvas->drawImage (image->image.get (), x, y, sampling,
			     paint ? &paint->paint : nullptr);
}

void
emacs_skia_canvas_draw_image_rect (emacs_skia_canvas_t *canvas,
				   emacs_skia_image_t *image,
				   const emacs_skia_rect_t *src,
				   const emacs_skia_rect_t *dst,
				   emacs_skia_paint_t *paint)
{
  if (!canvas || !canvas->canvas || !image || !image->image || !dst)
    {
      return;
    }

  SkSamplingOptions sampling (SkFilterMode::kLinear);
  SkRect sk_src = src ? to_sk_rect (src)
		      : SkRect::MakeWH (image->image->width (),
					image->image->height ());

  canvas->canvas->drawImageRect (image->image.get (), sk_src,
				 to_sk_rect (dst), sampling,
				 paint ? &paint->paint : nullptr,
				 SkCanvas::kStrict_SrcRectConstraint);
}

/* ============================================================
   Paint
   ============================================================ */

emacs_skia_paint_t *
emacs_skia_paint_create (void)
{
  auto paint = new emacs_skia_paint_t;
  paint->paint.setAntiAlias (true);
  return paint;
}

void
emacs_skia_paint_destroy (emacs_skia_paint_t *paint)
{
  delete paint;
}

void
emacs_skia_paint_set_color (emacs_skia_paint_t *paint,
			    emacs_skia_color_t color)
{
  if (paint)
    {
      paint->paint.setColor (to_sk_color (color));
    }
}

emacs_skia_color_t
emacs_skia_paint_get_color (emacs_skia_paint_t *paint)
{
  return paint ? paint->paint.getColor () : 0;
}

void
emacs_skia_paint_set_alpha (emacs_skia_paint_t *paint, uint8_t alpha)
{
  if (paint)
    {
      paint->paint.setAlpha (alpha);
    }
}

void
emacs_skia_paint_set_antialias (emacs_skia_paint_t *paint,
				bool antialias)
{
  if (paint)
    {
      paint->paint.setAntiAlias (antialias);
    }
}

void
emacs_skia_paint_set_blend_mode (emacs_skia_paint_t *paint,
				 emacs_skia_blend_mode_t mode)
{
  if (paint)
    {
      paint->paint.setBlendMode (to_sk_blend_mode (mode));
    }
}

void
emacs_skia_paint_set_stroke (emacs_skia_paint_t *paint, bool stroke)
{
  if (paint)
    {
      paint->paint.setStyle (stroke ? SkPaint::kStroke_Style
				    : SkPaint::kFill_Style);
    }
}

void
emacs_skia_paint_set_stroke_width (emacs_skia_paint_t *paint,
				   float width)
{
  if (paint)
    {
      paint->paint.setStrokeWidth (width);
    }
}

/* ============================================================
   Font and Typeface
   ============================================================ */

emacs_skia_typeface_t *
emacs_skia_typeface_create_from_file (const char *path)
{
  sk_sp<SkFontMgr> fontMgr = get_font_mgr ();
  if (!fontMgr)
    {
      return nullptr;
    }

  auto typeface = fontMgr->makeFromFile (path);
  if (!typeface)
    {
      return nullptr;
    }

  auto result = new emacs_skia_typeface_t;
  result->typeface = typeface;
  return result;
}

emacs_skia_typeface_t *
emacs_skia_typeface_create_from_data (const void *data, size_t size)
{
  sk_sp<SkFontMgr> fontMgr = get_font_mgr ();
  if (!fontMgr)
    {
      return nullptr;
    }

  auto skdata = SkData::MakeWithCopy (data, size);
  auto typeface = fontMgr->makeFromData (skdata);
  if (!typeface)
    {
      return nullptr;
    }

  auto result = new emacs_skia_typeface_t;
  result->typeface = typeface;
  return result;
}

emacs_skia_typeface_t *
emacs_skia_typeface_create_from_name (const char *family_name,
				      int weight, int width,
				      int slant)
{
  sk_sp<SkFontMgr> fontMgr = get_font_mgr ();
  if (!fontMgr)
    {
      return nullptr;
    }

  SkFontStyle style (weight, width,
		     static_cast<SkFontStyle::Slant> (slant));
  auto typeface = fontMgr->legacyMakeTypeface (family_name, style);
  if (!typeface)
    {
      return nullptr;
    }

  auto result = new emacs_skia_typeface_t;
  result->typeface = typeface;
  return result;
}

void
emacs_skia_typeface_destroy (emacs_skia_typeface_t *typeface)
{
  delete typeface;
}

emacs_skia_font_t *
emacs_skia_font_create (emacs_skia_typeface_t *typeface, float size)
{
  auto font = new emacs_skia_font_t;
  if (typeface && typeface->typeface)
    {
      font->font = SkFont (typeface->typeface, size);
      font->typeface = typeface->typeface;
    }
  else
    {
      font->font = SkFont (nullptr, size);
    }
  font->font.setSubpixel (true);
  return font;
}

void
emacs_skia_font_destroy (emacs_skia_font_t *font)
{
  delete font;
}

void
emacs_skia_font_set_size (emacs_skia_font_t *font, float size)
{
  if (font)
    {
      font->font.setSize (size);
    }
}

float
emacs_skia_font_get_size (emacs_skia_font_t *font)
{
  return font ? font->font.getSize () : 0;
}

void
emacs_skia_font_set_hinting (emacs_skia_font_t *font,
			     emacs_skia_hinting_t hinting)
{
  if (font)
    {
      font->font.setHinting (to_sk_hinting (hinting));
    }
}

void
emacs_skia_font_set_edging (emacs_skia_font_t *font,
			    emacs_skia_antialias_t edging)
{
  if (font)
    {
      font->font.setEdging (to_sk_edging (edging));
    }
}

void
emacs_skia_font_set_subpixel (emacs_skia_font_t *font, bool subpixel)
{
  if (font)
    {
      font->font.setSubpixel (subpixel);
    }
}

void
emacs_skia_font_get_metrics (emacs_skia_font_t *font,
			     emacs_skia_font_metrics_t *metrics)
{
  if (!font || !metrics)
    {
      return;
    }

  SkFontMetrics sk_metrics;
  font->font.getMetrics (&sk_metrics);

  metrics->ascent = sk_metrics.fAscent;
  metrics->descent = sk_metrics.fDescent;
  metrics->leading = sk_metrics.fLeading;
  metrics->avg_char_width = sk_metrics.fAvgCharWidth;
  metrics->max_char_width = sk_metrics.fMaxCharWidth;
  metrics->x_height = sk_metrics.fXHeight;
  metrics->cap_height = sk_metrics.fCapHeight;
}

int
emacs_skia_font_text_to_glyphs (emacs_skia_font_t *font,
				const char *text, size_t byte_length,
				emacs_skia_glyph_t *glyphs,
				int max_glyphs)
{
  if (!font || !text)
    {
      return 0;
    }

  /* New Skia API uses SkSpan for output buffer */
  SkSpan<SkGlyphID> glyph_span (glyphs, max_glyphs);
  size_t count
    = font->font.textToGlyphs (text, byte_length,
			       SkTextEncoding::kUTF8, glyph_span);
  return static_cast<int> (count);
}

emacs_skia_glyph_t
emacs_skia_font_char_to_glyph (emacs_skia_font_t *font,
			       int32_t codepoint)
{
  if (!font)
    {
      return 0;
    }
  return font->font.unicharToGlyph (codepoint);
}

void
emacs_skia_font_get_widths (emacs_skia_font_t *font,
			    const emacs_skia_glyph_t *glyphs,
			    int count, float *widths)
{
  if (!font || !glyphs || !widths)
    {
      return;
    }
  /* New Skia API uses SkSpan instead of raw pointers */
  SkSpan<const SkGlyphID> glyph_span (glyphs, count);
  SkSpan<SkScalar> width_span (widths, count);
  font->font.getWidths (glyph_span, width_span);
}

float
emacs_skia_font_measure_text (emacs_skia_font_t *font,
			      const char *text, size_t byte_length)
{
  if (!font || !text)
    {
      return 0;
    }
  return font->font.measureText (text, byte_length,
				 SkTextEncoding::kUTF8);
}

/* ============================================================
   Image
   ============================================================ */

emacs_skia_image_t *
emacs_skia_image_create_from_pixels (int width, int height,
				     const void *pixels,
				     size_t row_bytes, bool has_alpha)
{
  SkColorType colorType
    = has_alpha ? kRGBA_8888_SkColorType : kRGB_888x_SkColorType;
  SkAlphaType alphaType
    = has_alpha ? kPremul_SkAlphaType : kOpaque_SkAlphaType;
  SkImageInfo info
    = SkImageInfo::Make (width, height, colorType, alphaType);

  auto image = SkImages::RasterFromPixmapCopy (
    SkPixmap (info, pixels, row_bytes));

  if (!image)
    {
      return nullptr;
    }

  auto result = new emacs_skia_image_t;
  result->image = image;
  return result;
}

emacs_skia_image_t *
emacs_skia_image_create_from_encoded (const void *data, size_t size)
{
  auto skdata = SkData::MakeWithCopy (data, size);
  auto image = SkImages::DeferredFromEncodedData (skdata);

  if (!image)
    {
      return nullptr;
    }

  auto result = new emacs_skia_image_t;
  result->image = image;
  return result;
}

void
emacs_skia_image_destroy (emacs_skia_image_t *image)
{
  delete image;
}

int
emacs_skia_image_get_width (emacs_skia_image_t *image)
{
  return image && image->image ? image->image->width () : 0;
}

int
emacs_skia_image_get_height (emacs_skia_image_t *image)
{
  return image && image->image ? image->image->height () : 0;
}
