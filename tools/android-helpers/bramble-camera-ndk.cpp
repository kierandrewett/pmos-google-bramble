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
//
// While streaming, the client may send "capture <rotation>\n": the HAL takes
// a full-resolution still (TEMPLATE_STILL_CAPTURE, its own JPEG encoder) in
// the same session, and the JPEG comes back as a frame with the header
// "BRCJ", width, height, JPEG size. rotation is how far the device is turned
// clockwise from portrait (0, 90, 180, 270), for the JPEG orientation.

#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraError.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCaptureRequest.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <android/log.h>

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <poll.h>
#include <dlfcn.h>
#include <unistd.h>

// Log to stderr (command line use) and to logcat (when run as a service).
#define LOG(...) do { fprintf(stderr, "[bramble-camera] " __VA_ARGS__); fputc('\n', stderr); \
    __android_log_print(ANDROID_LOG_INFO, "bramble-camera", __VA_ARGS__); } while (0)

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
// capture request. With a still size, open() also adds a JPEG output to the
// session and a still-capture request that writes to both outputs.
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
    AImageReader *jpegReader = nullptr;
    ANativeWindow *jpegWindow = nullptr;
    ACaptureSessionOutput *jpegOutput = nullptr;
    ACameraOutputTarget *jpegTarget = nullptr;
    ACameraOutputTarget *previewInStill = nullptr;
    ACaptureRequest *still = nullptr;
    ACameraDevice_StateCallbacks devCb{nullptr, onDisconnected, onError};
    ACameraCaptureSession_stateCallbacks sessCb{nullptr, onSessionClosed, onSessionReady, onSessionActive};

    // stillSize: JPEG output size, or 0x0 for none.
    bool open(const char *id, int32_t w, int32_t h, int32_t format, int maxImages,
              ACameraDevice_request_template tmpl, float zoom, AImageReader_ImageListener *listener,
              Size stillSize = {}, AImageReader_ImageListener *stillListener = nullptr) {
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
        if (stillSize.w) {
            if (AImageReader_new(stillSize.w, stillSize.h, AIMAGE_FORMAT_JPEG, 2, &jpegReader) != AMEDIA_OK) {
                LOG("JPEG AImageReader_new %dx%d failed", stillSize.w, stillSize.h);
                return false;
            }
            if (stillListener)
                AImageReader_setImageListener(jpegReader, stillListener);
            AImageReader_getWindow(jpegReader, &jpegWindow);
            ACaptureSessionOutput_create(jpegWindow, &jpegOutput);
            ACaptureSessionOutputContainer_add(outputs, jpegOutput);
        }
        if (!check(ACameraDevice_createCaptureSession(dev, outputs, &sessCb, &session), "createCaptureSession"))
            return false;
        if (!check(ACameraDevice_createCaptureRequest(dev, tmpl, &request), "createCaptureRequest"))
            return false;
        ACameraOutputTarget_create(window, &target);
        ACaptureRequest_addTarget(request, target);
        if (zoom > 0)
            ACaptureRequest_setEntry_float(request, ACAMERA_CONTROL_ZOOM_RATIO, 1, &zoom);
        if (stillSize.w) {
            if (!check(ACameraDevice_createCaptureRequest(dev, TEMPLATE_STILL_CAPTURE, &still),
                       "createCaptureRequest (still)"))
                return false;
            // The preview output keeps the viewfinder running during the shot.
            ACameraOutputTarget_create(window, &previewInStill);
            ACaptureRequest_addTarget(still, previewInStill);
            ACameraOutputTarget_create(jpegWindow, &jpegTarget);
            ACaptureRequest_addTarget(still, jpegTarget);
            if (zoom > 0)
                ACaptureRequest_setEntry_float(still, ACAMERA_CONTROL_ZOOM_RATIO, 1, &zoom);
            const uint8_t quality = 95;
            ACaptureRequest_setEntry_u8(still, ACAMERA_JPEG_QUALITY, 1, &quality);
        }
        return true;
    }

    ~Capture() {
        if (session) {
            ACameraCaptureSession_stopRepeating(session);
            ACameraCaptureSession_close(session);
        }
        if (still) ACaptureRequest_free(still);
        if (jpegTarget) ACameraOutputTarget_free(jpegTarget);
        if (previewInStill) ACameraOutputTarget_free(previewInStill);
        if (request) ACaptureRequest_free(request);
        if (target) ACameraOutputTarget_free(target);
        if (jpegOutput) ACaptureSessionOutput_free(jpegOutput);
        if (output) ACaptureSessionOutput_free(output);
        if (outputs) ACaptureSessionOutputContainer_free(outputs);
        if (dev) ACameraDevice_close(dev);
        if (jpegReader) AImageReader_delete(jpegReader);
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

// Streaming: frames go to the one connected client. The camera callbacks
// never write to the socket themselves: they hand frames to the outbox, and a
// sender thread writes them. A blocked callback would hold the HAL's buffers
// and stall the camera pipeline until the HAL aborts (CamX "WaitOutFlush").
// A slow client therefore loses preview frames (the outbox keeps only the
// newest), never stills.
std::atomic<bool> gStop{false};
std::atomic<bool> gClientGone{false};

struct Outbox {
    std::mutex m;
    std::condition_variable cv;
    std::vector<uint8_t> frame;          // newest NV12 frame not yet sent
    uint32_t frameW = 0, frameH = 0;
    bool haveFrame = false;
    std::vector<std::vector<uint8_t>> stills;
    std::vector<std::pair<uint32_t, uint32_t>> stillSizes;
    bool stop = false;
    uint64_t replaced = 0;               // frames overwritten before sending
} gOut;

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

bool sendFrame(int fd, uint32_t magic, uint32_t w, uint32_t h, const std::vector<uint8_t> &data) {
    uint32_t hdr[4] = {magic, w, h, uint32_t(data.size())};
    return sendAll(fd, hdr, sizeof(hdr)) && sendAll(fd, data.data(), data.size());
}

// Sender thread for client fd: stills first, then the newest frame.
void sendLoop(int fd) {
    std::vector<uint8_t> frame;
    for (;;) {
        std::vector<uint8_t> still;
        uint32_t w = 0, h = 0;
        bool isStill = false;
        {
            std::unique_lock<std::mutex> l(gOut.m);
            gOut.cv.wait(l, [] { return gOut.stop || gOut.haveFrame || !gOut.stills.empty(); });
            if (gOut.stop)
                return;
            if (!gOut.stills.empty()) {
                still.swap(gOut.stills.front());
                w = gOut.stillSizes.front().first;
                h = gOut.stillSizes.front().second;
                gOut.stills.erase(gOut.stills.begin());
                gOut.stillSizes.erase(gOut.stillSizes.begin());
                isStill = true;
            } else {
                frame.swap(gOut.frame);
                w = gOut.frameW;
                h = gOut.frameH;
                gOut.haveFrame = false;
            }
        }
        bool ok = isStill ? sendFrame(fd, 0x4a435242 /* "BRCJ" */, w, h, still)
                          : sendFrame(fd, 0x46435242 /* "BRCF" */, w, h, frame);
        if (!ok) {
            gClientGone = true;
            return;
        }
    }
}

// Frame callback (image reader thread): convert to NV12 and put it in the
// outbox.
void onImage(void *, AImageReader *reader) {
    AImage *img = nullptr;
    if (AImageReader_acquireLatestImage(reader, &img) != AMEDIA_OK || !img)
        return;
    int32_t w = 0, h = 0;
    AImage_getWidth(img, &w);
    AImage_getHeight(img, &h);
    // Reused across frames: the image reader calls back from one thread,
    // and the outbox swaps buffers instead of copying.
    static std::vector<uint8_t> frame;
    frame.resize(size_t(w) * h * 3 / 2);
    uint8_t *y = nullptr, *u = nullptr, *v = nullptr;
    int yl = 0, ul = 0, vl = 0, ys = 0, us = 0, vs = 0, ups = 0;
    AImage_getPlaneData(img, 0, &y, &yl);
    AImage_getPlaneRowStride(img, 0, &ys);
    AImage_getPlaneData(img, 1, &u, &ul);
    AImage_getPlaneData(img, 2, &v, &vl);
    AImage_getPlaneRowStride(img, 1, &us);
    AImage_getPlaneRowStride(img, 2, &vs);
    AImage_getPlanePixelStride(img, 1, &ups);
    for (int r = 0; r < h; r++)
        memcpy(&frame[size_t(r) * w], y + size_t(r) * ys, size_t(w));
    uint8_t *uv = &frame[size_t(w) * h];
    if (ups == 2 && (v == u + 1 || u == v + 1)) {
        // Semi-planar chroma. NV12 (V follows U) copies rows; NV21 (U
        // follows V) also swaps each byte pair into NV12 order.
        const bool nv21 = u == v + 1;
        const uint8_t *src = nv21 ? v : u;
        const int sstride = nv21 ? vs : us;
        for (int r = 0; r < h / 2; r++) {
            const uint8_t *s = src + size_t(r) * sstride;
            uint8_t *d = uv + size_t(r) * w;
            if (!nv21) {
                memcpy(d, s, size_t(w));
                continue;
            }
            for (int c = 0; c < w; c += 2) {
                uint16_t p;
                memcpy(&p, s + c, 2);
                p = __builtin_bswap16(p);
                memcpy(d + c, &p, 2);
            }
        }
    } else {
        for (int r = 0; r < h / 2; r++)
            for (int c = 0; c < w / 2; c++) {
                uv[size_t(r) * w + 2 * c] = u[size_t(r) * us + size_t(c) * ups];
                uv[size_t(r) * w + 2 * c + 1] = v[size_t(r) * us + size_t(c) * ups];
            }
    }
    AImage_delete(img);

    {
        std::lock_guard<std::mutex> l(gOut.m);
        if (gOut.haveFrame)
            gOut.replaced++;
        gOut.frame.swap(frame);
        gOut.frameW = uint32_t(w);
        gOut.frameH = uint32_t(h);
        gOut.haveFrame = true;
    }
    gOut.cv.notify_one();
}

// JPEG callback for stills: queue the JPEG, without the HAL's padding after
// the end-of-image marker.
void onJpeg(void *, AImageReader *reader) {
    AImage *img = nullptr;
    if (AImageReader_acquireNextImage(reader, &img) != AMEDIA_OK || !img)
        return;
    int32_t w = 0, h = 0;
    uint8_t *data = nullptr;
    int len = 0;
    AImage_getWidth(img, &w);
    AImage_getHeight(img, &h);
    AImage_getPlaneData(img, 0, &data, &len);
    int end = len;
    while (end > 2 && !(data[end - 2] == 0xff && data[end - 1] == 0xd9))
        end--;
    if (end <= 2)
        end = len;
    std::vector<uint8_t> jpeg(data, data + end);
    AImage_delete(img);
    {
        std::lock_guard<std::mutex> l(gOut.m);
        gOut.stills.push_back(std::move(jpeg));
        gOut.stillSizes.emplace_back(uint32_t(w), uint32_t(h));
    }
    gOut.cv.notify_one();
    LOG("still %dx%d, %d bytes", w, h, end);
}

// Sensor orientation (degrees) and facing of a camera.
void sensorInfo(ACameraManager *mgr, const char *id, int &orientation, bool &front) {
    orientation = 0;
    front = false;
    ACameraMetadata *chars = nullptr;
    if (ACameraManager_getCameraCharacteristics(mgr, id, &chars) != ACAMERA_OK)
        return;
    ACameraMetadata_const_entry e{};
    if (ACameraMetadata_getConstEntry(chars, ACAMERA_SENSOR_ORIENTATION, &e) == ACAMERA_OK)
        orientation = e.data.i32[0];
    if (ACameraMetadata_getConstEntry(chars, ACAMERA_LENS_FACING, &e) == ACAMERA_OK)
        front = e.data.u8[0] == ACAMERA_LENS_FACING_FRONT;
    ACameraMetadata_free(chars);
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
// away, then close the camera. "capture <rotation>" lines take stills.
void serveClient(int c) {
    std::string id;
    int w = 0, h = 0;
    float zoom = 0;
    if (!readRequest(c, id, w, h, zoom)) {
        LOG("bad request");
        close(c);
        return;
    }
    Size stillSize;
    int sensorOrientation = 0;
    bool front = false;
    {
        ACameraManager *probe = ACameraManager_create();
        zoom = resolveZoom(probe, id.c_str(), zoom);
        stillSize = largestSize(probe, id.c_str(), AIMAGE_FORMAT_JPEG);
        sensorInfo(probe, id.c_str(), sensorOrientation, front);
        ACameraManager_delete(probe);
    }
    LOG("client wants camera %s %dx%d zoom %.2f, stills %dx%d", id.c_str(), w, h, zoom, stillSize.w,
        stillSize.h);
    {
        std::lock_guard<std::mutex> l(gOut.m);
        gOut.frame.clear();
        gOut.haveFrame = false;
        gOut.stills.clear();
        gOut.stillSizes.clear();
        gOut.stop = false;
        gOut.replaced = 0;
    }
    gClientGone = false;
    std::thread sender(sendLoop, c);
    {
        AImageReader_ImageListener listener{nullptr, onImage};
        AImageReader_ImageListener jpegListener{nullptr, onJpeg};
        auto cap = std::make_unique<Capture>();
        bool ok = cap->open(id.c_str(), w, h, AIMAGE_FORMAT_YUV_420_888, 4, TEMPLATE_PREVIEW, zoom, &listener,
                            stillSize, &jpegListener);
        if (!ok) {
            // Stream without stills rather than not at all if the camera
            // does not take this stream combination.
            LOG("camera %s: no session with a still output, trying preview only", id.c_str());
            cap = std::make_unique<Capture>();
            ok = cap->open(id.c_str(), w, h, AIMAGE_FORMAT_YUV_420_888, 4, TEMPLATE_PREVIEW, zoom, &listener);
        }
        if (ok && check(ACameraCaptureSession_setRepeatingRequest(cap->session, nullptr, 1, &cap->request, nullptr),
                        "setRepeatingRequest")) {
            std::string line;
            while (!gClientGone && !gStop) {
                pollfd p{c, POLLIN, 0};
                if (poll(&p, 1, 200) <= 0)
                    continue;
                char ch;
                if (recv(c, &ch, 1, 0) <= 0)
                    break; // the client closed the connection
                if (ch != '\n') {
                    if (line.size() < 64)
                        line += ch;
                    continue;
                }
                int rotation = 0;
                if (sscanf(line.c_str(), "capture %d", &rotation) == 1 && cap->still) {
                    // Android's rule for JPEG_ORIENTATION: the back camera
                    // turns with the device, the front camera against it.
                    int32_t o = front ? (sensorOrientation - rotation + 360) % 360
                                      : (sensorOrientation + rotation) % 360;
                    ACaptureRequest_setEntry_i32(cap->still, ACAMERA_JPEG_ORIENTATION, 1, &o);
                    check(ACameraCaptureSession_capture(cap->session, nullptr, 1, &cap->still, nullptr), "capture");
                } else {
                    LOG("unknown command: %s", line.c_str());
                }
                line.clear();
            }
        } else {
            LOG("camera %s could not stream", id.c_str());
        }
    }
    {
        std::lock_guard<std::mutex> l(gOut.m);
        gOut.stop = true;
        LOG("camera %s closed, %llu frames skipped for a slow client", id.c_str(),
            (unsigned long long)gOut.replaced);
    }
    gOut.cv.notify_one();
    sender.join();
    close(c);
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
