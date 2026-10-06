/*
 * PMPlayer Advance privileged driver bridge.
 *
 * This keeps the historic cooleyesBridge ABI for binary compatibility while
 * hardening argument validation and exposing an explicit capability handshake.
 * It is intentionally not named "kubridge": ARK/CFW already provides the
 * unrelated, general-purpose KUBridge library under that name.
 */
#ifndef COOLEYESBRIDGE_H
#define COOLEYESBRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

#define COOLEYES_BRIDGE_API_VERSION 0x00030001U
#define COOLEYES_BRIDGE_CAP_AUDIO_FREQUENCY 0x00000001U
#define COOLEYES_BRIDGE_CAP_ME_BOOT         0x00000002U
#define COOLEYES_BRIDGE_CAP_DISPLAY_POWER   0x00000004U
#define COOLEYES_BRIDGE_CAP_REGION_CODE     0x00000008U

/* Read-only PSCode query. Returns a validated code or -1 on unavailable,
 * failed, malformed or unknown output; no IdStorage/registry writes. */
int cooleyesGetRegionCode(void);

/* Restore the saved brightness on enable. Kernel display driver calls stay
 * inside this narrow ABI; no ME/GE context or framebuffer is changed. */
int cooleyesDisplaySetEnabled(int enabled);

unsigned int cooleyesBridgeGetVersion(void);
unsigned int cooleyesBridgeGetCapabilities(void);

/**
 * Set the hardware audio output frequency.
 *
 * @param devkitVersion Firmware devkit version, or <= 0 to query it in-kernel.
 * @param frequency Exactly 44100 or 48000 Hz.
 * @return 0 on success, a negative kernel/validation error on failure.
 */
int cooleyesAudioSetFrequency(int devkitVersion, int frequency);

/**
 * Start the firmware Media Engine booter selected by the caller.
 *
 * This is a narrow compatibility bridge to sceMeBootStart. It does not expose
 * arbitrary ME/VME execution, DMAC programming, or the H.264X diagnostic ABI.
 */
int cooleyesMeBootStart(int devkitVersion, int mebooterType);

#ifdef __cplusplus
}
#endif

#endif
