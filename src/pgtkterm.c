/* Communication module for window systems using GTK.

Copyright (C) 1989, 1993-1994, 2005-2006, 2008-2025 Free Software
Foundation, Inc.

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

/* This should be the first include, as it may set up #defines
   affecting interpretation of even the system includes. */
#include <config.h>

#include <cairo.h>
#ifdef CAIRO_HAS_PDF_SURFACE
# include <cairo-pdf.h>
#endif
#ifdef CAIRO_HAS_PS_SURFACE
# include <cairo-ps.h>
#endif
#ifdef CAIRO_HAS_SVG_SURFACE
# include <cairo-svg.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <c-ctype.h>
#include <c-strcase.h>
#include <ftoastr.h>

#include <dlfcn.h>

#include "lisp.h"
#include "blockinput.h"
#include "ccl.h"
#include "character.h"
#include "composite.h"
#include "fontset.h"
#include "frame.h"
#include "gtkutil.h"
#include "sysselect.h"
#include "systime.h"
#include "xwidget.h"

#include "atimer.h"
#include "buffer.h"
#include "emacsgtkfixed.h"
#include "font.h"
#include "keyboard.h"
#include "menu.h"
#include "termchar.h"
#include "termhooks.h"
#include "termopts.h"
#include "window.h"
#include "xsettings.h"

#ifdef GDK_WINDOWING_WAYLAND
# include <gdk/gdkwayland.h>
#endif

#ifdef USE_SKIA
# include <epoxy/gl.h> /* For GL types and functions */
# ifdef GDK_WINDOWING_WAYLAND
#  include <epoxy/egl.h> /* For eglGetCurrentContext */
# endif
# include "skia/emacs_skia.h"

/* Convert Emacs pixel color (0xRRGGBB) to Skia color (0xAARRGGBB). */
static inline emacs_skia_color_t
pgtk_color_to_skia (unsigned long color)
{
  return EMACS_SKIA_COLOR_RGB ((color >> 16) & 0xff,
			       (color >> 8) & 0xff, color & 0xff);
}

/* Check whether the GL context for frame F is still valid after
   calling gdk_gl_context_make_current.  On Wayland, GDK uses EGL
   and eglMakeCurrent can silently fail when the compositor enters
   a degraded state (output reconfiguration, shutdown).  GDK does
   not expose this failure to callers.  If we proceed to call
   Skia's flushAndSubmit, it invokes glGetError which triggers
   Mesa's glthread to drain stale batched commands, causing a
   SIGSEGV in the driver (tc_draw_user_indices_single).

   We cannot use glGetError for detection because it triggers the
   same crash.  Instead, on Wayland we check eglGetCurrentContext
   — if eglMakeCurrent failed, the EGL spec says the previous
   context stays current or becomes EGL_NO_CONTEXT.

   On X11 (GLX), this crash path doesn't apply, so we always
   return true.  */
static bool
pgtk_gl_context_valid_p (struct frame *f)
{
#ifdef GDK_WINDOWING_WAYLAND
  GdkDisplay *dpy = gtk_widget_get_display (FRAME_GTK_WIDGET (f));
  if (GDK_IS_WAYLAND_DISPLAY (dpy))
    return eglGetCurrentContext () != EGL_NO_CONTEXT;
#endif
  return true;
}

/* Convert Emacs pixel color with explicit alpha to Skia color.  */
static inline emacs_skia_color_t
pgtk_color_to_skia_alpha (unsigned long color, uint8_t alpha)
{
  return EMACS_SKIA_COLOR (alpha, (color >> 16) & 0xff,
			   (color >> 8) & 0xff, color & 0xff);
}
#endif

#ifndef USE_SKIA
# define FRAME_CR_CONTEXT(f) ((f)->output_data.pgtk->cr_context)
# define FRAME_CR_ACTIVE_CONTEXT(f) ((f)->output_data.pgtk->cr_active)
# define FRAME_CR_SURFACE(f) (cairo_get_target (FRAME_CR_CONTEXT (f)))
#endif

/* Non-zero means that a HELP_EVENT has been generated since Emacs
   start.  */

static bool any_help_event_p;

/* Chain of existing displays */
struct pgtk_display_info *x_display_list;

struct event_queue_t
{
  union buffered_input_event *q;
  int nr, cap;
};

/* A queue of events that will be read by the read_socket_hook.  */
static struct event_queue_t event_q;

/* Non-zero timeout value means ignore next mouse click if it arrives
   before that timeout elapses (i.e. as part of the same sequence of
   events resulting from clicking on a frame to select it).  */
static Time ignore_next_mouse_click_timeout;

/* The default Emacs icon .  */
static Lisp_Object xg_default_icon_file;

/* The current GdkDragContext of a drop.  */
static GdkDragContext *current_drop_context;

/* Whether or not current_drop_context was set from a drop
   handler.  */
static bool current_drop_context_drop;

/* The time of the last drop.  */
static guint32 current_drop_time;

static void pgtk_delete_display (struct pgtk_display_info *);
static void pgtk_clear_frame_area (struct frame *, int, int, int,
				   int);
static void pgtk_fill_rectangle (struct frame *, unsigned long, int,
				 int, int, int, bool);
#ifdef USE_SKIA
static void pgtk_skia_fill_rectangle (struct frame *, unsigned long,
				      int, int, int, int, bool);
static void pgtk_skia_draw_rectangle (struct frame *, unsigned long,
				      int, int, int, int, bool);
static void pgtk_skia_clip_to_row (struct window *,
				   struct glyph_row *,
				   enum glyph_row_area,
				   emacs_skia_canvas_t *);
static void pgtk_skia_set_clip_rectangles (struct frame *,
					   emacs_skia_canvas_t *,
					   XRectangle *, int);
static void
pgtk_skia_set_glyph_string_clipping (struct glyph_string *,
				     emacs_skia_canvas_t *);
static void
pgtk_skia_set_glyph_string_clipping_exactly (struct glyph_string *,
					     struct glyph_string *,
					     emacs_skia_canvas_t *);
#else
static void pgtk_clip_to_row (struct window *, struct glyph_row *,
			      enum glyph_row_area, cairo_t *);
#endif
static struct frame *pgtk_any_window_to_frame (GdkWindow *);
static void pgtk_regenerate_devices (struct pgtk_display_info *);

static void
pgtk_device_added_or_removal_cb (GdkSeat *seat, GdkDevice *device,
				 gpointer user_data)
{
  pgtk_regenerate_devices (user_data);
}

static void
pgtk_seat_added_cb (GdkDisplay *dpy, GdkSeat *seat,
		    gpointer user_data)
{
  pgtk_regenerate_devices (user_data);

  g_signal_connect (G_OBJECT (seat), "device-added",
		    G_CALLBACK (pgtk_device_added_or_removal_cb),
		    user_data);
  g_signal_connect (G_OBJECT (seat), "device-removed",
		    G_CALLBACK (pgtk_device_added_or_removal_cb),
		    user_data);
}

static void
pgtk_seat_removed_cb (GdkDisplay *dpy, GdkSeat *seat,
		      gpointer user_data)
{
  pgtk_regenerate_devices (user_data);

  g_signal_handlers_disconnect_by_func (
    G_OBJECT (seat), G_CALLBACK (pgtk_device_added_or_removal_cb),
    user_data);
}

static void
pgtk_enumerate_devices (struct pgtk_display_info *dpyinfo,
			bool initial_p)
{
  struct pgtk_device_t *rec;
  GList *all_seats, *devices_on_seat, *tem, *t1;
  GdkSeat *seat;
  char printbuf[1026]; /* Believe it or not, some device names are
			  actually almost this long.  */

  block_input ();
  all_seats = gdk_display_list_seats (dpyinfo->gdpy);

  for (tem = all_seats; tem; tem = tem->next)
    {
      seat = GDK_SEAT (tem->data);

      if (initial_p)
	{
	  g_signal_connect (G_OBJECT (seat), "device-added",
			    G_CALLBACK (
			      pgtk_device_added_or_removal_cb),
			    dpyinfo);
	  g_signal_connect (G_OBJECT (seat), "device-removed",
			    G_CALLBACK (
			      pgtk_device_added_or_removal_cb),
			    dpyinfo);
	}

      /* We only want slaves, not master devices.  */
      devices_on_seat
	= gdk_seat_get_slaves (seat, GDK_SEAT_CAPABILITY_ALL);

      for (t1 = devices_on_seat; t1; t1 = t1->next)
	{
	  rec = xmalloc (sizeof *rec);
	  rec->seat = g_object_ref (seat);

	  if (t1->data)
	    {
	      rec->device = GDK_DEVICE (t1->data);
	      snprintf (printbuf, 1026, "%u:%s",
			gdk_device_get_source (rec->device),
			gdk_device_get_name (rec->device));

	      rec->name = build_string (printbuf);
	    }
	  else
	    {
	      /* GTK bug 7737 results in GDK seats being initialized
		 with NULL devices in some cirumstances.  As events
		 will presumably also be delivered with their device
		 fields set to NULL, insert a ersatz device record
		 associated with NULL.  (bug#76239) */
	      rec->device = NULL;
	      rec->name = build_string ("0:unknown device");
	    }

	  rec->next = dpyinfo->devices;
	  dpyinfo->devices = rec;
	}

      g_list_free (devices_on_seat);
    }

  g_list_free (all_seats);
  unblock_input ();
}

static void
pgtk_free_devices (struct pgtk_display_info *dpyinfo)
{
  struct pgtk_device_t *last, *tem;

  tem = dpyinfo->devices;
  while (tem)
    {
      last = tem;
      tem = tem->next;

      g_object_unref (last->seat);
      xfree (last);
    }

  dpyinfo->devices = NULL;
}

static void
pgtk_regenerate_devices (struct pgtk_display_info *dpyinfo)
{
  pgtk_free_devices (dpyinfo);
  pgtk_enumerate_devices (dpyinfo, false);
}

static Lisp_Object
pgtk_get_device_for_event (struct pgtk_display_info *dpyinfo,
			   GdkEvent *event)
{
  struct pgtk_device_t *tem;
  GdkDevice *device;

  device = gdk_event_get_source_device (event);

  if (!device)
    return Qt;

  for (tem = dpyinfo->devices; tem; tem = tem->next)
    {
      if (tem->device == device)
	return tem->name;
    }

  return Qt;
}

/* This is not a flip context in the same sense as gpu rendering
   scenes, it only occurs when a new context was required due to a
   resize or other fundamental change.  This is called when that
   context's surface has completed drawing.  */

#ifndef USE_SKIA
static void
flip_cr_context (struct frame *f)
{
  cairo_t *cr = FRAME_CR_ACTIVE_CONTEXT (f);

  block_input ();
  if (cr != FRAME_CR_CONTEXT (f))
    {
      cairo_destroy (cr);

      FRAME_CR_ACTIVE_CONTEXT (f)
	= cairo_reference (FRAME_CR_CONTEXT (f));
    }
  unblock_input ();
}
#endif

static void
evq_enqueue (union buffered_input_event *ev)
{
  struct event_queue_t *evq = &event_q;
  struct frame *frame;
  struct pgtk_display_info *dpyinfo;

  if (evq->cap == 0)
    {
      evq->cap = 4;
      evq->q = xmalloc (sizeof *evq->q * evq->cap);
    }

  if (evq->nr >= evq->cap)
    {
      evq->cap += evq->cap / 2;
      evq->q = xrealloc (evq->q, sizeof *evq->q * evq->cap);
    }

  evq->q[evq->nr++] = *ev;

  if (ev->ie.kind != SELECTION_REQUEST_EVENT
      && ev->ie.kind != SELECTION_CLEAR_EVENT)
    {
      frame = NULL;

      if (WINDOWP (ev->ie.frame_or_window))
	frame = WINDOW_XFRAME (XWINDOW (ev->ie.frame_or_window));

      if (FRAMEP (ev->ie.frame_or_window))
	frame = XFRAME (ev->ie.frame_or_window);

      if (frame)
	{
	  dpyinfo = FRAME_DISPLAY_INFO (frame);

	  if (dpyinfo->last_user_time < ev->ie.timestamp)
	    dpyinfo->last_user_time = ev->ie.timestamp;
	}
    }

  raise (SIGIO);
}

static int
evq_flush (struct input_event *hold_quit)
{
  struct event_queue_t *evq = &event_q;
  int n = 0;

  while (evq->nr > 0)
    {
      /* kbd_buffer_store_buffered_event may do longjmp, so
	 we need to shift event queue first and pass the event
	 to kbd_buffer_store_buffered_event so that events in
	 queue are not processed twice.  Bug#52941 */
      union buffered_input_event ev = evq->q[0];
      int i;
      for (i = 1; i < evq->nr; i++)
	evq->q[i - 1] = evq->q[i];
      evq->nr--;

      kbd_buffer_store_buffered_event (&ev, hold_quit);
      n++;
    }

  return n;
}

void
mark_pgtkterm (void)
{
  struct pgtk_display_info *dpyinfo;
  struct pgtk_device_t *device;
  struct event_queue_t *evq = &event_q;
  int i, n = evq->nr;

  for (i = 0; i < n; i++)
    {
      union buffered_input_event *ev = &evq->q[i];

      /* Selection requests don't have Lisp object members.  */

      if (ev->ie.kind == SELECTION_REQUEST_EVENT
	  || ev->ie.kind == SELECTION_CLEAR_EVENT)
	continue;

      mark_object (ev->ie.x);
      mark_object (ev->ie.y);
      mark_object (ev->ie.frame_or_window);
      mark_object (ev->ie.arg);
      mark_object (ev->ie.device);
    }

  for (dpyinfo = x_display_list; dpyinfo; dpyinfo = dpyinfo->next)
    {
      for (device = dpyinfo->devices; device; device = device->next)
	mark_object (device->name);
    }
}

char *
get_keysym_name (int keysym)
{
  return gdk_keyval_name (keysym);
}

void
frame_set_mouse_pixel_position (struct frame *f, int pix_x, int pix_y)
/* --------------------------------------------------------------------------
     Programmatically reposition mouse pointer in pixel coordinates
   --------------------------------------------------------------------------
 */
{
}

/* Raise frame F.  */

static void
pgtk_raise_frame (struct frame *f)
{
  /* This works only for non-child frames on X.
     It does not work for child frames on X, and it does not work
     on Wayland too. */
  block_input ();
  if (FRAME_VISIBLE_P (f))
    gdk_window_raise (gtk_widget_get_window (FRAME_WIDGET (f)));
  unblock_input ();
}

/* Lower frame F.  */

static void
pgtk_lower_frame (struct frame *f)
{
  if (FRAME_VISIBLE_P (f))
    {
      block_input ();
      gdk_window_lower (gtk_widget_get_window (FRAME_WIDGET (f)));
      unblock_input ();
    }
}

static void
pgtk_frame_raise_lower (struct frame *f, bool raise_flag)
{
  if (raise_flag)
    pgtk_raise_frame (f);
  else
    pgtk_lower_frame (f);
}

/* Free X resources of frame F.  */

static void pgtk_unlink_touch_points (struct frame *);

void
pgtk_free_frame_resources (struct frame *f)
{
  struct pgtk_display_info *dpyinfo;
  Mouse_HLInfo *hlinfo;

  check_window_system (f);
  dpyinfo = FRAME_DISPLAY_INFO (f);
  hlinfo = MOUSE_HL_INFO (f);

  block_input ();

  pgtk_unlink_touch_points (f);
#ifdef HAVE_XWIDGETS
  kill_frame_xwidget_views (f);
#endif
  free_frame_faces (f);

  if (FRAME_X_OUTPUT (f)->scale_factor_atimer != NULL)
    {
      cancel_atimer (FRAME_X_OUTPUT (f)->scale_factor_atimer);
      FRAME_X_OUTPUT (f)->scale_factor_atimer = NULL;
    }

#define CLEAR_IF_EQ(FIELD)     \
  do                           \
    {                          \
      if (f == dpyinfo->FIELD) \
	dpyinfo->FIELD = 0;    \
    }                          \
  while (false)

  CLEAR_IF_EQ (x_focus_frame);
  CLEAR_IF_EQ (highlight_frame);
  CLEAR_IF_EQ (x_focus_event_frame);
  CLEAR_IF_EQ (last_mouse_frame);
  CLEAR_IF_EQ (last_mouse_motion_frame);
  CLEAR_IF_EQ (last_mouse_glyph_frame);
  CLEAR_IF_EQ (im.focused_frame);

#undef CLEAR_IF_EQ

  if (f == hlinfo->mouse_face_mouse_frame)
    reset_mouse_highlight (hlinfo);

  g_clear_object (&FRAME_X_OUTPUT (f)->text_cursor);
  g_clear_object (&FRAME_X_OUTPUT (f)->nontext_cursor);
  g_clear_object (&FRAME_X_OUTPUT (f)->modeline_cursor);
  g_clear_object (&FRAME_X_OUTPUT (f)->hand_cursor);
  g_clear_object (&FRAME_X_OUTPUT (f)->hourglass_cursor);
  g_clear_object (&FRAME_X_OUTPUT (f)->horizontal_drag_cursor);
  g_clear_object (&FRAME_X_OUTPUT (f)->vertical_drag_cursor);
  g_clear_object (&FRAME_X_OUTPUT (f)->left_edge_cursor);
  g_clear_object (&FRAME_X_OUTPUT (f)->right_edge_cursor);
  g_clear_object (&FRAME_X_OUTPUT (f)->top_edge_cursor);
  g_clear_object (&FRAME_X_OUTPUT (f)->bottom_edge_cursor);
  g_clear_object (&FRAME_X_OUTPUT (f)->top_left_corner_cursor);
  g_clear_object (&FRAME_X_OUTPUT (f)->top_right_corner_cursor);
  g_clear_object (&FRAME_X_OUTPUT (f)->bottom_right_corner_cursor);
  g_clear_object (&FRAME_X_OUTPUT (f)->bottom_left_corner_cursor);

  if (FRAME_X_OUTPUT (f)->border_color_css_provider != NULL)
    {
      GtkStyleContext *ctxt
	= gtk_widget_get_style_context (FRAME_WIDGET (f));
      GtkCssProvider *old
	= FRAME_X_OUTPUT (f)->border_color_css_provider;
      gtk_style_context_remove_provider (ctxt,
					 GTK_STYLE_PROVIDER (old));
      g_object_unref (old);
      FRAME_X_OUTPUT (f)->border_color_css_provider = NULL;
    }

  if (FRAME_X_OUTPUT (f)->scrollbar_foreground_css_provider != NULL)
    {
      GtkCssProvider *old
	= FRAME_X_OUTPUT (f)->scrollbar_foreground_css_provider;
      g_object_unref (old);
      FRAME_X_OUTPUT (f)->scrollbar_foreground_css_provider = NULL;
    }

  if (FRAME_X_OUTPUT (f)->scrollbar_background_css_provider != NULL)
    {
      GtkCssProvider *old
	= FRAME_X_OUTPUT (f)->scrollbar_background_css_provider;
      g_object_unref (old);
      FRAME_X_OUTPUT (f)->scrollbar_background_css_provider = NULL;
    }

  gtk_widget_destroy (FRAME_WIDGET (f));

#ifdef USE_SKIA
  if (FRAME_X_OUTPUT (f)->skia_image_pre_bell != NULL)
    {
      emacs_skia_image_destroy (FRAME_X_OUTPUT (f)->skia_image_pre_bell);
      FRAME_X_OUTPUT (f)->skia_image_pre_bell = NULL;
    }
#else
  if (FRAME_X_OUTPUT (f)->cr_surface_visible_bell != NULL)
    {
      cairo_surface_destroy (
	FRAME_X_OUTPUT (f)->cr_surface_visible_bell);
      FRAME_X_OUTPUT (f)->cr_surface_visible_bell = NULL;
    }
#endif

  if (FRAME_X_OUTPUT (f)->atimer_visible_bell != NULL)
    {
      cancel_atimer (FRAME_X_OUTPUT (f)->atimer_visible_bell);
      FRAME_X_OUTPUT (f)->atimer_visible_bell = NULL;
    }

  xfree (f->output_data.pgtk);
  f->output_data.pgtk = NULL;

  unblock_input ();
}

void
pgtk_destroy_window (struct frame *f)
/* --------------------------------------------------------------------------
     External: Delete the window
   --------------------------------------------------------------------------
 */
{
  struct pgtk_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);

  check_window_system (f);
  if (dpyinfo->gdpy != NULL)
    pgtk_free_frame_resources (f);

  dpyinfo->reference_count--;
}

/* Calculate the absolute position in frame F
   from its current recorded position values and gravity.  */

static void
pgtk_calc_absolute_position (struct frame *f)
{
  int flags = f->size_hint_flags;
  struct frame *p = FRAME_PARENT_FRAME (f);

  /* We have nothing to do if the current position
     is already for the top-left corner.  */
  if (!((flags & XNegative) || (flags & YNegative)))
    return;

  /* Treat negative positions as relative to the leftmost bottommost
     position that fits on the screen.  */
  if ((flags & XNegative) && (f->left_pos <= 0))
    {
      int width = FRAME_PIXEL_WIDTH (f);

      /* A frame that has been visible at least once should have outer
	 edges.  */
      if (f->output_data.pgtk->has_been_visible && !p)
	{
	  Lisp_Object frame;
	  Lisp_Object edges = Qnil;

	  XSETFRAME (frame, f);
	  edges = Fpgtk_frame_edges (frame, Qouter_edges);
	  if (!NILP (edges))
	    width = (XFIXNUM (Fnth (make_fixnum (2), edges))
		     - XFIXNUM (Fnth (make_fixnum (0), edges)));
	}

      if (p)
	f->left_pos = (FRAME_PIXEL_WIDTH (p) - width
		       - 2 * f->border_width + f->left_pos);
      else
	f->left_pos
	  = (pgtk_display_pixel_width (FRAME_DISPLAY_INFO (f)) - width
	     + f->left_pos);
    }

  if ((flags & YNegative) && (f->top_pos <= 0))
    {
      int height = FRAME_PIXEL_HEIGHT (f);

      if (f->output_data.pgtk->has_been_visible && !p)
	{
	  Lisp_Object frame;
	  Lisp_Object edges = Qnil;

	  XSETFRAME (frame, f);
	  if (NILP (edges))
	    edges = Fpgtk_frame_edges (frame, Qouter_edges);
	  if (!NILP (edges))
	    height = (XFIXNUM (Fnth (make_fixnum (3), edges))
		      - XFIXNUM (Fnth (make_fixnum (1), edges)));
	}

      if (p)
	f->top_pos = (FRAME_PIXEL_HEIGHT (p) - height
		      - 2 * f->border_width + f->top_pos);
      else
	f->top_pos
	  = (pgtk_display_pixel_height (FRAME_DISPLAY_INFO (f))
	     - height + f->top_pos);
    }

  /* The left_pos and top_pos
     are now relative to the top and left screen edges,
     so the flags should correspond.  */
  f->size_hint_flags &= ~(XNegative | YNegative);
}

/* CHANGE_GRAVITY is 1 when calling from Fset_frame_position,
   to really change the position, and 0 when calling from
   x_make_frame_visible (in that case, XOFF and YOFF are the current
   position values).  It is -1 when calling from
   x_set_frame_parameters, which means, do adjust for borders but
   don't change the gravity.  */

static void
pgtk_set_offset (struct frame *f, int xoff, int yoff,
		 int change_gravity)
{
  if (change_gravity > 0)
    {
      f->top_pos = yoff;
      f->left_pos = xoff;
      f->size_hint_flags &= ~(XNegative | YNegative);
      if (xoff < 0)
	f->size_hint_flags |= XNegative;
      if (yoff < 0)
	f->size_hint_flags |= YNegative;
      f->win_gravity = NorthWestGravity;
    }

  pgtk_calc_absolute_position (f);

  block_input ();
  xg_wm_set_size_hint (f, 0, false);

  if (change_gravity != 0)
    {
      if (FRAME_GTK_OUTER_WIDGET (f))
	gtk_window_move (GTK_WINDOW (FRAME_GTK_OUTER_WIDGET (f)),
			 f->left_pos, f->top_pos);
      else
	{
	  GtkWidget *fixed = FRAME_GTK_WIDGET (f);
	  GtkWidget *parent = gtk_widget_get_parent (fixed);
	  gtk_fixed_move (GTK_FIXED (parent), fixed, f->left_pos,
			  f->top_pos);
	}
    }
  unblock_input ();
  return;
}

static void
pgtk_set_window_size (struct frame *f, bool change_gravity, int width,
		      int height)
/* --------------------------------------------------------------------------
     Adjust window pixel size based on given character grid size
     Impl is a bit more complex than other terms, need to do some
     internal clipping.
   --------------------------------------------------------------------------
 */
{
  int pixelwidth, pixelheight;

  block_input ();

  gtk_widget_get_size_request (FRAME_GTK_WIDGET (f), &pixelwidth,
			       &pixelheight);

  pixelwidth = width;
  pixelheight = height;

  for (GtkWidget *w = FRAME_GTK_WIDGET (f); w != NULL;
       w = gtk_widget_get_parent (w))
    {
      gint wd, hi;
      gtk_widget_get_size_request (w, &wd, &hi);
    }

  f->output_data.pgtk->preferred_width = pixelwidth;
  f->output_data.pgtk->preferred_height = pixelheight;
  xg_wm_set_size_hint (f, 0, 0);
  xg_frame_set_char_size (f, pixelwidth, pixelheight);
  gtk_widget_queue_resize (FRAME_WIDGET (f));

  unblock_input ();
}

void
pgtk_iconify_frame (struct frame *f)
{
  GtkWindow *window;

  /* Don't keep the highlight on an invisible frame.  */

  if (FRAME_DISPLAY_INFO (f)->highlight_frame == f)
    FRAME_DISPLAY_INFO (f)->highlight_frame = NULL;

  /* If the frame is already iconified, return.  */

  if (FRAME_ICONIFIED_P (f))
    return;

  /* Child frames on PGTK have no outer widgets.  In that case, simply
     refuse to iconify the frame.  */

  if (FRAME_GTK_OUTER_WIDGET (f))
    {
      if (!FRAME_VISIBLE_P (f))
	gtk_widget_show_all (FRAME_GTK_OUTER_WIDGET (f));

      window = GTK_WINDOW (FRAME_GTK_OUTER_WIDGET (f));

      gtk_window_iconify (window);

      /* Don't make the frame iconified here.  Doing so will cause it
	 to be skipped by redisplay, until GDK says it is deiconified
	 (see window_state_event for more details).  However, if the
	 window server rejects the iconification request, GDK will
	 never tell Emacs about the iconification not happening,
	 leading to the frame not being redisplayed until the next
	 window state change.  */

      /* SET_FRAME_VISIBLE (f, 0);
	 SET_FRAME_ICONIFIED (f, true); */
    }
}

static gboolean
pgtk_make_frame_visible_wait_for_map_event_cb (GtkWidget *widget,
					       GdkEventAny *event,
					       gpointer user_data)
{
  int *foundptr = user_data;
  *foundptr = 1;
  return FALSE;
}

static gboolean
pgtk_make_frame_visible_wait_for_map_event_timeout (
  gpointer user_data)
{
  int *timedoutptr = user_data;
  *timedoutptr = 1;
  return FALSE;
}

static void
pgtk_wait_for_map_event (struct frame *f, bool multiple_times)
{
  if (FLOATP (Vpgtk_wait_for_event_timeout))
    {
      guint msec
	= (guint) (XFLOAT_DATA (Vpgtk_wait_for_event_timeout) * 1000);
      int found = 0;
      int timed_out = 0;
      gulong id = g_signal_connect (
	FRAME_WIDGET (f), "map-event",
	G_CALLBACK (pgtk_make_frame_visible_wait_for_map_event_cb),
	&found);
      guint src = g_timeout_add (
	msec, pgtk_make_frame_visible_wait_for_map_event_timeout,
	&timed_out);

      if (!multiple_times)
	{
	  while (!found && !timed_out)
	    gtk_main_iteration ();
	}
      else
	{
	  while (!timed_out)
	    gtk_main_iteration ();
	}

      g_signal_handler_disconnect (FRAME_WIDGET (f), id);

      if (!timed_out)
	g_source_remove (src);
    }
}

void
pgtk_make_frame_visible (struct frame *f)
{
  GtkWidget *win = FRAME_GTK_OUTER_WIDGET (f);

  if (!FRAME_VISIBLE_P (f))
    {
      gtk_widget_show (FRAME_WIDGET (f));
      if (win)
	gtk_window_deiconify (GTK_WINDOW (win));

      pgtk_wait_for_map_event (f, false);
    }
}

void
pgtk_make_frame_invisible (struct frame *f)
{
  gtk_widget_hide (FRAME_WIDGET (f));

  /* Handle any pending map event(s), then make the frame visible
     manually, to avoid race conditions.  */
  pgtk_wait_for_map_event (f, true);

  SET_FRAME_VISIBLE (f, 0);
  SET_FRAME_ICONIFIED (f, false);
}

static void
pgtk_make_frame_visible_invisible (struct frame *f, bool visible)
{
  if (visible)
    pgtk_make_frame_visible (f);
  else
    pgtk_make_frame_invisible (f);
}

static Lisp_Object
pgtk_new_font (struct frame *f, Lisp_Object font_object, int fontset)
{
  struct font *font = XFONT_OBJECT (font_object);
  int font_ascent, font_descent;

  if (fontset < 0)
    fontset = fontset_from_font (font_object);
  FRAME_FONTSET (f) = fontset;

  if (FRAME_FONT (f) == font)
    {
      /* This font is already set in frame F.  There's nothing more to
	 do.  */
      return font_object;
    }

  FRAME_FONT (f) = font;

  FRAME_BASELINE_OFFSET (f) = font->baseline_offset;
  FRAME_COLUMN_WIDTH (f) = font->average_width;
  get_font_ascent_descent (font, &font_ascent, &font_descent);
  FRAME_LINE_HEIGHT (f) = font_ascent + font_descent;

  /* We could use a more elaborate calculation here.  */
  FRAME_TAB_BAR_HEIGHT (f)
    = FRAME_TAB_BAR_LINES (f) * FRAME_LINE_HEIGHT (f);

  /* Compute the scroll bar width in character columns.  */
  if (FRAME_CONFIG_SCROLL_BAR_WIDTH (f) > 0)
    {
      int wid = FRAME_COLUMN_WIDTH (f);
      FRAME_CONFIG_SCROLL_BAR_COLS (f)
	= (FRAME_CONFIG_SCROLL_BAR_WIDTH (f) + wid - 1) / wid;
    }
  else
    {
      int wid = FRAME_COLUMN_WIDTH (f);
      FRAME_CONFIG_SCROLL_BAR_COLS (f) = (14 + wid - 1) / wid;
    }

  /* Compute the scroll bar height in character lines.  */
  if (FRAME_CONFIG_SCROLL_BAR_HEIGHT (f) > 0)
    {
      int height = FRAME_LINE_HEIGHT (f);
      FRAME_CONFIG_SCROLL_BAR_LINES (f)
	= (FRAME_CONFIG_SCROLL_BAR_HEIGHT (f) + height - 1) / height;
    }
  else
    {
      int height = FRAME_LINE_HEIGHT (f);
      FRAME_CONFIG_SCROLL_BAR_LINES (f) = (14 + height - 1) / height;
    }

  /* Now make the frame display the given font.  */
  if (FRAME_GTK_WIDGET (f) != NULL)
    adjust_frame_size (f, FRAME_COLS (f) * FRAME_COLUMN_WIDTH (f),
		       FRAME_LINES (f) * FRAME_LINE_HEIGHT (f), 3,
		       false, Qfont);

  return font_object;
}

int
pgtk_display_pixel_height (struct pgtk_display_info *dpyinfo)
{
  GdkDisplay *gdpy = dpyinfo->gdpy;
  GdkScreen *gscr = gdk_display_get_default_screen (gdpy);

  return gdk_screen_get_height (gscr);
}

int
pgtk_display_pixel_width (struct pgtk_display_info *dpyinfo)
{
  GdkDisplay *gdpy = dpyinfo->gdpy;
  GdkScreen *gscr = gdk_display_get_default_screen (gdpy);

  return gdk_screen_get_width (gscr);
}

void
pgtk_set_parent_frame (struct frame *f, Lisp_Object new_value,
		       Lisp_Object old_value)
{
  struct frame *p = NULL;

  if (!NILP (new_value)
      && (!FRAMEP (new_value)
	  || !FRAME_LIVE_P (p = XFRAME (new_value))
	  || !FRAME_PGTK_P (p)))
    {
      store_frame_param (f, Qparent_frame, old_value);
      error ("Invalid specification of `parent-frame'");
    }

  if (p != FRAME_PARENT_FRAME (f))
    {
      block_input ();

      if (p != NULL)
	{
	  if (FRAME_DISPLAY_INFO (f) != FRAME_DISPLAY_INFO (p))
	    error ("Cross display reparent.");
	}

      GtkWidget *fixed = FRAME_GTK_WIDGET (f);

      GtkAllocation alloc;
      gtk_widget_get_allocation (fixed, &alloc);
      g_object_ref (fixed);

      /* Remember the css provider, and restore it later. */
      GtkCssProvider *provider
	= FRAME_X_OUTPUT (f)->border_color_css_provider;
      FRAME_X_OUTPUT (f)->border_color_css_provider = NULL;
      {
	GtkStyleContext *ctxt
	  = gtk_widget_get_style_context (FRAME_WIDGET (f));
	if (provider != NULL)
	  gtk_style_context_remove_provider (ctxt,
					     GTK_STYLE_PROVIDER (
					       provider));
      }

      {
	GtkWidget *whbox_of_f = gtk_widget_get_parent (fixed);
	/* Here, unhighlight can be called and may change
	   border_color_css_provider.  */
	gtk_container_remove (GTK_CONTAINER (whbox_of_f), fixed);

	if (FRAME_GTK_OUTER_WIDGET (f))
	  {
	    gtk_widget_destroy (FRAME_GTK_OUTER_WIDGET (f));
	    FRAME_GTK_OUTER_WIDGET (f) = NULL;
	    FRAME_OUTPUT_DATA (f)->vbox_widget = NULL;
	    FRAME_OUTPUT_DATA (f)->hbox_widget = NULL;
	    FRAME_OUTPUT_DATA (f)->menubar_widget = NULL;
	    FRAME_OUTPUT_DATA (f)->toolbar_widget = NULL;
	    FRAME_OUTPUT_DATA (f)->ttip_widget = NULL;
	    FRAME_OUTPUT_DATA (f)->ttip_lbl = NULL;
	    FRAME_OUTPUT_DATA (f)->ttip_window = NULL;
	  }
      }

      if (p == NULL)
	{
	  xg_create_frame_outer_widgets (f);
	  pgtk_set_event_handler (f);
	  gtk_box_pack_start (GTK_BOX (
				f->output_data.pgtk->hbox_widget),
			      fixed, TRUE, TRUE, 0);
	  f->output_data.pgtk->preferred_width = alloc.width;
	  f->output_data.pgtk->preferred_height = alloc.height;
	  xg_wm_set_size_hint (f, 0, 0);
	  xg_frame_set_char_size (
	    f, FRAME_PIXEL_TO_TEXT_WIDTH (f, alloc.width),
	    FRAME_PIXEL_TO_TEXT_HEIGHT (f, alloc.height));
	  gtk_widget_queue_resize (FRAME_WIDGET (f));
	  gtk_widget_show_all (FRAME_GTK_OUTER_WIDGET (f));
	}
      else
	{
	  GtkWidget *fixed_of_p = FRAME_GTK_WIDGET (p);
	  gtk_fixed_put (GTK_FIXED (fixed_of_p), fixed, f->left_pos,
			 f->top_pos);
	  gtk_widget_set_size_request (fixed, alloc.width,
				       alloc.height);
	  gtk_widget_show_all (fixed);
	}

      /* Restore css provider. */
      GtkStyleContext *ctxt
	= gtk_widget_get_style_context (FRAME_WIDGET (f));
      GtkCssProvider *old
	= FRAME_X_OUTPUT (f)->border_color_css_provider;
      FRAME_X_OUTPUT (f)->border_color_css_provider = provider;
      if (provider != NULL)
	{
	  gtk_style_context_add_provider (
	    ctxt, GTK_STYLE_PROVIDER (provider),
	    GTK_STYLE_PROVIDER_PRIORITY_USER);
	}
      if (old != NULL)
	{
	  gtk_style_context_remove_provider (ctxt,
					     GTK_STYLE_PROVIDER (
					       old));
	  g_object_unref (old);
	}

      g_object_unref (fixed);

      unblock_input ();

      fset_parent_frame (f, new_value);
    }
}

/* Doesn't work on wayland.  */
void
pgtk_set_no_focus_on_map (struct frame *f, Lisp_Object new_value,
			  Lisp_Object old_value)
{
  if (!EQ (new_value, old_value))
    {
      xg_set_no_focus_on_map (f, new_value);
      FRAME_NO_FOCUS_ON_MAP (f) = !NILP (new_value);
    }
}

void
pgtk_set_no_accept_focus (struct frame *f, Lisp_Object new_value,
			  Lisp_Object old_value)
{
  xg_set_no_accept_focus (f, new_value);
  FRAME_NO_ACCEPT_FOCUS (f) = !NILP (new_value);
}

void
pgtk_set_z_group (struct frame *f, Lisp_Object new_value,
		  Lisp_Object old_value)
{
  if (!FRAME_GTK_OUTER_WIDGET (f))
    return;

  if (NILP (new_value))
    {
      gtk_window_set_keep_above (GTK_WINDOW (
				   FRAME_GTK_OUTER_WIDGET (f)),
				 FALSE);
      gtk_window_set_keep_below (GTK_WINDOW (
				   FRAME_GTK_OUTER_WIDGET (f)),
				 FALSE);
      FRAME_Z_GROUP (f) = z_group_none;
    }
  else if (EQ (new_value, Qabove))
    {
      gtk_window_set_keep_above (GTK_WINDOW (
				   FRAME_GTK_OUTER_WIDGET (f)),
				 TRUE);
      gtk_window_set_keep_below (GTK_WINDOW (
				   FRAME_GTK_OUTER_WIDGET (f)),
				 FALSE);
      FRAME_Z_GROUP (f) = z_group_above;
    }
  else if (EQ (new_value, Qabove_suspended))
    {
      gtk_window_set_keep_above (GTK_WINDOW (
				   FRAME_GTK_OUTER_WIDGET (f)),
				 FALSE);
      FRAME_Z_GROUP (f) = z_group_above_suspended;
    }
  else if (EQ (new_value, Qbelow))
    {
      gtk_window_set_keep_above (GTK_WINDOW (
				   FRAME_GTK_OUTER_WIDGET (f)),
				 FALSE);
      gtk_window_set_keep_below (GTK_WINDOW (
				   FRAME_GTK_OUTER_WIDGET (f)),
				 TRUE);
      FRAME_Z_GROUP (f) = z_group_below;
    }
  else
    error ("Invalid z-group specification");
}

static void
pgtk_initialize_display_info (struct pgtk_display_info *dpyinfo)
/* --------------------------------------------------------------------------
      Initialize global info and storage for display.
   --------------------------------------------------------------------------
 */
{
  dpyinfo->resx = 96;
  dpyinfo->resy = 96;
  dpyinfo->color_p = 1;
  dpyinfo->n_planes = 32;
  dpyinfo->root_window = 42; /* a placeholder.. */
  dpyinfo->highlight_frame = dpyinfo->x_focus_frame = NULL;
  dpyinfo->n_fonts = 0;
  dpyinfo->smallest_font_height = 1;
  dpyinfo->smallest_char_width = 1;

  reset_mouse_highlight (&dpyinfo->mouse_highlight);
}

/* Set S->gc to a suitable GC for drawing glyph string S in cursor
   face.  */

static void
pgtk_set_cursor_gc (struct glyph_string *s)
{
  if (s->font == FRAME_FONT (s->f)
      && s->face->background == FRAME_BACKGROUND_PIXEL (s->f)
      && s->face->foreground == FRAME_FOREGROUND_PIXEL (s->f)
      && !s->cmp)
    s->xgcv = FRAME_X_OUTPUT (s->f)->cursor_xgcv;
  else
    {
      /* Cursor on non-default face: must merge.  */
      Emacs_GC xgcv;

      xgcv.background = FRAME_X_OUTPUT (s->f)->cursor_color;
      xgcv.foreground = s->face->background;

      /* If the glyph would be invisible, try a different foreground.
       */
      if (xgcv.foreground == xgcv.background)
	xgcv.foreground = s->face->foreground;
      if (xgcv.foreground == xgcv.background)
	xgcv.foreground
	  = FRAME_X_OUTPUT (s->f)->cursor_foreground_color;
      if (xgcv.foreground == xgcv.background)
	xgcv.foreground = s->face->foreground;

      /* Make sure the cursor is distinct from text in this face.  */
      if (xgcv.background == s->face->background
	  && xgcv.foreground == s->face->foreground)
	{
	  xgcv.background = s->face->foreground;
	  xgcv.foreground = s->face->background;
	}

      s->xgcv = xgcv;
    }
}

/* Set up S->gc of glyph string S for drawing text in mouse face.  */

static void
pgtk_set_mouse_face_gc (struct glyph_string *s)
{
  prepare_face_for_display (s->f, s->face);

  if (s->font == s->face->font)
    {
      s->xgcv.foreground = s->face->foreground;
      s->xgcv.background = s->face->background;
    }
  else
    {
      /* Otherwise construct scratch_cursor_gc with values from FACE
	 except for FONT.  */
      Emacs_GC xgcv;

      xgcv.background = s->face->background;
      xgcv.foreground = s->face->foreground;

      s->xgcv = xgcv;
    }
}

/* Set S->gc of glyph string S to a GC suitable for drawing a mode
   line. Faces to use in the mode line have already been computed when
   the matrix was built, so there isn't much to do, here.  */

static void
pgtk_set_mode_line_face_gc (struct glyph_string *s)
{
  s->xgcv.foreground = s->face->foreground;
  s->xgcv.background = s->face->background;
}

/* Set S->gc of glyph string S for drawing that glyph string.  Set
   S->stippled_p to a non-zero value if the face of S has a stipple
   pattern.  */

static void
pgtk_set_glyph_string_gc (struct glyph_string *s)
{
  prepare_face_for_display (s->f, s->face);

  if (s->hl == DRAW_NORMAL_TEXT)
    {
      s->xgcv.foreground = s->face->foreground;
      s->xgcv.background = s->face->background;
      s->stippled_p = s->face->stipple != 0;
    }
  else if (s->hl == DRAW_INVERSE_VIDEO)
    {
      pgtk_set_mode_line_face_gc (s);
      s->stippled_p = s->face->stipple != 0;
    }
  else if (s->hl == DRAW_CURSOR)
    {
      pgtk_set_cursor_gc (s);
      s->stippled_p = false;
    }
  else if (s->hl == DRAW_MOUSE_FACE)
    {
      pgtk_set_mouse_face_gc (s);
      s->stippled_p = s->face->stipple != 0;
    }
  else if (s->hl == DRAW_IMAGE_RAISED || s->hl == DRAW_IMAGE_SUNKEN)
    {
      s->xgcv.foreground = s->face->foreground;
      s->xgcv.background = s->face->background;
      s->stippled_p = s->face->stipple != 0;
    }
  else
    emacs_abort ();
}

/* Set clipping for output of glyph string S.  S may be part of a mode
   line or menu if we don't have X toolkit support.  */

#ifdef USE_SKIA
static void
pgtk_skia_set_glyph_string_clipping (struct glyph_string *s,
				     emacs_skia_canvas_t *canvas)
{
  XRectangle r[2];
  int n = get_glyph_string_clip_rects (s, r, 2);

  if (n > 0)
    pgtk_skia_set_clip_rectangles (s->f, canvas, r, n);
}

static void
pgtk_skia_set_glyph_string_clipping_exactly (
  struct glyph_string *src, struct glyph_string *dst,
  emacs_skia_canvas_t *canvas)
{
  dst->clip[0].x = src->x;
  dst->clip[0].y = src->y;
  dst->clip[0].width = src->width;
  dst->clip[0].height = src->height;
  dst->num_clips = 1;

  emacs_skia_rect_t rect
    = { src->x, src->y, src->x + src->width, src->y + src->height };
  emacs_skia_canvas_clip_rect (canvas, &rect);
}
#endif

#ifdef USE_CAIRO
static void
pgtk_set_glyph_string_clipping (struct glyph_string *s, cairo_t *cr)
{
  XRectangle r[2];
  int n = get_glyph_string_clip_rects (s, r, 2);

  if (n > 0)
    {
      for (int i = 0; i < n; i++)
	{
	  cairo_rectangle (cr, r[i].x, r[i].y, r[i].width,
			   r[i].height);
	}
      cairo_clip (cr);
    }
}

/* Set SRC's clipping for output of glyph string DST.  This is called
   when we are drawing DST's left_overhang or right_overhang only in
   the area of SRC.  */

static void
pgtk_set_glyph_string_clipping_exactly (struct glyph_string *src,
					struct glyph_string *dst,
					cairo_t *cr)
{
  dst->clip[0].x = src->x;
  dst->clip[0].y = src->y;
  dst->clip[0].width = src->width;
  dst->clip[0].height = src->height;
  dst->num_clips = 1;

  cairo_rectangle (cr, src->x, src->y, src->width, src->height);
  cairo_clip (cr);
}
#endif

/* RIF:
   Compute left and right overhang of glyph string S.  */

static void
pgtk_compute_glyph_string_overhangs (struct glyph_string *s)
{
  if (s->cmp == NULL
      && (s->first_glyph->type == CHAR_GLYPH
	  || s->first_glyph->type == COMPOSITE_GLYPH))
    {
      struct font_metrics metrics;

      if (s->first_glyph->type == CHAR_GLYPH)
	{
	  unsigned *code = alloca (sizeof (unsigned) * s->nchars);
	  struct font *font = s->font;
	  int i;

	  for (i = 0; i < s->nchars; i++)
	    code[i] = s->char2b[i];
	  font->driver->text_extents (font, code, s->nchars,
				      &metrics);
	}
      else
	{
	  Lisp_Object gstring
	    = composition_gstring_from_id (s->cmp_id);

	  composition_gstring_width (gstring, s->cmp_from, s->cmp_to,
				     &metrics);
	}
      s->right_overhang = (metrics.rbearing > metrics.width
			     ? metrics.rbearing - metrics.width
			     : 0);
      s->left_overhang = metrics.lbearing < 0 ? -metrics.lbearing : 0;
    }
  else if (s->cmp)
    {
      s->right_overhang = s->cmp->rbearing - s->cmp->pixel_width;
      s->left_overhang = -s->cmp->lbearing;
    }
}

/* Fill rectangle X, Y, W, H with background color of glyph string
   S.  */
static void
pgtk_clear_glyph_string_rect (struct glyph_string *s, int x, int y,
			      int w, int h)
{
  pgtk_fill_rectangle (s->f, s->xgcv.background, x, y, w, h,
		       (s->first_glyph->type != STRETCH_GLYPH
			|| s->hl != DRAW_CURSOR));
}

static void
fill_background_by_face (struct frame *f, struct face *face, int x,
			 int y, int width, int height)
{
#ifdef USE_SKIA
  if (face->stipple != 0)
    {
      /* Full Skia stipple implementation.  */
      emacs_skia_canvas_t *canvas = pgtk_begin_skia_clip (f);
      if (!canvas)
	return;
      emacs_skia_paint_t *paint = FRAME_SKIA_PAINT (f);
      uint8_t alpha = (uint8_t) (f->alpha_background * 255);

      emacs_skia_rect_t clip = { x, y, x + width, y + height };
      emacs_skia_canvas_save (canvas);
      emacs_skia_canvas_clip_rect (canvas, &clip);

      /* Draw background color.  */
      emacs_skia_paint_set_color (
	paint, pgtk_color_to_skia_alpha (face->background, alpha));
      emacs_skia_paint_set_blend_mode (paint, EMACS_SKIA_BLEND_SRC);
      emacs_skia_paint_set_stroke (paint, false);
      emacs_skia_canvas_draw_rect (canvas, &clip, paint);

      /* Draw stipple pattern with foreground color.  */
      emacs_skia_image_t *stipple_image
	= FRAME_DISPLAY_INFO (f)
	    ->bitmaps[face->stipple - 1]
	    .skia_image;
      if (stipple_image)
	{
	  emacs_skia_paint_set_color (
	    paint,
	    pgtk_color_to_skia_alpha (face->foreground, alpha));
	  emacs_skia_paint_set_image_shader (paint, stipple_image);
	  emacs_skia_canvas_draw_rect (canvas, &clip, paint);
	  emacs_skia_paint_clear_shader (paint);
	}

      emacs_skia_paint_set_blend_mode (paint,
				       EMACS_SKIA_BLEND_SRC_OVER);
      emacs_skia_canvas_restore (canvas);
      pgtk_end_skia_clip (f);
    }
  else
    {
      /* Simple solid fill - use Skia.  */
      pgtk_skia_fill_rectangle (f, face->background, x, y, width,
				height, true);
    }
#else
  cairo_t *cr = pgtk_begin_cr_clip (f);
  double r, g, b, a;

  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_rectangle (cr, x, y, width, height);
  cairo_clip (cr);

  r = ((face->background >> 16) & 0xff) / 255.0;
  g = ((face->background >> 8) & 0xff) / 255.0;
  b = ((face->background >> 0) & 0xff) / 255.0;
  a = f->alpha_background;
  cairo_set_source_rgba (cr, r, g, b, a);
  cairo_paint (cr);

  if (face->stipple != 0)
    {
      cairo_pattern_t *mask
	= FRAME_DISPLAY_INFO (f)->bitmaps[face->stipple - 1].pattern;

      r = ((face->foreground >> 16) & 0xff) / 255.0;
      g = ((face->foreground >> 8) & 0xff) / 255.0;
      b = ((face->foreground >> 0) & 0xff) / 255.0;
      cairo_set_source_rgba (cr, r, g, b, a);
      cairo_mask (cr, mask);
    }

  pgtk_end_cr_clip (f);
#endif
}

static void
fill_background (struct glyph_string *s, int x, int y, int width,
		 int height)
{
  fill_background_by_face (s->f, s->face, x, y, width, height);
}

/* Draw the background of glyph_string S.  If S->background_filled_p
   is non-zero don't draw it.  FORCE_P non-zero means draw the
   background even if it wouldn't be drawn normally.  This is used
   when a string preceding S draws into the background of S, or S
   contains the first component of a composition.  */
static void
pgtk_draw_glyph_string_background (struct glyph_string *s,
				   bool force_p)
{
  /* Nothing to do if background has already been drawn or if it
     shouldn't be drawn in the first place.  */
  if (!s->background_filled_p)
    {
      int box_line_width
	= max (s->face->box_horizontal_line_width, 0);

      if (s->stippled_p)
	{
	  /* Fill background with a stipple pattern.  */
	  fill_background (s, s->x, s->y + box_line_width,
			   s->background_width,
			   s->height - 2 * box_line_width);
	  s->background_filled_p = true;
	}
      else if (FONT_HEIGHT (s->font) < s->height - 2 * box_line_width
	       /* When xdisp.c ignores FONT_HEIGHT, we cannot trust
		  font dimensions, since the actual glyphs might be
		  much smaller.  So in that case we always clear the
		  rectangle with background color.  */
	       || FONT_TOO_HIGH (s->font) || s->font_not_found_p
	       || s->extends_to_end_of_line_p || force_p)
	{
	  pgtk_clear_glyph_string_rect (s, s->x,
					s->y + box_line_width,
					s->background_width,
					s->height
					  - 2 * box_line_width);
	  s->background_filled_p = true;
	}
    }
}

static void
pgtk_draw_rectangle (struct frame *f, unsigned long color, int x,
		     int y, int width, int height,
		     bool respect_alpha_background)
{
#ifdef USE_SKIA
  pgtk_skia_draw_rectangle (f, color, x, y, width, height,
			    respect_alpha_background);
#else
  cairo_t *cr;

  cr = pgtk_begin_cr_clip (f);
  pgtk_set_cr_source_with_color (f, color, respect_alpha_background);
  cairo_rectangle (cr, x + 0.5, y + 0.5, width, height);
  cairo_set_line_width (cr, 1);
  cairo_stroke (cr);
  pgtk_end_cr_clip (f);
#endif
}

/* Draw the foreground of glyph string S.  */
static void
pgtk_draw_glyph_string_foreground (struct glyph_string *s)
{
  int i, x;

  /* If first glyph of S has a left box line, start drawing the text
     of S to the right of that box line.  */
  if (s->face->box != FACE_NO_BOX && s->first_glyph->left_box_line_p)
    x = s->x + max (s->face->box_vertical_line_width, 0);
  else
    x = s->x;

  /* Draw characters of S as rectangles if S's font could not be
     loaded.  */
  if (s->font_not_found_p)
    {
      for (i = 0; i < s->nchars; ++i)
	{
	  struct glyph *g = s->first_glyph + i;
	  pgtk_draw_rectangle (s->f, s->face->foreground, x, s->y,
			       g->pixel_width - 1, s->height - 1,
			       false);
	  x += g->pixel_width;
	}
    }
  else
    {
      struct font *font = s->font;
      int boff = font->baseline_offset;
      int y;

      if (font->vertical_centering)
	boff = VCENTER_BASELINE_OFFSET (font, s->f) - boff;

      y = s->ybase - boff;
      if (s->for_overlaps
	  || (s->background_filled_p && s->hl != DRAW_CURSOR))
	font->driver->draw (s, 0, s->nchars, x, y, false);
      else
	font->driver->draw (s, 0, s->nchars, x, y, true);
      if (s->face->overstrike)
	font->driver->draw (s, 0, s->nchars, x + 1, y, false);
    }
}

/* Draw the foreground of composite glyph string S.  */
static void
pgtk_draw_composite_glyph_string_foreground (struct glyph_string *s)
{
  int i, j, x;
  struct font *font = s->font;

  /* If first glyph of S has a left box line, start drawing the text
     of S to the right of that box line.  */
  if (s->face && s->face->box != FACE_NO_BOX
      && s->first_glyph->left_box_line_p)
    x = s->x + max (s->face->box_vertical_line_width, 0);
  else
    x = s->x;

  /* S is a glyph string for a composition.  S->cmp_from is the index
     of the first character drawn for glyphs of this composition.
     S->cmp_from == 0 means we are drawing the very first character of
     this composition.  */

  /* Draw a rectangle for the composition if the font for the very
     first character of the composition could not be loaded.  */
  if (s->font_not_found_p)
    {
      if (s->cmp_from == 0)
	pgtk_draw_rectangle (s->f, s->face->foreground, x, s->y,
			     s->width - 1, s->height - 1, false);
    }
  else if (!s->first_glyph->u.cmp.automatic)
    {
      int y = s->ybase;

      for (i = 0, j = s->cmp_from; i < s->nchars; i++, j++)
	/* TAB in a composition means display glyphs with padding
	   space on the left or right.  */
	if (COMPOSITION_GLYPH (s->cmp, j) != '\t')
	  {
	    int xx = x + s->cmp->offsets[j * 2];
	    int yy = y - s->cmp->offsets[j * 2 + 1];

	    font->driver->draw (s, j, j + 1, xx, yy, false);
	    if (s->face->overstrike)
	      font->driver->draw (s, j, j + 1, xx + 1, yy, false);
	  }
    }
  else
    {
      Lisp_Object gstring = composition_gstring_from_id (s->cmp_id);
      Lisp_Object glyph;
      int y = s->ybase;
      int width = 0;

      for (i = j = s->cmp_from; i < s->cmp_to; i++)
	{
	  glyph = LGSTRING_GLYPH (gstring, i);
	  if (NILP (LGLYPH_ADJUSTMENT (glyph)))
	    width += LGLYPH_WIDTH (glyph);
	  else
	    {
	      int xoff, yoff, wadjust;

	      if (j < i)
		{
		  font->driver->draw (s, j, i, x, y, false);
		  if (s->face->overstrike)
		    font->driver->draw (s, j, i, x + 1, y, false);
		  x += width;
		}
	      xoff = LGLYPH_XOFF (glyph);
	      yoff = LGLYPH_YOFF (glyph);
	      wadjust = LGLYPH_WADJUST (glyph);
	      font->driver->draw (s, i, i + 1, x + xoff, y + yoff,
				  false);
	      if (s->face->overstrike)
		font->driver->draw (s, i, i + 1, x + xoff + 1,
				    y + yoff, false);
	      x += wadjust;
	      j = i + 1;
	      width = 0;
	    }
	}
      if (j < i)
	{
	  font->driver->draw (s, j, i, x, y, false);
	  if (s->face->overstrike)
	    font->driver->draw (s, j, i, x + 1, y, false);
	}
    }
}

/* Draw the foreground of glyph string S for glyphless characters.  */
static void
pgtk_draw_glyphless_glyph_string_foreground (struct glyph_string *s)
{
  struct glyph *glyph = s->first_glyph;
  unsigned char2b[8];
  int x, i, j;

  /* If first glyph of S has a left box line, start drawing the text
     of S to the right of that box line.  */
  if (s->face && s->face->box != FACE_NO_BOX
      && s->first_glyph->left_box_line_p)
    x = s->x + max (s->face->box_vertical_line_width, 0);
  else
    x = s->x;

  s->char2b = char2b;

  for (i = 0; i < s->nchars; i++, glyph++)
    {
#ifdef GCC_LINT
      enum
      {
	PACIFY_GCC_BUG_81401 = 1
      };
#else
      enum
      {
	PACIFY_GCC_BUG_81401 = 0
      };
#endif
      char buf[7 + PACIFY_GCC_BUG_81401];
      char *str = NULL;
      int len = glyph->u.glyphless.len;

      if (glyph->u.glyphless.method == GLYPHLESS_DISPLAY_ACRONYM)
	{
	  if (len > 0 && CHAR_TABLE_P (Vglyphless_char_display)
	      && (CHAR_TABLE_EXTRA_SLOTS (
		    XCHAR_TABLE (Vglyphless_char_display))
		  >= 1))
	    {
	      Lisp_Object acronym
		= (!glyph->u.glyphless.for_no_font
		     ? CHAR_TABLE_REF (Vglyphless_char_display,
				       glyph->u.glyphless.ch)
		     : XCHAR_TABLE (Vglyphless_char_display)
			 ->extras[0]);
	      if (CONSP (acronym))
		acronym = XCAR (acronym);
	      if (STRINGP (acronym))
		str = SSDATA (acronym);
	    }
	}
      else if (glyph->u.glyphless.method
	       == GLYPHLESS_DISPLAY_HEX_CODE)
	{
	  unsigned int ch = glyph->u.glyphless.ch;
	  eassume (ch <= MAX_CHAR);
	  sprintf (buf, "%0*X", ch < 0x10000 ? 4 : 6, ch);
	  str = buf;
	}

      if (str)
	{
	  int upper_len = (len + 1) / 2;

	  /* It is assured that all LEN characters in STR is ASCII. */
	  for (j = 0; j < len; j++)
	    char2b[j] = s->font->driver->encode_char (s->font, str[j])
			& 0xFFFF;
	  s->font->driver
	    ->draw (s, 0, upper_len,
		    x + glyph->slice.glyphless.upper_xoff,
		    s->ybase + glyph->slice.glyphless.upper_yoff,
		    false);
	  s->font->driver
	    ->draw (s, upper_len, len,
		    x + glyph->slice.glyphless.lower_xoff,
		    s->ybase + glyph->slice.glyphless.lower_yoff,
		    false);
	}
      if (glyph->u.glyphless.method != GLYPHLESS_DISPLAY_THIN_SPACE)
	pgtk_draw_rectangle (s->f, s->face->foreground, x,
			     s->ybase - glyph->ascent,
			     glyph->pixel_width - 1,
			     glyph->ascent + glyph->descent - 1,
			     false);
      x += glyph->pixel_width;
    }

  /* Pacify GCC 12 even though s->char2b is not used after this
     function returns.  */
  s->char2b = NULL;
}

/* Brightness beyond which a color won't have its highlight brightness
   boosted.

   Nominally, highlight colors for `3d' faces are calculated by
   brightening an object's color by a constant scale factor, but this
   doesn't yield good results for dark colors, so for colors who's
   brightness is less than this value (on a scale of 0-65535) have an
   use an additional additive factor.

   The value here is set so that the default menu-bar/mode-line color
   (grey75) will not have its highlights changed at all.  */
#define HIGHLIGHT_COLOR_DARK_BOOST_LIMIT 48000

/* Compute a color which is lighter or darker than *PIXEL by FACTOR or
   DELTA.  Try a color with RGB values multiplied by FACTOR first.  If
   this produces the same color as PIXEL, try a color where all RGB
   values have DELTA added.  Return the computed color in *PIXEL.  F
   is the frame to act on.  */

static void
pgtk_compute_lighter_color (struct frame *f, unsigned long *pixel,
			    double factor, int delta)
{
  Emacs_Color color, new;
  long bright;

  /* Get RGB color values.  */
  color.pixel = *pixel;
  pgtk_query_color (f, &color);

  /* Change RGB values by specified FACTOR.  Avoid overflow!  */
  eassert (factor >= 0);
  new.red = min (0xffff, factor * color.red);
  new.green = min (0xffff, factor * color.green);
  new.blue = min (0xffff, factor * color.blue);

  /* Calculate brightness of COLOR.  */
  bright = (2 * color.red + 3 * color.green + color.blue) / 6;

  /* We only boost colors that are darker than
     HIGHLIGHT_COLOR_DARK_BOOST_LIMIT.  */
  if (bright < HIGHLIGHT_COLOR_DARK_BOOST_LIMIT)
    /* Make an additive adjustment to NEW, because it's dark enough so
       that scaling by FACTOR alone isn't enough.  */
    {
      /* How far below the limit this color is (0 - 1, 1 being
       * darker).  */
      double dimness
	= 1 - (double) bright / HIGHLIGHT_COLOR_DARK_BOOST_LIMIT;
      /* The additive adjustment.  */
      int min_delta = delta * dimness * factor / 2;

      if (factor < 1)
	{
	  new.red = max (0, new.red - min_delta);
	  new.green = max (0, new.green - min_delta);
	  new.blue = max (0, new.blue - min_delta);
	}
      else
	{
	  new.red = min (0xffff, min_delta + new.red);
	  new.green = min (0xffff, min_delta + new.green);
	  new.blue = min (0xffff, min_delta + new.blue);
	}
    }

  new.pixel
    = (new.red >> 8 << 16 | new.green >> 8 << 8 | new.blue >> 8);

  if (new.pixel == *pixel)
    {
      /* If we end up with the same color as before, try adding
	 delta to the RGB values.  */
      new.red = min (0xffff, delta + color.red);
      new.green = min (0xffff, delta + color.green);
      new.blue = min (0xffff, delta + color.blue);
      new.pixel
	= (new.red >> 8 << 16 | new.green >> 8 << 8 | new.blue >> 8);
    }

  *pixel = new.pixel;
}

static void
pgtk_fill_trapezoid_for_relief (struct frame *f, unsigned long color,
				int x, int y, int width, int height,
				int top_p)
{
#ifdef USE_SKIA
  emacs_skia_canvas_t *canvas = pgtk_begin_skia_clip (f);
  if (!canvas)
    return;
  emacs_skia_paint_t *paint = FRAME_SKIA_PAINT (f);
  emacs_skia_path_t *path = emacs_skia_path_create ();
  if (!path)
    {
      pgtk_end_skia_clip (f);
      return;
    }

  emacs_skia_paint_set_color (paint, pgtk_color_to_skia (color));
  emacs_skia_paint_set_blend_mode (paint, EMACS_SKIA_BLEND_SRC_OVER);
  emacs_skia_paint_set_stroke (paint, false);

  emacs_skia_path_move_to (path, top_p ? x : x + height, y);
  emacs_skia_path_line_to (path, x, y + height);
  emacs_skia_path_line_to (path,
			   top_p ? x + width - height : x + width,
			   y + height);
  emacs_skia_path_line_to (path, x + width, y);
  emacs_skia_path_close (path);

  emacs_skia_canvas_draw_path (canvas, path, paint);
  emacs_skia_path_destroy (path);
  pgtk_end_skia_clip (f);
#else
  cairo_t *cr;

  cr = pgtk_begin_cr_clip (f);
  pgtk_set_cr_source_with_color (f, color, false);
  cairo_move_to (cr, top_p ? x : x + height, y);
  cairo_line_to (cr, x, y + height);
  cairo_line_to (cr, top_p ? x + width - height : x + width,
		 y + height);
  cairo_line_to (cr, x + width, y);
  cairo_fill (cr);
  pgtk_end_cr_clip (f);
#endif
}

enum corners
{
  CORNER_BOTTOM_RIGHT, /* 0 -> pi/2 */
  CORNER_BOTTOM_LEFT,  /* pi/2 -> pi */
  CORNER_TOP_LEFT,     /* pi -> 3pi/2 */
  CORNER_TOP_RIGHT,    /* 3pi/2 -> 2pi */
  CORNER_LAST
};

static void
pgtk_erase_corners_for_relief (struct frame *f, unsigned long color,
			       int x, int y, int width, int height,
			       double radius, double margin,
			       int corners)
{
#ifdef USE_SKIA
  emacs_skia_canvas_t *canvas = pgtk_begin_skia_clip (f);
  if (!canvas)
    return;
  emacs_skia_paint_t *paint = FRAME_SKIA_PAINT (f);
  emacs_skia_path_t *clip_path = emacs_skia_path_create ();
  int i;
  if (!clip_path)
    {
      pgtk_end_skia_clip (f);
      return;
    }

  /* Build clipping path from corner arcs */
  for (i = 0; i < CORNER_LAST; i++)
    if (corners & (1 << i))
      {
	double xm, ym, xc, yc;
	emacs_skia_rect_t oval;

	if (i == CORNER_TOP_LEFT || i == CORNER_BOTTOM_LEFT)
	  xm = x - margin, xc = xm + radius;
	else
	  xm = x + width + margin, xc = xm - radius;
	if (i == CORNER_TOP_LEFT || i == CORNER_TOP_RIGHT)
	  ym = y - margin, yc = ym + radius;
	else
	  ym = y + height + margin, yc = ym - radius;

	emacs_skia_path_move_to (clip_path, xm, ym);
	/* Arc bounding box */
	oval.left = xc - radius;
	oval.top = yc - radius;
	oval.right = xc + radius;
	oval.bottom = yc + radius;
	/* Convert from radians to degrees: i * 90 degrees */
	emacs_skia_path_arc_to (clip_path, &oval, i * 90.0, 90.0,
				false);
      }

  emacs_skia_canvas_save (canvas);
  emacs_skia_canvas_clip_path (canvas, clip_path);
  emacs_skia_paint_set_color (paint, pgtk_color_to_skia (color));
  emacs_skia_paint_set_blend_mode (paint, EMACS_SKIA_BLEND_SRC_OVER);
  emacs_skia_paint_set_stroke (paint, false);
  emacs_skia_rect_t rect = { x, y, x + width, y + height };
  emacs_skia_canvas_draw_rect (canvas, &rect, paint);
  emacs_skia_canvas_restore (canvas);
  emacs_skia_path_destroy (clip_path);
  pgtk_end_skia_clip (f);
#else
  cairo_t *cr;
  int i;

  cr = pgtk_begin_cr_clip (f);
  pgtk_set_cr_source_with_color (f, color, false);
  for (i = 0; i < CORNER_LAST; i++)
    if (corners & (1 << i))
      {
	double xm, ym, xc, yc;

	if (i == CORNER_TOP_LEFT || i == CORNER_BOTTOM_LEFT)
	  xm = x - margin, xc = xm + radius;
	else
	  xm = x + width + margin, xc = xm - radius;
	if (i == CORNER_TOP_LEFT || i == CORNER_TOP_RIGHT)
	  ym = y - margin, yc = ym + radius;
	else
	  ym = y + height + margin, yc = ym - radius;

	cairo_move_to (cr, xm, ym);
	cairo_arc (cr, xc, yc, radius, i * M_PI_2, (i + 1) * M_PI_2);
      }
  cairo_clip (cr);
  cairo_rectangle (cr, x, y, width, height);
  cairo_fill (cr);
  pgtk_end_cr_clip (f);
#endif
}

static void
pgtk_setup_relief_color (struct frame *f, struct relief *relief,
			 double factor, int delta,
			 unsigned long default_pixel)
{
  Emacs_GC xgcv;
  struct pgtk_output *di = FRAME_X_OUTPUT (f);
  unsigned long pixel;
  unsigned long background = di->relief_background;

  /* Allocate new color.  */
  xgcv.foreground = default_pixel;
  pixel = background;
  pgtk_compute_lighter_color (f, &pixel, factor, delta);
  xgcv.foreground = relief->pixel = pixel;

  relief->xgcv = xgcv;
}

/* Set up colors for the relief lines around glyph string S.  */
static void
pgtk_setup_relief_colors (struct glyph_string *s)
{
  struct pgtk_output *di = FRAME_X_OUTPUT (s->f);
  unsigned long color;

  if (s->face->use_box_color_for_shadows_p)
    color = s->face->box_color;
  else if (s->first_glyph->type == IMAGE_GLYPH && s->img->pixmap
	   && !IMAGE_BACKGROUND_TRANSPARENT (s->img, s->f, 0))
    color = IMAGE_BACKGROUND (s->img, s->f, 0);
  else
    {
      /* Get the background color of the face.  */
      color = s->xgcv.background;
    }

  if (!di->relief_background_valid_p
      || di->relief_background != color)
    {
      di->relief_background_valid_p = true;
      di->relief_background = color;
      pgtk_setup_relief_color (s->f, &di->white_relief, 1.2, 0x8000,
			       WHITE_PIX_DEFAULT (s->f));
      pgtk_setup_relief_color (s->f, &di->black_relief, 0.6, 0x4000,
			       BLACK_PIX_DEFAULT (s->f));
    }
}

#ifdef USE_SKIA
static void
pgtk_skia_set_clip_rectangles (struct frame *f,
			       emacs_skia_canvas_t *canvas,
			       XRectangle *rectangles, int n)
{
  if (n <= 0)
    return;

  if (n == 1)
    {
      /* Single rectangle: clip directly (most common case).  */
      emacs_skia_rect_t rect
	= { rectangles[0].x, rectangles[0].y,
	    rectangles[0].x + rectangles[0].width,
	    rectangles[0].y + rectangles[0].height };
      emacs_skia_canvas_clip_rect (canvas, &rect);
    }
  else
    {
      /* Multiple rectangles: build a path for their union.
	 In Skia, successive clipRect calls INTERSECT the regions,
	 but we need UNION semantics (matching Cairo's behavior).
	 Use a path containing all rectangles and clip to that.  */
      emacs_skia_path_t *path = emacs_skia_path_create ();
      if (!path)
	return;
      for (int i = 0; i < n; i++)
	{
	  emacs_skia_rect_t rect
	    = { rectangles[i].x, rectangles[i].y,
		rectangles[i].x + rectangles[i].width,
		rectangles[i].y + rectangles[i].height };
	  emacs_skia_path_add_rect (path, &rect);
	}
      emacs_skia_canvas_clip_path (canvas, path);
      emacs_skia_path_destroy (path);
    }
}
#endif

#ifdef USE_CAIRO
static void
pgtk_set_clip_rectangles (struct frame *f, cairo_t *cr,
			  XRectangle *rectangles, int n)
{
  if (n > 0)
    {
      for (int i = 0; i < n; i++)
	cairo_rectangle (cr, rectangles[i].x, rectangles[i].y,
			 rectangles[i].width, rectangles[i].height);
      cairo_clip (cr);
    }
}
#endif

/* Draw a relief on frame F inside the rectangle given by LEFT_X,
   TOP_Y, RIGHT_X, and BOTTOM_Y.  WIDTH is the thickness of the relief
   to draw, it must be >= 0.  RAISED_P means draw a raised
   relief.  LEFT_P means draw a relief on the left side of
   the rectangle.  RIGHT_P means draw a relief on the right
   side of the rectangle.  CLIP_RECT is the clipping rectangle to use
   when drawing.  */

static void
pgtk_draw_relief_rect (struct frame *f, int left_x, int top_y,
		       int right_x, int bottom_y, int hwidth,
		       int vwidth, bool raised_p, bool top_p,
		       bool bot_p, bool left_p, bool right_p,
		       XRectangle *clip_rect)
{
  unsigned long top_left_color, bottom_right_color;
  int corners = 0;

  if (raised_p)
    {
      top_left_color
	= FRAME_X_OUTPUT (f)->white_relief.xgcv.foreground;
      bottom_right_color
	= FRAME_X_OUTPUT (f)->black_relief.xgcv.foreground;
    }
  else
    {
      top_left_color
	= FRAME_X_OUTPUT (f)->black_relief.xgcv.foreground;
      bottom_right_color
	= FRAME_X_OUTPUT (f)->white_relief.xgcv.foreground;
    }

#ifdef USE_SKIA
  emacs_skia_canvas_t *canvas = pgtk_begin_skia_clip (f);
  if (!canvas)
    return;
  pgtk_skia_set_clip_rectangles (f, canvas, clip_rect, 1);
#else
  cairo_t *cr = pgtk_begin_cr_clip (f);
  pgtk_set_clip_rectangles (f, cr, clip_rect, 1);
#endif

  if (left_p)
    {
      pgtk_fill_rectangle (f, top_left_color, left_x, top_y, vwidth,
			   bottom_y + 1 - top_y, false);
      if (top_p)
	corners |= 1 << CORNER_TOP_LEFT;
      if (bot_p)
	corners |= 1 << CORNER_BOTTOM_LEFT;
    }
  if (right_p)
    {
      pgtk_fill_rectangle (f, bottom_right_color,
			   right_x + 1 - vwidth, top_y, vwidth,
			   bottom_y + 1 - top_y, false);
      if (top_p)
	corners |= 1 << CORNER_TOP_RIGHT;
      if (bot_p)
	corners |= 1 << CORNER_BOTTOM_RIGHT;
    }
  if (top_p)
    {
      if (!right_p)
	pgtk_fill_rectangle (f, top_left_color, left_x, top_y,
			     right_x + 1 - left_x, hwidth, false);
      else
	pgtk_fill_trapezoid_for_relief (f, top_left_color, left_x,
					top_y, right_x + 1 - left_x,
					hwidth, 1);
    }
  if (bot_p)
    {
      if (!left_p)
	pgtk_fill_rectangle (f, bottom_right_color, left_x,
			     bottom_y + 1 - hwidth,
			     right_x + 1 - left_x, hwidth, false);
      else
	pgtk_fill_trapezoid_for_relief (f, bottom_right_color, left_x,
					bottom_y + 1 - hwidth,
					right_x + 1 - left_x, hwidth,
					0);
    }
  if (left_p && vwidth > 1)
    pgtk_fill_rectangle (f, bottom_right_color, left_x, top_y, 1,
			 bottom_y + 1 - top_y, false);
  if (top_p && hwidth > 1)
    pgtk_fill_rectangle (f, bottom_right_color, left_x, top_y,
			 right_x + 1 - left_x, 1, false);
  if (corners)
    pgtk_erase_corners_for_relief (f, FRAME_BACKGROUND_PIXEL (f),
				   left_x, top_y,
				   right_x - left_x + 1,
				   bottom_y - top_y + 1, 6, 1,
				   corners);

#ifdef USE_SKIA
  pgtk_end_skia_clip (f);
#else
  pgtk_end_cr_clip (f);
#endif
}

/* Draw a box on frame F inside the rectangle given by LEFT_X, TOP_Y,
   RIGHT_X, and BOTTOM_Y.  WIDTH is the thickness of the lines to
   draw, it must be >= 0.  LEFT_P means draw a line on the
   left side of the rectangle.  RIGHT_P means draw a line
   on the right side of the rectangle.  CLIP_RECT is the clipping
   rectangle to use when drawing.  */

static void
pgtk_draw_box_rect (struct glyph_string *s, int left_x, int top_y,
		    int right_x, int bottom_y, int hwidth, int vwidth,
		    bool left_p, bool right_p, XRectangle *clip_rect)
{
  unsigned long foreground_backup;

  foreground_backup = s->xgcv.foreground;
  s->xgcv.foreground = s->face->box_color;

#ifdef USE_SKIA
  emacs_skia_canvas_t *canvas = pgtk_begin_skia_clip (s->f);
  if (!canvas)
    {
      s->xgcv.foreground = foreground_backup;
      return;
    }
  pgtk_skia_set_clip_rectangles (s->f, canvas, clip_rect, 1);
#else
  cairo_t *cr = pgtk_begin_cr_clip (s->f);
  pgtk_set_clip_rectangles (s->f, cr, clip_rect, 1);
#endif

  /* Top.  */
  pgtk_fill_rectangle (s->f, s->xgcv.foreground, left_x, top_y,
		       right_x - left_x + 1, hwidth, false);

  /* Left.  */
  if (left_p)
    pgtk_fill_rectangle (s->f, s->xgcv.foreground, left_x, top_y,
			 vwidth, bottom_y - top_y + 1, false);

  /* Bottom.  */
  pgtk_fill_rectangle (s->f, s->xgcv.foreground, left_x,
		       bottom_y - hwidth + 1, right_x - left_x + 1,
		       hwidth, false);

  /* Right.  */
  if (right_p)
    pgtk_fill_rectangle (s->f, s->xgcv.foreground,
			 right_x - vwidth + 1, top_y, vwidth,
			 bottom_y - top_y + 1, false);

  s->xgcv.foreground = foreground_backup;

#ifdef USE_SKIA
  pgtk_end_skia_clip (s->f);
#else
  pgtk_end_cr_clip (s->f);
#endif
}

/* Draw a box around glyph string S.  */

static void
pgtk_draw_glyph_string_box (struct glyph_string *s)
{
  int hwidth, vwidth, left_x, right_x, top_y, bottom_y, last_x;
  bool raised_p, left_p, right_p;
  struct glyph *last_glyph;
  XRectangle clip_rect;

  last_x = ((s->row->full_width_p && !s->w->pseudo_window_p)
	      ? WINDOW_RIGHT_EDGE_X (s->w)
	      : window_box_right (s->w, s->area));

  /* The glyph that may have a right box line.  */
  last_glyph = (s->cmp || s->img ? s->first_glyph
				 : s->first_glyph + s->nchars - 1);

  vwidth = eabs (s->face->box_vertical_line_width);
  hwidth = eabs (s->face->box_horizontal_line_width);
  raised_p = s->face->box == FACE_RAISED_BOX;
  left_x = s->x;
  right_x = (s->row->full_width_p && s->extends_to_end_of_line_p
	       ? last_x - 1
	       : min (last_x, s->x + s->background_width) - 1);
  top_y = s->y;
  bottom_y = top_y + s->height - 1;

  left_p = (s->first_glyph->left_box_line_p
	    || (s->hl == DRAW_MOUSE_FACE
		&& (s->prev == NULL || s->prev->hl != s->hl)));
  right_p = (last_glyph->right_box_line_p
	     || (s->hl == DRAW_MOUSE_FACE
		 && (s->next == NULL || s->next->hl != s->hl)));

  get_glyph_string_clip_rect (s, &clip_rect);

  if (s->face->box == FACE_SIMPLE_BOX)
    pgtk_draw_box_rect (s, left_x, top_y, right_x, bottom_y, hwidth,
			vwidth, left_p, right_p, &clip_rect);
  else
    {
      pgtk_setup_relief_colors (s);
      pgtk_draw_relief_rect (s->f, left_x, top_y, right_x, bottom_y,
			     hwidth, vwidth, raised_p, true, true,
			     left_p, right_p, &clip_rect);
    }
}

static void
pgtk_draw_horizontal_wave (struct frame *f, unsigned long color,
			   int x, int y, int width, int height,
			   int wave_length)
{
#ifdef USE_SKIA
  emacs_skia_canvas_t *canvas = pgtk_begin_skia_clip (f);
  if (!canvas)
    return;
  emacs_skia_paint_t *paint = FRAME_SKIA_PAINT (f);
  emacs_skia_path_t *path = emacs_skia_path_create ();
  double dx = wave_length, dy = height - 1;
  int xoffset, n;
  if (!path)
    {
      pgtk_end_skia_clip (f);
      return;
    }

  emacs_skia_canvas_save (canvas);
  emacs_skia_rect_t clip = { x, y, x + width, y + height };
  emacs_skia_canvas_clip_rect (canvas, &clip);

  if (x >= 0)
    {
      xoffset = x % (wave_length * 2);
      if (xoffset == 0)
	xoffset = wave_length * 2;
    }
  else
    xoffset = x % (wave_length * 2) + wave_length * 2;
  n = (width + xoffset) / wave_length + 1;
  if (xoffset > wave_length)
    {
      xoffset -= wave_length;
      --n;
      y += height - 1;
      dy = -dy;
    }

  emacs_skia_path_move_to (path, x - xoffset + 0.5, y + 0.5);
  while (--n >= 0)
    {
      emacs_skia_path_rel_line_to (path, dx, dy);
      dy = -dy;
    }

  emacs_skia_paint_set_color (paint, pgtk_color_to_skia (color));
  emacs_skia_paint_set_blend_mode (paint, EMACS_SKIA_BLEND_SRC_OVER);
  emacs_skia_paint_set_stroke (paint, true);
  emacs_skia_paint_set_stroke_width (paint, 1.0);
  emacs_skia_canvas_draw_path (canvas, path, paint);
  emacs_skia_canvas_restore (canvas);
  emacs_skia_path_destroy (path);
  pgtk_end_skia_clip (f);
#else
  cairo_t *cr;
  double dx = wave_length, dy = height - 1;
  int xoffset, n;

  cr = pgtk_begin_cr_clip (f);
  pgtk_set_cr_source_with_color (f, color, false);
  cairo_rectangle (cr, x, y, width, height);
  cairo_clip (cr);

  if (x >= 0)
    {
      xoffset = x % (wave_length * 2);
      if (xoffset == 0)
	xoffset = wave_length * 2;
    }
  else
    xoffset = x % (wave_length * 2) + wave_length * 2;
  n = (width + xoffset) / wave_length + 1;
  if (xoffset > wave_length)
    {
      xoffset -= wave_length;
      --n;
      y += height - 1;
      dy = -dy;
    }

  cairo_move_to (cr, x - xoffset + 0.5, y + 0.5);
  while (--n >= 0)
    {
      cairo_rel_line_to (cr, dx, dy);
      dy = -dy;
    }
  cairo_set_line_width (cr, 1);
  cairo_stroke (cr);
  pgtk_end_cr_clip (f);
#endif
}

static void
pgtk_draw_underwave (struct glyph_string *s, unsigned long color)
{
  int wave_height = 3, wave_length = 2;

  pgtk_draw_horizontal_wave (s->f, color, s->x,
			     s->ybase - wave_height + 3, s->width,
			     wave_height, wave_length);
}

/* Draw a relief around the image glyph string S.  */

static void
pgtk_draw_image_relief (struct glyph_string *s)
{
  int x1, y1, thick;
  bool raised_p, top_p, bot_p, left_p, right_p;
  int extra_x, extra_y;
  XRectangle r;
  int x = s->x;
  int y = s->ybase - image_ascent (s->img, s->face, &s->slice);

  /* If first glyph of S has a left box line, start drawing it to the
     right of that line.  */
  if (s->face->box != FACE_NO_BOX && s->first_glyph->left_box_line_p
      && s->slice.x == 0)
    x += max (s->face->box_vertical_line_width, 0);

  /* If there is a margin around the image, adjust x- and y-position
     by that margin.  */
  if (s->slice.x == 0)
    x += s->img->hmargin;
  if (s->slice.y == 0)
    y += s->img->vmargin;

  if (s->hl == DRAW_IMAGE_SUNKEN || s->hl == DRAW_IMAGE_RAISED)
    {
      if (s->face->id == TAB_BAR_FACE_ID)
	thick = (tab_bar_button_relief < 0
		   ? DEFAULT_TAB_BAR_BUTTON_RELIEF
		   : min (tab_bar_button_relief, 1000000));
      else
	thick = (tool_bar_button_relief < 0
		   ? DEFAULT_TOOL_BAR_BUTTON_RELIEF
		   : min (tool_bar_button_relief, 1000000));
      raised_p = s->hl == DRAW_IMAGE_RAISED;
    }
  else
    {
      thick = eabs (s->img->relief);
      raised_p = s->img->relief > 0;
    }

  x1 = x + s->slice.width - 1;
  y1 = y + s->slice.height - 1;

  extra_x = extra_y = 0;
  if (s->face->id == TAB_BAR_FACE_ID)
    {
      if (CONSP (Vtab_bar_button_margin)
	  && FIXNUMP (XCAR (Vtab_bar_button_margin))
	  && FIXNUMP (XCDR (Vtab_bar_button_margin)))
	{
	  extra_x = XFIXNUM (XCAR (Vtab_bar_button_margin)) - thick;
	  extra_y = XFIXNUM (XCDR (Vtab_bar_button_margin)) - thick;
	}
      else if (FIXNUMP (Vtab_bar_button_margin))
	extra_x = extra_y = XFIXNUM (Vtab_bar_button_margin) - thick;
    }

  if (s->face->id == TOOL_BAR_FACE_ID)
    {
      if (CONSP (Vtool_bar_button_margin)
	  && FIXNUMP (XCAR (Vtool_bar_button_margin))
	  && FIXNUMP (XCDR (Vtool_bar_button_margin)))
	{
	  extra_x = XFIXNUM (XCAR (Vtool_bar_button_margin));
	  extra_y = XFIXNUM (XCDR (Vtool_bar_button_margin));
	}
      else if (FIXNUMP (Vtool_bar_button_margin))
	extra_x = extra_y = XFIXNUM (Vtool_bar_button_margin);
    }

  top_p = bot_p = left_p = right_p = false;

  if (s->slice.x == 0)
    x -= thick + extra_x, left_p = true;
  if (s->slice.y == 0)
    y -= thick + extra_y, top_p = true;
  if (s->slice.x + s->slice.width == s->img->width)
    x1 += thick + extra_x, right_p = true;
  if (s->slice.y + s->slice.height == s->img->height)
    y1 += thick + extra_y, bot_p = true;

  pgtk_setup_relief_colors (s);
  get_glyph_string_clip_rect (s, &r);
  pgtk_draw_relief_rect (s->f, x, y, x1, y1, thick, thick, raised_p,
			 top_p, bot_p, left_p, right_p, &r);
}

/* Draw part of the background of glyph string S.  X, Y, W, and H
   give the rectangle to draw.  */

static void
pgtk_draw_glyph_string_bg_rect (struct glyph_string *s, int x, int y,
				int w, int h)
{
  if (s->stippled_p)
    fill_background (s, x, y, w, h);
  else
    pgtk_clear_glyph_string_rect (s, x, y, w, h);
}

#ifdef USE_SKIA
/* Draw an image using Skia.
   image: Skia image to draw
   src_x, src_y: source position within the image
   width, height: dimensions to draw
   dest_x, dest_y: destination position on the frame
   overlay_p: if true, draw on top of existing content; if false, fill
   background first */
static void
pgtk_skia_draw_image (struct frame *f, Emacs_GC *gc,
		      emacs_skia_image_t *image, int src_x, int src_y,
		      int width, int height, int dest_x, int dest_y,
		      bool overlay_p)
{
  emacs_skia_canvas_t *canvas = pgtk_begin_skia_clip (f);
  if (!canvas)
    return;
  emacs_skia_paint_t *paint = FRAME_SKIA_PAINT (f);

  /* Clip to destination rectangle */
  emacs_skia_rect_t clip_rect
    = { dest_x, dest_y, dest_x + width, dest_y + height };
  emacs_skia_canvas_save (canvas);
  emacs_skia_canvas_clip_rect (canvas, &clip_rect);

  /* Fill background if not overlay mode */
  if (!overlay_p)
    {
      emacs_skia_paint_set_color (paint, pgtk_color_to_skia (
					   gc->background));
      emacs_skia_paint_set_blend_mode (paint,
				       EMACS_SKIA_BLEND_SRC_OVER);
      emacs_skia_paint_set_stroke (paint, false);
      emacs_skia_canvas_draw_rect (canvas, &clip_rect, paint);
    }

  /* Draw the image */
  emacs_skia_rect_t src_rect
    = { src_x, src_y, src_x + width, src_y + height };
  emacs_skia_rect_t dst_rect
    = { dest_x, dest_y, dest_x + width, dest_y + height };

  /* Reset paint to default for image drawing */
  emacs_skia_paint_set_color (paint,
			      EMACS_SKIA_COLOR (255, 255, 255, 255));
  emacs_skia_paint_set_blend_mode (paint, EMACS_SKIA_BLEND_SRC_OVER);
  emacs_skia_canvas_draw_image_rect (canvas, image, &src_rect,
				     &dst_rect, true, paint);

  emacs_skia_canvas_restore (canvas);
  pgtk_end_skia_clip (f);
}
#endif /* USE_SKIA */

#ifdef USE_CAIRO
static void
pgtk_cr_draw_image (struct frame *f, Emacs_GC *gc,
		    cairo_pattern_t *image, int src_x, int src_y,
		    int width, int height, int dest_x, int dest_y,
		    bool overlay_p)
{
  cairo_t *cr = pgtk_begin_cr_clip (f);

  if (overlay_p)
    cairo_rectangle (cr, dest_x, dest_y, width, height);
  else
    {
      pgtk_set_cr_source_with_gc_background (f, gc, false);
      cairo_rectangle (cr, dest_x, dest_y, width, height);
      cairo_fill_preserve (cr);
    }

  cairo_translate (cr, dest_x - src_x, dest_y - src_y);

  cairo_surface_t *surface;
  cairo_pattern_get_surface (image, &surface);
  cairo_format_t format = cairo_image_surface_get_format (surface);
  if (format != CAIRO_FORMAT_A8 && format != CAIRO_FORMAT_A1)
    {
      cairo_set_source (cr, image);
      cairo_fill (cr);
    }
  else
    {
      pgtk_set_cr_source_with_gc_foreground (f, gc, false);
      cairo_clip (cr);
      cairo_mask (cr, image);
    }

  pgtk_end_cr_clip (f);
}
#endif /* USE_CAIRO */

/* Draw foreground of image glyph string S.  */

static void
pgtk_draw_image_foreground (struct glyph_string *s)
{
  int x = s->x;
  int y = s->ybase - image_ascent (s->img, s->face, &s->slice);

  /* If first glyph of S has a left box line, start drawing it to the
     right of that line.  */
  if (s->face->box != FACE_NO_BOX && s->first_glyph->left_box_line_p
      && s->slice.x == 0)
    x += max (s->face->box_vertical_line_width, 0);

  /* If there is a margin around the image, adjust x- and y-position
     by that margin.  */
  if (s->slice.x == 0)
    x += s->img->hmargin;
  if (s->slice.y == 0)
    y += s->img->vmargin;

#ifdef USE_SKIA
  if (s->img->skia_data)
    {
      emacs_skia_canvas_t *canvas = pgtk_begin_skia_clip (s->f);
      if (!canvas)
	return;
      emacs_skia_paint_t *paint = FRAME_SKIA_PAINT (s->f);

      /* Set up clipping */
      if (s->num_clips > 0)
	{
	  for (int i = 0; i < s->num_clips; i++)
	    {
	      emacs_skia_rect_t clip_rect
		= { s->clip[i].x, s->clip[i].y,
		    s->clip[i].x + s->clip[i].width,
		    s->clip[i].y + s->clip[i].height };
	      emacs_skia_canvas_clip_rect (canvas, &clip_rect);
	    }
	}

      /* Apply transformation if present (for image scaling/rotation).
	 This mirrors how Cairo handles image transforms through its
	 pattern matrix.  */
      if (s->img->skia_transform)
	{
	  /* Get the transform matrix.  The matrix converts from
	     display coordinates to image coordinates.  For drawing,
	     we need to:
	     1. Clip to destination area
	     2. Translate canvas so slice position maps to dest
	     position
	     3. Apply inverse of transform (to scale image to display
	     size)
	     4. Draw the full image at (0,0) - clipping restricts
	     visible area

	     The math: we want original pixel (ox, oy) to appear at
	     screen position (ox * inv_scale + (dest - slice), oy *
	     inv_scale + ...). So when ox * inv_scale = slice.x, it
	     appears at dest.x.  */
	  emacs_skia_rect_t clip_rect
	    = { x, y, x + s->slice.width, y + s->slice.height };
	  emacs_skia_canvas_clip_rect (canvas, &clip_rect);

	  /* Get transform matrix [scaleX, skewY, skewX, scaleY,
	     transX, transY]. This transforms display coords -> image
	     coords, so scaleX = img_width/display_width. We need the
	     inverse for drawing: display_width/img_width.  */
	  float matrix[6];
	  emacs_skia_image_transform_get_matrix (s->img
						   ->skia_transform,
						 matrix);
	  float scale_x = matrix[0]; /* display -> image scale */
	  float scale_y = matrix[3];
	  float inv_scale_x = (scale_x != 0) ? 1.0f / scale_x : 1.0f;
	  float inv_scale_y = (scale_y != 0) ? 1.0f / scale_y : 1.0f;

	  /* Translate to position the slice correctly.
	     After scaling, display point (slice.x, slice.y) should
	     appear at screen position (x, y).  */
	  emacs_skia_canvas_translate (canvas, x - s->slice.x,
				       y - s->slice.y);

	  /* Scale image from original size to display size.  */
	  emacs_skia_canvas_scale (canvas, inv_scale_x, inv_scale_y);

	  /* Draw the full image - clipping will restrict to visible
	   * area.  */
	  bool smoothing = emacs_skia_image_transform_get_smoothing (
	    s->img->skia_transform);
	  emacs_skia_paint_set_color (paint,
				      EMACS_SKIA_COLOR (255, 255, 255,
							255));
	  emacs_skia_canvas_draw_image_with_sampling (canvas,
						      s->img
							->skia_data,
						      0, 0, smoothing,
						      paint);
	}
      else
	{
	  /* No transform - draw directly.  */
	  pgtk_skia_draw_image (s->f, &s->xgcv, s->img->skia_data,
				s->slice.x, s->slice.y,
				s->slice.width, s->slice.height, x, y,
				true);
	}

      if (!s->img->mask)
	{
	  /* When the image has a mask, we can expect that at
	     least part of a mouse highlight or a block cursor will
	     be visible.  If the image doesn't have a mask, make
	     a block cursor visible by drawing a rectangle around
	     the image.  I believe it's looking better if we do
	     nothing here for mouse-face.  */
	  if (s->hl == DRAW_CURSOR)
	    {
	      int relief = eabs (s->img->relief);
	      pgtk_draw_rectangle (s->f, s->xgcv.foreground,
				   x - relief, y - relief,
				   s->slice.width + relief * 2 - 1,
				   s->slice.height + relief * 2 - 1,
				   false);
	    }
	}
      pgtk_end_skia_clip (s->f);
    }
  else
    /* Draw a rectangle if image could not be loaded.  */
    pgtk_draw_rectangle (s->f, s->xgcv.foreground, x, y,
			 s->slice.width - 1, s->slice.height - 1,
			 false);
#else  /* USE_CAIRO */
  if (s->img->cr_data)
    {
      cairo_t *cr = pgtk_begin_cr_clip (s->f);
      pgtk_set_glyph_string_clipping (s, cr);
      pgtk_cr_draw_image (s->f, &s->xgcv, s->img->cr_data, s->slice.x,
			  s->slice.y, s->slice.width, s->slice.height,
			  x, y, true);
      if (!s->img->mask)
	{
	  /* When the image has a mask, we can expect that at
	     least part of a mouse highlight or a block cursor will
	     be visible.  If the image doesn't have a mask, make
	     a block cursor visible by drawing a rectangle around
	     the image.  I believe it's looking better if we do
	     nothing here for mouse-face.  */
	  if (s->hl == DRAW_CURSOR)
	    {
	      int relief = eabs (s->img->relief);
	      pgtk_draw_rectangle (s->f, s->xgcv.foreground,
				   x - relief, y - relief,
				   s->slice.width + relief * 2 - 1,
				   s->slice.height + relief * 2 - 1,
				   false);
	    }
	}
      pgtk_end_cr_clip (s->f);
    }
  else
    /* Draw a rectangle if image could not be loaded.  */
    pgtk_draw_rectangle (s->f, s->xgcv.foreground, x, y,
			 s->slice.width - 1, s->slice.height - 1,
			 false);
#endif /* USE_SKIA */
}

/* Draw image glyph string S.

	    s->y
   s->x      +-------------------------
	     |   s->face->box
	     |
	     |     +-------------------------
	     |     |  s->img->margin
	     |     |
	     |     |       +-------------------
	     |     |       |  the image

 */

static void
pgtk_draw_image_glyph_string (struct glyph_string *s)
{
  int box_line_hwidth = max (s->face->box_vertical_line_width, 0);
  int box_line_vwidth = max (s->face->box_horizontal_line_width, 0);
  int height;

  height = s->height;
  if (s->slice.y == 0)
    height -= box_line_vwidth;
  if (s->slice.y + s->slice.height >= s->img->height)
    height -= box_line_vwidth;

  /* Fill background with face under the image.  Do it only if row is
     taller than image or if image has a clip mask to reduce
     flickering.  */
  s->stippled_p = s->face->stipple != 0;
  if (height > s->slice.height || s->img->hmargin || s->img->vmargin
      || s->img->mask || s->img->pixmap == 0
      || s->width != s->background_width)
    {
      int x = s->x;
      int y = s->y;
      int width = s->background_width;

      if (s->first_glyph->left_box_line_p && s->slice.x == 0)
	{
	  x += box_line_hwidth;
	  width -= box_line_hwidth;
	}

      if (s->slice.y == 0)
	y += box_line_vwidth;

      pgtk_draw_glyph_string_bg_rect (s, x, y, width, height);

      s->background_filled_p = true;
    }

  /* Draw the foreground.  */
  pgtk_draw_image_foreground (s);

  /* If we must draw a relief around the image, do it.  */
  if (s->img->relief || s->hl == DRAW_IMAGE_RAISED
      || s->hl == DRAW_IMAGE_SUNKEN)
    pgtk_draw_image_relief (s);
}

/* Draw stretch glyph string S.  */

static void
pgtk_draw_stretch_glyph_string (struct glyph_string *s)
{
  eassert (s->first_glyph->type == STRETCH_GLYPH);

  if (s->hl == DRAW_CURSOR && !x_stretch_cursor_p)
    {
      /* If `x-stretch-cursor' is nil, don't draw a block cursor as
	 wide as the stretch glyph.  */
      int width, background_width = s->background_width;
      int x = s->x;

      if (!s->row->reversed_p)
	{
	  int left_x = window_box_left_offset (s->w, TEXT_AREA);

	  if (x < left_x)
	    {
	      background_width -= left_x - x;
	      x = left_x;
	    }
	}
      else
	{
	  /* In R2L rows, draw the cursor on the right edge of the
	     stretch glyph.  */
	  int right_x = window_box_right (s->w, TEXT_AREA);

	  if (x + background_width > right_x)
	    background_width -= x - right_x;
	  x += background_width;
	}
      width = min (FRAME_COLUMN_WIDTH (s->f), background_width);
      if (s->row->reversed_p)
	x -= width;

      /* Draw cursor.  */
      pgtk_draw_glyph_string_bg_rect (s, x, s->y, width, s->height);

      /* Clear rest using the GC of the original non-cursor face.  */
      if (width < background_width)
	{
	  int y = s->y;
	  int w = background_width - width, h = s->height;
	  XRectangle r;
	  unsigned long color;

	  if (!s->row->reversed_p)
	    x += width;
	  else
	    x = s->x;
	  if (s->row->mouse_face_p && cursor_in_mouse_face_p (s->w))
	    {
	      pgtk_set_mouse_face_gc (s);
	      color = s->xgcv.foreground;
	    }
	  else
	    color = s->face->background;

#ifdef USE_SKIA
	  {
	    emacs_skia_canvas_t *canvas = pgtk_begin_skia_clip (s->f);
	    if (!canvas)
	      return;

	    get_glyph_string_clip_rect (s, &r);
	    pgtk_skia_set_clip_rectangles (s->f, canvas, &r, 1);

	    if (s->face->stipple)
	      fill_background (s, x, y, w, h);
	    else
	      pgtk_fill_rectangle (s->f, color, x, y, w, h, true);

	    pgtk_end_skia_clip (s->f);
	  }
#else
	  cairo_t *cr = pgtk_begin_cr_clip (s->f);

	  get_glyph_string_clip_rect (s, &r);
	  pgtk_set_clip_rectangles (s->f, cr, &r, 1);

	  if (s->face->stipple)
	    fill_background (s, x, y, w, h);
	  else
	    pgtk_fill_rectangle (s->f, color, x, y, w, h, true);

	  pgtk_end_cr_clip (s->f);
#endif
	}
    }
  else if (!s->background_filled_p)
    {
      int background_width = s->background_width;
      int x = s->x, text_left_x = window_box_left (s->w, TEXT_AREA);

      /* Don't draw into left fringe or scrollbar area except for
	 header line and mode line.  */
      if (s->area == TEXT_AREA && x < text_left_x
	  && !s->row->mode_line_p)
	{
	  background_width -= text_left_x - x;
	  x = text_left_x;
	}

      if (background_width > 0)
	pgtk_draw_glyph_string_bg_rect (s, x, s->y, background_width,
					s->height);
    }

  s->background_filled_p = true;
}

/* Draw a dashed underline of thickness THICKNESS and width WIDTH onto
   F at a vertical offset of OFFSET from the position of the glyph
   string S, with each segment SEGMENT pixels in length.  */

static void
pgtk_draw_dash (struct frame *f, struct glyph_string *s,
		unsigned long foreground, int width, char segment,
		int offset, int thickness)
{
#ifdef USE_SKIA
  emacs_skia_canvas_t *canvas = pgtk_begin_skia_clip (f);
  if (!canvas)
    return;
  emacs_skia_paint_t *paint = FRAME_SKIA_PAINT (f);
  float sk_segment = (float) segment;
  float y_center = s->ybase + offset + (thickness / 2.0);
  float intervals[2] = { sk_segment, sk_segment };

  emacs_skia_paint_set_color (paint, pgtk_color_to_skia (foreground));
  emacs_skia_paint_set_blend_mode (paint, EMACS_SKIA_BLEND_SRC_OVER);
  emacs_skia_paint_set_stroke (paint, true);
  emacs_skia_paint_set_stroke_width (paint, thickness);
  emacs_skia_paint_set_dash (paint, intervals, 2, (float) s->x);
  emacs_skia_canvas_draw_line (canvas, s->x, y_center, s->x + width,
			       y_center, paint);
  emacs_skia_paint_clear_dash (paint);
  pgtk_end_skia_clip (f);
#else
  cairo_t *cr;
  double cr_segment, y_center;

  cr = pgtk_begin_cr_clip (s->f);
  pgtk_set_cr_source_with_color (f, foreground, false);
  cr_segment = (double) segment;
  y_center = s->ybase + offset + (thickness / 2.0);

  cairo_set_dash (cr, &cr_segment, 1, s->x);
  cairo_set_line_width (cr, thickness);
  cairo_move_to (cr, s->x, y_center);
  cairo_line_to (cr, s->x + width, y_center);
  cairo_stroke (cr);
  pgtk_end_cr_clip (f);
#endif
}

/* Draw an underline of STYLE onto F at an offset of POSITION from the
   baseline of the glyph string S in the color provided by FOREGROUND,
   DECORATION_WIDTH in length, and THICKNESS in height.  */

static void
pgtk_fill_underline (struct frame *f, struct glyph_string *s,
		     unsigned long foreground,
		     enum face_underline_type style, int position,
		     int decoration_width, int thickness)
{
  int segment;

  segment = thickness * 3;

  switch (style)
    {
      /* FACE_UNDERLINE_DOUBLE_LINE is treated identically to SINGLE,
	 as the second line will be filled by another invocation of
	 this function.  */
    case FACE_UNDERLINE_SINGLE:
    case FACE_UNDERLINE_DOUBLE_LINE:
      pgtk_fill_rectangle (f, foreground, s->x, s->ybase + position,
			   decoration_width, thickness, false);
      break;

    case FACE_UNDERLINE_DOTS:
      segment = thickness;
      FALLTHROUGH;

    case FACE_UNDERLINE_DASHES:
      pgtk_draw_dash (f, s, foreground, decoration_width, segment,
		      position, thickness);
      break;

    case FACE_NO_UNDERLINE:
    case FACE_UNDERLINE_WAVE:
    default:
      emacs_abort ();
    }
}

static void
pgtk_draw_glyph_string (struct glyph_string *s)
{
  bool relief_drawn_p = false;
#ifdef USE_SKIA
  emacs_skia_canvas_t *canvas;
#else
  cairo_t *cr;
#endif

  /* If S draws into the background of its successors, draw the
     background of the successors first so that S can draw into it.
     This makes S->next use XDrawString instead of XDrawImageString.
   */
  if (s->next && s->right_overhang && !s->for_overlaps)
    {
      int width;
      struct glyph_string *next;

      for (width = 0, next = s->next;
	   next && width < s->right_overhang;
	   width += next->width, next = next->next)
	if (next->first_glyph->type != IMAGE_GLYPH)
	  {
#ifdef USE_SKIA
	    canvas = pgtk_begin_skia_clip (next->f);
	    if (!canvas)
	      continue;
	    pgtk_set_glyph_string_gc (next);
	    pgtk_skia_set_glyph_string_clipping (next, canvas);
#else
	    cr = pgtk_begin_cr_clip (next->f);
	    pgtk_set_glyph_string_gc (next);
	    pgtk_set_glyph_string_clipping (next, cr);
#endif
	    if (next->first_glyph->type == STRETCH_GLYPH)
	      pgtk_draw_stretch_glyph_string (next);
	    else
	      pgtk_draw_glyph_string_background (next, true);
	    next->num_clips = 0;
#ifdef USE_SKIA
	    pgtk_end_skia_clip (next->f);
#else
	    pgtk_end_cr_clip (next->f);
#endif
	  }
    }

  /* Set up S->gc, set clipping and draw S.  */
  pgtk_set_glyph_string_gc (s);

#ifdef USE_SKIA
  canvas = pgtk_begin_skia_clip (s->f);
  if (!canvas)
    goto done;
#else
  cr = pgtk_begin_cr_clip (s->f);
#endif

  /* Draw relief (if any) in advance for char/composition so that the
     glyph string can be drawn over it.  */
  if (!s->for_overlaps && s->face->box != FACE_NO_BOX
      && (s->first_glyph->type == CHAR_GLYPH
	  || s->first_glyph->type == COMPOSITE_GLYPH))

    {
#ifdef USE_SKIA
      pgtk_skia_set_glyph_string_clipping (s, canvas);
      pgtk_draw_glyph_string_background (s, true);
      pgtk_draw_glyph_string_box (s);
      pgtk_skia_set_glyph_string_clipping (s, canvas);
#else
      pgtk_set_glyph_string_clipping (s, cr);
      pgtk_draw_glyph_string_background (s, true);
      pgtk_draw_glyph_string_box (s);
      pgtk_set_glyph_string_clipping (s, cr);
#endif
      relief_drawn_p = true;
    }
  else if (!s->clip_head /* draw_glyphs didn't specify a clip mask. */
	   && !s->clip_tail
	   && ((s->prev && s->prev->hl != s->hl && s->left_overhang)
	       || (s->next && s->next->hl != s->hl
		   && s->right_overhang)))
  /* We must clip just this glyph.  left_overhang part has already
     drawn when s->prev was drawn, and right_overhang part will be
     drawn later when s->next is drawn. */
#ifdef USE_SKIA
    pgtk_skia_set_glyph_string_clipping_exactly (s, s, canvas);
  else
    pgtk_skia_set_glyph_string_clipping (s, canvas);
#else
    pgtk_set_glyph_string_clipping_exactly (s, s, cr);
  else
    pgtk_set_glyph_string_clipping (s, cr);
#endif

  switch (s->first_glyph->type)
    {
    case IMAGE_GLYPH:
      pgtk_draw_image_glyph_string (s);
      break;

    case XWIDGET_GLYPH:
      x_draw_xwidget_glyph_string (s);
      break;

    case STRETCH_GLYPH:
      pgtk_draw_stretch_glyph_string (s);
      break;

    case CHAR_GLYPH:
      if (s->for_overlaps)
	s->background_filled_p = true;
      else
	pgtk_draw_glyph_string_background (s, false);
      pgtk_draw_glyph_string_foreground (s);
      break;

    case COMPOSITE_GLYPH:
      if (s->for_overlaps
	  || (s->cmp_from > 0 && !s->first_glyph->u.cmp.automatic))
	s->background_filled_p = true;
      else
	pgtk_draw_glyph_string_background (s, true);
      pgtk_draw_composite_glyph_string_foreground (s);
      break;

    case GLYPHLESS_GLYPH:
      if (s->for_overlaps)
	s->background_filled_p = true;
      else
	pgtk_draw_glyph_string_background (s, true);
      pgtk_draw_glyphless_glyph_string_foreground (s);
      break;

    default:
      emacs_abort ();
    }

  if (!s->for_overlaps)
    {
      /* Draw relief if not yet drawn.  */
      if (!relief_drawn_p && s->face->box != FACE_NO_BOX)
	pgtk_draw_glyph_string_box (s);

      /* Draw underline.  */
      if (s->face->underline)
	{
	  if (s->face->underline == FACE_UNDERLINE_WAVE)
	    {
	      if (s->face->underline_defaulted_p)
		pgtk_draw_underwave (s, s->xgcv.foreground);
	      else
		pgtk_draw_underwave (s, s->face->underline_color);
	    }
	  else if (s->face->underline >= FACE_UNDERLINE_SINGLE)
	    {
	      unsigned long thickness, position;
	      unsigned long foreground;

	      if (s->prev
		  && (s->prev->face->underline != FACE_UNDERLINE_WAVE
		      && s->prev->face->underline
			   >= FACE_UNDERLINE_SINGLE)
		  && (s->prev->face->underline_at_descent_line_p
		      == s->face->underline_at_descent_line_p)
		  && (s->prev->face
			->underline_pixels_above_descent_line
		      == s->face
			   ->underline_pixels_above_descent_line))
		{
		  /* We use the same underline style as the previous
		   * one.  */
		  thickness = s->prev->underline_thickness;
		  position = s->prev->underline_position;
		}
	      else
		{
		  struct font *font = font_for_underline_metrics (s);

		  /* Get the underline thickness.  Default is 1 pixel.
		   */
		  if (font && font->underline_thickness > 0)
		    thickness = font->underline_thickness;
		  else
		    thickness = 1;
		  if ((x_underline_at_descent_line
		       || s->face->underline_at_descent_line_p))
		    position
		      = ((s->height - thickness) - (s->ybase - s->y)
			 - s->face
			     ->underline_pixels_above_descent_line);
		  else
		    {
		      /* Get the underline position.  This is the
			 recommended vertical offset in pixels from
			 the baseline to the top of the underline.
			 This is a signed value according to the
			 specs, and its default is

			 ROUND ((maximum descent) / 2), with
			 ROUND(x) = floor (x + 0.5)  */

		      if (x_use_underline_position_properties && font
			  && font->underline_position >= 0)
			position = font->underline_position;
		      else if (font)
			position = (font->descent + 1) / 2;
		      else
			position = underline_minimum_offset;
		    }

		  /* Ignore minimum_offset if the amount of pixels was
		     explicitly specified.  */
		  if (!s->face->underline_pixels_above_descent_line)
		    position
		      = max (position, underline_minimum_offset);
		}
	      /* Check the sanity of thickness and position.  We
		 should avoid drawing underline out of the current
		 line area.  */
	      if (s->y + s->height <= s->ybase + position)
		position = (s->height - 1) - (s->ybase - s->y);
	      if (s->y + s->height < s->ybase + position + thickness)
		thickness
		  = (s->y + s->height) - (s->ybase + position);
	      s->underline_thickness = thickness;
	      s->underline_position = position;

	      if (s->face->underline_defaulted_p)
		foreground = s->xgcv.foreground;
	      else
		foreground = s->face->underline_color;

	      pgtk_fill_underline (s->f, s, foreground,
				   s->face->underline, position,
				   s->width, thickness);

	      /* Place a second underline above the first if this was
		 requested in the face specification.  */

	      if (s->face->underline == FACE_UNDERLINE_DOUBLE_LINE)
		{
		  /* Compute the position of the second underline.  */
		  position = position - thickness - 1;
		  pgtk_fill_underline (s->f, s, foreground,
				       s->face->underline, position,
				       s->width, thickness);
		}
	    }
	}
      /* Draw overline.  */
      if (s->face->overline_p)
	{
	  unsigned long dy = 0, h = 1;

	  if (s->face->overline_color_defaulted_p)
	    pgtk_fill_rectangle (s->f, s->xgcv.foreground, s->x,
				 s->y + dy, s->width, h, false);
	  else
	    pgtk_fill_rectangle (s->f, s->face->overline_color, s->x,
				 s->y + dy, s->width, h, false);
	}

      /* Draw strike-through.  */
      if (s->face->strike_through_p)
	{
	  /* Y-coordinate and height of the glyph string's first
	     glyph.  We cannot use s->y and s->height because those
	     could be larger if there are taller display elements
	     (e.g., characters displayed with a larger font) in the
	     same glyph row.  */
	  int glyph_y = s->ybase - s->first_glyph->ascent;
	  int glyph_height
	    = s->first_glyph->ascent + s->first_glyph->descent;
	  /* Strike-through width and offset from the glyph string's
	     top edge.  */
	  unsigned long h = 1;
	  unsigned long dy = (glyph_height - h) / 2;

	  if (s->face->strike_through_color_defaulted_p)
	    pgtk_fill_rectangle (s->f, s->xgcv.foreground, s->x,
				 glyph_y + dy, s->width, h, false);
	  else
	    pgtk_fill_rectangle (s->f, s->face->strike_through_color,
				 s->x, glyph_y + dy, s->width, h,
				 false);
	}

      if (s->prev)
	{
	  struct glyph_string *prev;

	  for (prev = s->prev; prev; prev = prev->prev)
	    if (prev->hl != s->hl
		&& prev->x + prev->width + prev->right_overhang
		     > s->x)
	      {
		/* As prev was drawn while clipped to its own area, we
		   must draw the right_overhang part using s->hl now.
		 */
		enum draw_glyphs_face save = prev->hl;

		prev->hl = s->hl;
		pgtk_set_glyph_string_gc (prev);
#ifdef USE_SKIA
		emacs_skia_canvas_save (canvas);
		pgtk_skia_set_glyph_string_clipping_exactly (s, prev,
							     canvas);
#else
		cairo_save (cr);
		pgtk_set_glyph_string_clipping_exactly (s, prev, cr);
#endif
		if (prev->first_glyph->type == CHAR_GLYPH)
		  pgtk_draw_glyph_string_foreground (prev);
		else
		  pgtk_draw_composite_glyph_string_foreground (prev);
		prev->hl = save;
		prev->num_clips = 0;
#ifdef USE_SKIA
		emacs_skia_canvas_restore (canvas);
#else
		cairo_restore (cr);
#endif
	      }
	}

      if (s->next)
	{
	  struct glyph_string *next;

	  for (next = s->next; next; next = next->next)
	    if (next->hl != s->hl
		&& next->x - next->left_overhang < s->x + s->width)
	      {
		/* As next will be drawn while clipped to its own
		   area, we must draw the left_overhang part using
		   s->hl now.  */
		enum draw_glyphs_face save = next->hl;

		next->hl = s->hl;
		pgtk_set_glyph_string_gc (next);
#ifdef USE_SKIA
		emacs_skia_canvas_save (canvas);
		pgtk_skia_set_glyph_string_clipping_exactly (s, next,
							     canvas);
#else
		cairo_save (cr);
		pgtk_set_glyph_string_clipping_exactly (s, next, cr);
#endif
		if (next->first_glyph->type == CHAR_GLYPH)
		  pgtk_draw_glyph_string_foreground (next);
		else
		  pgtk_draw_composite_glyph_string_foreground (next);
#ifdef USE_SKIA
		emacs_skia_canvas_restore (canvas);
#else
		cairo_restore (cr);
#endif
		next->hl = save;
		next->num_clips = 0;
		next->clip_head = s->next;
	      }
	}
    }

  /* TODO: figure out in which cases the stipple is actually drawn on
     PGTK.  */
  if (!s->row->stipple_p)
    s->row->stipple_p = s->face->stipple;

  /* Reset clipping.  */
#ifdef USE_SKIA
done:
  pgtk_end_skia_clip (s->f);
#else
  pgtk_end_cr_clip (s->f);
#endif
  s->num_clips = 0;
}

/* RIF: Define cursor CURSOR on frame F.  */

static void
pgtk_define_frame_cursor (struct frame *f, Emacs_Cursor cursor)
{
  if (!f->pointer_invisible
      && FRAME_X_OUTPUT (f)->current_cursor != cursor)
    gdk_window_set_cursor (gtk_widget_get_window (
			     FRAME_GTK_WIDGET (f)),
			   cursor);
  FRAME_X_OUTPUT (f)->current_cursor = cursor;
}

static void
pgtk_after_update_window_line (struct window *w,
			       struct glyph_row *desired_row)
{
  struct frame *f;
  int width, height;

  /* begin copy from other terms */
  eassert (w);

  if (!desired_row->mode_line_p && !w->pseudo_window_p)
    desired_row->redraw_fringe_bitmaps_p = 1;

  /* When a window has disappeared, make sure that no rest of
     full-width rows stays visible in the internal border.  */
  if (windows_or_buffers_changed && desired_row->full_width_p
      && (f = XFRAME (w->frame),
	  width = FRAME_INTERNAL_BORDER_WIDTH (f), width != 0)
      && (height = desired_row->visible_height, height > 0))
    {
      int y = WINDOW_TO_FRAME_PIXEL_Y (w, max (0, desired_row->y));

      block_input ();
      pgtk_clear_frame_area (f, 0, y, width, height);
      pgtk_clear_frame_area (f, FRAME_PIXEL_WIDTH (f) - width, y,
			     width, height);
      unblock_input ();
    }
}

static void
pgtk_clear_frame_area (struct frame *f, int x, int y, int width,
		       int height)
{
  pgtk_clear_area (f, x, y, width, height);
}

/* Draw a hollow box cursor on window W in glyph row ROW.  */

static void
pgtk_draw_hollow_cursor (struct window *w, struct glyph_row *row)
{
  struct frame *f = XFRAME (WINDOW_FRAME (w));
  int x, y, wd, h;
  struct glyph *cursor_glyph;

  /* Get the glyph the cursor is on.  If we can't tell because
     the current matrix is invalid or such, give up.  */
  cursor_glyph = get_phys_cursor_glyph (w);
  if (cursor_glyph == NULL)
    return;

  /* Compute frame-relative coordinates for phys cursor.  */
  get_phys_cursor_geometry (w, row, cursor_glyph, &x, &y, &h);
  wd = w->phys_cursor_width - 1;

  /* When on R2L character, show cursor at the right edge of the
     glyph, unless the cursor box is as wide as the glyph or wider
     (the latter happens when x-stretch-cursor is non-nil).  */
  if ((cursor_glyph->resolved_level & 1) != 0
      && cursor_glyph->pixel_width > wd)
    {
      x += cursor_glyph->pixel_width - wd;
      if (wd > 0)
	wd -= 1;
    }

#ifdef USE_SKIA
  /* Use Skia directly with Skia clipping.  */
  emacs_skia_canvas_t *canvas = pgtk_begin_skia_clip (f);
  if (canvas)
    {
      pgtk_skia_clip_to_row (w, row, TEXT_AREA, canvas);
      pgtk_skia_set_paint_color (f, FRAME_X_OUTPUT (f)->cursor_color,
				 false);

      emacs_skia_paint_t *paint = FRAME_SKIA_PAINT (f);
      emacs_skia_paint_set_stroke (paint, true);
      emacs_skia_paint_set_stroke_width (paint, 1.0f);

      emacs_skia_rect_t rect
	= { x + 0.5f, y + 0.5f, x + wd + 0.5f, y + h - 1 + 0.5f };
      emacs_skia_canvas_draw_rect (canvas, &rect, paint);

      emacs_skia_paint_set_stroke (paint, false);
      pgtk_end_skia_clip (f);
    }
#else
  /* The foreground of cursor_gc is typically the same as the normal
     background color, which can cause the cursor box to be invisible.
   */
  cairo_t *cr = pgtk_begin_cr_clip (f);
  pgtk_set_cr_source_with_color (f, FRAME_X_OUTPUT (f)->cursor_color,
				 false);
  /* Set clipping, draw the rectangle, and reset clipping again.  */
  pgtk_clip_to_row (w, row, TEXT_AREA, cr);
  pgtk_draw_rectangle (f, FRAME_X_OUTPUT (f)->cursor_color, x, y, wd,
		       h - 1, false);
  pgtk_end_cr_clip (f);
#endif
}

/* Draw a bar cursor on window W in glyph row ROW.

   Implementation note: One would like to draw a bar cursor with an
   angle equal to the one given by the font property XA_ITALIC_ANGLE.
   Unfortunately, I didn't find a font yet that has this property set.
   --gerd.  */

static void
pgtk_draw_bar_cursor (struct window *w, struct glyph_row *row,
		      int width, enum text_cursor_kinds kind)
{
  struct frame *f = XFRAME (w->frame);
  struct glyph *cursor_glyph;

  /* If cursor is out of bounds, don't draw garbage.  This can happen
     in mini-buffer windows when switching between echo area glyphs
     and mini-buffer.  */
  cursor_glyph = get_phys_cursor_glyph (w);
  if (cursor_glyph == NULL)
    return;

  /* Experimental avoidance of cursor on xwidget.  */
  if (cursor_glyph->type == XWIDGET_GLYPH)
    return;

  /* If on an image, draw like a normal cursor.  That's usually better
     visible than drawing a bar, esp. if the image is large so that
     the bar might not be in the window.  */
  if (cursor_glyph->type == IMAGE_GLYPH)
    {
      struct glyph_row *r;
      r = MATRIX_ROW (w->current_matrix, w->phys_cursor.vpos);
      draw_phys_cursor_glyph (w, r, DRAW_CURSOR);
    }
  else
    {
      struct face *face = FACE_FROM_ID (f, cursor_glyph->face_id);
      unsigned long color;

      /* If the glyph's background equals the color we normally draw
	 the bars cursor in, the bar cursor in its normal color is
	 invisible.  Use the glyph's foreground color instead in this
	 case, on the assumption that the glyph's colors are chosen so
	 that the glyph is legible.  */
      if (face->background == FRAME_X_OUTPUT (f)->cursor_color)
	color = face->foreground;
      else
	color = FRAME_X_OUTPUT (f)->cursor_color;

#ifdef USE_SKIA
      emacs_skia_canvas_t *canvas = pgtk_begin_skia_clip (f);
      if (canvas)
	{
	  pgtk_skia_clip_to_row (w, row, TEXT_AREA, canvas);
	  pgtk_skia_set_paint_color (f, color, false);

	  if (kind == BAR_CURSOR)
	    {
	      int x
		= WINDOW_TEXT_TO_FRAME_PIXEL_X (w, w->phys_cursor.x);

	      if (width < 0)
		width = FRAME_CURSOR_WIDTH (f);
	      width = min (cursor_glyph->pixel_width, width);

	      w->phys_cursor_width = width;

	      /* If the character under cursor is R2L, draw the bar
		 cursor on the right of its glyph, rather than on the
		 left.  */
	      if ((cursor_glyph->resolved_level & 1) != 0)
		x += cursor_glyph->pixel_width - width;

	      emacs_skia_irect_t rect
		= { x, WINDOW_TO_FRAME_PIXEL_Y (w, w->phys_cursor.y),
		    x + width,
		    WINDOW_TO_FRAME_PIXEL_Y (w, w->phys_cursor.y)
		      + row->height };
	      emacs_skia_canvas_draw_irect (canvas, &rect,
					    FRAME_SKIA_PAINT (f));
	    }
	  else /* HBAR_CURSOR */
	    {
	      int dummy_x, dummy_y, dummy_h;
	      int x
		= WINDOW_TEXT_TO_FRAME_PIXEL_X (w, w->phys_cursor.x);

	      if (width < 0)
		width = row->height;

	      width = min (row->height, width);

	      get_phys_cursor_geometry (w, row, cursor_glyph,
					&dummy_x, &dummy_y, &dummy_h);

	      if ((cursor_glyph->resolved_level & 1) != 0
		  && cursor_glyph->pixel_width
		       > w->phys_cursor_width - 1)
		x += cursor_glyph->pixel_width - w->phys_cursor_width
		     + 1;

	      int y = WINDOW_TO_FRAME_PIXEL_Y (w, w->phys_cursor.y
						    + row->height
						    - width);
	      emacs_skia_irect_t rect
		= { x, y, x + w->phys_cursor_width - 1, y + width };
	      emacs_skia_canvas_draw_irect (canvas, &rect,
					    FRAME_SKIA_PAINT (f));
	    }

	  pgtk_end_skia_clip (f);
	}
#else
      cairo_t *cr = pgtk_begin_cr_clip (f);

      pgtk_clip_to_row (w, row, TEXT_AREA, cr);

      if (kind == BAR_CURSOR)
	{
	  int x = WINDOW_TEXT_TO_FRAME_PIXEL_X (w, w->phys_cursor.x);

	  if (width < 0)
	    width = FRAME_CURSOR_WIDTH (f);
	  width = min (cursor_glyph->pixel_width, width);

	  w->phys_cursor_width = width;

	  /* If the character under cursor is R2L, draw the bar cursor
	     on the right of its glyph, rather than on the left.  */
	  if ((cursor_glyph->resolved_level & 1) != 0)
	    x += cursor_glyph->pixel_width - width;

	  pgtk_fill_rectangle (f, color, x,
			       WINDOW_TO_FRAME_PIXEL_Y (w,
							w->phys_cursor
							  .y),
			       width, row->height, false);
	}
      else /* HBAR_CURSOR */
	{
	  int dummy_x, dummy_y, dummy_h;
	  int x = WINDOW_TEXT_TO_FRAME_PIXEL_X (w, w->phys_cursor.x);

	  if (width < 0)
	    width = row->height;

	  width = min (row->height, width);

	  get_phys_cursor_geometry (w, row, cursor_glyph, &dummy_x,
				    &dummy_y, &dummy_h);

	  if ((cursor_glyph->resolved_level & 1) != 0
	      && cursor_glyph->pixel_width > w->phys_cursor_width - 1)
	    x += cursor_glyph->pixel_width - w->phys_cursor_width + 1;
	  pgtk_fill_rectangle (f, color, x,
			       WINDOW_TO_FRAME_PIXEL_Y (w,
							w->phys_cursor
							    .y
							  + row
							      ->height
							  - width),
			       w->phys_cursor_width - 1, width,
			       false);
	}

      pgtk_end_cr_clip (f);
#endif
    }
}

/* RIF: Draw cursor on window W.  */

static void
pgtk_draw_window_cursor (struct window *w,
			 struct glyph_row *glyph_row, int x, int y,
			 enum text_cursor_kinds cursor_type,
			 int cursor_width, bool on_p, bool active_p)
{
  struct frame *f = XFRAME (w->frame);

  if (on_p)
    {
      w->phys_cursor_type = cursor_type;
      w->phys_cursor_on_p = true;

      if (glyph_row->exact_window_width_line_p
	  && (glyph_row->reversed_p
		? (w->phys_cursor.hpos < 0)
		: (w->phys_cursor.hpos
		   >= glyph_row->used[TEXT_AREA])))
	{
	  glyph_row->cursor_in_fringe_p = true;
	  draw_fringe_bitmap (w, glyph_row, glyph_row->reversed_p);
	}
      else
	{
	  switch (cursor_type)
	    {
	    case HOLLOW_BOX_CURSOR:
	      pgtk_draw_hollow_cursor (w, glyph_row);
	      break;

	    case FILLED_BOX_CURSOR:
	      draw_phys_cursor_glyph (w, glyph_row, DRAW_CURSOR);
	      break;

	    case BAR_CURSOR:
	      pgtk_draw_bar_cursor (w, glyph_row, cursor_width,
				    BAR_CURSOR);
	      break;

	    case HBAR_CURSOR:
	      pgtk_draw_bar_cursor (w, glyph_row, cursor_width,
				    HBAR_CURSOR);
	      break;

	    case NO_CURSOR:
	      w->phys_cursor_width = 0;
	      break;

	    default:
	      emacs_abort ();
	    }
	}

      if (w == XWINDOW (f->selected_window))
	{
	  int frame_x = (WINDOW_TO_FRAME_PIXEL_X (w, x)
			 + WINDOW_LEFT_FRINGE_WIDTH (w)
			 + WINDOW_LEFT_MARGIN_WIDTH (w));
	  int frame_y = WINDOW_TO_FRAME_PIXEL_Y (w, y);
	  pgtk_im_set_cursor_location (f, frame_x, frame_y,
				       w->phys_cursor_width,
				       w->phys_cursor_height);
	}
    }
}

static void
pgtk_copy_bits (struct frame *f, cairo_rectangle_t *src_rect,
		cairo_rectangle_t *dst_rect)
{
#ifdef USE_SKIA
  /* Skia version: snapshot the source region and draw to destination.

     The Skia surface is at device-pixel resolution (logical * scale),
     but src_rect/dst_rect are in logical pixel coordinates.  The
     surface snapshot API operates in device pixels, so we must scale
     the snapshot rectangle.  When drawing back, we use drawImageRect
     to map the device-pixel snapshot into the logical-pixel
     destination (the canvas scale transform then maps to device
     pixels).  */
  emacs_skia_surface_t *skia_surface = FRAME_SKIA_SURFACE (f);
  if (skia_surface)
    {
      /* Flush the surface to ensure all pending draws are committed
	 before taking a snapshot.  Without this, we might read stale
	 content if Skia has buffered draw operations.  */
      emacs_skia_surface_flush (skia_surface);

      /* Scale source coordinates to device pixels for the surface
	 snapshot.  The surface is at device-pixel resolution, so
	 logical coordinates must be multiplied by the GTK scale
	 factor.  */
      int scale = FRAME_GL_DRAWING_AREA (f)
	? gtk_widget_get_scale_factor (FRAME_GL_DRAWING_AREA (f))
	: 1;

      emacs_skia_irect_t src_irect
	= { (int) (src_rect->x * scale),
	    (int) (src_rect->y * scale),
	    (int) ((src_rect->x + src_rect->width) * scale),
	    (int) ((src_rect->y + src_rect->height) * scale) };

      emacs_skia_image_t *snapshot
	= emacs_skia_surface_make_image_snapshot_rect (skia_surface,
						       &src_irect);
      if (snapshot)
	{
	  emacs_skia_canvas_t *canvas = pgtk_begin_skia_clip (f);
	  if (!canvas)
	    {
	      emacs_skia_image_destroy (snapshot);
	      return;
	    }

	  /* Use a fresh paint to avoid state pollution from previous
	     drawing operations.  The shared paint might have color,
	     alpha, or other properties set that could affect the
	     blit.  */
	  emacs_skia_paint_t *copy_paint = emacs_skia_paint_create ();
	  if (!copy_paint)
	    {
	      pgtk_end_skia_clip (f);
	      emacs_skia_image_destroy (snapshot);
	      return;
	    }
	  emacs_skia_paint_set_blend_mode (copy_paint,
					   EMACS_SKIA_BLEND_SRC);

	  /* Draw the device-pixel snapshot into the logical-pixel
	     destination rectangle.  The canvas has a scale transform
	     that maps logical to device pixels, so we must specify
	     the destination in logical coordinates.  drawImageRect
	     handles the scaling from the snapshot's device-pixel
	     dimensions to the logical destination size.  Use nearest
	     neighbor sampling (smooth=false) for exact pixel copies
	     without color interpolation artifacts.  */
	  emacs_skia_rect_t dst_sk
	    = { (float) dst_rect->x, (float) dst_rect->y,
		(float) (dst_rect->x + dst_rect->width),
		(float) (dst_rect->y + dst_rect->height) };
	  emacs_skia_canvas_draw_image_rect (canvas, snapshot,
					     NULL, &dst_sk,
					     false, copy_paint);

	  emacs_skia_paint_destroy (copy_paint);
	  pgtk_end_skia_clip (f);
	  emacs_skia_image_destroy (snapshot);
	}
    }
#else
  cairo_t *cr;
  cairo_surface_t *surface; /* temporary surface */

  surface = cairo_surface_create_similar (FRAME_CR_SURFACE (f),
					  CAIRO_CONTENT_COLOR_ALPHA,
					  (int) src_rect->width,
					  (int) src_rect->height);

  cr = cairo_create (surface);
  cairo_set_source_surface (cr, FRAME_CR_SURFACE (f), -src_rect->x,
			    -src_rect->y);
  cairo_rectangle (cr, 0, 0, src_rect->width, src_rect->height);
  cairo_clip (cr);
  cairo_paint (cr);
  cairo_destroy (cr);

  cr = pgtk_begin_cr_clip (f);
  cairo_set_source_surface (cr, surface, dst_rect->x, dst_rect->y);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_rectangle (cr, dst_rect->x, dst_rect->y, dst_rect->width,
		   dst_rect->height);
  cairo_clip (cr);
  cairo_paint (cr);
  pgtk_end_cr_clip (f);

  cairo_surface_destroy (surface);
#endif
}

/* Scroll part of the display as described by RUN.  */

static void
pgtk_scroll_run (struct window *w, struct run *run)
{
  struct frame *f = XFRAME (w->frame);
  int x, y, width, height, from_y, to_y, bottom_y;

  /* Get frame-relative bounding box of the text display area of W,
     without mode lines.  Include in this box the left and right
     fringe of W.  */
  window_box (w, ANY_AREA, &x, &y, &width, &height);

  from_y = WINDOW_TO_FRAME_PIXEL_Y (w, run->current_y);
  to_y = WINDOW_TO_FRAME_PIXEL_Y (w, run->desired_y);
  bottom_y = y + height;

  if (to_y < from_y)
    {
      /* Scrolling up.  Make sure we don't copy part of the mode
	 line at the bottom.  */
      if (from_y + run->height > bottom_y)
	height = bottom_y - from_y;
      else
	height = run->height;
    }
  else
    {
      /* Scrolling down.  Make sure we don't copy over the mode line.
	 at the bottom.  */
      if (to_y + run->height > bottom_y)
	height = bottom_y - to_y;
      else
	height = run->height;
    }

  block_input ();

#ifdef HAVE_XWIDGETS
  /* "Copy" xwidget views in the area that will be scrolled.  */
  GtkWidget *tem, *parent = FRAME_GTK_WIDGET (f);
  GList *children
    = gtk_container_get_children (GTK_CONTAINER (parent));
  GList *iter;
  struct xwidget_view *view;

  for (iter = children; iter; iter = iter->next)
    {
      tem = iter->data;
      view = g_object_get_data (G_OBJECT (tem), XG_XWIDGET_VIEW);

      if (view && !view->hidden)
	{
	  int window_y = view->y + view->clip_top;
	  int window_height = view->clip_bottom - view->clip_top;

	  Emacs_Rectangle r1, r2, result;
	  r1.x = w->pixel_left;
	  r1.y = from_y;
	  r1.width = w->pixel_width;
	  r1.height = height;
	  r2 = r1;
	  r2.y = window_y;
	  r2.height = window_height;

	  /* The window is offscreen, just unmap it.  */
	  if (window_height == 0)
	    {
	      view->hidden = true;
	      gtk_widget_hide (tem);
	      continue;
	    }

	  bool intersects_p
	    = gui_intersect_rectangles (&r1, &r2, &result);

	  if (XWINDOW (view->w) == w && intersects_p)
	    {
	      int y = view->y + (to_y - from_y);
	      int text_area_x, text_area_y, text_area_width,
		text_area_height;
	      int clip_top, clip_bottom;

	      window_box (w, view->area, &text_area_x, &text_area_y,
			  &text_area_width, &text_area_height);

	      view->y = y;

	      clip_top = 0;
	      clip_bottom = XXWIDGET (view->model)->height;

	      if (y < text_area_y)
		clip_top = text_area_y - y;

	      if ((y + clip_bottom)
		  > (text_area_y + text_area_height))
		{
		  clip_bottom -= (y + clip_bottom)
				 - (text_area_y + text_area_height);
		}

	      view->clip_top = clip_top;
	      view->clip_bottom = clip_bottom;

	      /* This means the view has moved offscreen.  Unmap
		 it and hide it here.  */
	      if ((view->clip_bottom - view->clip_top) <= 0)
		{
		  view->hidden = true;
		  gtk_widget_hide (tem);
		}
	      else
		{
		  gtk_fixed_move (GTK_FIXED (FRAME_GTK_WIDGET (f)),
				  tem, view->x + view->clip_left,
				  view->y + view->clip_top);
		  gtk_widget_set_size_request (tem,
					       view->clip_right
						 - view->clip_left,
					       view->clip_bottom
						 - view->clip_top);
		  gtk_widget_queue_allocate (tem);
		}
	    }
	}
    }

  g_list_free (children);
#endif

  /* Cursor off.  Will be switched on again in x_update_window_end. */
  gui_clear_cursor (w);

  {
    cairo_rectangle_t src_rect = { x, from_y, width, height };
    cairo_rectangle_t dst_rect = { x, to_y, width, height };
    pgtk_copy_bits (f, &src_rect, &dst_rect);
  }

  unblock_input ();
}

/* Icons.  */

static bool
pgtk_bitmap_icon (struct frame *f, Lisp_Object file)
{
  /* This code has never worked anyway for the reason that Wayland
     uses icons set within desktop files, and has been disabled
     because leaving it intact would require image.c to retain a
     reference to a GdkPixbuf (which are no longer used) within new
     bitmaps.  */
#if 0
  ptrdiff_t bitmap_id;

  if (FRAME_GTK_WIDGET (f) == 0)
    return true;

  /* Free up our existing icon bitmap and mask if any.  */
  if (f->output_data.pgtk->icon_bitmap > 0)
    image_destroy_bitmap (f, f->output_data.pgtk->icon_bitmap);
  f->output_data.pgtk->icon_bitmap = 0;

  if (STRINGP (file))
    {
      /* Use gtk_window_set_icon_from_file () if available,
	 It's not restricted to bitmaps */
      if (xg_set_icon (f, file))
	return false;
      bitmap_id = image_create_bitmap_from_file (f, file);
    }
  else
    {
      /* Create the GNU bitmap and mask if necessary.  */
      if (FRAME_DISPLAY_INFO (f)->icon_bitmap_id < 0)
	{
	  ptrdiff_t rc = -1;

          if (xg_set_icon (f, xg_default_icon_file)
              || xg_set_icon_from_xpm_data (f, gnu_xpm_bits))
            {
              FRAME_DISPLAY_INFO (f)->icon_bitmap_id = -2;
              return false;
            }

	  /* If all else fails, use the (black and white) xbm image. */
	  if (rc == -1)
	    {
              rc = image_create_bitmap_from_data (f,
                                                  (char *) gnu_xbm_bits,
                                                  gnu_xbm_width,
                                                  gnu_xbm_height);
	      if (rc == -1)
		return true;

	      FRAME_DISPLAY_INFO (f)->icon_bitmap_id = rc;
	    }
	}

      /* The first time we create the GNU bitmap and mask,
	 this increments the ref-count one extra time.
	 As a result, the GNU bitmap and mask are never freed.
	 That way, we don't have to worry about allocating it again.  */
      image_reference_bitmap (f, FRAME_DISPLAY_INFO (f)->icon_bitmap_id);

      bitmap_id = FRAME_DISPLAY_INFO (f)->icon_bitmap_id;
    }

  f->output_data.pgtk->icon_bitmap = bitmap_id;
#endif /* 0 */
  return false;
}

/* Make the x-window of frame F use a rectangle with text.
   Use ICON_NAME as the text.  */

bool
pgtk_text_icon (struct frame *f, const char *icon_name)
{
  if (FRAME_GTK_OUTER_WIDGET (f))
    {
      gtk_window_set_icon (GTK_WINDOW (FRAME_GTK_OUTER_WIDGET (f)),
			   NULL);
      gtk_window_set_title (GTK_WINDOW (FRAME_GTK_OUTER_WIDGET (f)),
			    icon_name);
    }

  return false;
}

/***********************************************************************
		    Starting and ending an update
 ***********************************************************************/

/* Start an update of frame F.  This function is installed as a hook
   for update_begin, i.e. it is called when update_begin is called.
   This function is called prior to calls to x_update_window_begin for
   each window being updated.  Currently, there is nothing to do here
   because all interesting stuff is done on a window basis.  */

static void
pgtk_update_begin (struct frame *f)
{
  pgtk_clear_under_internal_border (f);
}

/* Draw a vertical window border from (x,y0) to (x,y1)  */

static void
pgtk_draw_vertical_window_border (struct window *w, int x, int y0,
				  int y1)
{
  struct frame *f = XFRAME (WINDOW_FRAME (w));
  struct face *face;

  face = FACE_FROM_ID_OR_NULL (f, VERTICAL_BORDER_FACE_ID);
  unsigned long color
    = face ? face->foreground : FRAME_FOREGROUND_PIXEL (f);

#ifdef USE_SKIA
  pgtk_skia_fill_rectangle (f, color, x, y0, 1, y1 - y0, false);
#else
  cairo_t *cr;
  cr = pgtk_begin_cr_clip (f);
  pgtk_set_cr_source_with_color (f, color, false);
  cairo_rectangle (cr, x, y0, 1, y1 - y0);
  cairo_fill (cr);
  pgtk_end_cr_clip (f);
#endif
}

/* Draw a window divider from (x0,y0) to (x1,y1)  */

static void
pgtk_draw_window_divider (struct window *w, int x0, int x1, int y0,
			  int y1)
{
  struct frame *f = XFRAME (WINDOW_FRAME (w));
  struct face *face
    = FACE_FROM_ID_OR_NULL (f, WINDOW_DIVIDER_FACE_ID);
  struct face *face_first
    = FACE_FROM_ID_OR_NULL (f, WINDOW_DIVIDER_FIRST_PIXEL_FACE_ID);
  struct face *face_last
    = FACE_FROM_ID_OR_NULL (f, WINDOW_DIVIDER_LAST_PIXEL_FACE_ID);
  unsigned long color
    = face ? face->foreground : FRAME_FOREGROUND_PIXEL (f);
  unsigned long color_first
    = (face_first ? face_first->foreground
		  : FRAME_FOREGROUND_PIXEL (f));
  unsigned long color_last = (face_last ? face_last->foreground
					: FRAME_FOREGROUND_PIXEL (f));
  bool alpha = false;

#ifdef USE_SKIA
  if (y1 - y0 > x1 - x0 && x1 - x0 > 2)
    /* Vertical.  */
    {
      pgtk_skia_fill_rectangle (f, color_first, x0, y0, 1, y1 - y0,
				alpha);
      pgtk_skia_fill_rectangle (f, color, x0 + 1, y0, x1 - x0 - 2,
				y1 - y0, alpha);
      pgtk_skia_fill_rectangle (f, color_last, x1 - 1, y0, 1, y1 - y0,
				alpha);
    }
  else if (x1 - x0 > y1 - y0 && y1 - y0 > 3)
    /* Horizontal.  */
    {
      pgtk_skia_fill_rectangle (f, color_first, x0, y0, x1 - x0, 1,
				alpha);
      pgtk_skia_fill_rectangle (f, color, x0, y0 + 1, x1 - x0,
				y1 - y0 - 2, alpha);
      pgtk_skia_fill_rectangle (f, color_last, x0, y1 - 1, x1 - x0, 1,
				alpha);
    }
  else
    {
      pgtk_skia_fill_rectangle (f, color, x0, y0, x1 - x0, y1 - y0,
				alpha);
    }
#else
  cairo_t *cr = pgtk_begin_cr_clip (f);

  if (y1 - y0 > x1 - x0 && x1 - x0 > 2)
    /* Vertical.  */
    {
      pgtk_set_cr_source_with_color (f, color_first, false);
      cairo_rectangle (cr, x0, y0, 1, y1 - y0);
      cairo_fill (cr);
      pgtk_set_cr_source_with_color (f, color, false);
      cairo_rectangle (cr, x0 + 1, y0, x1 - x0 - 2, y1 - y0);
      cairo_fill (cr);
      pgtk_set_cr_source_with_color (f, color_last, false);
      cairo_rectangle (cr, x1 - 1, y0, 1, y1 - y0);
      cairo_fill (cr);
    }
  else if (x1 - x0 > y1 - y0 && y1 - y0 > 3)
    /* Horizontal.  */
    {
      pgtk_set_cr_source_with_color (f, color_first, false);
      cairo_rectangle (cr, x0, y0, x1 - x0, 1);
      cairo_fill (cr);
      pgtk_set_cr_source_with_color (f, color, false);
      cairo_rectangle (cr, x0, y0 + 1, x1 - x0, y1 - y0 - 2);
      cairo_fill (cr);
      pgtk_set_cr_source_with_color (f, color_last, false);
      cairo_rectangle (cr, x0, y1 - 1, x1 - x0, 1);
      cairo_fill (cr);
    }
  else
    {
      pgtk_set_cr_source_with_color (f, color, false);
      cairo_rectangle (cr, x0, y0, x1 - x0, y1 - y0);
      cairo_fill (cr);
    }

  pgtk_end_cr_clip (f);
#endif
}

/* End update of frame F.  This function is installed as a hook in
   update_end.  */

static void
pgtk_update_end (struct frame *f)
{
  /* Mouse highlight may be displayed again.  */
  MOUSE_HL_INFO (f)->mouse_face_defer = false;
}

static void
pgtk_frame_up_to_date (struct frame *f)
{
  block_input ();
  FRAME_MOUSE_UPDATE (f);

#ifdef USE_SKIA
  /* For Skia GL rendering, bypass the buffer_flipping_blocked check
     since we don't use Cairo's double-buffering mechanism.  */
  if (FRAME_GDK_GL_CONTEXT (f) && FRAME_GL_TEXTURE (f))
    {
      /* Frame pacing: limit to ~60 FPS (16.6ms) to reduce flickering
	 over waypipe.  Skip frame if we rendered too recently.  */
      gint64 now = g_get_monotonic_time ();
      gint64 elapsed = now - FRAME_LAST_RENDER_TIME (f);
      /* 16000 microseconds = 16ms ~ 60 FPS.  Use 8ms for smoother
	 response.  */
      const gint64 min_frame_interval = 8000;

      if (FRAME_LAST_RENDER_TIME (f) > 0
	  && elapsed < min_frame_interval)
	{
	  /* Too soon - but we still need to render eventually.
	     Queue a render anyway to ensure the update isn't lost.
	     The render callback will handle the actual
	     synchronization.  Previously this just returned, which
	     could cause visual freezes if no more
	     pgtk_frame_up_to_date calls occurred.  */
	  if (FRAME_GL_DRAWING_AREA (f))
	    gtk_widget_queue_draw (FRAME_GL_DRAWING_AREA (f));
	  unblock_input ();
	  return;
	}

      FRAME_LAST_RENDER_TIME (f) = now;

      /* Make GL context current and validate it before flushing.
	 See pgtk_gl_context_valid_p for why this is needed.  */
      gdk_gl_context_make_current (FRAME_GDK_GL_CONTEXT (f));

      if (pgtk_gl_context_valid_p (f))
	{
	  if (FRAME_SKIA_SURFACE (f))
	    emacs_skia_surface_flush (FRAME_SKIA_SURFACE (f));
	  if (FRAME_SKIA_GL_CONTEXT (f))
	    emacs_skia_gl_context_flush
	      (FRAME_SKIA_GL_CONTEXT (f));
	}
      else
	SET_FRAME_GARBAGED (f);

      /* Queue a draw on the drawing area.  */
      if (FRAME_GL_DRAWING_AREA (f))
	gtk_widget_queue_draw (FRAME_GL_DRAWING_AREA (f));
      unblock_input ();
      return;
    }
#else /* USE_CAIRO */
  if (!buffer_flipping_blocked_p ())
    {
      flip_cr_context (f);
      gtk_widget_queue_draw (FRAME_GTK_WIDGET (f));
    }
#endif
  unblock_input ();
}

/* Return the current position of the mouse.
   *FP should be a frame which indicates which display to ask about.

   If the mouse movement started in a scroll bar, set *FP,
   *BAR_WINDOW, and *PART to the frame, window, and scroll bar part
   that the mouse is over.  Set *X and *Y to the portion and whole of
   the mouse's position on the scroll bar.

   If the mouse movement started elsewhere, set *FP to the frame the
   mouse is on, *BAR_WINDOW to nil, and *X and *Y to the character
   cell the mouse is over.

   Set *TIMESTAMP to the server time-stamp for the time at which the
   mouse was at this position.

   Don't store anything if we don't have a valid set of values to
   report.

   This clears the mouse_moved flag, so we can wait for the next mouse
   movement.  */

static void
pgtk_mouse_position (struct frame **fp, int insist,
		     Lisp_Object *bar_window,
		     enum scroll_bar_part *part, Lisp_Object *x,
		     Lisp_Object *y, Time *timestamp)
{
  struct frame *f1;
  struct pgtk_display_info *dpyinfo = FRAME_DISPLAY_INFO (*fp);
  int win_x, win_y;
  GdkSeat *seat;
  GdkDevice *device;
  GdkModifierType mask;
  GdkWindow *win;
  bool return_frame_flag = false;

  block_input ();

  Lisp_Object frame, tail;

  /* Clear the mouse-moved flag for every frame on this display.  */
  FOR_EACH_FRAME (tail, frame)
  if (FRAME_PGTK_P (XFRAME (frame))
      && FRAME_X_DISPLAY (XFRAME (frame)) == FRAME_X_DISPLAY (*fp))
    XFRAME (frame)->mouse_moved = false;

  dpyinfo->last_mouse_scroll_bar = NULL;

  if (gui_mouse_grabbed (dpyinfo)
      && (!EQ (track_mouse, Qdropping)
	  && !EQ (track_mouse, Qdrag_source)))
    f1 = dpyinfo->last_mouse_frame;
  else
    {
      f1 = *fp;
      win = gtk_widget_get_window (FRAME_GTK_WIDGET (*fp));
      seat = gdk_display_get_default_seat (dpyinfo->gdpy);
      device = gdk_seat_get_pointer (seat);
      win = gdk_window_get_device_position (win, device, &win_x,
					    &win_y, &mask);
      if (win != NULL)
	f1 = pgtk_any_window_to_frame (win);
      else
	{
	  f1 = SELECTED_FRAME ();

	  if (!FRAME_PGTK_P (f1))
	    f1 = dpyinfo->last_mouse_frame;

	  return_frame_flag = EQ (track_mouse, Qdrag_source);
	}
    }

  /* F1 can be a terminal frame.  (Bug#50322) */
  if (f1 == NULL || !FRAME_PGTK_P (f1))
    {
      unblock_input ();
      return;
    }

  win = gtk_widget_get_window (FRAME_GTK_WIDGET (f1));
  seat = gdk_display_get_default_seat (dpyinfo->gdpy);
  device = gdk_seat_get_pointer (seat);

  win = gdk_window_get_device_position (win, device, &win_x, &win_y,
					&mask);

  if (f1 != NULL)
    {
      remember_mouse_glyph (f1, win_x, win_y,
			    &dpyinfo->last_mouse_glyph);
      dpyinfo->last_mouse_glyph_frame = f1;

      *bar_window = Qnil;
      *part = 0;
      *fp = !return_frame_flag ? f1 : NULL;
      XSETINT (*x, win_x);
      XSETINT (*y, win_y);
      *timestamp = dpyinfo->last_mouse_movement_time;
    }

  unblock_input ();
}

/* Fringe bitmaps.  */

static int max_fringe_bmp = 0;
#ifdef USE_SKIA
static emacs_skia_image_t **fringe_bmp_skia = 0;
#else
static cairo_pattern_t **fringe_bmp = 0;
#endif

static void
pgtk_define_fringe_bitmap (int which, unsigned short *bits, int h,
			   int wd)
{
  int i;

  if (which >= max_fringe_bmp)
    {
      i = max_fringe_bmp;
      max_fringe_bmp = which + 20;
#ifdef USE_SKIA
      fringe_bmp_skia
	= xrealloc (fringe_bmp_skia,
		    max_fringe_bmp * sizeof (emacs_skia_image_t *));
#else
      fringe_bmp
	= xrealloc (fringe_bmp,
		    max_fringe_bmp * sizeof (cairo_pattern_t *));
#endif
      while (i < max_fringe_bmp)
	{
#ifdef USE_SKIA
	  fringe_bmp_skia[i] = 0;
#else
	  fringe_bmp[i] = 0;
#endif
	  i++;
	}
    }

  block_input ();

#ifdef USE_SKIA
  {
    /* Convert the bits to a format suitable for Skia.
       The bits are 16-bit values representing each row of the fringe.
     */
    int stride = (wd + 7) / 8;
    unsigned char *bitmap_data = xmalloc (stride * h);

    for (i = 0; i < h; i++)
      {
	/* Copy the low bytes of each 16-bit value.  */
	if (stride >= 2)
	  {
	    bitmap_data[i * stride] = bits[i] & 0xff;
	    bitmap_data[i * stride + 1] = (bits[i] >> 8) & 0xff;
	  }
	else
	  bitmap_data[i * stride] = bits[i] & 0xff;
      }

    fringe_bmp_skia[which]
      = emacs_skia_image_create_from_bitmap (bitmap_data, wd, h,
					     stride);
    xfree (bitmap_data);
  }
#else /* USE_CAIRO */
  {
    int stride;
    cairo_surface_t *surface;
    unsigned char *data;
    cairo_pattern_t *pattern;

    surface = cairo_image_surface_create (CAIRO_FORMAT_A1, wd, h);
    stride = cairo_image_surface_get_stride (surface);
    data = cairo_image_surface_get_data (surface);

    for (i = 0; i < h; i++)
      {
	*((unsigned short *) data) = bits[i];
	data += stride;
      }

    cairo_surface_mark_dirty (surface);
    pattern = cairo_pattern_create_for_surface (surface);
    cairo_surface_destroy (surface);

    fringe_bmp[which] = pattern;
  }
#endif

  unblock_input ();
}

static void
pgtk_destroy_fringe_bitmap (int which)
{
  if (which >= max_fringe_bmp)
    return;

  block_input ();

#ifdef USE_SKIA
  if (fringe_bmp_skia[which])
    {
      emacs_skia_image_destroy (fringe_bmp_skia[which]);
      fringe_bmp_skia[which] = 0;
    }
#else
  if (fringe_bmp[which])
    {
      cairo_pattern_destroy (fringe_bmp[which]);
      fringe_bmp[which] = 0;
    }
#endif

  unblock_input ();
}

#ifdef USE_CAIRO
static void
pgtk_clip_to_row (struct window *w, struct glyph_row *row,
		  enum glyph_row_area area, cairo_t *cr)
{
  int window_x, window_y, window_width;
  cairo_rectangle_int_t rect;

  window_box (w, area, &window_x, &window_y, &window_width, 0);

  rect.x = window_x;
  rect.y = WINDOW_TO_FRAME_PIXEL_Y (w, max (0, row->y));
  rect.y = max (rect.y, window_y);
  rect.width = window_width;
  rect.height = row->visible_height;

  cairo_rectangle (cr, rect.x, rect.y, rect.width, rect.height);
  cairo_clip (cr);
}
#endif

#ifdef USE_SKIA
static void
pgtk_skia_clip_to_row (struct window *w, struct glyph_row *row,
		       enum glyph_row_area area,
		       emacs_skia_canvas_t *canvas)
{
  int window_x, window_y, window_width;
  emacs_skia_rect_t rect;

  window_box (w, area, &window_x, &window_y, &window_width, 0);

  rect.left = window_x;
  rect.top = WINDOW_TO_FRAME_PIXEL_Y (w, max (0, row->y));
  rect.top = max (rect.top, window_y);
  rect.right = rect.left + window_width;
  rect.bottom = rect.top + row->visible_height;

  emacs_skia_canvas_clip_rect (canvas, &rect);
}
#endif

static void
pgtk_draw_fringe_bitmap (struct window *w, struct glyph_row *row,
			 struct draw_fringe_bitmap_params *p)
{
  struct frame *f = XFRAME (WINDOW_FRAME (w));
  struct face *face = p->face;

#ifdef USE_SKIA
  /* Use pgtk_begin_skia_clip to ensure canvas/paint are initialized.
   */
  emacs_skia_canvas_t *canvas = pgtk_begin_skia_clip (f);
  if (!canvas)
    return;
  emacs_skia_paint_t *paint = FRAME_SKIA_PAINT (f);

  /* For child frames, use the default face background for fringes to
     ensure consistency with the main content area.  The fringe face
     may have a slightly different background due to frame parameter
     inheritance, causing visible color banding with GL rendering.  */
  unsigned long fringe_bg = face->background;
  if (FRAME_PARENT_FRAME (f))
    {
      struct face *default_face = FACE_FROM_ID (f, DEFAULT_FACE_ID);
      if (default_face)
	fringe_bg = default_face->background;
    }

  /* Must clip because of partially visible lines.  */
  pgtk_skia_clip_to_row (w, row, ANY_AREA, canvas);

  if (p->bx >= 0 && !p->overlay_p)
    pgtk_skia_fill_rectangle (f, fringe_bg, p->bx, p->by, p->nx,
			      p->ny, true);

  if (p->which && p->which < max_fringe_bmp
      && p->which < max_used_fringe_bitmap)
    {
      unsigned long foreground
	= (p->cursor_p
	     ? (p->overlay_p ? face->background
			     : FRAME_X_OUTPUT (f)->cursor_color)
	     : face->foreground);

      if (!fringe_bmp_skia[p->which])
	gui_define_fringe_bitmap (f, p->which);

      if (fringe_bmp_skia[p->which])
	{
	  emacs_skia_image_t *img = fringe_bmp_skia[p->which];

	  /* Draw background if not overlay.  Use the consistent
	     fringe_bg color computed above to avoid color banding in
	     child frames.  */
	  if (!p->overlay_p)
	    {
	      emacs_skia_rect_t bg_rect
		= { p->x, p->y, p->x + p->wd, p->y + p->h };
	      emacs_skia_paint_set_color (paint, pgtk_color_to_skia (
						   fringe_bg));
	      emacs_skia_paint_set_blend_mode (
		paint, EMACS_SKIA_BLEND_SRC_OVER);
	      emacs_skia_paint_set_stroke (paint, false);
	      emacs_skia_canvas_draw_rect (canvas, &bg_rect, paint);
	    }

	  /* Draw the fringe bitmap as a mask with foreground color.
	     The fringe bitmap is an alpha-only (A8) image.  When
	     drawn with drawImage, Skia uses the paint's color as RGB
	     and the image's alpha values as opacity.  */
	  emacs_skia_paint_set_color (paint, pgtk_color_to_skia (
					       foreground));
	  emacs_skia_paint_set_blend_mode (paint,
					   EMACS_SKIA_BLEND_SRC_OVER);

	  emacs_skia_rect_t fringe_rect
	    = { p->x, p->y, p->x + p->wd, p->y + p->h };
	  emacs_skia_canvas_save (canvas);
	  emacs_skia_canvas_clip_rect (canvas, &fringe_rect);

	  /* Draw the alpha image at the correct position.  The image
	     alpha (0 or 255) controls where the foreground color
	     appears.  */
	  emacs_skia_canvas_draw_image_with_sampling (
	    canvas, img, (float) p->x, (float) (p->y - p->dh), false,
	    paint);
	  emacs_skia_canvas_restore (canvas);
	}
    }

  pgtk_end_skia_clip (f);
#else
  cairo_t *cr = pgtk_begin_cr_clip (f);

  /* Must clip because of partially visible lines.  */
  pgtk_clip_to_row (w, row, ANY_AREA, cr);

  if (p->bx >= 0 && !p->overlay_p)
    fill_background_by_face (f, face, p->bx, p->by, p->nx, p->ny);

  if (p->which && p->which < max_fringe_bmp
      && p->which < max_used_fringe_bitmap)
    {
      Emacs_GC gcv;

      if (!fringe_bmp[p->which])
	{
	  /* This fringe bitmap is known to fringe.c, but lacks the
	     cairo_pattern_t pattern which shadows that bitmap.  This
	     is typical to define-fringe-bitmap being called when the
	     selected frame was not a GUI frame, for example, when
	     packages that define fringe bitmaps are loaded by a
	     daemon Emacs.  Create the missing pattern now.  */
	  gui_define_fringe_bitmap (f, p->which);
	}

      gcv.foreground
	= (p->cursor_p
	     ? (p->overlay_p ? face->background
			     : FRAME_X_OUTPUT (f)->cursor_color)
	     : face->foreground);
      gcv.background = face->background;
      pgtk_cr_draw_image (f, &gcv, fringe_bmp[p->which], 0, p->dh,
			  p->wd, p->h, p->x, p->y, p->overlay_p);
    }

  pgtk_end_cr_clip (f);
#endif
}

static struct atimer *hourglass_atimer = NULL;
static int hourglass_enter_count = 0;

static void
hourglass_cb (struct atimer *timer)
{
}

static void
pgtk_show_hourglass (struct frame *f)
{
  struct pgtk_output *x = FRAME_X_OUTPUT (f);
  if (x->hourglass_widget != NULL)
    gtk_widget_destroy (x->hourglass_widget);

  /* This creates a GDK_INPUT_ONLY window.  */
  x->hourglass_widget = gtk_event_box_new ();
  gtk_widget_set_has_window (x->hourglass_widget, true);
  gtk_fixed_put (GTK_FIXED (FRAME_GTK_WIDGET (f)),
		 x->hourglass_widget, 0, 0);
  gtk_widget_show (x->hourglass_widget);
  gtk_widget_set_size_request (x->hourglass_widget, 30000, 30000);
  gdk_window_raise (gtk_widget_get_window (x->hourglass_widget));
  gdk_window_set_cursor (gtk_widget_get_window (x->hourglass_widget),
			 x->hourglass_cursor);

  /* For cursor animation, we receive signals, set pending_signals,
     and wait for the signal handler to run.  */
  if (hourglass_enter_count++ == 0)
    {
      struct timespec ts = make_timespec (0, 50 * 1000 * 1000);
      if (hourglass_atimer != NULL)
	cancel_atimer (hourglass_atimer);
      hourglass_atimer
	= start_atimer (ATIMER_CONTINUOUS, ts, hourglass_cb, NULL);
    }
}

static void
pgtk_hide_hourglass (struct frame *f)
{
  struct pgtk_output *x = FRAME_X_OUTPUT (f);
  if (--hourglass_enter_count == 0)
    {
      if (hourglass_atimer != NULL)
	{
	  cancel_atimer (hourglass_atimer);
	  hourglass_atimer = NULL;
	}
    }
  if (x->hourglass_widget != NULL)
    {
      gtk_widget_destroy (x->hourglass_widget);
      x->hourglass_widget = NULL;
    }
}

/* Flushes changes to display.  */
static void
pgtk_flush_display (struct frame *f)
{
#ifdef USE_SKIA
  /* Flush Skia surface to ensure all drawing commands are complete
     before we blit to the screen.  */
  if (FRAME_SKIA_SURFACE (f))
    {
      emacs_skia_surface_flush (FRAME_SKIA_SURFACE (f));
      if (FRAME_SKIA_GL_CONTEXT (f))
	emacs_skia_gl_context_flush (FRAME_SKIA_GL_CONTEXT (f));
    }

  /* Queue a render to display the completed content.  */
  if (FRAME_GL_DRAWING_AREA (f))
    gtk_widget_queue_draw (FRAME_GL_DRAWING_AREA (f));
#endif
}

extern frame_parm_handler pgtk_frame_parm_handlers[];

static struct redisplay_interface pgtk_redisplay_interface = {
  pgtk_frame_parm_handlers,
  gui_produce_glyphs,
  gui_write_glyphs,
  gui_insert_glyphs,
  gui_clear_end_of_line,
  pgtk_scroll_run,
  pgtk_after_update_window_line,
  NULL, /* gui_update_window_begin, */
  NULL, /* gui_update_window_end, */
  pgtk_flush_display,
  gui_clear_window_mouse_face,
  gui_get_glyph_overhangs,
  gui_fix_overlapping_area,
  pgtk_draw_fringe_bitmap,
  pgtk_define_fringe_bitmap,
  pgtk_destroy_fringe_bitmap,
  pgtk_compute_glyph_string_overhangs,
  pgtk_draw_glyph_string,
  pgtk_define_frame_cursor,
  pgtk_clear_frame_area,
  pgtk_clear_under_internal_border,
  pgtk_draw_window_cursor,
  pgtk_draw_vertical_window_border,
  pgtk_draw_window_divider,
  NULL, /* pgtk_shift_glyphs_for_insert, */
  pgtk_show_hourglass,
  pgtk_hide_hourglass,
  pgtk_default_font_parameter,
};

void
pgtk_clear_frame (struct frame *f)
{
  if (!FRAME_DEFAULT_FACE (f))
    return;

  mark_window_cursors_off (XWINDOW (FRAME_ROOT_WINDOW (f)));

  block_input ();
  pgtk_clear_area (f, 0, 0, FRAME_PIXEL_WIDTH (f),
		   FRAME_PIXEL_HEIGHT (f));
  unblock_input ();
}

static void
recover_from_visible_bell (struct atimer *timer)
{
  struct frame *f = timer->client_data;

#ifdef USE_SKIA
  block_input ();

  /* Restore the pre-flash surface content from the saved snapshot.
     The snapshot is at device-pixel resolution; temporarily undo the
     canvas HiDPI scale so the image maps 1:1 to device pixels.  */
  if (FRAME_X_OUTPUT (f)->skia_image_pre_bell && FRAME_SKIA_CANVAS (f))
    {
      emacs_skia_canvas_t *canvas = FRAME_SKIA_CANVAS (f);
      int scale = FRAME_GL_DRAWING_AREA (f)
	? gtk_widget_get_scale_factor (FRAME_GL_DRAWING_AREA (f))
	: 1;
      emacs_skia_canvas_save (canvas);
      if (scale > 1)
	emacs_skia_canvas_scale (canvas, 1.0f / scale, 1.0f / scale);
      emacs_skia_canvas_draw_image (canvas,
				    FRAME_X_OUTPUT (f)->skia_image_pre_bell,
				    0, 0, NULL);
      emacs_skia_canvas_restore (canvas);

      /* Flush the restored content to the GL texture.  */
      emacs_skia_surface_flush (FRAME_SKIA_SURFACE (f));
      if (FRAME_SKIA_GL_CONTEXT (f))
	emacs_skia_gl_context_flush (FRAME_SKIA_GL_CONTEXT (f));
    }

  if (FRAME_X_OUTPUT (f)->skia_image_pre_bell)
    {
      emacs_skia_image_destroy (FRAME_X_OUTPUT (f)->skia_image_pre_bell);
      FRAME_X_OUTPUT (f)->skia_image_pre_bell = NULL;
    }

  /* Queue a draw to composite the restored content.  */
  if (FRAME_GL_DRAWING_AREA (f))
    gtk_widget_queue_draw (FRAME_GL_DRAWING_AREA (f));

  unblock_input ();
#else
  if (FRAME_X_OUTPUT (f)->cr_surface_visible_bell != NULL)
    {
      cairo_surface_destroy (
	FRAME_X_OUTPUT (f)->cr_surface_visible_bell);
      FRAME_X_OUTPUT (f)->cr_surface_visible_bell = NULL;
    }
#endif

  if (FRAME_X_OUTPUT (f)->atimer_visible_bell != NULL)
    FRAME_X_OUTPUT (f)->atimer_visible_bell = NULL;
}

/* Invert the middle quarter of the frame for .15 sec.  */

static void
pgtk_flash (struct frame *f)
{
#ifdef USE_SKIA
  emacs_skia_canvas_t *canvas;
  emacs_skia_paint_t *paint;
  int height, flash_height, flash_left, flash_right, width;
  struct timespec delay;

  if (!FRAME_SKIA_SURFACE (f) || !FRAME_SKIA_CANVAS (f))
    return;

  block_input ();

  /* Draw the flash effect directly onto the main GL-backed Skia
     surface.  The canvas already has the HiDPI scale transform, so
     all coordinates are in logical pixels (matching FRAME_PIXEL_*
     macros).  We save a snapshot before drawing so
     recover_from_visible_bell can restore the original content
     without a full redisplay (which would be racy — concurrent
     redraws during the 50ms flash window would composite on top of
     the inverted region).

     If a previous flash is still active (rapid bell invocations),
     restore the prior snapshot first to avoid double-inversion.  */
  canvas = FRAME_SKIA_CANVAS (f);

  if (FRAME_X_OUTPUT (f)->skia_image_pre_bell)
    {
      /* Restore previous pre-bell content before re-flashing.
	 The snapshot is at device-pixel resolution, but the canvas
	 has a HiDPI scale transform.  Temporarily undo the scale
	 so the image maps 1:1 to device pixels.  */
      int scale = FRAME_GL_DRAWING_AREA (f)
	? gtk_widget_get_scale_factor (FRAME_GL_DRAWING_AREA (f))
	: 1;
      emacs_skia_canvas_save (canvas);
      if (scale > 1)
	emacs_skia_canvas_scale (canvas, 1.0f / scale, 1.0f / scale);
      emacs_skia_canvas_draw_image (canvas,
				    FRAME_X_OUTPUT (f)->skia_image_pre_bell,
				    0, 0, NULL);
      emacs_skia_canvas_restore (canvas);
      emacs_skia_image_destroy (FRAME_X_OUTPUT (f)->skia_image_pre_bell);
      FRAME_X_OUTPUT (f)->skia_image_pre_bell = NULL;
    }

  /* Flush pending draws before snapshotting.  */
  emacs_skia_surface_flush (FRAME_SKIA_SURFACE (f));

  /* Save a snapshot of the clean surface for restoration.  */
  FRAME_X_OUTPUT (f)->skia_image_pre_bell
    = emacs_skia_surface_make_image_snapshot (FRAME_SKIA_SURFACE (f));

  paint = emacs_skia_paint_create ();
  if (!paint)
    {
      /* Clean up the snapshot we just saved — no timer will be
	 started, so nothing would restore/free it otherwise.  */
      if (FRAME_X_OUTPUT (f)->skia_image_pre_bell)
	{
	  emacs_skia_image_destroy (
	    FRAME_X_OUTPUT (f)->skia_image_pre_bell);
	  FRAME_X_OUTPUT (f)->skia_image_pre_bell = NULL;
	}
      unblock_input ();
      return;
    }

  /* Set up for DIFFERENCE blend mode with white color.  */
  emacs_skia_paint_set_color (paint,
			      EMACS_SKIA_COLOR_RGB (255, 255, 255));
  emacs_skia_paint_set_blend_mode (paint,
				   EMACS_SKIA_BLEND_DIFFERENCE);
  emacs_skia_paint_set_stroke (paint, false);

  /* Get the height not including a menu bar widget.  */
  height = FRAME_PIXEL_HEIGHT (f);
  /* Height of each line to flash.  */
  flash_height = FRAME_LINE_HEIGHT (f);
  /* These will be the left and right margins of the rectangles.  */
  flash_left = FRAME_INTERNAL_BORDER_WIDTH (f);
  flash_right
    = (FRAME_PIXEL_WIDTH (f) - FRAME_INTERNAL_BORDER_WIDTH (f));
  width = flash_right - flash_left;

  /* If window is tall, flash top and bottom line.  */
  if (height > 3 * FRAME_LINE_HEIGHT (f))
    {
      emacs_skia_rect_t rect1
	= { flash_left,
	    FRAME_INTERNAL_BORDER_WIDTH (f)
	      + FRAME_TOP_MARGIN_HEIGHT (f),
	    flash_left + width,
	    FRAME_INTERNAL_BORDER_WIDTH (f)
	      + FRAME_TOP_MARGIN_HEIGHT (f) + flash_height };
      emacs_skia_canvas_draw_rect (canvas, &rect1, paint);

      emacs_skia_rect_t rect2
	= { flash_left,
	    height - flash_height - FRAME_INTERNAL_BORDER_WIDTH (f)
	      - FRAME_BOTTOM_MARGIN_HEIGHT (f),
	    flash_left + width,
	    height - FRAME_INTERNAL_BORDER_WIDTH (f)
	      - FRAME_BOTTOM_MARGIN_HEIGHT (f) };
      emacs_skia_canvas_draw_rect (canvas, &rect2, paint);
    }
  else
    {
      /* If it is short, flash it all.  */
      emacs_skia_rect_t rect
	= { flash_left, FRAME_INTERNAL_BORDER_WIDTH (f),
	    flash_left + width,
	    height - FRAME_INTERNAL_BORDER_WIDTH (f) };
      emacs_skia_canvas_draw_rect (canvas, &rect, paint);
    }

  emacs_skia_paint_destroy (paint);

  /* Flush the flash into the GL texture and queue a draw so the
     compositing callback displays it immediately.  */
  emacs_skia_surface_flush (FRAME_SKIA_SURFACE (f));
  if (FRAME_SKIA_GL_CONTEXT (f))
    emacs_skia_gl_context_flush (FRAME_SKIA_GL_CONTEXT (f));
  if (FRAME_GL_DRAWING_AREA (f))
    gtk_widget_queue_draw (FRAME_GL_DRAWING_AREA (f));

  delay = make_timespec (0, 50 * 1000 * 1000);

  if (FRAME_X_OUTPUT (f)->atimer_visible_bell != NULL)
    {
      cancel_atimer (FRAME_X_OUTPUT (f)->atimer_visible_bell);
      FRAME_X_OUTPUT (f)->atimer_visible_bell = NULL;
    }

  FRAME_X_OUTPUT (f)->atimer_visible_bell
    = start_atimer (ATIMER_RELATIVE, delay, recover_from_visible_bell,
		    f);

  unblock_input ();
#else
  cairo_surface_t *surface_orig, *surface;
  cairo_t *cr;
  int width, height, flash_height, flash_left, flash_right;
  struct timespec delay;

  if (!FRAME_CR_CONTEXT (f))
    return;

  block_input ();

  surface_orig = FRAME_CR_SURFACE (f);

  width = FRAME_CR_SURFACE_DESIRED_WIDTH (f);
  height = FRAME_CR_SURFACE_DESIRED_HEIGHT (f);
  surface = cairo_surface_create_similar (surface_orig,
					  CAIRO_CONTENT_COLOR_ALPHA,
					  width, height);

  cr = cairo_create (surface);
  cairo_set_source_surface (cr, surface_orig, 0, 0);
  cairo_rectangle (cr, 0, 0, width, height);
  cairo_clip (cr);
  cairo_paint (cr);

  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_set_operator (cr, CAIRO_OPERATOR_DIFFERENCE);

  /* Get the height not including a menu bar widget.  */
  height = FRAME_PIXEL_HEIGHT (f);
  /* Height of each line to flash.  */
  flash_height = FRAME_LINE_HEIGHT (f);
  /* These will be the left and right margins of the rectangles.  */
  flash_left = FRAME_INTERNAL_BORDER_WIDTH (f);
  flash_right
    = (FRAME_PIXEL_WIDTH (f) - FRAME_INTERNAL_BORDER_WIDTH (f));
  width = flash_right - flash_left;

  /* If window is tall, flash top and bottom line.  */
  if (height > 3 * FRAME_LINE_HEIGHT (f))
    {
      cairo_rectangle (cr, flash_left,
		       (FRAME_INTERNAL_BORDER_WIDTH (f)
			+ FRAME_TOP_MARGIN_HEIGHT (f)),
		       width, flash_height);
      cairo_fill (cr);

      cairo_rectangle (cr, flash_left,
		       (height - flash_height
			- FRAME_INTERNAL_BORDER_WIDTH (f)
			- FRAME_BOTTOM_MARGIN_HEIGHT (f)),
		       width, flash_height);
      cairo_fill (cr);
    }
  else
    {
      /* If it is short, flash it all.  */
      cairo_rectangle (cr, flash_left,
		       FRAME_INTERNAL_BORDER_WIDTH (f), width,
		       height - 2 * FRAME_INTERNAL_BORDER_WIDTH (f));
      cairo_fill (cr);
    }

  FRAME_X_OUTPUT (f)->cr_surface_visible_bell = surface;

  delay = make_timespec (0, 50 * 1000 * 1000);

  if (FRAME_X_OUTPUT (f)->atimer_visible_bell != NULL)
    {
      cancel_atimer (FRAME_X_OUTPUT (f)->atimer_visible_bell);
      FRAME_X_OUTPUT (f)->atimer_visible_bell = NULL;
    }

  FRAME_X_OUTPUT (f)->atimer_visible_bell
    = start_atimer (ATIMER_RELATIVE, delay, recover_from_visible_bell,
		    f);

  cairo_destroy (cr);
  unblock_input ();
#endif
}

/* Make audible bell.  */

static void
pgtk_ring_bell (struct frame *f)
{
  if (visible_bell)
    {
      pgtk_flash (f);
    }
  else
    {
      block_input ();
      gtk_widget_error_bell (FRAME_GTK_WIDGET (f));
      unblock_input ();
    }
}

/* Read events coming from the X server.
   Return as soon as there are no more events to be read.

   Return the number of characters stored into the buffer,
   thus pretending to be `read' (except the characters we store
   in the keyboard buffer can be multibyte, so are not necessarily
   C chars).  */

static int
pgtk_read_socket (struct terminal *terminal,
		  struct input_event *hold_quit)
{
  GMainContext *context;
  bool context_acquired = false;
  int count;

  count = evq_flush (hold_quit);
  if (count > 0)
    return count;

  context = g_main_context_default ();
  context_acquired = g_main_context_acquire (context);

  block_input ();

  if (context_acquired)
    {
      while (g_main_context_pending (context))
	{
	  g_main_context_dispatch (context);
	}
    }

  unblock_input ();

  if (context_acquired)
    g_main_context_release (context);

  count = evq_flush (hold_quit);
  if (count > 0)
    {
      return count;
    }

  return 0;
}

/* Lisp window being scrolled.  Set when starting to interact with
   a toolkit scroll bar, reset to nil when ending the interaction.  */

static Lisp_Object window_being_scrolled;

static void
pgtk_send_scroll_bar_event (Lisp_Object window,
			    enum scroll_bar_part part, int portion,
			    int whole, bool horizontal)
{
  union buffered_input_event inev;

  EVENT_INIT (inev.ie);

  inev.ie.kind = (horizontal ? HORIZONTAL_SCROLL_BAR_CLICK_EVENT
			     : SCROLL_BAR_CLICK_EVENT);
  inev.ie.frame_or_window = window;
  inev.ie.arg = Qnil;
  inev.ie.timestamp = 0;
  inev.ie.code = 0;
  inev.ie.part = part;
  inev.ie.x = make_fixnum (portion);
  inev.ie.y = make_fixnum (whole);
  inev.ie.modifiers = 0;

  evq_enqueue (&inev);
}

/* Scroll bar callback for GTK scroll bars.  WIDGET is the scroll
   bar widget.  DATA is a pointer to the scroll_bar structure. */

static gboolean
xg_scroll_callback (GtkRange *range, GtkScrollType scroll,
		    gdouble value, gpointer user_data)
{
  int whole = 0, portion = 0;
  struct scroll_bar *bar = user_data;
  enum scroll_bar_part part = scroll_bar_nowhere;
  GtkAdjustment *adj
    = GTK_ADJUSTMENT (gtk_range_get_adjustment (range));

  if (xg_ignore_gtk_scrollbar)
    return false;

  switch (scroll)
    {
    case GTK_SCROLL_JUMP:
      if (bar->horizontal)
	{
	  part = scroll_bar_horizontal_handle;
	  whole = (int) (gtk_adjustment_get_upper (adj)
			 - gtk_adjustment_get_page_size (adj));
	  portion = min ((int) value, whole);
	  bar->dragging = portion;
	}
      else
	{
	  part = scroll_bar_handle;
	  whole = gtk_adjustment_get_upper (adj)
		  - gtk_adjustment_get_page_size (adj);
	  portion = min ((int) value, whole);
	  bar->dragging = portion;
	}
      break;
    case GTK_SCROLL_STEP_BACKWARD:
      part = (bar->horizontal ? scroll_bar_left_arrow
			      : scroll_bar_up_arrow);
      bar->dragging = -1;
      break;
    case GTK_SCROLL_STEP_FORWARD:
      part = (bar->horizontal ? scroll_bar_right_arrow
			      : scroll_bar_down_arrow);
      bar->dragging = -1;
      break;
    case GTK_SCROLL_PAGE_BACKWARD:
      part = (bar->horizontal ? scroll_bar_before_handle
			      : scroll_bar_above_handle);
      bar->dragging = -1;
      break;
    case GTK_SCROLL_PAGE_FORWARD:
      part = (bar->horizontal ? scroll_bar_after_handle
			      : scroll_bar_below_handle);
      bar->dragging = -1;
      break;
    default:
      break;
    }

  if (part != scroll_bar_nowhere)
    {
      window_being_scrolled = bar->window;
      pgtk_send_scroll_bar_event (bar->window, part, portion, whole,
				  bar->horizontal);
    }

  return false;
}

/* Callback for button release. Sets dragging to -1 when dragging is
 * done.  */

static gboolean
xg_end_scroll_callback (GtkWidget *widget, GdkEventButton *event,
			gpointer user_data)
{
  struct scroll_bar *bar = user_data;
  bar->dragging = -1;
  if (WINDOWP (window_being_scrolled))
    {
      pgtk_send_scroll_bar_event (window_being_scrolled,
				  scroll_bar_end_scroll, 0, 0,
				  bar->horizontal);
      window_being_scrolled = Qnil;
    }

  return false;
}

#define SCROLL_BAR_NAME "verticalScrollBar"
#define SCROLL_BAR_HORIZONTAL_NAME "horizontalScrollBar"

/* Create the widget for scroll bar BAR on frame F.  Record the widget
   and X window of the scroll bar in BAR.  */

static void
pgtk_create_toolkit_scroll_bar (struct frame *f,
				struct scroll_bar *bar)
{
  const char *scroll_bar_name = SCROLL_BAR_NAME;

  block_input ();
  xg_create_scroll_bar (f, bar, G_CALLBACK (xg_scroll_callback),
			G_CALLBACK (xg_end_scroll_callback),
			scroll_bar_name);
  unblock_input ();
}

static void
pgtk_create_horizontal_toolkit_scroll_bar (struct frame *f,
					   struct scroll_bar *bar)
{
  const char *scroll_bar_name = SCROLL_BAR_HORIZONTAL_NAME;

  block_input ();
  xg_create_horizontal_scroll_bar (f, bar,
				   G_CALLBACK (xg_scroll_callback),
				   G_CALLBACK (
				     xg_end_scroll_callback),
				   scroll_bar_name);
  unblock_input ();
}

/* Set the thumb size and position of scroll bar BAR.  We are
   currently displaying PORTION out of a whole WHOLE, and our position
   POSITION.  */

static void
pgtk_set_toolkit_scroll_bar_thumb (struct scroll_bar *bar,
				   int portion, int position,
				   int whole)
{
  xg_set_toolkit_scroll_bar_thumb (bar, portion, position, whole);
}

static void
pgtk_set_toolkit_horizontal_scroll_bar_thumb (struct scroll_bar *bar,
					      int portion,
					      int position, int whole)
{
  xg_set_toolkit_horizontal_scroll_bar_thumb (bar, portion, position,
					      whole);
}

/* Create a scroll bar and return the scroll bar vector for it.  W is
   the Emacs window on which to create the scroll bar. TOP, LEFT,
   WIDTH and HEIGHT are the pixel coordinates and dimensions of the
   scroll bar. */

static struct scroll_bar *
pgtk_scroll_bar_create (struct window *w, int top, int left,
			int width, int height, bool horizontal)
{
  struct frame *f = XFRAME (w->frame);
  struct scroll_bar *bar
    = ALLOCATE_PSEUDOVECTOR (struct scroll_bar, prev, PVEC_OTHER);
  Lisp_Object barobj;

  block_input ();

  if (horizontal)
    pgtk_create_horizontal_toolkit_scroll_bar (f, bar);
  else
    pgtk_create_toolkit_scroll_bar (f, bar);

  XSETWINDOW (bar->window, w);
  bar->top = top;
  bar->left = left;
  bar->width = width;
  bar->height = height;
  bar->start = 0;
  bar->end = 0;
  bar->dragging = -1;
  bar->horizontal = horizontal;

  /* Add bar to its frame's list of scroll bars.  */
  bar->next = FRAME_SCROLL_BARS (f);
  bar->prev = Qnil;
  XSETVECTOR (barobj, bar);
  fset_scroll_bars (f, barobj);
  if (!NILP (bar->next))
    XSETVECTOR (XSCROLL_BAR (bar->next)->prev, bar);

  /* Map the window/widget.  */
  {
    if (horizontal)
      xg_update_horizontal_scrollbar_pos (f, bar->x_window, top, left,
					  width, max (height, 1));
    else
      xg_update_scrollbar_pos (f, bar->x_window, top, left, width,
			       max (height, 1));
  }

  unblock_input ();
  return bar;
}

/* Destroy scroll bar BAR, and set its Emacs window's scroll bar to
   nil.  */

static void
pgtk_scroll_bar_remove (struct scroll_bar *bar)
{
  struct frame *f = XFRAME (WINDOW_FRAME (XWINDOW (bar->window)));
  block_input ();

  xg_remove_scroll_bar (f, bar->x_window);

  /* Dissociate this scroll bar from its window.  */
  if (bar->horizontal)
    wset_horizontal_scroll_bar (XWINDOW (bar->window), Qnil);
  else
    wset_vertical_scroll_bar (XWINDOW (bar->window), Qnil);

  unblock_input ();
}

/* Set the handle of the vertical scroll bar for WINDOW to indicate
   that we are displaying PORTION characters out of a total of WHOLE
   characters, starting at POSITION.  If WINDOW has no scroll bar,
   create one.  */

static void
pgtk_set_vertical_scroll_bar (struct window *w, int portion,
			      int whole, int position)
{
  struct frame *f = XFRAME (w->frame);
  Lisp_Object barobj;
  struct scroll_bar *bar;
  int top, height, left, width;
  int window_y, window_height;

  /* Get window dimensions.  */
  window_box (w, ANY_AREA, 0, &window_y, 0, &window_height);
  top = window_y;
  height = window_height;
  left = WINDOW_SCROLL_BAR_AREA_X (w);
  width = WINDOW_SCROLL_BAR_AREA_WIDTH (w);

  /* Does the scroll bar exist yet?  */
  if (NILP (w->vertical_scroll_bar))
    {
      if (width > 0 && height > 0)
	{
	  block_input ();
	  pgtk_clear_area (f, left, top, width, height);
	  unblock_input ();
	}

      bar = pgtk_scroll_bar_create (w, top, left, width,
				    max (height, 1), false);
    }
  else
    {
      /* It may just need to be moved and resized.  */
      unsigned int mask = 0;

      bar = XSCROLL_BAR (w->vertical_scroll_bar);

      block_input ();

      if (left != bar->left)
	mask |= 1;
      if (top != bar->top)
	mask |= 1;
      if (width != bar->width)
	mask |= 1;
      if (height != bar->height)
	mask |= 1;

      /* Move/size the scroll bar widget.  */
      if (mask)
	{
	  /* Since toolkit scroll bars are smaller than the space
	     reserved for them on the frame, we have to clear "under"
	     them.  */
	  if (width > 0 && height > 0)
	    pgtk_clear_area (f, left, top, width, height);
	  xg_update_scrollbar_pos (f, bar->x_window, top, left, width,
				   max (height, 1));
	}

      /* Remember new settings.  */
      bar->left = left;
      bar->top = top;
      bar->width = width;
      bar->height = height;

      unblock_input ();
    }

  pgtk_set_toolkit_scroll_bar_thumb (bar, portion, position, whole);

  XSETVECTOR (barobj, bar);
  wset_vertical_scroll_bar (w, barobj);
}

static void
pgtk_set_horizontal_scroll_bar (struct window *w, int portion,
				int whole, int position)
{
  struct frame *f = XFRAME (w->frame);
  Lisp_Object barobj;
  struct scroll_bar *bar;
  int top, height, left, width;
  int window_x, window_width;
  int pixel_width = WINDOW_PIXEL_WIDTH (w);

  /* Get window dimensions.  */
  window_box (w, ANY_AREA, &window_x, 0, &window_width, 0);
  left = window_x;
  width = window_width;
  top = WINDOW_SCROLL_BAR_AREA_Y (w);
  height = WINDOW_SCROLL_BAR_AREA_HEIGHT (w);

  /* Does the scroll bar exist yet?  */
  if (NILP (w->horizontal_scroll_bar))
    {
      if (width > 0 && height > 0)
	{
	  block_input ();

	  /* Clear also part between window_width and
	     WINDOW_PIXEL_WIDTH.  */
	  pgtk_clear_area (f, left, top, pixel_width, height);
	  unblock_input ();
	}

      bar
	= pgtk_scroll_bar_create (w, top, left, width, height, true);
    }
  else
    {
      /* It may just need to be moved and resized.  */
      unsigned int mask = 0;

      bar = XSCROLL_BAR (w->horizontal_scroll_bar);

      block_input ();

      if (left != bar->left)
	mask |= 1;
      if (top != bar->top)
	mask |= 1;
      if (width != bar->width)
	mask |= 1;
      if (height != bar->height)
	mask |= 1;

      /* Move/size the scroll bar widget.  */
      if (mask)
	{
	  /* Since toolkit scroll bars are smaller than the space
	     reserved for them on the frame, we have to clear "under"
	     them.  */
	  if (width > 0 && height > 0)
	    pgtk_clear_area (f, WINDOW_LEFT_EDGE_X (w), top,
			     pixel_width
			       - WINDOW_RIGHT_DIVIDER_WIDTH (w),
			     height);
	  xg_update_horizontal_scrollbar_pos (f, bar->x_window, top,
					      left, width, height);
	}

      /* Remember new settings.  */
      bar->left = left;
      bar->top = top;
      bar->width = width;
      bar->height = height;

      unblock_input ();
    }

  pgtk_set_toolkit_horizontal_scroll_bar_thumb (bar, portion,
						position, whole);

  XSETVECTOR (barobj, bar);
  wset_horizontal_scroll_bar (w, barobj);
}

/* The following three hooks are used when we're doing a thorough
   redisplay of the frame.  We don't explicitly know which scroll bars
   are going to be deleted, because keeping track of when windows go
   away is a real pain - "Can you say set-window-configuration, boys
   and girls?"  Instead, we just assert at the beginning of redisplay
   that *all* scroll bars are to be removed, and then save a scroll
   bar from the fiery pit when we actually redisplay its window.  */

/* Arrange for all scroll bars on FRAME to be removed at the next call
   to `*judge_scroll_bars_hook'.  A scroll bar may be spared if
   `*redeem_scroll_bar_hook' is applied to its window before the
   judgment.  */

static void
pgtk_condemn_scroll_bars (struct frame *frame)
{
  if (!NILP (FRAME_SCROLL_BARS (frame)))
    {
      if (!NILP (FRAME_CONDEMNED_SCROLL_BARS (frame)))
	{
	  /* Prepend scrollbars to already condemned ones.  */
	  Lisp_Object last = FRAME_SCROLL_BARS (frame);

	  while (!NILP (XSCROLL_BAR (last)->next))
	    last = XSCROLL_BAR (last)->next;

	  XSCROLL_BAR (last)->next
	    = FRAME_CONDEMNED_SCROLL_BARS (frame);
	  XSCROLL_BAR (FRAME_CONDEMNED_SCROLL_BARS (frame))->prev
	    = last;
	}

      fset_condemned_scroll_bars (frame, FRAME_SCROLL_BARS (frame));
      fset_scroll_bars (frame, Qnil);
    }
}

/* Un-mark WINDOW's scroll bar for deletion in this judgment cycle.
   Note that WINDOW isn't necessarily condemned at all.  */

static void
pgtk_redeem_scroll_bar (struct window *w)
{
  struct scroll_bar *bar;
  Lisp_Object barobj;
  struct frame *f;

  /* We can't redeem this window's scroll bar if it doesn't have one.
   */
  if (NILP (w->vertical_scroll_bar)
      && NILP (w->horizontal_scroll_bar))
    emacs_abort ();

  if (!NILP (w->vertical_scroll_bar)
      && WINDOW_HAS_VERTICAL_SCROLL_BAR (w))
    {
      bar = XSCROLL_BAR (w->vertical_scroll_bar);
      /* Unlink it from the condemned list.  */
      f = XFRAME (WINDOW_FRAME (w));
      if (NILP (bar->prev))
	{
	  /* If the prev pointer is nil, it must be the first in one
	     of the lists.  */
	  if (EQ (FRAME_SCROLL_BARS (f), w->vertical_scroll_bar))
	    /* It's not condemned.  Everything's fine.  */
	    goto horizontal;
	  else if (EQ (FRAME_CONDEMNED_SCROLL_BARS (f),
		       w->vertical_scroll_bar))
	    fset_condemned_scroll_bars (f, bar->next);
	  else
	    /* If its prev pointer is nil, it must be at the front of
	       one or the other!  */
	    emacs_abort ();
	}
      else
	XSCROLL_BAR (bar->prev)->next = bar->next;

      if (!NILP (bar->next))
	XSCROLL_BAR (bar->next)->prev = bar->prev;

      bar->next = FRAME_SCROLL_BARS (f);
      bar->prev = Qnil;
      XSETVECTOR (barobj, bar);
      fset_scroll_bars (f, barobj);
      if (!NILP (bar->next))
	XSETVECTOR (XSCROLL_BAR (bar->next)->prev, bar);
    }

horizontal:
  if (!NILP (w->horizontal_scroll_bar)
      && WINDOW_HAS_HORIZONTAL_SCROLL_BAR (w))
    {
      bar = XSCROLL_BAR (w->horizontal_scroll_bar);
      /* Unlink it from the condemned list.  */
      f = XFRAME (WINDOW_FRAME (w));
      if (NILP (bar->prev))
	{
	  /* If the prev pointer is nil, it must be the first in one
	     of the lists.  */
	  if (EQ (FRAME_SCROLL_BARS (f), w->horizontal_scroll_bar))
	    /* It's not condemned.  Everything's fine.  */
	    return;
	  else if (EQ (FRAME_CONDEMNED_SCROLL_BARS (f),
		       w->horizontal_scroll_bar))
	    fset_condemned_scroll_bars (f, bar->next);
	  else
	    /* If its prev pointer is nil, it must be at the front of
	       one or the other!  */
	    emacs_abort ();
	}
      else
	XSCROLL_BAR (bar->prev)->next = bar->next;

      if (!NILP (bar->next))
	XSCROLL_BAR (bar->next)->prev = bar->prev;

      bar->next = FRAME_SCROLL_BARS (f);
      bar->prev = Qnil;
      XSETVECTOR (barobj, bar);
      fset_scroll_bars (f, barobj);
      if (!NILP (bar->next))
	XSETVECTOR (XSCROLL_BAR (bar->next)->prev, bar);
    }
}

/* Remove all scroll bars on FRAME that haven't been saved since the
   last call to `*condemn_scroll_bars_hook'.  */

static void
pgtk_judge_scroll_bars (struct frame *f)
{
  Lisp_Object bar, next;

  bar = FRAME_CONDEMNED_SCROLL_BARS (f);

  /* Clear out the condemned list now so we won't try to process any
     more events on the hapless scroll bars.  */
  fset_condemned_scroll_bars (f, Qnil);

  for (; !NILP (bar); bar = next)
    {
      struct scroll_bar *b = XSCROLL_BAR (bar);

      pgtk_scroll_bar_remove (b);

      next = b->next;
      b->next = b->prev = Qnil;
    }

  /* Now there should be no references to the condemned scroll bars,
     and they should get garbage-collected.  */
}

static void
set_fullscreen_state (struct frame *f)
{
  if (!FRAME_GTK_OUTER_WIDGET (f))
    return;

  GtkWindow *widget = GTK_WINDOW (FRAME_GTK_OUTER_WIDGET (f));
  switch (f->want_fullscreen)
    {
    case FULLSCREEN_NONE:
      gtk_window_unfullscreen (widget);
      gtk_window_unmaximize (widget);
      store_frame_param (f, Qfullscreen, Qnil);
      break;

    case FULLSCREEN_BOTH:
      gtk_window_unmaximize (widget);
      gtk_window_fullscreen (widget);
      store_frame_param (f, Qfullscreen, Qfullboth);
      break;

    case FULLSCREEN_MAXIMIZED:
      gtk_window_unfullscreen (widget);
      gtk_window_maximize (widget);
      store_frame_param (f, Qfullscreen, Qmaximized);
      break;

    case FULLSCREEN_WIDTH:
    case FULLSCREEN_HEIGHT:
      /* Not supported by gtk. Ignore them. */
      break;
    }

  f->want_fullscreen = FULLSCREEN_NONE;
}

static void
pgtk_fullscreen_hook (struct frame *f)
{
  if (FRAME_VISIBLE_P (f))
    {
      block_input ();
      set_fullscreen_state (f);
      unblock_input ();
    }
}

/* This function is called when the last frame on a display is
 * deleted. */
void
pgtk_delete_terminal (struct terminal *terminal)
{
  struct pgtk_display_info *dpyinfo = terminal->display_info.pgtk;

  /* Protect against recursive calls.  delete_frame in
     delete_terminal calls us back when it deletes our last frame.  */
  if (!terminal->name)
    return;

  block_input ();

  pgtk_im_finish (dpyinfo);

  /* Normally, the display is available...  */
  if (dpyinfo->gdpy)
    {
      image_destroy_all_bitmaps (dpyinfo);

      g_clear_object (&dpyinfo->xg_cursor);
      g_clear_object (&dpyinfo->vertical_scroll_bar_cursor);
      g_clear_object (&dpyinfo->horizontal_scroll_bar_cursor);
      g_clear_object (&dpyinfo->invisible_cursor);
      if (dpyinfo->last_click_event != NULL)
	{
	  gdk_event_free (dpyinfo->last_click_event);
	  dpyinfo->last_click_event = NULL;
	}

      /* Disconnect these handlers before the display closes so
	 useless removal signals don't fire.  */
      g_signal_handlers_disconnect_by_func (G_OBJECT (dpyinfo->gdpy),
					    G_CALLBACK (
					      pgtk_seat_added_cb),
					    dpyinfo);
      g_signal_handlers_disconnect_by_func (G_OBJECT (dpyinfo->gdpy),
					    G_CALLBACK (
					      pgtk_seat_removed_cb),
					    dpyinfo);
      xg_display_close (dpyinfo->gdpy);

      dpyinfo->gdpy = NULL;
    }

  if (dpyinfo->connection >= 0)
    emacs_close (dpyinfo->connection);

  dpyinfo->connection = -1;

  delete_keyboard_wait_descriptor (0);

  pgtk_delete_display (dpyinfo);
  unblock_input ();
}

/* Store F's background color into *BGCOLOR.  */
static void
pgtk_query_frame_background_color (struct frame *f,
				   Emacs_Color *bgcolor)
{
  bgcolor->pixel = FRAME_BACKGROUND_PIXEL (f);
  pgtk_query_color (f, bgcolor);
}

static void
pgtk_free_pixmap (struct frame *f, Emacs_Pixmap pixmap)
{
  if (pixmap)
    {
      xfree (pixmap->data);
      xfree (pixmap);
    }
}

void
pgtk_focus_frame (struct frame *f, bool noactivate)
{
  struct pgtk_display_info *dpyinfo;
  GtkWidget *widget;
  GtkWindow *window;

  dpyinfo = FRAME_DISPLAY_INFO (f);

  if (FRAME_GTK_OUTER_WIDGET (f) && !noactivate)
    {
      /* The user says it is okay to activate the frame.  Call
	 gtk_window_present_with_time.  If the timestamp specified
	 (actually a display serial on Wayland) is new enough, then
	 any Wayland compositor supporting gtk_surface1_present will
	 cause the frame to be activated.  */

      window = GTK_WINDOW (FRAME_GTK_OUTER_WIDGET (f));
      gtk_window_present_with_time (window, dpyinfo->last_user_time);
      return;
    }

  widget = FRAME_WIDGET (f);

  if (widget)
    gtk_widget_grab_focus (widget);
}

static void
set_opacity_recursively (GtkWidget *w, gpointer data)
{
  gtk_widget_set_opacity (w, *(double *) data);

  if (GTK_IS_CONTAINER (w))
    gtk_container_foreach (GTK_CONTAINER (w), set_opacity_recursively,
			   data);
}

static void
pgtk_set_frame_alpha (struct frame *f)
{
  struct pgtk_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
  double alpha = 1.0;
  double alpha_min = 1.0;

  if (dpyinfo->highlight_frame == f)
    alpha = f->alpha[0];
  else
    alpha = f->alpha[1];

  if (alpha < 0.0)
    return;

  if (FLOATP (Vframe_alpha_lower_limit))
    alpha_min = XFLOAT_DATA (Vframe_alpha_lower_limit);
  else if (FIXNUMP (Vframe_alpha_lower_limit))
    alpha_min = (XFIXNUM (Vframe_alpha_lower_limit)) / 100.0;

  if (alpha > 1.0)
    alpha = 1.0;
  else if (alpha < alpha_min && alpha_min <= 1.0)
    alpha = alpha_min;

  set_opacity_recursively (FRAME_WIDGET (f), &alpha);
  /* without this, blending mode is strange on wayland. */
  gtk_widget_queue_resize_no_redraw (FRAME_WIDGET (f));
}

static void
frame_highlight (struct frame *f)
{
  block_input ();
  GtkWidget *w = FRAME_WIDGET (f);

  char *css
    = g_strdup_printf ("decoration { border: solid %dpx #%06x; }",
		       f->border_width,
		       ((unsigned int) FRAME_X_OUTPUT (f)
			  ->border_pixel
			& 0x00ffffff));

  GtkStyleContext *ctxt = gtk_widget_get_style_context (w);
  GtkCssProvider *css_provider = gtk_css_provider_new ();
  gtk_css_provider_load_from_data (css_provider, css, -1, NULL);
  gtk_style_context_add_provider (ctxt,
				  GTK_STYLE_PROVIDER (css_provider),
				  GTK_STYLE_PROVIDER_PRIORITY_USER);
  g_free (css);

  GtkCssProvider *old = FRAME_X_OUTPUT (f)->border_color_css_provider;
  FRAME_X_OUTPUT (f)->border_color_css_provider = css_provider;
  if (old != NULL)
    {
      gtk_style_context_remove_provider (ctxt,
					 GTK_STYLE_PROVIDER (old));
      g_object_unref (old);
    }

  unblock_input ();
  gui_update_cursor (f, true);
  pgtk_set_frame_alpha (f);
}

static void
frame_unhighlight (struct frame *f)
{
  GtkWidget *w;
  char *css;
  GtkStyleContext *ctxt;
  GtkCssProvider *css_provider, *old;

  block_input ();

  w = FRAME_WIDGET (f);

  css
    = g_strdup_printf ("decoration { border: dotted %dpx #ffffff; }",
		       f->border_width);

  ctxt = gtk_widget_get_style_context (w);
  css_provider = gtk_css_provider_new ();
  gtk_css_provider_load_from_data (css_provider, css, -1, NULL);
  gtk_style_context_add_provider (ctxt,
				  GTK_STYLE_PROVIDER (css_provider),
				  GTK_STYLE_PROVIDER_PRIORITY_USER);
  g_free (css);

  old = FRAME_X_OUTPUT (f)->border_color_css_provider;
  FRAME_X_OUTPUT (f)->border_color_css_provider = css_provider;
  if (old != NULL)
    {
      gtk_style_context_remove_provider (ctxt,
					 GTK_STYLE_PROVIDER (old));
      g_object_unref (old);
    }

  unblock_input ();
  gui_update_cursor (f, true);
  pgtk_set_frame_alpha (f);
}

void
pgtk_frame_rehighlight (struct pgtk_display_info *dpyinfo)
{
  struct frame *old_highlight = dpyinfo->highlight_frame;

  if (dpyinfo->x_focus_frame)
    {
      dpyinfo->highlight_frame
	= ((FRAMEP (FRAME_FOCUS_FRAME (dpyinfo->x_focus_frame)))
	     ? XFRAME (FRAME_FOCUS_FRAME (dpyinfo->x_focus_frame))
	     : dpyinfo->x_focus_frame);
      if (!FRAME_LIVE_P (dpyinfo->highlight_frame))
	{
	  fset_focus_frame (dpyinfo->x_focus_frame, Qnil);
	  dpyinfo->highlight_frame = dpyinfo->x_focus_frame;
	}
    }
  else
    dpyinfo->highlight_frame = 0;

  if (old_highlight)
    frame_unhighlight (old_highlight);
  if (dpyinfo->highlight_frame)
    frame_highlight (dpyinfo->highlight_frame);
}

/* The focus has changed, or we have redirected a frame's focus to
   another frame (this happens when a frame uses a surrogate
   mini-buffer frame).  Shift the highlight as appropriate.

   The FRAME argument doesn't necessarily have anything to do with
   which frame is being highlighted or un-highlighted; we only use it
   to find the appropriate X display info.  */

static void
pgtk_frame_rehighlight_hook (struct frame *frame)
{
  pgtk_frame_rehighlight (FRAME_DISPLAY_INFO (frame));
}

/* Set whether or not the mouse pointer should be visible on frame
   F.  */
static void
pgtk_toggle_invisible_pointer (struct frame *f, bool invisible)
{
  Emacs_Cursor cursor;
  if (invisible)
    cursor = FRAME_DISPLAY_INFO (f)->invisible_cursor;
  else
    cursor = f->output_data.pgtk->current_cursor;
  gdk_window_set_cursor (gtk_widget_get_window (FRAME_GTK_WIDGET (f)),
			 cursor);
  f->pointer_invisible = invisible;

  /* This is needed to make the pointer visible upon receiving a
     motion notify event.  */
  gdk_display_flush (FRAME_X_DISPLAY (f));
}

/* The focus has changed.  Update the frames as necessary to reflect
   the new situation.  Note that we can't change the selected frame
   here, because the Lisp code we are interrupting might become
   confused. Each event gets marked with the frame in which it
   occurred, so the Lisp code can tell when the switch took place by
   examining the events.  */

static void
pgtk_new_focus_frame (struct pgtk_display_info *dpyinfo,
		      struct frame *frame)
{
  struct frame *old_focus = dpyinfo->x_focus_frame;
  /* doesn't work on wayland */

  if (frame != dpyinfo->x_focus_frame)
    {
      /* Set this before calling other routines, so that they see
	 the correct value of x_focus_frame.  */
      dpyinfo->x_focus_frame = frame;

      if (old_focus && old_focus->auto_lower)
	if (FRAME_GTK_OUTER_WIDGET (old_focus))
	  gdk_window_lower (gtk_widget_get_window (
	    FRAME_GTK_OUTER_WIDGET (old_focus)));

      if (dpyinfo->x_focus_frame
	  && dpyinfo->x_focus_frame->auto_raise)
	if (FRAME_GTK_OUTER_WIDGET (dpyinfo->x_focus_frame))
	  gdk_window_raise (gtk_widget_get_window (
	    FRAME_GTK_OUTER_WIDGET (dpyinfo->x_focus_frame)));
    }

  pgtk_frame_rehighlight (dpyinfo);
}

static void
pgtk_buffer_flipping_unblocked_hook (struct frame *f)
{
  block_input ();
#ifdef USE_SKIA
  /* For Skia GL, queue a draw on the drawing area.  */
  if (FRAME_SKIA_SURFACE (f))
    {
      /* Flush Skia first.  */
      emacs_skia_surface_flush (FRAME_SKIA_SURFACE (f));
      if (FRAME_SKIA_GL_CONTEXT (f))
	emacs_skia_gl_context_flush (FRAME_SKIA_GL_CONTEXT (f));
      if (FRAME_GL_DRAWING_AREA (f))
	gtk_widget_queue_draw (FRAME_GL_DRAWING_AREA (f));
    }
  else
#endif
    {
#ifdef USE_CAIRO
      flip_cr_context (f);
#endif
      gtk_widget_queue_draw (FRAME_GTK_WIDGET (f));
    }
  unblock_input ();
}

static struct terminal *
pgtk_create_terminal (struct pgtk_display_info *dpyinfo)
{
  struct terminal *terminal;

  terminal = create_terminal (output_pgtk, &pgtk_redisplay_interface);

  terminal->display_info.pgtk = dpyinfo;
  dpyinfo->terminal = terminal;

  terminal->clear_frame_hook = pgtk_clear_frame;
  terminal->ring_bell_hook = pgtk_ring_bell;
  terminal->toggle_invisible_pointer_hook
    = pgtk_toggle_invisible_pointer;
  terminal->update_begin_hook = pgtk_update_begin;
  terminal->update_end_hook = pgtk_update_end;
  terminal->read_socket_hook = pgtk_read_socket;
  terminal->frame_up_to_date_hook = pgtk_frame_up_to_date;
  terminal->mouse_position_hook = pgtk_mouse_position;
  terminal->frame_rehighlight_hook = pgtk_frame_rehighlight_hook;
  terminal->buffer_flipping_unblocked_hook
    = pgtk_buffer_flipping_unblocked_hook;
  terminal->frame_raise_lower_hook = pgtk_frame_raise_lower;
  terminal->frame_visible_invisible_hook
    = pgtk_make_frame_visible_invisible;
  terminal->fullscreen_hook = pgtk_fullscreen_hook;
  terminal->menu_show_hook = pgtk_menu_show;
  terminal->activate_menubar_hook = pgtk_activate_menubar;
  terminal->popup_dialog_hook = pgtk_popup_dialog;
  terminal->change_tab_bar_height_hook = pgtk_change_tab_bar_height;
  terminal->set_vertical_scroll_bar_hook
    = pgtk_set_vertical_scroll_bar;
  terminal->set_horizontal_scroll_bar_hook
    = pgtk_set_horizontal_scroll_bar;
  terminal->condemn_scroll_bars_hook = pgtk_condemn_scroll_bars;
  terminal->redeem_scroll_bar_hook = pgtk_redeem_scroll_bar;
  terminal->judge_scroll_bars_hook = pgtk_judge_scroll_bars;
  terminal->get_string_resource_hook = pgtk_get_string_resource;
  terminal->delete_frame_hook = pgtk_destroy_window;
  terminal->delete_terminal_hook = pgtk_delete_terminal;
  terminal->query_frame_background_color
    = pgtk_query_frame_background_color;
  terminal->defined_color_hook = pgtk_defined_color;
  terminal->set_new_font_hook = pgtk_new_font;
  terminal->set_bitmap_icon_hook = pgtk_bitmap_icon;
  terminal->implicit_set_name_hook = pgtk_implicitly_set_name;
  terminal->iconify_frame_hook = pgtk_iconify_frame;
  terminal->set_scroll_bar_default_width_hook
    = pgtk_set_scroll_bar_default_width;
  terminal->set_scroll_bar_default_height_hook
    = pgtk_set_scroll_bar_default_height;
  terminal->set_window_size_hook = pgtk_set_window_size;
  terminal->query_colors = pgtk_query_colors;
  terminal->get_focus_frame = pgtk_get_focus_frame;
  terminal->focus_frame_hook = pgtk_focus_frame;
  terminal->set_frame_offset_hook = pgtk_set_offset;
  terminal->free_pixmap = pgtk_free_pixmap;

  /* Other hooks are NULL by default.  */

  return terminal;
}

struct pgtk_window_is_of_frame_recursive_t
{
  GdkWindow *window;
  bool result;
  GtkWidget
    *emacs_gtk_fixed; /* stop on emacsgtkfixed other than this. */
};

static void
pgtk_window_is_of_frame_recursive (GtkWidget *widget, gpointer data)
{
  struct pgtk_window_is_of_frame_recursive_t *datap = data;

  if (datap->result)
    return;

  if (EMACS_IS_FIXED (widget) && widget != datap->emacs_gtk_fixed)
    return;

  if (gtk_widget_get_window (widget) == datap->window)
    {
      datap->result = true;
      return;
    }

  if (GTK_IS_CONTAINER (widget))
    gtk_container_foreach (GTK_CONTAINER (widget),
			   pgtk_window_is_of_frame_recursive, datap);
}

static bool
pgtk_window_is_of_frame (struct frame *f, GdkWindow *window)
{
  struct pgtk_window_is_of_frame_recursive_t data;
  data.window = window;
  data.result = false;
  data.emacs_gtk_fixed = FRAME_GTK_WIDGET (f);
  pgtk_window_is_of_frame_recursive (FRAME_WIDGET (f), &data);
  return data.result;
}

/* Like x_window_to_frame but also compares the window with the
   widget's windows.  */
static struct frame *
pgtk_any_window_to_frame (GdkWindow *window)
{
  Lisp_Object tail, frame;
  struct frame *f, *found = NULL;

  if (window == NULL)
    return NULL;

  FOR_EACH_FRAME (tail, frame)
  {
    if (found)
      break;
    f = XFRAME (frame);
    if (FRAME_PGTK_P (f))
      {
	if (pgtk_window_is_of_frame (f, window))
	  found = f;
      }
  }

  return found;
}

static gboolean
pgtk_handle_event (GtkWidget *widget, GdkEvent *event, gpointer *data)
{
  struct frame *f;
  union buffered_input_event inev;
  GtkWidget *frame_widget;
  gint x, y;

  if (event->type == GDK_TOUCHPAD_PINCH
      && (event->touchpad_pinch.phase
	  != GDK_TOUCHPAD_GESTURE_PHASE_END))
    {
      f = pgtk_any_window_to_frame (gtk_widget_get_window (widget));
      frame_widget = FRAME_GTK_WIDGET (f);

      gtk_widget_translate_coordinates (widget, frame_widget,
					lrint (
					  event->touchpad_pinch.x),
					lrint (
					  event->touchpad_pinch.y),
					&x, &y);
      if (f)
	{
	  inev.ie.kind = PINCH_EVENT;
	  XSETFRAME (inev.ie.frame_or_window, f);
	  XSETINT (inev.ie.x, x);
	  XSETINT (inev.ie.y, y);
	  inev.ie.arg
	    = list4 (make_float (event->touchpad_pinch.dx),
		     make_float (event->touchpad_pinch.dy),
		     make_float (event->touchpad_pinch.scale),
		     make_float (event->touchpad_pinch.angle_delta));
	  inev.ie.modifiers
	    = pgtk_gtk_to_emacs_modifiers (FRAME_DISPLAY_INFO (f),
					   event->touchpad_pinch
					     .state);
	  inev.ie.device
	    = pgtk_get_device_for_event (FRAME_DISPLAY_INFO (f),
					 event);
	  evq_enqueue (&inev);
	}

      return TRUE;
    }
  return FALSE;
}

static void
pgtk_fill_rectangle (struct frame *f, unsigned long color, int x,
		     int y, int width, int height,
		     bool respect_alpha_background)
{
#ifdef USE_SKIA
  pgtk_skia_fill_rectangle (f, color, x, y, width, height,
			    respect_alpha_background);
#else
  cairo_t *cr;
  cr = pgtk_begin_cr_clip (f);
  pgtk_set_cr_source_with_color (f, color, respect_alpha_background);
  cairo_rectangle (cr, x, y, width, height);
  cairo_fill (cr);
  pgtk_end_cr_clip (f);
#endif
}

void
pgtk_clear_under_internal_border (struct frame *f)
{
  if (FRAME_INTERNAL_BORDER_WIDTH (f) > 0
      && (!FRAME_GTK_OUTER_WIDGET (f)
	  || gtk_widget_get_realized (FRAME_GTK_OUTER_WIDGET (f))))
    {
      int border = FRAME_INTERNAL_BORDER_WIDTH (f);
      int width = FRAME_PIXEL_WIDTH (f);
      int height = FRAME_PIXEL_HEIGHT (f);
      int margin = FRAME_TOP_MARGIN_HEIGHT (f);
      int bottom_margin = FRAME_BOTTOM_MARGIN_HEIGHT (f);
      int face_id
	= (FRAME_PARENT_FRAME (f)
	     ? (!NILP (Vface_remapping_alist)
		  ? lookup_basic_face (NULL, f,
				       CHILD_FRAME_BORDER_FACE_ID)
		  : CHILD_FRAME_BORDER_FACE_ID)
	     : (!NILP (Vface_remapping_alist)
		  ? lookup_basic_face (NULL, f,
				       INTERNAL_BORDER_FACE_ID)
		  : INTERNAL_BORDER_FACE_ID));
      struct face *face = FACE_FROM_ID_OR_NULL (f, face_id);

      block_input ();

      if (face)
	{
	  fill_background_by_face (f, face, 0, margin, width, border);
	  fill_background_by_face (f, face, 0, 0, border, height);
	  fill_background_by_face (f, face, width - border, 0, border,
				   height);
	  fill_background_by_face (f, face, 0,
				   (height - bottom_margin - border),
				   width, border);
	}
      else
	{
	  pgtk_clear_area (f, 0, 0, border, height);
	  pgtk_clear_area (f, 0, margin, width, border);
	  pgtk_clear_area (f, width - border, 0, border, height);
	  pgtk_clear_area (f, 0, height - bottom_margin - border,
			   width, border);
	}

      unblock_input ();
    }
}

static gboolean
pgtk_handle_draw (GtkWidget *widget, cairo_t *cr, gpointer *data)
{
  struct frame *f;

  GdkWindow *win = gtk_widget_get_window (widget);

  if (win == NULL)
    return FALSE;

  f = pgtk_any_window_to_frame (win);
  if (f == NULL)
    return FALSE;

#ifdef USE_SKIA
  /* For Skia with GL rendering, the GtkDrawingArea handles display
     via its own draw callback (pgtk_gl_drawing_area_draw).  This draw
     callback on the GtkFixed widget is not used.  */
  (void) cr; /* Suppress unused parameter warning.  */
  return FALSE;
#else /* USE_CAIRO */
  cairo_surface_t *src = NULL;
  src = FRAME_X_OUTPUT (f)->cr_surface_visible_bell;
  if (src == NULL && FRAME_CR_ACTIVE_CONTEXT (f) != NULL)
    src = cairo_get_target (FRAME_CR_ACTIVE_CONTEXT (f));
  if (src != NULL)
    {
      cairo_set_source_surface (cr, src, 0, 0);
      cairo_paint (cr);
    }
#endif
  return FALSE;
}

static void
size_allocate (GtkWidget *widget, GtkAllocation *alloc,
	       gpointer user_data)
{
  /* Use user_data directly since it's the frame passed when
     connecting the signal.  Using pgtk_any_window_to_frame can return
     the wrong frame for child frames that share their parent's GDK
     window.  */
  struct frame *f = user_data;

  if (f)
    {
      xg_frame_resized (f, alloc->width, alloc->height);
#ifdef USE_SKIA
      /* Resize the drawing area to fill the frame.  GtkFixed doesn't
	 automatically re-allocate children when their size request
	 changes, so we must explicitly allocate the drawing area.  This
	 triggers a redraw.  We resize the drawing area and update
	 the desired dimensions; pgtk_begin_skia_clip handles the
	 actual FBO resize on next draw.  */
      if (FRAME_GL_DRAWING_AREA (f))
	{
	  GtkAllocation gl_alloc;
	  gl_alloc.x = 0;
	  gl_alloc.y = 0;
	  gl_alloc.width = alloc->width;
	  gl_alloc.height = alloc->height;
	  gtk_widget_size_allocate (FRAME_GL_DRAWING_AREA (f), &gl_alloc);
	}
      pgtk_skia_update_surface_desired_size (f, alloc->width,
					     alloc->height, false);
#else
      pgtk_cr_update_surface_desired_size (f, alloc->width,
					   alloc->height, false);
#endif
    }
}

static void
get_modifier_values (int *mod_ctrl, int *mod_meta, int *mod_alt,
		     int *mod_hyper, int *mod_super)
{
  Lisp_Object tem;

  *mod_ctrl = ctrl_modifier;
  *mod_meta = meta_modifier;
  *mod_alt = alt_modifier;
  *mod_hyper = hyper_modifier;
  *mod_super = super_modifier;

  tem = Fget (Vx_ctrl_keysym, Qmodifier_value);
  if (INTEGERP (tem))
    *mod_ctrl = XFIXNUM (tem) & INT_MAX;
  tem = Fget (Vx_alt_keysym, Qmodifier_value);
  if (INTEGERP (tem))
    *mod_alt = XFIXNUM (tem) & INT_MAX;
  tem = Fget (Vx_meta_keysym, Qmodifier_value);
  if (INTEGERP (tem))
    *mod_meta = XFIXNUM (tem) & INT_MAX;
  tem = Fget (Vx_hyper_keysym, Qmodifier_value);
  if (INTEGERP (tem))
    *mod_hyper = XFIXNUM (tem) & INT_MAX;
  tem = Fget (Vx_super_keysym, Qmodifier_value);
  if (INTEGERP (tem))
    *mod_super = XFIXNUM (tem) & INT_MAX;
}

int
pgtk_gtk_to_emacs_modifiers (struct pgtk_display_info *dpyinfo,
			     int state)
{
  int mod_ctrl;
  int mod_meta;
  int mod_alt;
  int mod_hyper;
  int mod_super;
  int mod;

  get_modifier_values (&mod_ctrl, &mod_meta, &mod_alt, &mod_hyper,
		       &mod_super);

  mod = 0;
  if (state & GDK_SHIFT_MASK)
    mod |= shift_modifier;
  if (state & GDK_CONTROL_MASK)
    mod |= mod_ctrl;
  if (state & GDK_META_MASK || state & GDK_MOD1_MASK)
    mod |= mod_meta;
  if (state & GDK_SUPER_MASK)
    mod |= mod_super;
  if (state & GDK_HYPER_MASK)
    mod |= mod_hyper;

  return mod;
}

int
pgtk_emacs_to_gtk_modifiers (struct pgtk_display_info *dpyinfo,
			     int state)
{
  int mod_ctrl;
  int mod_meta;
  int mod_alt;
  int mod_hyper;
  int mod_super;
  int mask;

  get_modifier_values (&mod_ctrl, &mod_meta, &mod_alt, &mod_hyper,
		       &mod_super);

  mask = 0;
  if (state & mod_super)
    mask |= GDK_SUPER_MASK;
  if (state & mod_hyper)
    mask |= GDK_HYPER_MASK;
  if (state & shift_modifier)
    mask |= GDK_SHIFT_MASK;
  if (state & mod_ctrl)
    mask |= GDK_CONTROL_MASK;
  if (state & mod_meta)
    mask |= GDK_MOD1_MASK;
  return mask;
}

#define IsCursorKey(keysym) (0xff50 <= (keysym) && (keysym) < 0xff60)
#define IsMiscFunctionKey(keysym) \
  (0xff60 <= (keysym) && (keysym) < 0xff6c)
#define IsKeypadKey(keysym) (0xff80 <= (keysym) && (keysym) < 0xffbe)
#define IsFunctionKey(keysym) \
  (0xffbe <= (keysym) && (keysym) < 0xffe1)
#define IsModifierKey(keysym)                                       \
  ((((keysym) >= GDK_KEY_Shift_L) && ((keysym) <= GDK_KEY_Hyper_R)) \
   || (((keysym) >= GDK_KEY_ISO_Lock)                               \
       && ((keysym) <= GDK_KEY_ISO_Level5_Lock))                    \
   || ((keysym) == GDK_KEY_Mode_switch)                             \
   || ((keysym) == GDK_KEY_Num_Lock))

void
pgtk_enqueue_string (struct frame *f, gchar *str)
{
  gunichar *ustr, *uptr;

  uptr = ustr = g_utf8_to_ucs4 (str, -1, NULL, NULL, NULL);
  if (ustr == NULL)
    return;
  for (; *ustr != 0; ustr++)
    {
      union buffered_input_event inev;
      Lisp_Object c = make_fixnum (*ustr);
      EVENT_INIT (inev.ie);
      inev.ie.kind = (SINGLE_BYTE_CHAR_P (XFIXNAT (c))
			? ASCII_KEYSTROKE_EVENT
			: MULTIBYTE_CHAR_KEYSTROKE_EVENT);
      inev.ie.arg = Qnil;
      inev.ie.code = XFIXNAT (c);
      XSETFRAME (inev.ie.frame_or_window, f);
      inev.ie.modifiers = 0;
      inev.ie.timestamp = 0;
      evq_enqueue (&inev);
    }

  g_free (uptr);
}

void
pgtk_enqueue_preedit (struct frame *f, Lisp_Object preedit)
{
  union buffered_input_event inev;
  EVENT_INIT (inev.ie);
  inev.ie.kind = PREEDIT_TEXT_EVENT;
  inev.ie.arg = preedit;
  inev.ie.code = 0;
  XSETFRAME (inev.ie.frame_or_window, f);
  inev.ie.modifiers = 0;
  inev.ie.timestamp = 0;
  evq_enqueue (&inev);
}

static gboolean
key_press_event (GtkWidget *widget, GdkEvent *event,
		 gpointer *user_data)
{
  union buffered_input_event inev;
  ptrdiff_t nbytes;
  Mouse_HLInfo *hlinfo;
  struct frame *f;
  struct pgtk_display_info *dpyinfo;

  f = pgtk_any_window_to_frame (gtk_widget_get_window (widget));
  EVENT_INIT (inev.ie);
  hlinfo = MOUSE_HL_INFO (f);
  nbytes = 0;

  /* If mouse-highlight is an integer, input clears out
     mouse highlighting.  */
  if (!hlinfo->mouse_face_hidden && INTEGERP (Vmouse_highlight))
    {
      clear_mouse_face (hlinfo);
      hlinfo->mouse_face_hidden = true;
    }

  if (f != 0)
    {
      guint keysym, orig_keysym;
      /* al%imercury@uunet.uu.net says that making this 81
	 instead of 80 fixed a bug whereby meta chars made
	 his Emacs hang.

	 It seems that some version of XmbLookupString has
	 a bug of not returning XBufferOverflow in
	 status_return even if the input is too long to
	 fit in 81 bytes.  So, we must prepare sufficient
	 bytes for copy_buffer.  513 bytes (256 chars for
	 two-byte character set) seems to be a fairly good
	 approximation.  -- 2000.8.10 handa@etl.go.jp  */
      unsigned char copy_buffer[513];
      unsigned char *copy_bufptr = copy_buffer;
      int copy_bufsiz = sizeof (copy_buffer);
      int modifiers;
      Lisp_Object c;
      guint state;

      dpyinfo = FRAME_DISPLAY_INFO (f);

      /* Set the last user time for pgtk_focus_frame to work
	 correctly.  */
      dpyinfo->last_user_time = event->key.time;

      state = event->key.state;

      /* While super is pressed, the input method will always always
	 resend the key events ignoring super.  As a workaround, don't
	 filter key events with super or hyper pressed.  */
      if (!(event->key.state & (GDK_SUPER_MASK | GDK_HYPER_MASK)))
	{
	  if (pgtk_im_filter_keypress (f, &event->key))
	    return TRUE;
	}

      state |= pgtk_emacs_to_gtk_modifiers (FRAME_DISPLAY_INFO (f),
					    extra_keyboard_modifiers);
      modifiers = state;

      /* This will have to go some day...  */

      /* make_lispy_event turns chars into control chars.
	 Don't do it here because XLookupString is too eager.  */
      state &= ~GDK_CONTROL_MASK;
      state &= ~(GDK_META_MASK | GDK_SUPER_MASK | GDK_HYPER_MASK
		 | GDK_MOD1_MASK);

      nbytes = event->key.length;
      if (nbytes > copy_bufsiz)
	nbytes = copy_bufsiz;
      memcpy (copy_bufptr, event->key.string, nbytes);

      keysym = event->key.keyval;
      orig_keysym = keysym;

      /* Common for all keysym input events.  */
      XSETFRAME (inev.ie.frame_or_window, f);
      inev.ie.modifiers
	= pgtk_gtk_to_emacs_modifiers (FRAME_DISPLAY_INFO (f),
				       modifiers);
      inev.ie.timestamp = event->key.time;

      /* First deal with keysyms which have defined
	 translations to characters.  */
      if (keysym >= 32 && keysym < 128)
	/* Avoid explicitly decoding each ASCII character.  */
	{
	  inev.ie.kind = ASCII_KEYSTROKE_EVENT;
	  inev.ie.code = keysym;

	  inev.ie.device
	    = pgtk_get_device_for_event (FRAME_DISPLAY_INFO (f),
					 event);
	  goto done;
	}

      /* Keysyms directly mapped to Unicode characters.  */
      if (keysym >= 0x01000000 && keysym <= 0x0110FFFF)
	{
	  if (keysym < 0x01000080)
	    inev.ie.kind = ASCII_KEYSTROKE_EVENT;
	  else
	    inev.ie.kind = MULTIBYTE_CHAR_KEYSTROKE_EVENT;
	  inev.ie.code = keysym & 0xFFFFFF;

	  inev.ie.device
	    = pgtk_get_device_for_event (FRAME_DISPLAY_INFO (f),
					 event);
	  goto done;
	}

      /* Now non-ASCII.  */
      if (HASH_TABLE_P (Vpgtk_keysym_table)
	  && (c = Fgethash (make_fixnum (keysym), Vpgtk_keysym_table,
			    Qnil),
	      FIXNATP (c)))
	{
	  inev.ie.kind = (SINGLE_BYTE_CHAR_P (XFIXNAT (c))
			    ? ASCII_KEYSTROKE_EVENT
			    : MULTIBYTE_CHAR_KEYSTROKE_EVENT);
	  inev.ie.code = XFIXNAT (c);

	  inev.ie.device
	    = pgtk_get_device_for_event (FRAME_DISPLAY_INFO (f),
					 event);
	  goto done;
	}

      /* Random non-modifier sorts of keysyms.  */
      if (((keysym >= GDK_KEY_BackSpace && keysym <= GDK_KEY_Escape)
	   || keysym == GDK_KEY_Delete
#ifdef GDK_KEY_ISO_Left_Tab
	   || (keysym >= GDK_KEY_ISO_Left_Tab
	       && keysym <= GDK_KEY_ISO_Enter)
#endif
	   || IsCursorKey (keysym)	 /* 0xff50 <= x < 0xff60 */
	   || IsMiscFunctionKey (keysym) /* 0xff60 <= x < VARIES */
#ifdef HPUX
	   /* This recognizes the "extended function
	      keys".  It seems there's no cleaner way.
	      Test IsModifierKey to avoid handling
	      mode_switch incorrectly.  */
	   || (GDK_KEY_Select <= keysym && keysym < GDK_KEY_KP_Space)
#endif
#ifdef GDK_KEY_dead_circumflex
	   || orig_keysym == GDK_KEY_dead_circumflex
#endif
#ifdef GDK_KEY_dead_grave
	   || orig_keysym == GDK_KEY_dead_grave
#endif
#ifdef GDK_KEY_dead_tilde
	   || orig_keysym == GDK_KEY_dead_tilde
#endif
#ifdef GDK_KEY_dead_diaeresis
	   || orig_keysym == GDK_KEY_dead_diaeresis
#endif
#ifdef GDK_KEY_dead_macron
	   || orig_keysym == GDK_KEY_dead_macron
#endif
#ifdef GDK_KEY_dead_degree
	   || orig_keysym == GDK_KEY_dead_degree
#endif
#ifdef GDK_KEY_dead_acute
	   || orig_keysym == GDK_KEY_dead_acute
#endif
#ifdef GDK_KEY_dead_cedilla
	   || orig_keysym == GDK_KEY_dead_cedilla
#endif
#ifdef GDK_KEY_dead_breve
	   || orig_keysym == GDK_KEY_dead_breve
#endif
#ifdef GDK_KEY_dead_ogonek
	   || orig_keysym == GDK_KEY_dead_ogonek
#endif
#ifdef GDK_KEY_dead_caron
	   || orig_keysym == GDK_KEY_dead_caron
#endif
#ifdef GDK_KEY_dead_doubleacute
	   || orig_keysym == GDK_KEY_dead_doubleacute
#endif
#ifdef GDK_KEY_dead_abovedot
	   || orig_keysym == GDK_KEY_dead_abovedot
#endif
	   || IsKeypadKey (keysym)   /* 0xff80 <= x < 0xffbe */
	   || IsFunctionKey (keysym) /* 0xffbe <= x < 0xffe1 */
	   /* Any "vendor-specific" key is ok.  */
	   || (orig_keysym & (1 << 28))
	   || (keysym != GDK_KEY_VoidSymbol && nbytes == 0))
	  && !(event->key.is_modifier || IsModifierKey (orig_keysym)
      /* The symbols from GDK_KEY_ISO_Lock
	 to GDK_KEY_ISO_Last_Group_Lock
	 don't have real modifiers but
	 should be treated similarly to
	 Mode_switch by Emacs. */
#if defined GDK_KEY_ISO_Lock && defined GDK_KEY_ISO_Last_Group_Lock
	       || (GDK_KEY_ISO_Lock <= orig_keysym
		   && orig_keysym <= GDK_KEY_ISO_Last_Group_Lock)
#endif
		 ))
	{
	  /* make_lispy_event will convert this to a symbolic
	     key.  */
	  inev.ie.kind = NON_ASCII_KEYSTROKE_EVENT;
	  inev.ie.code = keysym;

	  inev.ie.device
	    = pgtk_get_device_for_event (FRAME_DISPLAY_INFO (f),
					 event);
	  goto done;
	}

      {
	inev.ie.kind = MULTIBYTE_CHAR_KEYSTROKE_EVENT;
	inev.ie.arg
	  = make_unibyte_string ((char *) copy_bufptr, nbytes);
	inev.ie.device
	  = pgtk_get_device_for_event (FRAME_DISPLAY_INFO (f), event);

	if (keysym == GDK_KEY_VoidSymbol)
	  goto done;
      }
    }

done:
  if (inev.ie.kind != NO_EVENT)
    {
      XSETFRAME (inev.ie.frame_or_window, f);
      evq_enqueue (&inev);
    }

  return TRUE;
}

static struct pgtk_display_info *
pgtk_display_info_for_display (GdkDisplay *dpy)
{
  struct pgtk_display_info *dpyinfo;

  for (dpyinfo = x_display_list; dpyinfo; dpyinfo = dpyinfo->next)
    {
      if (dpyinfo->display == dpy)
	return dpyinfo;
    }

  return NULL;
}

static gboolean
key_release_event (GtkWidget *widget, GdkEvent *event,
		   gpointer *user_data)
{
  GdkDisplay *display;
  struct pgtk_display_info *dpyinfo;

  display = gtk_widget_get_display (widget);
  dpyinfo = pgtk_display_info_for_display (display);

  if (dpyinfo)
    /* This is needed on Wayland because of some brain dead
       compositors.  Without them, we would not have to keep track of
       the serial of key release events.  */
    dpyinfo->last_user_time = event->key.time;

  return TRUE;
}

static gboolean
configure_event (GtkWidget *widget, GdkEvent *event,
		 gpointer *user_data)
{
  struct frame *f
    = pgtk_any_window_to_frame (event->configure.window);

  if (f && widget == FRAME_GTK_OUTER_WIDGET (f))
    {
      if (any_help_event_p)
	{
	  Lisp_Object frame;
	  if (f)
	    XSETFRAME (frame, f);
	  else
	    frame = Qnil;
	  help_echo_string = Qnil;
	  gen_help_event (Qnil, frame, Qnil, Qnil, 0);
	}

      if (f->win_gravity == NorthWestGravity)
	gtk_window_get_position (GTK_WINDOW (widget), &f->left_pos,
				 &f->top_pos);
      else
	{
	  f->top_pos = event->configure.y;
	  f->left_pos = event->configure.x;
	}
    }
  return FALSE;
}

static gboolean
map_event (GtkWidget *widget, GdkEvent *event, gpointer *user_data)
{
  struct frame *f = pgtk_any_window_to_frame (event->any.window);
  union buffered_input_event inev;

  EVENT_INIT (inev.ie);
  inev.ie.kind = NO_EVENT;
  inev.ie.arg = Qnil;

  if (f)
    {
      bool iconified = FRAME_ICONIFIED_P (f);

      /* Check if fullscreen was specified before we where mapped the
	 first time, i.e. from the command line.  */
      if (!FRAME_X_OUTPUT (f)->has_been_visible)
	set_fullscreen_state (f);

      if (!iconified)
	{
	  /* The `z-group' is reset every time a frame becomes
	     invisible.  Handle this here.  */
	  if (FRAME_Z_GROUP (f) == z_group_above)
	    pgtk_set_z_group (f, Qabove, Qnil);
	  else if (FRAME_Z_GROUP (f) == z_group_below)
	    pgtk_set_z_group (f, Qbelow, Qnil);
	}

      SET_FRAME_VISIBLE (f, 1);
      SET_FRAME_ICONIFIED (f, false);
      FRAME_X_OUTPUT (f)->has_been_visible = true;

#ifdef USE_SKIA
      /* Wayland compositors may discard surface content when windows
	 are unmapped.  Mark as garbaged so Emacs redisplay redraws
	 the content.  Do NOT queue a draw here — redisplay
	 hasn't run yet, so the render callback would blit a stale or
	 empty surface.  pgtk_frame_up_to_date will queue the render
	 after content has been drawn.  */
      if (FRAME_GL_DRAWING_AREA (f))
	SET_FRAME_GARBAGED (f);
#endif

      if (iconified)
	{
	  inev.ie.kind = DEICONIFY_EVENT;
	  XSETFRAME (inev.ie.frame_or_window, f);
	}
    }

  if (inev.ie.kind != NO_EVENT)
    evq_enqueue (&inev);
  return FALSE;
}

static gboolean
window_state_event (GtkWidget *widget, GdkEvent *event,
		    gpointer *user_data)
{
  struct frame *f
    = pgtk_any_window_to_frame (event->window_state.window);
  GdkWindowState new_state;
  union buffered_input_event inev;

  new_state = event->window_state.new_window_state;

  EVENT_INIT (inev.ie);
  inev.ie.kind = NO_EVENT;
  inev.ie.arg = Qnil;

  if (new_state & GDK_WINDOW_STATE_FULLSCREEN)
    store_frame_param (f, Qfullscreen, Qfullboth);
  else if (new_state & GDK_WINDOW_STATE_MAXIMIZED)
    store_frame_param (f, Qfullscreen, Qmaximized);
  else if ((new_state & GDK_WINDOW_STATE_TOP_TILED)
	   && (new_state & GDK_WINDOW_STATE_BOTTOM_TILED)
	   && !(new_state & GDK_WINDOW_STATE_TOP_RESIZABLE)
	   && !(new_state & GDK_WINDOW_STATE_BOTTOM_RESIZABLE))
    store_frame_param (f, Qfullscreen, Qfullheight);
  else if ((new_state & GDK_WINDOW_STATE_LEFT_TILED)
	   && (new_state & GDK_WINDOW_STATE_RIGHT_TILED)
	   && !(new_state & GDK_WINDOW_STATE_LEFT_RESIZABLE)
	   && !(new_state & GDK_WINDOW_STATE_RIGHT_RESIZABLE))
    store_frame_param (f, Qfullscreen, Qfullwidth);
  else
    store_frame_param (f, Qfullscreen, Qnil);

  /* The Wayland protocol provides no way for the client to know
     whether or not one of its toplevels has actually been
     deiconified.  It only provides a request for clients to iconify a
     toplevel, without even the ability to determine whether or not
     the iconification request was rejected by the display server.

     GDK computes the iconified state by sending a window state event
     containing only GDK_WINDOW_STATE_ICONIFIED immediately after
     gtk_window_iconify is called.  That is error-prone if the request
     to iconify the frame was rejected by the display server, but is
     not the main problem here, as Wayland compositors only rarely
     reject such requests.  GDK also assumes that it can clear the
     iconified state upon receiving the next toplevel configure event
     from the display server.  Unfortunately, such events can be sent
     by Wayland compositors while the frame is iconified, and may also
     not be sent upon deiconification.  So, no matter what Emacs does,
     the iconification state of a frame is likely to be wrong under
     one situation or another.  */

  if (new_state & GDK_WINDOW_STATE_ICONIFIED)
    {
      SET_FRAME_ICONIFIED (f, true);
      SET_FRAME_VISIBLE (f, false);
    }
  else
    {
      FRAME_X_OUTPUT (f)->has_been_visible = true;
      inev.ie.kind = DEICONIFY_EVENT;
      XSETFRAME (inev.ie.frame_or_window, f);
      SET_FRAME_ICONIFIED (f, false);
      SET_FRAME_VISIBLE (f, true);
    }

  if (new_state & GDK_WINDOW_STATE_STICKY)
    store_frame_param (f, Qsticky, Qt);
  else
    store_frame_param (f, Qsticky, Qnil);

#ifdef USE_SKIA
  /* Window state changes (fullscreen, maximize, etc.) may resize the
     frame or invalidate compositor-side content.  Mark as garbaged so
     Emacs redisplay redraws.  Do NOT queue a draw here —
     redisplay hasn't run yet, so the render callback would blit
     stale content.  pgtk_frame_up_to_date will queue the render
     after content has been drawn.  */
  if (FRAME_GL_DRAWING_AREA (f))
    SET_FRAME_GARBAGED (f);
#endif

  if (inev.ie.kind != NO_EVENT)
    evq_enqueue (&inev);
  return FALSE;
}

static gboolean
delete_event (GtkWidget *widget, GdkEvent *event, gpointer *user_data)
{
  struct frame *f = pgtk_any_window_to_frame (event->any.window);
  union buffered_input_event inev;

  EVENT_INIT (inev.ie);
  inev.ie.kind = NO_EVENT;
  inev.ie.arg = Qnil;

  if (f)
    {
      inev.ie.kind = DELETE_WINDOW_EVENT;
      XSETFRAME (inev.ie.frame_or_window, f);
    }

  if (inev.ie.kind != NO_EVENT)
    evq_enqueue (&inev);
  return TRUE;
}

/* The focus may have changed.  Figure out if it is a real focus
   change, by checking both FocusIn/Out and Enter/LeaveNotify events.

   Returns FOCUS_IN_EVENT event in *BUFP. */

/* Handle FocusIn and FocusOut state changes for FRAME.
   If FRAME has focus and there exists more than one frame, puts
   a FOCUS_IN_EVENT into *BUFP.  */

static void
pgtk_focus_changed (gboolean is_enter, int state,
		    struct pgtk_display_info *dpyinfo,
		    struct frame *frame,
		    union buffered_input_event *bufp)
{
  if (is_enter)
    {
      if (dpyinfo->x_focus_event_frame != frame)
	{
	  pgtk_new_focus_frame (dpyinfo, frame);
	  dpyinfo->x_focus_event_frame = frame;

	  /* Don't stop displaying the initial startup message
	     for a switch-frame event we don't need.  */
	  /* When run as a daemon, Vterminal_frame is always NIL.  */
	  bufp->ie.arg
	    = (((NILP (Vterminal_frame)
		 || !FRAME_PGTK_P (XFRAME (Vterminal_frame))
		 || EQ (Fdaemonp (), Qt))
		&& CONSP (Vframe_list) && !NILP (XCDR (Vframe_list)))
		 ? Qt
		 : Qnil);
	  bufp->ie.kind = FOCUS_IN_EVENT;
	  XSETFRAME (bufp->ie.frame_or_window, frame);
	}

      frame->output_data.pgtk->focus_state |= state;
    }
  else
    {
      frame->output_data.pgtk->focus_state &= ~state;

      if (dpyinfo->x_focus_event_frame == frame)
	{
	  dpyinfo->x_focus_event_frame = 0;
	  pgtk_new_focus_frame (dpyinfo, NULL);

	  bufp->ie.kind = FOCUS_OUT_EVENT;
	  XSETFRAME (bufp->ie.frame_or_window, frame);
	}

      if (frame->pointer_invisible)
	pgtk_toggle_invisible_pointer (frame, false);
    }
}

static gboolean
enter_notify_event (GtkWidget *widget, GdkEvent *event,
		    gpointer *user_data)
{
  union buffered_input_event inev;
  struct frame *frame
    = pgtk_any_window_to_frame (gtk_widget_get_window (widget));

  if (frame == NULL)
    return FALSE;

  struct pgtk_display_info *dpyinfo = FRAME_DISPLAY_INFO (frame);
  struct frame *focus_frame = dpyinfo->x_focus_frame;
  int focus_state
    = focus_frame ? focus_frame->output_data.pgtk->focus_state : 0;

  EVENT_INIT (inev.ie);
  inev.ie.kind = NO_EVENT;
  inev.ie.arg = Qnil;

  if (event->crossing.detail != GDK_NOTIFY_INFERIOR
      && event->crossing.focus && !(focus_state & FOCUS_EXPLICIT))
    pgtk_focus_changed (TRUE, FOCUS_IMPLICIT, dpyinfo, frame, &inev);
  if (inev.ie.kind != NO_EVENT)
    evq_enqueue (&inev);
  return TRUE;
}

static gboolean
leave_notify_event (GtkWidget *widget, GdkEvent *event,
		    gpointer *user_data)
{
  union buffered_input_event inev;
  struct frame *frame
    = pgtk_any_window_to_frame (gtk_widget_get_window (widget));

  if (frame == NULL)
    return FALSE;

  struct pgtk_display_info *dpyinfo = FRAME_DISPLAY_INFO (frame);
  struct frame *focus_frame = dpyinfo->x_focus_frame;
  int focus_state
    = focus_frame ? focus_frame->output_data.pgtk->focus_state : 0;
  Mouse_HLInfo *hlinfo = MOUSE_HL_INFO (frame);

  if (frame == hlinfo->mouse_face_mouse_frame)
    {
      /* If we move outside the frame, then we're
	 certainly no longer on any text in the frame.  */
      clear_mouse_face (hlinfo);
      hlinfo->mouse_face_mouse_frame = 0;
    }

  EVENT_INIT (inev.ie);
  inev.ie.kind = NO_EVENT;
  inev.ie.arg = Qnil;

  if (event->crossing.detail != GDK_NOTIFY_INFERIOR
      && event->crossing.focus && !(focus_state & FOCUS_EXPLICIT))
    pgtk_focus_changed (FALSE, FOCUS_IMPLICIT, dpyinfo, frame, &inev);

  if (frame)
    {
      if (any_help_event_p)
	{
	  Lisp_Object frame_obj;
	  XSETFRAME (frame_obj, frame);
	  help_echo_string = Qnil;
	  gen_help_event (Qnil, frame_obj, Qnil, Qnil, 0);
	}
    }

  if (inev.ie.kind != NO_EVENT)
    evq_enqueue (&inev);
  return TRUE;
}

static gboolean
focus_in_event (GtkWidget *widget, GdkEvent *event,
		gpointer *user_data)
{
  union buffered_input_event inev;
  struct frame *frame
    = pgtk_any_window_to_frame (gtk_widget_get_window (widget));

  if (frame == NULL)
    return TRUE;

  EVENT_INIT (inev.ie);
  inev.ie.kind = NO_EVENT;
  inev.ie.arg = Qnil;

  pgtk_focus_changed (TRUE, FOCUS_EXPLICIT,
		      FRAME_DISPLAY_INFO (frame), frame, &inev);
  if (inev.ie.kind != NO_EVENT)
    evq_enqueue (&inev);

  pgtk_im_focus_in (frame);

#ifdef USE_SKIA
  /* Force a complete redraw when focus is regained.  Wayland
     compositors may discard or invalidate surface content when
     windows are unfocused or their state changes.  Do NOT queue
     a draw here — redisplay hasn't run yet, so the
     render callback would blit stale content or hit the NULL-surface
     path.  SET_FRAME_GARBAGED triggers redisplay, and
     pgtk_frame_up_to_date queues the render after content is drawn.  */
  if (FRAME_GL_DRAWING_AREA (frame))
    SET_FRAME_GARBAGED (frame);
#endif

  return TRUE;
}

static gboolean
focus_out_event (GtkWidget *widget, GdkEvent *event,
		 gpointer *user_data)
{
  union buffered_input_event inev;
  struct frame *frame
    = pgtk_any_window_to_frame (gtk_widget_get_window (widget));

  if (frame == NULL)
    return TRUE;

  EVENT_INIT (inev.ie);
  inev.ie.kind = NO_EVENT;
  inev.ie.arg = Qnil;

  pgtk_focus_changed (FALSE, FOCUS_EXPLICIT,
		      FRAME_DISPLAY_INFO (frame), frame, &inev);
  if (inev.ie.kind != NO_EVENT)
    evq_enqueue (&inev);

  pgtk_im_focus_out (frame);

  return TRUE;
}

/* Function to report a mouse movement to the mainstream Emacs code.
   The input handler calls this.

   We have received a mouse movement event, which is given in *event.
   If the mouse is over a different glyph than it was last time, tell
   the mainstream emacs code by setting mouse_moved.  If not, ask for
   another motion event, so we can check again the next time it moves.
 */

static bool
note_mouse_movement (struct frame *frame, const GdkEventMotion *event)
{
  XRectangle *r;
  struct pgtk_display_info *dpyinfo;

  if (!FRAME_X_OUTPUT (frame))
    return false;

  dpyinfo = FRAME_DISPLAY_INFO (frame);
  dpyinfo->last_mouse_movement_time = event->time;
  dpyinfo->last_mouse_motion_frame = frame;
  dpyinfo->last_mouse_motion_x = event->x;
  dpyinfo->last_mouse_motion_y = event->y;

  if (event->window
      != gtk_widget_get_window (FRAME_GTK_WIDGET (frame)))
    {
      frame->mouse_moved = true;
      dpyinfo->last_mouse_scroll_bar = NULL;
      note_mouse_highlight (frame, -1, -1);
      dpyinfo->last_mouse_glyph_frame = NULL;
      frame->last_mouse_device
	= pgtk_get_device_for_event (FRAME_DISPLAY_INFO (frame),
				     (GdkEvent *) event);
      return true;
    }

  /* Has the mouse moved off the glyph it was on at the last sighting?
   */
  r = &dpyinfo->last_mouse_glyph;
  if (frame != dpyinfo->last_mouse_glyph_frame || event->x < r->x
      || event->x >= r->x + (int) r->width || event->y < r->y
      || event->y >= r->y + (int) r->height)
    {
      frame->mouse_moved = true;
      dpyinfo->last_mouse_scroll_bar = NULL;
      note_mouse_highlight (frame, event->x, event->y);
      /* Remember which glyph we're now on.  */
      remember_mouse_glyph (frame, event->x, event->y, r);
      dpyinfo->last_mouse_glyph_frame = frame;
      frame->last_mouse_device
	= pgtk_get_device_for_event (FRAME_DISPLAY_INFO (frame),
				     (GdkEvent *) event);
      return true;
    }

  return false;
}

static gboolean
motion_notify_event (GtkWidget *widget, GdkEvent *event,
		     gpointer *user_data)
{
  union buffered_input_event inev;
  struct frame *f, *frame;
  struct pgtk_display_info *dpyinfo;
  Mouse_HLInfo *hlinfo;
  GdkDevice *device;

  /* Ignore emulated pointer events generated from a touch screen
     event.  */
  if (gdk_event_get_pointer_emulated (event)
      /* The event must not have emerged from a touch device either,
	 as GDK does not set pointer_emulated in events generated on
	 Wayland as on X, and as the X Input Extension specifies.  */
      || ((device = gdk_event_get_source_device (event))
	  && (gdk_device_get_source (device)
	      == GDK_SOURCE_TOUCHSCREEN)))
    return FALSE;

  EVENT_INIT (inev.ie);
  inev.ie.kind = NO_EVENT;
  inev.ie.arg = Qnil;

  previous_help_echo_string = help_echo_string;
  help_echo_string = Qnil;

  frame = pgtk_any_window_to_frame (gtk_widget_get_window (widget));
  dpyinfo = FRAME_DISPLAY_INFO (frame);
  f = (gui_mouse_grabbed (dpyinfo)
	 ? dpyinfo->last_mouse_frame
	 : pgtk_any_window_to_frame (gtk_widget_get_window (widget)));
  hlinfo = MOUSE_HL_INFO (f);

  if (hlinfo->mouse_face_hidden)
    {
      hlinfo->mouse_face_hidden = false;
      clear_mouse_face (hlinfo);
    }

  if (f && xg_event_is_for_scrollbar (f, event, false))
    f = 0;

  if (f)
    {
      /* Maybe generate a SELECT_WINDOW_EVENT for
	 `mouse-autoselect-window' but don't let popup menus
	 interfere with this (Bug#1261).  */
      if (!NILP (Vmouse_autoselect_window)
	  /* Don't switch if we're currently in the minibuffer.
	     This tries to work around problems where the
	     minibuffer gets unselected unexpectedly, and where
	     you then have to move your mouse all the way down to
	     the minibuffer to select it.  */
	  && !MINI_WINDOW_P (XWINDOW (selected_window))
	  /* With `focus-follows-mouse' non-nil create an event
	     also when the target window is on another frame.  */
	  && (f == XFRAME (selected_frame)
	      || !NILP (focus_follows_mouse)))
	{
	  static Lisp_Object last_mouse_window;
	  Lisp_Object window
	    = window_from_coordinates (f, event->motion.x,
				       event->motion.y, 0, false,
				       false, false);

	  /* A window will be autoselected only when it is not
	     selected now and the last mouse movement event was
	     not in it.  The remainder of the code is a bit vague
	     wrt what a "window" is.  For immediate autoselection,
	     the window is usually the entire window but for GTK
	     where the scroll bars don't count.  For delayed
	     autoselection the window is usually the window's text
	     area including the margins.  */
	  if (WINDOWP (window) && !EQ (window, last_mouse_window)
	      && !EQ (window, selected_window))
	    {
	      inev.ie.kind = SELECT_WINDOW_EVENT;
	      inev.ie.frame_or_window = window;
	    }

	  /* Remember the last window where we saw the mouse.  */
	  last_mouse_window = window;
	}

      if (!note_mouse_movement (f, &event->motion))
	help_echo_string = previous_help_echo_string;
    }
  else
    /* If we move outside the frame, then we're
       certainly no longer on any text in the frame.  */
    clear_mouse_face (hlinfo);

  /* If the contents of the global variable help_echo_string
     has changed, generate a HELP_EVENT.  */
  int do_help = 0;
  if (!NILP (help_echo_string) || !NILP (previous_help_echo_string))
    do_help = 1;

  if (inev.ie.kind != NO_EVENT)
    evq_enqueue (&inev);

  if (do_help > 0)
    {
      Lisp_Object frame;

      if (f)
	XSETFRAME (frame, f);
      else
	frame = Qnil;

      any_help_event_p = true;
      gen_help_event (help_echo_string, frame, help_echo_window,
		      help_echo_object, help_echo_pos);
    }

  return TRUE;
}

/* Prepare a mouse-event in *RESULT for placement in the input queue.

   If the event is a button press, then note that we have grabbed
   the mouse.  */

static Lisp_Object
construct_mouse_click (struct input_event *result,
		       const GdkEventButton *event, struct frame *f)
{
  /* Make the event type NO_EVENT; we'll change that when we decide
     otherwise.  */
  result->kind = MOUSE_CLICK_EVENT;
  result->code = event->button - 1;
  result->timestamp = event->time;
  result->modifiers
    = (pgtk_gtk_to_emacs_modifiers (FRAME_DISPLAY_INFO (f),
				    event->state)
       | (event->type == GDK_BUTTON_RELEASE ? up_modifier
					    : down_modifier));

  XSETINT (result->x, event->x);
  XSETINT (result->y, event->y);
  XSETFRAME (result->frame_or_window, f);
  result->arg = Qnil;
  result->device = pgtk_get_device_for_event (FRAME_DISPLAY_INFO (f),
					      (GdkEvent *) event);
  return Qnil;
}

static gboolean
button_event (GtkWidget *widget, GdkEvent *event, gpointer *user_data)
{
  union buffered_input_event inev;
  struct frame *f, *frame;
  struct pgtk_display_info *dpyinfo;

  /* If we decide we want to generate an event to be seen
     by the rest of Emacs, we put it here.  */
  bool tab_bar_p = false;
  bool tool_bar_p = false;
  Lisp_Object tab_bar_arg = Qnil;
  GdkDevice *device;

  /* Ignore emulated pointer events generated from a touch screen
     event.  */
  if (gdk_event_get_pointer_emulated (event)
      /* The event must not have emerged from a touch device either,
	 as GDK does not set pointer_emulated in events generated on
	 Wayland as on X, and as the X Input Extension specifies.  */
      || ((device = gdk_event_get_source_device (event))
	  && (gdk_device_get_source (device)
	      == GDK_SOURCE_TOUCHSCREEN)))
    return FALSE;

  EVENT_INIT (inev.ie);
  inev.ie.kind = NO_EVENT;
  inev.ie.arg = Qnil;

  /* ignore double click and triple click. */
  if (event->type != GDK_BUTTON_PRESS
      && event->type != GDK_BUTTON_RELEASE)
    return TRUE;

  frame = pgtk_any_window_to_frame (gtk_widget_get_window (widget));
  dpyinfo = FRAME_DISPLAY_INFO (frame);

  dpyinfo->last_mouse_glyph_frame = NULL;

  if (gui_mouse_grabbed (dpyinfo))
    f = dpyinfo->last_mouse_frame;
  else
    {
      f = pgtk_any_window_to_frame (gtk_widget_get_window (widget));

      if (f && event->button.type == GDK_BUTTON_PRESS
	  && !FRAME_NO_ACCEPT_FOCUS (f))
	{
	  /* When clicking into a child frame or when clicking
	     into a parent frame with the child frame selected and
	     `no-accept-focus' is not set, select the clicked
	     frame.  */
	  struct frame *hf = dpyinfo->highlight_frame;

	  if (FRAME_PARENT_FRAME (f)
	      || (hf && frame_ancestor_p (f, hf)))
	    {
	      block_input ();
	      gtk_widget_grab_focus (FRAME_GTK_WIDGET (f));

	      if (FRAME_GTK_OUTER_WIDGET (f))
		gtk_window_present (
		  GTK_WINDOW (FRAME_GTK_OUTER_WIDGET (f)));
	      unblock_input ();
	    }
	}
    }

  /* Set the last user time, used to activate the frame in
     pgtk_focus_frame.  */
  dpyinfo->last_user_time = event->button.time;

  if (f)
    {
      /* Is this in the tab-bar?  */
      if (WINDOWP (f->tab_bar_window)
	  && WINDOW_TOTAL_LINES (XWINDOW (f->tab_bar_window)))
	{
	  Lisp_Object window;
	  int x = event->button.x;
	  int y = event->button.y;

	  window
	    = window_from_coordinates (f, x, y, 0, true, true, true);
	  tab_bar_p = EQ (window, f->tab_bar_window);

	  if (tab_bar_p)
	    tab_bar_arg = handle_tab_bar_click (
	      f, x, y, event->type == GDK_BUTTON_PRESS,
	      pgtk_gtk_to_emacs_modifiers (dpyinfo,
					   event->button.state));
	}

      if (!(tab_bar_p && NILP (tab_bar_arg)) && !tool_bar_p)
	{
	  if (ignore_next_mouse_click_timeout)
	    {
	      if (event->type == GDK_BUTTON_PRESS
		  && event->button.time
		       > ignore_next_mouse_click_timeout)
		{
		  ignore_next_mouse_click_timeout = 0;
		  construct_mouse_click (&inev.ie, &event->button, f);
		}
	      if (event->type == GDK_BUTTON_RELEASE)
		ignore_next_mouse_click_timeout = 0;
	    }
	  else
	    construct_mouse_click (&inev.ie, &event->button, f);

	  if (!NILP (tab_bar_arg))
	    inev.ie.arg = tab_bar_arg;
	}
    }

  if (event->type == GDK_BUTTON_PRESS)
    {
      dpyinfo->grabbed |= (1 << event->button.button);
      dpyinfo->last_mouse_frame = f;

      if (dpyinfo->last_click_event != NULL)
	gdk_event_free (dpyinfo->last_click_event);
      dpyinfo->last_click_event = gdk_event_copy (event);
    }
  else
    dpyinfo->grabbed &= ~(1 << event->button.button);

  /* Ignore any mouse motion that happened before this event;
     any subsequent mouse-movement Emacs events should reflect
     only motion after the ButtonPress/Release.  */
  if (f != 0)
    f->mouse_moved = false;

  if (inev.ie.kind != NO_EVENT)
    evq_enqueue (&inev);
  return TRUE;
}

static gboolean
scroll_event (GtkWidget *widget, GdkEvent *event, gpointer *user_data)
{
  union buffered_input_event inev;
  struct frame *f, *frame;
  struct pgtk_display_info *dpyinfo;
  GdkScrollDirection dir;
  double delta_x, delta_y;

  EVENT_INIT (inev.ie);
  inev.ie.kind = NO_EVENT;
  inev.ie.arg = Qnil;

  frame = pgtk_any_window_to_frame (gtk_widget_get_window (widget));
  dpyinfo = FRAME_DISPLAY_INFO (frame);

  if (gui_mouse_grabbed (dpyinfo))
    f = dpyinfo->last_mouse_frame;
  else
    f = pgtk_any_window_to_frame (gtk_widget_get_window (widget));

  inev.ie.kind = NO_EVENT;
  inev.ie.timestamp = event->scroll.time;
  inev.ie.modifiers
    = pgtk_gtk_to_emacs_modifiers (FRAME_DISPLAY_INFO (f),
				   event->scroll.state);
  XSETINT (inev.ie.x, event->scroll.x);
  XSETINT (inev.ie.y, event->scroll.y);
  XSETFRAME (inev.ie.frame_or_window, f);
  inev.ie.arg = Qnil;

  if (gdk_event_is_scroll_stop_event (event))
    {
      inev.ie.kind = TOUCH_END_EVENT;
      inev.ie.device
	= pgtk_get_device_for_event (FRAME_DISPLAY_INFO (f), event);
      evq_enqueue (&inev);
      return TRUE;
    }

  if (gdk_event_get_scroll_direction (event, &dir))
    {
      switch (dir)
	{
	case GDK_SCROLL_UP:
	  inev.ie.kind = WHEEL_EVENT;
	  inev.ie.modifiers |= up_modifier;
	  break;
	case GDK_SCROLL_DOWN:
	  inev.ie.kind = WHEEL_EVENT;
	  inev.ie.modifiers |= down_modifier;
	  break;
	case GDK_SCROLL_LEFT:
	  inev.ie.kind = HORIZ_WHEEL_EVENT;
	  inev.ie.modifiers |= up_modifier;
	  break;
	case GDK_SCROLL_RIGHT:
	  inev.ie.kind = HORIZ_WHEEL_EVENT;
	  inev.ie.modifiers |= down_modifier;
	  break;
	case GDK_SCROLL_SMOOTH: /* shut up warning */
	  break;
	}
    }
  else if (gdk_event_get_scroll_deltas (event, &delta_x, &delta_y))
    {
      if (!mwheel_coalesce_scroll_events)
	{
	  inev.ie.kind
	    = ((fabs (delta_x) > fabs (delta_y)) ? HORIZ_WHEEL_EVENT
						 : WHEEL_EVENT);
	  inev.ie.modifiers
	    |= (inev.ie.kind == HORIZ_WHEEL_EVENT
		  ? (delta_x >= 0 ? up_modifier : down_modifier)
		  : (delta_y >= 0 ? down_modifier : up_modifier));
	  inev.ie.arg = list3 (Qnil, make_float (-delta_x * 100),
			       make_float (-delta_y * 100));
	}
      else
	{
	  dpyinfo->scroll.acc_x += delta_x;
	  dpyinfo->scroll.acc_y += delta_y;
	  if (dpyinfo->scroll.acc_y >= dpyinfo->scroll.y_per_line)
	    {
	      int nlines
		= dpyinfo->scroll.acc_y / dpyinfo->scroll.y_per_line;
	      inev.ie.kind = WHEEL_EVENT;
	      inev.ie.modifiers |= down_modifier;
	      inev.ie.arg
		= list3 (make_fixnum (nlines),
			 make_float (-dpyinfo->scroll.acc_x * 100),
			 make_float (-dpyinfo->scroll.acc_y * 100));
	      dpyinfo->scroll.acc_y
		-= dpyinfo->scroll.y_per_line * nlines;
	    }
	  else if (dpyinfo->scroll.acc_y
		   <= -dpyinfo->scroll.y_per_line)
	    {
	      int nlines
		= -dpyinfo->scroll.acc_y / dpyinfo->scroll.y_per_line;
	      inev.ie.kind = WHEEL_EVENT;
	      inev.ie.modifiers |= up_modifier;
	      inev.ie.arg
		= list3 (make_fixnum (nlines),
			 make_float (-dpyinfo->scroll.acc_x * 100),
			 make_float (-dpyinfo->scroll.acc_y * 100));

	      dpyinfo->scroll.acc_y
		-= -dpyinfo->scroll.y_per_line * nlines;
	    }
	  else if (dpyinfo->scroll.acc_x >= dpyinfo->scroll.x_per_char
		   || !mwheel_coalesce_scroll_events)
	    {
	      int nchars
		= dpyinfo->scroll.acc_x / dpyinfo->scroll.x_per_char;
	      inev.ie.kind = HORIZ_WHEEL_EVENT;
	      inev.ie.modifiers |= up_modifier;
	      inev.ie.arg
		= list3 (make_fixnum (nchars),
			 make_float (-dpyinfo->scroll.acc_x * 100),
			 make_float (-dpyinfo->scroll.acc_y * 100));

	      dpyinfo->scroll.acc_x
		-= dpyinfo->scroll.x_per_char * nchars;
	    }
	  else if (dpyinfo->scroll.acc_x
		   <= -dpyinfo->scroll.x_per_char)
	    {
	      int nchars
		= -dpyinfo->scroll.acc_x / dpyinfo->scroll.x_per_char;
	      inev.ie.kind = HORIZ_WHEEL_EVENT;
	      inev.ie.modifiers |= down_modifier;
	      inev.ie.arg
		= list3 (make_fixnum (nchars),
			 make_float (-dpyinfo->scroll.acc_x * 100),
			 make_float (-dpyinfo->scroll.acc_y * 100));

	      dpyinfo->scroll.acc_x
		-= -dpyinfo->scroll.x_per_char * nchars;
	    }
	}
    }

  if (inev.ie.kind != NO_EVENT)
    {
      inev.ie.device
	= pgtk_get_device_for_event (FRAME_DISPLAY_INFO (f), event);
      evq_enqueue (&inev);
    }
  return TRUE;
}

/* C part of drop handling code.
   The Lisp part is in pgtk-dnd.el.  */

static GdkDragAction
symbol_to_drag_action (Lisp_Object act)
{
  if (EQ (act, Qcopy))
    return GDK_ACTION_COPY;

  if (EQ (act, Qmove))
    return GDK_ACTION_MOVE;

  if (EQ (act, Qlink))
    return GDK_ACTION_LINK;

  if (EQ (act, Qprivate))
    return GDK_ACTION_PRIVATE;

  if (NILP (act))
    return GDK_ACTION_DEFAULT;

  signal_error ("Invalid drag action", act);
}

static Lisp_Object
drag_action_to_symbol (GdkDragAction action)
{
  switch (action)
    {
    case GDK_ACTION_COPY:
      return Qcopy;

    case GDK_ACTION_MOVE:
      return Qmove;

    case GDK_ACTION_LINK:
      return Qlink;

    case GDK_ACTION_PRIVATE:
      return Qprivate;

    case GDK_ACTION_DEFAULT:
    default:
      return Qnil;
    }
}

void
pgtk_update_drop_status (Lisp_Object action, Lisp_Object event_time)
{
  guint32 time;

  CONS_TO_INTEGER (event_time, guint32, time);

  if (!current_drop_context || time < current_drop_time)
    return;

  gdk_drag_status (current_drop_context,
		   symbol_to_drag_action (action), time);
}

void
pgtk_finish_drop (Lisp_Object success, Lisp_Object event_time,
		  Lisp_Object del)
{
  guint32 time;

  CONS_TO_INTEGER (event_time, guint32, time);

  if (!current_drop_context || time < current_drop_time)
    return;

  gtk_drag_finish (current_drop_context, !NILP (success), !NILP (del),
		   time);

  if (current_drop_context_drop)
    g_clear_pointer (&current_drop_context, g_object_unref);
}

static void
drag_leave (GtkWidget *widget, GdkDragContext *context, guint time,
	    gpointer user_data)
{
  struct frame *f;
  union buffered_input_event inev;

  f = pgtk_any_window_to_frame (gtk_widget_get_window (widget));

  if (current_drop_context)
    {
      if (current_drop_context_drop)
	gtk_drag_finish (current_drop_context, FALSE, FALSE,
			 current_drop_time);

      g_clear_pointer (&current_drop_context, g_object_unref);
    }

  EVENT_INIT (inev.ie);

  inev.ie.kind = DRAG_N_DROP_EVENT;
  inev.ie.modifiers = 0;
  inev.ie.arg = Qnil;
  inev.ie.timestamp = time;

  XSETINT (inev.ie.x, 0);
  XSETINT (inev.ie.y, 0);
  XSETFRAME (inev.ie.frame_or_window, f);

  evq_enqueue (&inev);
}

static gboolean
drag_motion (GtkWidget *widget, GdkDragContext *context, gint x,
	     gint y, guint time)

{
  struct frame *f;
  union buffered_input_event inev;
  GdkAtom name;
  GdkDragAction suggestion;

  f = pgtk_any_window_to_frame (gtk_widget_get_window (widget));

  if (!f)
    return FALSE;

  if (current_drop_context)
    {
      if (current_drop_context_drop)
	gtk_drag_finish (current_drop_context, FALSE, FALSE,
			 current_drop_time);

      g_clear_pointer (&current_drop_context, g_object_unref);
    }

  current_drop_context = g_object_ref (context);
  current_drop_time = time;
  current_drop_context_drop = false;

  name = gdk_drag_get_selection (context);
  suggestion = gdk_drag_context_get_suggested_action (context);

  EVENT_INIT (inev.ie);

  inev.ie.kind = DRAG_N_DROP_EVENT;
  inev.ie.modifiers = 0;
  inev.ie.arg
    = list4 (Qlambda, intern (gdk_atom_name (name)), make_uint (time),
	     drag_action_to_symbol (suggestion));
  inev.ie.timestamp = time;

  XSETINT (inev.ie.x, x);
  XSETINT (inev.ie.y, y);
  XSETFRAME (inev.ie.frame_or_window, f);

  evq_enqueue (&inev);

  return TRUE;
}

static gboolean
drag_drop (GtkWidget *widget, GdkDragContext *context, int x, int y,
	   guint time, gpointer user_data)
{
  struct frame *f;
  union buffered_input_event inev;
  GdkAtom name;
  GdkDragAction selected_action;

  f = pgtk_any_window_to_frame (gtk_widget_get_window (widget));

  if (!f)
    return FALSE;

  if (current_drop_context)
    {
      if (current_drop_context_drop)
	gtk_drag_finish (current_drop_context, FALSE, FALSE,
			 current_drop_time);

      g_clear_pointer (&current_drop_context, g_object_unref);
    }

  current_drop_context = g_object_ref (context);
  current_drop_time = time;
  current_drop_context_drop = true;

  name = gdk_drag_get_selection (context);
  selected_action = gdk_drag_context_get_selected_action (context);

  EVENT_INIT (inev.ie);

  inev.ie.kind = DRAG_N_DROP_EVENT;
  inev.ie.modifiers = 0;
  inev.ie.arg
    = list4 (Qquote, intern (gdk_atom_name (name)), make_uint (time),
	     drag_action_to_symbol (selected_action));
  inev.ie.timestamp = time;

  XSETINT (inev.ie.x, x);
  XSETINT (inev.ie.y, y);
  XSETFRAME (inev.ie.frame_or_window, f);

  evq_enqueue (&inev);

  return TRUE;
}

/* Touch screen events.  */

/* Record a touch sequence with the identifier DETAIL from the given
   FRAME on the specified DPYINFO.  Round X and Y and record them as
   its current position, assign an identifier to the touch sequence
   suitable for reporting to Lisp, and return the same.  */

static EMACS_INT
pgtk_link_touch_point (struct pgtk_display_info *dpyinfo,
		       GdkEventSequence *detail, gdouble x, gdouble y,
		       struct frame *frame)
{
  struct pgtk_touch_point *touchpoint;
  static EMACS_INT local_detail;

  /* Assign an identifier suitable for reporting to Lisp.  On builds
     with 64-bit Lisp_Object, this is largely a theoretical problem,
     but CARD32s easily overflow 32-bit systems, as they are not
     specific to X clients (e.g. Emacs) but grow uniformly across all
     of them.  */

  if (FIXNUM_OVERFLOW_P (local_detail))
    local_detail = 0;

  touchpoint = xmalloc (sizeof *touchpoint);
  touchpoint->next = dpyinfo->touchpoints;
  touchpoint->x = lrint (x);
  touchpoint->y = lrint (y);
  touchpoint->number = detail;
  touchpoint->local_detail = local_detail++;
  touchpoint->frame = frame;
  dpyinfo->touchpoints = touchpoint;
  return touchpoint->local_detail;
}

/* Free and remove the touch sequence with the identifier DETAIL.
   DPYINFO is the display in which the touch sequence should be
   recorded.  If such a touch sequence exists, return its local
   identifier in *LOCAL_DETAIL.

   Value is 0 if no touch sequence by that identifier exists inside
   DPYINFO, or 1 if a touch sequence has been found.  */

static int
pgtk_unlink_touch_point (GdkEventSequence *detail,
			 struct pgtk_display_info *dpyinfo,
			 EMACS_INT *local_detail)
{
  struct pgtk_touch_point *last, *tem;

  for (last = NULL, tem = dpyinfo->touchpoints; tem;
       last = tem, tem = tem->next)
    {
      if (tem->number == detail)
	{
	  if (!last)
	    dpyinfo->touchpoints = tem->next;
	  else
	    last->next = tem->next;

	  *local_detail = tem->local_detail;
	  xfree (tem);

	  return 1;
	}
    }

  return 0;
}

/* Unlink all touch points associated with the frame F.  This is done
   upon destroying F's window (or its being destroyed), because touch
   point delivery after that point is undefined.  */

static void
pgtk_unlink_touch_points (struct frame *f)
{
  struct pgtk_touch_point **next, *last;
  struct pgtk_display_info *dpyinfo;

  /* Now unlink all touch points on F's display matching F.  */

  dpyinfo = FRAME_DISPLAY_INFO (f);
  for (next = &dpyinfo->touchpoints; (last = *next);)
    {
      if (last->frame == f)
	{
	  *next = last->next;
	  xfree (last);
	}
      else
	next = &last->next;
    }
}

/* Return the data associated with a touch sequence DETAIL recorded by
   `pgtk_link_touch_point' from DPYINFO, or NULL if it can't be
   found.  */

static struct pgtk_touch_point *
pgtk_find_touch_point (struct pgtk_display_info *dpyinfo,
		       GdkEventSequence *detail)
{
  struct pgtk_touch_point *point;

  for (point = dpyinfo->touchpoints; point; point = point->next)
    {
      if (point->number == detail)
	return point;
    }

  return NULL;
}

static gboolean
touch_event_cb (GtkWidget *self, GdkEvent *event, gpointer user_data)
{
  struct pgtk_display_info *dpyinfo;
  struct frame *f;
  EMACS_INT local_detail;
  union buffered_input_event inev;
  struct pgtk_touch_point *touchpoint;
  Lisp_Object arg = Qnil;
  int state;

  EVENT_INIT (inev.ie);

  f = pgtk_any_window_to_frame (gtk_widget_get_window (self));
  eassert (f);
  dpyinfo = FRAME_DISPLAY_INFO (f);
  switch (event->type)
    {
    case GDK_TOUCH_BEGIN:

      /* Verify that no touch point with this identifier is already at
	 large.  */
      if (pgtk_find_touch_point (dpyinfo, event->touch.sequence))
	break;

      /* Record this in the display structure.  */
      local_detail
	= pgtk_link_touch_point (dpyinfo, event->touch.sequence,
				 event->touch.x, event->touch.y, f);
      /* Generate the input event.  */
      inev.ie.kind = TOUCHSCREEN_BEGIN_EVENT;
      inev.ie.timestamp = event->touch.time;
      XSETFRAME (inev.ie.frame_or_window, f);
      XSETINT (inev.ie.x, lrint (event->touch.x));
      XSETINT (inev.ie.y, lrint (event->touch.y));
      XSETINT (inev.ie.arg, local_detail);
      break;

    case GDK_TOUCH_UPDATE:
      touchpoint
	= pgtk_find_touch_point (dpyinfo, event->touch.sequence);

      if (!touchpoint
	  /* Don't send this event if nothing has changed
	     either.  */
	  || (touchpoint->x == lrint (event->touch.x)
	      && touchpoint->y == lrint (event->touch.y)))
	break;

      /* Construct the input event.  */
      touchpoint->x = lrint (event->touch.x);
      touchpoint->y = lrint (event->touch.y);
      inev.ie.kind = TOUCHSCREEN_UPDATE_EVENT;
      inev.ie.timestamp = event->touch.time;
      XSETFRAME (inev.ie.frame_or_window, f);

      for (touchpoint = dpyinfo->touchpoints; touchpoint;
	   touchpoint = touchpoint->next)
	{
	  if (touchpoint->frame == f)
	    arg = Fcons (list3i (touchpoint->x, touchpoint->y,
				 touchpoint->local_detail),
			 arg);
	}

      inev.ie.arg = arg;
      break;

    case GDK_TOUCH_END:
    case GDK_TOUCH_CANCEL:
      /* Remove this touch point's record, also establishing its
	 existence.  */
      state = pgtk_unlink_touch_point (event->touch.sequence, dpyinfo,
				       &local_detail);
      /* If it did exist... */
      if (state)
	{
	  /* ... generate a suitable event.  */
	  inev.ie.kind = TOUCHSCREEN_END_EVENT;
	  inev.ie.timestamp = event->touch.time;
	  inev.ie.modifiers = (event->type != GDK_TOUCH_END);

	  XSETFRAME (inev.ie.frame_or_window, f);
	  XSETINT (inev.ie.x, lrint (event->touch.x));
	  XSETINT (inev.ie.y, lrint (event->touch.y));
	  XSETINT (inev.ie.arg, local_detail);
	}
      break;

    default:
      break;
    }

  /* If the above produced a workable event, report the name of the
     device that gave rise to it.  */

  if (inev.ie.kind != NO_EVENT)
    {
      inev.ie.device = pgtk_get_device_for_event (dpyinfo, event);
      evq_enqueue (&inev);

      /* Next, save this event for future menu activations, unless it
	 is only an update.  */
      if (event->type != GDK_TOUCH_UPDATE)
	{
	  if (dpyinfo->last_click_event != NULL)
	    gdk_event_free (dpyinfo->last_click_event);
	  dpyinfo->last_click_event = gdk_event_copy (event);
	}
    }

  return inev.ie.kind != NO_EVENT;
}

/* Callbacks for sundries.  */

static void
pgtk_monitors_changed_cb (GdkScreen *screen, gpointer user_data)
{
  struct terminal *terminal;
  union buffered_input_event inev;

  EVENT_INIT (inev.ie);
  terminal = user_data;
  inev.ie.kind = MONITORS_CHANGED_EVENT;
  XSETTERMINAL (inev.ie.arg, terminal);

  evq_enqueue (&inev);
}

static gboolean pgtk_selection_event (GtkWidget *, GdkEvent *,
				      gpointer);

#ifdef USE_SKIA
/* Forward declarations for Skia GL drawing area callbacks.  */
static void pgtk_gl_drawing_area_realize (GtkWidget *, gpointer);
static void pgtk_gl_drawing_area_unrealize (GtkWidget *, gpointer);
static gboolean pgtk_gl_drawing_area_draw (GtkWidget *, cairo_t *,
					   gpointer);
static bool pgtk_setup_gl_framebuffer (struct frame *, int, int);
static bool pgtk_check_gl_error (const char *);
#endif

void
pgtk_set_event_handler (struct frame *f)
{
  if (f->tooltip)
    {
      g_signal_connect (G_OBJECT (FRAME_GTK_WIDGET (f)), "draw",
			G_CALLBACK (pgtk_handle_draw), NULL);
      return;
    }

  gtk_drag_dest_set (FRAME_GTK_WIDGET (f), 0, NULL, 0,
		     (GDK_ACTION_MOVE | GDK_ACTION_COPY
		      | GDK_ACTION_LINK | GDK_ACTION_PRIVATE));

  if (FRAME_GTK_OUTER_WIDGET (f))
    {
      g_signal_connect (G_OBJECT (FRAME_GTK_OUTER_WIDGET (f)),
			"window-state-event",
			G_CALLBACK (window_state_event), NULL);
      g_signal_connect (G_OBJECT (FRAME_GTK_OUTER_WIDGET (f)),
			"delete-event", G_CALLBACK (delete_event),
			NULL);
      g_signal_connect (G_OBJECT (FRAME_GTK_OUTER_WIDGET (f)),
			"event", G_CALLBACK (pgtk_handle_event),
			NULL);
      g_signal_connect (G_OBJECT (FRAME_GTK_OUTER_WIDGET (f)),
			"configure-event",
			G_CALLBACK (configure_event), NULL);
    }

  g_signal_connect (G_OBJECT (FRAME_GTK_WIDGET (f)), "map-event",
		    G_CALLBACK (map_event), NULL);
  g_signal_connect (G_OBJECT (FRAME_GTK_WIDGET (f)), "size-allocate",
		    G_CALLBACK (size_allocate), f);
  g_signal_connect (G_OBJECT (FRAME_GTK_WIDGET (f)),
		    "key-press-event", G_CALLBACK (key_press_event),
		    NULL);
  g_signal_connect (G_OBJECT (FRAME_GTK_WIDGET (f)),
		    "key-release-event",
		    G_CALLBACK (key_release_event), NULL);
  g_signal_connect (G_OBJECT (FRAME_GTK_WIDGET (f)), "focus-in-event",
		    G_CALLBACK (focus_in_event), NULL);
  g_signal_connect (G_OBJECT (FRAME_GTK_WIDGET (f)),
		    "focus-out-event", G_CALLBACK (focus_out_event),
		    NULL);
  g_signal_connect (G_OBJECT (FRAME_GTK_WIDGET (f)),
		    "enter-notify-event",
		    G_CALLBACK (enter_notify_event), NULL);
  g_signal_connect (G_OBJECT (FRAME_GTK_WIDGET (f)),
		    "leave-notify-event",
		    G_CALLBACK (leave_notify_event), NULL);
  g_signal_connect (G_OBJECT (FRAME_GTK_WIDGET (f)),
		    "motion-notify-event",
		    G_CALLBACK (motion_notify_event), NULL);
  g_signal_connect (G_OBJECT (FRAME_GTK_WIDGET (f)),
		    "button-press-event", G_CALLBACK (button_event),
		    NULL);
  g_signal_connect (G_OBJECT (FRAME_GTK_WIDGET (f)),
		    "button-release-event", G_CALLBACK (button_event),
		    NULL);
  g_signal_connect (G_OBJECT (FRAME_GTK_WIDGET (f)), "scroll-event",
		    G_CALLBACK (scroll_event), NULL);
  g_signal_connect (G_OBJECT (FRAME_GTK_WIDGET (f)),
		    "configure-event", G_CALLBACK (configure_event),
		    NULL);
  g_signal_connect (G_OBJECT (FRAME_GTK_WIDGET (f)), "drag-leave",
		    G_CALLBACK (drag_leave), NULL);
  g_signal_connect (G_OBJECT (FRAME_GTK_WIDGET (f)), "drag-motion",
		    G_CALLBACK (drag_motion), NULL);
  g_signal_connect (G_OBJECT (FRAME_GTK_WIDGET (f)), "drag-drop",
		    G_CALLBACK (drag_drop), NULL);

#ifdef USE_SKIA
  /* For Skia GL rendering, create a GtkDrawingArea and manage our own
     GdkGLContext.  We call gdk_cairo_draw_from_gl directly in the
     draw callback, handing it our FBO texture.  This eliminates the
     extra FBO copy that GtkGLArea's internal FBO would require:
     Skia renders into our single FBO, and gdk_cairo_draw_from_gl
     composites that texture directly to the window back buffer.

     gdk_cairo_draw_from_gl's texture compositing path (gdkgl.c:526-656)
     trashes GL state (shaders, VAOs, textures, blend).  This is safe
     because pgtk_begin_skia_clip calls emacs_skia_gl_context_reset
     (GrDirectContext::resetContext(kAll)) before every Skia draw.  */
  {
    GtkWidget *drawing_area = gtk_drawing_area_new ();
    FRAME_GL_DRAWING_AREA (f) = drawing_area;

    /* Connect signals.  The GL context is created in realize (once the
       GdkWindow exists), and the draw callback composites via
       gdk_cairo_draw_from_gl.  */
    g_signal_connect (G_OBJECT (drawing_area), "realize",
		      G_CALLBACK (pgtk_gl_drawing_area_realize), f);
    g_signal_connect (G_OBJECT (drawing_area), "unrealize",
		      G_CALLBACK (pgtk_gl_drawing_area_unrealize), f);
    g_signal_connect (G_OBJECT (drawing_area), "draw",
		      G_CALLBACK (pgtk_gl_drawing_area_draw), f);

    /* CRITICAL: Make drawing area non-focusable so that keyboard
       focus stays on FRAME_GTK_WIDGET where the key-press-event
       handler is connected.  */
    gtk_widget_set_can_focus (drawing_area, FALSE);

    /* Add to the GtkFixed at position (0,0).  */
    gtk_fixed_put (GTK_FIXED (FRAME_GTK_WIDGET (f)), drawing_area, 0, 0);

    /* Make it fill the entire frame.  Updated on resize.  */
    gtk_widget_set_size_request (drawing_area, 1, 1);
    gtk_widget_set_hexpand (drawing_area, TRUE);
    gtk_widget_set_vexpand (drawing_area, TRUE);
    gtk_widget_show (drawing_area);
  }
#else
  /* For Cairo, use the draw callback.  */
  g_signal_connect (G_OBJECT (FRAME_GTK_WIDGET (f)), "draw",
		    G_CALLBACK (pgtk_handle_draw), NULL);
  g_signal_connect (G_OBJECT (FRAME_GTK_WIDGET (f)),
		    "property-notify-event",
		    G_CALLBACK (pgtk_selection_event), NULL);
#endif

  g_signal_connect (G_OBJECT (FRAME_GTK_WIDGET (f)),
		    "selection-clear-event",
		    G_CALLBACK (pgtk_selection_event), NULL);
  g_signal_connect (G_OBJECT (FRAME_GTK_WIDGET (f)),
		    "selection-request-event",
		    G_CALLBACK (pgtk_selection_event), NULL);
  g_signal_connect (G_OBJECT (FRAME_GTK_WIDGET (f)),
		    "selection-notify-event",
		    G_CALLBACK (pgtk_selection_event), NULL);
  g_signal_connect (G_OBJECT (FRAME_GTK_WIDGET (f)), "touch-event",
		    G_CALLBACK (touch_event_cb), NULL);
  g_signal_connect (G_OBJECT (FRAME_GTK_WIDGET (f)), "event",
		    G_CALLBACK (pgtk_handle_event), NULL);
}

static void
my_log_handler (const gchar *log_domain, GLogLevelFlags log_level,
		const gchar *msg, gpointer user_data)
{
  if (!strstr (msg, "g_set_prgname"))
    fprintf (stderr, "%s-WARNING **: %s", log_domain, msg);
}

/* Test whether two display-name strings agree up to the dot that
   separates the screen number from the server number.  */
static bool
same_x_server (const char *name1, const char *name2)
{
  bool seen_colon = false;
  Lisp_Object sysname = Fsystem_name ();
  const char *system_name = SSDATA (sysname);
  ptrdiff_t system_name_length = SBYTES (sysname);
  ptrdiff_t length_until_period = 0;

  while (system_name[length_until_period] != 0
	 && system_name[length_until_period] != '.')
    length_until_period++;

  /* Treat `unix' like an empty host name.  */
  if (!strncmp (name1, "unix:", 5))
    name1 += 4;
  if (!strncmp (name2, "unix:", 5))
    name2 += 4;
  /* Treat this host's name like an empty host name.  */
  if (!strncmp (name1, system_name, system_name_length)
      && name1[system_name_length] == ':')
    name1 += system_name_length;
  if (!strncmp (name2, system_name, system_name_length)
      && name2[system_name_length] == ':')
    name2 += system_name_length;
  /* Treat this host's domainless name like an empty host name.  */
  if (!strncmp (name1, system_name, length_until_period)
      && name1[length_until_period] == ':')
    name1 += length_until_period;
  if (!strncmp (name2, system_name, length_until_period)
      && name2[length_until_period] == ':')
    name2 += length_until_period;

  for (; *name1 != '\0' && *name1 == *name2; name1++, name2++)
    {
      if (*name1 == ':')
	seen_colon = true;
      if (seen_colon && *name1 == '.')
	return true;
    }
  return (seen_colon && (*name1 == '.' || *name1 == '\0')
	  && (*name2 == '.' || *name2 == '\0'));
}

static struct frame *
pgtk_find_selection_owner (GdkWindow *window)
{
  Lisp_Object tail, tem;
  struct frame *f;

  FOR_EACH_FRAME (tail, tem)
  {
    f = XFRAME (tem);

    if (FRAME_PGTK_P (f) && (FRAME_GDK_WINDOW (f) == window))
      return f;
  }

  return NULL;
}

static gboolean
pgtk_selection_event (GtkWidget *widget, GdkEvent *event,
		      gpointer user_data)
{
  struct frame *f;
  union buffered_input_event inev;

  if (event->type == GDK_PROPERTY_NOTIFY)
    pgtk_handle_property_notify (&event->property);
  else if (event->type == GDK_SELECTION_CLEAR
	   || event->type == GDK_SELECTION_REQUEST)
    {
      f = pgtk_find_selection_owner (event->selection.window);

      if (f)
	{
	  EVENT_INIT (inev.ie);

	  inev.sie.kind = (event->type == GDK_SELECTION_CLEAR
			     ? SELECTION_CLEAR_EVENT
			     : SELECTION_REQUEST_EVENT);

	  SELECTION_EVENT_DPYINFO (&inev.sie)
	    = FRAME_DISPLAY_INFO (f);
	  SELECTION_EVENT_SELECTION (&inev.sie)
	    = event->selection.selection;
	  SELECTION_EVENT_TIME (&inev.sie) = event->selection.time;

	  if (event->type == GDK_SELECTION_REQUEST)
	    {
	      /* FIXME: when does GDK destroy the requestor GdkWindow
		 object?

		 It would make sense to wait for the transfer to
		 complete.  But I don't know if GDK actually does
		 that.  */
	      SELECTION_EVENT_REQUESTOR (&inev.sie)
		= event->selection.requestor;
	      SELECTION_EVENT_TARGET (&inev.sie)
		= event->selection.target;
	      SELECTION_EVENT_PROPERTY (&inev.sie)
		= event->selection.property;
	    }

	  evq_enqueue (&inev);
	  return TRUE;
	}
    }
  else if (event->type == GDK_SELECTION_NOTIFY)
    pgtk_handle_selection_notify (&event->selection);

  return FALSE;
}

/* Display a warning message if the PGTK port is being used under X;
   that is not supported.  */

static void
pgtk_display_x_warning (GdkDisplay *display)
{
  GtkWidget *dialog_widget, *label, *content_area;
  GtkDialog *dialog;
  GtkWindow *window;
  GdkScreen *screen;

  /* Do this instead of GDK_IS_X11_DISPLAY because the GDK X header
     pulls in Xlib, which conflicts with definitions in pgtkgui.h.  */
  if (strcmp (G_OBJECT_TYPE_NAME (display), "GdkX11Display"))
    return;

  dialog_widget = gtk_dialog_new ();
  dialog = GTK_DIALOG (dialog_widget);
  window = GTK_WINDOW (dialog_widget);
  screen = gdk_display_get_default_screen (display);
  content_area = gtk_dialog_get_content_area (dialog);

  gtk_window_set_title (window, "Warning");
  gtk_window_set_screen (window, screen);

  label = gtk_label_new (
    "You are trying to run Emacs configured with\n"
    " the \"pure-GTK\" interface under the X Window\n"
    " System.  That configuration is unsupported and\n"
    " will lead to sporadic crashes during transfer of\n"
    " large selection data.  It will also lead to\n"
    " various problems with keyboard input.\n");
  gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
  gtk_container_add (GTK_CONTAINER (content_area), label);
  gtk_widget_show (label);
  gtk_widget_show (dialog_widget);
}

/* Open a connection to X display DISPLAY_NAME, and return
   the structure that describes the open display.
   If we cannot contact the display, return null.  */

struct pgtk_display_info *
pgtk_term_init (Lisp_Object display_name, char *resource_name)
{
  GdkDisplay *dpy;
  struct terminal *terminal;
  struct pgtk_display_info *dpyinfo;
  static int x_initialized = 0;
  static unsigned x_display_id = 0;
  static char *initial_display = NULL;
  char *dpy_name;
  static void *handle = NULL;
  Lisp_Object lisp_dpy_name = Qnil;
  GdkScreen *gscr;
  gdouble dpi;

  block_input ();

  if (!x_initialized)
    {
      any_help_event_p = false;

      Fset_input_interrupt_mode (Qt);
      baud_rate = 19200;

#ifdef USE_CAIRO
      gui_init_fringe (&pgtk_redisplay_interface);
#endif

      ++x_initialized;
    }

  dpy_name = SSDATA (display_name);
  if (strlen (dpy_name) == 0 && initial_display != NULL)
    dpy_name = initial_display;
  lisp_dpy_name = build_string (dpy_name);

  {
#define NUM_ARGV 10
    int argc;
    char *argv[NUM_ARGV];
    char **argv2 = argv;
    guint id;

    if (x_initialized++ > 1)
      {
	xg_display_open (dpy_name, &dpy);
      }
    else
      {
	static char display_opt[] = "--display";
	static char name_opt[] = "--name";

	for (argc = 0; argc < NUM_ARGV; ++argc)
	  argv[argc] = 0;

	argc = 0;
	argv[argc++] = initial_argv[0];

	if (strlen (dpy_name) != 0)
	  {
	    argv[argc++] = display_opt;
	    argv[argc++] = dpy_name;
	  }

	argv[argc++] = name_opt;
	argv[argc++] = resource_name;

	/* Work around GLib bug that outputs a faulty warning. See
	   https://bugzilla.gnome.org/show_bug.cgi?id=563627.  */
	id = g_log_set_handler ("GLib",
				G_LOG_LEVEL_WARNING | G_LOG_FLAG_FATAL
				  | G_LOG_FLAG_RECURSION,
				my_log_handler, NULL);

	/* gtk_init does set_locale.  Fix locale before and after.  */
	fixup_locale ();
	unrequest_sigio (); /* See comment in x_display_ok.  */
	gtk_init (&argc, &argv2);
	request_sigio ();
	fixup_locale ();

	g_log_remove_handler ("GLib", id);

	xg_initialize ();

	dpy = DEFAULT_GDK_DISPLAY ();

	initial_display = g_strdup (gdk_display_get_name (dpy));
	dpy_name = initial_display;
	lisp_dpy_name = build_string (dpy_name);
      }
  }

  /* Detect failure.  */
  if (dpy == 0)
    {
      unblock_input ();
      return 0;
    }

  /* If the PGTK port is being used under X, complain very loudly, as
     that isn't supported.  */
  pgtk_display_x_warning (dpy);

  dpyinfo = xzalloc (sizeof *dpyinfo);
  pgtk_initialize_display_info (dpyinfo);
  terminal = pgtk_create_terminal (dpyinfo);

  {
    struct pgtk_display_info *share;

    for (share = x_display_list; share; share = share->next)
      if (same_x_server (SSDATA (XCAR (share->name_list_element)),
			 dpy_name))
	break;
    if (share)
      terminal->kboard = share->terminal->kboard;
    else
      {
	terminal->kboard = allocate_kboard (Qpgtk);

	/* Don't let the initial kboard remain current longer than
	   necessary. That would cause problems if a file loaded on
	   startup tries to prompt in the mini-buffer.  */
	if (current_kboard == initial_kboard)
	  current_kboard = terminal->kboard;
      }
    terminal->kboard->reference_count++;
  }

  /* Put this display on the chain.  */
  dpyinfo->next = x_display_list;
  x_display_list = dpyinfo;

  dpyinfo->name_list_element = Fcons (lisp_dpy_name, Qnil);
  dpyinfo->gdpy = dpy;

  /* https://lists.gnu.org/r/emacs-devel/2015-11/msg00194.html  */
  dpyinfo->smallest_font_height = 1;
  dpyinfo->smallest_char_width = 1;

  /* Set the name of the terminal. */
  terminal->name = xlispstrdup (lisp_dpy_name);

  Lisp_Object system_name = Fsystem_name ();
  ptrdiff_t nbytes;
  if (ckd_add (&nbytes, SBYTES (Vinvocation_name),
	       SBYTES (system_name) + 2))
    memory_full (SIZE_MAX);
  dpyinfo->x_id = ++x_display_id;
  dpyinfo->x_id_name = xmalloc (nbytes);
  char *nametail = lispstpcpy (dpyinfo->x_id_name, Vinvocation_name);
  *nametail++ = '@';
  lispstpcpy (nametail, system_name);

  /* Get the scroll bar cursor.  */
  /* We must create a GTK cursor, it is required for GTK widgets.  */
  dpyinfo->xg_cursor = xg_create_default_cursor (dpyinfo->gdpy);

  dpyinfo->vertical_scroll_bar_cursor
    = gdk_cursor_new_for_display (dpyinfo->gdpy,
				  GDK_SB_V_DOUBLE_ARROW);

  dpyinfo->horizontal_scroll_bar_cursor
    = gdk_cursor_new_for_display (dpyinfo->gdpy,
				  GDK_SB_H_DOUBLE_ARROW);

  dpyinfo->icon_bitmap_id = -1;

  reset_mouse_highlight (&dpyinfo->mouse_highlight);

  gscr = gdk_display_get_default_screen (dpyinfo->gdpy);
  dpi = gdk_screen_get_resolution (gscr);

  if (dpi < 0)
    dpi = 96.0;

  dpyinfo->resx = dpi;
  dpyinfo->resy = dpi;

  g_signal_connect (G_OBJECT (gscr), "monitors-changed",
		    G_CALLBACK (pgtk_monitors_changed_cb), terminal);

  /* Set up scrolling increments.  */
  dpyinfo->scroll.x_per_char = 1;
  dpyinfo->scroll.y_per_line = 1;

  dpyinfo->connection = -1;

  if (!handle)
    handle = dlopen (NULL, RTLD_LAZY);

#ifdef GDK_WINDOWING_X11
  if (!strcmp (G_OBJECT_TYPE_NAME (dpy), "GdkX11Display") && handle)
    {
      void *(*gdk_x11_display_get_xdisplay) (GdkDisplay *)
	= dlsym (handle, "gdk_x11_display_get_xdisplay");
      int (*x_connection_number) (void *)
	= dlsym (handle, "XConnectionNumber");

      if (x_connection_number && gdk_x11_display_get_xdisplay)
	dpyinfo->connection
	  = x_connection_number (gdk_x11_display_get_xdisplay (dpy));
    }
#endif

#ifdef GDK_WINDOWING_WAYLAND
  if (GDK_IS_WAYLAND_DISPLAY (dpy) && handle)
    {
      struct wl_display *wl_dpy
	= gdk_wayland_display_get_wl_display (dpy);
      int (*display_get_fd) (struct wl_display *)
	= dlsym (handle, "wl_display_get_fd");

      if (display_get_fd)
	dpyinfo->connection = display_get_fd (wl_dpy);
    }
#endif

  if (dpyinfo->connection >= 0)
    {
      add_keyboard_wait_descriptor (dpyinfo->connection);
#ifdef F_SETOWN
      fcntl (dpyinfo->connection, F_SETOWN, getpid ());
#endif /* ! defined (F_SETOWN) */

      if (interrupt_input)
	init_sigio (dpyinfo->connection);
    }

  dpyinfo->invisible_cursor
    = gdk_cursor_new_for_display (dpyinfo->gdpy, GDK_BLANK_CURSOR);

  xsettings_initialize (dpyinfo);

  pgtk_im_init (dpyinfo);

  g_signal_connect (G_OBJECT (dpyinfo->gdpy), "seat-added",
		    G_CALLBACK (pgtk_seat_added_cb), dpyinfo);
  g_signal_connect (G_OBJECT (dpyinfo->gdpy), "seat-removed",
		    G_CALLBACK (pgtk_seat_removed_cb), dpyinfo);
  pgtk_enumerate_devices (dpyinfo, true);

  unblock_input ();

  return dpyinfo;
}

/* Get rid of display DPYINFO, deleting all frames on it,
   and without sending any more commands to the X server.  */

static void
pgtk_delete_display (struct pgtk_display_info *dpyinfo)
{
  struct terminal *t;
  struct pgtk_touch_point *last, *tem;

  /* Close all frames and delete the generic struct terminal for this
     X display.  */
  for (t = terminal_list; t; t = t->next_terminal)
    if (t->type == output_pgtk && t->display_info.pgtk == dpyinfo)
      {
	delete_terminal (t);
	break;
      }

  if (x_display_list == dpyinfo)
    x_display_list = dpyinfo->next;
  else
    {
      struct pgtk_display_info *tail;

      for (tail = x_display_list; tail; tail = tail->next)
	if (tail->next == dpyinfo)
	  tail->next = tail->next->next;
    }

  /* Free remaining touchpoints.  */
  tem = dpyinfo->touchpoints;
  while (tem)
    {
      last = tem;
      tem = tem->next;
      xfree (last);
    }

  pgtk_free_devices (dpyinfo);
  xfree (dpyinfo);
}

char *
pgtk_xlfd_to_fontname (const char *xlfd)
/* --------------------------------------------------------------------------
    Convert an X font name (XLFD) to an Gtk font name.
    Only family is used.
    The string returned is temporarily allocated.
   --------------------------------------------------------------------------
 */
{
  char *name = xmalloc (180);

  if (!strncmp (xlfd, "--", 2))
    {
      if (sscanf (xlfd, "--%179[^-]-", name) != 1)
	name[0] = '\0';
    }
  else
    {
      if (sscanf (xlfd, "-%*[^-]-%179[^-]-", name) != 1)
	name[0] = '\0';
    }

  /* stopgap for malformed XLFD input */
  if (strlen (name) == 0)
    strcpy (name, "Monospace");

  return name;
}

bool
pgtk_defined_color (struct frame *f, const char *name,
		    Emacs_Color *color_def, bool alloc,
		    bool makeIndex)
/* --------------------------------------------------------------------------
	 Return true if named color found, and set color_def rgb
   accordingly. If makeIndex and alloc are nonzero put the color in
   the color_table, and set color_def pixel to the resulting index. If
   makeIndex is zero, set color_def pixel to ARGB. Return false if not
   found
   --------------------------------------------------------------------------
 */
{
  int r;

  block_input ();
  r = xg_check_special_colors (f, name, color_def);
  if (!r)
    r = pgtk_parse_color (f, name, color_def);
  unblock_input ();
  return r;
}

/* On frame F, translate the color name to RGB values.  Use cached
   information, if possible.

   Note that there is currently no way to clean old entries out of the
   cache.  However, it is limited to names in the server's database,
   and names we've actually looked up; list-colors-display is probably
   the most color-intensive case we're likely to hit.  */

int
pgtk_parse_color (struct frame *f, const char *color_name,
		  Emacs_Color *color)
{
  GdkRGBA rgba;
  if (gdk_rgba_parse (&rgba, color_name))
    {
      color->red = rgba.red * 65535;
      color->green = rgba.green * 65535;
      color->blue = rgba.blue * 65535;
      color->pixel
	= ((color->red >> 8) << 16 | (color->green >> 8) << 8
	   | (color->blue >> 8) << 0);
      return 1;
    }
  return 0;
}

/* On frame F, translate pixel colors to RGB values for the NCOLORS
   colors in COLORS.  On W32, we no longer try to map colors to
   a palette.  */
void
pgtk_query_colors (struct frame *f, Emacs_Color *colors, int ncolors)
{
  int i;

  for (i = 0; i < ncolors; i++)
    {
      unsigned long pixel = colors[i].pixel;
      /* Convert to a 16 bit value in range 0 - 0xffff. */
#define GetRValue(p) (((p) >> 16) & 0xff)
#define GetGValue(p) (((p) >> 8) & 0xff)
#define GetBValue(p) (((p) >> 0) & 0xff)
      colors[i].red = GetRValue (pixel) * 257;
      colors[i].green = GetGValue (pixel) * 257;
      colors[i].blue = GetBValue (pixel) * 257;
    }
}

void
pgtk_query_color (struct frame *f, Emacs_Color *color)
{
  pgtk_query_colors (f, color, 1);
}

void
pgtk_clear_area (struct frame *f, int x, int y, int width, int height)
{
  eassert (width > 0 && height > 0);

#ifdef USE_SKIA
  pgtk_skia_fill_rectangle (f, FRAME_X_OUTPUT (f)->background_color,
			    x, y, width, height, true);
#else
  cairo_t *cr;

  cr = pgtk_begin_cr_clip (f);
  pgtk_set_cr_source_with_color (f,
				 FRAME_X_OUTPUT (f)->background_color,
				 true);
  cairo_rectangle (cr, x, y, width, height);
  cairo_fill (cr);
  pgtk_end_cr_clip (f);
#endif
}

#ifdef USE_SKIA
/* ============================================================
   Skia drawing functions
   ============================================================ */

/* Check for GL errors and clear them.  Returns true if an error
 * occurred.  */
static bool
pgtk_check_gl_error (const char *context)
{
  GLenum err = glGetError ();
  if (err != GL_NO_ERROR)
    {
      const char *err_str;
      switch (err)
	{
	case GL_INVALID_ENUM:
	  err_str = "GL_INVALID_ENUM";
	  break;
	case GL_INVALID_VALUE:
	  err_str = "GL_INVALID_VALUE";
	  break;
	case GL_INVALID_OPERATION:
	  err_str = "GL_INVALID_OPERATION";
	  break;
	case GL_INVALID_FRAMEBUFFER_OPERATION:
	  err_str = "GL_INVALID_FRAMEBUFFER_OPERATION";
	  break;
	case GL_OUT_OF_MEMORY:
	  err_str = "GL_OUT_OF_MEMORY";
	  break;
	default:
	  err_str = "UNKNOWN";
	  break;
	}
      fprintf (stderr, "Skia GL error in %s: %s (0x%x)\n", context,
	       err_str, err);
      return true;
    }
  return false;
}

/* Drawing area "unrealize" callback - clean up GL resources when
   the widget's GdkWindow is about to be destroyed.  */
static void
pgtk_gl_drawing_area_unrealize (GtkWidget *widget, gpointer user_data)
{
  struct frame *f = (struct frame *) user_data;

  if (!FRAME_GDK_GL_CONTEXT (f))
    return;

  gdk_gl_context_make_current (FRAME_GDK_GL_CONTEXT (f));

  /* Flush all pending GPU operations before destroying resources,
     but only if the GL context is still valid.  During compositor
     shutdown the EGL context may already be lost.  */
  if (pgtk_gl_context_valid_p (f))
    {
      if (FRAME_SKIA_SURFACE (f))
	emacs_skia_surface_flush (FRAME_SKIA_SURFACE (f));
      if (FRAME_SKIA_GL_CONTEXT (f))
	emacs_skia_gl_context_flush (FRAME_SKIA_GL_CONTEXT (f));
    }

  /* Destroy pre-bell snapshot (holds GPU texture reference).  */
  if (FRAME_X_OUTPUT (f)->skia_image_pre_bell)
    {
      emacs_skia_image_destroy (FRAME_X_OUTPUT (f)->skia_image_pre_bell);
      FRAME_X_OUTPUT (f)->skia_image_pre_bell = NULL;
    }

  /* Destroy Skia surface first (it references the GL context).  */
  if (FRAME_SKIA_SURFACE (f))
    {
      emacs_skia_surface_destroy (FRAME_SKIA_SURFACE (f));
      FRAME_SKIA_SURFACE (f) = NULL;
      FRAME_SKIA_CANVAS (f) = NULL;
    }

  /* Destroy paint object.  */
  if (FRAME_SKIA_PAINT (f))
    {
      emacs_skia_paint_destroy (FRAME_SKIA_PAINT (f));
      FRAME_SKIA_PAINT (f) = NULL;
    }

  /* Destroy the Skia GL context.  */
  if (FRAME_SKIA_GL_CONTEXT (f))
    {
      emacs_skia_gl_context_destroy (FRAME_SKIA_GL_CONTEXT (f));
      FRAME_SKIA_GL_CONTEXT (f) = NULL;
    }

  /* Delete GL resources we own.  */
  if (FRAME_GL_FRAMEBUFFER (f))
    {
      glDeleteFramebuffers (1, &FRAME_GL_FRAMEBUFFER (f));
      FRAME_GL_FRAMEBUFFER (f) = 0;
    }
  if (FRAME_GL_TEXTURE (f))
    {
      glDeleteTextures (1, &FRAME_GL_TEXTURE (f));
      FRAME_GL_TEXTURE (f) = 0;
    }
  if (FRAME_GL_STENCIL (f))
    {
      glDeleteRenderbuffers (1, &FRAME_GL_STENCIL (f));
      FRAME_GL_STENCIL (f) = 0;
    }

  /* We own this context (created via gdk_window_create_gl_context),
     so unref it.  */
  gdk_gl_context_clear_current ();
  g_object_unref (FRAME_GDK_GL_CONTEXT (f));
  FRAME_GDK_GL_CONTEXT (f) = NULL;

  FRAME_SKIA_GL_INITIALIZED (f) = false;
  FRAME_SKIA_GL_STATE_DIRTY (f) = true;
  FRAME_GL_SURFACE_CREATION_FAILURES (f) = 0;

  /* Mark frame as garbaged so it gets redrawn when realized again. */
  SET_FRAME_GARBAGED (f);
}

/* Drawing area "realize" callback - create GL context and set up
   resources.  At this point the GdkWindow exists, so we can create
   a GdkGLContext via gdk_window_create_gl_context (which shares with
   the window's paint context, required for gdk_cairo_draw_from_gl).  */
static void
pgtk_gl_drawing_area_realize (GtkWidget *widget, gpointer user_data)
{
  struct frame *f = (struct frame *) user_data;
  GdkWindow *gdk_window;
  GdkGLContext *gl_context;
  GError *error = NULL;

  if (FRAME_GDK_GL_CONTEXT (f))
    return; /* Already initialized.  */

  gdk_window = gtk_widget_get_window (widget);
  if (!gdk_window)
    return;

  /* Create a GL context from the GdkWindow.  This context shares
     objects with the window's internal paint context, so our textures
     are accessible to gdk_cairo_draw_from_gl.  */
  gl_context = gdk_window_create_gl_context (gdk_window, &error);
  if (!gl_context)
    {
      if (error)
	{
	  fprintf (stderr, "Skia: Failed to create GL context: %s\n",
		   error->message);
	  g_error_free (error);
	}
      return;
    }

  /* Request OpenGL 3.2 for Skia compatibility.  */
  gdk_gl_context_set_required_version (gl_context, 3, 2);

  if (!gdk_gl_context_realize (gl_context, &error))
    {
      if (error)
	{
	  fprintf (stderr, "Skia: Failed to realize GL context: %s\n",
		   error->message);
	  g_error_free (error);
	}
      g_object_unref (gl_context);
      return;
    }

  FRAME_GDK_GL_CONTEXT (f) = gl_context;
  gdk_gl_context_make_current (gl_context);

  /* Clear any pending GL errors.  */
  while (glGetError () != GL_NO_ERROR)
    ;

  /* Log GL version and renderer for debugging compositing issues.  */
  fprintf (stderr, "Skia: GL renderer: %s\n",
	   (const char *) glGetString (GL_RENDERER));
  fprintf (stderr, "Skia: GL version: %s\n",
	   (const char *) glGetString (GL_VERSION));

  /* Create Skia GL context using the native GL interface.  */
  FRAME_SKIA_GL_CONTEXT (f) = emacs_skia_gl_context_create_native ();

  if (!FRAME_SKIA_GL_CONTEXT (f))
    {
      gdk_gl_context_clear_current ();
      g_object_unref (gl_context);
      FRAME_GDK_GL_CONTEXT (f) = NULL;
      return;
    }

  /* Create GL framebuffer and texture for offscreen rendering.  */
  glGenFramebuffers (1, &FRAME_GL_FRAMEBUFFER (f));
  glGenTextures (1, &FRAME_GL_TEXTURE (f));

  /* Initialize GL state as dirty so Skia resets on first use.  */
  FRAME_SKIA_GL_STATE_DIRTY (f) = true;
  FRAME_GL_SURFACE_CREATION_FAILURES (f) = 0;

  FRAME_SKIA_GL_INITIALIZED (f) = true;

  /* Get the initial size and set up the FBO.  On HiDPI displays, the
     FBO and Skia surface are created at device-pixel resolution
     (logical pixels * scale factor) for crisp rendering.  The Skia
     canvas is then scaled so that Emacs's drawing code, which works
     in logical pixel coordinates, maps correctly to the larger
     device-pixel surface.  FRAME_SKIA_SURFACE_DESIRED_WIDTH/HEIGHT
     store logical pixel dimensions; the scale-up happens at FBO
     creation boundaries.  */
  GtkAllocation alloc;
  gtk_widget_get_allocation (widget, &alloc);
  if (alloc.width > 0 && alloc.height > 0)
    {
      int scale = gtk_widget_get_scale_factor (widget);
      int dev_width = alloc.width * scale;
      int dev_height = alloc.height * scale;
      pgtk_setup_gl_framebuffer (f, dev_width, dev_height);
      FRAME_SKIA_SURFACE_DESIRED_WIDTH (f) = alloc.width;
      FRAME_SKIA_SURFACE_DESIRED_HEIGHT (f) = alloc.height;

      /* Eagerly create the Skia surface and clear it to the frame's
	 background color.  This prevents the first draw callback from
	 compositing garbage/transparent content before Emacs redisplay
	 has drawn anything.  */
      if (FRAME_GL_FRAMEBUFFER (f) && FRAME_SKIA_GL_CONTEXT (f))
	{
	  FRAME_SKIA_SURFACE (f) = emacs_skia_surface_create_gl (
	    FRAME_SKIA_GL_CONTEXT (f), dev_width, dev_height,
	    FRAME_GL_FRAMEBUFFER (f), GL_RGBA8);
	  if (FRAME_SKIA_SURFACE (f))
	    {
	      emacs_skia_canvas_t *canvas
		= emacs_skia_surface_get_canvas (FRAME_SKIA_SURFACE (f));
	      if (canvas)
		{
		  FRAME_SKIA_CANVAS (f) = canvas;
		  /* Set HiDPI scale so logical-pixel drawing maps to
		     the device-pixel surface.  */
		  if (scale > 1)
		    emacs_skia_canvas_scale (canvas, (float) scale,
					    (float) scale);
		  unsigned long bg
		    = FRAME_X_OUTPUT (f)->background_color;
		  Emacs_Color col;
		  col.pixel = bg;
		  pgtk_query_color (f, &col);
		  uint8_t r = col.red >> 8;
		  uint8_t g = col.green >> 8;
		  uint8_t b = col.blue >> 8;
		  emacs_skia_canvas_clear (
		    canvas, EMACS_SKIA_COLOR (255, r, g, b));
		  emacs_skia_surface_flush (FRAME_SKIA_SURFACE (f));
		  emacs_skia_gl_context_flush (
		    FRAME_SKIA_GL_CONTEXT (f));
		  glFinish ();
		  emacs_skia_gl_context_reset (
		    FRAME_SKIA_GL_CONTEXT (f));
		}
	    }
	}
    }
}

/* Drawing area "draw" callback — composite our FBO texture to the
   window via gdk_cairo_draw_from_gl.  This eliminates the extra FBO
   copy that GtkGLArea's internal FBO required: we hand our texture
   directly to GDK for compositing into the window's back buffer.

   gdk_cairo_draw_from_gl handles cross-context fence synchronization
   internally (glFenceSync/glWaitSync between our context and the
   paint context), so no manual fence is needed here.  */
static gboolean
pgtk_gl_drawing_area_draw (GtkWidget *widget, cairo_t *cr,
			   gpointer user_data)
{
  struct frame *f = (struct frame *) user_data;
  GdkWindow *gdk_window;
  emacs_skia_surface_t *skia_surface;
  int width, height, scale;

  if (!FRAME_GDK_GL_CONTEXT (f) || !FRAME_GL_TEXTURE (f))
    return FALSE;

  gdk_window = gtk_widget_get_window (widget);
  if (!gdk_window)
    return FALSE;

  skia_surface = FRAME_SKIA_SURFACE (f);

  if (!skia_surface)
    {
      /* Surface is NULL — during resize or before first redisplay.
	 If the GL texture still has content from a previous frame,
	 composite it rather than showing a black box.  The stale
	 content is a much better placeholder than black until
	 redisplay creates a new surface.  */
      if (FRAME_GL_TEXTURE (f) && FRAME_GDK_GL_CONTEXT (f))
	{
	  gdk_gl_context_make_current (FRAME_GDK_GL_CONTEXT (f));

	  if (pgtk_gl_context_valid_p (f))
	    {
	      GLint tex_w = 0, tex_h = 0;
	      if (FRAME_SKIA_GL_CONTEXT (f))
		emacs_skia_gl_context_flush (
		  FRAME_SKIA_GL_CONTEXT (f));
	      glBindTexture (GL_TEXTURE_2D,
			     FRAME_GL_TEXTURE (f));
	      glGetTexLevelParameteriv (
		GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tex_w);
	      glGetTexLevelParameteriv (
		GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &tex_h);
	      glBindTexture (GL_TEXTURE_2D, 0);

	      if (tex_w > 0 && tex_h > 0)
		{
		  scale = gtk_widget_get_scale_factor (widget);
		  gdk_cairo_draw_from_gl (
		    cr, gdk_window, FRAME_GL_TEXTURE (f),
		    GL_TEXTURE, scale, 0, 0, tex_w, tex_h);
		  FRAME_SKIA_GL_STATE_DIRTY (f) = true;
		}
	    }
	}
      SET_FRAME_GARBAGED (f);
      return FALSE;
    }

  /* Make our context current and flush Skia rendering.
     Validate the GL context to avoid crashing if it was lost
     (e.g. during compositor shutdown).  */
  gdk_gl_context_make_current (FRAME_GDK_GL_CONTEXT (f));

  if (!pgtk_gl_context_valid_p (f))
    {
      SET_FRAME_GARBAGED (f);
      return FALSE;
    }

  emacs_skia_surface_flush (skia_surface);
  if (FRAME_SKIA_GL_CONTEXT (f))
    emacs_skia_gl_context_flush (FRAME_SKIA_GL_CONTEXT (f));

  width = emacs_skia_surface_get_width (skia_surface);
  height = emacs_skia_surface_get_height (skia_surface);
  scale = gtk_widget_get_scale_factor (widget);

  /* One-time diagnostic.  */
  {
    static bool logged_once = false;
    if (!logged_once)
      {
	fprintf (stderr,
		 "Skia draw: texture=%u, size=%dx%d, scale=%d\n",
		 FRAME_GL_TEXTURE (f), width, height, scale);
	logged_once = true;
      }
  }

  /* Hand our FBO texture directly to GDK for compositing.  GDK
     switches to the window's paint context, inserts a fence sync to
     wait for our rendering, then draws a textured quad onto the
     window's back buffer.  For textures with alpha, it also uploads
     the underlying cairo surface for correct blending.

     After this call, our GL context is NO LONGER current — GDK
     switched to the paint context.  */
  gdk_cairo_draw_from_gl (cr, gdk_window,
			  FRAME_GL_TEXTURE (f),
			  GL_TEXTURE,
			  scale, 0, 0, width, height);

  /* Mark GL state as dirty.  gdk_cairo_draw_from_gl trashes GL state
     (shaders, VAOs, textures, blend) in the paint context, and our
     context is no longer current.  The next Skia draw in
     pgtk_begin_skia_clip calls emacs_skia_gl_context_reset to
     recover.  */
  FRAME_SKIA_GL_STATE_DIRTY (f) = true;

  return TRUE;
}

/* Resize FBO while preserving existing content.  This blits the old
   content to a temp buffer, resizes the main FBO, clears to
   background, and blits the preserved content back.  */
static void
pgtk_resize_fbo_preserve_content (struct frame *f, int old_width,
				  int old_height, int new_width,
				  int new_height)
{
  if (old_width <= 0 || old_height <= 0)
    {
      if (pgtk_setup_gl_framebuffer (f, new_width, new_height))
	{
	  glBindFramebuffer (GL_FRAMEBUFFER, FRAME_GL_FRAMEBUFFER (f));
	  unsigned long bg = FRAME_X_OUTPUT (f)->background_color;
	  float r = RED_FROM_ULONG (bg) / 255.0f;
	  float g = GREEN_FROM_ULONG (bg) / 255.0f;
	  float b = BLUE_FROM_ULONG (bg) / 255.0f;
	  glClearColor (r, g, b, 1.0f);
	  glClear (GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	  glBindFramebuffer (GL_FRAMEBUFFER, 0);
	}
      return;
    }

  /* Create a temporary FBO to hold the old content.  */
  GLuint temp_fbo, temp_texture;
  glGenFramebuffers (1, &temp_fbo);
  glGenTextures (1, &temp_texture);

  /* Copy old texture to temp texture.  */
  glBindTexture (GL_TEXTURE_2D, temp_texture);
  glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, old_width, old_height, 0,
		GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glBindFramebuffer (GL_FRAMEBUFFER, temp_fbo);
  glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			  GL_TEXTURE_2D, temp_texture, 0);

  /* Blit old content to temp.  */
  glBindFramebuffer (GL_READ_FRAMEBUFFER, FRAME_GL_FRAMEBUFFER (f));
  glBindFramebuffer (GL_DRAW_FRAMEBUFFER, temp_fbo);
  glBlitFramebuffer (0, 0, old_width, old_height, 0, 0, old_width,
		     old_height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

  /* Resize main FBO to new size.  */
  pgtk_setup_gl_framebuffer (f, new_width, new_height);

  /* Clear new FBO to background color first.  */
  glBindFramebuffer (GL_FRAMEBUFFER, FRAME_GL_FRAMEBUFFER (f));
  unsigned long bg = FRAME_X_OUTPUT (f)->background_color;
  float r = RED_FROM_ULONG (bg) / 255.0f;
  float g = GREEN_FROM_ULONG (bg) / 255.0f;
  float b = BLUE_FROM_ULONG (bg) / 255.0f;
  glClearColor (r, g, b, 1.0f);
  glClear (GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

  /* Blit preserved content back.  Use min of old/new dimensions
     to handle both grow and shrink.  */
  int blit_width = old_width < new_width ? old_width : new_width;
  int blit_height = old_height < new_height ? old_height : new_height;
  glBindFramebuffer (GL_READ_FRAMEBUFFER, temp_fbo);
  glBindFramebuffer (GL_DRAW_FRAMEBUFFER, FRAME_GL_FRAMEBUFFER (f));
  glBlitFramebuffer (0, 0, blit_width, blit_height, 0, 0, blit_width,
		     blit_height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

  /* Wait for GPU to finish the blit operations.  */
  glFinish ();

  /* Clean up temp resources.  */
  glDeleteFramebuffers (1, &temp_fbo);
  glDeleteTextures (1, &temp_texture);
  glBindFramebuffer (GL_FRAMEBUFFER, 0);
}

/* Legacy init function — now the GL context is created in
   pgtk_gl_drawing_area_realize.  This is kept for the fallback path
   in pgtk_begin_skia_clip.  */
static bool
pgtk_init_gl_context (struct frame *f)
{
  if (FRAME_SKIA_GL_INITIALIZED (f))
    return FRAME_GDK_GL_CONTEXT (f) != NULL;

  /* The drawing area realize callback handles initialization.
     If we get here without a context, it hasn't realized yet.  */
  return false;
}

/* Set up GL framebuffer for rendering at given size.  */
static bool
pgtk_setup_gl_framebuffer (struct frame *f, int width, int height)
{
  if (!FRAME_GDK_GL_CONTEXT (f))
    return false;

  /* Make the GL context current.  */
  gdk_gl_context_make_current (FRAME_GDK_GL_CONTEXT (f));

  /* Clear any stale GL errors before starting.  */
  while (glGetError () != GL_NO_ERROR)
    ;

  /* Bind and configure the texture.  */
  glBindTexture (GL_TEXTURE_2D, FRAME_GL_TEXTURE (f));
  glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
		GL_UNSIGNED_BYTE, NULL);

  /* Check for GL errors after texture allocation - this is where
     out-of-memory errors would typically appear.  */
  if (pgtk_check_gl_error ("glTexImage2D"))
    {
      glBindTexture (GL_TEXTURE_2D, 0);
      return false;
    }

  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  /* Create or resize the stencil renderbuffer.  Skia needs this for
     clip mask operations.  */
  if (!FRAME_GL_STENCIL (f))
    glGenRenderbuffers (1, &FRAME_GL_STENCIL (f));
  glBindRenderbuffer (GL_RENDERBUFFER, FRAME_GL_STENCIL (f));
  glRenderbufferStorage (GL_RENDERBUFFER, GL_STENCIL_INDEX8, width,
			 height);

  if (pgtk_check_gl_error ("glRenderbufferStorage"))
    {
      glBindRenderbuffer (GL_RENDERBUFFER, 0);
      glBindTexture (GL_TEXTURE_2D, 0);
      return false;
    }

  /* Bind the framebuffer and attach the texture and stencil.  */
  glBindFramebuffer (GL_FRAMEBUFFER, FRAME_GL_FRAMEBUFFER (f));
  glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			  GL_TEXTURE_2D, FRAME_GL_TEXTURE (f), 0);
  glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
			     GL_RENDERBUFFER, FRAME_GL_STENCIL (f));

  GLenum status = glCheckFramebufferStatus (GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE)
    {
      const char *status_str;
      switch (status)
	{
	case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
	  status_str = "INCOMPLETE_ATTACHMENT";
	  break;
	case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
	  status_str = "INCOMPLETE_MISSING_ATTACHMENT";
	  break;
	case GL_FRAMEBUFFER_UNSUPPORTED:
	  status_str = "UNSUPPORTED";
	  break;
	default:
	  status_str = "UNKNOWN";
	  break;
	}
      fprintf (stderr,
	       "Skia: Framebuffer incomplete: %s (0x%x), "
	       "size=%dx%d\n",
	       status_str, status, width, height);
      glBindFramebuffer (GL_FRAMEBUFFER, 0);
      return false;
    }

  /* Clear the FBO to black initially to avoid garbage data.
     The actual background color will be set when Skia draws.  */
  glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
  glClear (GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
  glFinish ();

  /* Unbind the framebuffer.  */
  glBindFramebuffer (GL_FRAMEBUFFER, 0);

  /* Mark GL state as dirty so Skia will reset its cached state.  */
  FRAME_SKIA_GL_STATE_DIRTY (f) = true;

  return true;
}

/* Ensure GL framebuffer/texture exist and are complete.  If the
   framebuffer is missing or incomplete, reinitialize it and discard
   any existing Skia surface that references it.  */
static bool
pgtk_ensure_gl_framebuffer (struct frame *f, int width, int height,
                            bool *recreated)
{
  bool needs_setup = false;

  if (recreated)
    *recreated = false;

  if (!FRAME_GL_FRAMEBUFFER (f))
    {
      glGenFramebuffers (1, &FRAME_GL_FRAMEBUFFER (f));
      needs_setup = true;
    }
  if (!FRAME_GL_TEXTURE (f))
    {
      glGenTextures (1, &FRAME_GL_TEXTURE (f));
      needs_setup = true;
    }
  if (!FRAME_GL_STENCIL (f))
    {
      glGenRenderbuffers (1, &FRAME_GL_STENCIL (f));
      needs_setup = true;
    }

  if (!needs_setup)
    {
      glBindFramebuffer (GL_FRAMEBUFFER, FRAME_GL_FRAMEBUFFER (f));
      GLenum status = glCheckFramebufferStatus (GL_FRAMEBUFFER);
      glBindFramebuffer (GL_FRAMEBUFFER, 0);
      if (status != GL_FRAMEBUFFER_COMPLETE)
        {
          fprintf (stderr,
                   "Skia: Framebuffer incomplete before draw: 0x%x\n",
                   status);
          needs_setup = true;
        }
    }

  if (needs_setup)
    {
      if (recreated)
        *recreated = true;
      if (!pgtk_setup_gl_framebuffer (f, width, height))
        return false;
      if (FRAME_SKIA_SURFACE (f))
        {
          emacs_skia_surface_destroy (FRAME_SKIA_SURFACE (f));
          FRAME_SKIA_SURFACE (f) = NULL;
          FRAME_SKIA_CANVAS (f) = NULL;
        }
    }

  return true;
}

/* Clean up GL resources for frame F.  */
static void
pgtk_cleanup_gl_context (struct frame *f)
{
  if (FRAME_SKIA_GL_CONTEXT (f))
    {
      emacs_skia_gl_context_destroy (FRAME_SKIA_GL_CONTEXT (f));
      FRAME_SKIA_GL_CONTEXT (f) = NULL;
    }

  if (FRAME_GDK_GL_CONTEXT (f))
    {
      gdk_gl_context_make_current (FRAME_GDK_GL_CONTEXT (f));

      if (FRAME_GL_FRAMEBUFFER (f))
	{
	  glDeleteFramebuffers (1, &FRAME_GL_FRAMEBUFFER (f));
	  FRAME_GL_FRAMEBUFFER (f) = 0;
	}
      if (FRAME_GL_TEXTURE (f))
	{
	  glDeleteTextures (1, &FRAME_GL_TEXTURE (f));
	  FRAME_GL_TEXTURE (f) = 0;
	}
      if (FRAME_GL_STENCIL (f))
	{
	  glDeleteRenderbuffers (1, &FRAME_GL_STENCIL (f));
	  FRAME_GL_STENCIL (f) = 0;
	}

      /* We created this context ourselves, so unref it.  */
      gdk_gl_context_clear_current ();
      g_object_unref (FRAME_GDK_GL_CONTEXT (f));
      FRAME_GDK_GL_CONTEXT (f) = NULL;
    }

  FRAME_GL_DRAWING_AREA (f) = NULL;

  FRAME_SKIA_GL_INITIALIZED (f) = false;
}

/* Forward declaration.  */
static void pgtk_skia_destroy_surface_only (struct frame *f);

void
pgtk_skia_update_surface_desired_size (struct frame *f, int width,
				       int height, bool force)
{
  if (FRAME_SKIA_SURFACE_DESIRED_WIDTH (f) != width
      || FRAME_SKIA_SURFACE_DESIRED_HEIGHT (f) != height || force)
    {
      /* Don't destroy the Skia surface here.  Keep the old surface
	 alive so that the draw callback can still composite stale
	 content instead of showing a black frame.  The surface will
	 be recreated in pgtk_begin_skia_clip when it detects a size
	 mismatch between the surface and the desired dimensions.
	 This is analogous to Cairo's double-buffering via
	 FRAME_CR_ACTIVE_CONTEXT, which keeps old content visible
	 until new content is ready.  */
      FRAME_SKIA_SURFACE_DESIRED_WIDTH (f) = width;
      FRAME_SKIA_SURFACE_DESIRED_HEIGHT (f) = height;
      SET_FRAME_GARBAGED (f);
    }
}

emacs_skia_canvas_t *
pgtk_begin_skia_clip (struct frame *f)
{
  emacs_skia_canvas_t *canvas = FRAME_SKIA_CANVAS (f);

  /* Check if the existing surface needs recreation due to a size
     change.  pgtk_skia_update_surface_desired_size defers surface
     destruction so that the draw callback can composite stale content
     instead of showing black.  We detect the mismatch here and
     destroy the old surface atomically before creating the new one.  */
  if (canvas && FRAME_SKIA_SURFACE (f))
    {
      int scale = FRAME_GL_DRAWING_AREA (f)
	? gtk_widget_get_scale_factor (FRAME_GL_DRAWING_AREA (f))
	: 1;
      int logical_w = FRAME_SKIA_SURFACE_DESIRED_WIDTH (f);
      int logical_h = FRAME_SKIA_SURFACE_DESIRED_HEIGHT (f);
      if (logical_w <= 0) logical_w = 1;
      if (logical_h <= 0) logical_h = 1;
      int desired_w = logical_w * scale;
      int desired_h = logical_h * scale;
      int cur_w = emacs_skia_surface_get_width (FRAME_SKIA_SURFACE (f));
      int cur_h = emacs_skia_surface_get_height (FRAME_SKIA_SURFACE (f));

      if (cur_w != desired_w || cur_h != desired_h)
	{
	  pgtk_skia_destroy_surface_only (f);
	  canvas = NULL;
	}
    }

  if (!canvas)
    {
      int logical_width = FRAME_SKIA_SURFACE_DESIRED_WIDTH (f);
      int logical_height = FRAME_SKIA_SURFACE_DESIRED_HEIGHT (f);

      if (logical_width <= 0)
	logical_width = 1;
      if (logical_height <= 0)
	logical_height = 1;

      /* Scale to device pixels for the FBO and Skia surface.  On
	 HiDPI displays (scale > 1), the texture is larger than the
	 logical window so that each CSS pixel maps to multiple device
	 pixels, producing crisp text and graphics.  The Skia canvas
	 gets a scale transform so drawing code can continue using
	 logical pixel coordinates.  */
      int scale = FRAME_GL_DRAWING_AREA (f)
	? gtk_widget_get_scale_factor (FRAME_GL_DRAWING_AREA (f))
	: 1;
      int width = logical_width * scale;
      int height = logical_height * scale;

      /* If we've exceeded the maximum number of consecutive surface
	 creation failures, don't attempt again.  The counter is reset
	 when the GL context is re-realized or
	 when the surface is successfully created.  */
      if (FRAME_GL_SURFACE_CREATION_FAILURES (f)
	  >= SKIA_MAX_SURFACE_CREATION_FAILURES)
	return NULL;

      /* Use drawing area's context if available.  */
      if (FRAME_GL_DRAWING_AREA (f) && FRAME_GDK_GL_CONTEXT (f))
	{
	  /* Make our GL context current.  */
	  gdk_gl_context_make_current (FRAME_GDK_GL_CONTEXT (f));

	  /* Clear any stale GL errors.  */
	  while (glGetError () != GL_NO_ERROR)
	    ;

	  /* Ensure the FBO exists and is complete before drawing.  This
	     handles driver/context loss.  */
	  bool fbo_recreated = false;
	  if (!pgtk_ensure_gl_framebuffer (f, width, height, &fbo_recreated))
	    {
	      fprintf (stderr, "Skia: Failed to ensure GL framebuffer\n");
	      pgtk_check_gl_error ("ensure_gl_framebuffer");
	      return NULL;
	    }

	  /* Check if FBO needs resizing by querying the texture size.  */
	  GLint tex_width = 0, tex_height = 0;
	  glBindTexture (GL_TEXTURE_2D, FRAME_GL_TEXTURE (f));
	  glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,
				    &tex_width);
	  glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT,
				    &tex_height);
	  glBindTexture (GL_TEXTURE_2D, 0);

	  if (tex_width != width || tex_height != height)
	    {
	      pgtk_resize_fbo_preserve_content (f, tex_width, tex_height,
					    width, height);
	      if (FRAME_SKIA_SURFACE (f))
		{
		  emacs_skia_surface_destroy (FRAME_SKIA_SURFACE (f));
		  FRAME_SKIA_SURFACE (f) = NULL;
		  FRAME_SKIA_CANVAS (f) = NULL;
		}
	    }

	  /* Create Skia surface if needed.  */
	  if (!FRAME_SKIA_SURFACE (f) && FRAME_GL_FRAMEBUFFER (f))
	    {
	      /* Reset Skia's cached GL state before creating a new
		 surface.  The preceding pgtk_ensure_gl_framebuffer or
		 pgtk_resize_fbo_preserve_content may have modified GL
		 state (bound textures, FBOs, renderbuffers) that Skia's
		 GrDirectContext has cached.  Without this reset, Skia
		 may wrap the FBO with stale assumptions about GL state,
		 leading to rendering into wrong targets or failing
		 surface creation.  */
	      if (FRAME_SKIA_GL_CONTEXT (f))
		emacs_skia_gl_context_reset (FRAME_SKIA_GL_CONTEXT (f));

	      FRAME_SKIA_SURFACE (f) = emacs_skia_surface_create_gl (
		FRAME_SKIA_GL_CONTEXT (f), width, height,
		FRAME_GL_FRAMEBUFFER (f), GL_RGBA8);
	      if (!FRAME_SKIA_SURFACE (f))
		{
		  fprintf (stderr,
			   "Skia: Failed to create GL surface "
			   "(size=%dx%d, fbo=%u)\n",
			   width, height, FRAME_GL_FRAMEBUFFER (f));
		  pgtk_check_gl_error ("surface_create_gl");
		}
	    }
	}
      /* Fallback: create offscreen GL context if no drawing area.  */
      else if (pgtk_init_gl_context (f)
	       && pgtk_ensure_gl_framebuffer (f, width, height, NULL))
	{
	  /* Make GL context current.  */
	  gdk_gl_context_make_current (FRAME_GDK_GL_CONTEXT (f));

	  /* Clear any stale GL errors.  */
	  while (glGetError () != GL_NO_ERROR)
	    ;

	  /* Reset Skia's cached GL state before surface creation.  */
	  if (FRAME_SKIA_GL_CONTEXT (f))
	    emacs_skia_gl_context_reset (FRAME_SKIA_GL_CONTEXT (f));

	  FRAME_SKIA_SURFACE (f)
	    = emacs_skia_surface_create_gl (FRAME_SKIA_GL_CONTEXT (f),
					    width, height,
					    FRAME_GL_FRAMEBUFFER (f),
					    GL_RGBA8);
	  if (!FRAME_SKIA_SURFACE (f))
	    {
	      fprintf (stderr,
		       "Skia: Failed to create GL surface "
		       "(fallback path, size=%dx%d)\n",
		       width, height);
	      pgtk_check_gl_error ("surface_create_gl_fallback");
	    }
	}

      if (!FRAME_SKIA_SURFACE (f))
	{
	  FRAME_GL_SURFACE_CREATION_FAILURES (f)++;
	  if (FRAME_GL_SURFACE_CREATION_FAILURES (f)
	      >= SKIA_MAX_SURFACE_CREATION_FAILURES)
	    fprintf (stderr,
		     "Skia: GL surface creation failed %d times for "
		     "frame %p, giving up (will retry on re-realize)\n",
		     FRAME_GL_SURFACE_CREATION_FAILURES (f),
		     (void *) f);
	  else
	    fprintf (stderr,
		     "Skia: No surface available for frame %p "
		     "(attempt %d/%d)\n",
		     (void *) f,
		     FRAME_GL_SURFACE_CREATION_FAILURES (f),
		     SKIA_MAX_SURFACE_CREATION_FAILURES);
	  return NULL;
	}

      /* Surface creation succeeded — reset the failure counter.  */
      FRAME_GL_SURFACE_CREATION_FAILURES (f) = 0;

      canvas = emacs_skia_surface_get_canvas (FRAME_SKIA_SURFACE (f));
      if (!canvas)
	{
	  fprintf (stderr,
		   "Skia: Failed to get canvas from surface for frame %p\n",
		   (void *) f);
	  return NULL;
	}
      FRAME_SKIA_CANVAS (f) = canvas;

      /* Apply HiDPI scale transform so drawing code works in logical
	 pixels while the underlying surface has device-pixel
	 resolution.  This is the base transform for all subsequent
	 save/restore cycles in pgtk_begin/end_skia_clip.  */
      if (scale > 1)
	emacs_skia_canvas_scale (canvas, (float) scale, (float) scale);

      /* Create a reusable paint object.  */
      if (!FRAME_SKIA_PAINT (f))
	{
	  FRAME_SKIA_PAINT (f) = emacs_skia_paint_create ();
	  if (!FRAME_SKIA_PAINT (f))
	    return NULL;
	}

      /* Clear the newly created surface with the background color.
	 This is critical for GL surfaces where the FBO starts with
	 undefined contents.  Without this, the first readback may
	 show garbage data causing flickering.  */
      {
	unsigned long bg = FRAME_X_OUTPUT (f)->background_color;
	Emacs_Color col;
	col.pixel = bg;
	pgtk_query_color (f, &col);
	uint8_t r = col.red >> 8;
	uint8_t g = col.green >> 8;
	uint8_t b = col.blue >> 8;
	uint8_t a = (uint8_t) (f->alpha_background * 255.0);
	emacs_skia_canvas_clear (canvas,
				 EMACS_SKIA_COLOR (a, r, g, b));
	/* Flush the clear operation for GL surfaces and reset Skia's
	   GL state tracking since we just set up the GL context.  */
	if (FRAME_SKIA_GL_CONTEXT (f))
	  {
	    emacs_skia_gl_context_flush (FRAME_SKIA_GL_CONTEXT (f));
	    glFinish ();
	    /* Reset Skia state after initial setup.  */
	    emacs_skia_gl_context_reset (FRAME_SKIA_GL_CONTEXT (f));
	    FRAME_SKIA_GL_STATE_DIRTY (f) = false;
	  }
      }
    }
  else if (FRAME_GDK_GL_CONTEXT (f))
    {
      int logical_w = FRAME_SKIA_SURFACE_DESIRED_WIDTH (f);
      int logical_h = FRAME_SKIA_SURFACE_DESIRED_HEIGHT (f);

      if (logical_w <= 0)
	logical_w = 1;
      if (logical_h <= 0)
	logical_h = 1;

      /* Scale to device pixels, matching the FBO dimensions.  */
      int sc = FRAME_GL_DRAWING_AREA (f)
	? gtk_widget_get_scale_factor (FRAME_GL_DRAWING_AREA (f))
	: 1;
      int width = logical_w * sc;
      int height = logical_h * sc;

      /* For GL surfaces, ensure the GL context is current before any
	 drawing operations.  Skia's GL backend requires this.  */
      gdk_gl_context_make_current (FRAME_GDK_GL_CONTEXT (f));

      /* Tell Skia to re-query GL state since GTK/GDK may have
	 modified it between frames.  Without this, Skia's cached GL
	 state may be stale and rendering may go to the wrong target.
       */
      if (FRAME_SKIA_GL_CONTEXT (f))
	emacs_skia_gl_context_reset (FRAME_SKIA_GL_CONTEXT (f));

      bool fbo_recreated = false;
      if (!pgtk_ensure_gl_framebuffer (f, width, height, &fbo_recreated))
	return NULL;
      if (fbo_recreated)
	{
	  SET_FRAME_GARBAGED (f);
	  return NULL;
	}
    }

  emacs_skia_canvas_save (canvas);

  return canvas;
}

void
pgtk_end_skia_clip (struct frame *f)
{
  emacs_skia_canvas_t *canvas = FRAME_SKIA_CANVAS (f);
  if (canvas)
    emacs_skia_canvas_restore (canvas);
}

void
pgtk_skia_set_paint_color (struct frame *f, unsigned long color,
			   bool respects_alpha_background)
{
  emacs_skia_paint_t *paint = FRAME_SKIA_PAINT (f);
  if (!paint)
    return;

  Emacs_Color col;
  col.pixel = color;
  pgtk_query_color (f, &col);

  uint8_t r = col.red >> 8;
  uint8_t g = col.green >> 8;
  uint8_t b = col.blue >> 8;
  uint8_t a;

  if (!respects_alpha_background)
    {
      a = 255;
      emacs_skia_paint_set_blend_mode (paint,
				       EMACS_SKIA_BLEND_SRC_OVER);
    }
  else
    {
      a = (uint8_t) (f->alpha_background * 255.0);
      emacs_skia_paint_set_blend_mode (paint, EMACS_SKIA_BLEND_SRC);
    }

  /* Always ensure we're in fill mode, not stroke mode.  */
  emacs_skia_paint_set_stroke (paint, false);

  emacs_skia_paint_set_color (paint, EMACS_SKIA_COLOR (a, r, g, b));

  {
    static int debug_color = -1;
    if (debug_color == -1)
      debug_color = getenv ("EMACS_SKIA_DEBUG_COLOR") != NULL;
    if (debug_color)
      fprintf (stderr,
	       "SKIA_COLOR: rgba(%d,%d,%d,%d) blend=%s "
	       "alpha_bg=%.2f\n",
	       r, g, b, a,
	       respects_alpha_background ? "SRC" : "SRC_OVER",
	       f->alpha_background);
  }
}

/* Destroy only the Skia surface, preserving GL context.
   Used during resize to avoid recreating the entire GL setup.  */
static void
pgtk_skia_destroy_surface_only (struct frame *f)
{
  if (FRAME_SKIA_PAINT (f))
    {
      emacs_skia_paint_destroy (FRAME_SKIA_PAINT (f));
      FRAME_SKIA_PAINT (f) = NULL;
    }

  /* Canvas is owned by surface, so just NULL it.  */
  FRAME_SKIA_CANVAS (f) = NULL;

  if (FRAME_SKIA_SURFACE (f))
    {
      /* For GL surfaces, make context current and flush before
	 destroying to ensure any pending operations complete and
	 the GrDirectContext state is clean.  Skip the flush if
	 the EGL context is lost — flushing with stale GL state
	 would crash in the Mesa GL thread.  */
      if (FRAME_GDK_GL_CONTEXT (f))
	{
	  gdk_gl_context_make_current (FRAME_GDK_GL_CONTEXT (f));
	  if (pgtk_gl_context_valid_p (f)
	      && FRAME_SKIA_GL_CONTEXT (f))
	    {
	      emacs_skia_gl_context_flush (FRAME_SKIA_GL_CONTEXT (f));
	      glFinish ();
	    }
	}
      emacs_skia_surface_destroy (FRAME_SKIA_SURFACE (f));
      FRAME_SKIA_SURFACE (f) = NULL;

      /* Reset the GrDirectContext state after destroying the surface.
	 This clears Skia's internal caches that may reference the
	 old surface's backend render target.  */
      if (FRAME_SKIA_GL_CONTEXT (f))
	{
	  emacs_skia_gl_context_reset (FRAME_SKIA_GL_CONTEXT (f));
	}
    }
}

void
pgtk_skia_destroy_frame_context (struct frame *f)
{
  pgtk_skia_destroy_surface_only (f);

  /* Clean up GL resources (includes Skia GL context, GDK GL context,
     and GL framebuffer/texture).  */
  pgtk_cleanup_gl_context (f);
}

/* Skia version of fill rectangle.  */
static void
pgtk_skia_fill_rectangle (struct frame *f, unsigned long color, int x,
			  int y, int width, int height,
			  bool respect_alpha_background)
{
  emacs_skia_canvas_t *canvas = pgtk_begin_skia_clip (f);
  if (!canvas)
    return;

  pgtk_skia_set_paint_color (f, color, respect_alpha_background);

  emacs_skia_irect_t rect = { x, y, x + width, y + height };
  emacs_skia_canvas_draw_irect (canvas, &rect, FRAME_SKIA_PAINT (f));

  {
    static int debug_rect = -1;
    if (debug_rect == -1)
      debug_rect = getenv ("EMACS_SKIA_DEBUG_RECT") != NULL;
    if (debug_rect)
      fprintf (stderr,
	       "SKIA_RECT: frame=%p child=%d fill (%d,%d)-(%d,%d) "
	       "color=0x%08lx respect_alpha=%d alpha_bg=%.2f\n",
	       (void *) f, FRAME_PARENT_FRAME (f) != NULL, x, y,
	       x + width, y + height, color, respect_alpha_background,
	       f->alpha_background);
  }

  pgtk_end_skia_clip (f);
}

/* Skia version of draw rectangle (stroked outline).  */
static void
pgtk_skia_draw_rectangle (struct frame *f, unsigned long color, int x,
			  int y, int width, int height,
			  bool respect_alpha_background)
{
  emacs_skia_canvas_t *canvas = pgtk_begin_skia_clip (f);
  if (!canvas)
    return;

  emacs_skia_paint_t *paint = FRAME_SKIA_PAINT (f);
  pgtk_skia_set_paint_color (f, color, respect_alpha_background);
  emacs_skia_paint_set_stroke (paint, true);
  emacs_skia_paint_set_stroke_width (paint, 1.0f);

  /* Use float rect for proper stroke alignment (0.5 offset for crisp
   * lines).  */
  emacs_skia_rect_t rect
    = { x + 0.5f, y + 0.5f, x + width + 0.5f, y + height + 0.5f };
  emacs_skia_canvas_draw_rect (canvas, &rect, paint);

  /* Reset to fill mode for subsequent operations.  */
  emacs_skia_paint_set_stroke (paint, false);

  pgtk_end_skia_clip (f);
}

#endif /* USE_SKIA */

#ifdef USE_SKIA
DEFUN ("pgtk-skia-gl-enabled-p", Fpgtk_skia_gl_enabled_p,
       Spgtk_skia_gl_enabled_p, 0, 1, 0,
       doc: /* Return non-nil if Skia GL acceleration is active for FRAME.
If FRAME is nil, use the selected frame.  */)
(Lisp_Object frame)
{
  struct frame *f = decode_window_system_frame (frame);
  if (FRAME_GDK_GL_CONTEXT (f) && FRAME_SKIA_GL_CONTEXT (f))
    return Qt;
  return Qnil;
}
#endif

void
syms_of_pgtkterm (void)
{
  DEFSYM (Qmodifier_value, "modifier-value");
  DEFSYM (Qalt, "alt");
  DEFSYM (Qhyper, "hyper");
  DEFSYM (Qmeta, "meta");
  DEFSYM (Qsuper, "super");
  DEFSYM (Qcontrol, "control");
  DEFSYM (QUTF8_STRING, "UTF8_STRING");
  /* Referenced in gtkutil.c.  */
  DEFSYM (Qtheme_name, "theme-name");
  DEFSYM (Qfile_name_sans_extension, "file-name-sans-extension");

  DEFSYM (Qfile, "file");
  DEFSYM (Qurl, "url");

  DEFSYM (Qlatin_1, "latin-1");

  xg_default_icon_file
    = build_pure_c_string ("icons/hicolor/scalable/apps/emacs.svg");
  staticpro (&xg_default_icon_file);

  DEFSYM (Qx_gtk_map_stock, "x-gtk-map-stock");

  DEFSYM (Qcopy, "copy");
  DEFSYM (Qmove, "move");
  DEFSYM (Qlink, "link");
  DEFSYM (Qprivate, "private");

  Fput (Qalt, Qmodifier_value, make_fixnum (alt_modifier));
  Fput (Qhyper, Qmodifier_value, make_fixnum (hyper_modifier));
  Fput (Qmeta, Qmodifier_value, make_fixnum (meta_modifier));
  Fput (Qsuper, Qmodifier_value, make_fixnum (super_modifier));
  Fput (Qcontrol, Qmodifier_value, make_fixnum (ctrl_modifier));

  DEFVAR_LISP ("x-ctrl-keysym", Vx_ctrl_keysym,
	       doc:/* SKIP: real doc in xterm.c.  */);
  Vx_ctrl_keysym = Qnil;

  DEFVAR_LISP ("x-alt-keysym", Vx_alt_keysym,
	       doc:/* SKIP: real doc in xterm.c.  */);
  Vx_alt_keysym = Qnil;

  DEFVAR_LISP ("x-hyper-keysym", Vx_hyper_keysym,
	       doc:/* SKIP: real doc in xterm.c.  */);
  Vx_hyper_keysym = Qnil;

  DEFVAR_LISP ("x-meta-keysym", Vx_meta_keysym,
	       doc:/* SKIP: real doc in xterm.c.  */);
  Vx_meta_keysym = Qnil;

  DEFVAR_LISP ("x-super-keysym", Vx_super_keysym,
	       doc:/* SKIP: real doc in xterm.c.  */);
  Vx_super_keysym = Qnil;

  DEFVAR_BOOL ("x-use-underline-position-properties",
	       x_use_underline_position_properties,
	       doc:/* SKIP: real doc in xterm.c.  */);
  x_use_underline_position_properties = 1;

  DEFVAR_BOOL ("x-underline-at-descent-line",
	       x_underline_at_descent_line,
	       doc:/* SKIP: real doc in xterm.c.  */);
  x_underline_at_descent_line = 0;

  DEFVAR_LISP ("x-toolkit-scroll-bars", Vx_toolkit_scroll_bars,
	       doc:/* SKIP: real doc in xterm.c.  */);
  Vx_toolkit_scroll_bars = intern_c_string ("gtk");

  DEFVAR_LISP ("pgtk-wait-for-event-timeout", Vpgtk_wait_for_event_timeout,
	       doc: /* How long to wait for GTK events.

Emacs will wait up to this many seconds to receive some GTK events
after making changes which affect the state of the graphical
interface.  Under some window managers this can take an indefinite
amount of time, so it is important to limit the wait.

If set to a non-float value, there will be no wait at all.  */);
  Vpgtk_wait_for_event_timeout = make_float (0.1);

  DEFVAR_LISP ("pgtk-keysym-table", Vpgtk_keysym_table,
	       doc
:/* Hash table of character codes indexed by X keysym codes.  */);
  Vpgtk_keysym_table
    = make_hash_table (&hashtest_eql, 900, Weak_None, false);

  window_being_scrolled = Qnil;
  staticpro (&window_being_scrolled);

#ifdef USE_SKIA
  defsubr (&Spgtk_skia_gl_enabled_p);
#endif

  /* Tell Emacs about this window system.  */
  Fprovide (Qpgtk, Qnil);
}

#ifdef USE_CAIRO
/* Cairo does not allow resizing a surface/context after it is
   created, so we need to trash the old context, create a new context
   on the next cr_clip_begin with the new dimensions and request a
   re-draw.

   This will leave the active context available to present on screen
   until a redrawn frame is completed.  */
void
pgtk_cr_update_surface_desired_size (struct frame *f, int width,
				     int height, bool force)
{
  if (FRAME_CR_SURFACE_DESIRED_WIDTH (f) != width
      || FRAME_CR_SURFACE_DESIRED_HEIGHT (f) != height || force)
    {
      pgtk_cr_destroy_frame_context (f);
      FRAME_CR_SURFACE_DESIRED_WIDTH (f) = width;
      FRAME_CR_SURFACE_DESIRED_HEIGHT (f) = height;
      SET_FRAME_GARBAGED (f);
    }
}

cairo_t *
pgtk_begin_cr_clip (struct frame *f)
{
  cairo_t *cr = FRAME_CR_CONTEXT (f);

  if (!cr)
    {
      cairo_surface_t *surface = gdk_window_create_similar_surface (
	gtk_widget_get_window (FRAME_GTK_WIDGET (f)),
	CAIRO_CONTENT_COLOR_ALPHA, FRAME_CR_SURFACE_DESIRED_WIDTH (f),
	FRAME_CR_SURFACE_DESIRED_HEIGHT (f));

      cr = FRAME_CR_CONTEXT (f) = cairo_create (surface);
      cairo_surface_destroy (surface);
    }

  cairo_save (cr);

  return cr;
}

void
pgtk_end_cr_clip (struct frame *f)
{
  cairo_restore (FRAME_CR_CONTEXT (f));
}

void
pgtk_set_cr_source_with_gc_foreground (struct frame *f, Emacs_GC *gc,
				       bool respects_alpha_background)
{
  pgtk_set_cr_source_with_color (f, gc->foreground,
				 respects_alpha_background);
}

void
pgtk_set_cr_source_with_gc_background (struct frame *f, Emacs_GC *gc,
				       bool respects_alpha_background)
{
  pgtk_set_cr_source_with_color (f, gc->background,
				 respects_alpha_background);
}

void
pgtk_set_cr_source_with_color (struct frame *f, unsigned long color,
			       bool respects_alpha_background)
{
  Emacs_Color col;
  col.pixel = color;
  pgtk_query_color (f, &col);

  if (!respects_alpha_background)
    {
      cairo_set_source_rgb (FRAME_CR_CONTEXT (f), col.red / 65535.0,
			    col.green / 65535.0, col.blue / 65535.0);
      cairo_set_operator (FRAME_CR_CONTEXT (f), CAIRO_OPERATOR_OVER);
    }
  else
    {
      cairo_set_source_rgba (FRAME_CR_CONTEXT (f), col.red / 65535.0,
			     col.green / 65535.0, col.blue / 65535.0,
			     f->alpha_background);
      cairo_set_operator (FRAME_CR_CONTEXT (f),
			  CAIRO_OPERATOR_SOURCE);
    }
}

void
pgtk_cr_draw_frame (cairo_t *cr, struct frame *f)
{
  cairo_set_source_surface (cr, FRAME_CR_SURFACE (f), 0, 0);
  cairo_paint (cr);
}

static cairo_status_t
pgtk_cr_accumulate_data (void *closure, const unsigned char *data,
			 unsigned int length)
{
  Lisp_Object *acc = (Lisp_Object *) closure;

  *acc
    = Fcons (make_unibyte_string ((char const *) data, length), *acc);

  return CAIRO_STATUS_SUCCESS;
}

void
pgtk_cr_destroy_frame_context (struct frame *f)
{
  if (FRAME_CR_CONTEXT (f) != NULL)
    {
      cairo_destroy (FRAME_CR_CONTEXT (f));
      FRAME_CR_CONTEXT (f) = NULL;
    }
}

static void
pgtk_cr_destroy (void *cr)
{
  block_input ();
  cairo_destroy (cr);
  unblock_input ();
}

Lisp_Object
pgtk_cr_export_frames (Lisp_Object frames,
		       cairo_surface_type_t surface_type)
{
  struct frame *f;
  cairo_surface_t *surface;
  cairo_t *cr;
  int width, height;
  void (*surface_set_size_func) (cairo_surface_t *, double, double)
    = NULL;
  Lisp_Object acc = Qnil;
  specpdl_ref count = SPECPDL_INDEX ();

  specbind (Qredisplay_dont_pause, Qt);
  redisplay_preserve_echo_area (31);

  f = XFRAME (XCAR (frames));
  frames = XCDR (frames);
  width = FRAME_PIXEL_WIDTH (f);
  height = FRAME_PIXEL_HEIGHT (f);

  block_input ();
# ifdef CAIRO_HAS_PDF_SURFACE
  if (surface_type == CAIRO_SURFACE_TYPE_PDF)
    {
      surface = cairo_pdf_surface_create_for_stream (
	pgtk_cr_accumulate_data, &acc, width, height);
      surface_set_size_func = cairo_pdf_surface_set_size;
    }
  else
# endif
# ifdef CAIRO_HAS_PNG_FUNCTIONS
    if (surface_type == CAIRO_SURFACE_TYPE_IMAGE)
    surface = cairo_image_surface_create (CAIRO_FORMAT_RGB24, width,
					  height);
  else
# endif
# ifdef CAIRO_HAS_PS_SURFACE
    if (surface_type == CAIRO_SURFACE_TYPE_PS)
    {
      surface
	= cairo_ps_surface_create_for_stream (pgtk_cr_accumulate_data,
					      &acc, width, height);
      surface_set_size_func = cairo_ps_surface_set_size;
    }
  else
# endif
# ifdef CAIRO_HAS_SVG_SURFACE
    if (surface_type == CAIRO_SURFACE_TYPE_SVG)
    surface
      = cairo_svg_surface_create_for_stream (pgtk_cr_accumulate_data,
					     &acc, width, height);
  else
# endif
    abort ();

  cr = cairo_create (surface);
  cairo_surface_destroy (surface);
  record_unwind_protect_ptr (pgtk_cr_destroy, cr);

  while (1)
    {
      cairo_t *saved_cr = FRAME_CR_CONTEXT (f);
      FRAME_CR_CONTEXT (f) = cr;
      pgtk_clear_area (f, 0, 0, width, height);
      expose_frame (f, 0, 0, width, height);
      FRAME_CR_CONTEXT (f) = saved_cr;

      if (NILP (frames))
	break;

      cairo_surface_show_page (surface);
      f = XFRAME (XCAR (frames));
      frames = XCDR (frames);
      width = FRAME_PIXEL_WIDTH (f);
      height = FRAME_PIXEL_HEIGHT (f);
      if (surface_set_size_func)
	(*surface_set_size_func) (surface, width, height);

      unblock_input ();
      maybe_quit ();
      block_input ();
    }

# ifdef CAIRO_HAS_PNG_FUNCTIONS
  if (surface_type == CAIRO_SURFACE_TYPE_IMAGE)
    {
      cairo_surface_flush (surface);
      cairo_surface_write_to_png_stream (surface,
					 pgtk_cr_accumulate_data,
					 &acc);
    }
# endif
  unblock_input ();

  unbind_to (count, Qnil);

  return CALLN (Fapply, Qconcat, Fnreverse (acc));
}
#endif /* USE_CAIRO */

#ifdef USE_SKIA
/* Skia-based frame export.
   Export types: pdf, svg (PNG would need additional work).  */

/* Callback adapter for Skia write function.  */
struct skia_export_accumulator
{
  Lisp_Object data;
};

static size_t
pgtk_skia_accumulate_data (void *ctx, const void *data, size_t size)
{
  struct skia_export_accumulator *acc
    = (struct skia_export_accumulator *) ctx;
  acc->data = Fcons (make_unibyte_string ((const char *) data, size),
		     acc->data);
  return size;
}

/* Export frames to PDF, SVG, or PNG using Skia.
   Returns the exported data as a unibyte string.  */
Lisp_Object
pgtk_skia_export_frames (Lisp_Object frames, Lisp_Object type)
{
  struct frame *f;
  int width, height;
  struct skia_export_accumulator acc = { Qnil };
  bool is_pdf = NILP (type) || EQ (type, Qpdf);
  bool is_svg = EQ (type, Qsvg);
  bool is_png = EQ (type, Qpng);

  if (!is_pdf && !is_svg && !is_png)
    error ("Skia export supports pdf, svg, and png types");

  if ((is_svg || is_png) && !NILP (XCDR (frames)))
    error ("SVG and PNG export cannot handle multiple frames");

  redisplay_preserve_echo_area (31);

  f = XFRAME (XCAR (frames));
  frames = XCDR (frames);
  width = FRAME_PIXEL_WIDTH (f);
  height = FRAME_PIXEL_HEIGHT (f);

  block_input ();

  if (is_pdf)
    {
      /* Create PDF document.  */
      emacs_skia_document_t *doc
	= emacs_skia_document_create_pdf (pgtk_skia_accumulate_data,
					  &acc, width, height);
      if (!doc)
	{
	  unblock_input ();
	  error ("Failed to create PDF document");
	}

      while (1)
	{
	  emacs_skia_canvas_t *canvas
	    = emacs_skia_document_begin_page (doc, width, height);
	  if (!canvas)
	    {
	      emacs_skia_document_close (doc);
	      unblock_input ();
	      error ("Failed to begin PDF page");
	    }

	  /* Save current Skia canvas and use document page.  */
	  emacs_skia_canvas_t *saved_canvas = FRAME_SKIA_CANVAS (f);
	  FRAME_SKIA_CANVAS (f) = canvas;

	  /* Clear and redraw the frame.  */
	  emacs_skia_canvas_clear (canvas,
				   EMACS_SKIA_COLOR_RGB (255, 255,
							 255));
	  expose_frame (f, 0, 0, width, height);

	  FRAME_SKIA_CANVAS (f) = saved_canvas;
	  emacs_skia_document_end_page (doc);

	  if (NILP (frames))
	    break;

	  f = XFRAME (XCAR (frames));
	  frames = XCDR (frames);
	  width = FRAME_PIXEL_WIDTH (f);
	  height = FRAME_PIXEL_HEIGHT (f);

	  unblock_input ();
	  maybe_quit ();
	  block_input ();
	}

      emacs_skia_document_close (doc);
    }
  else if (is_svg)
    {
      /* Create SVG canvas.  */
      emacs_skia_canvas_t *canvas
	= emacs_skia_svg_canvas_create (pgtk_skia_accumulate_data,
					&acc, width, height);
      if (!canvas)
	{
	  unblock_input ();
	  error ("Failed to create SVG canvas");
	}

      /* Save current Skia canvas and use SVG canvas.  */
      emacs_skia_canvas_t *saved_canvas = FRAME_SKIA_CANVAS (f);
      FRAME_SKIA_CANVAS (f) = canvas;

      /* Clear and redraw the frame.  */
      emacs_skia_canvas_clear (canvas,
			       EMACS_SKIA_COLOR_RGB (255, 255, 255));
      expose_frame (f, 0, 0, width, height);

      FRAME_SKIA_CANVAS (f) = saved_canvas;
      emacs_skia_svg_canvas_finish (canvas);
    }
  else if (is_png)
    {
      /* Create a temporary raster surface for PNG export.  */
      emacs_skia_surface_t *png_surface
	= emacs_skia_surface_create_raster (width, height);
      if (!png_surface)
	{
	  unblock_input ();
	  error ("Failed to create PNG surface");
	}

      emacs_skia_canvas_t *canvas
	= emacs_skia_surface_get_canvas (png_surface);
      if (!canvas)
	{
	  emacs_skia_surface_destroy (png_surface);
	  unblock_input ();
	  error ("Failed to get PNG canvas");
	}

      /* Save current Skia canvas and use PNG canvas.  */
      emacs_skia_canvas_t *saved_canvas = FRAME_SKIA_CANVAS (f);
      FRAME_SKIA_CANVAS (f) = canvas;

      /* Clear and redraw the frame.  */
      emacs_skia_canvas_clear (canvas,
			       EMACS_SKIA_COLOR_RGB (255, 255, 255));
      expose_frame (f, 0, 0, width, height);

      FRAME_SKIA_CANVAS (f) = saved_canvas;

      /* Flush the surface and encode to PNG.  */
      emacs_skia_surface_flush (png_surface);
      if (!emacs_skia_surface_write_to_png (png_surface,
					    pgtk_skia_accumulate_data,
					    &acc))
	{
	  emacs_skia_surface_destroy (png_surface);
	  unblock_input ();
	  error ("Failed to encode PNG");
	}

      emacs_skia_surface_destroy (png_surface);
    }

  unblock_input ();

  return CALLN (Fapply, Qconcat, Fnreverse (acc.data));
}
#endif /* USE_SKIA */
