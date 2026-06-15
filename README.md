# GTK Vulkan / GL demo (incl. Android)

A small GTK4 / libadwaita app that makes the active GSK renderer visible and
comparable: it shows whether GTK is rendering through **Vulkan** or **OpenGL**,
runs a GPU-stressing animated scene (a field of meshing, blurred gears) and a
live **FPS** counter, and lets you flip between the two renderers at runtime.

It exists to demonstrate the **gdk-android Vulkan backend** - see
[Android](#android) - but it runs on any GTK 4 platform.

![screenshot](doc/screenshot.png)

## What it shows

- **Renderer label** - the live `GskRenderer` type (`Vulkan` / `OpenGL` /
  `Cairo`), read from the toplevel.
- **Vulkan switch** - relaunches the process under `GSK_RENDERER=vulkan` or
  `gl` (re-exec of `/proc/self/exe`), so you can A/B the same scene.
- **Blur (GPU stress)** - wraps the scene in a GSK blur pass; the cheapest way
  to make the renderer difference show up in the FPS counter.
- **Gears** - density of the scene (more gears = more draw load).

## Build & run (desktop)

```sh
meson setup build
meson compile -C build
./build/gtk-vulkan-demo
```

Flip the **Vulkan** switch (it relaunches) and watch the renderer label and FPS
under load. You can also force a renderer from the shell:

```sh
GSK_RENDERER=vulkan ./build/gtk-vulkan-demo
GSK_RENDERER=gl     ./build/gtk-vulkan-demo
```

## Android

Upstream GTK has **no Vulkan backend for `gdk-android`** - it only ships a GL
context, so GTK always falls back to the GL renderer on Android, even on
Vulkan-capable hardware. This demo runs on Android **with** the patches that add
one:

- a `GdkAndroidVulkanContext` (`vkCreateAndroidSurfaceKHR` from the surface's
  `ANativeWindow`) + `VK_KHR_android_surface`,
- runtime resolution of the Vulkan 1.1 entry points (via
  `vkGet{Instance,Device}ProcAddr`) so the renderer links on `minSdk < 28`,
- making Vulkan a *primary* renderer on Android (it was Wayland-only).

The patches and the branch they live on are linked from the project that drove
this work. Built with those patches, the demo reports `Renderer: Vulkan` on an
API-28+ device (verified on a Samsung A73 / Adreno) and falls back to GL on
API 26–27.

## License

LGPL-3.0-or-later.
