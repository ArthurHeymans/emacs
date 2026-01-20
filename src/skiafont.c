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

/* This font driver uses FreeType for font discovery and metrics,
   but renders glyphs using Skia.  */

#include <config.h>

#ifdef USE_SKIA

# include <cairo-ft.h>
# include <math.h>

# include "lisp.h"
# include "blockinput.h"
# include "charset.h"
# include "composite.h"
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
  struct font_info *ftfont_info = (struct font_info *) font;
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
      /* Use Cairo for metrics since it's already available.  */
      cairo_glyph_t cr_glyph = { .index = glyph };
      cairo_text_extents_t extents;

      cairo_scaled_font_glyph_extents (ftfont_info->cr_scaled_font,
				       &cr_glyph, 1, &extents);
      cache->lbearing = floor (extents.x_bearing);
      cache->rbearing = ceil (extents.width + extents.x_bearing);
      cache->width = lround (extents.x_advance);
      cache->ascent = ceil (-extents.y_bearing - 1.0 / 256);
      cache->descent = ceil (extents.height + extents.y_bearing);
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

static Lisp_Object
skiafont_open (struct frame *f, Lisp_Object entity, int pixel_size)
{
  FcResult result;
  Lisp_Object val, filename, font_object;
  FcPattern *pat, *match;
  struct skia_font_info *skiafont_info;
  struct font *font;
  double size = 0;
  cairo_font_face_t *font_face;
  cairo_font_extents_t extents;
  FT_Face ft_face;
  FcMatrix *matrix;
  char *filename_str;

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
  ftfont_fix_match (pat, match);

  FcPatternDestroy (pat);
  font_face = cairo_ft_font_face_create_for_pattern (match);
  if (!font_face
      || cairo_font_face_status (font_face) != CAIRO_STATUS_SUCCESS)
    {
      if (font_face)
	cairo_font_face_destroy (font_face);
      FcPatternDestroy (match);
      unblock_input ();
      return Qnil;
    }

  cairo_matrix_t font_matrix, ctm;
  cairo_matrix_init_scale (&font_matrix, pixel_size, pixel_size);
  if (FcPatternGetMatrix (match, FC_MATRIX, 0, &matrix)
      == FcResultMatch)
    {
      cairo_matrix_t m;
      cairo_matrix_init (&m, matrix->xx, matrix->yx, matrix->xy,
			 matrix->yy, 0, 0);
      cairo_matrix_multiply (&font_matrix, &m, &font_matrix);
    }
  cairo_matrix_init_identity (&ctm);

  cairo_font_options_t *options = cairo_font_options_create ();
  cairo_font_options_t *gsettings_options
    = xsettings_get_font_options ();
  if (gsettings_options)
    {
      cairo_font_options_merge (options, gsettings_options);
      cairo_font_options_destroy (gsettings_options);
    }

  cairo_scaled_font_t *scaled_font
    = cairo_scaled_font_create (font_face, &font_matrix, &ctm,
				options);
  cairo_font_face_destroy (font_face);
  cairo_font_options_destroy (options);

  if (!scaled_font
      || cairo_scaled_font_status (scaled_font)
	   != CAIRO_STATUS_SUCCESS)
    {
      if (scaled_font)
	cairo_scaled_font_destroy (scaled_font);
      FcPatternDestroy (match);
      unblock_input ();
      return Qnil;
    }

  ft_face = cairo_ft_scaled_font_lock_face (scaled_font);
  if (!ft_face)
    {
      cairo_scaled_font_destroy (scaled_font);
      FcPatternDestroy (match);
      unblock_input ();
      return Qnil;
    }

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

  skiafont_info->base.cr_scaled_font = scaled_font;
  skiafont_info->base.metrics = NULL;
  skiafont_info->base.metrics_nrows = 0;
  skiafont_info->base.bitmap_position_unit = 0;

  /* Create Skia typeface from the font file.  */
  filename_str = SSDATA (filename);
  fprintf (stderr, "skiafont_open: loading font from %s, size=%g\n",
	   filename_str, size);
  skiafont_info->skia_typeface
    = emacs_skia_typeface_create_from_file (filename_str);
  fprintf (stderr, "  skia_typeface=%p\n",
	   (void *) skiafont_info->skia_typeface);
  if (skiafont_info->skia_typeface)
    {
      skiafont_info->skia_font
	= emacs_skia_font_create (skiafont_info->skia_typeface, size);
      fprintf (stderr, "  skia_font=%p\n",
	       (void *) skiafont_info->skia_font);
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

  cairo_scaled_font_extents (scaled_font, &extents);
  font->ascent = lround (extents.ascent);
  font->descent = lround (extents.descent);
  font->height = lround (extents.height);
  font->min_width = font->average_width = font->space_width
    = lround (extents.max_x_advance);

  /* Get more precise metrics from the FT_Face.  */
  if (ft_face->face_flags & FT_FACE_FLAG_SCALABLE)
    {
      font->underline_position
	= -ft_face->underline_position * size / ft_face->units_per_EM;
      font->underline_thickness
	= ft_face->underline_thickness * size / ft_face->units_per_EM;
    }
  else
    {
      font->underline_position = -1;
      font->underline_thickness = 1;
    }

  cairo_ft_scaled_font_unlock_face (scaled_font);
  FcPatternDestroy (match);
  unblock_input ();

  return font_object;
}

static void
skiafont_close (struct font *font)
{
  struct skia_font_info *skiafont_info
    = (struct skia_font_info *) font;

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

  if (skiafont_info->base.cr_scaled_font)
    {
      int i;

      block_input ();
      cairo_scaled_font_destroy (skiafont_info->base.cr_scaled_font);
      skiafont_info->base.cr_scaled_font = NULL;
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
  struct font_info *ftfont_info = (struct font_info *) font;
  cairo_scaled_font_t *scaled_font = ftfont_info->cr_scaled_font;
  FT_Face ft_face = cairo_ft_scaled_font_lock_face (scaled_font);
  unsigned code = FT_Get_Char_Index (ft_face, c);
  cairo_ft_scaled_font_unlock_face (scaled_font);
  return code;
}

static void
skiafont_text_extents (struct font *font, const unsigned int *code,
		       int nglyphs, struct font_metrics *metrics)
{
  int i, width = 0;

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

static int
skiafont_get_bitmap (struct font *font, unsigned int code,
		     struct font_bitmap *bitmap, int bits_per_pixel)
{
  struct font_info *ftfont_info = (struct font_info *) font;
  cairo_scaled_font_t *scaled_font = ftfont_info->cr_scaled_font;
  FT_Face ft_face = cairo_ft_scaled_font_lock_face (scaled_font);

  ftfont_info->ft_size = ft_face->size;
  int result = ftfont_get_bitmap (font, code, bitmap, bits_per_pixel);
  cairo_ft_scaled_font_unlock_face (scaled_font);
  ftfont_info->ft_size = NULL;

  return result;
}

static int
skiafont_anchor_point (struct font *font, unsigned int code, int idx,
		       int *x, int *y)
{
  struct font_info *ftfont_info = (struct font_info *) font;
  cairo_scaled_font_t *scaled_font = ftfont_info->cr_scaled_font;
  FT_Face ft_face = cairo_ft_scaled_font_lock_face (scaled_font);

  ftfont_info->ft_size = ft_face->size;
  int result = ftfont_anchor_point (font, code, idx, x, y);
  cairo_ft_scaled_font_unlock_face (scaled_font);
  ftfont_info->ft_size = NULL;

  return result;
}

# ifdef HAVE_LIBOTF
static Lisp_Object
skiafont_otf_capability (struct font *font)
{
  struct font_info *ftfont_info = (struct font_info *) font;
  cairo_scaled_font_t *scaled_font = ftfont_info->cr_scaled_font;
  FT_Face ft_face = cairo_ft_scaled_font_lock_face (scaled_font);

  ftfont_info->ft_size = ft_face->size;
  Lisp_Object result = ftfont_otf_capability (font);
  cairo_ft_scaled_font_unlock_face (scaled_font);
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
  cairo_scaled_font_t *scaled_font = ftfont_info->cr_scaled_font;
  FT_Face ft_face = cairo_ft_scaled_font_lock_face (scaled_font);

  ftfont_info->ft_size = ft_face->size;
  Lisp_Object result = ftfont_shape (lgstring, direction);
  cairo_ft_scaled_font_unlock_face (scaled_font);
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
  cairo_scaled_font_t *scaled_font = ftfont_info->cr_scaled_font;
  FT_Face ft_face = cairo_ft_scaled_font_lock_face (scaled_font);

  ftfont_info->ft_size = ft_face->size;
  int result = ftfont_variation_glyphs (font, c, variations);
  cairo_ft_scaled_font_unlock_face (scaled_font);
  ftfont_info->ft_size = NULL;

  return result;
}
# endif

/* Check if a cached font is still valid based on current settings. */
static bool
skiafont_cached_font_ok (struct frame *f, Lisp_Object font_object,
			 Lisp_Object entity)
{
  struct font_info *info
    = (struct font_info *) XFONT_OBJECT (font_object);

  cairo_font_options_t *options = cairo_font_options_create ();
  cairo_scaled_font_get_font_options (info->cr_scaled_font, options);
  cairo_font_options_t *gsettings_options
    = xsettings_get_font_options ();

  bool equal = cairo_font_options_equal (options, gsettings_options);
  cairo_font_options_destroy (options);
  cairo_font_options_destroy (gsettings_options);

  return equal;
}

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
  cairo_scaled_font_t *scaled_font = ftfont_info->cr_scaled_font;
  FT_Face ft_face = cairo_ft_scaled_font_lock_face (scaled_font);

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
  cairo_scaled_font_t *scaled_font = ftfont_info->cr_scaled_font;

  eassert (hb_font == ftfont_info->hb_font);
  hb_font_destroy (ftfont_info->hb_font);
  ftfont_info->hb_font = NULL;

  cairo_ft_scaled_font_unlock_face (scaled_font);
  ftfont_info->ft_size = NULL;
}

# endif /* HAVE_HARFBUZZ */

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
  .cached_font_ok = skiafont_cached_font_ok,
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
