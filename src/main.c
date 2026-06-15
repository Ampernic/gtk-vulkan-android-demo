/*
 * gtk-vulkan-android-demo - show the active GSK renderer (Vulkan vs GL), a
 * GPU-stressing animated scene and a live FPS counter, with a toggle that
 * relaunches under the other renderer. Built to run on the desktop and, with
 * the gdk-android Vulkan patches, on Android.
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <adwaita.h>
#include <math.h>

/* ---- a single cog/gear drawn once into a texture ---------------------- */

static GdkTexture *
make_gear_texture (double size, const GdkRGBA *color)
{
  int teeth = 12;
  double r_out = size * 0.5, r_in = r_out * 0.72, r_hole = r_out * 0.28;
  cairo_surface_t *cs = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, (int) size, (int) size);
  cairo_t *cr = cairo_create (cs);
  cairo_translate (cr, size / 2, size / 2);

  for (int i = 0; i < teeth * 2; i++)
    {
      double a = G_PI * i / teeth;
      double r = (i % 2 == 0) ? r_out : r_in;
      if (i == 0)
        cairo_move_to (cr, r * cos (a), r * sin (a));
      else
        cairo_line_to (cr, r * cos (a), r * sin (a));
    }
  cairo_close_path (cr);
  cairo_set_source_rgba (cr, color->red, color->green, color->blue, color->alpha);
  cairo_fill_preserve (cr);
  cairo_set_source_rgba (cr, 0, 0, 0, 0.35);
  cairo_set_line_width (cr, size * 0.02);
  cairo_stroke (cr);

  cairo_arc (cr, 0, 0, r_hole, 0, 2 * G_PI);
  cairo_set_operator (cr, CAIRO_OPERATOR_CLEAR);
  cairo_fill (cr);

  cairo_destroy (cr);
  cairo_surface_flush (cs);
  int wpx = cairo_image_surface_get_width (cs);
  int hpx = cairo_image_surface_get_height (cs);
  int stride = cairo_image_surface_get_stride (cs);
  /* cairo ARGB32 is premultiplied BGRA in memory on little-endian. */
  GBytes *bytes = g_bytes_new (cairo_image_surface_get_data (cs), (gsize) stride * hpx);
  GdkTexture *tex = gdk_memory_texture_new (wpx, hpx,
                                            GDK_MEMORY_B8G8R8A8_PREMULTIPLIED,
                                            bytes, stride);
  g_bytes_unref (bytes);
  cairo_surface_destroy (cs);
  return tex;
}

/* ---- the animated, GPU-stressing gears widget ------------------------- */

#define GEARS_TYPE_VIEW (gears_view_get_type ())
G_DECLARE_FINAL_TYPE (GearsView, gears_view, GEARS, VIEW, GtkWidget)

struct _GearsView
{
  GtkWidget parent_instance;
  GdkTexture *gear;
  double      angle;        /* radians */
  int         density;      /* gears per row */
  gboolean    blur;         /* GPU-heavy blur pass */
  gint64      last_us;
  double      fps;
  GtkLabel   *fps_label;
};
G_DEFINE_FINAL_TYPE (GearsView, gears_view, GTK_TYPE_WIDGET)

#define GEAR_PX 96.0

static void
gears_view_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  GearsView *self = GEARS_VIEW (widget);
  int w = gtk_widget_get_width (widget), h = gtk_widget_get_height (widget);
  int cols = self->density;
  double cell = (double) w / cols;
  int rows = (int) ceil ((double) h / cell) + 1;

  if (self->blur)
    gtk_snapshot_push_blur (snapshot, 6.0);

  for (int row = 0; row < rows; row++)
    {
      for (int col = 0; col < cols; col++)
        {
          double cx = (col + 0.5) * cell;
          double cy = (row + 0.5) * cell;
          /* neighbouring gears mesh: alternate spin direction */
          double dir = ((row + col) % 2 == 0) ? 1.0 : -1.0;

          gtk_snapshot_save (snapshot);
          gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (cx, cy));
          gtk_snapshot_rotate (snapshot, (float) (dir * self->angle * 180.0 / G_PI));
          gtk_snapshot_append_texture (snapshot, self->gear,
                                       &GRAPHENE_RECT_INIT (-cell * 0.45f, -cell * 0.45f,
                                                            cell * 0.9f, cell * 0.9f));
          gtk_snapshot_restore (snapshot);
        }
    }

  if (self->blur)
    gtk_snapshot_pop (snapshot);
}

static gboolean
gears_view_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  GearsView *self = GEARS_VIEW (widget);
  gint64 now = gdk_frame_clock_get_frame_time (clock);

  if (self->last_us != 0)
    {
      double dt = (now - self->last_us) / 1e6;
      if (dt > 0)
        {
          double inst = 1.0 / dt;
          self->fps = self->fps == 0 ? inst : self->fps * 0.9 + inst * 0.1;
        }
      self->angle += dt * 0.8;            /* ~0.13 rev/s */
    }
  self->last_us = now;

  if (self->fps_label)
    {
      char *s = g_strdup_printf ("%.0f FPS", self->fps);
      gtk_label_set_text (self->fps_label, s);
      g_free (s);
    }
  gtk_widget_queue_draw (widget);
  return G_SOURCE_CONTINUE;
}

static void
gears_view_dispose (GObject *o)
{
  g_clear_object (&GEARS_VIEW (o)->gear);
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
  GdkRGBA c = { 0.20f, 0.52f, 0.89f, 1.0f };
  self->gear = make_gear_texture (GEAR_PX, &c);
  self->density = 8;
  self->blur = TRUE;
  gtk_widget_add_tick_callback (GTK_WIDGET (self), gears_view_tick, NULL, NULL);
}

/* ---- relaunch under a chosen GSK renderer ----------------------------- */

static char **saved_argv;

static void
relaunch_with_renderer (const char *renderer)
{
  g_setenv ("GSK_RENDERER", renderer, TRUE);
  /* re-exec self; the modified environ is inherited */
  execv ("/proc/self/exe", saved_argv);
  g_warning ("re-exec failed: %s", g_strerror (errno));
}

/* ---- UI --------------------------------------------------------------- */

static const char *
active_renderer_name (GtkWidget *window)
{
  GskRenderer *r = gtk_native_get_renderer (GTK_NATIVE (window));
  const char *t = r ? G_OBJECT_TYPE_NAME (r) : "?";
  if (strstr (t, "Vulkan")) return "Vulkan";
  if (strstr (t, "Ngl") || strstr (t, "GL")) return "OpenGL";
  if (strstr (t, "Cairo")) return "Cairo (software)";
  return t;
}

static void
on_vulkan_toggled (AdwSwitchRow *row, GParamSpec *ps, gpointer data)
{
  GtkWidget *root = GTK_WIDGET (gtk_widget_get_root (GTK_WIDGET (row)));
  gboolean want_vulkan = adw_switch_row_get_active (row);
  gboolean is_vulkan = g_strcmp0 (active_renderer_name (root), "Vulkan") == 0;
  /* ignore the programmatic sync in on_map; only relaunch on a real change */
  if (want_vulkan == is_vulkan)
    return;
  relaunch_with_renderer (want_vulkan ? "vulkan" : "gl");
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
on_map (GtkWidget *window, gpointer label)
{
  const char *r = active_renderer_name (window);
  char *s = g_strdup_printf ("Renderer: <b>%s</b>", r);
  gtk_label_set_markup (GTK_LABEL (label), s);
  g_free (s);

  /* now that the renderer exists, reflect it in the toggle (no-op relaunch guarded) */
  GtkWidget *vk = g_object_get_data (G_OBJECT (window), "vkswitch");
  if (vk)
    adw_switch_row_set_active (ADW_SWITCH_ROW (vk), g_strcmp0 (r, "Vulkan") == 0);
}

static void
activate (GtkApplication *app, gpointer data)
{
  GtkWidget *win = adw_application_window_new (app);
  gtk_window_set_title (GTK_WINDOW (win), "GTK Vulkan / GL demo");
  /* phone-sized default so the adaptive layout is obvious; resizes freely */
  gtk_window_set_default_size (GTK_WINDOW (win), 420, 740);

  GtkWidget *toolbar = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), adw_header_bar_new ());

  /* --- graphics preview, fills the window --- */
  GtkWidget *gears = g_object_new (GEARS_TYPE_VIEW, "hexpand", TRUE, "vexpand", TRUE, NULL);

  GtkWidget *overlay = gtk_overlay_new ();
  gtk_widget_set_vexpand (overlay, TRUE);
  /* clip the animated scene to the preview area so it never bleeds onto the UI */
  gtk_widget_set_overflow (overlay, GTK_OVERFLOW_HIDDEN);
  gtk_overlay_set_child (GTK_OVERLAY (overlay), gears);

  /* renderer + FPS as an OSD pill (top-start), per the HIG overlay style */
  GtkWidget *info = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_halign (info, GTK_ALIGN_START);
  gtk_widget_set_valign (info, GTK_ALIGN_START);
  gtk_widget_set_margin_top (info, 12);
  gtk_widget_set_margin_start (info, 12);
  gtk_widget_add_css_class (info, "osd");
  GtkWidget *rlabel = gtk_label_new (NULL);
  GtkWidget *fps = gtk_label_new ("… FPS");
  gtk_widget_set_halign (rlabel, GTK_ALIGN_START);
  gtk_widget_set_halign (fps, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (info), rlabel);
  gtk_box_append (GTK_BOX (info), fps);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), info);
  GEARS_VIEW (gears)->fps_label = GTK_LABEL (fps);

  /* --- controls: HIG "boxed list" (gives the rows their card backing),
   *     full-width and adaptive --- */
  GtkWidget *list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (list, "boxed-list");

  GtkWidget *vk = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (vk), "Vulkan renderer");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (vk), "Relaunches the app under the chosen renderer");
  adw_switch_row_set_active (ADW_SWITCH_ROW (vk),
                             g_strcmp0 (active_renderer_name (win), "Vulkan") == 0);
  g_signal_connect (vk, "notify::active", G_CALLBACK (on_vulkan_toggled), NULL);
  g_object_set_data (G_OBJECT (win), "vkswitch", vk);
  gtk_list_box_append (GTK_LIST_BOX (list), vk);

  GtkWidget *blur = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (blur), "Blur");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (blur), "GPU-heavy pass to surface the renderer difference");
  adw_switch_row_set_active (ADW_SWITCH_ROW (blur), TRUE);
  g_signal_connect (blur, "notify::active", G_CALLBACK (on_blur_toggled), gears);
  gtk_list_box_append (GTK_LIST_BOX (list), blur);

  GtkWidget *dens = adw_spin_row_new_with_range (3, 24, 1);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (dens), "Gears per row");
  adw_spin_row_set_value (ADW_SPIN_ROW (dens), 8);
  g_signal_connect (adw_spin_row_get_adjustment (ADW_SPIN_ROW (dens)),
                    "value-changed", G_CALLBACK (on_density), gears);
  gtk_list_box_append (GTK_LIST_BOX (list), dens);

  GtkWidget *clamp = adw_clamp_new ();
  adw_clamp_set_child (ADW_CLAMP (clamp), list);
  gtk_widget_set_margin_top (clamp, 12);
  gtk_widget_set_margin_bottom (clamp, 12);
  gtk_widget_set_margin_start (clamp, 12);
  gtk_widget_set_margin_end (clamp, 12);

  /* opaque backing panel for the settings, so the preview never shows through */
  GtkWidget *panel = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (panel, "background");
  gtk_box_append (GTK_BOX (panel), clamp);

  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (content), overlay);
  gtk_box_append (GTK_BOX (content), panel);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), content);

  adw_application_window_set_content (ADW_APPLICATION_WINDOW (win), toolbar);

  g_signal_connect (win, "map", G_CALLBACK (on_map), rlabel);
  gtk_window_present (GTK_WINDOW (win));
}

int
main (int argc, char **argv)
{
  saved_argv = argv;
  AdwApplication *app = adw_application_new ("space.ampernic.GtkVulkanDemo",
                                             G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect (app, "activate", G_CALLBACK (activate), NULL);
  return g_application_run (G_APPLICATION (app), argc, argv);
}
