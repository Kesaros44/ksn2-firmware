/*
 * KSN-2 central BLE connection-status LED
 *
 * - Connected to host:      LED steady on
 * - Not connected:          LED blinks, toggling every 500 ms
 *
 * Drives the `indicator-led` alias (ble_led, D20/P0.29 on the left/central
 * half). This LED used to be driven by zmk-poor-mans-led-indicator
 * (CONFIG_INDICATOR_LED_SHOW_BLE), but that widget only re-evaluates on a
 * zmk_ble_active_profile_changed event (i.e. when the user switches BT
 * profile) and each blink sequence ends in a fixed state rather than
 * tracking live connection status - so once that one event passed, the LED
 * just stayed in whatever state the sequence ended on, regardless of
 * whether the host was actually still connected. That widget's LED
 * indication is disabled in ksn_2_left.conf (CONFIG_INDICATOR_LED_WIDGET=n)
 * so it no longer drives this pin.
 *
 * Ported from nexplit-keypad's config/src/keypad_indicator.c, which solves
 * the exact same class of problem there.
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
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>

LOG_MODULE_REGISTER(ksn2_conn_status, CONFIG_ZMK_LOG_LEVEL);

#define LED_NODE DT_ALIAS(indicator_led)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED_NODE, gpios);

/* Blink interval while not connected. */
#define BLINK_INTERVAL_DISCONNECTED_MS 500
/* Re-check interval while connected (LED just stays on; this only guards
 * against missing a disconnect event). */
#define RECHECK_INTERVAL_CONNECTED_MS 1000

static bool led_is_on = false;
static struct k_work_delayable led_work;

static void set_led(bool on) {
    led_is_on = on;
    gpio_pin_set_dt(&led, on ? 1 : 0);
}

static void led_work_handler(struct k_work *work) {
    bool connected = zmk_ble_active_profile_is_connected();

    if (connected) {
        if (!led_is_on) {
            set_led(true);
        }
        k_work_schedule(&led_work, K_MSEC(RECHECK_INTERVAL_CONNECTED_MS));
    } else {
        set_led(!led_is_on);
        k_work_schedule(&led_work, K_MSEC(BLINK_INTERVAL_DISCONNECTED_MS));
    }
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
