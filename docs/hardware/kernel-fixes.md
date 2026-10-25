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
