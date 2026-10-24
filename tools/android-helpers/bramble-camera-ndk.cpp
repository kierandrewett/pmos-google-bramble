// bramble-camera-ndk: small Camera2 NDK client that runs inside the Android HAL
// container on google-bramble and hands frames to the Linux side.
//
//   bramble-camera-ndk list
//   bramble-camera-ndk still <camera-id> <out.jpg> [zoom]
//   bramble-camera-ndk serve <socket>
//
// "still" writes one full-resolution JPEG. "serve" listens on a Unix socket
// and opens a camera only while a client is connected. The client sends one
// request line, "<camera-id> <width> <height> <zoom>\n", and then receives
// frames: a 16-byte header ("BRCF", width, height, payload size; little
// endian u32) followed by NV12 data. Closing the connection closes the
// camera. zoom is ACAMERA_CONTROL_ZOOM_RATIO: on the logical rear camera
// values below 1.0 switch to the ultrawide sensor, a negative value picks
// the widest zoom available and 0 keeps the HAL default.

#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraError.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCaptureRequest.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <poll.h>
#include <dlfcn.h>
#include <unistd.h>

#define LOG(...) do { fprintf(stderr, "[bramble-camera] " __VA_ARGS__); fputc('\n', stderr); } while (0)

namespace {

struct Size { int32_t w = 0, h = 0; };

bool check(camera_status_t s, const char *what) {
    if (s == ACAMERA_OK)
        return true;
    LOG("%s failed: %d", what, s);
    return false;
}

void onDisconnected(void *, ACameraDevice *) { LOG("camera disconnected"); }
void onError(void *, ACameraDevice *, int err) { LOG("camera error %d", err); }

void onSessionClosed(void *, ACameraCaptureSession *) {}
void onSessionReady(void *, ACameraCaptureSession *) {}
void onSessionActive(void *, ACameraCaptureSession *) {}

// Largest size the camera offers for an output format.
Size largestSize(ACameraManager *mgr, const char *id, int32_t format) {
    Size best;
    ACameraMetadata *chars = nullptr;
    if (!check(ACameraManager_getCameraCharacteristics(mgr, id, &chars), "getCameraCharacteristics"))
        return best;
    ACameraMetadata_const_entry e{};
    if (ACameraMetadata_getConstEntry(chars, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &e) == ACAMERA_OK) {
        for (uint32_t i = 0; i + 3 < e.count; i += 4) {
            int32_t fmt = e.data.i32[i], w = e.data.i32[i + 1], h = e.data.i32[i + 2], in = e.data.i32[i + 3];
            if (fmt == format && !in && int64_t(w) * h > int64_t(best.w) * best.h)
                best = {w, h};
        }
    }
    ACameraMetadata_free(chars);
    return best;
}

int listCameras() {
    ACameraManager *mgr = ACameraManager_create();
    ACameraIdList *ids = nullptr;
    if (!check(ACameraManager_getCameraIdList(mgr, &ids), "getCameraIdList"))
        return 1;
    printf("%d camera(s)\n", ids->numCameras);
    for (int i = 0; i < ids->numCameras; i++) {
        const char *id = ids->cameraIds[i];
        ACameraMetadata *chars = nullptr;
        if (ACameraManager_getCameraCharacteristics(mgr, id, &chars) != ACAMERA_OK)
            continue;
        ACameraMetadata_const_entry e{};
        const char *facing = "?";
        if (ACameraMetadata_getConstEntry(chars, ACAMERA_LENS_FACING, &e) == ACAMERA_OK)
            facing = e.data.u8[0] == ACAMERA_LENS_FACING_FRONT ? "front" :
                     e.data.u8[0] == ACAMERA_LENS_FACING_BACK ? "back" : "external";
        float zmin = 1, zmax = 1;
        if (ACameraMetadata_getConstEntry(chars, ACAMERA_CONTROL_ZOOM_RATIO_RANGE, &e) == ACAMERA_OK) {
            zmin = e.data.f[0];
            zmax = e.data.f[1];
        }
        std::string phys;
        size_t n = 0;
        const char *const *physIds = nullptr;
        if (ACameraMetadata_isLogicalMultiCamera(chars, &n, &physIds))
            for (size_t j = 0; j < n; j++)
                phys += std::string(j ? "," : "") + physIds[j];
        Size jpeg = largestSize(mgr, id, AIMAGE_FORMAT_JPEG);
        int orientation = 0;
        if (ACameraMetadata_getConstEntry(chars, ACAMERA_SENSOR_ORIENTATION, &e) == ACAMERA_OK)
            orientation = e.data.i32[0];
        std::string yuv;
        if (ACameraMetadata_getConstEntry(chars, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &e) == ACAMERA_OK)
            for (uint32_t k = 0; k + 3 < e.count; k += 4)
                if (e.data.i32[k] == AIMAGE_FORMAT_YUV_420_888 && !e.data.i32[k + 3])
                    yuv += (yuv.empty() ? "" : ",") + std::to_string(e.data.i32[k + 1]) + "x" +
                           std::to_string(e.data.i32[k + 2]);
        printf("id=%s facing=%s orientation=%d jpeg=%dx%d zoom=%.2f-%.2f physical=[%s] yuv=%s\n", id,
               facing, orientation, jpeg.w, jpeg.h, zmin, zmax, phys.c_str(), yuv.c_str());
        ACameraMetadata_free(chars);
    }
    ACameraManager_deleteCameraIdList(ids);
    ACameraManager_delete(mgr);
    return 0;
}

// One open camera with one image reader output and a repeating or single
// capture request.
struct Capture {
    ACameraManager *mgr = nullptr;
    ACameraDevice *dev = nullptr;
    AImageReader *reader = nullptr;
    ANativeWindow *window = nullptr;
    ACaptureSessionOutputContainer *outputs = nullptr;
    ACaptureSessionOutput *output = nullptr;
    ACameraOutputTarget *target = nullptr;
    ACaptureRequest *request = nullptr;
    ACameraCaptureSession *session = nullptr;
    ACameraDevice_StateCallbacks devCb{nullptr, onDisconnected, onError};
    ACameraCaptureSession_stateCallbacks sessCb{nullptr, onSessionClosed, onSessionReady, onSessionActive};

    bool open(const char *id, int32_t w, int32_t h, int32_t format, int maxImages,
              ACameraDevice_request_template tmpl, float zoom, AImageReader_ImageListener *listener) {
        mgr = ACameraManager_create();
        if (!check(ACameraManager_openCamera(mgr, id, &devCb, &dev), "openCamera"))
            return false;
        if (AImageReader_new(w, h, format, maxImages, &reader) != AMEDIA_OK) {
            LOG("AImageReader_new %dx%d failed", w, h);
            return false;
        }
        if (listener)
            AImageReader_setImageListener(reader, listener);
        AImageReader_getWindow(reader, &window);
        ACaptureSessionOutputContainer_create(&outputs);
        ACaptureSessionOutput_create(window, &output);
        ACaptureSessionOutputContainer_add(outputs, output);
        if (!check(ACameraDevice_createCaptureSession(dev, outputs, &sessCb, &session), "createCaptureSession"))
            return false;
        if (!check(ACameraDevice_createCaptureRequest(dev, tmpl, &request), "createCaptureRequest"))
            return false;
        ACameraOutputTarget_create(window, &target);
        ACaptureRequest_addTarget(request, target);
        if (zoom > 0)
            ACaptureRequest_setEntry_float(request, ACAMERA_CONTROL_ZOOM_RATIO, 1, &zoom);
        return true;
    }

    ~Capture() {
        if (session) {
            ACameraCaptureSession_stopRepeating(session);
            ACameraCaptureSession_close(session);
        }
        if (request) ACaptureRequest_free(request);
        if (target) ACameraOutputTarget_free(target);
        if (output) ACaptureSessionOutput_free(output);
        if (outputs) ACaptureSessionOutputContainer_free(outputs);
        if (dev) ACameraDevice_close(dev);
        if (reader) AImageReader_delete(reader);
        if (mgr) ACameraManager_delete(mgr);
    }
};

// A negative zoom means the widest zoom the camera offers (on the logical
// rear camera that switches to the ultrawide sensor).
float resolveZoom(ACameraManager *mgr, const char *id, float zoom) {
    if (zoom >= 0)
        return zoom;
    float z = 1.0f;
    ACameraMetadata *chars = nullptr;
    if (ACameraManager_getCameraCharacteristics(mgr, id, &chars) == ACAMERA_OK) {
        ACameraMetadata_const_entry e{};
        if (ACameraMetadata_getConstEntry(chars, ACAMERA_CONTROL_ZOOM_RATIO_RANGE, &e) == ACAMERA_OK)
            z = e.data.f[0];
        ACameraMetadata_free(chars);
    }
    return z;
}

int still(const char *id, const char *path, float zoom) {
    ACameraManager *probe = ACameraManager_create();
    Size s = largestSize(probe, id, AIMAGE_FORMAT_JPEG);
    zoom = resolveZoom(probe, id, zoom);
    ACameraManager_delete(probe);
    if (!s.w) {
        LOG("camera %s has no JPEG output", id);
        return 1;
    }

    // Let auto exposure and focus settle on a preview stream first.
    {
        Size p{1280, 720};
        Capture warm;
        if (!warm.open(id, p.w, p.h, AIMAGE_FORMAT_YUV_420_888, 4, TEMPLATE_PREVIEW, zoom, nullptr))
            return 1;
        if (!check(ACameraCaptureSession_setRepeatingRequest(warm.session, nullptr, 1, &warm.request, nullptr),
                   "setRepeatingRequest"))
            return 1;
        for (int i = 0; i < 30; i++) {
            usleep(50 * 1000);
            AImage *img = nullptr;
            while (AImageReader_acquireLatestImage(warm.reader, &img) == AMEDIA_OK && img) {
                AImage_delete(img);
                img = nullptr;
            }
        }
    }

    Capture cap;
    if (!cap.open(id, s.w, s.h, AIMAGE_FORMAT_JPEG, 2, TEMPLATE_STILL_CAPTURE, zoom, nullptr))
        return 1;
    if (!check(ACameraCaptureSession_capture(cap.session, nullptr, 1, &cap.request, nullptr), "capture"))
        return 1;
    AImage *img = nullptr;
    for (int i = 0; i < 100 && !img; i++) {
        if (AImageReader_acquireNextImage(cap.reader, &img) != AMEDIA_OK)
            img = nullptr;
        if (!img)
            usleep(50 * 1000);
    }
    if (!img) {
        LOG("no JPEG within 5 s");
        return 1;
    }
    uint8_t *data = nullptr;
    int len = 0;
    AImage_getPlaneData(img, 0, &data, &len);
    FILE *f = fopen(path, "wb");
    if (!f) {
        LOG("cannot write %s: %s", path, strerror(errno));
        AImage_delete(img);
        return 1;
    }
    fwrite(data, 1, len, f);
    fclose(f);
    LOG("wrote %s (%dx%d, %d bytes)", path, s.w, s.h, len);
    AImage_delete(img);
    return 0;
}

// Streaming: frames go to the one connected client; a slow client drops
// frames instead of stalling the camera.
std::mutex gClientLock;
int gClient = -1;
std::atomic<bool> gStop{false};

bool sendAll(int fd, const void *buf, size_t len) {
    const uint8_t *p = static_cast<const uint8_t *>(buf);
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0)
            return false;
        p += n;
        len -= size_t(n);
    }
    return true;
}

// Frame callback (image reader thread): convert to NV12 and send it to the
// current client. A failed send marks the client as gone; the serve loop
// then closes the camera.
std::atomic<bool> gClientGone{false};

void onImage(void *, AImageReader *reader) {
    AImage *img = nullptr;
    if (AImageReader_acquireLatestImage(reader, &img) != AMEDIA_OK || !img)
        return;
    int32_t w = 0, h = 0;
    AImage_getWidth(img, &w);
    AImage_getHeight(img, &h);
    std::vector<uint8_t> frame(size_t(w) * h * 3 / 2);
    uint8_t *y = nullptr, *u = nullptr, *v = nullptr;
    int yl = 0, ul = 0, vl = 0, ys = 0, us = 0, ups = 0;
    AImage_getPlaneData(img, 0, &y, &yl);
    AImage_getPlaneRowStride(img, 0, &ys);
    AImage_getPlaneData(img, 1, &u, &ul);
    AImage_getPlaneData(img, 2, &v, &vl);
    AImage_getPlaneRowStride(img, 1, &us);
    AImage_getPlanePixelStride(img, 1, &ups);
    for (int r = 0; r < h; r++)
        memcpy(&frame[size_t(r) * w], y + size_t(r) * ys, size_t(w));
    uint8_t *uv = &frame[size_t(w) * h];
    if (ups == 2 && v == u + 1) {
        // Already semi-planar UVUV (NV12): copy rows.
        for (int r = 0; r < h / 2; r++)
            memcpy(uv + size_t(r) * w, u + size_t(r) * us, size_t(w));
    } else {
        for (int r = 0; r < h / 2; r++)
            for (int c = 0; c < w / 2; c++) {
                uv[size_t(r) * w + 2 * c] = u[size_t(r) * us + size_t(c) * ups];
                uv[size_t(r) * w + 2 * c + 1] = v[size_t(r) * us + size_t(c) * ups];
            }
    }
    AImage_delete(img);

    std::lock_guard<std::mutex> l(gClientLock);
    if (gClient < 0 || gClientGone)
        return;
    uint32_t hdr[4] = {0x46435242 /* "BRCF" */, uint32_t(w), uint32_t(h), uint32_t(frame.size())};
    if (!sendAll(gClient, hdr, sizeof(hdr)) || !sendAll(gClient, frame.data(), frame.size()))
        gClientGone = true;
}

// Read the request line: "<camera-id> <width> <height> <zoom>".
bool readRequest(int fd, std::string &id, int &w, int &h, float &zoom) {
    char line[128];
    size_t n = 0;
    while (n + 1 < sizeof(line)) {
        ssize_t r = recv(fd, line + n, 1, 0);
        if (r <= 0)
            return false;
        if (line[n] == '\n')
            break;
        n++;
    }
    line[n] = 0;
    char cam[32];
    if (sscanf(line, "%31s %d %d %f", cam, &w, &h, &zoom) != 4 || w <= 0 || h <= 0)
        return false;
    id = cam;
    return true;
}

// Serve one client: open the camera it asks for, stream until it goes
// away, then close the camera.
void serveClient(int c) {
    std::string id;
    int w = 0, h = 0;
    float zoom = 0;
    if (!readRequest(c, id, w, h, zoom)) {
        LOG("bad request");
        close(c);
        return;
    }
    {
        ACameraManager *probe = ACameraManager_create();
        zoom = resolveZoom(probe, id.c_str(), zoom);
        ACameraManager_delete(probe);
    }
    LOG("client wants camera %s %dx%d zoom %.2f", id.c_str(), w, h, zoom);
    {
        std::lock_guard<std::mutex> l(gClientLock);
        gClient = c;
        gClientGone = false;
    }
    {
        AImageReader_ImageListener listener{nullptr, onImage};
        Capture cap;
        if (cap.open(id.c_str(), w, h, AIMAGE_FORMAT_YUV_420_888, 4, TEMPLATE_PREVIEW, zoom, &listener) &&
            check(ACameraCaptureSession_setRepeatingRequest(cap.session, nullptr, 1, &cap.request, nullptr),
                  "setRepeatingRequest")) {
            // The client sends nothing after the request: readable means
            // it closed the connection.
            while (!gClientGone && !gStop) {
                pollfd p{c, POLLIN, 0};
                if (poll(&p, 1, 200) > 0)
                    break;
            }
        }
        std::lock_guard<std::mutex> l(gClientLock);
        gClient = -1;
    }
    close(c);
    LOG("camera %s closed", id.c_str());
}

int serve(const char *sock) {
    int srv = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock);
    unlink(sock);
    if (bind(srv, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) || listen(srv, 4)) {
        LOG("cannot listen on %s: %s", sock, strerror(errno));
        return 1;
    }
    // Access is controlled by the directory the socket lives in.
    chmod(sock, 0666);
    LOG("serving cameras on %s", sock);
    while (!gStop) {
        int c = accept4(srv, nullptr, nullptr, SOCK_CLOEXEC);
        if (c >= 0)
            serveClient(c);
    }
    close(srv);
    return 0;
}

} // namespace

// cameraserver delivers device and session callbacks over binder, so this
// process needs binder threads. ABinderProcess_startThreadPool() is a
// platform API (not in the NDK stubs): look it up at run time.
void startBinderThreads() {
    void *lib = dlopen("libbinder_ndk.so", RTLD_NOW);
    using SetMax = bool (*)(uint32_t);
    using Start = void (*)();
    auto setMax = lib ? reinterpret_cast<SetMax>(dlsym(lib, "ABinderProcess_setThreadPoolMaxThreadCount")) : nullptr;
    auto start = lib ? reinterpret_cast<Start>(dlsym(lib, "ABinderProcess_startThreadPool")) : nullptr;
    if (!start) {
        LOG("ABinderProcess_startThreadPool not found; callbacks will not arrive");
        return;
    }
    if (setMax)
        setMax(4);
    start();
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    startBinderThreads();
    if (argc >= 2 && !strcmp(argv[1], "list"))
        return listCameras();
    if (argc >= 4 && !strcmp(argv[1], "still"))
        return still(argv[2], argv[3], argc >= 5 ? strtof(argv[4], nullptr) : 0.0f);
    if (argc >= 3 && !strcmp(argv[1], "serve"))
        return serve(argv[2]);
    fprintf(stderr, "usage: %s list | still <id> <out.jpg> [zoom] | serve <socket>\n", argv[0]);
    return 2;
}
