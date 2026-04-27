# PGTK Skia GL reliability plan

## Summary

The PGTK Skia GL backend currently uses a `GtkDrawingArea` with an Emacs-created detached `GdkGLContext`, renders Skia into an Emacs-owned FBO texture, and then calls `gdk_cairo_draw_from_gl` directly to composite that texture into the GTK/Wayland window.

This architecture is fragile on GTK3/Wayland. The reliable direction is to return the default backend to a `GtkGLArea`-managed presentation path, while keeping the recent fail-closed/context-loss hardening where it still applies.

No raster fallback is proposed in this plan.

## Current failure mode

After hours of use, Emacs can enter a state where PGTK Skia GL frames flicker, freeze, crash, or show only the frame background. Cairo rendering does not exhibit the same issue.

Observed logs include:

```text
eglMakeCurrent failed
[EGL] 0x3002 (BAD_ACCESS) eglMakeCurrent: eglMakeCurrent
Skia GL make-current-failed: ... realized=1 mapped=1 visible=1 ... egl_current=(nil)
Skia: abandoning lost GL context ... after 3 make-current failures
[EGL] 0x300d (BAD_SURFACE) eglSwapInterval: eglSwapInterval
```

Important observations from the live failure:

- The first failing frames were normal visible frames, not only Corfu/child frames.
- Failed frames were still GTK-realized, mapped, visible, and had valid-looking `GdkWindow`s.
- `emacsclient` remained reachable, so this was not necessarily an immediate total process death.
- After abandoning the GL context, visible frames could remain blank/background because no new usable presentation path was reached.
- The issue happens outside EWM, so EWM should not be treated as the root cause.

## Relevant existing commits

Recent hardening already added:

```text
0b2fb5c5dfd fix(pgtk/skia): fail closed on EGL context loss
797d2f77a1f fix(pgtk/skia): recreate lost GL contexts
56d79dea378 fix(pgtk/skia): log GL context lifecycle
528bd244802 fix(pgtk/skia): retry GL context recreation
```

These commits improve behavior after failure, but they do not fully solve the root architectural problem.

The architectural change that introduced the current direct `GtkDrawingArea` path is:

```text
2d39ab453d6 refactor(pgtk/skia): Replace GtkGLArea with GtkDrawingArea
```

Before that commit, this branch used a `GtkGLArea`-based presentation path.

## Current architecture

Current path:

```text
GtkDrawingArea
  -> Emacs creates detached GdkGLContext with gdk_window_create_gl_context
  -> Skia renders into Emacs-owned FBO texture
  -> Emacs calls gdk_cairo_draw_from_gl directly
  -> GDK switches to its hidden window paint context
  -> GDK composites into the window back buffer
```

Important source locations:

- `src/pgtkterm.c`: creation of `GtkDrawingArea` and GL callbacks.
- `src/pgtkterm.c`: `pgtk_gl_drawing_area_realize` creates the context with `gdk_window_create_gl_context`.
- `src/pgtkterm.c`: `pgtk_gl_drawing_area_draw` calls `gdk_cairo_draw_from_gl` directly.
- `src/pgtkterm.c`: `pgtk_frame_gl_context_make_current` attempts to verify that Emacs' context is actually current.

## Why this is fragile

### 1. `gdk_window_create_gl_context` creates a dual-context setup

GTK source shows that `gdk_window_create_gl_context` first ensures the window's internal paint context exists, then creates an unattached app context sharing with it.

Source evidence:

- `gtk-source/gdk/gdkwindow.c`: `gdk_window_get_paint_gl_context` creates `window->impl_window->gl_paint_context` with `attached = TRUE`.
- `gtk-source/gdk/gdkwindow.c`: `gdk_window_create_gl_context` creates the returned context with `attached = FALSE`, sharing with the paint context.

This means each Skia frame using the direct drawing-area path has at least two relevant GL contexts:

1. GDK's attached paint context, tied to the real window EGL surface.
2. Emacs' detached/offscreen context, used by Skia/FBO rendering.

If only Emacs' context is recreated, GDK's paint context and the window EGL surface remain untouched.

### 2. GTK3's public `gdk_gl_context_make_current` cannot report failure

GTK source:

- `gtk-source/gdk/gdkglcontext.c`: `gdk_gl_context_make_current` returns `void`.
- It calls the backend make-current function internally.
- If the backend fails, callers get no direct error result.

On Wayland, the backend does this:

- `gtk-source/gdk/wayland/gdkglcontext-wayland.c`: calls `eglMakeCurrent`.
- On failure it logs `eglMakeCurrent failed` and returns `FALSE` internally.
- The public caller still cannot observe this directly except through indirect state checks.

This is why Emacs added explicit verification around make-current. That hardening is useful, but it cannot protect against failures hidden inside GTK's own `gdk_cairo_draw_from_gl` implementation.

### 3. `gdk_cairo_draw_from_gl` switches to GDK's paint context and keeps going

GTK source:

- `gtk-source/gdk/gdkgl.c`: `gdk_cairo_draw_from_gl` obtains the window paint context.
- It records the current context, may insert a fence, then calls `gdk_gl_context_make_current(paint_context)`.
- Since that function returns `void`, `gdk_cairo_draw_from_gl` has no explicit failure result to check.
- It then proceeds with GL operations.

This creates a dangerous failure mode: if switching to the paint context fails, GDK may continue issuing GL commands with the previous context or with no valid context.

### 4. Detached contexts on Wayland use surfaceless EGL, but GTK still calls `eglSwapInterval`

GTK Wayland source:

- Attached contexts use the real window EGL surface.
- Unattached contexts use `EGL_NO_SURFACE` when `EGL_KHR_surfaceless_context` is available.
- After successful `eglMakeCurrent`, GTK unconditionally calls `eglSwapInterval`.

This plausibly explains the repeated:

```text
[EGL] 0x300d (BAD_SURFACE) eglSwapInterval: eglSwapInterval
```

The `BAD_SURFACE` spam is probably not the original fatal failure, but it is strong evidence that the direct detached-context path is exercising a shaky GTK3/Wayland path.

### 5. Current recovery resets only Emacs' context

`pgtk_abandon_gl_context` drops Emacs' Skia/GL objects and `FRAME_GDK_GL_CONTEXT`, then schedules redraw/recreation.

However, it does not recreate:

- the `GtkDrawingArea` widget;
- the `GdkWindow`;
- GDK's hidden paint context;
- the Wayland EGL window surface.

If the poisoned object is GDK's paint context or the window EGL surface, recreating only Emacs' detached context is insufficient.

## Previous architecture

Before `2d39ab453d6`, the branch used `GtkGLArea`.

Previous path:

```text
GtkGLArea
  -> GTK owns GL context/window paint integration
  -> Emacs/Skia uses GtkGLArea's current context
  -> Skia renders into Emacs-owned FBO
  -> render callback blits Emacs FBO into GtkGLArea's framebuffer
  -> GTK presents the GtkGLArea
```

This has one extra GPU copy/blit compared with the current direct path, but it keeps GL lifecycle and presentation inside GTK's intended `GtkGLArea` model.

GTK's own `GtkGLArea` implementation also restores its context after calling `gdk_cairo_draw_from_gl`:

```c
gdk_cairo_draw_from_gl (...);
gtk_gl_area_make_current (area);
```

The current manual drawing-area path does not follow that pattern by default.

## Recommended reliability target

The recommended default architecture is:

```text
GtkGLArea + Skia GL
```

No raster fallback is included in this plan.

Desired properties:

- GTK owns the presentation context lifecycle.
- Emacs does not manually create detached `GdkGLContext`s for normal frame presentation.
- Emacs does not call `gdk_cairo_draw_from_gl` directly for the default path.
- GL failures fail closed and do not call Skia/GL with an invalid context.
- Repeated context loss recreates the `GtkGLArea` widget/context, not only Emacs' own FBO/context fields.

## Implementation plan

### Phase 1: Restore `GtkGLArea` presentation

Bring back the `GtkGLArea` default path from before `2d39ab453d6`, updated to include the useful later safety fixes.

Core changes:

- Replace `FRAME_GL_DRAWING_AREA` default presentation with `FRAME_GL_AREA` / `GtkGLArea` again.
- Use `gtk_gl_area_queue_render` instead of `gtk_widget_queue_draw` for Skia GL presentation.
- Restore callbacks:
  - `pgtk_gl_area_realize`
  - `pgtk_gl_area_unrealize`
  - `pgtk_gl_area_render`
  - `pgtk_gl_area_resize`
- Use `gtk_gl_area_make_current` before Skia/GL work.
- Use `gtk_gl_area_get_error` as part of context-health checks.
- Remove direct `gdk_window_create_gl_context` usage from the default presentation path.
- Remove direct `gdk_cairo_draw_from_gl` usage from Emacs' default Skia draw callback.

This can be implemented either by partially reverting `2d39ab453d6` or by reintroducing the old code manually and adapting it to current field names and safety guards.

### Phase 2: Preserve fail-closed behavior

Keep the spirit of the recent context-loss hardening:

- Never call Skia GL flush/destroy paths unless the GL context is confirmed current.
- Do not use `glGetError` as the first diagnostic on a broken-context path if it can flush stale driver work and crash.
- On failed make-current:
  - mark the frame garbaged;
  - stop issuing GL work for that frame;
  - rate-limit recovery attempts;
  - avoid queueing render loops that feed EGL warning storms.

The exact helper functions will need adjustment because `GtkGLArea` manages context ownership differently from the direct `GdkGLContext` path.

### Phase 3: Recreate the whole `GtkGLArea` on repeated loss

If context loss persists, recovery should recreate the GTK GL widget, not just Emacs' FBO/Skia objects.

Target recovery:

```text
make-current failure threshold reached
  -> stop using existing GL resources
  -> destroy/unrealize/remove GtkGLArea child
  -> create a fresh GtkGLArea
  -> connect callbacks
  -> show/allocate it
  -> mark frame garbaged
  -> let redisplay repopulate content
```

Rationale:

- Recreating the widget lets GTK recreate its GLArea context and presentation state.
- This has a better chance of resetting GDK/Wayland objects than recreating only Emacs' detached context.

### Phase 4: Keep the direct path only as experimental, if desired

The current direct `GtkDrawingArea` path could be kept behind a compile-time or runtime experimental switch, but it should not be the default if reliability is the priority.

If kept, it should additionally:

- restore Emacs' context after each `gdk_cairo_draw_from_gl`, matching `GtkGLArea`'s pattern;
- stop clearing/rebinding contexts on every normal make-current path;
- recreate the whole drawing-area widget/GdkWindow on context loss;
- include extra diagnostics around `gdk_cairo_draw_from_gl` and current EGL surface/context state.

## Non-goals

For this plan, do not add:

- Skia raster fallback;
- Cairo fallback;
- broad EWM-specific workarounds;
- GTK source patching as a required runtime dependency.

GTK-side fixes may still be useful upstream, but Emacs should be reliable against stock GTK3.

## Validation plan

### Build validation

Run at least:

```sh
make -C src pgtkterm.o -j2
make -C src emacs -j2
```

### Runtime validation

Run the rebuilt Emacs under normal PGTK/Wayland usage for multiple hours.

Monitor logs for:

```sh
journalctl --user -u ewm-emacs.service --since now -f
```

or the equivalent service/session when not using EWM.

Important log checks:

- no recurring `eglMakeCurrent failed` loop;
- no repeated `EGL_BAD_ACCESS eglMakeCurrent` loop;
- no persistent `EGL_BAD_SURFACE eglSwapInterval` spam;
- no blank/background-only frames after context recovery;
- frames redraw correctly after monitor changes, focus changes, child-frame activity, and long idle periods.

### Functional stress cases

Exercise:

- multiple visible frames on multiple monitors;
- Corfu/company child frames;
- Magit/status buffers with frequent redisplay;
- window resize/maximize/fullscreen changes;
- monitor layout changes;
- long idle periods followed by heavy redisplay;
- visible bell if enabled;
- alpha-background if supported by the branch.

## Expected tradeoff

The `GtkGLArea` path may reintroduce an extra GPU blit/copy that the direct `GtkDrawingArea` path removed.

That is an acceptable tradeoff for an editor UI:

```text
slightly more GPU work > rare freezes/crashes/blank frames after hours
```

Once the default backend is stable again, performance optimizations can be revisited behind an experimental option.

## Bottom line

The reliable path is to return to a `GtkGLArea`-managed Skia GL presentation architecture and keep the recent fail-closed safety checks. The direct `GtkDrawingArea` + detached `GdkGLContext` + direct `gdk_cairo_draw_from_gl` design is likely the root of the long-running GTK3/Wayland instability and should not remain the default reliability-focused backend.
