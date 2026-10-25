"""Save one frame of the first monitor of the GNOME Shell on the session bus.

usage: shot.py OUT.png

Uses mutter's ScreenCast D-Bus API and a PipeWire stream, because the Shell's
own screenshot API only answers to a few allowed callers.
"""
import subprocess
import sys

from gi.repository import Gio, GLib

out = sys.argv[1]
bus = Gio.bus_get_sync(Gio.BusType.SESSION)


def call(path, iface, method, args, sig):
    reply = bus.call_sync(
        "org.gnome.Mutter.ScreenCast", path, iface, method, GLib.Variant(sig, args),
        None, Gio.DBusCallFlags.NONE, 20000, None,
    )
    return reply.unpack()


session = call("/org/gnome/Mutter/ScreenCast", "org.gnome.Mutter.ScreenCast", "CreateSession", ({},), "(a{sv})")[0]
stream = call(session, "org.gnome.Mutter.ScreenCast.Session", "RecordMonitor", ("Meta-0", {}), "(sa{sv})")[0]

nodes = []
loop = GLib.MainLoop()


def on_added(conn, sender, path, iface, name, params, data):
    nodes.append(params.unpack()[0])
    loop.quit()


bus.signal_subscribe(
    None, "org.gnome.Mutter.ScreenCast.Stream", "PipeWireStreamAdded", stream, None,
    Gio.DBusSignalFlags.NONE, on_added, None,
)
call(session, "org.gnome.Mutter.ScreenCast.Session", "Start", (), "()")
GLib.timeout_add_seconds(10, loop.quit)
loop.run()

if not nodes:
    sys.exit("no PipeWire stream from the Shell")
subprocess.run(
    ["gst-launch-1.0", "-q", "pipewiresrc", "path=%d" % nodes[0], "num-buffers=15", "!", "videoconvert", "!",
     "pngenc", "snapshot=true", "!", "filesink", "location=" + out],
    check=True, timeout=60,
)
call(session, "org.gnome.Mutter.ScreenCast.Session", "Stop", (), "()")
