# Other kernel fixes

## polkit

polkit 127 resolves each caller through a pidfd and reads the PID from
`/proc/self/fdinfo/<fd>`. On this kernel that directory was mode 0500 and
owned by root while the task is not dumpable, so polkitd (running as its
own user) got `EACCES` and every check failed with "Process not found":
Settings and Phosh could not switch Wi-Fi off, for example. 0018 backports
the mainline fdinfo access rule (7bc3fa0172a4).

## Shared memory

GNOME's image loader (glycin) passes decoded images in memfds sealed
with `F_SEAL_WRITE` and maps them read-only and shared. 4.19 refuses any
shared mapping of a write-sealed memfd with `EPERM`, so Image Viewer
(Loupe) showed "Could not load image" for every file. 0024 backports the
mainline 6.7 rule: a read-only shared mapping of a write-sealed memfd is
allowed, and `VM_MAYWRITE` is cleared so it cannot be made writable.

## SO_BINDTODEVICE

Mainline lets a process bind a socket that is not bound yet to an interface
with `SO_BINDTODEVICE`, without `CAP_NET_RAW` (5.7, c427bfec18f2). Only a
change or a removal of an existing binding needs the capability. 4.19 refused
every call from an ordinary user with `EPERM`. `mmsd-tng` binds its DNS
queries and HTTP requests to the interface of the MMS bearer, so without the
patch the queries left on Wi-Fi and no picture message could be fetched or
sent. 0027 moves the capability check in `sock_bindtoindex_locked()` to the
case of an existing binding, as mainline does. Check as an ordinary user: the
first `setsockopt(SO_BINDTODEVICE)` on a new socket works, and a second one
with another interface name fails with `EPERM`.

## Console switch on suspend

Before it suspends, the kernel switches to a suspend console (tty63) and
switches back after the resume, unless every framebuffer sets
`fb_info->skip_vt_switch`. The msm framebuffer did not. With a Phosh session
this ended the session at every resume:

1. elogind pauses the DRM device on the switch away and resumes it on the
   switch back.
2. wlroots destroys every output when the seat is paused and adds `DSI-1`
   again on the resume, so phoc has a new output.
3. Phosh builds its whole UI again for the new output. phoc waits 3 s for
   the layer surfaces to answer ("Timeout (3000ms) expired with 2 configures
   pending"), and Phosh takes longer on this phone.
4. The late answer hits the assertion in
   `phoc_layout_transaction_notify_layer_configured`, phoc closes the
   connection ("error in client communication"), Phosh prints "Lost
   connection to Wayland compositor" and exits with code 1, gnome-session
   ends the session ("Unrecoverable failure in required component
   mobi.phosh.Shell.desktop"), and greetd starts a new greeter.

0028 sets `skip_vt_switch` for the msm framebuffer. The display driver does
not need the console switch: `sde_kms_pm_suspend()` keeps a copy of the
display state and `sde_kms_pm_resume()` commits it again. Android has no VT.
Check: lock the screen and run `rtcwake -m mem -s 60` as root. The Phosh PID
(`pgrep -f '^/usr/libexec/phosh$'`) and the session in `loginctl
list-sessions` stay the same, and `dmesg` shows no `dsi_backlight_early_dpms:
power_mode:0` after the resume, so the screen stays off.
