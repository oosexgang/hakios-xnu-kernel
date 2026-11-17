/*
 * /dev/hakios-audio: the sound Darwin plays, on its way to whatever can make it heard.
 *
 * Darwin expects its output devices from Apple's audio drivers through IOAudio2; neither exists
 * here. Instead the audio server (mediaserverd) is given a plug-in of ours that presents one
 * stereo output device, and the plug-in writes what the server has mixed for that device
 * here. In the kernel the samples go to a sink -- a driver for something that plays them --
 * and, if asked for, into a recording that outlives the kernel (see dev/board/audio.c).
 *
 *   write(): whole frames in the format below. Never blocks: the writer is the audio server's
 *            real-time thread, which paces itself (48000 frames a second while anything
 *            plays, nothing at all otherwise). What the sink cannot take is dropped.
 */
#ifndef _SYS_HAKIOS_AUDIO_H_
#define _SYS_HAKIOS_AUDIO_H_

#include <stdint.h>

#define HAKIOS_AUDIO_DEVICE    "/dev/hakios-audio"

/* The one format: 16-bit signed little-endian samples, left then right. */
#define HAKIOS_AUDIO_RATE      48000
#define HAKIOS_AUDIO_CHANNELS  2
#define HAKIOS_AUDIO_FRAME     4       /* bytes */

/*
 * ioctl(HAKIOS_AUDIO_SET_VOLUME): the speakers' volume, as Darwin sets it on the plug-in's volume
 * control -- how many eighths of a dB under the loudest they may play, and whether they are
 * muted (Darwin's volume at 0). The loudest is the kernel's to say (on the phone: Android's own
 * level less sound_below, board/sound.c); the plug-in only says how far under it.
 */
#include <sys/ioccom.h>

struct hakios_audio_volume {
	uint32_t below;                 /* eighths of a dB, at most HAKIOS_AUDIO_BELOW_MOST */
	uint32_t muted;
};
#define HAKIOS_AUDIO_BELOW_MOST (120 * 8)       /* further under than any amplifier goes (EINVAL past it) */
_Static_assert(sizeof(struct hakios_audio_volume) == 8, "the volume is 8 bytes (the ioctl's number carries the size)");
#define HAKIOS_AUDIO_SET_VOLUME _IOW('r', 1, struct hakios_audio_volume)

#ifdef KERNEL
#include <stdbool.h>

/*
 * For drivers. A sink is offered frames as they are written and says how many it took (the
 * rest are lost); it is called from the writer's thread and must not wait.
 */
typedef uint32_t (*hakios_audio_sink_t)(const int16_t *frames, uint32_t count);

void hakios_audio_set_sink(const char *name, hakios_audio_sink_t sink);
/* The volume Darwin has set last (HAKIOS_AUDIO_SET_VOLUME): false if it has set none; *generation counts the settings. */
bool hakios_audio_volume(uint32_t *below, bool *muted, uint32_t *generation);
void hakios_audio_init(void);
void hakios_virtio_sound_init(void);   /* boot-arg virtio_sound=1: QEMU's virtio sound device */
void hakios_speaker_init(void);        /* boot-arg speaker_probe=N: a look at the phone's speaker amplifiers */

/*
 * The phone's speakers (bsd/dev/board/speaker.c): its two Awinic amplifiers, set up as
 * Android's driver sets them up, from Android's own configuration file (see there).
 */
bool hakios_speaker_prepare(const uint8_t *acf, uint64_t size);       /* found, reset, configured: powered down, muted */
bool hakios_speaker_start(void);       /* powered up and unmuted at the lowest volume: only while the I2S clock runs */
bool hakios_speaker_volume(uint32_t below, uint32_t step_ms); /* below Android's level, in eighths of a dB; 6 dB a step */
bool hakios_speaker_healthy(void);     /* no fault reported; on one, both are powered down at once */
void hakios_speaker_stop(void);        /* faded out, muted, powered down, in Android's order */
void hakios_speaker_quiesce(void);     /* powered down at once, without sleeping: on a panic's way out */

/*
 * The audio DSP's services, for the sound's path (bsd/dev/board/adsp.c, bsd/dev/board/sound.c).
 * hakios_adsp_apr() sends one APR packet to a service of the DSP (AFE 4, ASM 7, ADM 8) with the
 * given port and token (0: one of its own) and, with wait_ms, waits for the packet that carries
 * that token back. What is not waited for goes to the handler of events, if there is one and it
 * takes it (returns true). The handler is called on the link's thread and must not wait.
 */
struct hakios_apr_answer {
	uint32_t opcode, token, length;         /* length: of the payload, in bytes */
	uint32_t words[64];
};
typedef bool (*hakios_apr_events_t)(uint8_t service, uint16_t port, uint32_t opcode, uint32_t token, const uint32_t *payload,
    uint32_t length);

bool hakios_adsp_apr(uint8_t service, uint16_t port, uint32_t token, uint32_t opcode, const void *payload, uint32_t length,
    uint32_t wait_ms, struct hakios_apr_answer *answer);
void hakios_adsp_apr_events(hakios_apr_events_t handler);
const uint8_t *hakios_adsp_fw_file(const char *name, uint64_t *size);        /* /fw/name, read whole; give back with: */
void hakios_adsp_fw_free(const uint8_t *data, uint64_t size);

void hakios_sound_dsp_up(void);        /* the DSP's audio services are up (adsp=1): boot-arg sound=N begins */
void hakios_sound_dsp_down(void);      /* before the DSP is shut down at the run's end: everything of the sound's stopped */
void hakios_sound_quiesce(void);       /* on a panic's way out: the amplifiers off, without sleeping */
#endif

#endif /* _SYS_HAKIOS_AUDIO_H_ */
