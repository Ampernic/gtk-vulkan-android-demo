/*
 * gtk-vulkan-android-demo - compare the GSK renderers (Vulkan / OpenGL / Cairo)
 * on a heavy 3D scene. Each renderer is created explicitly and realized for the
 * display, then the scene is rendered off-screen with gsk_renderer_render_texture()
 * and forced to completion, so the reported frame time is that renderer's real
 * cost (not the vsync-capped on-screen rate). Switching renderer just swaps the
 * off-screen renderer in place - no process relaunch, so it works on Android too,
 * where the process is owned by the Android runtime and cannot re-exec itself.
 * Built to demonstrate the gdk-android Vulkan backend; runs on any GTK 4 platform.
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <adwaita.h>
#include <math.h>

#define BENCH 720            /* fixed off-screen render resolution (consistent metric) */
#define BENCH_REPEAT 16      /* scenes drawn per readback to amortise the GPU round-trip */

/* ---- a single cog/gear drawn once into a texture ---------------------- */

static GdkTexture *
make_gear_texture (double size, const GdkRGBA *color)
{
  int teeth = 12;
  double r_out = size * 0.5, r_in = r_out * 0.72, r_hole = r_out * 0.26;
  cairo_surface_t *cs = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, (int) size, (int) size);
  cairo_t *cr = cairo_create (cs);
  cairo_translate (cr, size / 2, size / 2);

  for (int i = 0; i < teeth * 2; i++)
    {
      double a = G_PI * i / teeth;
      double r = (i % 2 == 0) ? r_out : r_in;
      (i == 0 ? cairo_move_to : cairo_line_to) (cr, r * cos (a), r * sin (a));
    }
  cairo_close_path (cr);
  cairo_set_source_rgba (cr, color->red, color->green, color->blue, color->alpha);
  cairo_fill_preserve (cr);
  cairo_set_source_rgba (cr, 1, 1, 1, 0.25);
  cairo_set_line_width (cr, size * 0.03);
  cairo_stroke (cr);

  cairo_arc (cr, 0, 0, r_hole, 0, 2 * G_PI);
  cairo_set_operator (cr, CAIRO_OPERATOR_CLEAR);
  cairo_fill (cr);

  cairo_destroy (cr);
  cairo_surface_flush (cs);
  int wpx = cairo_image_surface_get_width (cs), hpx = cairo_image_surface_get_height (cs);
  int stride = cairo_image_surface_get_stride (cs);
  GBytes *bytes = g_bytes_new (cairo_image_surface_get_data (cs), (gsize) stride * hpx);
  GdkTexture *tex = gdk_memory_texture_new (wpx, hpx, GDK_MEMORY_B8G8R8A8_PREMULTIPLIED, bytes, stride);
  g_bytes_unref (bytes);
  cairo_surface_destroy (cs);
  return tex;
}

/* ---- the heavy 3D scene (built into a snapshot) ----------------------- */

/* flat = no perspective/3D (for the Cairo software renderer, which cannot
 * rasterize a perspective projection); the scene then degrades to 2D. */
static void
build_scene (GtkSnapshot *s, double w, double h, GdkTexture *gear,
             double angle, int density, gboolean blur, gboolean flat)
{
  /* dark backdrop so the off-screen frame is opaque */
  gtk_snapshot_append_color (s, &(GdkRGBA){ 0.09f, 0.10f, 0.12f, 1.0f },
                             &GRAPHENE_RECT_INIT (0, 0, (float) w, (float) h));

  if (blur)
    gtk_snapshot_push_blur (s, 8.0);

  gtk_snapshot_save (s);
  if (!flat)
    {
      /* perspective + tumble the whole field in 3D */
      gtk_snapshot_translate (s, &GRAPHENE_POINT_INIT ((float) w / 2, (float) h / 2));
      gtk_snapshot_perspective (s, (float) (MAX (w, h) * 1.4));
      gtk_snapshot_rotate_3d (s, (float) (sin (angle * 0.6) * 38.0), graphene_vec3_y_axis ());
      gtk_snapshot_rotate_3d (s, (float) (cos (angle * 0.4) * 26.0), graphene_vec3_x_axis ());
      gtk_snapshot_translate (s, &GRAPHENE_POINT_INIT (-(float) w / 2, -(float) h / 2));
    }

  double cell = w / density;
  int rows = (int) ceil (h / cell) + 1;
  for (int r = 0; r < rows; r++)
    for (int c = 0; c < density; c++)
      {
        double cx = (c + 0.5) * cell, cy = (r + 0.5) * cell;
        double dir = ((r + c) % 2 == 0) ? 1.0 : -1.0;

        gtk_snapshot_save (s);
        if (!flat)
          {
            float z = ((r + c) % 2 == 0) ? cell * 0.6f : -cell * 0.6f;
            gtk_snapshot_translate_3d (s, &GRAPHENE_POINT3D_INIT ((float) cx, (float) cy, z));
          }
        else
          gtk_snapshot_translate (s, &GRAPHENE_POINT_INIT ((float) cx, (float) cy));
        gtk_snapshot_rotate (s, (float) (dir * angle * 180.0 / G_PI));
        gtk_snapshot_append_texture (s, gear,
                                     &GRAPHENE_RECT_INIT (-(float) cell * 0.45f, -(float) cell * 0.45f,
                                                          (float) cell * 0.9f, (float) cell * 0.9f));
        gtk_snapshot_restore (s);
      }

  gtk_snapshot_restore (s);
  if (blur)
    gtk_snapshot_pop (s);
}

/* Globals-stress scene: deeply nested transforms (mvp changes every level) with a
 * rounded-clipped colour at each level (clip changes every level). This hammers the
 * per-op globals path the same way @otte's benchmark node does, but stays visible so
 * a wrong globals delivery (push vs UBO) shows up as garbled output. */
#define STRESS_LEVELS 24
static void
build_transform_stress (GtkSnapshot *s, double w, double h, double angle)
{
  gtk_snapshot_append_color (s, &(GdkRGBA){ 0.09f, 0.10f, 0.12f, 1.0f },
                             &GRAPHENE_RECT_INIT (0, 0, (float) w, (float) h));

  float side = (float) MIN (w, h) * 0.8f;
  gtk_snapshot_save (s);
  gtk_snapshot_translate (s, &GRAPHENE_POINT_INIT ((float) w / 2, (float) h / 2));

  for (int i = 0; i < STRESS_LEVELS; i++)
    {
      /* cumulative transform: every level pushes a fresh mvp */
      gtk_snapshot_rotate (s, (float) (angle * 12.0 + i * 7.0));
      gtk_snapshot_scale (s, 0.93f, 0.93f);

      GdkRGBA col = {
        0.5f + 0.5f * (float) sin (i * 0.5 + angle),
        0.5f + 0.5f * (float) sin (i * 0.5 + angle + 2.0),
        0.5f + 0.5f * (float) sin (i * 0.5 + angle + 4.0),
        1.0f
      };
      graphene_rect_t r = GRAPHENE_RECT_INIT (-side / 2, -side / 2, side, side);
      GskRoundedRect rr;
      gsk_rounded_rect_init_from_rect (&rr, &r, side * 0.18f);
      gtk_snapshot_push_rounded_clip (s, &rr);
      gtk_snapshot_append_color (s, &col, &r);
      gtk_snapshot_pop (s);
    }

  gtk_snapshot_restore (s);
}

/* ---- the widget: off-screen benchmark + on-screen display ------------- */

/* combo order: 0 Vulkan, 1 OpenGL, 2 Cairo */

#define GEARS_TYPE_VIEW (gears_view_get_type ())
G_DECLARE_FINAL_TYPE (GearsView, gears_view, GEARS, VIEW, GtkWidget)

struct _GearsView
{
  GtkWidget    parent_instance;
  GdkTexture  *gear;
  GskRenderer *renderer;     /* off-screen renderer we own and benchmark */
  int          backend;      /* selected backend; -1 = not yet initialised */
  GdkTexture  *frame;        /* last off-screen frame, shown on screen */
  guchar      *dlbuf;        /* reused download buffer (force GPU completion) */
  double       angle;
  int          density;
  gboolean     blur;
  int          scene;        /* 0 = gears, 1 = transform stress */
  double       render_ms;    /* smoothed off-screen frame cost (uncapped) */
  GtkLabel    *fps_label;
  GtkLabel    *renderer_label;
  AdwComboRow *combo;

  /* automated sweep: every renderer x scene, collected into a table */
  gboolean     collecting;
  int          collect_combo;            /* 0..5: renderer = combo/2, scene = combo%2 */
  int          collect_frames;           /* frames spent on the current combo */
  int          collect_n;                /* raw samples gathered for the current combo */
  double       collect_samples[64];
  double       results[3][2];            /* ms/frame per renderer per scene; NAN = n/a */
  int          saved_backend, saved_scene;
  GtkLabel    *table_label;
  AdwComboRow *scene_combo;
};

#define COLLECT_WARMUP  20               /* frames discarded before sampling a combo */
#define COLLECT_SAMPLES 40               /* frames sampled per combo (median taken) */
G_DEFINE_FINAL_TYPE (GearsView, gears_view, GTK_TYPE_WIDGET)

#define GEAR_PX 110.0

static GskRenderer *
make_renderer (int backend)
{
  switch (backend)
    {
    case 0:  return gsk_vulkan_renderer_new ();
    case 2:  return gsk_cairo_renderer_new ();
    default: return gsk_gl_renderer_new ();
    }
}

static int
renderer_backend (GskRenderer *r)
{
  const char *t = r ? G_OBJECT_TYPE_NAME (r) : "";
  if (strstr (t, "Vulkan")) return 0;
  if (strstr (t, "Cairo"))  return 2;
  return 1; /* GL / Ngl */
}

static void
update_renderer_label (GearsView *self)
{
  if (self->renderer_label == NULL || self->renderer == NULL)
    return;
  const char *names[] = { "Vulkan", "OpenGL", "Cairo (software)" };
  char *s = g_strdup_printf ("Renderer: <b>%s</b>", names[renderer_backend (self->renderer)]);
  gtk_label_set_markup (self->renderer_label, s);
  g_free (s);
}

/* Swap the off-screen renderer in place. Falls back to GL if the requested
 * backend cannot be realized (e.g. Vulkan on Android API < 28). */
static void
gears_view_set_backend (GearsView *self, int backend)
{
  if (backend == self->backend)
    return;
  /* Realize against the surface, not the display: the display-only (headless)
   * path crashes the Android Vulkan backend, while the surface path is the one
   * proven to work on device. render_texture renders off-screen regardless. */
  GtkNative *native = gtk_widget_get_native (GTK_WIDGET (self));
  GdkSurface *surface = native ? gtk_native_get_surface (native) : NULL;
  if (surface == NULL)
    return;   /* not realized yet; the tick retries */

  GError *err = NULL;
  GskRenderer *r = make_renderer (backend);
  if (!gsk_renderer_realize (r, surface, &err))
    {
      g_warning ("%s renderer unavailable: %s",
                 backend == 0 ? "Vulkan" : backend == 2 ? "Cairo" : "OpenGL",
                 err ? err->message : "(unknown)");
      g_clear_error (&err);
      g_object_unref (r);
      if (backend == 1)             /* GL itself failed: nothing to fall back to */
        {
          /* Keep the current renderer and reflect that in the combo, so the UI
           * does not falsely show OpenGL (e.g. emulators whose EGL lacks
           * EGL_KHR_surfaceless_context, which the GL renderer requires). */
          if (self->combo)
            adw_combo_row_set_selected (self->combo, (guint) self->backend);
          return;
        }
      r = gsk_gl_renderer_new ();   /* graceful fallback */
      if (!gsk_renderer_realize (r, surface, &err))
        {
          g_clear_error (&err);
          g_object_unref (r);
          return;
        }
    }

  if (self->renderer)
    {
      gsk_renderer_unrealize (self->renderer);
      g_object_unref (self->renderer);
    }
  self->renderer = r;
  self->backend = renderer_backend (r);   /* effective backend (after any fallback) */
  self->render_ms = 0;
  g_clear_object (&self->frame);

  update_renderer_label (self);
  if (self->combo)
    adw_combo_row_set_selected (self->combo, (guint) self->backend);
}

static void
gears_view_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  GearsView *self = GEARS_VIEW (widget);
  int w = gtk_widget_get_width (widget), h = gtk_widget_get_height (widget);
  /* Draw the scene live with the window's own renderer. No per-frame textures
   * are created or retained, so nothing accumulates in the renderer's texture
   * cache. The Cairo selection is shown flat (Cairo has no 3D); GL/Vulkan 3D. */
  gboolean flat = self->backend == 2;
  if (self->scene == 1)
    build_transform_stress (snapshot, w, h, self->angle);
  else
    build_scene (snapshot, w, h, self->gear, self->angle, self->density, self->blur, flat);
}

/* ---- automated sweep: every renderer x scene into a table ------------- */

static const char *const RENDERER_NAMES[3] = { "Vulkan", "OpenGL", "Cairo" };
static const char *const SCENE_NAMES[2]    = { "Gears", "Transform stress" };

static int
cmp_double (const void *a, const void *b)
{
  double x = *(const double *) a, y = *(const double *) b;
  return (x > y) - (x < y);
}

static double
median_of (double *v, int n)
{
  if (n <= 0)
    return NAN;
  qsort (v, n, sizeof *v, cmp_double);
  return n & 1 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

/* point the sweep at its current combo (renderer + scene); the next ticks warm
 * up and then sample it. A scene-only change keeps the same renderer. */
static void
collect_apply_combo (GearsView *self)
{
  self->scene = self->collect_combo % 2;
  gears_view_set_backend (self, self->collect_combo / 2);  /* may fall back */
  self->collect_frames = 0;
  self->collect_n = 0;
}

static void
collect_render_table (GearsView *self)
{
  /* on-screen: a compact monospace grid */
  GString *ui = g_string_new ("<tt>            Gears    Transform\n");
  /* logcat: a markdown table ready to paste into a reply */
  GString *md = g_string_new ("\n| Renderer | Gears (ms/frame) | Transform stress (ms/frame) |\n"
                              "|---|---|---|\n");
  for (int r = 0; r < 3; r++)
    {
      g_string_append_printf (ui, "%-9s", RENDERER_NAMES[r]);
      g_string_append_printf (md, "| %s |", RENDERER_NAMES[r]);
      for (int s = 0; s < 2; s++)
        {
          double m = self->results[r][s];
          if (isnan (m))
            {
              g_string_append (ui, s == 0 ? "   n/a   " : "   n/a");
              g_string_append (md, " n/a |");
            }
          else
            {
              g_string_append_printf (ui, s == 0 ? " %6.2f  " : " %6.2f", m);
              g_string_append_printf (md, " %.2f (%.0f fps) |", m, 1000.0 / m);
            }
        }
      g_string_append_c (ui, '\n');
      g_string_append_c (md, '\n');
    }
  g_string_append (ui, "</tt>");
  if (self->table_label)
    gtk_label_set_markup (self->table_label, ui->str);
  g_message ("benchmark results (%d scenes/readback, BENCH %dpx):%s",
             BENCH_REPEAT, BENCH, md->str);
  g_string_free (ui, TRUE);
  g_string_free (md, TRUE);
}

static void
collect_finish (GearsView *self)
{
  self->collecting = FALSE;
  self->scene = self->saved_scene;
  gears_view_set_backend (self, self->saved_backend);
  if (self->scene_combo)
    adw_combo_row_set_selected (self->scene_combo, (guint) self->saved_scene);
  collect_render_table (self);
}

static gboolean
gears_view_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  GearsView *self = GEARS_VIEW (widget);

  if (self->backend < 0)
    gears_view_set_backend (self, 0);   /* prefer Vulkan; falls back to GL */

  /* Benchmark only: render the scene off-screen with the chosen renderer, force
   * it to complete (download), time it, then drop the texture immediately. The
   * on-screen display above is independent, so nothing is retained per frame. */
  if (self->renderer)
    {
      gboolean flat = renderer_backend (self->renderer) == 2;
      GtkSnapshot *s = gtk_snapshot_new ();
      /* gsk_renderer_render_texture() submits, waits on a fence, resolves the
       * tile buffer and downloads it on every call. That round-trip is a fixed
       * per-call cost (tens of ms on mobile tilers) independent of scene weight,
       * so a single light scene per readback makes every GPU renderer look as
       * slow as the readback and lets Cairo (no GPU round-trip) "win". Draw
       * BENCH_REPEAT scenes tiled in a grid into one node and read back once:
       * the grid avoids occlusion culling dropping stacked copies, so the GPU
       * really does REPEAT x the work (and REPEAT x the globals ops), and
       * dividing by REPEAT amortises the round-trip away. The number is then the
       * real per-frame render cost, comparable across renderers. */
      int cols = (int) ceil (sqrt ((double) BENCH_REPEAT));
      int rows = (BENCH_REPEAT + cols - 1) / cols;
      float cw = (float) BENCH / cols, ch = (float) BENCH / rows;
      for (int i = 0; i < BENCH_REPEAT; i++)
        {
          gtk_snapshot_save (s);
          gtk_snapshot_translate (s, &GRAPHENE_POINT_INIT ((i % cols) * cw, (i / cols) * ch));
          if (self->scene == 1)
            build_transform_stress (s, cw, ch, self->angle);
          else
            build_scene (s, cw, ch, self->gear, self->angle, self->density, self->blur, flat);
          gtk_snapshot_restore (s);
        }
      GskRenderNode *node = gtk_snapshot_free_to_node (s);
      if (node)
        {
          gint64 t0 = g_get_monotonic_time ();
          GdkTexture *tex = gsk_renderer_render_texture (self->renderer, node,
                                                         &GRAPHENE_RECT_INIT (0, 0, BENCH, BENCH));
          gint64 t1 = g_get_monotonic_time ();

          double ms = (t1 - t0) / 1000.0 / BENCH_REPEAT;
          self->render_ms = self->render_ms == 0 ? ms : self->render_ms * 0.85 + ms * 0.15;

          if (self->collecting)
            {
              if (++self->collect_frames > COLLECT_WARMUP && self->collect_n < COLLECT_SAMPLES)
                self->collect_samples[self->collect_n++] = ms;
              if (self->collect_n >= COLLECT_SAMPLES)
                {
                  int r = self->collect_combo / 2, s = self->collect_combo % 2;
                  self->results[r][s] = renderer_backend (self->renderer) == r
                                          ? median_of (self->collect_samples, self->collect_n)
                                          : NAN;   /* requested renderer fell back */
                  if (++self->collect_combo >= 6)
                    collect_finish (self);
                  else
                    collect_apply_combo (self);
                }
            }

          g_object_unref (tex);             /* measurement only - do not retain */
          gsk_render_node_unref (node);
        }
    }

  self->angle += 0.045;
  if (self->fps_label)
    {
      char *str;
      if (self->collecting)
        str = g_strdup_printf ("collecting %s / %s  ·  %d/%d",
                               RENDERER_NAMES[self->collect_combo / 2],
                               SCENE_NAMES[self->collect_combo % 2],
                               MIN (self->collect_n, COLLECT_SAMPLES), COLLECT_SAMPLES);
      else
        str = g_strdup_printf ("%.1f ms/frame  ·  %.0f FPS (uncapped)",
                               self->render_ms, self->render_ms > 0 ? 1000.0 / self->render_ms : 0);
      gtk_label_set_text (self->fps_label, str);
      g_free (str);
    }
  gtk_widget_queue_draw (widget);
  return G_SOURCE_CONTINUE;
}

static void
gears_view_dispose (GObject *o)
{
  GearsView *self = GEARS_VIEW (o);
  if (self->renderer)
    {
      gsk_renderer_unrealize (self->renderer);
      g_clear_object (&self->renderer);
    }
  g_clear_object (&self->gear);
  g_clear_object (&self->frame);
  g_clear_pointer (&self->dlbuf, g_free);
  G_OBJECT_CLASS (gears_view_parent_class)->dispose (o);
}

static void
gears_view_class_init (GearsViewClass *klass)
{
  GTK_WIDGET_CLASS (klass)->snapshot = gears_view_snapshot;
  G_OBJECT_CLASS (klass)->dispose = gears_view_dispose;
}

static void
gears_view_init (GearsView *self)
{
  GdkRGBA c = { 0.22f, 0.55f, 0.92f, 1.0f };
  self->gear = make_gear_texture (GEAR_PX, &c);
  self->backend = -1;
  self->density = 10;
  self->blur = TRUE;
  gtk_widget_add_tick_callback (GTK_WIDGET (self), gears_view_tick, NULL, NULL);
}

/* ---- UI --------------------------------------------------------------- */

static void
on_renderer_selected (AdwComboRow *row, GParamSpec *ps, gpointer data)
{
  gears_view_set_backend (GEARS_VIEW (data), (int) adw_combo_row_get_selected (row));
}

static void
on_scene_selected (AdwComboRow *row, GParamSpec *ps, gpointer data)
{
  GEARS_VIEW (data)->scene = (int) adw_combo_row_get_selected (row);
}

static void
on_blur_toggled (AdwSwitchRow *row, GParamSpec *ps, gpointer data)
{
  GEARS_VIEW (data)->blur = adw_switch_row_get_active (row);
}

static void
on_density (GtkAdjustment *adj, gpointer data)
{
  GEARS_VIEW (data)->density = (int) gtk_adjustment_get_value (adj);
}

static void
on_collect_clicked (GtkButton *btn, gpointer data)
{
  GearsView *self = GEARS_VIEW (data);
  if (self->collecting)
    return;
  self->saved_backend = self->backend < 0 ? 0 : self->backend;
  self->saved_scene = self->scene;
  for (int r = 0; r < 3; r++)
    for (int s = 0; s < 2; s++)
      self->results[r][s] = NAN;
  self->collect_combo = 0;
  self->collecting = TRUE;
  collect_apply_combo (self);
}

static void
activate (GtkApplication *app, gpointer data)
{
  GtkWidget *win = adw_application_window_new (app);
  gtk_window_set_title (GTK_WINDOW (win), "GTK renderer benchmark");
  gtk_window_set_default_size (GTK_WINDOW (win), 420, 760);

  GtkWidget *toolbar = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), adw_header_bar_new ());

  GtkWidget *gears = g_object_new (GEARS_TYPE_VIEW, "hexpand", TRUE, "vexpand", TRUE, NULL);

  GtkWidget *overlay = gtk_overlay_new ();
  gtk_widget_set_vexpand (overlay, TRUE);
  gtk_widget_set_overflow (overlay, GTK_OVERFLOW_HIDDEN);
  gtk_overlay_set_child (GTK_OVERLAY (overlay), gears);

  GtkWidget *info = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_halign (info, GTK_ALIGN_START);
  gtk_widget_set_valign (info, GTK_ALIGN_START);
  gtk_widget_set_margin_top (info, 12);
  gtk_widget_set_margin_start (info, 12);
  gtk_widget_add_css_class (info, "osd");
  GtkWidget *rlabel = gtk_label_new (NULL);
  GtkWidget *fps = gtk_label_new ("measuring…");
  gtk_widget_set_halign (rlabel, GTK_ALIGN_START);
  gtk_widget_set_halign (fps, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (info), rlabel);
  gtk_box_append (GTK_BOX (info), fps);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), info);
  GEARS_VIEW (gears)->fps_label = GTK_LABEL (fps);
  GEARS_VIEW (gears)->renderer_label = GTK_LABEL (rlabel);

  /* controls: HIG boxed list (its own card backing) */
  GtkWidget *list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (list, "boxed-list");

  GtkStringList *models = gtk_string_list_new ((const char *[]){ "Vulkan", "OpenGL", "Cairo", NULL });
  GtkWidget *rcombo = adw_combo_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (rcombo), "Renderer");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (rcombo), "GSK renderer used for the benchmark");
  adw_combo_row_set_model (ADW_COMBO_ROW (rcombo), G_LIST_MODEL (models));
  GEARS_VIEW (gears)->combo = ADW_COMBO_ROW (rcombo);
  g_signal_connect (rcombo, "notify::selected", G_CALLBACK (on_renderer_selected), gears);
  gtk_list_box_append (GTK_LIST_BOX (list), rcombo);

  GtkStringList *scenes = gtk_string_list_new ((const char *[]){ "Gears", "Transform stress", NULL });
  GtkWidget *scombo = adw_combo_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (scombo), "Scene");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (scombo), "Transform stress hammers the per-op globals path");
  adw_combo_row_set_model (ADW_COMBO_ROW (scombo), G_LIST_MODEL (scenes));
  GEARS_VIEW (gears)->scene_combo = ADW_COMBO_ROW (scombo);
  g_signal_connect (scombo, "notify::selected", G_CALLBACK (on_scene_selected), gears);
  gtk_list_box_append (GTK_LIST_BOX (list), scombo);

  GtkWidget *blur = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (blur), "Blur");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (blur), "GPU-heavy pass to amplify the renderer difference");
  adw_switch_row_set_active (ADW_SWITCH_ROW (blur), TRUE);
  g_signal_connect (blur, "notify::active", G_CALLBACK (on_blur_toggled), gears);
  gtk_list_box_append (GTK_LIST_BOX (list), blur);

  GtkWidget *dens = adw_spin_row_new_with_range (4, 32, 1);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (dens), "Gears per row");
  adw_spin_row_set_value (ADW_SPIN_ROW (dens), 10);
  g_signal_connect (adw_spin_row_get_adjustment (ADW_SPIN_ROW (dens)),
                    "value-changed", G_CALLBACK (on_density), gears);
  gtk_list_box_append (GTK_LIST_BOX (list), dens);

  GtkWidget *collect = gtk_button_new_with_label ("Collect all (renderer × scene)");
  gtk_widget_add_css_class (collect, "pill");
  gtk_widget_add_css_class (collect, "suggested-action");
  gtk_widget_set_margin_top (collect, 12);
  g_signal_connect (collect, "clicked", G_CALLBACK (on_collect_clicked), gears);

  GtkWidget *table = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (table), 0.0f);
  gtk_widget_set_margin_top (table, 8);
  gtk_label_set_markup (GTK_LABEL (table), "<tt>press Collect to sweep all renderers</tt>");
  GEARS_VIEW (gears)->table_label = GTK_LABEL (table);

  GtkWidget *col = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (col), list);
  gtk_box_append (GTK_BOX (col), collect);
  gtk_box_append (GTK_BOX (col), table);

  GtkWidget *clamp = adw_clamp_new ();
  adw_clamp_set_child (ADW_CLAMP (clamp), col);
  gtk_widget_set_margin_top (clamp, 12);
  gtk_widget_set_margin_bottom (clamp, 12);
  gtk_widget_set_margin_start (clamp, 12);
  gtk_widget_set_margin_end (clamp, 12);

  GtkWidget *panel = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (panel, "background");
  gtk_box_append (GTK_BOX (panel), clamp);

  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (content), overlay);
  gtk_box_append (GTK_BOX (content), panel);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), content);

  adw_application_window_set_content (ADW_APPLICATION_WINDOW (win), toolbar);
  gtk_window_present (GTK_WINDOW (win));
}

int
main (int argc, char **argv)
{
  AdwApplication *app = adw_application_new ("space.ampernic.GtkVulkanDemo",
                                             G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect (app, "activate", G_CALLBACK (activate), NULL);
  return g_application_run (G_APPLICATION (app), argc, argv);
}
