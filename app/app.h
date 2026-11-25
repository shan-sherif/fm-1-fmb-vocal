/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal
 * Copyright (C) 2026 Shan-Sherif */
/* The template: the smallest complete fm1-base firmware. Copy this directory to start a firmware.
 * This header is the app's half of the contract with the platform (device/unity.c, host/plat_host.c). */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "FMB VOCAL"
#define APP_SPLASH_RGB RGB(242, 120, 72)
#define APP_SPLASH                       /* the boot screen: the head, side on, and the version (app.c app_splash) */
void app_splash(void);
#define APP_BLOCK 256u                   /* frames per audio block: 5.8 ms at 44.1 kHz */
#define APP_TOUCH                        /* velocity and pressure from the keys (lib/touch.h) */
#define APP_BTDEV                        /* Bluetooth MIDI devices (lib/btdev.h) */
#define APP_BLE_AT_BOOT (!template_ble_off())   /* Bluetooth at power-on: as the player left it (REC held) */
int template_ble_off(void);

/* the store: the app's objects, the panel calibration (the platform's) and Bluetooth's address and pairing among
 * them, and their flash offsets */
/* OBJ_WORDS: the word slots (the Studio's); OBJ_PRESETS: the voice presets and the vowel snapshots; OBJ_BEATS: the
 * beatbox's beats; OBJ_CREATURES: the Bestiary's settings (0.7.0); OBJ_SONG: the SING page's tune, tempo, key and
 * vibrato (0.8.0); OBJ_MELODY: the MELODY page's own melodies (SEQ twice, FMB Vocal) */
enum { OBJ_SETTINGS, OBJ_PANEL, OBJ_BLE, OBJ_WORDS, OBJ_PRESETS, OBJ_BEATS, OBJ_CREATURES, OBJ_SONG, OBJ_MELODY, OBJ_NOBJ };
#define APP_STORE_MAP {0xFC000u, 0xDC000u, 0xDE000u, 0xD8000u, 0xDA000u, 0xD6000u, 0xD4000u, 0xD2000u, 0xD0000u}

/* the contract: the platform calls these */
void app_init(void);                     /* boot: the engine, then the saved settings (or defaults) */
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);   /* audio interrupt: n stereo frames, 24-bit */
void app_panic(void);                    /* silence now (before an update rewrites the flash) */
void app_ui_init(void);                  /* the first screen, after the splash */
void app_frame(void);                    /* the UI: input and a redraw, about 60 times a second */
void app_input_only(void);               /* between frames: input only */
void app_say(const char *a, const char *b);   /* a short message on the screen and to a screen reader */

/* the Studio (web/studio.html): its SysEx, F0 7D 56 4F cmd ... F7 (app.c "words") */
#define APP_SYSEX
int app_sysex(const uint8_t *p, uint32_t n);

/* the host simulator's checks (host/plat_host.c "expect"): octave, wave, note (-1 none), dirty, turnsN */
#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
/* the host simulator's own commands: "loads C2 C1" stands in for the cores' measured loads (%: the simulator measures
 * none), for the brain's colours (app/tests/brain.txt) */
#define APP_HOST_CMD
int app_host_cmd(const char *cmd, const char *a, const char *b, const char *c, const char *d);
