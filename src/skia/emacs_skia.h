/* Minimal Skia C API for Emacs
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

#ifndef EMACS_SKIA_H
#define EMACS_SKIA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

  /* Opaque types */
  typedef struct emacs_skia_surface emacs_skia_surface_t;
  typedef struct emacs_skia_canvas emacs_skia_canvas_t;
  typedef struct emacs_skia_paint emacs_skia_paint_t;
  typedef struct emacs_skia_font emacs_skia_font_t;
  typedef struct emacs_skia_typeface emacs_skia_typeface_t;
  typedef struct emacs_skia_image emacs_skia_image_t;
  typedef struct emacs_skia_gl_context emacs_skia_gl_context_t;

  /* Color representation: ARGB in native byte order */
  typedef uint32_t emacs_skia_color_t;

#define EMACS_SKIA_COLOR(a, r, g, b)               \
  (((uint32_t) (a) << 24) | ((uint32_t) (r) << 16) \
   | ((uint32_t) (g) << 8) | (uint32_t) (b))

#define EMACS_SKIA_COLOR_RGB(r, g, b) EMACS_SKIA_COLOR (255, r, g, b)

  /* Rectangle */
  typedef struct
  {
    float left, top, right, bottom;
  } emacs_skia_rect_t;

  /* Integer rectangle */
  typedef struct
  {
    int32_t left, top, right, bottom;
  } emacs_skia_irect_t;

  /* Point */
  typedef struct
  {
    float x, y;
  } emacs_skia_point_t;

  /* Glyph ID */
  typedef uint16_t emacs_skia_glyph_t;

  /* Font metrics */
  typedef struct
  {
    float ascent;
    float descent;
    float leading;
    float avg_char_width;
    float max_char_width;
    float x_height;
    float cap_height;
  } emacs_skia_font_metrics_t;

  /* Blend modes */
  typedef enum
  {
    EMACS_SKIA_BLEND_SRC,
    EMACS_SKIA_BLEND_SRC_OVER,
    EMACS_SKIA_BLEND_DST_OVER,
    EMACS_SKIA_BLEND_CLEAR,
  } emacs_skia_blend_mode_t;

  /* Anti-alias mode */
  typedef enum
  {
    EMACS_SKIA_ANTIALIAS_NONE,
    EMACS_SKIA_ANTIALIAS_NORMAL,
    EMACS_SKIA_ANTIALIAS_SUBPIXEL,
  } emacs_skia_antialias_t;

  /* Font hinting */
  typedef enum
  {
    EMACS_SKIA_HINTING_NONE,
    EMACS_SKIA_HINTING_SLIGHT,
    EMACS_SKIA_HINTING_NORMAL,
    EMACS_SKIA_HINTING_FULL,
  } emacs_skia_hinting_t;

  /* ============================================================
     Initialization / Cleanup
     ============================================================ */

  /* Initialize the Skia subsystem. Call once at startup. */
  void emacs_skia_init (void);

  /* Cleanup the Skia subsystem. Call once at shutdown. */
  void emacs_skia_cleanup (void);

  /* ============================================================
     GL Context (for GPU-accelerated rendering)
     ============================================================ */

  /* Create a GL context for GPU rendering.
     gl_get_proc: function to get GL proc addresses
     Returns NULL on failure. */
  typedef void (*emacs_skia_gl_proc_t) (void);
  typedef emacs_skia_gl_proc_t (*emacs_skia_gl_get_proc_fn) (
    void *ctx, const char *name);

  emacs_skia_gl_context_t *
  emacs_skia_gl_context_create (emacs_skia_gl_get_proc_fn get_proc,
				void *ctx);

  void emacs_skia_gl_context_destroy (emacs_skia_gl_context_t *ctx);

  /* Flush pending GPU operations */
  void emacs_skia_gl_context_flush (emacs_skia_gl_context_t *ctx);

  /* ============================================================
     Surface
     ============================================================ */

  /* Create a raster (CPU) surface */
  emacs_skia_surface_t *emacs_skia_surface_create_raster (int width,
							  int height);

  /* Create a GPU-backed surface using GL framebuffer */
  emacs_skia_surface_t *emacs_skia_surface_create_gl (
    emacs_skia_gl_context_t *ctx, int width, int height,
    unsigned int framebuffer_id, unsigned int format);

  /* Create a surface wrapping existing pixel data */
  emacs_skia_surface_t *emacs_skia_surface_create_from_pixels (
    int width, int height, void *pixels, size_t row_bytes);

  void emacs_skia_surface_destroy (emacs_skia_surface_t *surface);

  /* Get canvas from surface */
  emacs_skia_canvas_t *
  emacs_skia_surface_get_canvas (emacs_skia_surface_t *surface);

  /* Get pixel data (for raster surfaces) */
  void *emacs_skia_surface_get_pixels (emacs_skia_surface_t *surface);

  /* Flush surface drawing */
  void emacs_skia_surface_flush (emacs_skia_surface_t *surface);

  int emacs_skia_surface_get_width (emacs_skia_surface_t *surface);
  int emacs_skia_surface_get_height (emacs_skia_surface_t *surface);

  /* ============================================================
     Canvas (drawing context)
     ============================================================ */

  /* State save/restore */
  void emacs_skia_canvas_save (emacs_skia_canvas_t *canvas);
  void emacs_skia_canvas_restore (emacs_skia_canvas_t *canvas);
  int emacs_skia_canvas_get_save_count (emacs_skia_canvas_t *canvas);
  void
  emacs_skia_canvas_restore_to_count (emacs_skia_canvas_t *canvas,
				      int count);

  /* Clear entire canvas */
  void emacs_skia_canvas_clear (emacs_skia_canvas_t *canvas,
				emacs_skia_color_t color);

  /* Clipping */
  void emacs_skia_canvas_clip_rect (emacs_skia_canvas_t *canvas,
				    const emacs_skia_rect_t *rect);
  void emacs_skia_canvas_clip_irect (emacs_skia_canvas_t *canvas,
				     const emacs_skia_irect_t *rect);

  /* Transform */
  void emacs_skia_canvas_translate (emacs_skia_canvas_t *canvas,
				    float dx, float dy);
  void emacs_skia_canvas_scale (emacs_skia_canvas_t *canvas, float sx,
				float sy);

  /* Drawing primitives */
  void emacs_skia_canvas_draw_rect (emacs_skia_canvas_t *canvas,
				    const emacs_skia_rect_t *rect,
				    emacs_skia_paint_t *paint);

  void emacs_skia_canvas_draw_irect (emacs_skia_canvas_t *canvas,
				     const emacs_skia_irect_t *rect,
				     emacs_skia_paint_t *paint);

  void emacs_skia_canvas_draw_line (emacs_skia_canvas_t *canvas,
				    float x0, float y0, float x1,
				    float y1,
				    emacs_skia_paint_t *paint);

  /* Draw glyphs at specified positions */
  void emacs_skia_canvas_draw_glyphs (
    emacs_skia_canvas_t *canvas, int count,
    const emacs_skia_glyph_t *glyphs,
    const emacs_skia_point_t *positions, emacs_skia_point_t origin,
    emacs_skia_font_t *font, emacs_skia_paint_t *paint);

  /* Draw image */
  void emacs_skia_canvas_draw_image (emacs_skia_canvas_t *canvas,
				     emacs_skia_image_t *image,
				     float x, float y,
				     emacs_skia_paint_t *paint);

  void emacs_skia_canvas_draw_image_rect (
    emacs_skia_canvas_t *canvas, emacs_skia_image_t *image,
    const emacs_skia_rect_t *src, const emacs_skia_rect_t *dst,
    emacs_skia_paint_t *paint);

  /* ============================================================
     Paint (style and color)
     ============================================================ */

  emacs_skia_paint_t *emacs_skia_paint_create (void);
  void emacs_skia_paint_destroy (emacs_skia_paint_t *paint);

  void emacs_skia_paint_set_color (emacs_skia_paint_t *paint,
				   emacs_skia_color_t color);
  emacs_skia_color_t
  emacs_skia_paint_get_color (emacs_skia_paint_t *paint);

  void emacs_skia_paint_set_alpha (emacs_skia_paint_t *paint,
				   uint8_t alpha);

  void emacs_skia_paint_set_antialias (emacs_skia_paint_t *paint,
				       bool antialias);
  void emacs_skia_paint_set_blend_mode (emacs_skia_paint_t *paint,
					emacs_skia_blend_mode_t mode);

  /* Fill vs stroke */
  void emacs_skia_paint_set_stroke (emacs_skia_paint_t *paint,
				    bool stroke);
  void emacs_skia_paint_set_stroke_width (emacs_skia_paint_t *paint,
					  float width);

  /* ============================================================
     Font and Typeface
     ============================================================ */

  /* Load a typeface from a file path */
  emacs_skia_typeface_t *
  emacs_skia_typeface_create_from_file (const char *path);

  /* Load a typeface from memory */
  emacs_skia_typeface_t *
  emacs_skia_typeface_create_from_data (const void *data,
					size_t size);

  /* Load a typeface by family name and style */
  emacs_skia_typeface_t *emacs_skia_typeface_create_from_name (
    const char *family_name, int weight, int width, int slant);

  void emacs_skia_typeface_destroy (emacs_skia_typeface_t *typeface);

  /* Create a font from a typeface at given size */
  emacs_skia_font_t *
  emacs_skia_font_create (emacs_skia_typeface_t *typeface,
			  float size);
  void emacs_skia_font_destroy (emacs_skia_font_t *font);

  void emacs_skia_font_set_size (emacs_skia_font_t *font, float size);
  float emacs_skia_font_get_size (emacs_skia_font_t *font);

  void emacs_skia_font_set_hinting (emacs_skia_font_t *font,
				    emacs_skia_hinting_t hinting);
  void emacs_skia_font_set_edging (emacs_skia_font_t *font,
				   emacs_skia_antialias_t edging);
  void emacs_skia_font_set_subpixel (emacs_skia_font_t *font,
				     bool subpixel);

  /* Get font metrics */
  void
  emacs_skia_font_get_metrics (emacs_skia_font_t *font,
			       emacs_skia_font_metrics_t *metrics);

  /* Convert text to glyphs (UTF-8 input) */
  int emacs_skia_font_text_to_glyphs (emacs_skia_font_t *font,
				      const char *text,
				      size_t byte_length,
				      emacs_skia_glyph_t *glyphs,
				      int max_glyphs);

  /* Convert single Unicode codepoint to glyph */
  emacs_skia_glyph_t
  emacs_skia_font_char_to_glyph (emacs_skia_font_t *font,
				 int32_t codepoint);

  /* Get glyph advance widths */
  void emacs_skia_font_get_widths (emacs_skia_font_t *font,
				   const emacs_skia_glyph_t *glyphs,
				   int count, float *widths);

  /* Measure text width */
  float emacs_skia_font_measure_text (emacs_skia_font_t *font,
				      const char *text,
				      size_t byte_length);

  /* ============================================================
     Image
     ============================================================ */

  /* Create an image from pixel data (RGBA format) */
  emacs_skia_image_t *emacs_skia_image_create_from_pixels (
    int width, int height, const void *pixels, size_t row_bytes,
    bool has_alpha);

  /* Create an image from encoded data (PNG, JPEG, etc.) */
  emacs_skia_image_t *
  emacs_skia_image_create_from_encoded (const void *data,
					size_t size);

  void emacs_skia_image_destroy (emacs_skia_image_t *image);

  int emacs_skia_image_get_width (emacs_skia_image_t *image);
  int emacs_skia_image_get_height (emacs_skia_image_t *image);

#ifdef __cplusplus
}
#endif

#endif /* EMACS_SKIA_H */
