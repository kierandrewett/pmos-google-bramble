# Experiment: stock GNOME Shell instead of Phosh

Question: can the phone run stock GNOME Shell (not Phosh)? This page
records what was checked, what is known to depend on Phosh, and what is
still open. The port was built for Phosh. Nothing in it was prepared for
GNOME Shell, and Phosh is still the only supported shell.

## Result so far

Stock GNOME Shell 51.0 (mutter 51.0) starts on the phone's graphics stack
and draws its Overview. The check ran the Shell headless, so nothing was
drawn on the real screen and Phosh kept running:

- mutter picks `/dev/dri/renderD128` (`msm_drm`), creates a GBM renderer on
  it and uses it as the primary GPU. That GPU path is Mesa zink on turnip
  over KGSL, the same one phoc uses.
- The Shell finishes its start-up and renders the Overview at 360x780, the
  same size as the phoc scale 3.0 screen.
- A frame of that monitor was saved through mutter's ScreenCast API and
  PipeWire. Repeat the check with `tools/gnome-shell-probe/probe.sh`.

It is not the full result. Nothing of this was tried on the real display.

| Checked | Result |
|---|---|
| Packages | Alpine edge has `gnome-shell` 51.0, `mutter` 51.0 and `gnome-session` 48. Adding them to a Phosh system installs 13 packages (29 MiB) and conflicts with nothing. `gnome-shell-mobile` 48 (the GNOME mobile fork) conflicts with Phosh at package level (`mutter-mobile-schemas` breaks `postmarketos-ui-phosh`), so the two cannot be installed together. |
| Start-up | Needs `Gdm-1.0.typelib` and the `org.gnome.login-screen` schema, which only the `gdm` package ships. The probe unpacks `gdm` in a temporary directory, because installing it also installs `wireplumber-logind`. |
| GPU | mutter and the Shell use zink over turnip. The log has one zink performance warning ("> 100 copy boxes") and no crash. |
| Network indicator | JS error `this._mobileDevice is undefined` in `status/network.js` for the modem. Not understood yet. |
| Sensors | `Failed to claim accelerometer: Sensor claim not allowed`. The probe runs on its own D-Bus, outside the logind session, so this may only be a polkit result of the probe. Not checked. |
| Power profiles | `power-profiles-daemon` is not installed, so the Shell logs an error for it. |

## Not tried yet

These need the real display, so they need the Shell to take the screen
from Phosh:

- Scan-out through KMS on the downstream DRM driver. phoc works because of
  `libfix_drm_formats.so` (an `LD_PRELOAD` shim) and the `WLR_*` variables
  in `/etc/profile.d/bramble-display.sh`. mutter has not been run with
  them. The hardware cursor plane probably needs to be off
  (`MUTTER_DEBUG_DISABLE_HW_CURSORS=1`; not tested).
- Touch, gestures, the on-screen keyboard, the lock screen, rotation.
- Whether `KEY_POWER` from tap to wake turns the screen on, or suspends the
  phone (GNOME's default power button action is suspend).
- Fractional scale. phoc takes `scale = 3.0` from `phoc.ini`. mutter picks
  its own scale, and fractional scale is an experimental setting there.
- Frame rate. Phosh needs `bramble-ui-boost` to reach 47-60 fps.

## What depends on Phosh

| Part | Dependency | With GNOME Shell |
|---|---|---|
| `temp/scrcpy-server` | Captures the screen with wlr-screencopy, which phoc has and mutter does not. | Would send no frames. It needs a ScreenCast or RemoteDesktop backend. |
| `bramble-fingerprint-unlock` | Finds the `phosh` process to get the user's D-Bus address, then sets `PowerSaveMode` on `org.gnome.Mutter.DisplayConfig`. | The D-Bus interface is mutter's own, so it should work once the process name is changed. The unlock uses logind `LockedHint`, which GNOME Shell also sets (not checked). |
| `bramble-ui-boost` | Puts `phoc`, `phosh` and `phrog` in the schedtune boost group. | Add `gnome-shell`. |
| `temp/phoc` | Screen fade on power-save. | Not available. mutter blanks the screen at once. |
| `temp/gmobile` | Panel data (rounded corners, punch-hole) used by Phosh's top bar. | GNOME Shell does not read it. The status bar would sit under the punch-hole. |
| `device-google-bramble` `phoc.ini` | Scale 3.0. | Not used. |
| Torch, haptics | Phosh quick setting; feedbackd. | Stock GNOME Shell has neither. |
| `bramble-camera-pipewire` | Autostart entry with `X-GNOME-Autostart-Phase`. | Works in any `gnome-session`. |
| Sensors, camera, Wi-Fi, modem, Bluetooth, audio | `iio-sensor-proxy`, PipeWire, NetworkManager, ModemManager. | No Phosh code. mutter and GNOME Settings use the same D-Bus services. |

## Next step

Run the Shell on the real display from a second virtual terminal, so that
greetd and Phosh stay as they are and `chvt 7` brings Phosh back. It takes
the screen away from Phosh while the test runs, so ask before doing it.
Order of checks: scan-out, touch, power key, lock screen, rotation, then
scrcpy and the fingerprint unlock.
