/*
 * KSN-2 central BLE connection-status LED
 *
 * - Not connected: blinks out (active profile index + 1) short pulses,
 *   pauses, then repeats - see ksn1-firmware's
 *   ksn1_conn_status_relay_peripheral.c for the full profile-count design
 *   rationale (ported here). KSN-2's status LED lives on the central half
 *   itself (unlike KSN-1/KSN-3 where it's on the peripheral), so everything
 *   here is a local function call - no GATT relay between halves needed.
 * - Connected, after showing the profile count at least
 *   KSN2_PROFILE_MIN_CYCLES times: LED steady on. An already-paired
 *   profile that reconnects almost instantly still plays the full count
 *   first, so a fast reconnect can't rob the user of the readout.
 *
 * Drives the `indicator-led` alias (ble_led, D20/P0.29 on the left/central
 * half). This LED used to be driven by zmk-poor-mans-led-indicator
 * (CONFIG_INDICATOR_LED_SHOW_BLE), but that widget only re-evaluates on a
 * zmk_ble_active_profile_changed event and each blink sequence ends in a
 * fixed state rather than tracking live connection status - so once that
 * one event passed, the LED just stayed in whatever state the sequence
 * ended on, regardless of whether the host was actually still connected.
 * That widget's LED indication is disabled in ksn_2_left.conf
 * (CONFIG_INDICATOR_LED_WIDGET=n) so it no longer drives this pin.
 *
 * NOTE: switched from zmk_ble_active_profile_is_connected() to
 * zmk_endpoint_is_connected() - the former only reflects the currently
 * selected BLE profile's own link and stays false while wired over USB.
 * This board has USB HID security hardening specifically for wired
 * corporate-PC use (see ksn_2_left.conf), so a USB-blind status LED was a
 * real functional gap, not just cosmetic.
 *
 * Guarded on DT_NODE_EXISTS(DT_ALIAS(indicator_led)) so this file is a
 * no-op on any shield build that doesn't define that alias (e.g.
 * ksn_2_right, settings_reset) - mirrors the guard pattern used in
 * ksn1-firmware's peripheral indicator driver and nexplit-keypad's.
 *
 * NOTE: <zephyr/devicetree.h> must be included BEFORE the #if below, since
 * DT_NODE_EXISTS/DT_ALIAS are ordinary macros defined by that header - if
 * the #if runs first, the preprocessor treats them as plain (undefined)
 * tokens and errors out with "missing binary operator before token (".
 */

#include <zephyr/devicetree.h>

#if DT_NODE_EXISTS(DT_ALIAS(indicator_led))

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>

LOG_MODULE_REGISTER(ksn2_conn_status, CONFIG_ZMK_LOG_LEVEL);

#define LED_NODE DT_ALIAS(indicator_led)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED_NODE, gpios);

/* Profile-count blink cycle, played whenever there is no active host
 * connection: (active profile index + 1) pulses of ON_MS/OFF_MS, then a
 * CYCLE_PAUSE_MS gap, then repeat for as long as disconnected. The "+1"
 * is so profile 0 still blinks once instead of looking identical to "no
 * signal at all". MIN_CYCLES is the minimum number of full cycles played
 * after any profile switch (or a drop from a previously-solid state)
 * before "connected" is allowed to turn the LED solid. */
#define KSN2_PROFILE_BLINK_ON_MS 150
#define KSN2_PROFILE_BLINK_OFF_MS 150
#define KSN2_PROFILE_CYCLE_PAUSE_MS 700
#define KSN2_PROFILE_MIN_CYCLES 5
/* Re-check interval once settled solid - purely a backstop in case a
 * disconnect is ever missed by the event listener below (which normally
 * re-triggers immediately on a profile-changed event). */
#define KSN2_RECHECK_CONNECTED_MS 1000

static bool led_is_on;

/* Profile-count cycle state. */
static uint8_t current_profile;
static uint8_t blink_index;   /* which pulse within the current cycle (0-based) */
static bool blink_on_phase;   /* mid-pulse: currently in the "on" half? */
static bool in_pause;         /* between cycles, waiting out CYCLE_PAUSE_MS */
static uint8_t cycles_played; /* full cycles completed since the last reset */
static bool settled_solid;    /* true once MIN_CYCLES was met and we've gone solid */
static bool have_applied_once;

static struct k_work_delayable led_work;

static void set_led(bool on) {
    led_is_on = on;
    gpio_pin_set_dt(&led, on ? 1 : 0);
}

static void start_cycle(void) {
    blink_index = 0;
    blink_on_phase = true;
    in_pause = false;
    set_led(true);
    k_work_reschedule(&led_work, K_MSEC(KSN2_PROFILE_BLINK_ON_MS));
}

static void led_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    bool connected = zmk_endpoint_is_connected();
    uint8_t profile = (uint8_t)zmk_ble_active_profile_index();

    /* Re-derived fresh on every tick (poll or event-triggered alike), so
     * there's no separate "apply" entry point like the relay-based boards
     * need - everything here is a direct local read. */
    bool profile_changed = (profile != current_profile);
    bool fresh_drop = (settled_solid && !connected);
    bool force_restart = !have_applied_once || profile_changed || fresh_drop;

    have_applied_once = true;
    current_profile = profile;

    if (force_restart) {
        cycles_played = 0;
        settled_solid = false;
        start_cycle();
        return;
    }

    if (settled_solid) {
        /* fresh_drop above already restarts the cycle the instant we're
         * no longer connected, so reaching here means still connected. */
        set_led(true);
        k_work_reschedule(&led_work, K_MSEC(KSN2_RECHECK_CONNECTED_MS));
        return;
    }

    if (in_pause) {
        /* A cycle just finished. Only now do we check whether we're
         * allowed to settle solid - never mid-pulse, so a connect event
         * arriving mid-blink can't cut a pulse short and make it
         * unreadable. */
        if (connected && cycles_played >= KSN2_PROFILE_MIN_CYCLES) {
            settled_solid = true;
            set_led(true);
            k_work_reschedule(&led_work, K_MSEC(KSN2_RECHECK_CONNECTED_MS));
            return;
        }
        start_cycle();
        return;
    }

    if (blink_on_phase) {
        set_led(false);
        blink_on_phase = false;
        k_work_reschedule(&led_work, K_MSEC(KSN2_PROFILE_BLINK_OFF_MS));
        return;
    }

    /* Finished one full pulse (on+off). One cycle = (current_profile + 1)
     * pulses, so profile 0 still blinks once instead of looking like "no
     * signal". */
    if (++blink_index >= (uint8_t)(current_profile + 1)) {
        cycles_played++;
        in_pause = true;
        set_led(false);
        k_work_reschedule(&led_work, K_MSEC(KSN2_PROFILE_CYCLE_PAUSE_MS));
        return;
    }

    blink_on_phase = true;
    set_led(true);
    k_work_reschedule(&led_work, K_MSEC(KSN2_PROFILE_BLINK_ON_MS));
}

static int ksn2_conn_status_init(void) {
    if (!gpio_is_ready_dt(&led)) {
        LOG_ERR("Indicator LED device not ready");
        return -ENODEV;
    }

    int err = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
    if (err) {
        LOG_ERR("Failed to configure indicator LED (%d)", err);
        return err;
    }

    k_work_init_delayable(&led_work, led_work_handler);
    k_work_schedule(&led_work, K_NO_WAIT);

    return 0;
}

SYS_INIT(ksn2_conn_status_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

static int ksn2_conn_status_event_listener(const zmk_event_t *eh) {
    /* Re-evaluate connection state immediately on profile/connection change
     * instead of waiting for the next poll tick. */
    k_work_reschedule(&led_work, K_NO_WAIT);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(ksn2_conn_status, ksn2_conn_status_event_listener);
ZMK_SUBSCRIPTION(ksn2_conn_status, zmk_ble_active_profile_changed);

#endif /* DT_NODE_EXISTS(DT_ALIAS(indicator_led)) */
