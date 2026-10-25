/*
 * fp-enroll-verify: CLI test tool for the Android HIDL fingerprint HAL
 * (android.hardware.biometrics.fingerprint@2.1 served by fps_hal inside the
 * Android container). Proves enrol + verify end to end with no PAM/lock-screen.
 *
 * Subcommands:
 *   status                          device id, authenticator id, enrolled fingers
 *   enroll    [--gid N] [--timeout S]   enrol one finger (touch sensor until done)
 *   verify    [--gid N] [--timeout S]   wait for a touch matching an enrolled finger
 *   enumerate [--gid N]               list enrolled finger ids
 *   remove    --fid N [--gid N]       remove one finger
 *   hmac-share                      keymaster@4.0: getHmacSharingParameters + computeSharedHmac
 *   hat-check                       gatekeeper HAT accepted by keymaster verifyAuthorization?
 *
 * Gatekeeper: enrol needs a hardware auth token (HAT), Android's flow:
 *   preEnroll() -> challenge; IGatekeeper.verify(uid, challenge, handle, secret) -> HAT;
 *   enroll(HAT, gid, timeout). The gatekeeper password is a random 32-byte secret
 *   generated once into <state>/secret (0600); its handle goes in <state>/handle.
 *   Nothing is hardcoded.
 *
 * Options: --hwbinder DEV (default /dev/hwbinder), --state DIR (default
 *          /var/lib/bramble-fingerprint), --store PATH (fingerprint data dir passed to
 *          setActiveGroup, default /data/vendor_de/0/fpdata/).
 *
 * Build (libgbinder-devel + glib2-devel):
 *   cc -Wall -Wextra -o fp-enroll-verify fp-enroll-verify.c \
 *      $(pkg-config --cflags --libs gbinder glib-2.0 gobject-2.0)
 *
 * Exit: 0 success, 1 HAL/gatekeeper/transaction failure or timeout,
 *       2 fingerprint HAL or service manager unreachable, 64 usage error.
 */
#include <fcntl.h>
#include <endian.h>
#include <gbinder.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <glib-unix.h>
#include <signal.h>

#define FP_IFACE "android.hardware.biometrics.fingerprint@2.1::IBiometricsFingerprint"
#define FP_SERVICE FP_IFACE "/default"
#define FP_CB_IFACE "android.hardware.biometrics.fingerprint@2.1::IBiometricsFingerprintClientCallback"
#define GK_IFACE "android.hardware.gatekeeper@1.0::IGatekeeper"
#define GK_SERVICE GK_IFACE "/default"

#define HAT_SIZE 69
#define SECRET_SIZE 32
#define HANDLE_MAX 256

/* IBiometricsFingerprint transaction codes (HIDL declaration order). */
enum {
	FP_SET_NOTIFY = 1,
	FP_PRE_ENROLL = 2,
	FP_ENROLL = 3,
	FP_POST_ENROLL = 4,
	FP_GET_AUTHENTICATOR_ID = 5,
	FP_CANCEL = 6,
	FP_ENUMERATE = 7,
	FP_REMOVE = 8,
	FP_SET_ACTIVE_GROUP = 9,
	FP_AUTHENTICATE = 10,
};

/* IGatekeeper 1.0 transaction codes (declaration order). */
enum { GK_ENROLL = 1, GK_VERIFY = 2 };

/* IBiometricsFingerprintClientCallback transaction codes (oneway). */
enum {
	CB_ON_ENROLL_RESULT = 1,
	CB_ON_ACQUIRED = 2,
	CB_ON_AUTHENTICATED = 3,
	CB_ON_ERROR = 4,
	CB_ON_REMOVED = 5,
	CB_ON_ENUMERATE = 6,
};

static const char* opt_hwbinder = GBINDER_DEFAULT_HWBINDER;
static const char* opt_state = "/var/lib/bramble-fingerprint";
static const char* opt_store = "/data/vendor_de/0/fpdata/";
static guint opt_gid = 0;
static guint opt_fid = 0;
static guint opt_timeout = 0;   /* 0: per-command default (enrol 180 s, others 60 s) */

typedef struct {
	GMainLoop* loop;
	gboolean done;        /* terminal event seen */
	gboolean ok;          /* terminal event was success */
	gboolean timed_out;
	guint fid;            /* last fingerId seen */
	guint remaining;      /* last remaining count */
	guint enumerated;     /* enumerate callbacks with fingerId != 0 */
	gboolean aborted;     /* SIGINT or SIGTERM received */
} Session;

static void finish(Session* s, gboolean ok)
{
	s->done = TRUE;
	s->ok = ok;
	if (s->loop) g_main_loop_quit(s->loop);
}

static gboolean rd_u64_u32x3(GBinderReader* r, guint64* dev, guint32* a, guint32* b, guint32* c)
{
	return gbinder_reader_read_uint64(r, dev) && gbinder_reader_read_uint32(r, a) &&
	       gbinder_reader_read_uint32(r, b) && gbinder_reader_read_uint32(r, c);
}

static const char* acquired_hint(guint info)
{
	switch (info) {
	case 0: return "good";
	case 1: return "partial touch, lift and try again";
	case 2: return "not enough of the finger on the sensor";
	case 3: return "sensor is dirty, wipe it";
	case 4: return "too slow, lift and try again";
	case 5: return "too fast, lift and try again";
	case 6: return "vendor-specific warning";
	default: return "unknown warning";
	}
}

/*
 * Callback from the HAL (oneway), dispatched on the GLib main loop. The caller
 * waits in wait_terminal() and reads the Session fields afterwards.
 */
static GBinderLocalReply* on_callback(GBinderLocalObject* obj, GBinderRemoteRequest* req,
	guint code, guint flags, int* status, void* user_data)
{
	Session* s = user_data;
	GBinderReader r;
	guint64 dev = 0;
	guint32 fid = 0, gid = 0, rem = 0, info = 0;
	gint32 err = 0, vendor = 0;
	gsize n = 0, es = 0;

	(void)obj;
	(void)flags;
	*status = GBINDER_STATUS_OK;
	gbinder_remote_request_init_reader(req, &r);

	switch (code) {
	case CB_ON_ENROLL_RESULT:
		if (!rd_u64_u32x3(&r, &dev, &fid, &gid, &rem)) break;
		g_print("enrol: fingerId=%u groupId=%u remaining=%u\n", fid, gid, rem);
		s->fid = fid;
		s->remaining = rem;
		if (rem) g_print("  touch recorded, %u more needed: lift the finger and place it again\n", rem);
		if (rem == 0) finish(s, TRUE);
		break;

	case CB_ON_ACQUIRED:
		if (!gbinder_reader_read_uint64(&r, &dev) || !gbinder_reader_read_uint32(&r, &info) ||
		    !gbinder_reader_read_int32(&r, &vendor)) break;
		g_print("acquired: info=%u (%s) vendor=%d\n", info, acquired_hint(info), vendor);
		break;

	case CB_ON_AUTHENTICATED:
		if (!gbinder_reader_read_uint64(&r, &dev) || !gbinder_reader_read_uint32(&r, &fid) ||
		    !gbinder_reader_read_uint32(&r, &gid)) break;
		gbinder_reader_read_hidl_vec(&r, &n, &es); /* HAT token, unused here */
		if (fid == 0) {
			g_print("authenticated: REJECTED (fingerId 0)\n");
		} else {
			g_print("authenticated: MATCH fingerId=%u groupId=%u\n", fid, gid);
			s->fid = fid;
			finish(s, TRUE);
		}
		break;

	case CB_ON_ERROR:
		if (!gbinder_reader_read_uint64(&r, &dev) || !gbinder_reader_read_int32(&r, &err) ||
		    !gbinder_reader_read_int32(&r, &vendor)) break;
		g_printerr("hal error: FingerprintError=%d vendorCode=%d\n", err, vendor);
		finish(s, FALSE);
		break;

	case CB_ON_REMOVED:
		if (!rd_u64_u32x3(&r, &dev, &fid, &gid, &rem)) break;
		g_print("removed: fingerId=%u groupId=%u remaining=%u\n", fid, gid, rem);
		s->remaining = rem;
		if (rem == 0) finish(s, TRUE);
		break;

	case CB_ON_ENUMERATE:
		if (!rd_u64_u32x3(&r, &dev, &fid, &gid, &rem)) break;
		if (fid) {
			g_print("enrolled: fingerId=%u groupId=%u\n", fid, gid);
			s->enumerated++;
		}
		s->remaining = rem;
		if (rem == 0) finish(s, TRUE);
		break;

	default:
		g_printerr("unknown callback code %u\n", code);
		break;
	}
	return NULL;
}

static gboolean on_timeout(gpointer user_data)
{
	Session* s = user_data;
	s->timed_out = TRUE;
	finish(s, FALSE);
	return G_SOURCE_REMOVE;
}

static gboolean on_abort(gpointer user_data)
{
	Session* s = user_data;
	if (s->aborted) return G_SOURCE_CONTINUE;
	s->aborted = TRUE;
	g_printerr("\ninterrupted: cancelling on the HAL\n");
	finish(s, FALSE);
	return G_SOURCE_CONTINUE;
}

/* Runs the main loop until a terminal callback or timeout. Returns TRUE on success. */
static gboolean wait_terminal(Session* s, guint timeout_s)
{
	guint tid = g_timeout_add_seconds(timeout_s, on_timeout, s);
	s->loop = g_main_loop_new(NULL, FALSE);
	if (!s->done) g_main_loop_run(s->loop);
	g_source_remove(tid);
	g_main_loop_unref(s->loop);
	s->loop = NULL;
	if (s->timed_out) g_printerr("timed out after %us\n", timeout_s);
	return s->done && s->ok;
}

/*
 * HIDL replies begin with an int32 exception word (0 = no exception), followed
 * by the declared return values. [INFERENCE: libgbinder passes the raw reply, so
 * the word is still present; confirm on device.]
 */
static gboolean reply_begin(GBinderRemoteReply* rep, GBinderReader* r)
{
	gint32 exc = -1;
	gbinder_remote_reply_init_reader(rep, r);
	return gbinder_reader_read_int32(r, &exc) && exc == 0;
}

/* Sends one transaction; returns the reply or NULL on failure. */
static GBinderRemoteReply* transact(GBinderClient* cl, guint code, GBinderLocalRequest* req)
{
	int status = 0;
	GBinderRemoteReply* rep = gbinder_client_transact_sync_reply(cl, code, req, &status);
	if (!rep || status != GBINDER_STATUS_OK) {
		g_printerr("transact code %u failed (binder status %d)\n", code, status);
		if (rep) gbinder_remote_reply_unref(rep);
		return NULL;
	}
	return rep;
}

/* Call returning RequestStatus (int32); TRUE when the HAL reports 0. */
static gboolean call_status(GBinderClient* cl, guint code, GBinderLocalRequest* req)
{
	GBinderRemoteReply* rep = transact(cl, code, req);
	GBinderReader r;
	gint32 rs = -1;
	gboolean ok = FALSE;

	if (rep) {
		ok = reply_begin(rep, &r) && gbinder_reader_read_int32(&r, &rs) && rs == 0;
		gbinder_remote_reply_unref(rep);
	}
	if (!ok) g_printerr("HAL call code %u failed (RequestStatus %d)\n", code, rs);
	return ok;
}

static GBinderLocalRequest* new_request(GBinderClient* cl, GBinderWriter* w)
{
	GBinderLocalRequest* req = gbinder_client_new_request(cl);
	gbinder_local_request_init_writer(req, w);
	return req;
}

/* Opens the fingerprint HAL and registers our callback. Returns the client. */
static GBinderClient* open_fp(GBinderServiceManager* sm, GBinderRemoteObject** obj_out,
	GBinderLocalObject** cb_out, Session* s, guint64* device_id)
{
	int status = 0;
	GBinderRemoteObject* obj = gbinder_servicemanager_get_service_sync(sm, FP_SERVICE, &status);
	GBinderClient* cl;
	GBinderLocalObject* cb;
	GBinderWriter w;
	GBinderLocalRequest* req;
	GBinderRemoteReply* rep;
	const char* const ifaces[] = { FP_CB_IFACE, NULL };
	GBinderReader r;

	if (!obj) {
		g_printerr("fingerprint HAL not reachable: %s (status %d)\n", FP_SERVICE, status);
		return NULL;
	}
	cl = gbinder_client_new(obj, FP_IFACE);
	cb = gbinder_local_object_new(gbinder_remote_object_ipc(obj), ifaces, on_callback, s);

	req = new_request(cl, &w);
	gbinder_writer_append_local_object(&w, cb);
	rep = transact(cl, FP_SET_NOTIFY, req);
	gbinder_local_request_unref(req);
	if (!rep) return NULL;
	if (!reply_begin(rep, &r) || !gbinder_reader_read_uint64(&r, device_id)) {
		g_printerr("setNotify: bad reply\n");
		gbinder_remote_reply_unref(rep);
		return NULL;
	}
	gbinder_remote_reply_unref(rep);

	*obj_out = obj;
	*cb_out = cb;
	return cl;
}

static gboolean set_active_group(GBinderClient* cl)
{
	GBinderWriter w;
	GBinderLocalRequest* req = new_request(cl, &w);
	gboolean ok;

	gbinder_writer_append_int32(&w, opt_gid);
	gbinder_writer_append_hidl_string(&w, opt_store);
	ok = call_status(cl, FP_SET_ACTIVE_GROUP, req);
	gbinder_local_request_unref(req);
	return ok;
}

static gboolean read_file(const char* path, guint8* buf, gsize want, gsize* got)
{
	gchar* data = NULL;
	gsize len = 0;
	if (!g_file_get_contents(path, &data, &len, NULL)) return FALSE;
	if (len > want) len = want;
	memcpy(buf, data, len);
	*got = len;
	g_free(data);
	return TRUE;
}

static gboolean write_private(const char* path, const guint8* buf, gsize len)
{
	int fd = g_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	gboolean ok;
	if (fd < 0) return FALSE;
	ok = write(fd, buf, len) == (ssize_t)len;
	close(fd);
	return ok;
}

/* Random 32-byte gatekeeper secret, created once and kept root-only. */
static gboolean load_or_create_secret(guint8* secret)
{
	gchar* path = g_build_filename(opt_state, "secret", NULL);
	gsize got = 0;
	FILE* f;
	gboolean ok = FALSE;

	if (read_file(path, secret, SECRET_SIZE, &got) && got == SECRET_SIZE) {
		ok = TRUE;
	} else {
		f = fopen("/dev/urandom", "rb");
		if (f && fread(secret, 1, SECRET_SIZE, f) == SECRET_SIZE)
			ok = write_private(path, secret, SECRET_SIZE);
		if (f) fclose(f);
	}
	g_free(path);
	return ok;
}

/*
 * GatekeeperResponse is a HIDL struct { int32 code; uint32 timeout; vec<uint8> data }.
 * The struct is one buffer object; its vec payload is the child buffer that follows.
 */
typedef struct {
	gint32 code;
	guint32 timeout;
	GBinderHidlVec data;
} GkResponse;

static gboolean gk_read_response(GBinderRemoteReply* rep, gint32* code, guint8* data, gsize max, gsize* len)
{
	GBinderReader r;
	const GkResponse* rsp;
	GBinderBuffer* child = NULL;
	gsize n;
	gboolean ok = FALSE;

	*len = 0;
	if (!reply_begin(rep, &r)) return FALSE;
	rsp = gbinder_reader_read_hidl_struct(&r, GkResponse);
	if (!rsp) return FALSE;
	*code = rsp->code;
	n = rsp->data.count;
	if (!n) {
		ok = TRUE;
	} else if (n <= max && (child = gbinder_reader_read_buffer(&r)) && child->size == n) {
		memcpy(data, child->data, n);
		*len = n;
		ok = TRUE;
	}
	g_print("gatekeeper response: code=%d timeout=%u data=%zu bytes\n", rsp->code, rsp->timeout, n);
	if (child) gbinder_buffer_free(child);
	return ok;
}

/* IGatekeeper.enroll with no current password: creates the handle for `secret`. */
static gboolean gk_enroll_handle(GBinderClient* gk, guint8* handle, gsize* hlen, const guint8* secret)
{
	GBinderWriter w;
	GBinderLocalRequest* req = new_request(gk, &w);
	GBinderRemoteReply* rep;
	gint32 code = -1;
	gboolean ok = FALSE;

	gbinder_writer_append_int32(&w, 0);                       /* uid */
	gbinder_writer_append_hidl_vec(&w, NULL, 0, 1);           /* no current handle */
	gbinder_writer_append_hidl_vec(&w, NULL, 0, 1);           /* no current password */
	gbinder_writer_append_hidl_vec(&w, secret, SECRET_SIZE, 1);
	rep = transact(gk, GK_ENROLL, req);
	gbinder_local_request_unref(req);
	if (rep) {
		ok = gk_read_response(rep, &code, handle, HANDLE_MAX, hlen) && (code == 0 || code == 1) && *hlen > 0;
		gbinder_remote_reply_unref(rep);
	}
	if (!ok) g_printerr("gatekeeper enroll failed (code %d)\n", code);
	return ok;
}

/* IGatekeeper.verify(uid, challenge, handle, secret) -> HAT in response.data. */
static gboolean gk_verify_hat(GBinderClient* gk, guint64 challenge, const guint8* handle, gsize hlen,
	const guint8* secret, guint8* hat)
{
	GBinderWriter w;
	GBinderLocalRequest* req = new_request(gk, &w);
	GBinderRemoteReply* rep;
	gint32 code = -1;
	gsize len = 0;
	gboolean ok = FALSE;

	gbinder_writer_append_int32(&w, 0);                       /* uid */
	gbinder_writer_append_int64(&w, challenge);
	gbinder_writer_append_hidl_vec(&w, handle, hlen, 1);
	gbinder_writer_append_hidl_vec(&w, secret, SECRET_SIZE, 1);
	rep = transact(gk, GK_VERIFY, req);
	gbinder_local_request_unref(req);
	if (rep) {
		ok = gk_read_response(rep, &code, hat, HAT_SIZE, &len) && (code == 0 || code == 1) && len == HAT_SIZE;
		gbinder_remote_reply_unref(rep);
	}
	if (!ok) g_printerr("gatekeeper verify failed (code %d, token %zu bytes)\n", code, len);
	return ok;
}

/* Gets a HAT for `challenge`. Creates the gatekeeper handle on first use. */
static gboolean get_hat(GBinderServiceManager* sm, guint64 challenge, guint8* hat)
{
	guint8 secret[SECRET_SIZE], handle[HANDLE_MAX];
	gchar* path;
	gsize hlen = 0;
	int status = 0;
	GBinderRemoteObject* obj;
	GBinderClient* gk;
	gboolean ok = FALSE;

	if (!load_or_create_secret(secret)) {
		g_printerr("cannot create gatekeeper secret in %s\n", opt_state);
		return FALSE;
	}

	path = g_build_filename(opt_state, "handle", NULL);
	obj = gbinder_servicemanager_get_service_sync(sm, GK_SERVICE, &status);
	if (!obj) {
		g_printerr("gatekeeper not reachable: %s (status %d)\n", GK_SERVICE, status);
		g_free(path);
		return FALSE;
	}
	gk = gbinder_client_new(obj, GK_IFACE);

	if (!(read_file(path, handle, HANDLE_MAX, &hlen) && hlen > 0)) {
		hlen = 0;
		if (gk_enroll_handle(gk, handle, &hlen, secret))
			write_private(path, handle, hlen);
		else
			goto out;
	}
	ok = gk_verify_hat(gk, challenge, handle, hlen, secret, hat);
out:
	gbinder_client_unref(gk);
	gbinder_remote_object_unref(obj);
	g_free(path);
	return ok;
}

static gboolean fp_enroll(GBinderClient* cl, GBinderServiceManager* sm, Session* s)
{
	GBinderWriter w;
	GBinderLocalRequest* req;
	GBinderRemoteReply* rep;
	GBinderReader r;
	guint64 challenge = 0;
	guint8 hat[HAT_SIZE];
	gboolean ok;

	req = new_request(cl, &w);
	rep = transact(cl, FP_PRE_ENROLL, req);
	gbinder_local_request_unref(req);
	if (!rep) return FALSE;
	if (!reply_begin(rep, &r) || !gbinder_reader_read_uint64(&r, &challenge)) {
		g_printerr("preEnroll: bad reply\n");
		gbinder_remote_reply_unref(rep);
		return FALSE;
	}
	gbinder_remote_reply_unref(rep);
	g_print("preEnroll challenge=0x%016" G_GINT64_MODIFIER "x\n", challenge);

	if (!get_hat(sm, challenge, hat)) return FALSE;
	{
		guint i;
		g_print("HAT:");
		for (i = 0; i < HAT_SIZE; i++) g_print(" %02x", hat[i]);
		g_print("\n");
	}

	req = new_request(cl, &w);
	gbinder_writer_append_buffer_object(&w, hat, HAT_SIZE);   /* enroll(uint8_t hat[69]): fixed-size array, not a vec */
	gbinder_writer_append_int32(&w, opt_gid);
	gbinder_writer_append_int32(&w, opt_timeout);
	ok = call_status(cl, FP_ENROLL, req);
	gbinder_local_request_unref(req);
	if (!ok) return FALSE;

	g_print("enrolling: place the finger on the sensor, lift it, and repeat until the count reaches zero.\n"
		"vary the angle slightly between touches. Ctrl-C cancels. Timeout: %us.\n", opt_timeout);
	ok = wait_terminal(s, opt_timeout);
	if (!ok) {
		req = new_request(cl, &w);
		call_status(cl, FP_CANCEL, req);
		gbinder_local_request_unref(req);
	}

	/* postEnroll closes the enrolment session; attempt it regardless of outcome. */
	req = new_request(cl, &w);
	call_status(cl, FP_POST_ENROLL, req);
	gbinder_local_request_unref(req);
	return ok;
}

static gboolean fp_verify(GBinderClient* cl, Session* s)
{
	GBinderWriter w;
	GBinderLocalRequest* req = new_request(cl, &w);
	gboolean ok;

	gbinder_writer_append_int64(&w, 0);                       /* operationId: no crypto */
	gbinder_writer_append_int32(&w, opt_gid);
	ok = call_status(cl, FP_AUTHENTICATE, req);
	gbinder_local_request_unref(req);
	if (!ok) return FALSE;

	g_print("verifying: touch the enrolled finger within %us\n", opt_timeout);
	ok = wait_terminal(s, opt_timeout);
	if (!ok) {
		req = new_request(cl, &w);
		call_status(cl, FP_CANCEL, req);
		gbinder_local_request_unref(req);
	}
	return ok;
}

/* Enumerate or remove (gid, fid); waits for the terminal callback. */
static gboolean fp_list_or_remove(GBinderClient* cl, Session* s, guint code)
{
	GBinderWriter w;
	GBinderLocalRequest* req = new_request(cl, &w);
	gboolean ok;

	if (code == FP_REMOVE) {
		gbinder_writer_append_int32(&w, opt_gid);
		gbinder_writer_append_int32(&w, opt_fid);
	}
	ok = call_status(cl, code, req);
	gbinder_local_request_unref(req);
	return ok && wait_terminal(s, opt_timeout);
}

static gboolean fp_authenticator_id(GBinderClient* cl)
{
	GBinderWriter w;
	GBinderLocalRequest* req = new_request(cl, &w);
	GBinderRemoteReply* rep = transact(cl, FP_GET_AUTHENTICATOR_ID, req);
	GBinderReader r;
	guint64 aid = 0;
	gboolean ok = FALSE;

	gbinder_local_request_unref(req);
	if (rep) {
		ok = reply_begin(rep, &r) && gbinder_reader_read_uint64(&r, &aid);
		gbinder_remote_reply_unref(rep);
	}
	if (ok) g_print("authenticator id: 0x%016" G_GINT64_MODIFIER "x\n", aid);
	return ok;
}

/* IKeymasterDevice 4.0 transaction codes (HIDL declaration order). */
#define KM_IFACE "android.hardware.keymaster@4.0::IKeymasterDevice"
#define KM_SERVICE KM_IFACE "/default"
enum { KM_GET_HMAC_PARAMS = 2, KM_COMPUTE_SHARED_HMAC = 3, KM_VERIFY_AUTHORIZATION = 4 };

#define HMAC_NONCE_SIZE 32
#define HMAC_SEED_MAX 256

/* HmacSharingParameters { vec<uint8_t> seed; uint8_t nonce[32]; } */
typedef struct {
	GBinderHidlVec seed;
	guint8 nonce[HMAC_NONCE_SIZE];
} HmacParams;
G_STATIC_ASSERT(sizeof(HmacParams) == 48);

static const GBinderWriterField hmac_fields[] = {
	GBINDER_WRITER_FIELD_HIDL_VEC_BYTE(HmacParams, seed),
	GBINDER_WRITER_FIELD_END()
};
static const GBinderWriterType hmac_type = {
	GBINDER_WRITER_STRUCT_NAME_AND_SIZE(HmacParams), hmac_fields
};

/* HIDL HardwareAuthToken { challenge, userId, authenticatorId, authenticatorType, timestamp, mac }. */
typedef struct {
	guint64 challenge;
	guint64 user_id;
	guint64 authenticator_id;
	guint32 authenticator_type;
	guint64 timestamp;
	GBinderHidlVec mac;
} KmAuthToken;
G_STATIC_ASSERT(sizeof(KmAuthToken) == 56);

static const GBinderWriterField km_token_fields[] = {
	GBINDER_WRITER_FIELD_HIDL_VEC_BYTE(KmAuthToken, mac),
	GBINDER_WRITER_FIELD_END()
};
static const GBinderWriterType km_token_type = {
	GBINDER_WRITER_STRUCT_NAME_AND_SIZE(KmAuthToken), km_token_fields
};

static void print_hex(const char* label, const guint8* p, gsize n)
{
	gsize i;

	g_print("%s (%zu bytes): ", label, n);
	for (i = 0; i < n; i++) g_print("%02x", p[i]);
	g_print("\n");
}

static GBinderClient* km_open(GBinderServiceManager* sm, GBinderRemoteObject** obj_out)
{
	int status = 0;
	GBinderRemoteObject* obj = gbinder_servicemanager_get_service_sync(sm, KM_SERVICE, &status);

	if (!obj) {
		g_printerr("keymaster not reachable: %s (status %d)\n", KM_SERVICE, status);
		return NULL;
	}
	*obj_out = obj;
	return gbinder_client_new(obj, KM_IFACE);
}

/* getHmacSharingParameters() -> (ErrorCode, HmacSharingParameters); seed is copied into seedbuf. */
static gboolean km_get_hmac_params(GBinderClient* km, HmacParams* out, guint8* seedbuf)
{
	GBinderWriter w;
	GBinderLocalRequest* req = new_request(km, &w);
	GBinderRemoteReply* rep = transact(km, KM_GET_HMAC_PARAMS, req);
	GBinderReader r;
	const HmacParams* p;
	GBinderBuffer* child = NULL;
	gint32 err = -1;
	gboolean ok = FALSE;

	gbinder_local_request_unref(req);
	if (!rep) return FALSE;
	if (reply_begin(rep, &r) && gbinder_reader_read_int32(&r, &err) && err == 0 &&
	    (p = gbinder_reader_read_hidl_struct(&r, HmacParams)) != NULL) {
		guint32 n = p->seed.count;

		memcpy(out->nonce, p->nonce, HMAC_NONCE_SIZE);
		memset(&out->seed, 0, sizeof(out->seed));
		if (!n) {
			ok = TRUE;
		} else if (n <= HMAC_SEED_MAX && (child = gbinder_reader_read_buffer(&r)) && child->size == n) {
			memcpy(seedbuf, child->data, n);
			out->seed.data.ptr = seedbuf;
			out->seed.count = n;
			ok = TRUE;
		}
	}
	if (child) gbinder_buffer_free(child);
	gbinder_remote_reply_unref(rep);
	if (!ok) g_printerr("getHmacSharingParameters failed (ErrorCode %d)\n", err);
	return ok;
}

/*
 * Keystore's boot-time job: collect the sharing parameters of every KM4 instance, then hand the
 * full list to each instance's computeSharedHmac. This container registers one instance only.
 */
static gboolean km_hmac_share(GBinderServiceManager* sm)
{
	GBinderRemoteObject* obj = NULL;
	GBinderClient* km = km_open(sm, &obj);
	HmacParams params;
	guint8 seed[HMAC_SEED_MAX];
	GBinderWriter w;
	GBinderLocalRequest* req;
	GBinderRemoteReply* rep;
	GBinderReader r;
	const guint8* chk;
	gsize n = 0;
	gint32 err = -1;
	gboolean ok = FALSE;

	if (!km) return FALSE;
	if (km_get_hmac_params(km, &params, seed)) {
		print_hex("keymaster seed", seed, params.seed.count);
		print_hex("keymaster nonce", params.nonce, HMAC_NONCE_SIZE);

		req = new_request(km, &w);
		gbinder_writer_append_struct_vec(&w, &params, 1, &hmac_type);
		rep = transact(km, KM_COMPUTE_SHARED_HMAC, req);
		gbinder_local_request_unref(req);
		if (rep) {
			if (reply_begin(rep, &r) && gbinder_reader_read_int32(&r, &err)) {
				g_print("computeSharedHmac ErrorCode=%d\n", err);
				if (err == 0 && (chk = gbinder_reader_read_hidl_byte_vec(&r, &n)) != NULL) {
					print_hex("sharingCheck", chk, n);
					ok = n == 32;
				}
			}
			gbinder_remote_reply_unref(rep);
		}
	}
	if (!ok) g_printerr("hmac sharing failed (ErrorCode %d)\n", err);
	gbinder_client_unref(km);
	gbinder_remote_object_unref(obj);
	return ok;
}

/* Fresh gatekeeper HAT, converted to HIDL HardwareAuthToken, checked by keymaster verifyAuthorization. */
static gboolean km_hat_check(GBinderServiceManager* sm)
{
	GBinderRemoteObject* obj = NULL;
	GBinderClient* km;
	guint64 challenge = ((guint64)g_random_int() << 32) | g_random_int();
	guint8 hat[HAT_SIZE];
	guint8 mac[32];
	KmAuthToken tok;
	guint32 type;
	guint64 ts;
	GBinderWriter w;
	GBinderLocalRequest* req;
	GBinderRemoteReply* rep;
	GBinderReader r;
	gint32 err = -1;
	gboolean ok = FALSE;

	if (!get_hat(sm, challenge, hat)) return FALSE;

	/* hw_auth_token_t (packed): version u8, challenge, user_id, authenticator_id (host order),
	 * authenticator_type u32 and timestamp u64 (big endian), hmac[32]. */
	memset(&tok, 0, sizeof(tok));
	memcpy(&tok.challenge, hat + 1, 8);
	memcpy(&tok.user_id, hat + 9, 8);
	memcpy(&tok.authenticator_id, hat + 17, 8);
	memcpy(&type, hat + 25, 4);
	memcpy(&ts, hat + 29, 8);
	tok.authenticator_type = be32toh(type);
	tok.timestamp = be64toh(ts);
	memcpy(mac, hat + 37, 32);
	tok.mac.data.ptr = mac;
	tok.mac.count = 32;
	g_print("HAT: version=%u challenge=0x%016" G_GINT64_MODIFIER "x user=0x%016" G_GINT64_MODIFIER
		"x auth_id=0x%016" G_GINT64_MODIFIER "x type=%u timestamp=%" G_GUINT64_FORMAT "\n",
		hat[0], tok.challenge, tok.user_id, tok.authenticator_id, tok.authenticator_type, tok.timestamp);
	print_hex("HAT hmac", mac, 32);

	km = km_open(sm, &obj);
	if (!km) return FALSE;
	req = new_request(km, &w);
	gbinder_writer_append_int64(&w, tok.challenge);           /* must equal the token's challenge */
	gbinder_writer_append_hidl_vec(&w, NULL, 0, 1);           /* no parametersToVerify */
	gbinder_writer_append_struct(&w, &tok, &km_token_type, NULL);
	rep = transact(km, KM_VERIFY_AUTHORIZATION, req);
	gbinder_local_request_unref(req);
	if (rep) {
		if (reply_begin(rep, &r) && gbinder_reader_read_int32(&r, &err)) {
			g_print("verifyAuthorization ErrorCode=%d (0 = OK)\n", err);
			ok = err == 0;
		}
		gbinder_remote_reply_unref(rep);
	}
	if (!ok) g_printerr("verifyAuthorization rejected the HAT (ErrorCode %d)\n", err);
	gbinder_client_unref(km);
	gbinder_remote_object_unref(obj);
	return ok;
}

static int usage(void)
{
	g_printerr("usage: bramble-fingerprint [--hwbinder DEV] [--state DIR] [--store PATH] [--gid N]\n"
		"                             [--fid N] [--timeout S]\n"
		"                             (status|enroll|verify|enumerate|list|remove|hmac-share|hat-check)\n");
	return 64;
}

int main(int argc, char* argv[])
{
	const char* cmd = NULL;
	int i;
	GBinderServiceManager* sm;
	GBinderRemoteObject* obj = NULL;
	GBinderLocalObject* cb = NULL;
	GBinderClient* cl;
	Session s;
	guint64 device_id = 0;
	int rc = 1;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--hwbinder") && i + 1 < argc) opt_hwbinder = argv[++i];
		else if (!strcmp(argv[i], "--state") && i + 1 < argc) opt_state = argv[++i];
		else if (!strcmp(argv[i], "--store") && i + 1 < argc) opt_store = argv[++i];
		else if (!strcmp(argv[i], "--gid") && i + 1 < argc) opt_gid = (guint)strtoul(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "--fid") && i + 1 < argc) opt_fid = (guint)strtoul(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) opt_timeout = (guint)strtoul(argv[++i], NULL, 10);
		else if (!cmd && argv[i][0] != '-') cmd = argv[i];
		else return usage();
	}
	if (!cmd) return usage();
	if (!strcmp(cmd, "list")) cmd = "enumerate";
	if (strcmp(cmd, "status") && strcmp(cmd, "enroll") && strcmp(cmd, "verify") &&
	    strcmp(cmd, "enumerate") && strcmp(cmd, "remove") && strcmp(cmd, "hmac-share") &&
	    strcmp(cmd, "hat-check"))
		return usage();
	if (!strcmp(cmd, "remove") && !opt_fid) return usage();
	if (!opt_timeout) opt_timeout = strcmp(cmd, "enroll") ? 60 : 180;

	memset(&s, 0, sizeof(s));
	g_unix_signal_add(SIGINT, on_abort, &s);
	g_unix_signal_add(SIGTERM, on_abort, &s);
	g_mkdir_with_parents(opt_state, 0700);

	sm = gbinder_servicemanager_new(opt_hwbinder);
	if (!sm) {
		g_printerr("cannot open service manager on %s\n", opt_hwbinder);
		return 2;
	}

	if (!strcmp(cmd, "hmac-share") || !strcmp(cmd, "hat-check")) {
		rc = (!strcmp(cmd, "hmac-share") ? km_hmac_share(sm) : km_hat_check(sm)) ? 0 : 1;
		gbinder_servicemanager_unref(sm);
		return rc;
	}

	cl = open_fp(sm, &obj, &cb, &s, &device_id);
	if (!cl) {
		gbinder_servicemanager_unref(sm);
		return 2;
	}
	g_print("HAL device id: 0x%016" G_GINT64_MODIFIER "x\n", device_id);

	if (!strcmp(cmd, "status")) {
		rc = fp_authenticator_id(cl) && set_active_group(cl) && fp_list_or_remove(cl, &s, FP_ENUMERATE) ? 0 : 1;
	} else if (!strcmp(cmd, "enroll")) {
		rc = set_active_group(cl) && fp_enroll(cl, sm, &s) ? 0 : 1;
		if (rc == 0) g_print("enrol OK: fingerId=%u groupId=%u\n", s.fid, opt_gid);
	} else if (!strcmp(cmd, "verify")) {
		rc = set_active_group(cl) && fp_verify(cl, &s) ? 0 : 1;
		if (rc == 0) g_print("verify OK: matched fingerId=%u\n", s.fid);
	} else if (!strcmp(cmd, "enumerate")) {
		rc = set_active_group(cl) && fp_list_or_remove(cl, &s, FP_ENUMERATE) ? 0 : 1;
		if (rc == 0) g_print("enumerate done: %u enrolled\n", s.enumerated);
	} else {
		rc = set_active_group(cl) && fp_list_or_remove(cl, &s, FP_REMOVE) ? 0 : 1;
		if (rc == 0) g_print("remove OK: fingerId=%u\n", opt_fid);
	}

	gbinder_client_unref(cl);
	gbinder_local_object_unref(cb);
	gbinder_remote_object_unref(obj);
	gbinder_servicemanager_unref(sm);
	return rc;
}
