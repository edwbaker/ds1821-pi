/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ds1821_read.c — Read temperature from a DS1821 1-Wire sensor
 *
 * On Linux the w1 subsystem exposes 1-Wire slaves under
 *   /sys/bus/w1/devices/<family>-<serial>/
 *
 * The DS1821 family code is 0x22, so devices appear as:
 *   /sys/bus/w1/devices/22-xxxxxxxxxxxx/
 *
 * Since there's no dedicated DS1821 kernel family driver, we use the
 * generic "rw" sysfs file to send raw function commands and read back
 * responses.  The w1 core handles ROM-level addressing for us.
 *
 * Usage:
 *   ds1821_read              — auto-detect first DS1821 on the bus
 *   ds1821_read 22-0123456789ab  — read a specific device
 *   ds1821_read --loop [N]   — continuous reading every N seconds (default 2)
 *
 * Prerequisites:
 *   - A w1 bus master driver loaded for the GPIO pin
 *   - The DS1821 must be in 1-Wire mode (not thermostat-only mode)
 */

#define _DEFAULT_SOURCE  /* for usleep() */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <math.h>

/* ── DS1821 Commands ─────────────────────────────────────────────── */
#define DS1821_CMD_START_CONVERT  0xEE
#define DS1821_CMD_READ_TEMP     0xAA
#define DS1821_CMD_READ_COUNTER  0xA0
#define DS1821_CMD_READ_SLOPE    0xA9

/* ── W1 sysfs paths ──────────────────────────────────────────────── */
#define W1_DEVICES_DIR  "/sys/bus/w1/devices"
#define DS1821_FAMILY   "22"

static volatile int keep_running = 1;

static void sigint_handler(int sig)
{
    (void)sig;
    keep_running = 0;
}

/* ── Raw 1-Wire I/O via sysfs ────────────────────────────────────── */

/*
 * Open the "rw" file for a w1 slave.  This file allows sending raw
 * function commands after the ROM-select is done automatically by
 * the w1 core.
 */
static int w1_open_rw(const char *device_id)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s/rw", W1_DEVICES_DIR, device_id);
    return open(path, O_RDWR);
}

/*
 * Send a command byte and optionally read back `rlen` response bytes.
 * Returns 0 on success, -1 on error.
 */
static int w1_command(int fd, uint8_t cmd, uint8_t *rbuf, int rlen)
{
    /* Each open/write/read cycle on w1 "rw" does:
     *   1. Bus reset
     *   2. MATCH ROM (selects this slave)
     *   3. Writes our bytes
     *   4. Reads back bytes (if we request them)
     *
     * We need to seek to 0 before each transaction.
     */
    if (lseek(fd, 0, SEEK_SET) < 0)
        return -1;

    if (write(fd, &cmd, 1) != 1) {
        perror("w1 write command");
        return -1;
    }

    if (rbuf && rlen > 0) {
        /* For reads we need a fresh transaction — reset + select + cmd + read */
        /* Actually on most w1 sysfs implementations, we send cmd and then read
         * in the same transaction.  Some need a small delay. */
        usleep(10000);  /* 10 ms settle */

        int n = read(fd, rbuf, rlen);
        if (n != rlen) {
            perror("w1 read response");
            return -1;
        }
    }

    return 0;
}

/* ── Find DS1821 devices on the bus ──────────────────────────────── */

static int find_ds1821(char *out_id, size_t out_sz)
{
    DIR *dir = opendir(W1_DEVICES_DIR);
    struct dirent *ent;

    if (!dir) {
        fprintf(stderr, "Cannot open %s: %s\n"
                "  Is the w1 bus master loaded?\n",
                W1_DEVICES_DIR, strerror(errno));
        return -1;
    }

    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, DS1821_FAMILY "-", 3) == 0) {
            snprintf(out_id, out_sz, "%s", ent->d_name);
            closedir(dir);
            return 0;
        }
    }

    closedir(dir);
    fprintf(stderr, "No DS1821 (family %s) found on the 1-Wire bus.\n"
            "  Devices present in %s:\n", DS1821_FAMILY, W1_DEVICES_DIR);

    dir = opendir(W1_DEVICES_DIR);
    if (dir) {
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] != '.')
                fprintf(stderr, "    %s\n", ent->d_name);
        }
        closedir(dir);
    }
    return -1;
}

/* ── Read temperature from real hardware ─────────────────────────── */

static int read_hw_temperature(const char *device_id, float *temp_out, int *millideg_out)
{
    int fd = w1_open_rw(device_id);
    if (fd < 0) {
        fprintf(stderr, "Cannot open rw for %s: %s\n", device_id, strerror(errno));
        return -1;
    }

    /* Step 1: Start Convert T (0xEE) */
    if (w1_command(fd, DS1821_CMD_START_CONVERT, NULL, 0) < 0) {
        close(fd);
        return -1;
    }

    /* Wait for conversion — DS1821 takes up to 1 second */
    printf("  Converting...");
    fflush(stdout);
    usleep(1000000);  /* 1 second */
    printf(" done\n");

    /* Step 2: Read temperature register (0xAA → 1 byte) */
    uint8_t raw_temp;
    if (lseek(fd, 0, SEEK_SET) < 0 ||
        write(fd, (uint8_t[]){DS1821_CMD_READ_TEMP}, 1) != 1) {
        perror("write READ_TEMP");
        close(fd);
        return -1;
    }
    usleep(10000);
    if (read(fd, &raw_temp, 1) != 1) {
        perror("read temperature");
        close(fd);
        return -1;
    }

    /* Step 3: Read COUNT_REMAIN (0xA0 → 1 byte) */
    uint8_t count_remain;
    if (lseek(fd, 0, SEEK_SET) < 0 ||
        write(fd, (uint8_t[]){DS1821_CMD_READ_COUNTER}, 1) != 1) {
        perror("write READ_COUNTER");
        close(fd);
        return -1;
    }
    usleep(10000);
    if (read(fd, &count_remain, 1) != 1) {
        perror("read counter");
        close(fd);
        return -1;
    }

    /* Step 4: Read COUNT_PER_C (0xA9 → 1 byte) */
    uint8_t count_per_c;
    if (lseek(fd, 0, SEEK_SET) < 0 ||
        write(fd, (uint8_t[]){DS1821_CMD_READ_SLOPE}, 1) != 1) {
        perror("write READ_SLOPE");
        close(fd);
        return -1;
    }
    usleep(10000);
    if (read(fd, &count_per_c, 1) != 1) {
        perror("read slope");
        close(fd);
        return -1;
    }

    close(fd);

    /* Step 5: Compute high-resolution temperature */
    int8_t temp_int = (int8_t)raw_temp;
    int cpc = count_per_c ? count_per_c : 1;

    *temp_out = (float)temp_int - 0.25f +
                (float)(cpc - count_remain) / (float)cpc;

    *millideg_out = (int)temp_int * 1000 - 250 +
                    ((int)(cpc - count_remain) * 1000) / cpc;

    return 0;
}

/* ── Pretty-print ────────────────────────────────────────────────── */

static void print_temp(float temp_c, int millideg)
{
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%H:%M:%S", tm);

    printf("  [%s]  %.2f °C  (%d m°C)\n", ts, temp_c, millideg);
}

/* ── Usage ───────────────────────────────────────────────────────── */

static void usage(const char *prog)
{
    printf("Usage: %s [OPTIONS] [DEVICE-ID]\n\n"
           "Read temperature from a DS1821 1-Wire sensor.\n\n"
           "Options:\n"
           "  --loop [N]      Read continuously every N seconds (default: 2)\n"
           "  --help          Show this help\n\n"
           "Examples:\n"
           "  %s                          Auto-detect DS1821 on bus\n"
           "  %s 22-0123456789ab          Read specific device\n"
           "  %s --loop 1                 Continuous reading every second\n",
           prog, prog, prog, prog);
}

/* ── main ────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    int loop_mode = 0;
    int loop_sec = 2;
    const char *device_id = NULL;

    /* Parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--loop") == 0) {
            loop_mode = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                loop_sec = atoi(argv[++i]);
                if (loop_sec < 1) loop_sec = 1;
            }
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            device_id = argv[i];
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    signal(SIGINT, sigint_handler);

    printf("DS1821 Temperature Reader\n");
    printf("─────────────────────────\n");

    {
        /* Real hardware path */
        char dev_id[256];

        if (device_id) {
            snprintf(dev_id, sizeof(dev_id), "%s", device_id);
        } else {
            printf("Scanning for DS1821 devices...\n");
            if (find_ds1821(dev_id, sizeof(dev_id)) < 0) {
                return 1;
            }
        }

        printf("Device: %s\n\n", dev_id);

        do {
            float temp;
            int millideg;
            if (read_hw_temperature(dev_id, &temp, &millideg) == 0) {
                print_temp(temp, millideg);
            } else {
                fprintf(stderr, "  Read failed\n");
                if (!loop_mode) return 1;
            }

            if (loop_mode && keep_running) {
                printf("\n");
                sleep(loop_sec);
            }
        } while (loop_mode && keep_running);
    }

    if (!keep_running)
        printf("\nInterrupted.\n");

    return 0;
}
