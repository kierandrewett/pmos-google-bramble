// SPDX-License-Identifier: GPL-3.0-or-later
//
// bramble-camera-pipewire: offer the Pixel 4a (5G) cameras as PipeWire
// camera nodes ("Back camera", "Ultrawide camera", "Front camera"), so camera
// apps such as GNOME Snapshot find them through the camera portal.
//
// The frames come from the vendor camera HAL in the Android HAL container
// (bramble-android-hal): bramble-camera-ndk serves them on a Unix socket and
// opens a camera only while a client is connected. This bridge connects only
// while a node is streaming, so the camera is off when no app uses it.
//
// The nodes offer viewfinder and video sizes (landscape, 4:3). Frames are
// passed through unrotated: each buffer carries a SPA_META_VideoTransform that
// tells the application how the sensor is mounted (rotated 90 degrees on the
// back cameras, 270 on the front). Frames are read from the socket straight
// into the PipeWire buffer, with no intermediate copy. The ultrawide is the
// logical rear camera zoomed out to its widest setting (zoom -1), which the
// HAL serves from the ultrawide sensor.
//
// Photos: the HAL takes full-resolution stills with its own processing and
// JPEG encoder in the streaming session. Apps ask for one on the control
// socket $XDG_RUNTIME_DIR/bramble-camera.sock with the line
// "capture <node-name> <rotation> <path>\n" (rotation: degrees the device is
// turned clockwise from portrait); the bridge writes the JPEG to <path> and
// answers "ok <width> <height>\n" or "error <reason>\n". The node must be
// streaming. Snapshot uses this through a patch in temp/snapshot.
//
// The bridge waits for PipeWire at start and starts over if the connection to
// PipeWire is lost, so it can be started before PipeWire.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>

#define SOCKET_PATH "/run/bramble-android/camera/camera.sock"
#define FRAME_MAGIC 0x46435242u /* "BRCF": NV12 frame */
#define STILL_MAGIC 0x4a435242u /* "BRCJ": JPEG still */
#define FPS 30
#define SCRATCH_BYTES (64 * 1024)
#define CONTROL_NAME "bramble-camera.sock"
#define STILL_TIMEOUT_S 10

#ifndef BACK_TRANSFORM
#define BACK_TRANSFORM SPA_META_TRANSFORMATION_90
#endif
#ifndef FRONT_TRANSFORM
#define FRONT_TRANSFORM SPA_META_TRANSFORMATION_270
#endif

// Viewfinder and video size offered to applications, landscape (sensor
// orientation, 4:3). Photos do not depend on it: they come from the HAL at
// full resolution. Measured with Snapshot: 1600x1200 keeps a 30 fps
// viewfinder and records at 30 fps (software H.264); 2048x1536 records at
// under 20 fps; at 1280x960 and 1440x1080 its viewfinder pipeline falls
// behind and stops taking frames. Up to 1080p area the HAL also guarantees a
// YUV stream next to a full-size JPEG output.
static const int offered[][2] = {
	{ 1600, 1200 },
};

struct camera {
	const char *name;
	const char *description;
	const char *id;        // Android camera id
	const char *zoom;      // zoom ratio requested from the HAL: "1.0", or "-1" for the widest
	const char *location;  // "back" or "front", for the viewfinder mirroring
	uint32_t transform;    // enum spa_meta_videotransform_value that makes the frame upright
	int max_w, max_h;      // largest sensor size (landscape) offered

	int w, h;              // frame size requested from the HAL and delivered (landscape)

	struct pw_main_loop *loop;
	struct pw_stream *stream;
	bool streaming;

	int fd;
	struct spa_source *io;
	struct spa_source *retry;
	uint8_t header[16];
	size_t header_got;
	struct pw_buffer *pending;  // PipeWire buffer the current frame is read into
	size_t payload;             // bytes of the current frame received so far
	uint8_t *scratch;           // discards frames when no PipeWire buffer is free
	uint64_t frame_count;
	uint64_t dropped;

	// Still capture in progress (control client waiting for the JPEG).
	int still_client;           // control connection, or -1
	char still_path[512];
	uint8_t *still;             // JPEG being received
	size_t still_size, still_got;
	bool receiving_still;       // the current socket frame is a JPEG
	struct spa_source *still_timer;
	struct spa_source *reconnect;
	struct spa_hook core_listener;
};

static void log_msg(const struct camera *c, const char *fmt, ...)
{
	va_list ap;
	fprintf(stderr, "[bramble-camera] %s: ", c->name);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static size_t frame_bytes(const struct camera *c)
{
	return (size_t)c->w * c->h * 3 / 2;
}

// Answer the control client waiting for a still and forget the capture.
static void finish_still(struct camera *c, const char *fmt, ...)
{
	struct timespec zero = { 0 };
	char reply[256];
	va_list ap;

	if (c->still_client < 0)
		return;
	va_start(ap, fmt);
	vsnprintf(reply, sizeof(reply) - 1, fmt, ap);
	va_end(ap);
	strcat(reply, "\n");
	if (write(c->still_client, reply, strlen(reply)) < 0)
		log_msg(c, "control client went away");
	close(c->still_client);
	c->still_client = -1;
	pw_loop_update_timer(pw_main_loop_get_loop(c->loop), c->still_timer, &zero, &zero, false);
}

// Write a received JPEG next to the requested path, then move it there, so
// the app never sees a partial file.
static void save_still(struct camera *c, uint32_t w, uint32_t h)
{
	char tmp[sizeof(c->still_path) + 8];
	int fd;
	bool ok = false;

	if (c->still_client < 0) {
		log_msg(c, "still without a request, dropped");
		return;
	}
	snprintf(tmp, sizeof(tmp), "%s.part", c->still_path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd >= 0) {
		ok = write(fd, c->still, c->still_size) == (ssize_t)c->still_size;
		ok = close(fd) == 0 && ok;
		ok = ok && rename(tmp, c->still_path) == 0;
		if (!ok)
			unlink(tmp);
	}
	if (ok) {
		log_msg(c, "photo %ux%u, %zu bytes: %s", w, h, c->still_size, c->still_path);
		finish_still(c, "ok %u %u", w, h);
	} else {
		log_msg(c, "cannot write %s: %s", c->still_path, strerror(errno));
		finish_still(c, "error cannot write the file");
	}
}

static void on_still_timeout(void *data, uint64_t expirations)
{
	struct camera *c = data;
	(void)expirations;
	log_msg(c, "no photo from the camera within %d s", STILL_TIMEOUT_S);
	finish_still(c, "error timeout");
}

static void disconnect_camera(struct camera *c)
{
	struct pw_loop *l = pw_main_loop_get_loop(c->loop);
	// A frame cut short stays in c->pending and is overwritten by the next
	// frame. It must not be queued: a buffer without a full picture (even
	// with an empty chunk) makes some consumers read past its data, which
	// crashes GTK's texture upload in Snapshot.
	if (c->io) {
		pw_loop_destroy_source(l, c->io);
		c->io = NULL;
	}
	if (c->fd >= 0) {
		close(c->fd);
		c->fd = -1;
		log_msg(c, "camera closed after %llu frames, %llu dropped",
			(unsigned long long)c->frame_count, (unsigned long long)c->dropped);
	}
	free(c->still);
	c->still = NULL;
	c->receiving_still = false;
	finish_still(c, "error camera closed");
	c->header_got = c->payload = 0;
}

static void on_socket(void *data, int fd, uint32_t mask);

// Hand a completed frame to the stream and wake the graph.
static void finish_frame(struct camera *c)
{
	struct pw_buffer *b = c->pending;
	struct spa_buffer *buf = b->buffer;
	struct spa_meta_header *h;
	struct spa_meta_videotransform *vt;
	struct timespec now;

	c->pending = NULL;
	buf->datas[0].chunk->offset = 0;
	buf->datas[0].chunk->size = frame_bytes(c);
	buf->datas[0].chunk->stride = c->w;
	if ((h = spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof(*h)))) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		h->pts = SPA_TIMESPEC_TO_NSEC(&now);
		h->flags = 0;
		h->seq = c->frame_count;
		h->dts_offset = 0;
	}
	if ((vt = spa_buffer_find_meta_data(buf, SPA_META_VideoTransform, sizeof(*vt))))
		vt->transform = c->transform;
	c->frame_count++;
	pw_stream_queue_buffer(c->stream, b);
	pw_stream_trigger_process(c->stream);
}

static void on_socket(void *data, int fd, uint32_t mask)
{
	struct camera *c = data;
	const size_t size = frame_bytes(c);
	ssize_t n = 0;

	if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
		log_msg(c, "camera service closed the connection");
		disconnect_camera(c);
		return;
	}
	for (;;) {
		if (c->header_got < sizeof(c->header)) {
			n = read(fd, c->header + c->header_got, sizeof(c->header) - c->header_got);
			if (n <= 0)
				break;
			c->header_got += n;
			if (c->header_got < sizeof(c->header))
				continue;
			uint32_t h[4];
			memcpy(h, c->header, sizeof(h));
			if (h[0] == STILL_MAGIC && h[3] > 0 && h[3] <= 64u * 1024 * 1024) {
				c->receiving_still = true;
				c->still_size = h[3];
				c->still_got = 0;
				free(c->still);
				c->still = malloc(c->still_size);
				if (!c->still) {
					log_msg(c, "no memory for a %u byte photo", h[3]);
					disconnect_camera(c);
					return;
				}
				continue;
			}
			if (h[0] != FRAME_MAGIC || h[1] != (uint32_t)c->w ||
			    h[2] != (uint32_t)c->h || h[3] != size) {
				log_msg(c, "unexpected frame %ux%u (%u bytes)", h[1], h[2], h[3]);
				disconnect_camera(c);
				return;
			}
			c->payload = 0;
			if (!c->pending)
				c->pending = pw_stream_dequeue_buffer(c->stream);
			if (!c->pending)
				c->dropped++;
		}

		if (c->receiving_still) {
			n = read(fd, c->still + c->still_got, c->still_size - c->still_got);
			if (n <= 0)
				break;
			c->still_got += n;
			if (c->still_got < c->still_size)
				continue;
			uint32_t h[4];
			memcpy(h, c->header, sizeof(h));
			save_still(c, h[1], h[2]);
			free(c->still);
			c->still = NULL;
			c->receiving_still = false;
			c->header_got = 0;
			continue;
		}

		const size_t left = size - c->payload;
		if (c->pending)
			n = read(fd, c->pending->buffer->datas[0].data + c->payload, left);
		else
			n = read(fd, c->scratch, left < SCRATCH_BYTES ? left : SCRATCH_BYTES);
		if (n <= 0)
			break;
		c->payload += n;
		if (c->payload < size)
			continue;

		if (c->pending)
			finish_frame(c);
		c->header_got = 0;
	}
	if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
		log_msg(c, "camera service connection lost");
		disconnect_camera(c);
	}
}

static bool connect_camera(struct camera *c)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	char request[64];
	int fd;

	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCKET_PATH);
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		if (fd >= 0)
			close(fd);
		return false;
	}
	snprintf(request, sizeof(request), "%s %d %d %s\n", c->id, c->w, c->h, c->zoom);
	if (write(fd, request, strlen(request)) != (ssize_t)strlen(request)) {
		close(fd);
		return false;
	}
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	c->fd = fd;
	c->header_got = c->payload = 0;
	c->frame_count = 0;
	c->dropped = 0;
	c->io = pw_loop_add_io(pw_main_loop_get_loop(c->loop), fd,
			       SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP, false, on_socket, c);
	log_msg(c, "camera opened (zoom %s, %dx%d)", c->zoom, c->w, c->h);
	return true;
}

static void on_retry(void *data, uint64_t expirations)
{
	(void)expirations;
	struct camera *c = data;
	struct timespec value = { 0 }, interval = { 0 };

	if (!c->streaming || c->fd >= 0 || connect_camera(c))
		pw_loop_update_timer(pw_main_loop_get_loop(c->loop), c->retry, &value, &interval, false);
}

// Connect after a short delay, then retry every second until connected.
// Apps often start streaming, stop and start again while they set up; the
// delay keeps that from opening the camera twice, which is slow (the
// camera service handles one client at a time, and the ultrawide takes
// seconds to open).
static void start_retry(struct camera *c)
{
	struct timespec value = { .tv_nsec = 300 * 1000 * 1000 }, interval = { .tv_sec = 1 };
	pw_loop_update_timer(pw_main_loop_get_loop(c->loop), c->retry, &value, &interval, false);
}

// Set when the connection to PipeWire is lost: main() then starts over.
static bool restart_wanted;

static void on_state_changed(void *data, enum pw_stream_state old, enum pw_stream_state state,
			     const char *error)
{
	struct camera *c = data;

	(void)old;
	c->streaming = state == PW_STREAM_STATE_STREAMING;
	if (c->streaming) {
		if (c->fd < 0)
			start_retry(c);
	} else {
		disconnect_camera(c);
	}
	if (error)
		log_msg(c, "stream error: %s", error);
	if (state == PW_STREAM_STATE_ERROR) {
		// For example a failed format negotiation with one app: offer the
		// camera again, after a moment, without touching the others.
		struct timespec value = { .tv_sec = 1 }, interval = { 0 };
		pw_loop_update_timer(pw_main_loop_get_loop(c->loop), c->reconnect, &value, &interval, false);
	}
}

static const struct spa_pod *enum_format(struct spa_pod_builder *b, const struct camera *c);
static int connect_stream(struct camera *c, const struct spa_pod **params);

static void drop_core_listener(struct camera *c)
{
	if (c->core_listener.link.next) {
		spa_hook_remove(&c->core_listener);
		spa_zero(c->core_listener);
	}
}

static void on_reconnect(void *data, uint64_t expirations)
{
	struct camera *c = data;
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const struct spa_pod *params[1];

	(void)expirations;
	log_msg(c, "offering the camera again");
	disconnect_camera(c);
	// A simple stream disconnects its core too, so drop the core listener.
	drop_core_listener(c);
	pw_stream_disconnect(c->stream);
	params[0] = enum_format(&b, c);
	if (connect_stream(c, params) < 0) {
		restart_wanted = true;
		pw_main_loop_quit(c->loop);
	}
}

// The connection to PipeWire is gone (PipeWire restarted): start over.
static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct camera *c = data;

	(void)seq;
	if (id != PW_ID_CORE || res != -EPIPE)
		return;
	log_msg(c, "PipeWire connection lost: %s", message);
	restart_wanted = true;
	pw_main_loop_quit(c->loop);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = on_core_error,
};

// Connect the stream; its core exists only from then on, so listen to it
// afterwards.
static int connect_stream(struct camera *c, const struct spa_pod **params)
{
	struct pw_core *core;
	int res;

	res = pw_stream_connect(c->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
				PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS, params, 1);
	if (res < 0)
		return res;
	if ((core = pw_stream_get_core(c->stream)) != NULL)
		pw_core_add_listener(core, &c->core_listener, &core_events, c);
	return 0;
}

static void on_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	struct camera *c = data;
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const struct spa_pod *params[3];
	struct spa_video_info_raw info;

	if (param == NULL || id != SPA_PARAM_Format)
		return;
	if (spa_format_video_raw_parse(param, &info) < 0)
		return;

	if ((int)info.size.width != c->w || (int)info.size.height != c->h) {
		disconnect_camera(c);
		c->w = info.size.width;
		c->h = info.size.height;
		if (c->streaming)
			start_retry(c);
	}
	log_msg(c, "format %ux%u", c->w, c->h);

	params[0] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		// The viewfinder sink, queues and a video encoder each hold
		// buffers while recording.
		SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 16),
		SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_size, SPA_POD_Int(frame_bytes(c)),
		SPA_PARAM_BUFFERS_stride, SPA_POD_Int(c->w),
		SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd)));
	params[1] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
	params[2] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoTransform),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_videotransform)));
	pw_stream_update_params(c->stream, params, 3);
}

// The frames are queued from on_socket; nothing to do when the graph runs.
static void on_process(void *data)
{
	(void)data;
}

// PipeWire frees the buffers on a format change; forget a held one.
static void on_remove_buffer(void *data, struct pw_buffer *b)
{
	struct camera *c = data;

	if (c->pending == b)
		c->pending = NULL;
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_state_changed,
	.param_changed = on_param_changed,
	.remove_buffer = on_remove_buffer,
	.process = on_process,
};

// EnumFormat: NV12 video at 30 fps, in each landscape size the camera offers.
// An enum choice is the default value followed by all allowed values, so the
// default (the largest size) appears twice. Without that, the default is not
// among the allowed values, and with a single size there are none at all.
static const struct spa_pod *enum_format(struct spa_pod_builder *b, const struct camera *c)
{
	struct spa_pod_frame obj, choice;
	int def = -1;

	for (size_t i = 0; i < SPA_N_ELEMENTS(offered); i++)
		if (offered[i][0] <= c->max_w && offered[i][1] <= c->max_h)
			def = i;

	spa_pod_builder_push_object(b, &obj, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	spa_pod_builder_prop(b, SPA_FORMAT_mediaType, 0);
	spa_pod_builder_id(b, SPA_MEDIA_TYPE_video);
	spa_pod_builder_prop(b, SPA_FORMAT_mediaSubtype, 0);
	spa_pod_builder_id(b, SPA_MEDIA_SUBTYPE_raw);
	spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_format, 0);
	spa_pod_builder_id(b, SPA_VIDEO_FORMAT_NV12);
	spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_size, 0);
	spa_pod_builder_push_choice(b, &choice, SPA_CHOICE_Enum, 0);
	spa_pod_builder_rectangle(b, offered[def][0], offered[def][1]);
	for (size_t i = 0; i < SPA_N_ELEMENTS(offered); i++) {
		if (offered[i][0] > c->max_w || offered[i][1] > c->max_h)
			continue;
		spa_pod_builder_rectangle(b, offered[i][0], offered[i][1]);
	}
	spa_pod_builder_pop(b, &choice);
	spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_framerate, 0);
	spa_pod_builder_fraction(b, FPS, 1);
	return spa_pod_builder_pop(b, &obj);
}

static int setup_camera(struct camera *c, struct pw_main_loop *loop)
{
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const struct spa_pod *params[1];
	struct pw_properties *props;

	c->loop = loop;
	c->fd = -1;
	c->still_client = -1;
	c->w = offered[0][0];
	c->h = offered[0][1];
	c->scratch = malloc(SCRATCH_BYTES);
	if (c->scratch == NULL)
		return -ENOMEM;
	c->retry = pw_loop_add_timer(pw_main_loop_get_loop(loop), on_retry, c);
	c->still_timer = pw_loop_add_timer(pw_main_loop_get_loop(loop), on_still_timeout, c);
	c->reconnect = pw_loop_add_timer(pw_main_loop_get_loop(loop), on_reconnect, c);

	props = pw_properties_new(
		PW_KEY_MEDIA_TYPE, "Video",
		PW_KEY_MEDIA_CATEGORY, "Capture",
		PW_KEY_MEDIA_CLASS, "Video/Source",
		PW_KEY_MEDIA_ROLE, "Camera",
		PW_KEY_NODE_NAME, c->name,
		PW_KEY_NODE_DESCRIPTION, c->description,
		PW_KEY_NODE_PAUSE_ON_IDLE, "true",
		// Camera apps (Snapshot) mirror the preview unless they know the
		// camera faces away from the user; the libcamera property is the
		// one they read.
		"api.libcamera.location", c->location,
		NULL);
	c->stream = pw_stream_new_simple(pw_main_loop_get_loop(loop), c->name, props, &stream_events, c);
	if (c->stream == NULL)
		return -errno;

	params[0] = enum_format(&b, c);
	return connect_stream(c, params);
}

static void free_camera(struct camera *c)
{
	struct pw_loop *l = pw_main_loop_get_loop(c->loop);

	disconnect_camera(c);
	if (c->stream) {
		drop_core_listener(c);
		pw_stream_destroy(c->stream);
	}
	if (c->retry)
		pw_loop_destroy_source(l, c->retry);
	if (c->still_timer)
		pw_loop_destroy_source(l, c->still_timer);
	if (c->reconnect)
		pw_loop_destroy_source(l, c->reconnect);
	free(c->scratch);
	c->stream = NULL;
	c->retry = c->still_timer = c->reconnect = NULL;
	c->scratch = NULL;
}

// Control socket: "capture <node-name> <rotation> <path>\n".
struct control {
	struct camera *cameras;
	size_t n_cameras;
	struct pw_main_loop *loop;
	int fd;
	struct spa_source *io;
};

static void handle_request(struct control *ctl, int client, char *line)
{
	char node[64], path[512];
	int rotation;
	struct camera *c = NULL;
	struct timespec value = { .tv_sec = STILL_TIMEOUT_S }, interval = { 0 };
	char cmd[32];

	if (sscanf(line, "capture %63s %d %511[^\n]", node, &rotation, path) != 3) {
		dprintf(client, "error bad request\n");
		close(client);
		return;
	}
	for (size_t i = 0; i < ctl->n_cameras; i++)
		if (!strcmp(ctl->cameras[i].name, node))
			c = &ctl->cameras[i];
	if (c == NULL || c->fd < 0 || path[0] != '/') {
		dprintf(client, "error %s\n", c == NULL ? "unknown camera" :
			c->fd < 0 ? "camera not streaming" : "path must be absolute");
		close(client);
		return;
	}
	if (c->still_client >= 0) {
		dprintf(client, "error busy\n");
		close(client);
		return;
	}
	rotation = ((rotation % 360) + 360) % 360;
	snprintf(cmd, sizeof(cmd), "capture %d\n", rotation);
	if (write(c->fd, cmd, strlen(cmd)) != (ssize_t)strlen(cmd)) {
		dprintf(client, "error camera service\n");
		close(client);
		return;
	}
	c->still_client = client;
	snprintf(c->still_path, sizeof(c->still_path), "%s", path);
	pw_loop_update_timer(pw_main_loop_get_loop(c->loop), c->still_timer, &value, &interval, false);
	log_msg(c, "photo requested (rotation %d)", rotation);
}

static void on_control(void *data, int fd, uint32_t mask)
{
	struct control *ctl = data;
	char line[640];
	size_t n = 0;
	int client;

	(void)mask;
	client = accept4(fd, NULL, NULL, SOCK_CLOEXEC);
	if (client < 0)
		return;
	// One short line from a local client; give it a second to arrive.
	struct timeval tv = { .tv_sec = 1 };
	setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	while (n + 1 < sizeof(line)) {
		ssize_t r = read(client, line + n, 1);
		if (r <= 0 || line[n] == '\n')
			break;
		n++;
	}
	line[n] = 0;
	handle_request(ctl, client, line);
}

static int setup_control(struct control *ctl)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	const char *dir = getenv("XDG_RUNTIME_DIR");

	ctl->fd = -1;
	if (dir == NULL)
		return -ENOENT;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/%s", dir, CONTROL_NAME);
	unlink(addr.sun_path);
	ctl->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (ctl->fd < 0 || bind(ctl->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    listen(ctl->fd, 4) < 0)
		return -errno;
	// XDG_RUNTIME_DIR is private to the user, which limits the socket too.
	ctl->io = pw_loop_add_io(pw_main_loop_get_loop(ctl->loop), ctl->fd, SPA_IO_IN, false,
				 on_control, ctl);
	return 0;
}

static void on_signal(void *data, int sig)
{
	(void)sig;
	pw_main_loop_quit(data);
}

// Offer the cameras until a signal arrives (returns 0) or the connection to
// PipeWire fails or is lost (returns -1).
static int run(void)
{
	struct camera cameras[] = {
		{ .name = "bramble-camera-back", .description = "Back camera", .id = "0",
		  .zoom = "1.0", .location = "back", .transform = BACK_TRANSFORM,
		  .max_w = 4032, .max_h = 3024 },
		{ .name = "bramble-camera-wide", .description = "Ultrawide camera", .id = "0",
		  .zoom = "-1", .location = "back", .transform = BACK_TRANSFORM,
		  .max_w = 4032, .max_h = 3024 },
		{ .name = "bramble-camera-front", .description = "Front camera", .id = "1",
		  .zoom = "1.0", .location = "front", .transform = FRONT_TRANSFORM,
		  .max_w = 3264, .max_h = 2448 },
	};
	struct control ctl = { .cameras = cameras, .n_cameras = SPA_N_ELEMENTS(cameras), .fd = -1 };
	struct pw_main_loop *loop;
	int res = 0;

	loop = pw_main_loop_new(NULL);
	if (loop == NULL)
		return -1;
	ctl.loop = loop;
	pw_loop_add_signal(pw_main_loop_get_loop(loop), SIGINT, on_signal, loop);
	pw_loop_add_signal(pw_main_loop_get_loop(loop), SIGTERM, on_signal, loop);

	restart_wanted = false;
	for (size_t i = 0; i < SPA_N_ELEMENTS(cameras); i++) {
		if ((res = setup_camera(&cameras[i], loop)) < 0) {
			fprintf(stderr, "[bramble-camera] %s: cannot create stream: %s\n",
				cameras[i].name, strerror(-res));
			restart_wanted = true;
			break;
		}
	}
	if (!restart_wanted) {
		if ((res = setup_control(&ctl)) < 0)
			fprintf(stderr, "[bramble-camera] no control socket, photos come from the app: %s\n",
				strerror(-res));
		fprintf(stderr, "[bramble-camera] cameras offered\n");
		pw_main_loop_run(loop);
	}

	for (size_t i = 0; i < SPA_N_ELEMENTS(cameras); i++)
		if (cameras[i].loop)
			free_camera(&cameras[i]);
	if (ctl.io)
		pw_loop_destroy_source(pw_main_loop_get_loop(loop), ctl.io);
	if (ctl.fd >= 0)
		close(ctl.fd);
	pw_main_loop_destroy(loop);
	return restart_wanted ? -1 : 0;
}

int main(int argc, char *argv[])
{
	pw_init(&argc, &argv);
	// gnome-session may start this before PipeWire, and PipeWire may
	// restart: try again every two seconds.
	while (run() < 0) {
		fprintf(stderr, "[bramble-camera] no PipeWire connection, trying again\n");
		sleep(2);
	}
	pw_deinit();
	return 0;
}
