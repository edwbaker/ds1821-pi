/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ds1821_program.c — DS1821 direct GPIO bit-bang utility
 *
 * Talks to a DS1821 by bit-banging the 1-Wire protocol on a
 * Raspberry Pi GPIO pin using the pigpio library.
 *
 * The DS1821 in thermostat mode does not participate in the
 * standard 1-Wire ROM protocol — it ignores Read ROM, Search
 * ROM, and Match ROM entirely.  It must be the only device on
 * its bus.  Send a reset pulse, then immediately send the
 * function command (no ROM step).
 *
 * To switch to 1-Wire mode:
 *   1. Reset + Read Status to confirm communication
 *   2. Reset + Write Status with 1SHOT=1, POL cleared, etc.
 *   3. Power-cycle the DS1821
 *   4. The DS1821 will now appear as family 0x22
 *
 * Build:  gcc -Wall -o ds1821_program ds1821_program.c -lpigpio -lrt -lpthread
 * Run:    sudo ./ds1821_program [options]
 *
 * Must be run as root.
 */

#define _DEFAULT_SOURCE  /* for usleep() */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <pigpio.h>

/* ── Configuration ───────────────────────────────────────────────── */
#define DEFAULT_GPIO_PIN   17     /* Default 1-Wire data GPIO pin   */
#define DEFAULT_POWER_PIN  -1     /* GPIO pin powering DS1821 VDD   */

/* ── DS1821 Commands (same as ds1821.h) ──────────────────────────── */
#define DS1821_CMD_START_CONVERT  0xEE
#define DS1821_CMD_STOP_CONVERT   0x22
#define DS1821_CMD_READ_TEMP      0xAA
#define DS1821_CMD_READ_COUNTER   0xA0
#define DS1821_CMD_READ_SLOPE     0xA9
#define DS1821_CMD_READ_TH        0xA1
#define DS1821_CMD_READ_TL        0xA2
#define DS1821_CMD_WRITE_TH       0x01
#define DS1821_CMD_WRITE_TL       0x02
#define DS1821_CMD_READ_STATUS    0xAC
#define DS1821_CMD_WRITE_STATUS   0x0C

/* Status bits */
#define DS1821_STATUS_DONE   0x80
#define DS1821_STATUS_THF    0x40
#define DS1821_STATUS_TLF    0x20
#define DS1821_STATUS_NVB    0x10
#define DS1821_STATUS_1SHOT  0x01

/* 1-Wire ROM commands */
#define OW_CMD_READ_ROM    0x33
#define OW_CMD_SKIP_ROM    0xCC
#define OW_CMD_SEARCH_ROM  0xF0

/* ── 1-Wire Timing (microseconds) ────────────────────────────────── */
/*
 * Standard speed, per Maxim AN126 / DS1821 datasheet:
 */
#define OW_RESET_LOW_US       480   /* Reset pulse, master pulls low */
#define OW_RESET_RELEASE_US   70    /* Wait for presence pulse start */
#define OW_RESET_PRESENCE_US  410   /* Remaining presence detect window */

#define OW_WRITE1_LOW_US      6     /* Write-1: short low pulse */
#define OW_WRITE1_RELEASE_US  64    /* Write-1: release for rest of slot */
#define OW_WRITE0_LOW_US      60    /* Write-0: long low pulse */
#define OW_WRITE0_RELEASE_US  10    /* Write-0: release, recovery */

#define OW_READ_LOW_US        6     /* Read: initiate with short low */
#define OW_READ_SAMPLE_US     9     /* Read: sample after this delay */
#define OW_READ_SLOT_US       55    /* Read: total slot time */

#define OW_RECOVERY_US        2     /* Inter-slot recovery */

static int gpio_pin = DEFAULT_GPIO_PIN;
static int power_pin = DEFAULT_POWER_PIN;  /* -1 = not used */
static int read_tout_flag = 0;             /* --read-tout: check DQ/TOUT state */
static int verbose = 0;
static int quiet = 0;   /* --quick: minimal output for scripting */

/* ── Low-level 1-Wire bit-bang ───────────────────────────────────── */

/*
 * Pin control: the 1-Wire bus is open-drain.  We simulate this with
 * GPIO direction switching:
 *   - "Release" (high): set pin to INPUT (external pullup pulls high)
 *   - "Drive low":      set pin to OUTPUT, write 0
 */

static inline void ow_release(void)
{
    gpioSetMode(gpio_pin, PI_INPUT);
    gpioSetPullUpDown(gpio_pin, PI_PUD_UP);
}

static inline void ow_drive_low(void)
{
    gpioSetMode(gpio_pin, PI_OUTPUT);
    gpioWrite(gpio_pin, 0);
}

static inline int ow_read_bit_raw(void)
{
    return gpioRead(gpio_pin);
}

/*
 * Reset pulse — returns 1 if a presence pulse was detected, 0 if not.
 */
static int ow_reset(void)
{
    int presence;

    /* Pull low for reset duration */
    ow_drive_low();
    gpioDelay(OW_RESET_LOW_US);

    /* Release and wait for device to respond */
    ow_release();
    gpioDelay(OW_RESET_RELEASE_US);

    /* Sample: device pulls low during presence pulse */
    presence = !ow_read_bit_raw();  /* low = present */

    /* Wait out the rest of the reset window */
    gpioDelay(OW_RESET_PRESENCE_US);

    if (verbose)
        printf("  [OW] Reset: presence %s\n", presence ? "DETECTED" : "not detected");

    return presence;
}

/*
 * Write a single bit.
 */
static void ow_write_bit(int bit)
{
    if (bit) {
        /* Write 1: short low, long release */
        ow_drive_low();
        gpioDelay(OW_WRITE1_LOW_US);
        ow_release();
        gpioDelay(OW_WRITE1_RELEASE_US);
    } else {
        /* Write 0: long low, short release */
        ow_drive_low();
        gpioDelay(OW_WRITE0_LOW_US);
        ow_release();
        gpioDelay(OW_WRITE0_RELEASE_US);
    }
    gpioDelay(OW_RECOVERY_US);
}

/*
 * Read a single bit.
 */
static int ow_read_bit(void)
{
    int bit;

    /* Initiate read slot with short low pulse */
    ow_drive_low();
    gpioDelay(OW_READ_LOW_US);

    /* Release and sample */
    ow_release();
    gpioDelay(OW_READ_SAMPLE_US);
    bit = ow_read_bit_raw();

    /* Wait out rest of time slot */
    gpioDelay(OW_READ_SLOT_US);
    gpioDelay(OW_RECOVERY_US);

    return bit;
}

/*
 * Write a byte (LSB first, per 1-Wire standard).
 */
static void ow_write_byte(uint8_t byte)
{
    if (verbose)
        printf("  [OW] Write: 0x%02X\n", byte);

    for (int i = 0; i < 8; i++) {
        ow_write_bit(byte & 0x01);
        byte >>= 1;
    }
}

/*
 * Read a byte (LSB first).
 */
static uint8_t ow_read_byte(void)
{
    uint8_t byte = 0;

    for (int i = 0; i < 8; i++) {
        if (ow_read_bit())
            byte |= (1 << i);
    }

    if (verbose)
        printf("  [OW] Read:  0x%02X\n", byte);

    return byte;
}

/* ── DS1821 high-level operations (thermostat mode — no ROM) ─────── */

/*
 * In thermostat mode there is no ROM layer.  After reset + presence,
 * send the function command directly.
 */

static int ds1821_read_status_reg(uint8_t *status)
{
    if (!ow_reset()) {
        fprintf(stderr, "No presence pulse — check wiring!\n");
        return -1;
    }
    ow_write_byte(DS1821_CMD_READ_STATUS);
    *status = ow_read_byte();
    return 0;
}

static int ds1821_write_status_reg(uint8_t status)
{
    if (!ow_reset()) {
        fprintf(stderr, "No presence pulse — check wiring!\n");
        return -1;
    }
    ow_write_byte(DS1821_CMD_WRITE_STATUS);
    ow_write_byte(status);

    /*
     * After writing the status register, the device copies it to
     * EEPROM.  The NVB flag will be set during the write.
     * Per datasheet, EEPROM write takes up to 10ms, but we'll be
     * generous.  During this time DQ must remain high (pulled up).
     */
    printf("  Waiting for EEPROM write...\n");
    ow_release();
    usleep(200000);  /* 200 ms, very generous */

    return 0;
}

/*
 * Try writing status register using Skip ROM first (proper 1-Wire addressing).
 * Some DS1821s in transitional state may need this.
 */
static int ds1821_write_status_skiprom(uint8_t status)
{
    if (!ow_reset()) {
        fprintf(stderr, "No presence pulse!\n");
        return -1;
    }
    ow_write_byte(OW_CMD_SKIP_ROM);     /* Skip ROM — address all devices */
    ow_write_byte(DS1821_CMD_WRITE_STATUS);
    ow_write_byte(status);

    printf("  (Skip ROM) Waiting for EEPROM write...\n");
    ow_release();
    usleep(200000);

    return 0;
}

static int ds1821_read_status_skiprom(uint8_t *status)
{
    if (!ow_reset()) return -1;
    ow_write_byte(OW_CMD_SKIP_ROM);
    ow_write_byte(DS1821_CMD_READ_STATUS);
    *status = ow_read_byte();
    return 0;
}

static int ds1821_read_temperature(int8_t *temp)
{
    if (!ow_reset()) {
        fprintf(stderr, "No presence pulse!\n");
        return -1;
    }
    ow_write_byte(DS1821_CMD_READ_TEMP);
    *temp = (int8_t)ow_read_byte();
    return 0;
}

static int ds1821_read_counter(uint8_t *count_remain)
{
    if (!ow_reset()) return -1;
    ow_write_byte(DS1821_CMD_READ_COUNTER);
    *count_remain = ow_read_byte();
    return 0;
}

static int ds1821_read_slope(uint8_t *count_per_c)
{
    if (!ow_reset()) return -1;
    ow_write_byte(DS1821_CMD_READ_SLOPE);
    *count_per_c = ow_read_byte();
    return 0;
}

static int ds1821_start_convert(void)
{
    if (!ow_reset()) return -1;
    ow_write_byte(DS1821_CMD_START_CONVERT);
    return 0;
}

static int ds1821_read_th(int8_t *th)
{
    if (!ow_reset()) return -1;
    ow_write_byte(DS1821_CMD_READ_TH);
    *th = (int8_t)ow_read_byte();
    return 0;
}

static int ds1821_read_tl(int8_t *tl)
{
    if (!ow_reset()) return -1;
    ow_write_byte(DS1821_CMD_READ_TL);
    *tl = (int8_t)ow_read_byte();
    return 0;
}

static int ds1821_write_th(int8_t th)
{
    if (!ow_reset()) {
        fprintf(stderr, "No presence pulse!\n");
        return -1;
    }
    ow_write_byte(DS1821_CMD_WRITE_TH);
    ow_write_byte((uint8_t)th);
    printf("  Waiting for EEPROM write...\n");
    ow_release();
    usleep(200000);  /* 200ms for EEPROM */
    return 0;
}

static int ds1821_write_tl(int8_t tl)
{
    if (!ow_reset()) {
        fprintf(stderr, "No presence pulse!\n");
        return -1;
    }
    ow_write_byte(DS1821_CMD_WRITE_TL);
    ow_write_byte((uint8_t)tl);
    printf("  Waiting for EEPROM write...\n");
    ow_release();
    usleep(200000);
    return 0;
}

/* ── Utility: print status register ──────────────────────────────── */

static void print_status(uint8_t s)
{
    printf("  Status register: 0x%02X\n", s);
    printf("    DONE  (bit 7): %d  — %s\n", !!(s & 0x80),
           (s & 0x80) ? "conversion complete" : "conversion in progress");
    printf("    THF   (bit 6): %d  — %s\n", !!(s & 0x40),
           (s & 0x40) ? "HIGH alarm tripped" : "no high alarm");
    printf("    TLF   (bit 5): %d  — %s\n", !!(s & 0x20),
           (s & 0x20) ? "LOW alarm tripped" : "no low alarm");
    printf("    NVB   (bit 4): %d  — %s\n", !!(s & 0x10),
           (s & 0x10) ? "EEPROM write in progress" : "EEPROM idle");
    printf("    POL   (bit 1): %d  — thermostat output polarity %s\n",
           !!(s & 0x02), (s & 0x02) ? "active-high" : "active-low");
    printf("    1SHOT (bit 0): %d  — %s mode\n", !!(s & 0x01),
           (s & 0x01) ? "one-shot" : "continuous");
}

/*
 * Read ROM — only works with a SINGLE 1-Wire device on the bus.
 * Returns the 8-byte ROM code (family + 48-bit serial + CRC).
 */
static int ow_read_rom(uint8_t rom[8])
{
    if (!ow_reset()) {
        printf("  No presence pulse.\n");
        return -1;
    }
    ow_write_byte(OW_CMD_READ_ROM);
    for (int i = 0; i < 8; i++)
        rom[i] = ow_read_byte();
    return 0;
}

/*
 * CRC8 for 1-Wire ROM codes (polynomial x^8+x^5+x^4+1 = 0x131).
 */
static uint8_t ow_crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) {
        uint8_t byte = data[i];
        for (int j = 0; j < 8; j++) {
            uint8_t mix = (crc ^ byte) & 0x01;
            crc >>= 1;
            if (mix) crc ^= 0x8C;
            byte >>= 1;
        }
    }
    return crc;
}

static void print_rom(const uint8_t rom[8])
{
    printf("  ROM: ");
    for (int i = 0; i < 8; i++)
        printf("%02X", rom[i]);

    uint8_t crc = ow_crc8(rom, 7);
    printf("  (family=0x%02X, CRC %s)\n", rom[0],
           crc == rom[7] ? "OK" : "BAD");

    /* Identify known families */
    switch (rom[0]) {
    case 0x22: printf("         → Family 0x22 (DS1822 / DS1821 in 1-Wire mode)\n"); break;
    case 0x10: printf("         → DS18S20 (thermometer)\n"); break;
    case 0x28: printf("         → DS18B20 (thermometer)\n"); break;
    case 0x3B: printf("         → DS1825 (thermometer)\n"); break;
    case 0x42: printf("         → DS28EA00 (thermometer)\n"); break;
    case 0x00: printf("         → Family 0 — likely thermostat-mode DS1821\n"); break;
    default:   printf("         → Unknown family\n"); break;
    }
}

/*
 * 1-Wire Search ROM algorithm (per Maxim AN187).
 * Finds all devices on the bus and stores their 64-bit ROM codes.
 * Returns the number of devices found.
 */
static int ow_search_rom(uint8_t roms[][8], int max_devices)
{
    int device_count = 0;
    int last_discrepancy = -1;
    int done = 0;

    uint8_t rom[8];
    memset(rom, 0, sizeof(rom));

    while (!done && device_count < max_devices) {
        if (!ow_reset()) {
            if (device_count == 0)
                printf("  No presence pulse on search.\n");
            break;
        }

        ow_write_byte(OW_CMD_SEARCH_ROM);

        int new_discrepancy = -1;

        for (int bit_pos = 0; bit_pos < 64; bit_pos++) {
            int byte_idx = bit_pos / 8;
            int bit_mask = 1 << (bit_pos % 8);

            /* Read two bits: id_bit and cmp_bit */
            int id_bit  = ow_read_bit();
            int cmp_bit = ow_read_bit();

            if (id_bit && cmp_bit) {
                /* No devices responding — error or done */
                done = 1;
                break;
            }

            int dir;
            if (id_bit != cmp_bit) {
                /* All devices agree on this bit */
                dir = id_bit;
            } else {
                /* Discrepancy — both 0 and 1 present */
                if (bit_pos == last_discrepancy) {
                    dir = 1;  /* Take the 1 branch this time */
                } else if (bit_pos > last_discrepancy) {
                    dir = 0;  /* Take the 0 branch first */
                    new_discrepancy = bit_pos;
                } else {
                    /* Use same direction as last search */
                    dir = (rom[byte_idx] & bit_mask) ? 1 : 0;
                    if (dir == 0)
                        new_discrepancy = bit_pos;
                }
            }

            /* Set the bit in our ROM buffer */
            if (dir)
                rom[byte_idx] |= bit_mask;
            else
                rom[byte_idx] &= ~bit_mask;

            /* Write direction bit to select that branch */
            ow_write_bit(dir);
        }

        if (!done) {
            memcpy(roms[device_count], rom, 8);
            device_count++;
        }

        last_discrepancy = new_discrepancy;
        if (last_discrepancy < 0)
            done = 1;  /* No more discrepancies — all devices found */
    }

    return device_count;
}


/* ── Actions ─────────────────────────────────────────────────────── */

static int action_scan(void)
{
    printf("\n=== Scanning 1-Wire bus on GPIO%d ===\n\n", gpio_pin);

    /* First: basic presence check */
    printf("  1. Presence check...\n");
    if (!ow_reset()) {
        printf("     No presence pulse — no devices responding at all.\n");
        printf("     Check wiring: DQ→GPIO%d, 4.7kΩ pullup to 3.3V, GND.\n",
               gpio_pin);
        return -1;
    }
    printf("     Presence pulse detected — at least one device on bus.\n\n");

    /* Try Read ROM (only valid with exactly 1 device) */
    printf("  2. Read ROM (single-device command)...\n");
    uint8_t rom[8];
    if (ow_read_rom(rom) == 0) {
        uint8_t crc = ow_crc8(rom, 7);
        if (crc == rom[7] && rom[0] != 0x00) {
            printf("     Single device found:\n     ");
            print_rom(rom);
        } else {
            printf("     Got garbled ROM (multi-device collision or thermostat mode):\n     ");
            print_rom(rom);
            printf("     This is expected with multiple devices or thermostat-mode DS1821s.\n");
        }
    }
    printf("\n");

    /* Search ROM to find all 1-Wire mode devices */
    printf("  3. Search ROM (multi-device enumeration)...\n");
    uint8_t found_roms[16][8];
    int count = ow_search_rom(found_roms, 16);

    if (count == 0) {
        printf("     No devices found via Search ROM.\n");
        printf("     If DS1821s are in thermostat mode, they won't respond to ROM commands.\n");
    } else {
        printf("     Found %d device(s):\n", count);
        int valid = 0, phantom = 0;
        for (int i = 0; i < count; i++) {
            printf("     [%d] ", i + 1);
            print_rom(found_roms[i]);
            uint8_t crc = ow_crc8(found_roms[i], 7);
            if (crc == found_roms[i][7] && found_roms[i][0] != 0x00)
                valid++;
            else
                phantom++;
        }
        if (phantom > 0) {
            printf("\n     %d phantom device(s) detected — likely DS1821(s) in thermostat mode\n", phantom);
            printf("     driving the bus and creating false ROM codes.\n");
        }
        if (valid > 0) {
            printf("\n     %d valid 1-Wire device(s) found.\n", valid);
        }
    }

    /* Probe thermostat-mode status (all respond simultaneously) */
    printf("\n  4. Direct status read (thermostat-mode, no ROM)...\n");
    printf("     Note: If multiple devices respond, bits are ANDed together.\n");
    uint8_t status;
    if (ds1821_read_status_reg(&status) == 0) {
        print_status(status);
        int8_t th, tl;
        if (ds1821_read_th(&th) == 0 && ds1821_read_tl(&tl) == 0)
            printf("\n  Alarm thresholds: TH=%d°C  TL=%d°C\n", th, tl);
    }

    printf("\n  Summary:\n");
    printf("  ─────────\n");
    printf("  Presence:      YES\n");
    printf("  ROM devices:   %d\n", count);
    printf("  You said:      3 devices connected\n");
    printf("\n  Next steps:\n");
    printf("    sudo ./ds1821_program fix     — attempt to reprogram all to 1-Wire mode\n");
    printf("    sudo ./ds1821_program temp    — read temperature (all respond at once)\n");

    return 0;
}

/*
 * Read the TOUT state on the DQ/data pin.
 * In thermostat mode, DQ doubles as the thermostat output.
 * Returns 0 or 1 for the pin level, or -1 if not enabled.
 */
static int read_tout(void)
{
    if (!read_tout_flag)
        return -1;
    gpioSetMode(gpio_pin, PI_INPUT);
    gpioSetPullUpDown(gpio_pin, PI_PUD_OFF);
    return gpioRead(gpio_pin);
}

static int action_probe(void)
{
    if (!quiet)
        printf("\n=== Probing DS1821 on GPIO%d ===\n\n", gpio_pin);

    uint8_t status;
    if (ds1821_read_status_reg(&status) < 0)
        return -1;

    int8_t th = 0, tl = 0;
    int have_thresholds = (ds1821_read_th(&th) == 0 && ds1821_read_tl(&tl) == 0);
    int tout = read_tout();

    if (quiet) {
        /* Machine-readable key=value output */
        printf("status=0x%02X\n", status);
        printf("done=%d\n",   (status & DS1821_STATUS_DONE) ? 1 : 0);
        printf("thf=%d\n",    (status & DS1821_STATUS_THF)  ? 1 : 0);
        printf("tlf=%d\n",    (status & DS1821_STATUS_TLF)  ? 1 : 0);
        printf("nvb=%d\n",    (status & DS1821_STATUS_NVB)  ? 1 : 0);
        printf("oneshot=%d\n",(status & DS1821_STATUS_1SHOT)? 1 : 0);
        if (have_thresholds) {
            printf("th=%d\n", th);
            printf("tl=%d\n", tl);
        }
        if (tout >= 0)
            printf("tout=%d\n", tout);
    } else {
        print_status(status);
        if (have_thresholds)
            printf("\n  Alarm thresholds: TH=%d°C  TL=%d°C\n", th, tl);
        if (tout >= 0)
            printf("  TOUT (DQ/GPIO%d): %s\n", gpio_pin,
                   tout ? "HIGH (active)" : "LOW (inactive)");
    }

    return 0;
}

static int action_read_temp(void)
{
    if (!quiet)
        printf("\n=== Reading Temperature from DS1821 ===\n\n");

    /* Start conversion */
    if (!quiet) printf("  Starting conversion...\n");
    if (ds1821_start_convert() < 0)
        return -1;

    /* Wait for conversion — DS1821 needs up to 1s */
    usleep(1000000);

    /* Check DONE */
    uint8_t status;
    if (ds1821_read_status_reg(&status) < 0)
        return -1;
    if (!(status & DS1821_STATUS_DONE) && !quiet) {
        printf("  Warning: DONE bit not set, conversion may not be complete.\n");
    }

    /* Read temperature */
    int8_t temp;
    if (ds1821_read_temperature(&temp) < 0)
        return -1;

    /* Read counter and slope for high-res */
    uint8_t count_remain, count_per_c;
    if (ds1821_read_counter(&count_remain) < 0)
        return -1;
    if (ds1821_read_slope(&count_per_c) < 0)
        return -1;

    /* Calculate */
    int cpc = count_per_c ? count_per_c : 1;
    float hires = (float)temp - 0.25f +
                  (float)(cpc - count_remain) / (float)cpc;
    int millideg = (int)temp * 1000 - 250 +
                   ((int)(cpc - count_remain) * 1000) / cpc;

    int tout = read_tout();

    if (quiet) {
        /* Machine-readable: just the temperature */
        printf("%.2f\n", hires);
    } else {
        printf("\n  ┌─────────────────────────────────────┐\n");
        printf("  │  Integer temp:   %4d °C             │\n", temp);
        printf("  │  COUNT_REMAIN:   %4d                │\n", count_remain);
        printf("  │  COUNT_PER_C:    %4d                │\n", count_per_c);
        printf("  │  Hi-res temp:    %7.2f °C          │\n", hires);
        printf("  │  Millidegrees:   %5d m°C           │\n", millideg);
        printf("  └─────────────────────────────────────┘\n");

        if (status & DS1821_STATUS_THF)
            printf("  *** HIGH alarm flag set!\n");
        if (status & DS1821_STATUS_TLF)
            printf("  *** LOW alarm flag set!\n");
        if (tout >= 0)
            printf("  TOUT (DQ/GPIO%d): %s\n", gpio_pin,
                   tout ? "HIGH (active)" : "LOW (inactive)");
    }

    return 0;
}

/*
 * status action — machine-readable dump of everything:
 * temperature + thresholds + alarm flags + TOUT.
 * Always outputs key=value format regardless of --quiet.
 */
static int action_status(void)
{
    /* Start conversion */
    if (ds1821_start_convert() < 0)
        return -1;
    usleep(1000000);

    uint8_t status;
    if (ds1821_read_status_reg(&status) < 0)
        return -1;

    int8_t temp;
    if (ds1821_read_temperature(&temp) < 0)
        return -1;

    uint8_t count_remain, count_per_c;
    if (ds1821_read_counter(&count_remain) < 0)
        return -1;
    if (ds1821_read_slope(&count_per_c) < 0)
        return -1;

    int cpc = count_per_c ? count_per_c : 1;
    int millideg = (int)temp * 1000 - 250 +
                   ((int)(cpc - count_remain) * 1000) / cpc;

    int8_t th = 0, tl = 0;
    int have_th = (ds1821_read_th(&th) == 0 && ds1821_read_tl(&tl) == 0);
    int tout = read_tout();

    printf("temperature=%d\n", millideg);
    printf("thf=%d\n", (status & DS1821_STATUS_THF) ? 1 : 0);
    printf("tlf=%d\n", (status & DS1821_STATUS_TLF) ? 1 : 0);
    if (have_th) {
        printf("th=%d\n", th);
        printf("tl=%d\n", tl);
    }
    if (tout >= 0)
        printf("tout=%d\n", tout);

    return 0;
}

static int action_set_thresholds(int set_th, int set_tl, int8_t new_th, int8_t new_tl)
{
    printf("\n=== DS1821 Thermostat Thresholds ===\n\n");

    /* Read current values */
    int8_t cur_th, cur_tl;
    if (ds1821_read_th(&cur_th) < 0 || ds1821_read_tl(&cur_tl) < 0)
        return -1;

    printf("  Current: TH=%d°C  TL=%d°C\n", cur_th, cur_tl);

    if (set_th) {
        printf("  Writing TH=%d°C...\n", new_th);
        if (ds1821_write_th(new_th) < 0)
            return -1;
    }
    if (set_tl) {
        printf("  Writing TL=%d°C...\n", new_tl);
        if (ds1821_write_tl(new_tl) < 0)
            return -1;
    }

    /* Verify */
    if (ds1821_read_th(&cur_th) < 0 || ds1821_read_tl(&cur_tl) < 0)
        return -1;

    printf("  Verified: TH=%d°C  TL=%d°C\n", cur_th, cur_tl);

    if (cur_tl >= cur_th)
        printf("  Warning: TL >= TH — thermostat will not operate correctly.\n");

    return 0;
}

static int action_set_oneshot(void)
{
    printf("\n=== Setting DS1821(s) to 1-Wire / One-Shot mode ===\n\n");

    /* Read current status */
    uint8_t status;
    if (ds1821_read_status_reg(&status) < 0)
        return -1;

    printf("  Current status (direct, ANDed if multiple devices):\n");
    print_status(status);

    /*
     * Write status: 1SHOT=1, POL=0, clear alarm flags.
     * We try MULTIPLE methods since these devices may be in different states:
     *   1. Direct write (thermostat mode — no ROM command)
     *   2. Skip ROM + write (proper 1-Wire addressing)
     *   3. Repeat both for good measure
     */
    uint8_t new_status = DS1821_STATUS_1SHOT;  /* 0x01 */

    printf("\n  === Attempt 1: Direct write (no ROM) ===\n");
    printf("  Writing status: 0x%02X\n", new_status);
    if (ds1821_write_status_reg(new_status) < 0)
        return -1;

    /* Verify */
    if (ds1821_read_status_reg(&status) == 0) {
        printf("  Read back: 0x%02X  1SHOT=%d POL=%d\n",
               status, !!(status & DS1821_STATUS_1SHOT), !!(status & 0x02));
    }

    printf("\n  === Attempt 2: Skip ROM + write ===\n");
    printf("  Writing status: 0x%02X\n", new_status);
    if (ds1821_write_status_skiprom(new_status) < 0)
        return -1;

    /* Verify via skip ROM too */
    if (ds1821_read_status_skiprom(&status) == 0) {
        printf("  Read back (skip ROM): 0x%02X  1SHOT=%d POL=%d\n",
               status, !!(status & DS1821_STATUS_1SHOT), !!(status & 0x02));
    }

    printf("\n  === Attempt 3: Direct write again ===\n");
    if (ds1821_write_status_reg(new_status) < 0)
        return -1;

    /* Final verification */
    printf("\n  Final readback:\n");
    if (ds1821_read_status_reg(&status) == 0) {
        print_status(status);
    }
    if (ds1821_read_status_skiprom(&status) == 0) {
        printf("  Via Skip ROM: 0x%02X\n", status);
    }

    printf("\n  Note: With 3 devices on the bus, status reads are ANDed.\n");
    printf("  If ANY device has 1SHOT=0, the combined read shows 0.\n");
    printf("  The write goes to ALL devices simultaneously, so all should\n");
    printf("  be programmed. A power cycle may be needed for the change\n");
    printf("  to take effect.\n");

    return 0;
}


/*
 * Power-cycle DS1821s via a GPIO pin driving their VDD.
 * Requires pigpio to be initialised.
 */
static int power_cycle_ds1821(void)
{
    if (power_pin < 0) {
        if (!quiet)
            printf("\nNo --power-gpio set — cannot power-cycle.\n"
                   "  Please disconnect and reconnect DS1821 VDD manually.\n");
        return -1;
    }

    if (!quiet)
        printf("\nPower-cycling DS1821s via GPIO%d...\n", power_pin);

    /* Drive power pin LOW to cut VDD */
    gpioSetMode(power_pin, PI_OUTPUT);
    gpioWrite(power_pin, 0);

    if (!quiet)
        printf("  VDD OFF — waiting 500ms for capacitors to drain...\n");
    usleep(500000);  /* 500ms off */

    /* Drive power pin HIGH to restore VDD */
    gpioWrite(power_pin, 1);

    if (!quiet)
        printf("  VDD ON — waiting 500ms for DS1821s to boot...\n");
    usleep(500000);  /* 500ms for POR */

    if (!quiet)
        printf("  Power cycle complete.\n");

    return 0;
}

/*
 * Ensure the power pin stays HIGH after pigpio terminates.
 * pigpio resets all pins on gpioTerminate(), so we use pinctrl
 * to re-assert the output state.
 */
static void persist_power_pin(void)
{
    if (power_pin < 0)
        return;

    char cmd[128];
    snprintf(cmd, sizeof(cmd),
             "pinctrl set %d op dh 2>/dev/null", power_pin);
    if (system(cmd) != 0) {
        /* Fallback: try raspi-gpio */
        snprintf(cmd, sizeof(cmd),
                 "raspi-gpio set %d op dh 2>/dev/null", power_pin);
        system(cmd);
    }
}

/* ── Usage ───────────────────────────────────────────────────────── */

static void usage(const char *prog)
{
    printf("Usage: %s [OPTIONS] ACTION\n\n"
           "Direct GPIO bit-bang utility for DS1821 (thermostat mode).\n"
           "Must be run as root.\n\n"
           "Actions:\n"
           "  scan         Enumerate all devices on the bus\n"
           "  probe        Read status register and thresholds\n"
           "  temp         Start conversion and read temperature\n"
           "  status       Read everything (key=value for scripting)\n"
           "  set-th N     Set high-alarm threshold to N °C (-55 to 125)\n"
           "  set-tl N     Set low-alarm threshold to N °C (-55 to 125)\n"
           "  set-oneshot  Write status register to enable 1-Wire mode\n"
           "  fix          Full sequence: set-oneshot + power-cycle\n\n"
           "Options:\n"
           "  --gpio N        Use GPIO pin N for 1-Wire data (default: %d)\n"
           "  --power-gpio N  GPIO pin powering DS1821 VDD (enables auto power-cycle)\n"
           "  --read-tout     Read thermostat output state from DQ pin\n"
           "  --quick, -q     Minimal output (just temperature value)\n"
           "  --verbose       Show low-level 1-Wire traffic\n"
           "  --help          Show this help\n\n"
           "Typical workflow:\n"
           "  sudo %s probe          # Verify communication\n"
           "  sudo %s temp           # Read temperature\n"
           "  sudo %s fix            # Switch to 1-Wire mode & reload\n",
           prog, DEFAULT_GPIO_PIN, prog, prog, prog);
}

/* ── main ────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    const char *action = NULL;
    int8_t arg_th = 0, arg_tl = 0;
    int has_th = 0, has_tl = 0;

    /* Parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--gpio") == 0 && i + 1 < argc) {
            gpio_pin = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--power-gpio") == 0 && i + 1 < argc) {
            power_pin = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--read-tout") == 0) {
            read_tout_flag = 1;
        } else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "--quick") == 0 || strcmp(argv[i], "-q") == 0) {
            quiet = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "set-th") == 0 && i + 1 < argc) {
            action = "set-th";
            arg_th = (int8_t)atoi(argv[++i]);
            has_th = 1;
        } else if (strcmp(argv[i], "set-tl") == 0 && i + 1 < argc) {
            action = "set-tl";
            arg_tl = (int8_t)atoi(argv[++i]);
            has_tl = 1;
        } else if (argv[i][0] != '-') {
            action = argv[i];
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (!action) {
        usage(argv[0]);
        return 1;
    }

    if (geteuid() != 0) {
        fprintf(stderr, "This tool must be run as root (sudo).\n");
        return 1;
    }

    int do_fix = (strcmp(action, "fix") == 0);
    int do_temp = (strcmp(action, "temp") == 0);
    int do_status = (strcmp(action, "status") == 0);

    /* status is inherently machine-readable — suppress banner */
    if (do_status)
        quiet = 1;

    if (!quiet) {
        printf("DS1821 Direct Programmer — GPIO%d\n", gpio_pin);
        printf("──────────────────────────────────\n");
    }

    /* Initialize pigpio */
    if (gpioInitialise() < 0) {
        fprintf(stderr, "Failed to initialize pigpio!\n");
        fprintf(stderr, "Make sure pigpiod is NOT running: sudo systemctl stop pigpiod\n");
        return 1;
    }

    /* If power pin is set, make sure it's driving HIGH (VDD on) */
    if (power_pin >= 0) {
        gpioSetMode(power_pin, PI_OUTPUT);
        gpioWrite(power_pin, 1);
    }

    /* Set pin to input with pullup (idle state for 1-Wire) */
    ow_release();

    /* Wait for DS1821s to power up and bus to settle */
    if (power_pin >= 0)
        usleep(500000);  /* 500ms for DS1821 power-on reset */
    else
        gpioDelay(1000);

    int ret = 0;

    if (strcmp(action, "scan") == 0) {
        ret = action_scan();
    } else if (strcmp(action, "probe") == 0) {
        ret = action_probe();
    } else if (do_status) {
        ret = action_status();
    } else if (do_temp) {
        ret = action_read_temp();
    } else if (has_th || has_tl) {
        ret = action_set_thresholds(has_th, has_tl, arg_th, arg_tl);
    } else if (strcmp(action, "set-oneshot") == 0 || do_fix) {
        ret = action_probe();
        if (ret == 0) {
            ret = action_set_oneshot();
        }
        if (do_fix && ret == 0) {
            ret = power_cycle_ds1821();
        }
    } else {
        fprintf(stderr, "Unknown action: %s\n", action);
        ret = 1;
    }

    /* Clean up pigpio */
    gpioTerminate();

    /* Keep power pin HIGH after pigpio releases GPIO */
    persist_power_pin();

    return ret < 0 ? 1 : 0;
}
