/* Polled SPI NOR driver (chip select on PB9) and settings persistence.
 * Commands used: 0x06 WREN, 0x03 read, 0x02 page program, 0x20 4K erase,
 * 0xd8 64K erase. Settings occupy one 4K sector well above the installed
 * firmware region, inside this firmware's declared user-data range. */
#include <stddef.h>

#include "storage.h"
#include "board.h"

int memcmp(const void *a, const void *b, size_t n); /* startup.c */

#define SETTINGS_MAGIC 0x31474741u /* "AGG1" */
#define PROJECT_MAGIC  0x31504741u /* "AGP1" */

static void cs(bool selected)
{
    GPIO_BSRR(GPIOB) = selected ? 1u << (16 + PIN_FLASH_CS) : 1u << PIN_FLASH_CS;
}

static uint8_t xfer(uint8_t value)
{
    REG8(SPI_STORE_DR) = value;
    while (!(REG32(SPI_STORE_SR) & SPI_SR_RXNE)) { }
    return REG8(SPI_STORE_DR);
}

static void command_address(uint8_t command, uint32_t address)
{
    xfer(command);
    xfer((uint8_t)(address >> 16));
    xfer((uint8_t)(address >> 8));
    xfer((uint8_t)address);
}

static void write_enable(void)
{
    cs(true);
    xfer(0x06);
    cs(false);
}

void spi_flash_read(uint32_t address, uint8_t *data, uint32_t length)
{
    cs(true);
    command_address(0x03, address);
    while (length--) *data++ = xfer(0);
    cs(false);
}

void spi_flash_erase_4k(uint32_t address)
{
    write_enable();
    cs(true);
    command_address(0x20, address);
    cs(false);
}

void spi_flash_erase_64k(uint32_t address)
{
    write_enable();
    cs(true);
    command_address(0xd8, address);
    cs(false);
}

void spi_flash_program(uint32_t address, const uint8_t *data, uint32_t length)
{
    while (length) {
        uint32_t page_room = 256 - (address & 255);
        uint32_t chunk = length < page_room ? length : page_room;
        write_enable();
        cs(true);
        command_address(0x02, address);
        for (uint32_t i = 0; i < chunk; i++) xfer(data[i]);
        cs(false);
        address += chunk;
        data += chunk;
        length -= chunk;
    }
}

static uint32_t fnv1a(const void *data, uint32_t length)
{
    const uint8_t *bytes = data;
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < length; i++)
        hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}

/* Both record types end in their uint32_t hash over everything before it. */
#define RECORD_HASH(r) fnv1a(r, sizeof(*(r)) - sizeof(uint32_t))

static const track_cfg_t TRACK_DEFAULTS[4] = {
    {16, 4, 0, 60, 50}, /* A: four on the floor, C4  */
    {16, 3, 2, 48, 50}, /* B */
    {12, 5, 0, 36, 50}, /* C */
    {16, 7, 0, 38, 50}, /* D: triggers               */
};
static const lfo_cfg_t LFO_DEFAULTS[4] = {
    /* wave       div         depth phase cc  dest */
    {WAVE_SINE, 7 /* 1/4  */, 100, 0, 1,  DEST_MOD}, /* A: mod wheel  */
    {WAVE_TRI,  5 /* 1/2  */, 100, 4, 2,  DEST_MOD}, /* B: breath, 90 degrees later */
    {WAVE_RISE, 3 /* 1BAR */, 100, 0, 11, DEST_MOD}, /* C: expression */
    {WAVE_SNH,  10 /* 1/8 */, 100, 0, 74, 0},        /* D: MIDI only  */
};
/* Mixed by default: rhythm on A/B, modulation on C/D. */
static const uint8_t MODE_DEFAULTS[4] = {MODE_SEQ, MODE_SEQ, MODE_LFO, MODE_LFO};

static bool track_cfg_valid(const track_cfg_t *c)
{
    return c->length >= 1 && c->length <= 16 && c->pulses <= c->length
        && c->rotate < c->length && c->note <= 127
        && c->gate_pct >= 5 && c->gate_pct <= 95;
}

static bool lfo_cfg_valid(const lfo_cfg_t *l)
{
    return l->wave < WAVE_COUNT && l->div < DIV_COUNT && l->depth <= 100
        && l->phase <= 15 && l->cc <= 127 && l->dest <= 7;
}

static bool lanes_valid(const uint8_t mode[4], const track_cfg_t track[4],
                        const lfo_cfg_t lfo[4])
{
    for (int t = 0; t < 4; t++)
        if (mode[t] > MODE_LFO || !track_cfg_valid(&track[t]) || !lfo_cfg_valid(&lfo[t]))
            return false;
    return true;
}

void settings_defaults(settings_t *s)
{
    s->magic = SETTINGS_MAGIC;
    s->bpm = 120;
    s->swing = 50;
    s->selected = 0;
    for (int t = 0; t < 4; t++) {
        s->mode[t] = MODE_DEFAULTS[t];
        s->track[t] = TRACK_DEFAULTS[t];
        s->lfo[t] = LFO_DEFAULTS[t];
    }
    s->hash = RECORD_HASH(s);
}

static bool settings_valid(const settings_t *s)
{
    if (s->magic != SETTINGS_MAGIC || s->hash != RECORD_HASH(s)) return false;
    if (s->bpm < 20 || s->bpm > 300 || s->swing < 50 || s->swing > 75 || s->selected > 3)
        return false;
    return lanes_valid(s->mode, s->track, s->lfo);
}

bool settings_load(settings_t *s)
{
    spi_flash_read(SETTINGS_ADDR, (uint8_t *)s, sizeof(*s));
    if (settings_valid(s)) return true;
    settings_defaults(s);
    return false;
}

void settings_save(const settings_t *s)
{
    settings_t copy = *s;
    copy.magic = SETTINGS_MAGIC;
    copy.hash = RECORD_HASH(&copy);
    /* Skip the erase/program cycle when the stored bytes already match. */
    settings_t stored;
    spi_flash_read(SETTINGS_ADDR, (uint8_t *)&stored, sizeof(stored));
    if (memcmp(&stored, &copy, sizeof(copy)) == 0) return;
    spi_flash_erase_4k(SETTINGS_ADDR);
    spi_flash_program(SETTINGS_ADDR, (const uint8_t *)&copy, sizeof(copy));
}

/* --- Saved-project catalog (PROJECT-001..004) --------------------------- */

int project_slots_available(void)
{
    /* Sized once from the SPI part's JEDEC density byte: the documented
     * catalog needs a 16 MiB part, and the native emulator model is only
     * 1 MiB, so out-of-range slots must be refused rather than addressed
     * (the model faults out-of-range transactions). A slot counts only when
     * its whole 0x3d000-byte region fits. */
    static int available = -1;
    if (available >= 0) return available;
    cs(true);
    xfer(0x9f);
    xfer(0); /* manufacturer */
    xfer(0); /* type */
    uint8_t capacity = xfer(0);
    cs(false);
    uint32_t bytes = (capacity >= 20 && capacity <= 24) ? 1u << capacity : 1u << 20;
    available = bytes <= PROJECT_BASE ? 0 : (int)((bytes - PROJECT_BASE) / PROJECT_SLOT_BYTES);
    if (available > PROJECT_SLOTS) available = PROJECT_SLOTS;
    return available;
}

void project_defaults(project_t *p)
{
    p->magic = PROJECT_MAGIC;
    p->bpm = 120;
    p->swing = 50;
    p->reserved = 0;
    for (int t = 0; t < 4; t++) {
        p->mode[t] = MODE_DEFAULTS[t];
        p->track[t] = TRACK_DEFAULTS[t];
        p->lfo[t] = LFO_DEFAULTS[t];
    }
    p->hash = RECORD_HASH(p);
}

static bool project_valid(const project_t *p)
{
    if (p->magic != PROJECT_MAGIC || p->hash != RECORD_HASH(p)) return false;
    if (p->bpm < 20 || p->bpm > 300 || p->swing < 50 || p->swing > 75) return false;
    return lanes_valid(p->mode, p->track, p->lfo);
}

slot_state_t project_load(int slot, project_t *out)
{
    project_t record;
    if (slot < 0 || slot >= project_slots_available()) return SLOT_UNAVAILABLE;
    spi_flash_read(PROJECT_ADDR(slot), (uint8_t *)&record, sizeof(record));
    if (project_valid(&record)) {
        *out = record;
        return SLOT_VALID;
    }
    const uint8_t *bytes = (const uint8_t *)&record;
    for (uint32_t i = 0; i < sizeof(record); i++)
        if (bytes[i] != 0xff) return SLOT_INVALID;
    return SLOT_EMPTY;
}

slot_state_t project_state(int slot)
{
    project_t record;
    return project_load(slot, &record);
}

bool project_save(int slot, const project_t *p)
{
    if (slot < 0 || slot >= project_slots_available()) return false;
    project_t copy = *p;
    copy.magic = PROJECT_MAGIC;
    copy.reserved = 0;
    copy.hash = RECORD_HASH(&copy);
    project_t stored;
    spi_flash_read(PROJECT_ADDR(slot), (uint8_t *)&stored, sizeof(stored));
    if (memcmp(&stored, &copy, sizeof(copy)) == 0) return true;
    spi_flash_erase_4k(PROJECT_ADDR(slot));
    spi_flash_program(PROJECT_ADDR(slot), (const uint8_t *)&copy, sizeof(copy));
    /* Completion is reported only once the committed bytes read back. */
    spi_flash_read(PROJECT_ADDR(slot), (uint8_t *)&stored, sizeof(stored));
    return memcmp(&stored, &copy, sizeof(copy)) == 0;
}

bool project_delete(int slot)
{
    if (slot < 0 || slot >= project_slots_available()) return false;
    if (project_state(slot) == SLOT_EMPTY) return true; /* already empty */
    spi_flash_erase_4k(PROJECT_ADDR(slot));
    return project_state(slot) == SLOT_EMPTY;
}
