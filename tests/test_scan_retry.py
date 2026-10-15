"""Compile actual patched ZMK scan functions with controlled host interleavings.

Usage: python3 tests/test_scan_retry.py /path/to/patched/central.c
Requires a host C compiler and pthreads; no Python packages or Zephyr hardware.
This checks selected interleavings, not the complete Zephyr scheduler or radio.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def function(source, signature):
    start = source.index(signature + " {")
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


source = Path(sys.argv[1]).read_text()
signatures = [
    "static void retry_scanning_work_cb(struct k_work *work)",
    "static int stop_scanning_locked(void)",
    "static int stop_scanning(void)",
    "static bool split_central_eir_found(const bt_addr_le_t *addr)",
    "static int start_scanning(void)",
]
actual_functions = "\n".join(function(source, signature) for signature in signatures)
preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#define ARG_UNUSED(x) (void)(x)
#define LOG_DBG(...) ((void)0)
#define LOG_INF(...) ((void)0)
#define LOG_ERR(...) ((void)0)
#define CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS 2
#define CONFIG_ZMK_SPLIT_BLE_PREF_INT 6
#define CONFIG_ZMK_SPLIT_BLE_PREF_LATENCY 30
#define CONFIG_ZMK_SPLIT_BLE_PREF_TIMEOUT 400
#define BT_LE_SCAN_PASSIVE NULL
#define BT_CONN_LE_CREATE_CONN NULL
#define BT_HCI_OP_LE_CREATE_CONN 0
#define K_SECONDS(x) ((x) * 1000)
#define K_NO_WAIT 0
#define BT_LE_CONN_PARAM(a,b,c,d) (&connection_params)
struct k_work { int unused; };
struct k_work_delayable { int unused; };
static struct k_work_delayable retry_scanning_work;
static pthread_mutex_t scan_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool is_enabled = true, is_scanning;
enum peripheral_slot_state { PERIPHERAL_SLOT_STATE_OPEN,
    PERIPHERAL_SLOT_STATE_CONNECTING, PERIPHERAL_SLOT_STATE_CONNECTED };
struct peripheral_slot { enum peripheral_slot_state state; void *conn; };
static struct peripheral_slot peripherals[2];
typedef struct { int unused; } bt_addr_le_t;
struct bt_le_conn_param { int unused; };
static struct bt_le_conn_param connection_params;
static const bt_addr_le_t address;
static int scan_result, stop_result, scan_calls, stop_calls;
static int reservations, releases, creates, create_result, now;
static atomic_int pending_deadline;
static pthread_mutex_t gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static int block_operation; // 1=start, 2=stop
static bool entered, released;
static void block_if_requested(int operation) {
    pthread_mutex_lock(&gate_mutex);
    if (block_operation == operation) {
        entered = true;
        pthread_cond_broadcast(&gate_cond);
        while (!released) pthread_cond_wait(&gate_cond, &gate_mutex);
    }
    pthread_mutex_unlock(&gate_mutex);
}
static void wait_until_blocked(void) {
    pthread_mutex_lock(&gate_mutex);
    while (!entered) pthread_cond_wait(&gate_cond, &gate_mutex);
    pthread_mutex_unlock(&gate_mutex);
}
static void release_blocked_operation(void) {
    pthread_mutex_lock(&gate_mutex);
    released = true;
    pthread_cond_broadcast(&gate_cond);
    pthread_mutex_unlock(&gate_mutex);
}
static int k_mutex_lock(pthread_mutex_t *mutex, int timeout) {
    assert(timeout == K_NO_WAIT);
    int err = pthread_mutex_trylock(mutex);
    return err ? -err : 0;
}
static void k_mutex_unlock(pthread_mutex_t *mutex) {
    assert(pthread_mutex_unlock(mutex) == 0);
}
static void split_central_device_found(void) {}
static int bt_le_scan_start(void *params, void (*cb)(void)) {
    (void)params; (void)cb; scan_calls++;
    block_if_requested(1);
    return scan_result;
}
static int bt_le_scan_stop(void) {
    stop_calls++;
    block_if_requested(2);
    return stop_result;
}
static int k_work_reschedule(struct k_work_delayable *work, int delay) {
    assert(work == &retry_scanning_work && delay == 1000);
    atomic_store(&pending_deadline, now + delay);
    return 0;
}
static int reserve_peripheral_slot(const bt_addr_le_t *addr) {
    (void)addr; reservations++;
    assert(peripherals[0].state == PERIPHERAL_SLOT_STATE_OPEN);
    peripherals[0].state = PERIPHERAL_SLOT_STATE_CONNECTING;
    return 0;
}
static int release_peripheral_slot(int index) {
    assert(index == 0); releases++;
    peripherals[index].state = PERIPHERAL_SLOT_STATE_OPEN;
    peripherals[index].conn = NULL;
    return 0;
}
static int bt_conn_le_create(const bt_addr_le_t *addr, void *options,
                            const struct bt_le_conn_param *params, void **conn) {
    (void)addr; (void)options; (void)params; creates++;
    if (!create_result) *conn = (void *)1;
    return create_result;
}
static int start_scanning(void);
'''
checks = r'''
static void reset(void) {
    is_enabled = true; is_scanning = false;
    memset(peripherals, 0, sizeof(peripherals));
    scan_result = stop_result = scan_calls = stop_calls = 0;
    reservations = releases = creates = create_result = 0;
    now = 1000; atomic_store(&pending_deadline, 0);
    block_operation = 0; entered = released = false;
}
static void run_retry(void) {
    int deadline = atomic_exchange(&pending_deadline, 0);
    assert(deadline >= now + 1000);
    now = deadline;
    retry_scanning_work_cb(NULL);
}
static void *operation_thread(void *operation) {
    int result = (intptr_t)operation == 1 ? start_scanning() : stop_scanning();
    return (void *)(intptr_t)result;
}
static void join_success(pthread_t thread) {
    void *result;
    assert(pthread_join(thread, &result) == 0);
    assert((intptr_t)result == 0);
}
int main(void) {
    int failures[] = {-ENOMEM, -EAGAIN, -EBUSY};
    for (unsigned i = 0; i < sizeof(failures)/sizeof(failures[0]); i++) {
        reset(); scan_result = failures[i];
        assert(start_scanning() == failures[i]);
        for (int j = 0; j < 3; j++) {
            assert(!is_scanning && atomic_load(&pending_deadline));
            run_retry();
        }
        scan_result = 0;
        run_retry();
        assert(is_scanning && scan_calls == 5 && !atomic_load(&pending_deadline));
    }
    // Another caller recovers before delayed work runs; the work is harmless.
    reset(); scan_result = -ENOMEM;
    assert(start_scanning() == -ENOMEM);
    scan_result = 0;
    assert(start_scanning() == 0 && atomic_load(&pending_deadline));
    run_retry(); assert(scan_calls == 2 && !atomic_load(&pending_deadline));
    reset(); scan_result = -EALREADY;
    assert(start_scanning() == 0 && is_scanning);
    assert(start_scanning() == 0 && scan_calls == 1);
    // Fully connected and either CONNECTING slot must suppress scan starts.
    reset();
    peripherals[0].conn = peripherals[1].conn = (void *)1;
    assert(start_scanning() == 0 && scan_calls == 0);
    for (int i = 0; i < 2; i++) {
        reset(); peripherals[i].state = PERIPHERAL_SLOT_STATE_CONNECTING;
        atomic_store(&pending_deadline, now + 1000);
        run_retry(); assert(!is_scanning && scan_calls == 0);
    }
    // Retry a disabled transport's failed stop; stop retrying on success.
    reset(); is_enabled = false; is_scanning = true; stop_result = -EIO;
    assert(start_scanning() == -EIO && !is_scanning);
    run_retry(); assert(stop_calls == 2 && atomic_load(&pending_deadline));
    stop_result = 0;
    run_retry(); assert(stop_calls == 3 && scan_calls == 0 && !atomic_load(&pending_deadline));
    // Disable while HCI scan-start is blocked. Contention must not block caller
    // or lose the stop request when the original start subsequently succeeds.
    pthread_t thread;
    reset(); block_operation = 1;
    assert(pthread_create(&thread, NULL, operation_thread, (void *)1) == 0);
    wait_until_blocked();
    is_enabled = false;
    assert(stop_scanning() < 0 && atomic_load(&pending_deadline));
    release_blocked_operation(); join_success(thread);
    run_retry(); assert(!is_scanning && scan_calls == 1 && stop_calls == 1);
    assert(!atomic_load(&pending_deadline));
    // Start requested while HCI scan-stop is blocked survives that stop.
    reset(); is_scanning = true; block_operation = 2;
    assert(pthread_create(&thread, NULL, operation_thread, (void *)2) == 0);
    wait_until_blocked();
    assert(start_scanning() < 0 && atomic_load(&pending_deadline));
    release_blocked_operation(); join_success(thread);
    run_retry(); assert(is_scanning && scan_calls == 1 && stop_calls == 1);
    // Stop contention after an advertisement reservation cannot leak the slot.
    reset(); block_operation = 1;
    assert(pthread_create(&thread, NULL, operation_thread, (void *)1) == 0);
    wait_until_blocked();
    assert(!split_central_eir_found(&address));
    assert(reservations == 1 && releases == 1 && creates == 0);
    assert(peripherals[0].state == PERIPHERAL_SLOT_STATE_OPEN);
    release_blocked_operation(); join_success(thread);
    run_retry(); assert(scan_calls == 1 && !atomic_load(&pending_deadline));
    // Actual scan-stop errors also release reservations and allow recovery.
    reset(); is_scanning = true; stop_result = -EIO;
    assert(!split_central_eir_found(&address));
    assert(reservations == 1 && releases == 1 && creates == 0);
    stop_result = 0; run_retry(); assert(is_scanning && scan_calls == 1);
    // Ignore late advertisements after disable.
    reset(); is_enabled = false;
    assert(!split_central_eir_found(&address));
    assert(reservations == 0 && creates == 0);
    // Existing connection-create failure must also release its slot/restart.
    reset(); is_scanning = true; create_result = -ENOMEM;
    assert(!split_central_eir_found(&address));
    assert(reservations == 1 && releases == 1 && creates == 1 && scan_calls == 1);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="cornix-scan-test-") as directory:
    cfile = Path(directory) / "scan_test.c"
    binary = Path(directory) / "scan_test"
    cfile.write_text(preamble + actual_functions + checks)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-pthread", "-Wall", "-Wextra", "-Werror", str(cfile), "-o", str(binary)], check=True, timeout=30)
    subprocess.run([str(binary)], check=True, timeout=10)
print("PASS: real scan functions, repeated failures, delayed recovery, shutdown/start-stop races, connecting guards, and reservation cleanup")
