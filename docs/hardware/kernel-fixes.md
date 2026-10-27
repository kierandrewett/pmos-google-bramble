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
