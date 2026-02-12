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

  /* Glyph extents (replacement for cairo_text_extents_t) */
  typedef struct
  {
    float x_bearing; /* Left side bearing */
    float
      y_bearing;  /* Top side bearing (negative = above baseline) */
    float width;  /* Glyph width */
    float height; /* Glyph height */
    float x_advance; /* Horizontal advance */
    float y_advance; /* Vertical advance (usually 0) */
  } emacs_skia_glyph_extents_t;

  /* Font extents (replacement for cairo_font_extents_t) */
  typedef struct
  {
    float ascent;	 /* Distance from baseline to top */
    float descent;	 /* Distance from baseline to bottom */
    float height;	 /* Recommended line height */
    float max_x_advance; /* Maximum horizontal advance */
    float max_y_advance; /* Maximum vertical advance */
  } emacs_skia_font_extents_t;

  /* Blend modes */
  typedef enum
  {
    EMACS_SKIA_BLEND_SRC,
    EMACS_SKIA_BLEND_SRC_OVER,
    EMACS_SKIA_BLEND_DST_OVER,
    EMACS_SKIA_BLEND_CLEAR,
    EMACS_SKIA_BLEND_XOR,
    EMACS_SKIA_BLEND_DIFFERENCE,
    EMACS_SKIA_BLEND_EXCLUSION,
  } emacs_skia_blend_mode_t;

  /* Opaque path type for complex shapes */
  typedef struct emacs_skia_path emacs_skia_path_t;

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
     Capability Queries
     ============================================================ */

  /* Query whether specific Skia features are available.
     These functions return true if the feature was compiled in.  */
  bool emacs_skia_has_gl_support (void);
  bool emacs_skia_has_pdf_support (void);
  bool emacs_skia_has_svg_support (void);

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

  /* Create a GL context using the native GL interface.  This uses
     the currently active GL context (e.g., set by GDK).  */
  emacs_skia_gl_context_t *emacs_skia_gl_context_create_native (void);

  void emacs_skia_gl_context_destroy (emacs_skia_gl_context_t *ctx);

  /* Flush pending GPU operations */
  void emacs_skia_gl_context_flush (emacs_skia_gl_context_t *ctx);

  /* Reset the GL context state tracking.  Call this after destroying
     a GL surface to clear Skia's internal caches.  */
  void emacs_skia_gl_context_reset (emacs_skia_gl_context_t *ctx);

  /* ============================================================
     GL Fence Sync (for async GPU synchronization)
     ============================================================ */

  /* Opaque fence object for non-blocking GPU synchronization.  */
  typedef struct emacs_skia_fence emacs_skia_fence_t;

  /* Create a fence that will be signaled when all preceding GL
     commands have completed on the GPU.  */
  emacs_skia_fence_t *emacs_skia_fence_create (void);

  /* Wait for fence to be signaled.  Returns true if signaled within
     timeout_ns nanoseconds, false if timed out.  Pass 0 for no wait
     (poll), or UINT64_MAX for infinite wait.  */
  bool emacs_skia_fence_wait (emacs_skia_fence_t *fence, uint64_t timeout_ns);

  /* Check if fence is signaled without waiting.  */
  bool emacs_skia_fence_is_signaled (emacs_skia_fence_t *fence);

  /* Destroy fence object.  */
  void emacs_skia_fence_destroy (emacs_skia_fence_t *fence);

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

  /* Make an image snapshot from the surface (for scrolling/copying)
   */
  emacs_skia_image_t *emacs_skia_surface_make_image_snapshot (
    emacs_skia_surface_t *surface);
  emacs_skia_image_t *emacs_skia_surface_make_image_snapshot_rect (
    emacs_skia_surface_t *surface, const emacs_skia_irect_t *rect);

  /* Write callback for PNG/document output.
     Returns number of bytes written, or 0 on error.  */
  typedef size_t (*emacs_skia_write_fn) (void *ctx, const void *data,
					 size_t size);

  /* Write surface to PNG using a callback function.
     Returns true on success, false on failure.  */
  bool emacs_skia_surface_write_to_png (emacs_skia_surface_t *surface,
					emacs_skia_write_fn write_fn,
					void *write_ctx);

  /* Write image to PNG using a callback function.
     Returns true on success, false on failure.  */
  bool emacs_skia_image_write_to_png (emacs_skia_image_t *image,
				      emacs_skia_write_fn write_fn,
				      void *write_ctx);

  /* Calculate stride for pixel buffers.
     format: 0 = A8 (1 byte/pixel), 1 = RGB24/ARGB32 (4 bytes/pixel).
     Returns stride in bytes (4-byte aligned).  */
  int emacs_skia_format_stride_for_width (int format, int width);

  /* ============================================================
     Image Transformation
     ============================================================ */

  /* Opaque image transformation type.  */
  typedef struct emacs_skia_image_transform emacs_skia_image_transform_t;

  /* Create/destroy image transformation.  */
  emacs_skia_image_transform_t *emacs_skia_image_transform_create (void);
  void emacs_skia_image_transform_destroy (
    emacs_skia_image_transform_t *transform);

  /* Set transformation matrix.
     matrix is [a, b, c, d, e, f] representing:
     [a c e]
     [b d f]
     [0 0 1]  */
  void emacs_skia_image_transform_set_matrix (
    emacs_skia_image_transform_t *transform, const float matrix[6]);
  void emacs_skia_image_transform_get_matrix (
    emacs_skia_image_transform_t *transform, float matrix[6]);

  /* Set/get smoothing (filter mode).  */
  void emacs_skia_image_transform_set_smoothing (
    emacs_skia_image_transform_t *transform, bool smoothing);
  bool emacs_skia_image_transform_get_smoothing (
    emacs_skia_image_transform_t *transform);

  /* ============================================================
     Canvas (drawing context)
     ============================================================ */

  /* State save/restore */
  void emacs_skia_canvas_save (emacs_skia_canvas_t *canvas);
  void emacs_skia_canvas_save_layer (emacs_skia_canvas_t *canvas,
				     const emacs_skia_rect_t *bounds);
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

  /* Get current clip bounds */
  void emacs_skia_canvas_get_clip_bounds (emacs_skia_canvas_t *canvas,
					  emacs_skia_rect_t *bounds);

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

  /* Draw image with explicit sampling control.
     If smooth is true, use linear filtering (good for scaling down).
     If smooth is false, use nearest neighbor (shows pixels when scaling up).  */
  void emacs_skia_canvas_draw_image_with_sampling (emacs_skia_canvas_t *canvas,
						   emacs_skia_image_t *image,
						   float x, float y,
						   bool smooth,
						   emacs_skia_paint_t *paint);

  void emacs_skia_canvas_draw_image_rect (
    emacs_skia_canvas_t *canvas, emacs_skia_image_t *image,
    const emacs_skia_rect_t *src, const emacs_skia_rect_t *dst,
    bool smooth, emacs_skia_paint_t *paint);

  /* Draw image with transformation applied.  */
  void emacs_skia_canvas_draw_image_transformed (
    emacs_skia_canvas_t *canvas, emacs_skia_image_t *image,
    emacs_skia_image_transform_t *transform, float x, float y,
    emacs_skia_paint_t *paint);

  /* Draw a path */
  void emacs_skia_canvas_draw_path (emacs_skia_canvas_t *canvas,
				    emacs_skia_path_t *path,
				    emacs_skia_paint_t *paint);

  /* Draw an arc (part of an ellipse)
     oval: bounding rectangle of the ellipse
     start_angle: starting angle in degrees (0 = 3 o'clock)
     sweep_angle: arc extent in degrees (positive = clockwise)
     use_center: if true, draws pie slice; if false, draws arc */
  void emacs_skia_canvas_draw_arc (emacs_skia_canvas_t *canvas,
				   const emacs_skia_rect_t *oval,
				   float start_angle,
				   float sweep_angle, bool use_center,
				   emacs_skia_paint_t *paint);

  /* Draw image with a mask (for stipples and transparency masks)
     image: the image to draw
     mask: alpha mask image (A8 format preferred)
     x, y: destination position */
  void emacs_skia_canvas_draw_image_with_mask (
    emacs_skia_canvas_t *canvas, emacs_skia_image_t *image,
    emacs_skia_image_t *mask, float x, float y,
    emacs_skia_paint_t *paint);

  /* ============================================================
     Path (for complex shapes)
     ============================================================ */

  emacs_skia_path_t *emacs_skia_path_create (void);
  void emacs_skia_path_destroy (emacs_skia_path_t *path);

  /* Reset path to empty */
  void emacs_skia_path_reset (emacs_skia_path_t *path);

  /* Path construction */
  void emacs_skia_path_move_to (emacs_skia_path_t *path, float x,
				float y);
  void emacs_skia_path_line_to (emacs_skia_path_t *path, float x,
				float y);
  void emacs_skia_path_rel_line_to (emacs_skia_path_t *path, float dx,
				    float dy);

  /* Add an arc to the path
     oval: bounding rectangle of the ellipse
     start_angle: starting angle in degrees
     sweep_angle: arc extent in degrees */
  void emacs_skia_path_arc_to (emacs_skia_path_t *path,
			       const emacs_skia_rect_t *oval,
			       float start_angle, float sweep_angle,
			       bool force_move_to);

  /* Close the current contour */
  void emacs_skia_path_close (emacs_skia_path_t *path);

  /* Add a rectangle to the path */
  void emacs_skia_path_add_rect (emacs_skia_path_t *path,
				 const emacs_skia_rect_t *rect);

  /* Clip canvas to path */
  void emacs_skia_canvas_clip_path (emacs_skia_canvas_t *canvas,
				    emacs_skia_path_t *path);

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

  /* Dash pattern for strokes.
     intervals: array of on/off lengths (must have even count)
     count: number of elements in intervals array
     phase: offset into the dash pattern */
  void emacs_skia_paint_set_dash (emacs_skia_paint_t *paint,
				  const float *intervals, int count,
				  float phase);

  /* Clear dash pattern (solid line) */
  void emacs_skia_paint_clear_dash (emacs_skia_paint_t *paint);

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

#ifdef HAVE_FONTCONFIG
  /* Forward declaration for FcPattern */
  struct _FcPattern;
  typedef struct _FcPattern FcPattern;

  /* Load a typeface from a fontconfig pattern
     (replacement for cairo_ft_font_face_create_for_pattern) */
  emacs_skia_typeface_t *
  emacs_skia_typeface_create_from_fc_pattern (FcPattern *pattern);
#endif

  void emacs_skia_typeface_destroy (emacs_skia_typeface_t *typeface);

  /* Get the font file path from a typeface (if available) */
  const char *
  emacs_skia_typeface_get_path (emacs_skia_typeface_t *typeface);

  /* Get FreeType face index within the font file */
  int emacs_skia_typeface_get_index (emacs_skia_typeface_t *typeface);

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

  /* Get font extents (replacement for cairo_scaled_font_extents) */
  void
  emacs_skia_font_get_extents (emacs_skia_font_t *font,
			       emacs_skia_font_extents_t *extents);

  /* Get glyph extents (replacement for
     cairo_scaled_font_glyph_extents) Returns extents for each glyph
     in the array. */
  void emacs_skia_font_get_glyph_extents (
    emacs_skia_font_t *font, const emacs_skia_glyph_t *glyphs,
    int count, emacs_skia_glyph_extents_t *extents);

  /* Get bounds for multiple glyphs (array version) */
  void emacs_skia_font_get_glyph_bounds (
    emacs_skia_font_t *font, const emacs_skia_glyph_t *glyphs,
    int count, emacs_skia_rect_t *bounds);

  /* Convert text to glyphs (UTF-8 input).
     The caller must ensure that byte_length does not exceed the actual
     buffer size of text to prevent buffer over-read.  */
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

  /* Create an image from BGRA pixel data (Cairo/native format).
     This is the format used by Cairo (ARGB32 in native byte order),
     which on little-endian machines is BGRA when viewed as bytes. */
  emacs_skia_image_t *emacs_skia_image_create_from_bgra_pixels (
    int width, int height, const void *pixels, size_t row_bytes,
    bool has_alpha);

  /* Create an image from encoded data (PNG, JPEG, etc.) */
  emacs_skia_image_t *
  emacs_skia_image_create_from_encoded (const void *data,
					size_t size);

  void emacs_skia_image_destroy (emacs_skia_image_t *image);

  int emacs_skia_image_get_width (emacs_skia_image_t *image);
  int emacs_skia_image_get_height (emacs_skia_image_t *image);

  /* Create a 1-bit (A1 format) image from packed bitmap data.
     This is used for fringe bitmaps and stipple patterns.
     data: packed 1-bit data, MSB first, rows padded to byte boundary
     width, height: dimensions in pixels
     stride: bytes per row (0 = calculate from width) */
  emacs_skia_image_t *emacs_skia_image_create_from_bitmap (
    const unsigned char *data, int width, int height, int stride);

  /* Create a tiled shader/pattern from an image for stipple fills.
     Returns a paint configured with the tiled image. */
  void emacs_skia_paint_set_image_shader (emacs_skia_paint_t *paint,
					  emacs_skia_image_t *image);

  /* Clear the shader from paint (return to solid color) */
  void emacs_skia_paint_clear_shader (emacs_skia_paint_t *paint);

  /* ============================================================
     Document Export (PDF/SVG)
     ============================================================ */

  /* Opaque document type for multi-page PDF output */
  typedef struct emacs_skia_document emacs_skia_document_t;

  /* Create a PDF document that writes to a callback.
     width/height are the initial page dimensions in points.  */
  emacs_skia_document_t *
  emacs_skia_document_create_pdf (emacs_skia_write_fn write_fn,
				  void *write_ctx, float width,
				  float height);

  /* Begin a new page in the document.
     Returns a canvas to draw on.  */
  emacs_skia_canvas_t *
  emacs_skia_document_begin_page (emacs_skia_document_t *doc,
				  float width, float height);

  /* End the current page.  */
  void emacs_skia_document_end_page (emacs_skia_document_t *doc);

  /* Close and finalize the document.
     This must be called to complete the output.  */
  void emacs_skia_document_close (emacs_skia_document_t *doc);

  /* Create an SVG canvas that writes to a callback.
     SVG is a single-page format, so there's no document concept.
     The returned canvas should be destroyed with
     emacs_skia_svg_canvas_finish() when done.  */
  emacs_skia_canvas_t *
  emacs_skia_svg_canvas_create (emacs_skia_write_fn write_fn,
				void *write_ctx, float width,
				float height);

  /* Finish and close the SVG canvas.
     This writes the closing SVG tags and finalizes output.  */
  void emacs_skia_svg_canvas_finish (emacs_skia_canvas_t *canvas);

#ifdef __cplusplus
}
#endif

#endif /* EMACS_SKIA_H */
