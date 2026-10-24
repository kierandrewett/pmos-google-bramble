# Pixel 4a (5G) "bramble": camera feasibility on the postmarketOS port

Status: COMPLETE DRAFT for review. Local kernel/DT findings (sections 1-5), web research (6), options (7), recommendation (8). Nothing tested on hardware: phone offline.
Scope: rear main, rear auxiliary, front. Kernel 4.19.325 (downstream, commit 2af5306961aebce4cc30098c9f12a09f8d906906). Phone is offline; nothing here was tested on hardware.

Paths below are relative to the kernel tree `scratch/camera-research/android_kernel_google_redbull-2af5306961aebce4cc30098c9f12a09f8d906906/` unless stated.

## 1. Camera kernel stack: Qualcomm Spectra CAMX (downstream, out of tree)

The camera stack is not mainline. It lives in `techpack/camera/` as the Qualcomm "CAMX" driver set.

- Build switch: `CONFIG_SPECTRA_CAMERA=m` in `arch/arm64/configs/redbull-gki_defconfig:435` and `redbull_defconfig:446`. Config fragment: `techpack/camera/config/litocamera-gki.conf` (also sets `CONFIG_CAMERA_FW_UPDATE=m`, `CONFIG_CAMERA_GYRO=m`). Makefile picks the `-gki` variant for SM7250 (`techpack/camera/Makefile`).
- Core pieces (all `=m` in stock, see section 3):
  - `cam_req_mgr`: request manager and session/link broker. Exposes a V4L2 video node named `cam-req-mgr` (`drivers/cam_req_mgr/cam_req_mgr_dev.c:575-582`) with a private ioctl `_IOWR('V', BASE_VIDIOC_PRIVATE, struct cam_control)` (`include/uapi/media/cam_req_mgr.h:97`). Opcodes such as `CAM_REQ_MGR_CREATE_SESSION` (`cam_req_mgr.h:251`) and `CAM_REQ_MGR_LINK`.
  - `cam_cpas`: camera power/AXI bus manager. `cam_cdm`: DMA/command engine. `cam_smmu`/`cam_mem_mgr`: IOMMU and buffer handling. `cam_sync`: fence objects (also a V4L2 node, `cam_sync.c:1098`).
  - `cam_isp` with `cam_ife_hw_mgr`, `cam_vfe`, `cam_ife_csid`, `cam_ife_csid17x`/`lite17x`: the IFE (ISP front end) and CSID for SM7250. Packets are built from the UAPI in `include/uapi/media/cam_isp*.h`.
  - `cam_icp` / `cam_a5` / `cam_hfi` / `cam_bps` / `cam_ipe`: ICP block (Cortex-A5 running firmware) that drives the IPE/BPS image processing engines. Also `cam_jpeg`, `cam_fd`, `cam_lrme`.
  - `cam_sensor_module/`: `cam_cci` (I2C master), `cam_csiphy` (MIPI D-PHY), `cam_sensor` (sensor driver), `cam_eeprom`, `cam_actuator` (VCM), `cam_ois`, `cam_flash`, `cam_res_mgr` (regulators), `cam_fw_update` (OIS firmware updater).
- Userspace API: pure ioctl on `/dev/videoN` nodes plus private opcodes. There is no standard V4L2 capture path in CAMX. Frames are delivered through the IFE/CSID to memory buffers managed by `cam_mem_mgr`, not through `videobuf2`. `CONFIG_VIDEO_QCOM_CAMSS` (mainline camss) is `# is not set` in the bramble config (`pmaports/device/downstream/linux-google-bramble/config-google-bramble.aarch64:3219`), and `CONFIG_MSM_CAMERA`/`CONFIG_MSMB_CAMERA` are also off.

## 2. Declared sensors (device tree)

Board files: `arch/arm64/boot/dts/google/`.

- `lito.dtsi:3884` includes `vendor/qcom/camera/lito-camera.dtsi` (generic SoC nodes: `qcom,csiphy` 0-3, `qcom,cci` 0/1, `qcom,cam-req-mgr`, `qcom,cam-isp`, `csid175_200`, `vfe175_130`, `vfe-lite175`, `cam-icp`, `cam-a5`, `cam-ipe`, `cam-bps`, `cam-cpas`, `msm-cam-smmu`, `jpeg`, `fd501`, `lrme`).
- Which sensor file each bramble board uses (from `lito-v2-bramble-*.dtsi`):
  - `pvt`, `evt1_9`, `dvt`, `evt`: `lito-bramble-camera-sensor-v4.dtsi` (`lito-v2-bramble-pvt.dtsi:16`). This is the one that matters for the production phone.
  - `proto1_2`: v3. `proto1_6`: v2. `dev`/`proto1_1`: base `sensor.dtsi`.
- Sensor nodes in `lito-bramble-camera-sensor-v4.dtsi` (line numbers approximate):

| Node | Role | CCI master | CSIPHY | Reset GPIO | MCLK GPIO | Notes |
|---|---|---|---|---|---|---|
| `qcom,cam-sensor@0` (line ~321) | Rear main | cci0 master0 | csiphy0 (`csiphy-sd-index=0`) | 23 | 13 | EEPROM `eeprom_rear`, VCM `actuator_rear`, OIS `ois_rear` (cci-device 0), flash `led_flash_rear`. Rails: slg51000 LDO7 (vio), LDO3 (vana), `camera_ldo` (pm7250b GPIO 11 for VCM) |
| `qcom,cam-sensor@1` (line ~373) | Rear auxiliary | cci0 master1 | csiphy1 | 25 | 14 | EEPROM `eeprom_rear_aux`, flash `led_flash_rear_aux`. **No actuator node** |
| `qcom,cam-sensor@2` (line ~455) | Front | cci1 master0 | csiphy2 | 21 | 15 | EEPROM `eeprom_front`, `sensor-position-roll = 270`, pwm-switch |

- Main rear sensor identity: `techpack/camera/drivers/cam_sensor_module/cam_fw_update/fw_update.c:229,240` and `cam_sensor_core.c:875,983,1025` gate OIS/FW behaviour on `sensor_id == 0x363`, with the comment-free comparison and the OIS slave address `0x76` (`fw_update.c:10`). This matches the Sony IMX363 chip ID. The main rear sensor is therefore IMX363 with a separate OIS controller on the rear main I2C bus.
- Aux rear and front sensor models: **not identified in the tree.** The sensor driver is generic (`qcom,cam-sensor`) and the model comes from the sensor probe ID and per-sensor `*.dtsi` or sensor-library files, which are not in this tree. Web research below.

## 3. Stock Android module load (reference only)

Source: `backup/stock-android-reference/lsmod.txt` (Android kernel 4.19.278, Android 14 build UP1A.231105.001.B2).

- Loaded CAMX stack (all as modules): `cam_req_mgr`, `cam_cpas`, `cam_cdm`, `cam_isp` (+ `cam_ife_hw_mgr`, `cam_vfe`, `cam_ife_csid*`), `cam_icp` (+ `cam_hfi`, `cam_a5`), `cam_bps`, `cam_ipe`, `cam_jpeg`, `cam_fd`, `cam_lrme`, `cam_sensor` (+ `cam_sensor_io`, `cam_sensor_util`), `cam_eeprom`, `cam_ois`, `cam_actuator`, `cam_flash`, `cam_csiphy`, `cam_cci`, `cam_res_mgr`, `cam_smmu_api`, `cam_mem_mgr`, `cam_sync`, `cam_utils`, `cam_context`, `cam_custom_*`, `cam_gyro_core`, `fw_update`, `leds_qpnp_flash_v2`, `camcc_lito`.
- Userspace HAL on stock: `vendor/bin/hw/android.hardware.camera.provider@2.7-service-google` (`vendor-etc/init/android.hardware.camera.provider@2.7-service-google.rc`). Its VINTF manifest declares `android.hardware.camera.provider` v2 AIDL on `internal/0`. Camera feature XMLs present: `android.hardware.camera.front`, `.full`, `.flash-autofocus`, `.concurrent`, `.raw`.
- Vendor firmware: the stock pull has no `/vendor/firmware` and no `/vendor/lib/modules` content (`pull-errors.txt`: "0 files pulled"). So the firmware blobs needed by the modules below were **not captured**.

## 4. Firmware the stack requests

Only three `request_firmware()` call sites exist in the whole `techpack/camera` tree:

- `drivers/cam_icp/icp_hw/a5_hw/a5_core.c:195`: `CAMERA_ICP.elf`. This is the Cortex-A5 image for the ICP block. It is needed when `cam_a5` initialises (`cam_a5_init_hw`). The ICP drives the IPE/BPS engines (`cam_ipe`, `cam_bps`), which handle the offline image processing and the JPEG/encode path. The IFE/CSID capture path itself does not request firmware in this tree. [INFERENCE: IFE raw capture can run without ICP; not verified on hardware.]
- `drivers/cam_sensor_module/cam_ois/cam_ois_core.c:347,392`: `<ois_name>.prog` and `<ois_name>.coeff`. Needed only for the rear main OIS.
- `cam_fw_update/fw_update.c` updates OIS firmware from an embedded blob (`doFWupdate`), not via `request_firmware`.
- The exact `CAMERA_ICP.elf` file for SM7250 is not in the source tree and is not in the stock pull. It must come from a stock Android image (`vendor/firmware/`), which has not been captured.

## 5. Port state (pmaports)

- Kernel config (`pmaports/device/downstream/linux-google-bramble/config-google-bramble.aarch64`): `CONFIG_SPECTRA_CAMERA` is `=m` (line 3222). `CONFIG_MEDIA_SUPPORT`, `CONFIG_V4L2` and `CONFIG_MEDIA_CONTROLLER` are on. `CONFIG_VIDEO_QCOM_CAMSS` is off.
- Kernel package: `APKBUILD` pins `pkgver=4.19.325`, commit `2af5306961aebce4cc30098c9f12a09f8d906906`. The downstream patches are in `pmaports/device/downstream/linux-google-bramble/` (fbmem, msm, touch, haptics, WCN, LED flash). None touch CAMX.
- Modules: `device-google-bramble/modules-load.conf` says "Camera modules are left out." No camera module is loaded on the port. `modprobe.conf` has no camera blacklist.
- Therefore the CAMX modules are built into the package set when `SPECTRA_CAMERA=m` (the config file sets it), but nothing loads them and no firmware is packaged.
- Repo docs: `bramble/README.md:29` lists camera as "Not looked at".

## 6. Web research

Checked 2026-10-23. Searches only; no live browsing of the phone or of pmaports.

- Upstream camss (mainline) only covers older SoCs. The kernel doc says: "The current version of the driver supports the Camera Subsystem found on Qualcomm MSM8916/APQ8016 and MSM8996/APQ8096 processors." https://www.kernel.org/doc/html/latest/admin-guide/media/qcom_camss.html . SM7250 (lito) is not listed.
- Upstream camss is growing, but for newer SoCs. Recent series add SM8750 and Kaanapali (RDI-only paths): https://lkml.iu.edu/2605.1/00517.html , https://lkml.iu.edu/2606.0/02172.html , https://lkml.iu.edu/2605.1/00335.html . No SM7250 camss series found in these searches. [INFERENCE: none exists yet; not exhaustive.]
- postmarketOS SM7250 status (from the postmarketOS/nura SM7250 wiki, read earlier, URL not kept): mainline "In LKML, not merged yet"; camera "Untested"; bramble listed as google-bramble.
- libcamera: FAQ says it supports "a subset of the Qualcomm Snapdragon" (https://libcamera.org/faq.html). The ISP matrix lists "Qualcomm Snapdragon (CamSS)" (https://github.com/libcamera-org/libcamera/blob/master/Documentation/isp-feature-matrix.rst). The early qcom-camss enablement targeted APQ8016 and APQ8096 (https://patchwork.libcamera.org/patch/10045). So libcamera's Qualcomm path is the upstream camss path, not CAMX. No libcamera or other open client for cam_req_mgr/CAMX found.
- Open CAMX userspace: none found. The only client seen is the vendor Android HAL (`android.hardware.camera.provider@2.7-service-google`, section 3).
- Halium / UBports: "libhybris lets Ubuntu Touch use Android HALs for cameras, sensors and more" (https://ubports.com/halium). Droidian uses the same Halium/libhybris approach (https://github.com/droidian/droidian). From an earlier search (URL not kept): Droidian camera uses Qt and a gstreamer1.0-droid element on top of the Android HAL. Not verified on a bramble-class device here.
- Musl/Alpine: libhybris and the Android bionic loader were not checked for musl support. [INFERENCE: this is an open risk. Halium-style setups are glibc or bionic-based.]
- Sensor identity from the web (gsmarena, not verified against hardware): main rear 12.2MP, 1/2.55", OIS, matches IMX363 from section 2; ultrawide 16MP f/2.2 (aux node, no actuator in DT); front 8MP f/2.0. One secondary source says the front is IMX355; not confirmed. Aux and front sensor drivers are not in this tree.

## 7. Options

| # | Option | What it needs | Effort | Risk | Outcome |
|---|---|---|---|---|---|
| 1 | Ship with no camera; mark as unsupported | Nothing. Update `bramble/README.md:29` from "Not looked at" to "Not supported". | Hours | None | No camera. Honest and cheap. |
| 2 | Android camera HAL via libhybris / Halium-style bridge | Stock vendor blobs (`vendor/lib*`, `vendor/firmware/CAMERA_ICP.elf`, sensor libs, `camera.provider` service). A hwbinder/vndbinder path. The downstream CAMX modules loaded and probing on 4.19. A bionic environment that runs on the postmarketOS musl userspace. | High; months, unknown | High: musl/bionic gap, Android init and property services, binder, blob capture, phone offline | Best chance of usable preview and stills, because Qualcomm 3A and tuning come with the HAL. Still tied to vendor blobs. |
| 3 | Direct CAMX client: open userspace on `cam_req_mgr` / IFE / CSIPHY / CCI | Reverse-engineer the UAPI (`include/uapi/media/cam_*.h`). Sensor init register tables for IMX363 and the other two sensors (not in tree). ICP firmware for the IPE/BPS path. Own 3A and ISP tuning. | Very high; many months, expert | Very high | At most raw Bayer dumps or a crude preview. No usable photos without vendor tuning. |
| 4 | Mainline camss port for SM7250 (+ sensor drivers + libcamera pipeline) | New camss gen for lito CSID/IFE/CSIPHY. Sensor drivers for all three sensors, which may not exist upstream. Upstreaming. Software ISP in libcamera. | Very high; a year or more of kernel work | High; no upstream SM7250 work seen | Clean long-term route. Still no 3A/tuning, so a software ISP is needed. |
| 5 | Narrow to one camera (e.g. front or rear main) | Same shared stack (cam_req_mgr, cpas, cci, csiphy, sensor, ICP) as options 2-4. | Saves little | Same as the option chosen | Does not remove the stack cost. Not a separate option. |

## 8. Recommendation

- Verdict: a usable camera is NOT feasible on this kernel and port today. There is no open userspace for the downstream CAMX stack, no mainline SM7250 camss, and no working client in the tree. The only path with known precedent is the vendor Android HAL, and that is unverified on this port.
- Now: option 1. Document camera as unsupported. The tree already has the CAMX modules built (`CONFIG_SPECTRA_CAMERA=m`) but nothing loads them, so the port does not regress.
- If camera is required, run a time-boxed go/no-go spike for option 2 before committing:
  1. When the phone is back online, capture `/vendor/firmware/CAMERA_ICP.elf`, the vendor camera libs and the `camera.provider` service. The stock pull has none of these (section 3).
  2. Load the CAMX modules in dependency order (`cam_req_mgr`, `cam_cpas`, `cam_cdm`, `cam_cci`, `cam_csiphy`, `cam_sensor`, then `cam_isp`/`cam_icp`) and check `dmesg` for sensor probe ID `0x363` and `CAMERA_ICP` loading.
  3. Check the libhybris / bionic question on musl with a trivial HAL open call.
  4. Go only if steps 2 and 3 both pass. Otherwise stop at option 1.
- Do not start option 3: the effort is very high and the result still has no tuned image.
- Watch option 4: track the upstream camss SM7250 status. Revisit when a series appears.
- Blockers: phone offline; no vendor firmware or sensor blobs captured; no hardware test of any module. Everything above is source-based.

