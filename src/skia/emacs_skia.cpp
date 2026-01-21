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
#include "core/SkPath.h"
#include "core/SkPathBuilder.h"
#include "core/SkPathEffect.h"
#include "core/SkShader.h"
#include "core/SkStream.h"
#include "core/SkSurface.h"
#include "core/SkTileMode.h"
#include "core/SkTypeface.h"
#include "effects/SkDashPathEffect.h"
#include "encode/SkPngEncoder.h"

/* Platform-specific font manager for Linux (fontconfig) */
#ifdef HAVE_FONTCONFIG
# include <fontconfig/fontconfig.h>
# include "ports/SkFontMgr_fontconfig.h"
# include "ports/SkFontScanner_FreeType.h"
#endif

#include <cmath>
#include <cstring>
#include <map>
#include <memory>

/* PDF and SVG support */
#ifdef SK_PDF
# include "docs/SkPDFDocument.h"
#endif

#ifdef SK_SVG
# include "svg/SkSVGCanvas.h"
#endif

#ifdef SK_GL
# include "gpu/ganesh/GrBackendSurface.h"
# include "gpu/ganesh/GrDirectContext.h"
# include "gpu/ganesh/SkSurfaceGanesh.h"
# include "gpu/ganesh/gl/GrGLAssembleInterface.h"
# include "gpu/ganesh/gl/GrGLBackendSurface.h"
# include "gpu/ganesh/gl/GrGLDirectContext.h"
# include "gpu/ganesh/gl/GrGLInterface.h"
#endif

/* ============================================================
   Internal type wrappers
   ============================================================ */

struct emacs_skia_surface
{
  sk_sp<SkSurface> surface;
  void *pixels; /* For raster surfaces with external pixels */
#ifdef SK_GL
  GrDirectContext *context; /* For GPU surfaces, to flush */
#endif
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

struct emacs_skia_path
{
  SkPathBuilder builder;
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
    case EMACS_SKIA_BLEND_XOR:
      return SkBlendMode::kXor;
    case EMACS_SKIA_BLEND_DIFFERENCE:
      return SkBlendMode::kDifference;
    case EMACS_SKIA_BLEND_EXCLUSION:
      return SkBlendMode::kExclusion;
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

/* Create GL context using the native GL interface.  This uses the
   currently active GL context (e.g., set by GDK).  */
/* Callback for GrGLMakeAssembledInterface to get GL function
   pointers. We use dlsym on libGL.so which works for both GLX and EGL
   contexts when the context is already current.  */
#ifdef SK_GL
# include <dlfcn.h>

static void *gl_lib_handle = nullptr;

static GrGLFuncPtr
get_gl_proc (void *ctx, const char *name)
{
  (void) ctx;

  /* Lazy-load libGL.so */
  if (!gl_lib_handle)
    {
      gl_lib_handle = dlopen ("libGL.so.1", RTLD_LAZY | RTLD_GLOBAL);
      if (!gl_lib_handle)
	gl_lib_handle = dlopen ("libGL.so", RTLD_LAZY | RTLD_GLOBAL);
      if (!gl_lib_handle)
	{
	  /* Try EGL/GLES for Wayland */
	  gl_lib_handle
	    = dlopen ("libGLESv2.so.2", RTLD_LAZY | RTLD_GLOBAL);
	  if (!gl_lib_handle)
	    gl_lib_handle
	      = dlopen ("libGLESv2.so", RTLD_LAZY | RTLD_GLOBAL);
	}
    }

  if (!gl_lib_handle)
    return nullptr;

  return (GrGLFuncPtr) dlsym (gl_lib_handle, name);
}
#endif

emacs_skia_gl_context_t *
emacs_skia_gl_context_create_native (void)
{
#ifdef SK_GL
  /* Use GrGLMakeAssembledInterface with our dlsym-based function
   * loader.  */
  auto interface = GrGLMakeAssembledInterface (nullptr, get_gl_proc);
  if (!interface)
    return nullptr;

  auto grContext = GrDirectContexts::MakeGL (interface);
  if (!grContext)
    return nullptr;

  auto result = new emacs_skia_gl_context_t;
  result->context = grContext;
  return result;
#else
  return nullptr;
#endif
}

#ifdef SK_GL
emacs_skia_gl_context_t *
emacs_skia_gl_context_create (emacs_skia_gl_get_proc_fn get_proc,
			      void *ctx)
{
  auto interface = GrGLMakeAssembledInterface (ctx, (GrGLGetProc)
						      get_proc);
  if (!interface)
    return nullptr;

  auto grContext = GrDirectContexts::MakeGL (interface);
  if (!grContext)
    return nullptr;

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
    ctx->context->flushAndSubmit ();
}

void
emacs_skia_gl_context_reset (emacs_skia_gl_context_t *ctx)
{
  if (ctx && ctx->context)
    {
      /* Reset Skia's internal GL state tracking.  This is needed
	 after destroying a surface that was wrapped around a backend
	 render target, as Skia may have cached state related to that
	 target. resetContext() tells Skia to re-query GL state on
	 next use.  */
      ctx->context->resetContext ();
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

void
emacs_skia_gl_context_reset (emacs_skia_gl_context_t *ctx)
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
#ifdef SK_GL
  result->context = nullptr;
#endif
  return result;
}

#ifdef SK_GL
emacs_skia_surface_t *
emacs_skia_surface_create_gl (emacs_skia_gl_context_t *ctx, int width,
			      int height, unsigned int framebuffer_id,
			      unsigned int format)
{
  if (!ctx || !ctx->context)
    return nullptr;

  GrGLFramebufferInfo fbInfo;
  fbInfo.fFBOID = framebuffer_id;
  fbInfo.fFormat = format;

  /* Create backend render target with 8-bit stencil buffer.  Skia needs
     stencil for clip mask operations.  */
  auto backendRT
    = GrBackendRenderTargets::MakeGL (width, height, 0, 8, fbInfo);

  auto surface = SkSurfaces::
    WrapBackendRenderTarget (ctx->context.get (), backendRT,
			     kBottomLeft_GrSurfaceOrigin,
			     kRGBA_8888_SkColorType, nullptr,
			     nullptr);

  if (!surface)
    return nullptr;

  auto result = new emacs_skia_surface_t;
  result->surface = surface;
  result->pixels = nullptr;
  result->context = ctx->context.get ();
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
#ifdef SK_GL
  result->context = nullptr;
#endif
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
emacs_skia_canvas_save_layer (emacs_skia_canvas_t *canvas,
			      const emacs_skia_rect_t *bounds)
{
  if (canvas && canvas->canvas)
    {
      if (bounds)
	canvas->canvas->saveLayer (to_sk_rect (bounds), nullptr);
      else
	canvas->canvas->saveLayer (nullptr, nullptr);
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
emacs_skia_canvas_get_clip_bounds (emacs_skia_canvas_t *canvas,
				   emacs_skia_rect_t *bounds)
{
  if (canvas && canvas->canvas && bounds)
    {
      SkRect sk_bounds = canvas->canvas->getLocalClipBounds ();
      bounds->left = sk_bounds.left ();
      bounds->top = sk_bounds.top ();
      bounds->right = sk_bounds.right ();
      bounds->bottom = sk_bounds.bottom ();
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

void
emacs_skia_canvas_draw_path (emacs_skia_canvas_t *canvas,
			     emacs_skia_path_t *path,
			     emacs_skia_paint_t *paint)
{
  if (canvas && canvas->canvas && path && paint)
    {
      /* Convert builder to path for drawing.  */
      canvas->canvas->drawPath (path->builder.snapshot (),
				paint->paint);
    }
}

void
emacs_skia_canvas_draw_arc (emacs_skia_canvas_t *canvas,
			    const emacs_skia_rect_t *oval,
			    float start_angle, float sweep_angle,
			    bool use_center,
			    emacs_skia_paint_t *paint)
{
  if (!canvas || !canvas->canvas || !oval || !paint)
    return;

  canvas->canvas->drawArc (to_sk_rect (oval), start_angle,
			   sweep_angle, use_center, paint->paint);
}

void
emacs_skia_canvas_draw_image_with_mask (emacs_skia_canvas_t *canvas,
					emacs_skia_image_t *image,
					emacs_skia_image_t *mask,
					float x, float y,
					emacs_skia_paint_t *paint)
{
  if (!canvas || !canvas->canvas || !image || !image->image)
    return;

  /* If no mask, just draw the image normally */
  if (!mask || !mask->image)
    {
      emacs_skia_canvas_draw_image (canvas, image, x, y, paint);
      return;
    }

  /* Use a layer with mask as alpha:
     1. Save the canvas state and create a layer
     2. Draw the mask as the alpha channel
     3. Draw the image with SrcIn blend mode (uses mask alpha)
     4. Restore the layer */
  canvas->canvas->save ();

  SkRect bounds = SkRect::MakeXYWH (x, y, image->image->width (),
				    image->image->height ());
  canvas->canvas->saveLayer (bounds, nullptr);

  /* Draw mask as grayscale (will become alpha) */
  SkSamplingOptions sampling (SkFilterMode::kNearest);
  canvas->canvas->drawImage (mask->image.get (), x, y, sampling,
			     nullptr);

  /* Draw image with SrcIn to use mask as alpha */
  SkPaint srcInPaint;
  srcInPaint.setBlendMode (SkBlendMode::kSrcIn);
  if (paint)
    srcInPaint.setColor (paint->paint.getColor ());
  canvas->canvas->drawImage (image->image.get (), x, y, sampling,
			     &srcInPaint);

  canvas->canvas->restore (); /* Restore layer */
  canvas->canvas->restore (); /* Restore original state */
}

void
emacs_skia_canvas_clip_path (emacs_skia_canvas_t *canvas,
			     emacs_skia_path_t *path)
{
  if (canvas && canvas->canvas && path)
    {
      /* Convert builder to path for clipping.  */
      canvas->canvas->clipPath (path->builder.snapshot ());
    }
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

void
emacs_skia_paint_set_dash (emacs_skia_paint_t *paint,
			   const float *intervals, int count,
			   float phase)
{
  if (!paint || !intervals || count < 2)
    return;

  /* Skia requires SkScalar array, which is float.
     Newer Skia versions use SkSpan instead of pointer+count.  */
  auto effect
    = SkDashPathEffect::Make (SkSpan<const float> (intervals, count),
			      phase);
  paint->paint.setPathEffect (effect);
}

void
emacs_skia_paint_clear_dash (emacs_skia_paint_t *paint)
{
  if (paint)
    {
      paint->paint.setPathEffect (nullptr);
    }
}

void
emacs_skia_paint_set_image_shader (emacs_skia_paint_t *paint,
				   emacs_skia_image_t *image)
{
  if (!paint || !image || !image->image)
    return;

  /* Create a tiled shader from the image */
  SkSamplingOptions sampling (SkFilterMode::kNearest);
  auto shader
    = image->image->makeShader (SkTileMode::kRepeat,
				SkTileMode::kRepeat, sampling);
  paint->paint.setShader (shader);
}

void
emacs_skia_paint_clear_shader (emacs_skia_paint_t *paint)
{
  if (paint)
    {
      paint->paint.setShader (nullptr);
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

#ifdef HAVE_FONTCONFIG
emacs_skia_typeface_t *
emacs_skia_typeface_create_from_fc_pattern (FcPattern *pattern)
{
  if (!pattern)
    return nullptr;

  /* Extract the font file path and index from the pattern */
  FcChar8 *file = nullptr;
  int index = 0;

  if (FcPatternGetString (pattern, FC_FILE, 0, &file) != FcResultMatch
      || !file)
    return nullptr;

  FcPatternGetInteger (pattern, FC_INDEX, 0, &index);

  sk_sp<SkFontMgr> fontMgr = get_font_mgr ();
  if (!fontMgr)
    return nullptr;

  /* Load the typeface from file with the specified face index */
  auto typeface
    = fontMgr->makeFromFile (reinterpret_cast<const char *> (file),
			     index);
  if (!typeface)
    return nullptr;

  auto result = new emacs_skia_typeface_t;
  result->typeface = typeface;
  return result;
}
#endif

const char *
emacs_skia_typeface_get_path (emacs_skia_typeface_t *typeface)
{
  /* Skia doesn't expose the font file path directly.
     This would need platform-specific code or tracking during
     creation. For now, return nullptr - callers should use the path
     they originally provided.  */
  (void) typeface;
  return nullptr;
}

int
emacs_skia_typeface_get_index (emacs_skia_typeface_t *typeface)
{
  /* Skia doesn't expose the face index directly.
     Return 0 as the default.  */
  (void) typeface;
  return 0;
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

void
emacs_skia_font_get_extents (emacs_skia_font_t *font,
			     emacs_skia_font_extents_t *extents)
{
  if (!font || !extents)
    return;

  SkFontMetrics sk_metrics;
  font->font.getMetrics (&sk_metrics);

  /* Cairo convention: ascent and descent are positive values.
     Skia convention: ascent is negative (distance up from baseline).
   */
  extents->ascent = -sk_metrics.fAscent;
  extents->descent = sk_metrics.fDescent;
  extents->height
    = -sk_metrics.fAscent + sk_metrics.fDescent + sk_metrics.fLeading;
  extents->max_x_advance = sk_metrics.fMaxCharWidth;
  extents->max_y_advance = 0; /* Horizontal fonts */
}

void
emacs_skia_font_get_glyph_extents (
  emacs_skia_font_t *font, const emacs_skia_glyph_t *glyphs,
  int count, emacs_skia_glyph_extents_t *extents)
{
  if (!font || !glyphs || !extents || count <= 0)
    return;

  /* Get bounds and widths for all glyphs */
  std::vector<SkRect> bounds (count);
  std::vector<SkScalar> widths (count);

  SkSpan<const SkGlyphID> glyph_span (glyphs, count);
  SkSpan<SkScalar> width_span (widths.data (), count);
  SkSpan<SkRect> bounds_span (bounds.data (), count);

  font->font.getWidthsBounds (glyph_span, width_span, bounds_span,
			      nullptr);

  /* Convert to emacs_skia_glyph_extents_t format
     (compatible with Cairo's cairo_text_extents_t) */
  for (int i = 0; i < count; i++)
    {
      extents[i].x_bearing = bounds[i].left ();
      extents[i].y_bearing = bounds[i].top ();
      extents[i].width = bounds[i].width ();
      extents[i].height = bounds[i].height ();
      extents[i].x_advance = widths[i];
      extents[i].y_advance = 0; /* Horizontal fonts */
    }
}

void
emacs_skia_font_get_glyph_bounds (emacs_skia_font_t *font,
				  const emacs_skia_glyph_t *glyphs,
				  int count,
				  emacs_skia_rect_t *bounds)
{
  if (!font || !glyphs || !bounds || count <= 0)
    return;

  std::vector<SkRect> sk_bounds (count);
  SkSpan<const SkGlyphID> glyph_span (glyphs, count);
  SkSpan<SkRect> bounds_span (sk_bounds.data (), count);

  font->font.getBounds (glyph_span, bounds_span, nullptr);

  for (int i = 0; i < count; i++)
    {
      bounds[i].left = sk_bounds[i].left ();
      bounds[i].top = sk_bounds[i].top ();
      bounds[i].right = sk_bounds[i].right ();
      bounds[i].bottom = sk_bounds[i].bottom ();
    }
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
emacs_skia_image_create_from_bgra_pixels (int width, int height,
					  const void *pixels,
					  size_t row_bytes,
					  bool has_alpha)
{
  /* BGRA is Cairo's native format on little-endian machines.
     Skia supports BGRA natively with kBGRA_8888_SkColorType. */
  SkColorType colorType = kBGRA_8888_SkColorType;
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

emacs_skia_image_t *
emacs_skia_image_create_from_bitmap (const unsigned char *data,
				     int width, int height,
				     int stride)
{
  if (!data || width <= 0 || height <= 0)
    return nullptr;

  /* Calculate stride if not provided */
  if (stride <= 0)
    stride = (width + 7) / 8;

  /* Convert 1-bit packed data to 8-bit alpha.
     Cairo A1 format: MSB first, rows padded to 32-bit boundary.
     We convert to Skia A8 format for compatibility.  */
  int a8_stride = width;
  std::vector<uint8_t> a8_data (width * height);

  for (int y = 0; y < height; y++)
    {
      const unsigned char *src_row = data + y * stride;
      uint8_t *dst_row = a8_data.data () + y * a8_stride;

      for (int x = 0; x < width; x++)
	{
	  int byte_idx = x / 8;
	  int bit_idx = 7 - (x % 8); /* MSB first */
	  bool bit_set = (src_row[byte_idx] >> bit_idx) & 1;
	  dst_row[x] = bit_set ? 255 : 0;
	}
    }

  /* Create A8 (alpha-only) image */
  SkImageInfo info
    = SkImageInfo::Make (width, height, kAlpha_8_SkColorType,
			 kPremul_SkAlphaType);
  auto image = SkImages::RasterFromPixmapCopy (
    SkPixmap (info, a8_data.data (), a8_stride));

  if (!image)
    return nullptr;

  auto result = new emacs_skia_image_t;
  result->image = image;
  return result;
}

/* ============================================================
   Path
   ============================================================ */

emacs_skia_path_t *
emacs_skia_path_create (void)
{
  return new emacs_skia_path_t;
}

void
emacs_skia_path_destroy (emacs_skia_path_t *path)
{
  delete path;
}

void
emacs_skia_path_reset (emacs_skia_path_t *path)
{
  if (path)
    path->builder.reset ();
}

void
emacs_skia_path_move_to (emacs_skia_path_t *path, float x, float y)
{
  if (path)
    path->builder.moveTo (x, y);
}

void
emacs_skia_path_line_to (emacs_skia_path_t *path, float x, float y)
{
  if (path)
    path->builder.lineTo (x, y);
}

void
emacs_skia_path_rel_line_to (emacs_skia_path_t *path, float dx,
			     float dy)
{
  if (path)
    path->builder.rLineTo (dx, dy);
}

void
emacs_skia_path_arc_to (emacs_skia_path_t *path,
			const emacs_skia_rect_t *oval,
			float start_angle, float sweep_angle,
			bool force_move_to)
{
  if (!path || !oval)
    return;

  /* SkPathBuilder::arcTo uses forceMoveTo parameter.  */
  path->builder.arcTo (to_sk_rect (oval), start_angle, sweep_angle,
		       force_move_to);
}

void
emacs_skia_path_close (emacs_skia_path_t *path)
{
  if (path)
    path->builder.close ();
}

void
emacs_skia_path_add_rect (emacs_skia_path_t *path,
			  const emacs_skia_rect_t *rect)
{
  if (path && rect)
    path->builder.addRect (to_sk_rect (rect));
}

/* ============================================================
   Document Export (PDF/SVG)
   ============================================================ */

/* Custom SkWStream that writes to a callback function.  */
class CallbackWStream : public SkWStream
{
public:
  CallbackWStream (emacs_skia_write_fn fn, void *ctx)
      : write_fn_ (fn), ctx_ (ctx), bytes_written_ (0)
  {
  }

  bool write (const void *buffer, size_t size) override
  {
    if (!write_fn_)
      return false;
    size_t written = write_fn_ (ctx_, buffer, size);
    bytes_written_ += written;
    return written == size;
  }

  void flush () override
  {
    /* Callback stream doesn't buffer, so nothing to flush.  */
  }

  size_t bytesWritten () const override { return bytes_written_; }

private:
  emacs_skia_write_fn write_fn_;
  void *ctx_;
  size_t bytes_written_;
};

struct emacs_skia_document
{
#ifdef SK_PDF
  sk_sp<SkDocument> document;
  std::unique_ptr<CallbackWStream> stream;
  SkCanvas *current_page; /* Borrowed from document */
#else
  int dummy;
#endif
};

/* Canvas wrapper for SVG that owns its stream.  */
struct emacs_skia_svg_canvas_data
{
  std::unique_ptr<CallbackWStream> stream;
  std::unique_ptr<SkCanvas> canvas;
};

#ifdef SK_PDF
emacs_skia_document_t *
emacs_skia_document_create_pdf (emacs_skia_write_fn write_fn,
				void *write_ctx, float width,
				float height)
{
  auto stream
    = std::make_unique<CallbackWStream> (write_fn, write_ctx);

  SkPDF::Metadata metadata;
  /* Could add metadata like title, creator, etc. here.  */

  auto document = SkPDF::MakeDocument (stream.get (), metadata);
  if (!document)
    return nullptr;

  auto result = new emacs_skia_document_t;
  result->document = document;
  result->stream = std::move (stream);
  result->current_page = nullptr;

  return result;
}

emacs_skia_canvas_t *
emacs_skia_document_begin_page (emacs_skia_document_t *doc,
				float width, float height)
{
  if (!doc || !doc->document)
    return nullptr;

  /* End any existing page first.  */
  if (doc->current_page)
    {
      doc->document->endPage ();
      doc->current_page = nullptr;
    }

  doc->current_page
    = doc->document->beginPage (width, height, nullptr);
  if (!doc->current_page)
    return nullptr;

  /* Return a wrapper - we use a static because the canvas is
     owned by the document.  */
  static emacs_skia_canvas_t canvas_wrapper;
  canvas_wrapper.canvas = doc->current_page;
  return &canvas_wrapper;
}

void
emacs_skia_document_end_page (emacs_skia_document_t *doc)
{
  if (doc && doc->document && doc->current_page)
    {
      doc->document->endPage ();
      doc->current_page = nullptr;
    }
}

void
emacs_skia_document_close (emacs_skia_document_t *doc)
{
  if (doc)
    {
      if (doc->document)
	{
	  /* End any open page.  */
	  if (doc->current_page)
	    doc->document->endPage ();
	  doc->document->close ();
	}
      delete doc;
    }
}
#else
/* Stub implementations when PDF support is not compiled in.  */
emacs_skia_document_t *
emacs_skia_document_create_pdf (emacs_skia_write_fn write_fn,
				void *write_ctx, float width,
				float height)
{
  (void) write_fn;
  (void) write_ctx;
  (void) width;
  (void) height;
  return nullptr;
}

emacs_skia_canvas_t *
emacs_skia_document_begin_page (emacs_skia_document_t *doc,
				float width, float height)
{
  (void) doc;
  (void) width;
  (void) height;
  return nullptr;
}

void
emacs_skia_document_end_page (emacs_skia_document_t *doc)
{
  (void) doc;
}

void
emacs_skia_document_close (emacs_skia_document_t *doc)
{
  (void) doc;
}
#endif

#ifdef SK_SVG
/* Global map to track SVG canvas data.
   This is needed because we return a generic emacs_skia_canvas_t
   pointer but need to track the underlying stream ownership.  */
static std::map<SkCanvas *, emacs_skia_svg_canvas_data *>
  svg_canvas_map;

emacs_skia_canvas_t *
emacs_skia_svg_canvas_create (emacs_skia_write_fn write_fn,
			      void *write_ctx, float width,
			      float height)
{
  auto stream
    = std::make_unique<CallbackWStream> (write_fn, write_ctx);

  SkRect bounds = SkRect::MakeWH (width, height);
  auto canvas = SkSVGCanvas::Make (bounds, stream.get ());
  if (!canvas)
    return nullptr;

  /* Store the canvas data so we can clean up later.  */
  auto data = new emacs_skia_svg_canvas_data;
  data->stream = std::move (stream);
  data->canvas = std::move (canvas);

  svg_canvas_map[data->canvas.get ()] = data;

  static emacs_skia_canvas_t canvas_wrapper;
  canvas_wrapper.canvas = data->canvas.get ();
  return &canvas_wrapper;
}

void
emacs_skia_svg_canvas_finish (emacs_skia_canvas_t *canvas)
{
  if (!canvas || !canvas->canvas)
    return;

  auto it = svg_canvas_map.find (canvas->canvas);
  if (it != svg_canvas_map.end ())
    {
      emacs_skia_svg_canvas_data *data = it->second;
      /* Deleting the canvas flushes and finishes the SVG output.  */
      svg_canvas_map.erase (it);
      delete data;
    }
}
#else
/* Stub implementations when SVG support is not compiled in.  */
emacs_skia_canvas_t *
emacs_skia_svg_canvas_create (emacs_skia_write_fn write_fn,
			      void *write_ctx, float width,
			      float height)
{
  (void) write_fn;
  (void) write_ctx;
  (void) width;
  (void) height;
  return nullptr;
}

void
emacs_skia_svg_canvas_finish (emacs_skia_canvas_t *canvas)
{
  (void) canvas;
}
#endif

/* ============================================================
   PNG Export
   ============================================================ */

/* Helper to encode pixmap to PNG and write via callback.
   Uses SkDynamicMemoryWStream to avoid RTTI issues with custom streams.  */
static bool
encode_png_to_callback (const SkPixmap &pixmap,
			emacs_skia_write_fn write_fn, void *write_ctx)
{
  SkDynamicMemoryWStream stream;
  SkPngEncoder::Options options;

  if (!SkPngEncoder::Encode (&stream, pixmap, options))
    return false;

  /* Write the encoded data to the callback.  */
  sk_sp<SkData> data = stream.detachAsData ();
  if (!data)
    return false;

  size_t written = write_fn (write_ctx, data->data (), data->size ());
  return written == data->size ();
}

/* Write surface to PNG using a callback function.
   Returns true on success, false on failure.  */
bool
emacs_skia_surface_write_to_png (emacs_skia_surface_t *surface,
				 emacs_skia_write_fn write_fn,
				 void *write_ctx)
{
  if (!surface || !surface->surface || !write_fn)
    return false;

  /* Create an image snapshot of the surface.  */
  sk_sp<SkImage> image = surface->surface->makeImageSnapshot ();
  if (!image)
    return false;

  /* Read pixels into a raster format suitable for encoding.  */
  SkPixmap pixmap;
  if (!image->peekPixels (&pixmap))
    {
      /* For GPU surfaces, we need to read back the pixels.  */
      SkImageInfo info
	= SkImageInfo::Make (image->width (), image->height (),
			     kRGBA_8888_SkColorType,
			     kUnpremul_SkAlphaType);
      std::vector<uint8_t> pixels (info.computeMinByteSize ());
      if (!image->readPixels (info, pixels.data (), info.minRowBytes (),
			      0, 0))
	return false;

      SkPixmap temp_pixmap (info, pixels.data (), info.minRowBytes ());
      return encode_png_to_callback (temp_pixmap, write_fn, write_ctx);
    }

  return encode_png_to_callback (pixmap, write_fn, write_ctx);
}

/* Write image to PNG using a callback function.
   Returns true on success, false on failure.  */
bool
emacs_skia_image_write_to_png (emacs_skia_image_t *image,
			       emacs_skia_write_fn write_fn,
			       void *write_ctx)
{
  if (!image || !image->image || !write_fn)
    return false;

  SkPixmap pixmap;
  if (!image->image->peekPixels (&pixmap))
    {
      /* Need to read pixels back.  */
      SkImageInfo info
	= SkImageInfo::Make (image->image->width (),
			     image->image->height (),
			     kRGBA_8888_SkColorType,
			     kUnpremul_SkAlphaType);
      std::vector<uint8_t> pixels (info.computeMinByteSize ());
      if (!image->image->readPixels (info, pixels.data (),
				     info.minRowBytes (), 0, 0))
	return false;

      SkPixmap temp_pixmap (info, pixels.data (), info.minRowBytes ());
      return encode_png_to_callback (temp_pixmap, write_fn, write_ctx);
    }

  return encode_png_to_callback (pixmap, write_fn, write_ctx);
}

/* ============================================================
   Image Transformation
   ============================================================ */

/* Image transformation data for Skia.
   This is used to store transformation matrices for images,
   replacing the use of cairo_pattern_t for this purpose.  */
struct emacs_skia_image_transform
{
  /* 3x3 transformation matrix in row-major order.
     [a c e]   [0 2 4]
     [b d f] = [1 3 5]
     [0 0 1]   (implicit) */
  float matrix[6];
  /* Filter mode: true = bilinear, false = nearest neighbor.  */
  bool smoothing;
};

emacs_skia_image_transform_t *
emacs_skia_image_transform_create (void)
{
  auto result = new emacs_skia_image_transform_t;
  /* Initialize to identity matrix.  */
  result->matrix[0] = 1.0f; /* a = scale x */
  result->matrix[1] = 0.0f; /* b = skew y */
  result->matrix[2] = 0.0f; /* c = skew x */
  result->matrix[3] = 1.0f; /* d = scale y */
  result->matrix[4] = 0.0f; /* e = translate x */
  result->matrix[5] = 0.0f; /* f = translate y */
  result->smoothing = true;
  return result;
}

void
emacs_skia_image_transform_destroy (emacs_skia_image_transform_t *transform)
{
  delete transform;
}

void
emacs_skia_image_transform_set_matrix (emacs_skia_image_transform_t *transform,
				       const float matrix[6])
{
  if (transform && matrix)
    {
      for (int i = 0; i < 6; i++)
	transform->matrix[i] = matrix[i];
    }
}

void
emacs_skia_image_transform_get_matrix (emacs_skia_image_transform_t *transform,
				       float matrix[6])
{
  if (transform && matrix)
    {
      for (int i = 0; i < 6; i++)
	matrix[i] = transform->matrix[i];
    }
}

void
emacs_skia_image_transform_set_smoothing (emacs_skia_image_transform_t *transform,
					  bool smoothing)
{
  if (transform)
    transform->smoothing = smoothing;
}

bool
emacs_skia_image_transform_get_smoothing (emacs_skia_image_transform_t *transform)
{
  return transform ? transform->smoothing : true;
}

/* Draw an image with transformation applied.  */
void
emacs_skia_canvas_draw_image_transformed (emacs_skia_canvas_t *canvas,
					  emacs_skia_image_t *image,
					  emacs_skia_image_transform_t *transform,
					  float x, float y,
					  emacs_skia_paint_t *paint)
{
  if (!canvas || !canvas->canvas || !image || !image->image)
    return;

  canvas->canvas->save ();

  if (transform)
    {
      /* Apply the transformation matrix.
	 Skia uses column-major SkMatrix, but our matrix is in
	 [a c e; b d f] format which maps to:
	 SkMatrix: [scaleX, skewX, transX, skewY, scaleY, transY, ...] */
      SkMatrix sk_matrix;
      sk_matrix.setAll (transform->matrix[0], /* scaleX = a */
			transform->matrix[2], /* skewX = c */
			transform->matrix[4], /* transX = e */
			transform->matrix[1], /* skewY = b */
			transform->matrix[3], /* scaleY = d */
			transform->matrix[5], /* transY = f */
			0, 0, 1);
      canvas->canvas->concat (sk_matrix);
    }

  SkSamplingOptions sampling (
    transform && transform->smoothing ? SkFilterMode::kLinear
				      : SkFilterMode::kNearest);

  if (paint)
    canvas->canvas->drawImage (image->image.get (), x, y, sampling,
			       &paint->paint);
  else
    canvas->canvas->drawImage (image->image.get (), x, y, sampling);

  canvas->canvas->restore ();
}

/* Calculate stride for pixel buffers (replacement for
   cairo_format_stride_for_width). Skia uses 4-byte aligned rows for RGBA.  */
int
emacs_skia_format_stride_for_width (int format, int width)
{
  /* format: 0 = A8 (1 byte per pixel), 1 = RGB24/ARGB32 (4 bytes per pixel) */
  int bytes_per_pixel = (format == 0) ? 1 : 4;
  int stride = width * bytes_per_pixel;
  /* Align to 4 bytes (Skia's default alignment).  */
  return (stride + 3) & ~3;
}
