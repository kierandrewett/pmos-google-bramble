# Display and GPU

- Display: Qualcomm SDE (`msm_drm`), DSI command-mode OLED, 1080x2340.
  The panel's `autorefresh` connector property must be 0. With it on,
  every atomic commit waits for the running transfer and for the
  autorefresh disable sequence, so each page flip took three TE periods:
  50 ms, 20 fps. `libfix_drm_formats.so` (an `LD_PRELOAD` shim in
  device-google-bramble) sets it and works around a few other quirks of
  the downstream DRM driver.
- GPU: Adreno 620 through the downstream KGSL driver (`/dev/kgsl-3d0`),
  not the upstream msm DRM driver. Alpine's Mesa only builds freedreno for
  msm, so `mesa-vulkan-freedreno-kgsl` builds Mesa's Vulkan driver (turnip)
  with the KGSL backend. OpenGL comes from Alpine's zink on top of turnip
  (`MESA_LOADER_DRIVER_OVERRIDE=zink`), and phoc uses its GLES2 renderer.
  - turnip on KGSL reports no DRM nodes, so zink and wlroots cannot match
    it to the display. A patch makes it report the display controller's
    nodes (`/dev/dri/card0`, `renderD128`).
  - The msm backend is built as well. With KGSL alone, meson drops libdrm,
    which compiles out the DRM image paths of the Vulkan WSI, and
    swapchain creation crashes.
  - `/dev/kgsl-3d0` and `/dev/ion` must be readable by the `video` group
    (udev rule); the greeter fails with `EGL_NOT_INITIALIZED` otherwise.
- Scale: `phoc.ini` sets `scale = 3.0`, a 360x780 logical screen. The panel
  is 1080 px wide, and a scale that does not divide it (2.75 did) leaves
  columns at the right edge that phoc never repaints, so old frames show
  there as a thin line. At 3.0 the Phosh app grid has 4 columns.
- Top bar: gmobile (Phosh's panel database) has an entry for
  `google,bramble-sm7250` (temp/gmobile), so Phosh keeps the status icons
  clear of the punch-hole camera and the rounded corners. Phosh starts the
  left icons 4 logical px after the cutout, so the entry lists the hole
  with a 56 px radius (the hardware hole is 40 px): the icons start about
  10 logical px after it. Change the radius in the patch to move them.
- Screen fade: Phosh and phoc turn the output off and on in one step, so
  the screen went black or lit up at once on the power button. This is
  upstream behaviour, not specific to this phone. temp/phoc carries a
  patch that reuses phoc's output shield to fade over 250 ms in both
  directions; it is written to be sent upstream.
- GPU clock: kgsl accepts only the vendor `msm-adreno-tz` governor, which
  does not raise the clock here, so on its own the GPU stays at 275 MHz.
- CPU: the kernel config makes schedutil the default cpufreq governor,
  as Android uses on this SoC (the imported config ran every cluster at
  its highest clock).
- Animation speed: a lock screen or greeter frame runs in sequence. phosh
  or phrog repaints the whole 1176x2550 window in software (GTK3,
  `hdy_carousel_box_draw`, 17-21 ms on a little core), phoc copies the
  12 MB SHM buffer into a texture, composites every layer on the GPU, and
  waits for the GPU before the next frame: about 46 ms, 20-30 fps. Android
  boosts its UI through schedtune; here no schedtune group was mounted, so
  everything ran on the little cores at 275 MHz GPU. `bramble-ui-boost`
  (OpenRC) mounts schedtune, keeps only phoc, phosh and phrog in a group
  with boost 50 (apps started from the shell are moved back out) and sets
  the GPU floor to 625 MHz (`min_pwrlevel` 0). Measured with injected
  swipes and the CRTC `total_framecount` in DRM debugfs: lock screen
  47-55 fps (21-23 without), greeter 55-60, app grid while dragging 57-60.
  The rest is upstream: phoc still draws layers hidden under the opaque
  lock screen and waits for the GPU twice per frame, and GTK3/libhandy
  repaint the whole window for every carousel frame.
