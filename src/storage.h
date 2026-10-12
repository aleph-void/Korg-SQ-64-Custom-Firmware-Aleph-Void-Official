#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>
#include <stdint.h>

/* External SPI NOR primitives. Addresses below 0x30000 hold the installed
 * firmware image and are written only by the updater during an installation;
 * all application data lives at or above SETTINGS_ADDR. */
void spi_flash_read(uint32_t address, uint8_t *data, uint32_t length);
void spi_flash_erase_4k(uint32_t address);
void spi_flash_erase_64k(uint32_t address);
void spi_flash_program(uint32_t address, const uint8_t *data, uint32_t length);

#define SETTINGS_ADDR 0x40000u

/* Saved project slots (board contract): slot n is the 0x3d000-byte region at
 * 0xc0000 + n*0x3d000, the same catalog stock firmware uses for its saved
 * projects. This firmware stores one project record (the workspace: engine
 * modes, both engines' track configurations, tempo, swing) in the first 4 KiB
 * sector of a slot and never touches the rest of the region. Slots past the
 * end of the fitted SPI part (JEDEC capacity) are unavailable rather than
 * silently aliased. */
#define PROJECT_SLOTS 64
#define PROJECT_BASE 0xc0000u
#define PROJECT_SLOT_BYTES 0x3d000u
#define PROJECT_ADDR(slot) (PROJECT_BASE + (uint32_t)(slot) * PROJECT_SLOT_BYTES)

/* Per-track engine: a Euclidean sequencer lane or a synced LFO. */
#define MODE_SEQ 0
#define MODE_LFO 1

/* LFO waveforms */
#define WAVE_SINE 0
#define WAVE_TRI  1
#define WAVE_RISE 2
#define WAVE_FALL 3
#define WAVE_SQR  4
#define WAVE_SNH  5
#define WAVE_COUNT 6

/* LFO output destinations (A/B/C: their jacks; D's gate bit pulses trigger 1). */
#define DEST_MOD   1u
#define DEST_PITCH 2u
#define DEST_GATE  4u

#define DIV_COUNT 16

typedef struct {
    uint8_t length;   /* 1..16 steps */
    uint8_t pulses;   /* 0..length  */
    uint8_t rotate;   /* 0..length-1 */
    uint8_t note;     /* MIDI note  */
    uint8_t gate_pct; /* 5..95 % of a step */
} track_cfg_t;

typedef struct {
    uint8_t wave;  /* 0..WAVE_COUNT-1 */
    uint8_t div;   /* 0..DIV_COUNT-1 synced rate division */
    uint8_t depth; /* 0..100 % */
    uint8_t phase; /* 0..15 sixteenths of a cycle */
    uint8_t cc;    /* 0..127 MIDI controller number */
    uint8_t dest;  /* DEST_* bit mask */
} lfo_cfg_t;

typedef struct {
    uint32_t magic;
    uint16_t bpm;      /* 20..300, internal clock */
    uint8_t swing;     /* 50..75 %, sequencer lanes */
    uint8_t selected;  /* edited track 0..3 */
    uint8_t mode[4];   /* MODE_SEQ or MODE_LFO per track */
    track_cfg_t track[4]; /* sequencer parameters, kept across mode toggles */
    lfo_cfg_t lfo[4];     /* LFO parameters, kept across mode toggles */
    uint32_t hash;
} settings_t;

void settings_defaults(settings_t *s);
bool settings_load(settings_t *s);
void settings_save(const settings_t *s);

/* A project is the workspace's musical content for both engine modes: the
 * per-track engine selection plus every track's sequencer and LFO
 * configurations, with the tempo and swing. The edited-track selection stays
 * in the settings sector. */
typedef struct {
    uint32_t magic;
    uint16_t bpm;      /* 20..300 */
    uint8_t swing;     /* 50..75 % */
    uint8_t reserved;  /* zero */
    uint8_t mode[4];   /* MODE_SEQ or MODE_LFO per track */
    track_cfg_t track[4];
    lfo_cfg_t lfo[4];
    uint32_t hash;
} project_t;

typedef enum {
    SLOT_EMPTY,       /* erased: no record            */
    SLOT_VALID,       /* committed project record     */
    SLOT_INVALID,     /* unreadable: never loaded     */
    SLOT_UNAVAILABLE, /* beyond the fitted SPI part   */
} slot_state_t;

int project_slots_available(void); /* 0..PROJECT_SLOTS addressable slots */
void project_defaults(project_t *p);
slot_state_t project_state(int slot);
/* Copies the record into *out only when the slot is SLOT_VALID. */
slot_state_t project_load(int slot, project_t *out);
/* Both report success only after the stored bytes read back verified;
 * deleting an already-empty slot succeeds without a flash write. */
bool project_save(int slot, const project_t *p);
bool project_delete(int slot);

#endif
