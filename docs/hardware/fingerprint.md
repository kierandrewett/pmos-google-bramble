# Fingerprint

The reader is driven by Android's fingerprint HAL (`fps_hal`), which runs in the `bramble-android-hal` container. The phone's own libfprint has only the elan driver, and it does not drive this HAL, so stock fprintd cannot be used. The `bramble-fingerprint` package (`pmaports/device/downstream/bramble-fingerprint`) talks to the HAL over hwbinder. `bramble-fingerprint.sh` finds the container's logd, then runs the client in a private mount namespace where `/dev/hwbinder` is the container's binder node, so the host `/dev` is untouched. The templates are stored by the HAL in the container's `/data`; the client keeps its own state in `/var/lib/bramble-fingerprint` (root only).

Commands: `bramble-fingerprint status|enroll|verify|list|remove`. It re-runs itself with sudo. Enrol waits up to 180 s for the touches, and the other commands up to 60 s.

Enrolment needs an auth token from gatekeeper, signed with the key that
keymaster agrees on at boot (`computeSharedHmac`). On Android, keystore2
does that agreement; the container has no keystore2, so the
`bramble-fingerprint` OpenRC service runs it after the container starts,
then restarts `fps_hal`. The restart also matters on its own: an
`fps_hal` that opened its trusted app before `qseecomd` registered its
listeners rejects every enrol with `FPC_ERROR_INPUT`.

Enrolment also writes to the `persist` partition. The fingerprint trusted app keeps its secure storage there (`/mnt/vendor/persist`, writable on Android). `msm-firmware-loader` mounts `persist` read-only on the host, and the container used to get a read-only bind of that mount. With it read-only, the last touch of every enrol failed with `FPC_ERROR_INPUT`. `bramble-android.sh` now remounts the filesystem read-write, binds it read-write into the container, and sets the host mount back to read-only (mount flags are per mount, so the host stays read-only and the container is the only writer). `persist` also holds calibration data that is unique to the phone, so keep a copy of it ([recovery.md](../guides/recovery.md)).

Unlock. This Phosh build has no fingerprint code: the lock screen only asks PAM for the password, and `fprintd` cannot reach the HAL. So `bramble-fingerprint-unlock` (an OpenRC service, enabled by the package) watches logind. While the user session on seat0 is locked (`LockedHint`), it runs `bramble-fingerprint verify`. On a match it runs `loginctl unlock-session`, which Phosh follows, and it turns the screen on if the screen is off. The PIN is unchanged and always works. The service does not use PAM, so `sudo` and polkit prompts still ask for the password.

To turn the screen on, the service sets the `PowerSaveMode` property of `org.gnome.Mutter.DisplayConfig` on the user's session bus (through `setpriv`, because only that user can connect). Setting it to 0 does nothing once Phosh has blanked the screen itself, so the service sets 3 first and then 0. It does this only when the display reports `dpms Off`.

The HAL does not count wrong fingers; on Android the framework does. The service copies Android's limits: 5 wrong fingers stop the reader for 30 s, and after 20 wrong fingers in one lock only the PIN unlocks, until the phone is unlocked once. A watchdog stops `verify` as soon as the session is unlocked another way. The service writes to `/var/log/bramble-fingerprint-unlock.log` (root only). Turn it off with `sudo rc-service bramble-fingerprint-unlock stop` and `sudo rc-update del bramble-fingerprint-unlock default`.

Not verified: whether a touch wakes the phone from suspend. While the phone is awake with the screen off, the reader is active.
