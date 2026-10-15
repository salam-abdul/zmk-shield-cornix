"""Compile the real patched scan function with a deterministic fake Bluetooth stack.

Usage: python3 tests/test_scan_retry.py /path/to/zmk/app/src/split/bluetooth/central.c
Requires a C compiler; no Zephyr hardware or Python packages needed.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def function(source, signature):
    start = source.index(signature + " {")
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


source = Path(sys.argv[1]).read_text()
scan = function(source, "static int start_scanning(void)")
retry = function(source, "static void retry_scanning_work_cb(struct k_work *work)")
preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#define ARG_UNUSED(x) (void)(x)
#define LOG_DBG(...) ((void)0)
#define LOG_ERR(...) ((void)0)
#define CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS 2
#define BT_LE_SCAN_PASSIVE NULL
#define K_SECONDS(x) ((x) * 1000)
struct k_work { int unused; };
struct k_work_delayable { int unused; };
static struct k_work_delayable retry_scanning_work;
static bool is_enabled = true, is_scanning;
static struct { void *conn; } peripherals[2];
static int scan_result, scan_calls, retries, cancellations;
static void split_central_device_found(void) {}
static int bt_le_scan_start(void *params, void (*cb)(void)) {
    (void)params; (void)cb; scan_calls++; return scan_result;
}
static int k_work_reschedule(struct k_work_delayable *work, int delay) {
    assert(work == &retry_scanning_work && delay == 1000); retries++; return 0;
}
static int k_work_cancel_delayable(struct k_work_delayable *work) {
    assert(work == &retry_scanning_work); cancellations++; return 0;
}
static int start_scanning(void);
'''
main = r'''
int main(void) {
    // Transient errors leave the scanner retryable, including repeated errors.
    scan_result = -ENOMEM;
    assert(start_scanning() == -ENOMEM);
    assert(!is_scanning && retries == 1 && scan_calls == 1);
    retry_scanning_work_cb(NULL);
    assert(!is_scanning && retries == 2 && scan_calls == 2);
    // A later retry succeeds and cancels pending retry work.
    scan_result = 0;
    retry_scanning_work_cb(NULL);
    assert(is_scanning && scan_calls == 3 && cancellations == 1);
    assert(start_scanning() == 0 && scan_calls == 3);
    // Bluetooth reporting an existing scanner is a successful reconciliation.
    is_scanning = false;
    scan_result = -EALREADY;
    assert(start_scanning() == 0 && is_scanning && scan_calls == 4);
    // A pending retry must respect transport disable and fully connected state.
    is_scanning = false;
    is_enabled = false;
    retry_scanning_work_cb(NULL);
    assert(!is_scanning && scan_calls == 4 && retries == 2);
    is_enabled = true;
    peripherals[0].conn = peripherals[1].conn = (void *)1;
    retry_scanning_work_cb(NULL);
    assert(!is_scanning && scan_calls == 4 && retries == 2);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="cornix-scan-test-") as directory:
    cfile = Path(directory) / "scan_test.c"
    binary = Path(directory) / "scan_test"
    cfile.write_text(preamble + retry + scan + main)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", str(cfile), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("PASS: failed/repeated scan starts, delayed recovery, existing scan, disable, and fully connected guards")
