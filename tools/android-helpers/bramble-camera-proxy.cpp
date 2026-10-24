// bramble-camera-proxy: stand-in for system_server's CameraServiceProxy
// ("media.camera.proxy") in the google-bramble Android HAL container.
//
// cameraserver asks this service whether the camera is disabled by device
// policy before every connectDevice(); without it the answer is "disabled"
// and every open fails with PERMISSION_DENIED. There is no system_server in
// the container, so answer the five calls of
// frameworks/av/camera/aidl/android/hardware/ICameraServiceProxy.aidl
// (Android 14) with the defaults: nothing overridden, camera enabled.

#include <android/binder_ibinder.h>
#include <android/binder_status.h>

#include <cstdio>

#include <dlfcn.h>

namespace {

constexpr const char *kDescriptor = "android.hardware.ICameraServiceProxy";
constexpr const char *kService = "media.camera.proxy";

enum : transaction_code_t {
    kPingForUserUpdate = FIRST_CALL_TRANSACTION,  // oneway
    kNotifyCameraState,                           // oneway
    kGetRotateAndCropOverride,                    // int
    kGetAutoframingOverride,                      // int
    kIsCameraDisabled,                            // boolean
};

void *onCreate(void *args) { return args; }
void onDestroy(void *) {}

binder_status_t writeOk(AParcel *out) {
    AStatus *ok = AStatus_newOk();
    binder_status_t s = AParcel_writeStatusHeader(out, ok);
    AStatus_delete(ok);
    return s;
}

binder_status_t onTransact(AIBinder *, transaction_code_t code, const AParcel *, AParcel *out) {
    switch (code) {
    case kPingForUserUpdate:
    case kNotifyCameraState:
        return STATUS_OK;
    case kGetRotateAndCropOverride:  // ANDROID_SCALER_ROTATE_AND_CROP_NONE
    case kGetAutoframingOverride:    // ANDROID_CONTROL_AUTOFRAMING_OFF
        if (binder_status_t s = writeOk(out); s != STATUS_OK)
            return s;
        return AParcel_writeInt32(out, 0);
    case kIsCameraDisabled:
        if (binder_status_t s = writeOk(out); s != STATUS_OK)
            return s;
        return AParcel_writeBool(out, false);
    default:
        return STATUS_UNKNOWN_TRANSACTION;
    }
}

} // namespace

int main() {
    AIBinder_Class *cls = AIBinder_Class_define(kDescriptor, onCreate, onDestroy, onTransact);
    AIBinder *binder = AIBinder_new(cls, nullptr);
    // AServiceManager_addService() and ABinderProcess_joinThreadPool() are
    // in libbinder_ndk but not in the NDK headers or stubs (platform API),
    // so look them up at run time.
    using AddService = binder_exception_t (*)(AIBinder *, const char *);
    void *lib = dlopen("libbinder_ndk.so", RTLD_NOW);
    auto addService = lib ? reinterpret_cast<AddService>(dlsym(lib, "AServiceManager_addService")) : nullptr;
    if (!addService) {
        fprintf(stderr, "[bramble-camera-proxy] AServiceManager_addService not found\n");
        return 1;
    }
    binder_exception_t e = addService(binder, kService);
    if (e != EX_NONE) {
        fprintf(stderr, "[bramble-camera-proxy] addService(%s) failed: %d\n", kService, e);
        return 1;
    }
    fprintf(stderr, "[bramble-camera-proxy] serving %s\n", kService);
    using Join = void (*)();
    auto join = reinterpret_cast<Join>(dlsym(lib, "ABinderProcess_joinThreadPool"));
    if (!join) {
        fprintf(stderr, "[bramble-camera-proxy] ABinderProcess_joinThreadPool not found\n");
        return 1;
    }
    join();
    return 0;
}
