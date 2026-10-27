// SPDX-License-Identifier: GPL-3.0-or-later
//
// Send the stock audio calibration (ACDB) for a voice call to the DSP.
//
// The kernel's voice path (msm-pcm-voice) needs calibration blocks for the
// voice processing, stream and volume modules. Android's audio HAL loads
// them from /vendor/etc/acdbdata with libacdbloader, which registers them
// through /dev/msm_audio_cal. Nothing does that on postmarketOS, so a call
// connects but no audio goes either way ("voice_get_cal: No cal data").
//
// The kernel frees the blocks when the last process closes
// /dev/msm_audio_cal, so this program stays running while the calibration is
// needed.
//
//   bramble-acdb serve
//
// It loads the ACDB files, then reads lines from standard input and exits at
// the end of input:
//
//   voice RX_ACDB_ID TX_ACDB_ID   send the voice calibration for these devices
//
// Device ids are the acdb_id values in audio_platform_info_*.xml: for a call
// on the speaker 14 and 19, on the earpiece 7 and 4. Sending again replaces
// the calibration, as the HAL does when the output device changes.
#include <dlfcn.h>
#include <fcntl.h>
#include <sound/asound.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

struct ListNode {
	ListNode *next;
	ListNode *prev;
};

using InitV3 = int (*)(const char *, char *, ListNode *);
using SendVoiceCal = void (*)(int, int);
using SendCustomTop = int (*)(void);

constexpr int kCvdVersionSize = 100;

bool readSoundCard(int fd, char *name, size_t nameSize, char *cvd, size_t cvdSize)
{
	snd_ctl_card_info info;
	memset(&info, 0, sizeof(info));
	if (ioctl(fd, SNDRV_CTL_IOCTL_CARD_INFO, &info) < 0) {
		perror("CARD_INFO");
		return false;
	}
	snprintf(name, nameSize, "%s", reinterpret_cast<char *>(info.name));

	snd_ctl_elem_value value;
	memset(&value, 0, sizeof(value));
	value.id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	snprintf(reinterpret_cast<char *>(value.id.name), sizeof(value.id.name), "CVD Version");
	if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &value) < 0) {
		perror("read CVD Version");
		return false;
	}
	memset(cvd, 0, cvdSize);
	memcpy(cvd, value.value.bytes.data, cvdSize - 1 < 512 ? cvdSize - 1 : 512);
	return true;
}

}  // namespace

int main(int argc, char **argv)
{
	if (argc != 2 || strcmp(argv[1], "serve") != 0) {
		fprintf(stderr, "usage: %s serve\n", argv[0]);
		return 2;
	}

	int ctl = open("/dev/snd/controlC0", O_RDWR);
	if (ctl < 0) {
		perror("open controlC0");
		return 1;
	}
	char cardName[64];
	char *cvd = static_cast<char *>(calloc(1, kCvdVersionSize));
	if (!readSoundCard(ctl, cardName, sizeof(cardName), cvd, kCvdVersionSize))
		return 1;
	fprintf(stderr, "bramble-acdb: card %s, CVD %s\n", cardName, cvd);

	void *lib = dlopen("libacdbloader.so", RTLD_NOW);
	if (!lib) {
		fprintf(stderr, "dlopen libacdbloader.so: %s\n", dlerror());
		return 1;
	}
	auto init = reinterpret_cast<InitV3>(dlsym(lib, "acdb_loader_init_v3"));
	auto sendVoice = reinterpret_cast<SendVoiceCal>(dlsym(lib, "acdb_loader_send_voice_cal"));
	auto sendTop = reinterpret_cast<SendCustomTop>(dlsym(lib, "acdb_loader_send_common_custom_topology"));
	if (!init || !sendVoice) {
		fprintf(stderr, "missing symbols in libacdbloader.so\n");
		return 1;
	}

	ListNode metaKeys;
	metaKeys.next = metaKeys.prev = &metaKeys;
	int ret = init(cardName, cvd, &metaKeys);
	if (ret != 0) {
		fprintf(stderr, "bramble-acdb: acdb_loader_init_v3 failed: %d\n", ret);
		return 1;
	}
	if (sendTop)
		sendTop();
	fprintf(stderr, "bramble-acdb: ready\n");

	char line[128];
	while (fgets(line, sizeof(line), stdin)) {
		int rx, tx;
		if (sscanf(line, "voice %d %d", &rx, &tx) == 2) {
			sendVoice(rx, tx);
			fprintf(stderr, "bramble-acdb: voice calibration sent for rx %d tx %d\n", rx, tx);
		} else {
			fprintf(stderr, "bramble-acdb: unknown command: %s", line);
		}
	}
	return 0;
}
