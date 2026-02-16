# DS1821 Tools for Raspberry Pi

Read temperature from a **DS1821** 1-Wire thermometer/thermostat on a Raspberry Pi using GPIO.

## The problem

The DS1821 has a completely different protocol from most 1-Wire thermometers — no scratchpad, dedicated function commands, and a thermostat mode that makes it invisible to the kernel's `w1_therm` driver.

This project provides tools to communicate with a DS1821 by bit-banging the 1-Wire bus directly via GPIO.

## Why the DS1821 must have its own bus

The DS1821 **cannot share a 1-Wire bus** with any other device. This is a
hardware-level limitation, not a software bug.

### No ROM

Normal 1-Wire devices have a unique 64-bit ROM code. The master uses
Match ROM (0x55) or Skip ROM (0xCC) to address a specific device before
sending function commands. A DS1821 in thermostat mode **does not respond
to any ROM command** — it ignores Read ROM, Search ROM, and Match ROM
entirely. Instead, it immediately interprets the next byte after a reset
pulse as a function command.

This means every command intended for the DS1821 is effectively a broadcast:
every device on the bus sees it. When the master sends Start Convert (0xEE)
to the DS1821, any other device also sees 0xEE after the reset pulse. The
other device interprets that byte as part of its own protocol (typically a
ROM command), causing unpredictable behaviour on the bus.

### Reads collide

When the master reads data bits after a function command, the DS1821
responds — but so does any other device that is in an active state. 1-Wire
is an open-drain bus, so conflicting responses are AND-ed together at the
electrical level: if one device drives a `0` and another drives a `1`, the
bus reads `0`. This corrupts temperature readings, counter values, and
status registers.

### Thermostat mode makes it worse

In thermostat mode the DS1821 drives the DQ pin as TOUT (thermostat output)
whenever it is not being addressed. A DS1821 asserting TOUT low will pull
the entire bus low, preventing any other device from communicating at all.

**Each DS1821 must be the only device on its 1-Wire bus.** Use a separate
GPIO pin for each DS1821, and do not place any other 1-Wire device on the
same pin.

## Hardware setup

- Data line connected to GPIO 17 (configurable with `--gpio`)
- 4.7kΩ pull-up resistor to 3.3V

### Optional: GPIO-controlled power

If DS1821 VDD is connected to a GPIO pin (e.g. GPIO 4), you can power-cycle the device from software — needed for mode switching:

```
DS1821 VDD ── GPIO 4
DS1821 GND ── GND
DS1821 DQ  ── GPIO 17 (with 4.7kΩ pull-up)
```

### Thermostat output (TOUT)

In thermostat mode the DQ pin doubles as TOUT — it goes high/low when
temperature crosses the TH/TL thresholds. Since it's the same physical
pin as the 1-Wire data line, `--read-tout` reads the data GPIO
(default 17) to check the thermostat output state.

## Prerequisites

```bash
sudo apt install libpigpio-dev
```

## Build

```bash
make
```

This produces two binaries:

| Binary | Purpose |
|--------|---------|
| `ds1821-program` | GPIO bit-bang tool — reads DS1821 in thermostat mode |
| `ds1821-read` | Sysfs-based reader — for DS1821 in 1-Wire mode |
| `ds1821-update` | Wrapper script — writes readings to `/run/ds1821/` |

## Usage

### Read DS1821 temperature (thermostat mode)

```bash
# Quick read — outputs just the temperature
sudo ./ds1821-program -q temp
# 20.34

# Verbose read with full register dump
sudo ./ds1821-program temp
```

The tool reads the DS1821 via direct GPIO bit-bang using the pigpio library.

### Other commands

```bash
# Read status register and alarm thresholds
sudo ./ds1821-program probe

# Machine-readable probe (key=value output)
sudo ./ds1821-program -q probe

# Full status dump: temperature + thresholds + alarms + TOUT (key=value)
sudo ./ds1821-program status
sudo ./ds1821-program --read-tout status

# Set thermostat thresholds (both at once or individually)
sudo ./ds1821-program set-th 25 set-tl 18
sudo ./ds1821-program set-th 30
sudo ./ds1821-program set-tl 5

# Switch DS1821 to 1-Wire mode (needs power cycle)
sudo ./ds1821-program fix
sudo ./ds1821-program --power-gpio 4 fix   # auto power-cycle via GPIO 4
```

### Options

| Flag | Description |
|------|-------------|
| `--quick`, `-q` | Minimal output — just the temperature value (or key=value for `probe`) |
| `--gpio N` | Use GPIO pin N instead of default 17 |
| `--power-gpio N` | GPIO pin driving DS1821 VDD (enables `fix` auto power-cycle) |
| `--read-tout` | Read thermostat output state from DQ pin |
| `--verbose`, `-v` | Show low-level 1-Wire bit traffic |
| `--help`, `-h` | Show help |

### Write readings to `/run/ds1821/` (wrapper script)

`ds1821-update` reads the DS1821 and writes files under `/run/ds1821/<name>/`
that mirror the w1_therm sysfs layout.

By default it reads `/etc/ds1821/sensors.conf` to discover which sensors are
present. You can also use `--name` for one-off single-sensor reads.

```bash
# Read all sensors listed in /etc/ds1821/sensors.conf
sudo ds1821-update

# Single sensor (ignores config file)
sudo ds1821-update --name indoor --gpio 17

# Custom config file
sudo ds1821-update --config /path/to/sensors.conf

# Then read just like w1_therm:
cat /run/ds1821/indoor/temperature   # millidegrees, e.g. 20340
cat /run/ds1821/shed/alarms           # "thf=1 tlf=0"
cat /run/ds1821/shed/tout             # "0" or "1"
cat /run/ds1821/shed/thresholds       # "th=25 tl=18"
```

### Sensor configuration (`/etc/ds1821/sensors.conf`)

Define one sensor per line with its name, data GPIO, and optional power/TOUT
GPIO pins:

```
# <name>    <data-gpio>  [power-gpio]  [read-tout]
indoor      17
outdoor     27           4
shed        17           5             yes
```

| Field | Required | Description |
|-------|----------|-------------|
| name | yes | Identifier, used in `/run/ds1821/<name>/` |
| data-gpio | yes | GPIO pin for the 1-Wire data line |
| power-gpio | no | GPIO pin controlling DS1821 VDD (use `-` to skip) |
| read-tout | no | Read thermostat output from DQ pin (`yes` or `no`) |

The default config ships with a single sensor `0` on GPIO 17. Edit it to match
your wiring. The file is marked as a conffile in the Debian package, so your
edits are preserved across upgrades.

### Multiple DS1821s

Each DS1821 needs its own bus (see [Why the DS1821 must have its own bus](#why-the-ds1821-must-have-its-own-bus)). To use more than one, give each a separate GPIO pin, a separate power-control GPIO, or both:

**Option A: Separate GPIO buses** — Put each DS1821 on a different GPIO pin:
```
# /etc/ds1821/sensors.conf
indoor   17
outdoor  27
```

**Option B: Individual power control** — Wire each DS1821 VDD to a separate GPIO.
The tool powers one at a time:
```
# /etc/ds1821/sensors.conf
sensor1  17  4
sensor2  17  5
```

## Runtime files (`/run/ds1821/`)

`ds1821-update` writes sensor data to a tmpfs directory structure that mirrors
the kernel's `w1_therm` sysfs layout. Files are created under
`/run/ds1821/<name>/`, where `<name>` defaults to `0` or is set with `--name`.

```
/run/ds1821/<name>/
├── temperature    # millidegrees integer, e.g. 20340 = 20.34 °C
├── alarms         # "thf=<0|1> tlf=<0|1>"
├── thresholds     # "th=<N> tl=<N>"  (°C, integer)
└── tout           # "0" or "1"  (only if --read-tout was set)
```

| File | Format | Example | Notes |
|------|--------|---------|-------|
| `temperature` | integer (millidegrees) | `20340` | Always written |
| `alarms` | `thf=N tlf=N` | `thf=1 tlf=0` | High/low alarm flags |
| `thresholds` | `th=N tl=N` | `th=25 tl=18` | Thermostat thresholds (°C) |
| `tout` | `0` or `1` | `1` | Only present with `--read-tout` |

Example with two named sensors:

```bash
sudo ds1821-update --name indoor --gpio 17
sudo ds1821-update --name outdoor --gpio 27

cat /run/ds1821/indoor/temperature    # 20340
cat /run/ds1821/outdoor/temperature   # 18125
cat /run/ds1821/indoor/alarms         # thf=0 tlf=0
cat /run/ds1821/indoor/thresholds     # th=25 tl=18
```

## Systemd timer

A systemd timer is included to poll the DS1821 automatically. It is **not
enabled by default** — you must enable it manually after install:

```bash
sudo systemctl enable --now ds1821-update.timer
```

This runs `ds1821-update` every 60 seconds (first read 10 seconds after boot).
To customise the device name, GPIO pins, or polling interval, create a drop-in
override:

```bash
sudo systemctl edit ds1821-update.service
```

```ini
[Service]
ExecStart=
ExecStart=/usr/bin/ds1821-update --name indoor --gpio 17
```

To change the polling interval:

```bash
sudo systemctl edit ds1821-update.timer
```

```ini
[Timer]
OnUnitActiveSec=30s
```

Check status:

```bash
systemctl status ds1821-update.timer
systemctl list-timers ds1821-update*
journalctl -u ds1821-update.service
```

## Source files

| File | Description |
|------|-------------|
| `ds1821-program.c` | GPIO bit-bang utility (pigpio). Reads DS1821 in thermostat mode. |
| `ds1821-read.c` | Sysfs reader via `/sys/bus/w1/devices/*/rw`. For 1-Wire mode. |
| `ds1821-update` | Shell wrapper — writes readings to `/run/ds1821/<name>/`. |
| `sensors.conf` | Default config — sensor names and GPIO pins. Installs to `/etc/ds1821/`. |

| `test_hardware.sh` | Bash integration test suite (requires hardware + root) |
| `Makefile` | Build rules |

## How it works

The DS1821 in thermostat mode doesn't respond to ROM commands (Read ROM, Search ROM, Match ROM), so the kernel `w1` stack can't see it. However, it still responds to function commands after a reset pulse — you just skip the ROM layer entirely.

`ds1821-program` does this:

1. Initialises pigpio for microsecond-precision bit-bang
2. Sends reset → function command sequences directly (no ROM)
3. For temperature: Start Convert (0xEE) → wait 1s → Read Temp (0xAA) + counters
4. Computes high-resolution temperature: `T = integer - 0.25 + (COUNT_PER_C - COUNT_REMAIN) / COUNT_PER_C`
5. Terminates pigpio

## Running tests

```bash
sudo ./test_hardware.sh
```
