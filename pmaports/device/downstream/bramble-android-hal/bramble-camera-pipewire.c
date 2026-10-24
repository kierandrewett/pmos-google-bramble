// SPDX-License-Identifier: GPL-3.0-or-later
//
// bramble-camera-pipewire: offer the Pixel 4a (5G) cameras as PipeWire
// camera nodes ("Back camera", "Front camera"), so camera apps such as GNOME
// Snapshot find them through the camera portal.
//
// The frames come from the vendor camera HAL in the Android HAL container
// (bramble-android-hal): bramble-camera-ndk serves them on a Unix socket and
// opens a camera only while a client is connected. This bridge connects only
// while a node is streaming, so the camera is off when no app uses it.
// Frames arrive in sensor orientation (landscape) and are rotated to
// portrait here.

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
#include <spa/param/video/format-utils.h>
#include <spa/param/buffers.h>
#include <spa/buffer/meta.h>
#include <spa/pod/builder.h>

#define SOCKET_PATH "/run/bramble-android/camera/camera.sock"
#define FRAME_MAGIC 0x46435242u /* "BRCF" */

// Size requested from the HAL, in sensor orientation (4:3).
#define SENSOR_W 1440
#define SENSOR_H 1080
#define FPS 30

struct camera {
	const char *name;
	const char *description;
	const char *id;     // Android camera id
	const char *location; // "back" or "front", for the viewfinder mirroring
	int rotation;       // clockwise rotation to portrait: 90 or 270
	int out_w, out_h;   // after rotation

	struct pw_main_loop *loop;
	struct pw_stream *stream;
	struct spa_hook listener;
	bool streaming;

	int fd;
	struct spa_source *io;
	struct spa_source *retry;
	uint8_t header[16];
	size_t header_got;
	uint8_t *raw;       // frame being received (sensor orientation)
	size_t raw_size, raw_got;
	uint8_t *frame;     // latest complete frame, rotated
	bool frame_ready;
	uint64_t frame_count;
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

// Rotate an NV12 frame (w x h) clockwise by 90 or 270 degrees into dst
// (h x w). The chroma plane holds interleaved UV pairs at half resolution.
static void rotate_nv12(const uint8_t *src, uint8_t *dst, int w, int h, int rotation)
{
	const int dw = h, dh = w;
	for (int dy = 0; dy < dh; dy++) {
		uint8_t *row = dst + (size_t)dy * dw;
		if (rotation == 90) {
			for (int dx = 0; dx < dw; dx++)
				row[dx] = src[(size_t)(h - 1 - dx) * w + dy];
		} else {
			for (int dx = 0; dx < dw; dx++)
				row[dx] = src[(size_t)dx * w + (w - 1 - dy)];
		}
	}
	const uint8_t *suv = src + (size_t)w * h;
	uint8_t *duv = dst + (size_t)dw * dh;
	const int cw = w / 2, ch = h / 2, dcw = dw / 2, dch = dh / 2;
	for (int dy = 0; dy < dch; dy++) {
		uint8_t *row = duv + (size_t)dy * dw;
		for (int dx = 0; dx < dcw; dx++) {
			const uint8_t *p;
			if (rotation == 90)
				p = suv + (size_t)(ch - 1 - dx) * w + 2 * dy;
			else
				p = suv + (size_t)dx * w + 2 * (cw - 1 - dy);
			row[2 * dx] = p[0];
			row[2 * dx + 1] = p[1];
		}
	}
}

static void disconnect_camera(struct camera *c)
{
	struct pw_loop *l = pw_main_loop_get_loop(c->loop);
	if (c->io) {
		pw_loop_destroy_source(l, c->io);
		c->io = NULL;
	}
	if (c->fd >= 0) {
		close(c->fd);
		c->fd = -1;
		log_msg(c, "camera closed after %llu frames", (unsigned long long)c->frame_count);
	}
	c->header_got = c->raw_got = 0;
}

static void on_socket(void *data, int fd, uint32_t mask)
{
	struct camera *c = data;

	if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
		log_msg(c, "camera service closed the connection");
		disconnect_camera(c);
		return;
	}
	ssize_t n;

	for (;;) {
		if (c->header_got < sizeof(c->header)) {
			n = read(fd, c->header + c->header_got, sizeof(c->header) - c->header_got);
			if (n <= 0)
				goto out;
			c->header_got += n;
			if (c->header_got < sizeof(c->header))
				continue;
			uint32_t h[4];
			memcpy(h, c->header, sizeof(h));
			if (h[0] != FRAME_MAGIC || h[1] != SENSOR_W || h[2] != SENSOR_H ||
			    h[3] != (uint32_t)SENSOR_W * SENSOR_H * 3 / 2) {
				log_msg(c, "unexpected frame %ux%u (%u bytes)", h[1], h[2], h[3]);
				disconnect_camera(c);
				return;
			}
			c->raw_got = 0;
		}
		n = read(fd, c->raw + c->raw_got, c->raw_size - c->raw_got);
		if (n <= 0)
			goto out;
		c->raw_got += n;
		if (c->raw_got < c->raw_size)
			continue;
		rotate_nv12(c->raw, c->frame, SENSOR_W, SENSOR_H, c->rotation);
		c->frame_ready = true;
		c->frame_count++;
		c->header_got = 0;
		pw_stream_trigger_process(c->stream);
	}
out:
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
	// The ultrawide is reached by zooming out on the logical rear camera;
	// 1.0 keeps the main camera.
	snprintf(request, sizeof(request), "%s %d %d 1.0\n", c->id, SENSOR_W, SENSOR_H);
	if (write(fd, request, strlen(request)) != (ssize_t)strlen(request)) {
		close(fd);
		return false;
	}
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	c->fd = fd;
	c->header_got = c->raw_got = 0;
	c->frame_count = 0;
	c->io = pw_loop_add_io(pw_main_loop_get_loop(c->loop), fd,
			       SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP, false, on_socket, c);
	log_msg(c, "camera opened");
	return true;
}

static void on_retry(void *data, uint64_t expirations)
{
	(void)expirations;
	struct camera *c = data;
	struct timespec value = { 0 }, interval = { 0 };

	if (!c->streaming || c->fd >= 0 || connect_camera(c)) {
		pw_loop_update_timer(pw_main_loop_get_loop(c->loop), c->retry, &value, &interval, false);
		return;
	}
}

static void start_retry(struct camera *c)
{
	struct timespec value = { .tv_sec = 1 }, interval = { .tv_sec = 1 };
	pw_loop_update_timer(pw_main_loop_get_loop(c->loop), c->retry, &value, &interval, false);
}

static void on_state_changed(void *data, enum pw_stream_state old, enum pw_stream_state state,
			     const char *error)
{
	struct camera *c = data;

	(void)old;
	c->streaming = state == PW_STREAM_STATE_STREAMING;
	if (c->streaming) {
		if (c->fd < 0 && !connect_camera(c)) {
			log_msg(c, "camera service not reachable yet, retrying");
			start_retry(c);
		}
	} else {
		disconnect_camera(c);
	}
	if (error)
		log_msg(c, "stream error: %s", error);
}

static void on_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	struct camera *c = data;
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const struct spa_pod *params[2];

	if (param == NULL || id != SPA_PARAM_Format)
		return;

	params[0] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
		SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_size, SPA_POD_Int(c->out_w * c->out_h * 3 / 2),
		SPA_PARAM_BUFFERS_stride, SPA_POD_Int(c->out_w),
		SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd)));
	params[1] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
	pw_stream_update_params(c->stream, params, 2);
}

static void on_process(void *data)
{
	struct camera *c = data;
	struct pw_buffer *b;
	struct spa_buffer *buf;
	struct spa_meta_header *h;
	struct timespec now;
	const size_t size = (size_t)c->out_w * c->out_h * 3 / 2;

	if (!c->frame_ready)
		return;
	b = pw_stream_dequeue_buffer(c->stream);
	if (b == NULL)
		return;
	buf = b->buffer;
	if (buf->datas[0].data == NULL || buf->datas[0].maxsize < size) {
		pw_stream_queue_buffer(c->stream, b);
		return;
	}
	memcpy(buf->datas[0].data, c->frame, size);
	buf->datas[0].chunk->offset = 0;
	buf->datas[0].chunk->size = size;
	buf->datas[0].chunk->stride = c->out_w;
	if ((h = spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof(*h)))) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		h->pts = SPA_TIMESPEC_TO_NSEC(&now);
		h->flags = 0;
		h->seq = c->frame_count;
		h->dts_offset = 0;
	}
	c->frame_ready = false;
	pw_stream_queue_buffer(c->stream, b);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_state_changed,
	.param_changed = on_param_changed,
	.process = on_process,
};

static int setup_camera(struct camera *c, struct pw_main_loop *loop)
{
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const struct spa_pod *params[1];
	struct pw_properties *props;

	c->loop = loop;
	c->fd = -1;
	c->out_w = SENSOR_H;
	c->out_h = SENSOR_W;
	c->raw_size = (size_t)SENSOR_W * SENSOR_H * 3 / 2;
	c->raw = malloc(c->raw_size);
	c->frame = malloc(c->raw_size);
	if (!c->raw || !c->frame)
		return -ENOMEM;
	c->retry = pw_loop_add_timer(pw_main_loop_get_loop(loop), on_retry, c);

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

	params[0] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
		SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_NV12),
		SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&SPA_RECTANGLE(c->out_w, c->out_h)),
		SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&SPA_FRACTION(FPS, 1)));

	return pw_stream_connect(c->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
				 PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS, params, 1);
}

static void on_signal(void *data, int sig)
{
	(void)sig;
	pw_main_loop_quit(data);
}

int main(int argc, char *argv[])
{
	struct camera cameras[] = {
		{ .name = "bramble-camera-back", .description = "Back camera", .id = "0", .location = "back", .rotation = 90 },
		{ .name = "bramble-camera-front", .description = "Front camera", .id = "1", .location = "front", .rotation = 270 },
	};
	struct pw_main_loop *loop;
	int res;

	pw_init(&argc, &argv);
	loop = pw_main_loop_new(NULL);
	if (loop == NULL) {
		fprintf(stderr, "[bramble-camera] cannot create PipeWire loop\n");
		return 1;
	}
	pw_loop_add_signal(pw_main_loop_get_loop(loop), SIGINT, on_signal, loop);
	pw_loop_add_signal(pw_main_loop_get_loop(loop), SIGTERM, on_signal, loop);

	for (size_t i = 0; i < SPA_N_ELEMENTS(cameras); i++) {
		if ((res = setup_camera(&cameras[i], loop)) < 0) {
			fprintf(stderr, "[bramble-camera] %s: cannot create stream: %s\n",
				cameras[i].name, strerror(-res));
			return 1;
		}
	}
	pw_main_loop_run(loop);

	for (size_t i = 0; i < SPA_N_ELEMENTS(cameras); i++) {
		disconnect_camera(&cameras[i]);
		pw_stream_destroy(cameras[i].stream);
	}
	pw_main_loop_destroy(loop);
	pw_deinit();
	return 0;
}
