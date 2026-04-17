/* skiafont.c -- FreeType font driver with Skia rendering.
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

/* This font driver uses FreeType for font discovery and Skia for
   rendering.  It uses Skia for all font metrics and rendering.  */

#include <config.h>

#ifdef USE_SKIA

#include <stdio.h>

/* Debug logging for Skia font driver failures.
   Define SKIA_DEBUG at compile time to enable verbose error messages.  */
#ifdef SKIA_DEBUG
#define SKIAFONT_LOG_ERROR(fmt, ...) \
  fprintf (stderr, "skiafont: " fmt "\n", ##__VA_ARGS__)
#else
#define SKIAFONT_LOG_ERROR(fmt, ...) ((void)0)
#endif

# include <fontconfig/fontconfig.h>
# include <ft2build.h>
# include <math.h>
# include FT_FREETYPE_H

# include "lisp.h"
# include "blockinput.h"
# include "charset.h"
# include "composite.h"
# include "dispextern.h"
# include "font.h"
# include "ftfont.h"
# include "pdumper.h"
# include "pgtkterm.h"
# include "xsettings.h"

/* We extend font_info to include Skia-specific data.  */
struct skia_font_info
{
  /* The base font_info from ftfont.  */
  struct font_info base;

  /* Skia typeface for this font.  */
  emacs_skia_typeface_t *skia_typeface;

  /* Skia font object (typeface + size).  */
  emacs_skia_font_t *skia_font;
};

# define METRICS_NCOLS_PER_ROW (128)

enum metrics_status
{
  METRICS_INVALID = -1, /* metrics entry is invalid */
};

# define METRICS_STATUS(metrics) \
   ((metrics)->ascent + (metrics)->descent)
# define METRICS_SET_STATUS(metrics, status) \
   ((metrics)->ascent = 0, (metrics)->descent = (status))

static int
skiafont_glyph_extents (struct font *font, unsigned glyph,
			struct font_metrics *metrics)
{
  struct skia_font_info *skiafont_info
    = (struct skia_font_info *) font;
  struct font_info *ftfont_info = &skiafont_info->base;
  int row, col;
  struct font_metrics *cache;

  row = glyph / METRICS_NCOLS_PER_ROW;
  col = glyph % METRICS_NCOLS_PER_ROW;
  if (row >= ftfont_info->metrics_nrows)
    {
      ftfont_info->metrics
	= xrealloc (ftfont_info->metrics,
		    sizeof (struct font_metrics *) * (row + 1));
      memset (ftfont_info->metrics + ftfont_info->metrics_nrows, 0,
	      (sizeof (struct font_metrics *)
	       * (row + 1 - ftfont_info->metrics_nrows)));
      ftfont_info->metrics_nrows = row + 1;
    }
  if (ftfont_info->metrics[row] == NULL)
    {
      struct font_metrics *new;
      int i;

      new = xmalloc (sizeof (struct font_metrics)
		     * METRICS_NCOLS_PER_ROW);
      for (i = 0; i < METRICS_NCOLS_PER_ROW; i++)
	METRICS_SET_STATUS (new + i, METRICS_INVALID);
      ftfont_info->metrics[row] = new;
    }
  cache = ftfont_info->metrics[row] + col;

  if (METRICS_STATUS (cache) == METRICS_INVALID)
    {
      /* Get glyph extents from Skia.  */
      if (skiafont_info->skia_font)
	{
	  emacs_skia_glyph_t skia_glyph = glyph;
	  emacs_skia_glyph_extents_t extents;

	  emacs_skia_font_get_glyph_extents (skiafont_info->skia_font,
					     &skia_glyph, 1,
					     &extents);
	  cache->lbearing = floor (extents.x_bearing);
	  cache->rbearing = ceil (extents.width + extents.x_bearing);
	  cache->width = lround (extents.x_advance);
	  cache->ascent = ceil (-(double)extents.y_bearing - 1.0 / 256);
	  cache->descent = ceil (extents.height + extents.y_bearing);
	}
      else
	{
	  /* Fallback: return zero metrics if no Skia font.  */
	  cache->lbearing = 0;
	  cache->rbearing = 0;
	  cache->width = 0;
	  cache->ascent = 0;
	  cache->descent = 0;
	}
    }

  if (metrics)
    *metrics = *cache;

  return cache->width;
}

static Lisp_Object
skiafont_list (struct frame *f, Lisp_Object spec)
{
  return ftfont_list2 (f, spec, Qskia);
}

static Lisp_Object
skiafont_match (struct frame *f, Lisp_Object spec)
{
  return ftfont_match2 (f, spec, Qskia);
}

/* FreeType library handle for direct font access.
   Thread safety: This global is initialized once from the main thread
   during font driver setup.  Emacs display code runs single-threaded,
   so no synchronization is needed.  If Emacs ever becomes multi-threaded
   for display operations, this would need mutex protection.  */
static FT_Library ft_library;
static bool ft_library_initialized;

static bool
skiafont_init_freetype (void)
{
  if (!ft_library_initialized)
    {
      if (FT_Init_FreeType (&ft_library) == 0)
	ft_library_initialized = true;
    }
  return ft_library_initialized;
}

static Lisp_Object
skiafont_open (struct frame *f, Lisp_Object entity, int pixel_size)
{
  FcResult result;
  Lisp_Object val, filename, font_object;
  FcPattern *pat, *match;
  struct skia_font_info *skiafont_info;
  struct font *font;
  double size = 0;
  char *filename_str;
  int font_index = 0;
  FT_Face ft_face = NULL;

  val = assq_no_quit (QCfont_entity, AREF (entity, FONT_EXTRA_INDEX));
  if (!CONSP (val))
    return Qnil;
  val = XCDR (val);
  filename = XCAR (val);
  size = XFIXNUM (AREF (entity, FONT_SIZE_INDEX));
  if (size == 0)
    size = pixel_size;

  block_input ();

  pat = ftfont_entity_pattern (entity, pixel_size);
  FcConfigSubstitute (NULL, pat, FcMatchPattern);
  FcDefaultSubstitute (pat);
  match = FcFontMatch (NULL, pat, &result);
  FcPatternDestroy (pat);
  if (!match)
    {
      unblock_input ();
      return Qnil;
    }
  ftfont_fix_match (NULL, match);

  /* Get font index from the match.  */
  FcPatternGetInteger (match, FC_INDEX, 0, &font_index);

  font_object = font_build_object (VECSIZE (struct skia_font_info),
				   Qskia, entity, size);
  skiafont_info
    = (struct skia_font_info *) XFONT_OBJECT (font_object);

  font = &skiafont_info->base.font;
  font->pixel_size = size;
  font->driver = &skiafont_driver;
  font->encoding_charset = -1;
  font->repertory_charset = -1;
  font->default_ascent = 0;
  font->vertical_centering = false;
  font->baseline_offset = 0;
  font->relative_compose = 0;

  skiafont_info->base.metrics = NULL;
  skiafont_info->base.metrics_nrows = 0;
  skiafont_info->base.bitmap_position_unit = 0;
  skiafont_info->base.ft_face = NULL;

  /* Initialize FT matrix from fontconfig pattern.  */
  {
    FcMatrix *fc_matrix;
    skiafont_info->base.matrix.xx = 0;
    skiafont_info->base.matrix.yy = 0;
    skiafont_info->base.matrix.xy = 0;
    skiafont_info->base.matrix.yx = 0;
    if (FcPatternGetMatrix (match, FC_MATRIX, 0, &fc_matrix)
	== FcResultMatch)
      {
	skiafont_info->base.matrix.xx = 0x10000L * fc_matrix->xx;
	skiafont_info->base.matrix.yy = 0x10000L * fc_matrix->yy;
	skiafont_info->base.matrix.xy = 0x10000L * fc_matrix->xy;
	skiafont_info->base.matrix.yx = 0x10000L * fc_matrix->yx;
      }
  }

# ifdef HAVE_LIBOTF
  skiafont_info->base.maybe_otf = false;
  skiafont_info->base.otf = NULL;
# endif
# ifdef HAVE_HARFBUZZ
  skiafont_info->base.hb_font = NULL;
# endif

  /* Set FONT_FILE_INDEX for font introspection.  */
  ASET (font_object, FONT_FILE_INDEX, filename);

  /* Create Skia typeface and font, using fontconfig pattern to
     correctly handle font index for TTC (TrueType Collection) files.  */
  filename_str = SSDATA (filename);
  skiafont_info->skia_typeface
    = emacs_skia_typeface_create_from_fc_pattern (match);
  if (skiafont_info->skia_typeface)
    {
      skiafont_info->skia_font
	= emacs_skia_font_create (skiafont_info->skia_typeface, size);
      if (skiafont_info->skia_font)
	{
	  emacs_skia_font_set_subpixel (skiafont_info->skia_font,
					true);
	  emacs_skia_font_set_hinting (skiafont_info->skia_font,
				       EMACS_SKIA_HINTING_NORMAL);
	}
    }
  else
    skiafont_info->skia_font = NULL;

  /* Get font metrics from Skia.  */
  if (skiafont_info->skia_font)
    {
      emacs_skia_font_extents_t extents;
      emacs_skia_font_get_extents (skiafont_info->skia_font,
				   &extents);
      font->ascent = lround (extents.ascent);

      /* Handle :minspace property like ftcrfont_open does.  */
      {
	Lisp_Object val = assq_no_quit (QCminspace,
					AREF (entity, FONT_EXTRA_INDEX));
	if (!(CONSP (val) && NILP (XCDR (val))))
	  {
	    font->descent = lround (extents.descent);
	    font->height = font->ascent + font->descent;
	  }
	else
	  {
	    /* Fallback to max_x_advance if we couldn't measure
	     * characters.  */
	    font->min_width = font->average_width = font->space_width
	      = font->max_width = lround (extents.max_x_advance);
	  }
      }

      /* Calculate average_width properly by measuring printable ASCII
	 characters, similar to ftfont.c.  Using max_x_advance would give
	 the maximum character width, which is incorrect for proportional
	 fonts and causes issues with image scaling.  */
      {
	int total_width = 0, n = 0, min_w = 0, max_w = 0, space_w = 0;

	for (int c = 32; c < 127; c++)
	  {
	    emacs_skia_glyph_t glyph
	      = emacs_skia_font_char_to_glyph (skiafont_info->skia_font, c);
	    if (glyph != 0)
	      {
		emacs_skia_glyph_extents_t glyph_ext;
		emacs_skia_font_get_glyph_extents (skiafont_info->skia_font,
						   &glyph, 1, &glyph_ext);
		int w = lround (glyph_ext.x_advance);
		if (w > 0)
		  {
		    total_width += w;
		    n++;
		    if (min_w == 0 || w < min_w)
		      min_w = w;
		    if (w > max_w)
		      max_w = w;
		    if (c == 32)
		      space_w = w;
		  }
	      }
	  }

	if (n > 0)
	  {
	    font->average_width = total_width / n;
	    font->min_width = min_w;
	    font->max_width = max_w;
	    font->space_width
	      = space_w > 0 ? space_w : font->average_width;
	  }
	else
	  {
	    /* Fallback to max_x_advance if we couldn't measure characters.  */
	    font->min_width = font->average_width = font->space_width
	      = lround (extents.max_x_advance);
	  }
      }
    }
  else
    {
      /* Fallback to default metrics if Skia font creation failed.  */
      SKIAFONT_LOG_ERROR ("failed to create Skia font for '%s', "
			  "using fallback metrics", filename_str);
      font->ascent = pixel_size;
      font->descent = pixel_size / 4;
      font->height = font->ascent + font->descent;
      font->min_width = font->average_width = font->space_width
	= font->max_width = pixel_size / 2;
    }

  /* Get underline metrics from FreeType directly.  */
  if (skiafont_init_freetype ()
      && FT_New_Face (ft_library, filename_str, font_index, &ft_face)
	   == 0)
    {
      if (ft_face->face_flags & FT_FACE_FLAG_SCALABLE)
	{
	  font->underline_position = -ft_face->underline_position
				     * size / ft_face->units_per_EM;
	  font->underline_thickness = ft_face->underline_thickness
				      * size / ft_face->units_per_EM;
	}
      else
	{
	  font->underline_position = -1;
	  font->underline_thickness = 1;
	}
      /* Store ft_face for later use (encode_char, etc.).  */
      skiafont_info->base.ft_face = ft_face;

      /* Compute bitmap_position_unit for bitmap fonts (Bug#73752).  */
      if (ft_face->units_per_EM)
	skiafont_info->base.bitmap_position_unit = 0;
      else if (skiafont_info->skia_font && ft_face->size
	       && ft_face->size->metrics.height > 0)
	{
	  emacs_skia_font_extents_t ext;
	  emacs_skia_font_get_extents (skiafont_info->skia_font, &ext);
	  skiafont_info->base.bitmap_position_unit
	    = ext.height / ft_face->size->metrics.height;
	}
      else
	skiafont_info->base.bitmap_position_unit = 0;

# ifdef HAVE_LIBOTF
      skiafont_info->base.maybe_otf
	= (ft_face->face_flags & FT_FACE_FLAG_SFNT) != 0;
# endif

      /* Adjust underline thickness/position like ftcrfont.  */
      if (font->underline_thickness > 2)
	font->underline_position -= font->underline_thickness / 2;
    }
  else
    {
      /* Default underline metrics.  */
      font->underline_position = -1;
      font->underline_thickness = 1;
      /* ft_face is NULL or failed to load, nothing to clean up.  */
    }

  FcPatternDestroy (match);
  unblock_input ();

  return font_object;
}

static void
skiafont_close (struct font *font)
{
  if (font_data_structures_may_be_ill_formed ())
    return;

  struct skia_font_info *skiafont_info
    = (struct skia_font_info *) font;
  int i;

  block_input ();

  if (skiafont_info->skia_font)
    {
      emacs_skia_font_destroy (skiafont_info->skia_font);
      skiafont_info->skia_font = NULL;
    }
  if (skiafont_info->skia_typeface)
    {
      emacs_skia_typeface_destroy (skiafont_info->skia_typeface);
      skiafont_info->skia_typeface = NULL;
    }

  /* Close the FreeType face.  */
  if (skiafont_info->base.ft_face)
    {
      FT_Done_Face (skiafont_info->base.ft_face);
      skiafont_info->base.ft_face = NULL;
    }

  if (skiafont_info->base.metrics)
    {
      for (i = 0; i < skiafont_info->base.metrics_nrows; i++)
	if (skiafont_info->base.metrics[i])
	  xfree (skiafont_info->base.metrics[i]);
      xfree (skiafont_info->base.metrics);
      skiafont_info->base.metrics = NULL;
    }

  unblock_input ();
}

static int
skiafont_has_char (Lisp_Object font, int c)
{
  if (FONT_ENTITY_P (font))
    return ftfont_has_char (font, c);

  struct charset *cs = NULL;

  if (EQ (AREF (font, FONT_ADSTYLE_INDEX), Qja)
      && charset_jisx0208 >= 0)
    cs = CHARSET_FROM_ID (charset_jisx0208);
  else if (EQ (AREF (font, FONT_ADSTYLE_INDEX), Qko)
	   && charset_ksc5601 >= 0)
    cs = CHARSET_FROM_ID (charset_ksc5601);
  if (cs)
    return (ENCODE_CHAR (cs, c) != CHARSET_INVALID_CODE (cs));

  return -1;
}

static unsigned
skiafont_encode_char (struct font *font, int c)
{
  struct skia_font_info *skiafont_info
    = (struct skia_font_info *) font;

  /* Use Skia if available.  */
  if (skiafont_info->skia_font)
    {
      unsigned glyph
	= emacs_skia_font_char_to_glyph (skiafont_info->skia_font, c);
      /* Glyph index 0 means the character is not in the font.
	 Return FONT_INVALID_CODE so Emacs will look for a fallback font.  */
      if (glyph)
	return glyph;
      return FONT_INVALID_CODE;
    }

  /* Fall back to FreeType directly.  */
  struct font_info *ftfont_info = &skiafont_info->base;
  if (ftfont_info->ft_face)
    {
      unsigned glyph = FT_Get_Char_Index (ftfont_info->ft_face, c);
      if (glyph)
	return glyph;
    }

  return FONT_INVALID_CODE;
}

static void
skiafont_text_extents (struct font *font, const unsigned int *code,
		       int nglyphs, struct font_metrics *metrics)
{
  int i, width = 0;

  block_input ();
  memset (metrics, 0, sizeof (*metrics));

  for (i = 0; i < nglyphs; i++)
    {
      struct font_metrics m;
      int glyph_width = skiafont_glyph_extents (font, code[i], &m);

      if (i == 0)
	{
	  metrics->lbearing = m.lbearing;
	  metrics->rbearing = m.rbearing;
	}
      else
	{
	  if (metrics->lbearing > m.lbearing + width)
	    metrics->lbearing = m.lbearing + width;
	  if (metrics->rbearing < m.rbearing + width)
	    metrics->rbearing = m.rbearing + width;
	}
      if (metrics->ascent < m.ascent)
	metrics->ascent = m.ascent;
      if (metrics->descent < m.descent)
	metrics->descent = m.descent;

      width += glyph_width;
    }

  metrics->width = width;
  unblock_input ();
}

static int
skiafont_draw (struct glyph_string *s, int from, int to, int x, int y,
	       bool with_background)
{
  struct frame *f = s->f;
  struct skia_font_info *skiafont_info
    = (struct skia_font_info *) s->font;
  int len = to - from;
  int i;

  block_input ();

  emacs_skia_canvas_t *canvas = pgtk_begin_skia_clip (f);
  if (!canvas)
    {
      unblock_input ();
      return 0;
    }

  /* Apply clipping from glyph string to prevent drawing outside
     bounds.  Use the shared helper so that when
     get_glyph_string_clip_rects returns multiple rectangles (the
     OVERLAPS case), the clip is the UNION of those rectangles rather
     than their intersection.  Skia's canvas->clipRect intersects by
     default, so calling it in a loop over disjoint rectangles would
     produce an empty clip region and drop drawing.  */
  pgtk_skia_set_glyph_string_clipping (s, canvas);

  emacs_skia_paint_t *paint = FRAME_SKIA_PAINT (f);

  if (with_background)
    {
      pgtk_skia_set_paint_color (f, s->xgcv.background,
				 s->hl != DRAW_CURSOR);
      emacs_skia_irect_t rect
	= { x, y - FONT_BASE (s->font), x + s->width,
	    y - FONT_BASE (s->font) + FONT_HEIGHT (s->font) };
      emacs_skia_canvas_draw_irect (canvas, &rect, paint);
    }

  /* Set foreground color for text.  */
  pgtk_skia_set_paint_color (f, s->xgcv.foreground, false);

  /* Draw glyphs using Skia if we have a Skia font, otherwise fall
     back to Cairo-style rendering via the raster surface.  */
  if (skiafont_info->skia_font)
    {
      /* Allocate glyph and position arrays.  */
      emacs_skia_glyph_t *glyphs
	= alloca (sizeof (emacs_skia_glyph_t) * len);
      emacs_skia_point_t *positions
	= alloca (sizeof (emacs_skia_point_t) * len);

      float current_x = 0;
      for (i = 0; i < len; i++)
	{
	  glyphs[i] = s->char2b[from + i];
	  positions[i].x = current_x;
	  positions[i].y = 0;
	  current_x += (s->padding_p
			  ? 1
			  : skiafont_glyph_extents (s->font,
						    glyphs[i], NULL));
	}

      emacs_skia_point_t origin = { x, y };
      emacs_skia_canvas_draw_glyphs (canvas, len, glyphs, positions,
				     origin, skiafont_info->skia_font,
				     paint);
    }
  else
    {
      /* Fallback: draw each glyph as a rectangle (placeholder).  */
      float current_x = x;
      for (i = 0; i < len; i++)
	{
	  int glyph_width
	    = skiafont_glyph_extents (s->font, s->char2b[from + i],
				      NULL);
	  /* For fallback, we could potentially use Cairo here,
	     but for now just advance.  */
	  current_x += (s->padding_p ? 1 : glyph_width);
	}
    }

  pgtk_end_skia_clip (f);
  unblock_input ();

  return len;
}

/* These functions use the stored FT_Face directly for FreeType
 * operations.  */

static int
skiafont_get_bitmap (struct font *font, unsigned int code,
		     struct font_bitmap *bitmap, int bits_per_pixel)
{
  struct font_info *ftfont_info = (struct font_info *) font;
  FT_Face ft_face = ftfont_info->ft_face;

  if (!ft_face)
    return -1;

  ftfont_info->ft_size = ft_face->size;
  int result = ftfont_get_bitmap (font, code, bitmap, bits_per_pixel);
  ftfont_info->ft_size = NULL;

  return result;
}

static int
skiafont_anchor_point (struct font *font, unsigned int code, int idx,
		       int *x, int *y)
{
  struct font_info *ftfont_info = (struct font_info *) font;
  FT_Face ft_face = ftfont_info->ft_face;

  if (!ft_face)
    return -1;

  ftfont_info->ft_size = ft_face->size;
  int result = ftfont_anchor_point (font, code, idx, x, y);
  ftfont_info->ft_size = NULL;

  return result;
}

# ifdef HAVE_LIBOTF
static Lisp_Object
skiafont_otf_capability (struct font *font)
{
  struct font_info *ftfont_info = (struct font_info *) font;
  FT_Face ft_face = ftfont_info->ft_face;

  if (!ft_face)
    return Qnil;

  ftfont_info->ft_size = ft_face->size;
  Lisp_Object result = ftfont_otf_capability (font);
  ftfont_info->ft_size = NULL;

  return result;
}
# endif

# if defined HAVE_M17N_FLT && defined HAVE_LIBOTF
static Lisp_Object
skiafont_shape (Lisp_Object lgstring, Lisp_Object direction)
{
  struct font *font
    = CHECK_FONT_GET_OBJECT (LGSTRING_FONT (lgstring));
  struct font_info *ftfont_info = (struct font_info *) font;
  FT_Face ft_face = ftfont_info->ft_face;

  if (!ft_face)
    return Qnil;

  ftfont_info->ft_size = ft_face->size;
  Lisp_Object result = ftfont_shape (lgstring, direction);
  ftfont_info->ft_size = NULL;

  return result;
}
# endif

# if defined HAVE_OTF_GET_VARIATION_GLYPHS \
   || defined HAVE_FT_FACE_GETCHARVARIANTINDEX
static int
skiafont_variation_glyphs (struct font *font, int c,
			   unsigned variations[256])
{
  struct font_info *ftfont_info = (struct font_info *) font;
  FT_Face ft_face = ftfont_info->ft_face;

  if (!ft_face)
    return 0;

  ftfont_info->ft_size = ft_face->size;
  int result = ftfont_variation_glyphs (font, c, variations);
  ftfont_info->ft_size = NULL;

  return result;
}
# endif

# ifdef HAVE_HARFBUZZ

static Lisp_Object
skiahbfont_list (struct frame *f, Lisp_Object spec)
{
  return ftfont_list2 (f, spec, Qskiahb);
}

static Lisp_Object
skiahbfont_match (struct frame *f, Lisp_Object spec)
{
  return ftfont_match2 (f, spec, Qskiahb);
}

static hb_font_t *
skiahbfont_begin_hb_font (struct font *font, double *position_unit)
{
  struct font_info *ftfont_info = (struct font_info *) font;
  FT_Face ft_face = ftfont_info->ft_face;

  if (!ft_face)
    return NULL;

  ftfont_info->ft_size = ft_face->size;
  hb_font_t *hb_font = fthbfont_begin_hb_font (font, position_unit);
  if ((hb_version_atleast (5, 2, 0) || !hb_version_atleast (5, 0, 0))
      && ftfont_info->bitmap_position_unit)
    *position_unit = ftfont_info->bitmap_position_unit;

  return hb_font;
}

static void
skiahbfont_end_hb_font (struct font *font, hb_font_t *hb_font)
{
  struct font_info *ftfont_info = (struct font_info *) font;

  eassert (hb_font == ftfont_info->hb_font);
  hb_font_destroy (ftfont_info->hb_font);
  ftfont_info->hb_font = NULL;
  ftfont_info->ft_size = NULL;
}

# endif /* HAVE_HARFBUZZ */

# ifdef HAVE_PGTK
static bool
skiafont_cached_font_ok (struct frame *f, Lisp_Object font_object,
			 Lisp_Object entity)
{
  /* Skia manages its own font rendering settings independently of
     the system font options.  For now, always accept cached fonts.
     If Skia font rendering settings need to track gsettings changes,
     this callback should query xsettings and compare.  */
  return true;
}
# endif

static void syms_of_skiafont_for_pdumper (void);

struct font_driver const skiafont_driver = {
  .type = LISPSYM_INITIALLY (Qskia),
  .get_cache = ftfont_get_cache,
  .list = skiafont_list,
  .match = skiafont_match,
  .list_family = ftfont_list_family,
  .open_font = skiafont_open,
  .close_font = skiafont_close,
  .has_char = skiafont_has_char,
  .encode_char = skiafont_encode_char,
  .text_extents = skiafont_text_extents,
  .draw = skiafont_draw,
  .get_bitmap = skiafont_get_bitmap,
  .anchor_point = skiafont_anchor_point,
# ifdef HAVE_LIBOTF
  .otf_capability = skiafont_otf_capability,
# endif
# if defined HAVE_M17N_FLT && defined HAVE_LIBOTF
  .shape = skiafont_shape,
# endif
# if defined HAVE_OTF_GET_VARIATION_GLYPHS \
   || defined HAVE_FT_FACE_GETCHARVARIANTINDEX
  .get_variation_glyphs = skiafont_variation_glyphs,
# endif
  .filter_properties = ftfont_filter_properties,
  .combining_capability = ftfont_combining_capability,
# ifdef HAVE_PGTK
  .cached_font_ok = skiafont_cached_font_ok,
# endif
};

# ifdef HAVE_HARFBUZZ
struct font_driver skiahbfont_driver;
# endif

void
syms_of_skiafont (void)
{
  DEFSYM (Qskia, "skia");
# ifdef HAVE_HARFBUZZ
  DEFSYM (Qskiahb, "skiahb");
  Fput (Qskia, Qfont_driver_superseded_by, Qskiahb);
# endif
  pdumper_do_now_and_after_load (syms_of_skiafont_for_pdumper);
}

static void
syms_of_skiafont_for_pdumper (void)
{
  register_font_driver (&skiafont_driver, NULL);
# ifdef HAVE_HARFBUZZ
  skiahbfont_driver = skiafont_driver;
  skiahbfont_driver.type = Qskiahb;
  skiahbfont_driver.list = skiahbfont_list;
  skiahbfont_driver.match = skiahbfont_match;
  skiahbfont_driver.otf_capability = hbfont_otf_capability;
  skiahbfont_driver.shape = hbfont_shape;
  skiahbfont_driver.combining_capability
    = hbfont_combining_capability;
  skiahbfont_driver.begin_hb_font = skiahbfont_begin_hb_font;
  skiahbfont_driver.end_hb_font = skiahbfont_end_hb_font;
  register_font_driver (&skiahbfont_driver, NULL);
# endif
}

#endif /* USE_SKIA */
