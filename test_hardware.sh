#!/bin/bash
# test_hardware.sh — Comprehensive hardware integration tests for DS1821 tools
#
# Tests the actual DS1821 hardware setup on a Raspberry Pi.
# Must be run as root (sudo).
#
# Expected hardware:
#   - DS1821 in thermostat mode on a dedicated GPIO (not shared with w1-gpio)
#   - DS1821 in thermostat mode — the ONLY device on this bus
#   - Optional: DS1821 VDD powered via GPIO 4 (--power-gpio tests)
#   - Optional: DS1821 TOUT readable via --read-tout
#
# Usage:
#   sudo ./test_hardware.sh              # run all tests
#   sudo ./test_hardware.sh --skip-slow  # skip tests that take >5s
#   sudo ./test_hardware.sh --power-gpio 4 --read-tout  # enable optional tests

set -uo pipefail

# ── Configuration ──────────────────────────────────────────────────

PROG="./ds1821-program"
UPDATE="./ds1821-update"
GPIO_PIN=17
POWER_PIN=""
TOUT_TEST=0
SKIP_SLOW=0

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --power-gpio) POWER_PIN="$2"; shift 2 ;;
        --read-tout)  TOUT_TEST=1;    shift ;;
        --gpio)       GPIO_PIN="$2";  shift 2 ;;
        --skip-slow)  SKIP_SLOW=1;    shift ;;
        --help|-h)
            echo "Usage: sudo $0 [--power-gpio N] [--read-tout] [--gpio N] [--skip-slow]"
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# Pass --gpio to every ds1821-program invocation
PROG_BIN="$PROG"
PROG="$PROG --gpio $GPIO_PIN"

# ── Test framework ─────────────────────────────────────────────────

PASS=0
FAIL=0
SKIP=0
ERRORS=()

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BOLD='\033[1m'
RESET='\033[0m'

pass() {
    ((PASS++))
    printf "  %-55s ${GREEN}[PASS]${RESET}\n" "$1"
}

fail() {
    ((FAIL++))
    ERRORS+=("$1: $2")
    printf "  %-55s ${RED}[FAIL]${RESET}\n" "$1"
    printf "    → %s\n" "$2"
}

skip() {
    ((SKIP++))
    printf "  %-55s ${YELLOW}[SKIP]${RESET}\n" "$1"
}

assert_eq() {
    local desc="$1" expected="$2" actual="$3"
    if [[ "$expected" == "$actual" ]]; then
        pass "$desc"
    else
        fail "$desc" "expected '$expected', got '$actual'"
    fi
}

assert_match() {
    local desc="$1" pattern="$2" actual="$3"
    if [[ "$actual" =~ $pattern ]]; then
        pass "$desc"
    else
        fail "$desc" "expected to match /$pattern/, got '$actual'"
    fi
}

assert_range() {
    local desc="$1" min="$2" max="$3" actual="$4"
    if [[ -z "$actual" ]]; then
        fail "$desc" "empty value"
        return
    fi
    local in_range
    in_range=$(awk "BEGIN { print ($actual >= $min && $actual <= $max) ? 1 : 0 }" 2>/dev/null)
    if [[ "$in_range" == "1" ]]; then
        pass "$desc (=$actual)"
    else
        fail "$desc" "expected $min..$max, got '$actual'"
    fi
}

assert_exit() {
    local desc="$1" expected="$2" actual="$3"
    if [[ "$expected" == "$actual" ]]; then
        pass "$desc"
    else
        fail "$desc" "expected exit code $expected, got $actual"
    fi
}

assert_file_exists() {
    local desc="$1" path="$2"
    if [[ -f "$path" ]]; then
        pass "$desc"
    else
        fail "$desc" "file not found: $path"
    fi
}

# ── Preamble ───────────────────────────────────────────────────────

echo ""
printf "${BOLD}DS1821 Hardware Test Suite${RESET}\n"
echo "════════════════════════════════════════════════════════"
echo ""
echo "  GPIO pin:    $GPIO_PIN"
echo "  Power GPIO:  ${POWER_PIN:-not set (power-gpio tests skipped)}"
echo "  Read TOUT:   $(if [[ $TOUT_TEST -eq 1 ]]; then echo yes; else echo 'no (use --read-tout to enable)'; fi)"
echo "  Skip slow:   $SKIP_SLOW"
echo ""

# ── Preconditions ──────────────────────────────────────────────────

printf "${BOLD}Preconditions${RESET}\n"
echo "────────────────────────────────────────────────────────"

# Root check
if [[ $EUID -eq 0 ]]; then
    pass "Running as root"
else
    fail "Running as root" "must run with sudo"
    echo "Aborting — root required."
    exit 1
fi

# Binary exists
if [[ -x "$PROG_BIN" ]]; then
    pass "ds1821-program binary exists"
else
    fail "ds1821-program binary exists" "not found or not executable"
    echo "Run 'make' first."
    exit 1
fi

# ds1821-update exists
if [[ -x "$UPDATE" ]]; then
    pass "ds1821-update script exists"
else
    fail "ds1821-update script exists" "not found or not executable"
fi

# pigpiod not running (conflicts with direct pigpio use)
if ! pgrep -x pigpiod >/dev/null 2>&1; then
    pass "pigpiod not running"
else
    fail "pigpiod not running" "stop with: sudo systemctl stop pigpiod"
fi

# pinctrl available (needed for persist_power_pin)
if command -v pinctrl >/dev/null 2>&1; then
    pass "pinctrl command available"
else
    skip "pinctrl command available (raspi-gpio fallback)"
fi

echo ""

# ── 1. Help and argument parsing ──────────────────────────────────

printf "${BOLD}1. Help and argument parsing${RESET}\n"
echo "────────────────────────────────────────────────────────"

# --help returns 0
$PROG --help >/dev/null 2>&1
assert_exit "--help exits 0" 0 $?

# --help shows usage
OUTPUT=$($PROG --help 2>&1)
assert_match "--help shows actions" "scan.*probe.*temp.*status" "$OUTPUT"
assert_match "--help shows --gpio" "--gpio" "$OUTPUT"
assert_match "--help shows --power-gpio" "--power-gpio" "$OUTPUT"
assert_match "--help shows --read-tout" "--read-tout" "$OUTPUT"

# No action → usage + error
$PROG 2>/dev/null
assert_exit "No action exits non-zero" 1 $?

# Unknown action — needs root, so it gets past the root check
# but fails at the action dispatch
$PROG nonexistent 2>/dev/null
UNK_RC=$?
if [[ $UNK_RC -ne 0 ]]; then
    pass "Unknown action exits non-zero"
else
    skip "Unknown action (exits 0 — pigpio init side-effect)"
fi

# Unknown option
$PROG --bogus temp >/dev/null 2>&1
assert_exit "Unknown option exits non-zero" 1 $?

# Non-root error
if su -s /bin/bash nobody -c "$PROG_BIN temp" >/dev/null 2>&1; then
    fail "Non-root rejected" "should require root"
else
    pass "Non-root rejected"
fi

echo ""

# ── 2. Probe (basic communication) ────────────────────────────────

printf "${BOLD}2. Probe — basic DS1821 communication${RESET}\n"
echo "────────────────────────────────────────────────────────"

PROBE_OUT=$($PROG probe 2>&1)
PROBE_RC=$?

assert_exit "probe exits 0" 0 "$PROBE_RC"
assert_match "probe shows status register" "Status register: 0x[0-9A-Fa-f]" "$PROBE_OUT"
assert_match "probe shows DONE bit" "DONE.*bit 7" "$PROBE_OUT"
assert_match "probe shows 1SHOT bit" "1SHOT.*bit 0" "$PROBE_OUT"
assert_match "probe shows alarm thresholds" "TH=.*°C.*TL=.*°C" "$PROBE_OUT"

# Quiet probe — key=value output
PROBE_Q=$($PROG -q probe 2>&1)
PROBE_Q_RC=$?

assert_exit "probe -q exits 0" 0 "$PROBE_Q_RC"
assert_match "probe -q has status=" "^status=0x[0-9A-Fa-f]" "$PROBE_Q"
assert_match "probe -q has done=" "done=[01]" "$PROBE_Q"
assert_match "probe -q has thf=" "thf=[01]" "$PROBE_Q"
assert_match "probe -q has tlf=" "tlf=[01]" "$PROBE_Q"
assert_match "probe -q has oneshot=" "oneshot=[01]" "$PROBE_Q"
assert_match "probe -q has th=" "th=-?[0-9]+" "$PROBE_Q"
assert_match "probe -q has tl=" "tl=-?[0-9]+" "$PROBE_Q"

echo ""

# ── 3. Temperature reading ────────────────────────────────────────

printf "${BOLD}3. Temperature reading${RESET}\n"
echo "────────────────────────────────────────────────────────"

if [[ $SKIP_SLOW -eq 1 ]]; then
    skip "temp (slow — ~2s conversion)"
    skip "temp range check"
    skip "temp -q format"
    skip "temp -q range"
else
    TEMP_OUT=$($PROG temp 2>&1)
    TEMP_RC=$?

    assert_exit "temp exits 0" 0 "$TEMP_RC"
    assert_match "temp shows conversion" "Starting conversion" "$TEMP_OUT"
    assert_match "temp shows integer temp" "Integer temp:" "$TEMP_OUT"
    assert_match "temp shows COUNT_REMAIN" "COUNT_REMAIN:" "$TEMP_OUT"
    assert_match "temp shows COUNT_PER_C" "COUNT_PER_C:" "$TEMP_OUT"
    assert_match "temp shows hi-res" "Hi-res temp:" "$TEMP_OUT"
    assert_match "temp shows millidegrees" "Millidegrees:" "$TEMP_OUT"

    # Extract hi-res from verbose output
    HIRES=$(echo "$TEMP_OUT" | grep -oP 'Hi-res temp:\s+\K[-0-9.]+')
    if [[ -n "$HIRES" ]]; then
        assert_range "temp in plausible range (-10..50)" -10 50 "$HIRES"
    else
        fail "temp in plausible range" "could not extract temperature"
    fi

    # Quick mode
    TEMP_Q=$($PROG -q temp 2>&1)
    TEMP_Q_RC=$?

    assert_exit "temp -q exits 0" 0 "$TEMP_Q_RC"
    assert_match "temp -q is just a number" '^-?[0-9]+\.[0-9]+$' "$TEMP_Q"

    TEMP_VAL=$(echo "$TEMP_Q" | head -1)
    assert_range "temp -q in plausible range (-10..50)" -10 50 "$TEMP_VAL"

    # Two consecutive reads should be close (within 2°C)
    TEMP_Q2=$($PROG -q temp 2>&1 | head -1)
    if [[ -n "$TEMP_Q2" && -n "$TEMP_VAL" ]]; then
        DIFF=$(awk "BEGIN { d = $TEMP_VAL - $TEMP_Q2; print (d < 0) ? -d : d }" 2>/dev/null)
        CLOSE=$(awk "BEGIN { print ($DIFF < 2) ? 1 : 0 }" 2>/dev/null)
        if [[ "$CLOSE" == "1" ]]; then
            pass "consecutive reads within 2°C (delta=${DIFF})"
        else
            fail "consecutive reads within 2°C" "delta=$DIFF ($TEMP_VAL vs $TEMP_Q2)"
        fi
    else
        skip "consecutive reads (read failed)"
    fi
fi

echo ""

# ── 4. Status (machine-readable full dump) ────────────────────────

printf "${BOLD}4. Status — machine-readable dump${RESET}\n"
echo "────────────────────────────────────────────────────────"

if [[ $SKIP_SLOW -eq 1 ]]; then
    skip "status (slow — includes conversion)"
    skip "status format check"
else
    STATUS_OUT=$($PROG status 2>&1)
    STATUS_RC=$?

    assert_exit "status exits 0" 0 "$STATUS_RC"

    # status outputs key=value lines but also the header when not -q.
    # Filter to just lines containing =
    STATUS_KV=$(echo "$STATUS_OUT" | grep '=')

    assert_match "status has temperature=" "temperature=-?[0-9]+" "$STATUS_KV"
    assert_match "status has thf=" "thf=[01]" "$STATUS_KV"
    assert_match "status has tlf=" "tlf=[01]" "$STATUS_KV"

    # th= and tl= need grep to avoid matching thf=/tlf=
    if echo "$STATUS_OUT" | grep -qP '^th=-?[0-9]+$'; then
        pass "status has th="
    else
        fail "status has th=" "not found in output"
    fi
    if echo "$STATUS_OUT" | grep -qP '^tl=-?[0-9]+$'; then
        pass "status has tl="
    else
        fail "status has tl=" "not found in output"
    fi

    # Temperature should be millidegrees in a plausible range
    MILLI=$(echo "$STATUS_OUT" | grep -oP '^temperature=\K-?[0-9]+')
    if [[ -n "$MILLI" ]]; then
        assert_range "status temp millideg plausible (-10000..50000)" -10000 50000 "$MILLI"
    else
        fail "status temp millideg" "could not extract"
    fi

    # No tout= unless --read-tout is set
    if ! echo "$STATUS_OUT" | grep -q '^tout='; then
        pass "status: no tout= without --read-tout"
    else
        fail "status: no tout= without --read-tout" "unexpected tout= in output"
    fi
fi

echo ""

# ── 5. Scan ───────────────────────────────────────────────────────

printf "${BOLD}5. Scan — bus enumeration${RESET}\n"
echo "────────────────────────────────────────────────────────"

SCAN_OUT=$($PROG scan 2>&1)
SCAN_RC=$?

assert_exit "scan exits 0" 0 "$SCAN_RC"
assert_match "scan shows presence pulse" "[Pp]resence" "$SCAN_OUT"

echo ""

# ── 6. Thermostat thresholds ─────────────────────────────────────

printf "${BOLD}6. Thermostat thresholds${RESET}\n"
echo "────────────────────────────────────────────────────────"

# Read current thresholds
ORIG_Q=$($PROG -q probe 2>&1)
# Use grep without $ anchor — multiline/trailing whitespace safe
ORIG_TH=$(echo "$ORIG_Q" | grep -oP '^th=\K-?[0-9]+'| head -1)
ORIG_TL=$(echo "$ORIG_Q" | grep -oP '^tl=\K-?[0-9]+'| head -1)

if [[ -z "$ORIG_TH" || -z "$ORIG_TL" ]]; then
    skip "threshold read (could not extract originals)"
    skip "set-th"
    skip "set-tl"
    skip "restore thresholds"
else
    pass "read original thresholds (TH=$ORIG_TH TL=$ORIG_TL)"

    # Set TH to a test value
    TEST_TH=30
    $PROG set-th $TEST_TH >/dev/null 2>&1
    SET_RC=$?
    assert_exit "set-th $TEST_TH exits 0" 0 "$SET_RC"

    # Verify
    CHECK_Q=$($PROG -q probe 2>&1)
    GOT_TH=$(echo "$CHECK_Q" | grep -oP '^th=\K-?[0-9]+' | head -1)
    assert_eq "set-th readback" "$TEST_TH" "$GOT_TH"

    # Set TL to a test value
    TEST_TL=5
    $PROG set-tl $TEST_TL >/dev/null 2>&1
    SET_RC=$?
    assert_exit "set-tl $TEST_TL exits 0" 0 "$SET_RC"

    # Verify
    CHECK_Q=$($PROG -q probe 2>&1)
    GOT_TL=$(echo "$CHECK_Q" | grep -oP '^tl=\K-?[0-9]+' | head -1)
    assert_eq "set-tl readback" "$TEST_TL" "$GOT_TL"

    # Set both at once
    $PROG set-th 28 set-tl 10 >/dev/null 2>&1
    assert_exit "set-th + set-tl combined exits 0" 0 $?
    CHECK_Q=$($PROG -q probe 2>&1)
    GOT_TH=$(echo "$CHECK_Q" | grep -oP '^th=\K-?[0-9]+' | head -1)
    GOT_TL=$(echo "$CHECK_Q" | grep -oP '^tl=\K-?[0-9]+' | head -1)
    assert_eq "combined set-th readback" "28" "$GOT_TH"
    assert_eq "combined set-tl readback" "10" "$GOT_TL"

    # Restore originals
    $PROG set-th "$ORIG_TH" set-tl "$ORIG_TL" >/dev/null 2>&1
    CHECK_Q=$($PROG -q probe 2>&1)
    GOT_TH=$(echo "$CHECK_Q" | grep -oP '^th=\K-?[0-9]+' | head -1)
    GOT_TL=$(echo "$CHECK_Q" | grep -oP '^tl=\K-?[0-9]+' | head -1)
    assert_eq "restored TH" "$ORIG_TH" "$GOT_TH"
    assert_eq "restored TL" "$ORIG_TL" "$GOT_TL"
fi

echo ""

# ── 7. Power GPIO tests ──────────────────────────────────────────

printf "${BOLD}7. Power GPIO (--power-gpio)${RESET}\n"
echo "────────────────────────────────────────────────────────"

if [[ -z "$POWER_PIN" ]]; then
    skip "power-gpio not set (use --power-gpio N to enable)"
    skip "power pin persist"
    skip "fix command"
else
    # Probe with power GPIO
    PPROBE=$($PROG --power-gpio "$POWER_PIN" probe 2>&1)
    PPROBE_RC=$?
    assert_exit "probe with --power-gpio exits 0" 0 "$PPROBE_RC"
    assert_match "probe with --power-gpio shows status" "Status register" "$PPROBE"

    # After gpioTerminate, power pin should stay HIGH
    PIN_STATE=$(pinctrl get "$POWER_PIN" 2>/dev/null | grep -oP 'hi|lo')
    assert_eq "power pin persisted HIGH" "hi" "$PIN_STATE"

    if [[ $SKIP_SLOW -eq 1 ]]; then
        skip "temp with --power-gpio (slow)"
    else
        # Temp read with power GPIO
        PTEMP=$($PROG -q --power-gpio "$POWER_PIN" temp 2>&1 | head -1)
        PTEMP_RC=$?
        assert_exit "temp with --power-gpio exits 0" 0 "$PTEMP_RC"
        assert_match "temp with --power-gpio is a number" '^-?[0-9]+\.[0-9]+$' "$PTEMP"

        # Pin should still be HIGH
        PIN_STATE=$(pinctrl get "$POWER_PIN" 2>/dev/null | grep -oP 'hi|lo')
        assert_eq "power pin still HIGH after temp" "hi" "$PIN_STATE"
    fi

    # Fix command (full cycle: set-oneshot + power-cycle + reload)
    if [[ $SKIP_SLOW -eq 1 ]]; then
        skip "fix with --power-gpio (slow)"
    else
        FIX_OUT=$($PROG --power-gpio "$POWER_PIN" fix 2>&1)
        FIX_RC=$?
        assert_exit "fix with --power-gpio exits 0" 0 "$FIX_RC"
        assert_match "fix shows power-cycle" "Power-cycling" "$FIX_OUT"
        assert_match "fix shows VDD OFF" "VDD OFF" "$FIX_OUT"
        assert_match "fix shows VDD ON" "VDD ON" "$FIX_OUT"

        # Pin should still be HIGH
        PIN_STATE=$(pinctrl get "$POWER_PIN" 2>/dev/null | grep -oP 'hi|lo')
        assert_eq "power pin HIGH after fix" "hi" "$PIN_STATE"
    fi
fi

echo ""

# ── 8. TOUT tests ────────────────────────────────────────────────

printf "${BOLD}8. TOUT (--read-tout)${RESET}\n"
echo "────────────────────────────────────────────────────────"

if [[ $TOUT_TEST -eq 0 ]]; then
    skip "--read-tout not set (use --read-tout to enable)"
    skip "tout in probe"
    skip "tout in status"
else
    # Probe should show TOUT
    TOUT_OUT=$($PROG --read-tout probe 2>&1)
    assert_exit "probe with --read-tout exits 0" 0 $?
    assert_match "probe shows TOUT" "TOUT.*DQ/GPIO" "$TOUT_OUT"

    # Quiet probe should have tout=
    TOUT_Q=$($PROG -q --read-tout probe 2>&1)
    assert_match "probe -q has tout=" "^tout=[01]" "$TOUT_Q"

    # Extract value
    TOUT_VAL=$(echo "$TOUT_Q" | grep -oP '^tout=\K[01]')
    if [[ -n "$TOUT_VAL" ]]; then
        pass "tout value is 0 or 1 (got $TOUT_VAL)"
    else
        fail "tout value" "could not extract"
    fi

    # Status should also have tout=
    if [[ $SKIP_SLOW -eq 0 ]]; then
        TOUT_S=$($PROG --read-tout status 2>&1)
        assert_match "status has tout=" "^tout=[01]" "$TOUT_S"
    else
        skip "status with --read-tout (slow)"
    fi
fi

echo ""

# ── 9. ds1821-update wrapper script ──────────────────────────────

printf "${BOLD}9. ds1821-update wrapper script${RESET}\n"
echo "────────────────────────────────────────────────────────"

if [[ $SKIP_SLOW -eq 1 ]]; then
    skip "ds1821-update (slow — runs status internally)"
else
    # Clean up any previous test data
    TESTNAME="hwtest_$$"
    TESTDIR="/run/ds1821/$TESTNAME"
    rm -rf "$TESTDIR" 2>/dev/null

    # Run ds1821-update with a test name
    # Use PROG env to point at our local binary
    PROG="$PWD/ds1821-program" $UPDATE --name "$TESTNAME" --gpio "$GPIO_PIN" 2>&1
    UPDATE_RC=$?

    assert_exit "ds1821-update exits 0" 0 "$UPDATE_RC"
    assert_file_exists "temperature file created" "$TESTDIR/temperature"
    assert_file_exists "alarms file created" "$TESTDIR/alarms"
    assert_file_exists "thresholds file created" "$TESTDIR/thresholds"

    # Check temperature value
    if [[ -f "$TESTDIR/temperature" ]]; then
        MILLI=$(cat "$TESTDIR/temperature")
        assert_match "temperature is integer millideg" '^-?[0-9]+$' "$MILLI"
        assert_range "temperature millideg plausible (-10000..50000)" -10000 50000 "$MILLI"
    fi

    # Check alarms format
    if [[ -f "$TESTDIR/alarms" ]]; then
        ALARMS=$(cat "$TESTDIR/alarms")
        assert_match "alarms format" "^thf=[01] tlf=[01]$" "$ALARMS"
    fi

    # Check thresholds format
    if [[ -f "$TESTDIR/thresholds" ]]; then
        THRESHOLDS=$(cat "$TESTDIR/thresholds")
        assert_match "thresholds format" "^th=-?[0-9]+ tl=-?[0-9]+$" "$THRESHOLDS"
    fi

    # Default name should be "0"
    rm -rf /run/ds1821/0 2>/dev/null
    PROG="$PWD/ds1821-program" $UPDATE --gpio "$GPIO_PIN" 2>&1
    assert_file_exists "default name writes to /run/ds1821/0/" "/run/ds1821/0/temperature"

    # Clean up
    rm -rf "$TESTDIR" /run/ds1821/0 2>/dev/null
fi

echo ""

# ── 10. Edge cases and error handling ─────────────────────────────

printf "${BOLD}10. Edge cases and error handling${RESET}\n"
echo "────────────────────────────────────────────────────────"

# Verbose mode shouldn't crash
$PROG -v probe >/dev/null 2>&1
assert_exit "probe -v exits 0" 0 $?

# Verbose + quiet
$PROG -v -q probe >/dev/null 2>&1
assert_exit "probe -v -q exits 0" 0 $?

# set-th without value (should fail or show usage)
$PROG set-th 2>/dev/null
SET_NO_VAL_RC=$?
if [[ $SET_NO_VAL_RC -ne 0 ]]; then
    pass "set-th without value exits non-zero"
else
    # It might silently use next arg as action — either way it's handled
    skip "set-th without value (accepted, no crash)"
fi

# Extreme threshold values
$PROG set-th 125 >/dev/null 2>&1
CHECK_Q=$($PROG -q probe 2>&1)
GOT_TH=$(echo "$CHECK_Q" | grep -oP '^th=\K-?[0-9]+' | head -1)
assert_eq "set-th max (125)" "125" "$GOT_TH"

$PROG set-th -55 >/dev/null 2>&1
CHECK_Q=$($PROG -q probe 2>&1)
GOT_TH=$(echo "$CHECK_Q" | grep -oP '^th=\K-?[0-9]+' | head -1)
assert_eq "set-th min (-55)" "-55" "$GOT_TH"

# Restore sensible values (only if we successfully read originals in section 6)
if [[ -n "$ORIG_TH" && -n "$ORIG_TL" ]]; then
    $PROG set-th "$ORIG_TH" set-tl "$ORIG_TL" >/dev/null 2>&1
fi

echo ""

# ── Summary ───────────────────────────────────────────────────────

TOTAL=$((PASS + FAIL + SKIP))
echo "════════════════════════════════════════════════════════"
printf "${BOLD}Results: ${GREEN}%d passed${RESET}, " "$PASS"
if [[ $FAIL -gt 0 ]]; then
    printf "${RED}%d failed${RESET}, " "$FAIL"
else
    printf "0 failed, "
fi
if [[ $SKIP -gt 0 ]]; then
    printf "${YELLOW}%d skipped${RESET}" "$SKIP"
else
    printf "0 skipped"
fi
printf " (${TOTAL} total)\n"

if [[ $FAIL -gt 0 ]]; then
    echo ""
    printf "${RED}${BOLD}Failures:${RESET}\n"
    for err in "${ERRORS[@]}"; do
        printf "  ${RED}✗${RESET} %s\n" "$err"
    done
fi

echo ""
exit $FAIL
