/*
 * What the rest of the kernel calls in a driver that is left out of it (CONFIG_HAKIOS_<NAME> not set: drivers/<subsystem>/conf,
 * docs/drivers-config.md), the way XNU keeps bsd/kern/bsd_stubs.c for its own parts that a configuration leaves out.
 *
 * The callers are the places that start the drivers (hakios.input.c, hakios.audio.c, bsd_init.c), that stop them on
 * the way back to the loader or out of a panic (loader_return.c, the thermal and battery guards, machine_routines.c)
 * and the few that ask a driver something on the way (is this device one of the storage's, what the battery's
 * temperature is). Each of them calls the driver unconditionally, as it did when every driver was always built; here,
 * for a driver that is not, is what each call then does: nothing, and the answer "no", "not known" or "not there".
 * A driver that is built gets none of this: every block below is for one driver, and empty when that one is in.
 *
 * What is not here, on purpose: a call from one driver to another that it cannot work without. That is a `requires`
 * of its conf (SOUND requires ADSP and SPEAKER, MODEM requires QMI and UFS, ...), and tools/hakios-config.py refuses
 * the configuration that would need a stand-in for it.
 */
#include <stdbool.h>
#include <stdint.h>
#include <sys/errno.h>
#include <sys/types.h>
#include <sys/hakios.audio.h>
#include <sys/hakios.input.h>
#include <sys/hakios.net.h>
#include <sys/hakios.virtio_blk.h>

/* the ends of a run (loader_return.c, the guards), which have no header of their own */
void hakios_adsp_stop(void);
void hakios_adsp_quiesce(void);
void hakios_mpss_stop(void);
void hakios_mpss_quiesce(void);
void hakios_ddr_quiesce(void);
void hakios_ufs_quiesce(void);
void hakios_var_save_on_return(void);
void hakios_mic_quiesce(void);
void HakiosWLANStart(const char *interface, const uint8_t *address);

#ifndef HAKIOS_VIRTIO_INPUT
void hakios_virtio_input_init(void) {}
#endif

#ifndef HAKIOS_GOODIX
void hakios_goodix_init(void) {}
#endif

#ifndef HAKIOS_FTS
void hakios_fts_init(void) {}
#endif

#if !defined(HAKIOS_BUTTONS) && !defined(HAKIOS_BUTTONS_SM8450)
void hakios_buttons_init(void) {}
#endif

#ifndef HAKIOS_UFS
void hakios_ufs_init(void) {}
void hakios_ufs_report(__unused const char *when) {}
bool hakios_ufs_dev(__unused dev_t dev) { return false; }
void hakios_ufs_quiesce(void) {}
void hakios_var_save_on_return(void) {}         /* no flash, so no data volume to save into it */
#endif

#ifndef HAKIOS_VIRTIO_BLK
void hakios_virtio_blk_init(void) {}
bool hakios_virtio_blk_dev(__unused dev_t dev) { return false; }
#endif

#ifndef HAKIOS_BATTERY
void hakios_battery_init(void) {}
int hakios_battery_temperature(void) { return -273; }      /* "not known", as the driver says before its first reading */
void hakios_battery_report(__unused const struct hakios_battery_state *state) {}
#endif

#if !defined(HAKIOS_CPUFREQ) && !defined(HAKIOS_CPUFREQ_SM8450)
void hakios_cpufreq_init(void) {}
void hakios_ddr_quiesce(void) {}
uint32_t hakios_cpufreq_cap(__unused uint32_t domain, __unused uint32_t steps) { return 0; }    /* no brake: nothing raised */
uint32_t hakios_cpufreq_steps(__unused uint32_t domain) { return 0; }
#endif

#if !defined(HAKIOS_ADRENO) && !defined(HAKIOS_A730)
void hakios_adreno_init(void) {}
void hakios_adreno_quiesce(void) {}
void hakios_adreno_stop(void) {}
void hakios_adreno_minute(void) {}
#endif

#if !defined(HAKIOS_WIFI) && !defined(HAKIOS_WLAN_SM8450)
void hakios_wifi_init(void) {}
void hakios_wifi_stop(void) {}
void hakios_wifi_quiesce(void) {}
#endif

#ifndef HAKIOS_WIFI_LX
errno_t hakios_apple80211_ioctl(__unused u_long command, __unused void *data) { return EOPNOTSUPP; }
void HakiosWLANStart(__unused const char *interface, __unused const uint8_t *address) {}
#endif

#if !defined(HAKIOS_ADSP) && !defined(HAKIOS_ADSP_SM8450)
void hakios_adsp_init(void) {}
void hakios_adsp_stop(void) {}
void hakios_adsp_quiesce(void) {}
#endif

#ifndef HAKIOS_SLPI_SM8450
void hakios_slpi_init(void) {}
void hakios_slpi_stop(void) {}
void hakios_slpi_quiesce(void) {}
#endif

#ifndef HAKIOS_MODEM
void hakios_mpss_init(void) {}
void hakios_mpss_stop(void) {}
void hakios_mpss_quiesce(void) {}
#endif

#ifndef HAKIOS_VIRTIO_NET
void hakios_virtio_net_init(void) {}
#endif

#ifndef HAKIOS_VIRTIO_SOUND
void hakios_virtio_sound_init(void) {}
#endif

#if !defined(HAKIOS_SPEAKER) && !defined(HAKIOS_CS35L41)
void hakios_speaker_init(void) {}
#endif

#if !defined(HAKIOS_SOUND) && !defined(HAKIOS_SOUND_SM8450)
void hakios_sound_dsp_up(void) {}
void hakios_sound_dsp_down(void) {}
void hakios_sound_quiesce(void) {}
#endif

#ifndef HAKIOS_MIC
void hakios_mic_dsp_up(void) {}
void hakios_mic_dsp_down(void) {}
void hakios_mic_quiesce(void) {}
#endif

#if !defined(HAKIOS_HAPTIC) && !defined(HAKIOS_HAPTIC_SM8450)
void hakios_haptic_init(void) {}
void hakios_haptic_quiesce(void) {}
void hakios_haptic_pretend(__unused uint32_t peak) {}
#endif

#if !defined(HAKIOS_TORCH) && !defined(HAKIOS_TORCH_SM8450)
void hakios_torch_init(void) {}
void hakios_torch_quiesce(void) {}
#endif

#if !defined(HAKIOS_CAMERA) && !defined(HAKIOS_CAMERA_SM8450)
void hakios_camera_init(void) {}
void hakios_camera_quiesce(void) {}
#endif

#ifndef HAKIOS_CAMERA_SM8450
void hakios_video_stop(void);
void hakios_video_stop(void) {}         /* (the video codec is built with zeus's camera) */
#endif
