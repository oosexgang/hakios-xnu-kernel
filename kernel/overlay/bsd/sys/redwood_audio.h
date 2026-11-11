/*
 * /dev/redwood-audio: the sound iOS plays, on its way to whatever can make it heard.
 *
 * iOS expects its output devices from Apple's audio drivers through IOAudio2; neither exists
 * here. Instead the audio server (mediaserverd) is given a plug-in of ours that presents one
 * stereo output device, and the plug-in writes what the server has mixed for that device
 * here. In the kernel the samples go to a sink -- a driver for something that plays them --
 * and, if asked for, into a recording that outlives the kernel (see dev/redwood_audio.c).
 *
 *   write(): whole frames in the format below. Never blocks: the writer is the audio server's
 *            real-time thread, which paces itself (48000 frames a second while anything
 *            plays, nothing at all otherwise). What the sink cannot take is dropped.
 */
#ifndef _SYS_REDWOOD_AUDIO_H_
#define _SYS_REDWOOD_AUDIO_H_

#include <stdint.h>

#define REDWOOD_AUDIO_DEVICE    "/dev/redwood-audio"

/* The one format: 16-bit signed little-endian samples, left then right. */
#define REDWOOD_AUDIO_RATE      48000
#define REDWOOD_AUDIO_CHANNELS  2
#define REDWOOD_AUDIO_FRAME     4       /* bytes */

/*
 * ioctl(REDWOOD_AUDIO_SET_VOLUME): the speakers' volume, as iOS sets it on the plug-in's volume
 * control -- how many eighths of a dB under the loudest they may play, and whether they are
 * muted (iOS's volume at 0). The loudest is the kernel's to say (on the phone: Android's own
 * level less sound_below, redwood_sound.c); the plug-in only says how far under it.
 */
#include <sys/ioccom.h>

struct redwood_audio_volume {
	uint32_t below;                 /* eighths of a dB */
	uint32_t muted;
};
#define REDWOOD_AUDIO_SET_VOLUME _IOW('r', 1, struct redwood_audio_volume)

#ifdef KERNEL
#include <stdbool.h>

/*
 * For drivers. A sink is offered frames as they are written and says how many it took (the
 * rest are lost); it is called from the writer's thread and must not wait.
 */
typedef uint32_t (*redwood_audio_sink_t)(const int16_t *frames, uint32_t count);

void redwood_audio_set_sink(const char *name, redwood_audio_sink_t sink);
/* The volume iOS has set last (REDWOOD_AUDIO_SET_VOLUME): false if it has set none; *generation counts the settings. */
bool redwood_audio_volume(uint32_t *below, bool *muted, uint32_t *generation);
void redwood_audio_init(void);
void redwood_virtio_sound_init(void);   /* boot-arg virtio_sound=1: QEMU's virtio sound device */
void redwood_speaker_init(void);        /* boot-arg speaker_probe=N: a look at the phone's speaker amplifiers */
void redwood_haptic_init(void);         /* boot-arg haptic=N: the phone's vibration motor (bsd/dev/redwood_haptic.c) */
void redwood_haptic_quiesce(void);      /* its chip back into reset: at the run's end and on a panic's way out */
void redwood_haptic_pretend(uint32_t peak); /* a test's stand-in for iOS's waveform: as if a write of this loudness came */
void redwood_torch_init(void);          /* boot-arg torch=N: the phone's flashlight (bsd/dev/redwood_torch.c) */
void redwood_torch_quiesce(void);       /* the light off: at the run's end and on a panic's way out */
void redwood_camera_init(void);         /* boot-arg cam=N: the phone's cameras (bsd/dev/redwood_camera.c) */
void redwood_camera_quiesce(void);      /* whatever of them was switched on, off: at the run's end and on a panic's way out */

/*
 * The phone's speakers (bsd/dev/redwood_speaker.c): its two Awinic amplifiers, set up as
 * Android's driver sets them up, from Android's own configuration file (see there).
 */
bool redwood_speaker_prepare(const uint8_t *acf, uint64_t size);       /* found, reset, configured: powered down, muted */
bool redwood_speaker_start(void);       /* powered up and unmuted at the lowest volume: only while the I2S clock runs */
bool redwood_speaker_volume(uint32_t below, uint32_t step_ms); /* below Android's level, in eighths of a dB; 6 dB a step */
bool redwood_speaker_healthy(void);     /* no fault reported; on one, both are powered down at once */
void redwood_speaker_stop(void);        /* faded out, muted, powered down, in Android's order */
void redwood_speaker_quiesce(void);     /* powered down at once, without sleeping: on a panic's way out */
/* the amplifiers' own temperature and battery voltage read (every second while they play): the reduction they call for */
uint32_t redwood_speaker_monitor(bool tell);

/*
 * The speakers' guard (bsd/dev/redwood_guard.c): what the samples go through in the feeder on their
 * way to the amplifiers, for want of the DSP's speaker protection. Levels in eighths of a dB.
 */
#define REDWOOD_GUARD_SILENT    0xffffffffu     /* the amplifiers are off: nothing reaches the speakers */
struct redwood_guard_report {
	uint32_t blocks, rms_blocks, fast_blocks, slow_blocks;  /* blocks done, and in how many each average held the level down */
	uint32_t limited_frames;        /* frames in which the peak limiter turned the level down */
	uint32_t peak_in, peak_out;     /* the largest sample that came in and that went out */
	uint32_t least_peak_gain;       /* the peak limiter's least gain, in 1/65536 */
	uint64_t least_gain, gain;      /* the power guard's (with the monitor's) least gain and the latest, Q30 */
	uint64_t rms, fast, slow;       /* the averages now, the louder speaker's: Q30 of a full-scale square wave at Android's level */
};
#define REDWOOD_GUARD_MAX_GAIN  (12 * 8)        /* the most gain before the guard: 12 dB */
void redwood_guard_configure(bool hpf, uint32_t gain);         /* the high-pass filter; `gain' before everything */
void redwood_guard_attenuation(uint32_t below);                 /* the amplifiers' level under Android's (or REDWOOD_GUARD_SILENT):
                                                                 * what the limits are reckoned from */
void redwood_guard_monitor(uint32_t below);                     /* a reduction asked for by redwood_speaker_monitor() */
void redwood_guard_process(int16_t *frames, uint32_t count);    /* stereo frames, in place */
void redwood_guard_take_report(struct redwood_guard_report *report);   /* what happened since the last call */
int redwood_guard_decibels(uint64_t ratio);                     /* 10 log10 of a Q30 ratio, in tenths of a dB */
/* boot-arg rec_sound_fed=1: the recording (rec_sound=N) has what the guard let through instead of what iOS wrote */
bool redwood_audio_recording_fed(void);
void redwood_audio_record_fed(const int16_t *frames, uint32_t count);

/*
 * The audio DSP's services, for the sound's path (bsd/dev/redwood_adsp.c, bsd/dev/redwood_sound.c).
 * redwood_adsp_apr() sends one APR packet to a service of the DSP (AFE 4, ASM 7, ADM 8) with the
 * given port and token (0: one of its own) and, with wait_ms, waits for the packet that carries
 * that token back. What is not waited for goes to the handler of events, if there is one and it
 * takes it (returns true). The handler is called on the link's thread and must not wait.
 */
struct redwood_apr_answer {
	uint32_t opcode, token, length;         /* length: of the payload, in bytes */
	uint32_t words[64];
};
typedef bool (*redwood_apr_events_t)(uint8_t service, uint16_t port, uint32_t opcode, uint32_t token, const uint32_t *payload,
    uint32_t length);

bool redwood_adsp_apr(uint8_t service, uint16_t port, uint32_t token, uint32_t opcode, const void *payload, uint32_t length,
    uint32_t wait_ms, struct redwood_apr_answer *answer);
void redwood_adsp_apr_events(redwood_apr_events_t handler);
const uint8_t *redwood_adsp_fw_file(const char *name, uint64_t *size);        /* /fw/name, read whole; give back with: */
void redwood_adsp_fw_free(const uint8_t *data, uint64_t size);

void redwood_sound_dsp_up(void);        /* the DSP's audio services are up (adsp=1): boot-arg sound=N begins */
void redwood_sound_dsp_down(void);      /* before the DSP is shut down at the run's end: everything of the sound's stopped */
void redwood_sound_quiesce(void);       /* on a panic's way out: the amplifiers off, without sleeping */
/* the microphones' part of the memory the DSP sees (redwood_sound.c): false if it cannot be had; given back with _put() */
bool redwood_audio_dsp_memory(uintptr_t *memory, uint64_t *dsp_address, uint32_t *bytes);
void redwood_audio_dsp_memory_put(void);
/* a second handler of the DSP's events (the microphones'), asked when the first does not take one */
void redwood_adsp_apr_events_second(redwood_apr_events_t handler);
/* boot-arg rec_sound_mic=1: the recording (rec_sound=N) has what the microphone hears (given with redwood_audio_record_fed()) */
bool redwood_audio_recording_mic(void);
void redwood_mic_dsp_up(void);          /* the DSP's audio services are up (adsp=1): boot-arg mic=N begins */
void redwood_mic_dsp_down(void);        /* before the DSP is shut down at the run's end: what the microphones' path asked for given back */
#endif

#endif /* _SYS_REDWOOD_AUDIO_H_ */
