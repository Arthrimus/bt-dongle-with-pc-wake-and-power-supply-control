#include "power_supervisor.h"

#include "board_config.h"
#include "debug_log.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "pico/time.h"
#include "usb_hid_wake.h"

#ifndef BUZZER_BEEP_MS
#define BUZZER_BEEP_MS 100u
#endif

#if HCI_BACKEND_cyw43 && ENABLE_CYW43_STATUS_LED
#include "pico/cyw43_arch.h"
#endif

static power_state_t state = POWER_STATE_UNKNOWN;
static bool wake_requested;
static bool debug_force_standby;
static bool pwr_ok_filtered;
static bool pwr_ok_last_raw;
static bool usb_vbus_filtered;
static bool usb_vbus_last_raw;
static const char *wake_reason = "none";
static absolute_time_t state_deadline;
static absolute_time_t cooldown_until;
static absolute_time_t pwr_ok_changed_at;
static absolute_time_t usb_vbus_changed_at;
static absolute_time_t standby_armed_at;
#if PIN_PS_ON_CONTROL >= 0
static bool ps_on_active;
static absolute_time_t ps_on_grace_deadline;
#endif
#if PIN_MOMENTARY_BUTTON >= 0
static bool momentary_button_last_raw;
static bool momentary_button_filtered;
static bool passthrough_active;
static absolute_time_t momentary_button_changed_at;
#if PIN_PS_ON_CONTROL >= 0
static bool force_shutdown_active;
static absolute_time_t long_press_timer;
#endif
#endif

// PS_ON GPIO helpers: assert LOW (output) to turn PSU on, release as input+pullup to cut power
typedef enum { ps_on_release_mode_high_z = 0 } ps_on_release_mode_t;

// Buzzer PWM state (static so init configures it once)
#if PIN_BUZZER >= 0
static uint buzzer_slice_index;
static uint buzzer_lane;  // will be 0 or 1
#endif

static void ps_on_assert(void) {
#if PIN_PS_ON_CONTROL >= 0
    gpio_set_dir(PIN_PS_ON_CONTROL, GPIO_OUT); // Ensure output before driving
    gpio_put(PIN_PS_ON_CONTROL, 0);            // LOW → PSU enabled
#endif
}

static void ps_on_release(void) {
#if PIN_PS_ON_CONTROL >= 0
    gpio_put(PIN_PS_ON_CONTROL, 0);
    gpio_set_dir(PIN_PS_ON_CONTROL, GPIO_IN);
    gpio_pull_up(PIN_PS_ON_CONTROL);      // High-Z with pullup → PSU off
#endif
}

// Power indicator LED helpers: driven HIGH when PC is on (same logic as PS_ON assertion)
static void power_led_on(void) {
#if PIN_POWER_INDICATOR_LED >= 0
    gpio_put(PIN_POWER_INDICATOR_LED, 1); // HIGH → LED on
#endif
}

static void power_led_off(void) {
#if PIN_POWER_INDICATOR_LED >= 0
    gpio_put(PIN_POWER_INDICATOR_LED, 0); // LOW → LED off
#endif
}

#define POWER_SENSE_DEBOUNCE_MS 250u
#ifndef STANDBY_WAKE_ARM_DELAY_MS
#define STANDBY_WAKE_ARM_DELAY_MS 5000u
#endif
#ifndef POWER_BUTTON_PULSE_MS
#define POWER_BUTTON_PULSE_MS 200u
#endif
#ifndef POST_WAKE_SENSE_SETTLE_MS
#define POST_WAKE_SENSE_SETTLE_MS 10000u
#endif
#ifndef STATUS_LED_BLINK_MS
#define STATUS_LED_BLINK_MS 1000u
#endif
#ifndef MOMENTARY_BUTTON_LONG_PRESS_MS
#define MOMENTARY_BUTTON_LONG_PRESS_MS 10000u
#endif

static bool pin_read_or_default(int pin, bool default_value) {
    if (pin < 0) return default_value;
    return gpio_get((uint)pin);
}

static bool debounce_bool(bool raw, bool *last_raw, bool *filtered, absolute_time_t *changed_at) {
    if (raw != *last_raw) {
        *last_raw = raw;
        *changed_at = make_timeout_time_ms(POWER_SENSE_DEBOUNCE_MS);
    }
    if (raw != *filtered && time_reached(*changed_at)) {
        *filtered = raw;
    }
    return *filtered;
}

static absolute_time_t standby_arm_deadline(void) {
    if (debug_force_standby || STANDBY_WAKE_ARM_DELAY_MS == 0u) return nil_time;
    return make_timeout_time_ms(STANDBY_WAKE_ARM_DELAY_MS);
}

static bool standby_wake_armed(void) {
    return is_nil_time(standby_armed_at) || time_reached(standby_armed_at);
}

static void status_led_put(bool on) {
#if PIN_LED_STATUS >= 0
    gpio_put(PIN_LED_STATUS, on);
#endif
#if HCI_BACKEND_cyw43 && ENABLE_CYW43_STATUS_LED && defined(CYW43_WL_GPIO_LED_PIN)
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
#else
    (void)on;
#endif
}

static void update_status_led(void) {
    bool on = false;
    if (state == POWER_STATE_STANDBY_HCI_HOST) {
        on = true;
    } else if (state == POWER_STATE_HOST_OFF && !standby_wake_armed()) {
        uint32_t blink_ms = STATUS_LED_BLINK_MS == 0u ? 1000u : STATUS_LED_BLINK_MS;
        on = ((to_ms_since_boot(get_absolute_time()) / blink_ms) & 1u) == 0u;
    }
    status_led_put(on);
}

// Forward declarations for button control helpers used in update_power_inputs()
static void power_button_press(void);
static void power_button_release(void);

static void update_power_inputs(void) {
    if (debug_force_standby) {
        pwr_ok_last_raw = pwr_ok_filtered = false;
        usb_vbus_last_raw = usb_vbus_filtered = false;
        return;
    }

    (void)debounce_bool(pin_read_or_default(PIN_PWR_OK_SENSE, true),
                        &pwr_ok_last_raw, &pwr_ok_filtered, &pwr_ok_changed_at);
    (void)debounce_bool(pin_read_or_default(PIN_USB_VBUS_SENSE, true),
                        &usb_vbus_last_raw, &usb_vbus_filtered, &usb_vbus_changed_at);
#if PIN_MOMENTARY_BUTTON >= 0
    {
        bool button_pressed = !pin_read_or_default(PIN_MOMENTARY_BUTTON, true);
        (void)debounce_bool(button_pressed,
                            &momentary_button_last_raw,
                            &momentary_button_filtered,
                            &momentary_button_changed_at);

        // Passthrough mode: when PC is ON (PWR_OK high), directly relay button
        // to PWR_BUTTON_OUT for soft/hard shutdown — bypasses all timing rules
        if (power_supervisor_pwr_ok()) {
            // Use RAW input for immediate response (no debounce in passthrough)
            if (button_pressed) {
                if (!passthrough_active) {
                    passthrough_active = true;
                    power_button_press();
                    debug_log("passthrough: button pressed, PWR_BUTTON_OUT driven low");
#if PIN_PS_ON_CONTROL >= 0
                    // Start long-press timer for force shutdown detection
                    if (is_nil_time(long_press_timer)) {
                        long_press_timer = make_timeout_time_ms(MOMENTARY_BUTTON_LONG_PRESS_MS);
                        debug_log("long-press timer started (%d ms)", MOMENTARY_BUTTON_LONG_PRESS_MS);
                    }
#endif
                }
            } else if (passthrough_active) {
                // Button released — release passthrough immediately
                passthrough_active = false;
                power_button_release();
                debug_log("passthrough: button released, PWR_BUTTON_OUT released");
#if PIN_PS_ON_CONTROL >= 0
                // Reset long-press timer on release
                if (!is_nil_time(long_press_timer)) {
                    long_press_timer = nil_time;
                    debug_log("long-press timer cancelled (button released)");
                }
#endif
            }
        } else {
            // PC is OFF — use DEBOUNCED button for wake (bypasses arm delay)
            if (momentary_button_filtered) {
                if ((state == POWER_STATE_STANDBY_HCI_HOST || state == POWER_STATE_HOST_OFF)
                    && time_reached(cooldown_until)) {
                    wake_reason = "button";
                    wake_requested = true;
                    debug_log("momentary button press: immediate wake");
                }
            }
            // Clear passthrough when PC goes off
            if (passthrough_active) {
                passthrough_active = false;
                power_button_release();
                debug_log("passthrough: cleared (PC powered off)");
            }
        }
#if PIN_PS_ON_CONTROL >= 0
        // Check if long-press threshold reached
        if (!is_nil_time(long_press_timer) && time_reached(long_press_timer)) {
            force_shutdown_active = true;
            long_press_timer = nil_time;
            debug_log("long-press detected: FORCING PS_ON HIGH (emergency shutdown)");
        }
#endif
    }
#endif
}

static void power_button_release(void) {
#if PIN_PWR_BUTTON_OUT >= 0
    gpio_put(PIN_PWR_BUTTON_OUT, 0);
    gpio_set_dir(PIN_PWR_BUTTON_OUT, GPIO_IN);
    gpio_pull_up(PIN_PWR_BUTTON_OUT);
#endif
}

static void power_button_press(void) {
#if PIN_PWR_BUTTON_OUT >= 0
    gpio_put(PIN_PWR_BUTTON_OUT, 0);
    gpio_set_dir(PIN_PWR_BUTTON_OUT, GPIO_OUT);
#endif
}
// ── Buzzer helpers ──────────────────────────────────────────────────────
static void buzzer_beep(void) {
#if PIN_BUZZER >= 0
    // Ensure pin is routed to PWM peripheral (may have been switched to SIO after previous beep)
    gpio_set_function(PIN_BUZZER, GPIO_FUNC_PWM);

    // Passive piezo needs PWM signal - use hardware PWM for reliable timing
    // Set level to ~50% duty cycle (level must be < wrap for proper toggling)
    pwm_set_chan_level(buzzer_slice_index, buzzer_lane, 15625); // 50% of ~31249 wrap
    // Enable the PWM output
    pwm_set_enabled(buzzer_slice_index, true);
    sleep_ms(BUZZER_BEEP_MS);

    // HARDEN: Ensure pin goes LOW after beep by forcing comparator to 0.
    // When level=0, the PWM output is always low (since counter >= 0).
    // The key insight: set level BEFORE disabling PWM, while pin is still
    // routed to the PWM peripheral (not SIO GPIO). gpio_put() won't work
    // on a pin multiplexed to GPIO_FUNC_PWM.
    pwm_set_chan_level(buzzer_slice_index, buzzer_lane, 0);
    // Wait long enough for one full PWM cycle so latch updates to LOW
    // At 4kHz that's ~250µs; use 500µs margin for safety.
    busy_wait_us(500);
    // Disable - pin already low, no race condition
    pwm_set_enabled(buzzer_slice_index, false);

    // Reassign pin to SIO GPIO as final safeguard (keeps it a stable LOW)
    gpio_set_function(PIN_BUZZER, GPIO_FUNC_SIO);
    gpio_put(PIN_BUZZER, 0);
#endif
}
void power_supervisor_init(void) {
#if PIN_PWR_OK_SENSE >= 0
    gpio_init(PIN_PWR_OK_SENSE);
    gpio_set_dir(PIN_PWR_OK_SENSE, GPIO_IN);
    gpio_pull_down(PIN_PWR_OK_SENSE);
#endif
#if PIN_USB_VBUS_SENSE >= 0
    gpio_init(PIN_USB_VBUS_SENSE);
    gpio_set_dir(PIN_USB_VBUS_SENSE, GPIO_IN);
    gpio_pull_down(PIN_USB_VBUS_SENSE);
#endif
#if PIN_PWR_BUTTON_OUT >= 0
    gpio_init(PIN_PWR_BUTTON_OUT);
    power_button_release();
#endif
#if PIN_PS_ON_CONTROL >= 0
    gpio_init(PIN_PS_ON_CONTROL);
    ps_on_release();  // Release as input with pullup (PSU off)
    ps_on_active = false;
    ps_on_grace_deadline = nil_time;
#endif
#if PIN_POWER_INDICATOR_LED >= 0
    gpio_init(PIN_POWER_INDICATOR_LED);
    gpio_set_dir(PIN_POWER_INDICATOR_LED, GPIO_OUT);
    gpio_put(PIN_POWER_INDICATOR_LED, 0);  // Start off (PC off)
#endif
#if PIN_MOMENTARY_BUTTON >= 0
    gpio_init(PIN_MOMENTARY_BUTTON);
    gpio_set_dir(PIN_MOMENTARY_BUTTON, GPIO_IN);
    gpio_pull_up(PIN_MOMENTARY_BUTTON); // Active-low button
    momentary_button_last_raw = momentary_button_filtered = false;
    passthrough_active = false;
#if PIN_PS_ON_CONTROL >= 0
    force_shutdown_active = false;
    long_press_timer = nil_time;
#endif
    momentary_button_changed_at = make_timeout_time_ms(0);
#endif
#if PIN_LED_STATUS >= 0
    gpio_init(PIN_LED_STATUS);
    gpio_set_dir(PIN_LED_STATUS, GPIO_OUT);
    gpio_put(PIN_LED_STATUS, 0);
#endif
#if PIN_LED_FAULT >= 0
    gpio_init(PIN_LED_FAULT);
    gpio_set_dir(PIN_LED_FAULT, GPIO_OUT);
    gpio_put(PIN_LED_FAULT, 0);
#endif
#if PIN_BUZZER >= 0
    gpio_init(PIN_BUZZER);
    // Connect pin to hardware PWM for passive piezo drive
    buzzer_slice_index = pwm_gpio_to_slice_num(PIN_BUZZER);
    buzzer_lane = pwm_gpio_to_channel(PIN_BUZZER);
    // 4kHz tone: wrap value sets frequency
    // freq = clk_sys / (DIV_INT + DIV_FRACT/256) / (WRAP+1)
    // With DIV=1: WRAP = 125000000/4000 - 1 = 31249
    // Using integer divider for reliable frequency
    uint wrap = (uint)(125000000.0f / 4000.0f) - 1; // ~31249 for 4kHz
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_wrap(&cfg, wrap);
    pwm_config_set_clkdiv_int(&cfg, 1); // Integer divider = 1 (no division)
    pwm_init(buzzer_slice_index, &cfg, false);
    gpio_set_function(PIN_BUZZER, GPIO_FUNC_PWM);
#endif
    pwr_ok_filtered = pwr_ok_last_raw = pin_read_or_default(PIN_PWR_OK_SENSE, true);
    usb_vbus_filtered = usb_vbus_last_raw = pin_read_or_default(PIN_USB_VBUS_SENSE, true);
    pwr_ok_changed_at = make_timeout_time_ms(0);
    usb_vbus_changed_at = make_timeout_time_ms(0);

    state = (pwr_ok_filtered && usb_vbus_filtered)
                ? POWER_STATE_HOST_ON_USB_HCI
                : POWER_STATE_HOST_OFF;
    standby_armed_at = state == POWER_STATE_HOST_OFF ? standby_arm_deadline() : nil_time;
}

bool power_supervisor_pwr_ok(void) {
    return pwr_ok_filtered;
}

bool power_supervisor_usb_vbus_present(void) {
    return usb_vbus_filtered;
}

power_state_t power_supervisor_get_state(void) {
    return state;
}

const char *power_supervisor_last_wake_reason(void) {
    return wake_reason;
}

void power_supervisor_debug_force_standby(bool force) {
    debug_force_standby = force;
    if (force) {
        pwr_ok_last_raw = pwr_ok_filtered = false;
        usb_vbus_last_raw = usb_vbus_filtered = false;
        standby_armed_at = nil_time;
        wake_requested = false;
        debug_log("debug force standby enabled");
    } else {
        pwr_ok_last_raw = pwr_ok_filtered = pin_read_or_default(PIN_PWR_OK_SENSE, true);
        usb_vbus_last_raw = usb_vbus_filtered = pin_read_or_default(PIN_USB_VBUS_SENSE, true);
        debug_log("debug force standby disabled");
    }
}

void power_supervisor_pulse_power_button_ms(uint32_t ms) {
#if PIN_PWR_BUTTON_OUT >= 0
    power_button_press();
    sleep_ms(ms);
    power_button_release();
#else
    (void)ms;
#endif
}

void power_supervisor_request_wake(const char *reason) {
    if (state != POWER_STATE_STANDBY_HCI_HOST && state != POWER_STATE_HOST_OFF) return;
    if (!time_reached(cooldown_until)) return;
    if (!standby_wake_armed()) return;
    wake_reason = reason ? reason : "wake policy";
    wake_requested = true;
}

static void set_state(power_state_t next) {
    if (state == next) return;
    debug_log("power state %d -> %d", state, next);
    state = next;
    if (state == POWER_STATE_HOST_OFF) {
        standby_armed_at = standby_arm_deadline();
        wake_requested = false;
        if (!is_nil_time(standby_armed_at)) {
            debug_log("standby wake arm delay: %lu ms", (unsigned long)STANDBY_WAKE_ARM_DELAY_MS);
        }
    } else if (state != POWER_STATE_STANDBY_HCI_HOST) {
        standby_armed_at = nil_time;
    }
}

void power_supervisor_task(void) {
    update_power_inputs();

    switch (state) {
    case POWER_STATE_HOST_OFF:
        if (power_supervisor_pwr_ok() && power_supervisor_usb_vbus_present()) {
            // PC is already powered on (external wake or boot) — turn LED on
#if PIN_POWER_INDICATOR_LED >= 0
            power_led_on();
#endif
            set_state(POWER_STATE_HOST_ON_USB_HCI);
        } else if (standby_wake_armed()) {
            set_state(POWER_STATE_STANDBY_HCI_HOST);
        }
        break;

    case POWER_STATE_STANDBY_HCI_HOST:
        if (power_supervisor_pwr_ok() && power_supervisor_usb_vbus_present()) {
            // PC booted externally — turn LED on before entering HCI host mode
#if PIN_POWER_INDICATOR_LED >= 0
            power_led_on();
#endif
            set_state(POWER_STATE_HOST_ON_USB_HCI);
        } else if (wake_requested) {
            wake_requested = false;
#if ENABLE_POWER_BUTTON_WAKE || ENABLE_STANDBY_HID_KEYBOARD
            set_state(POWER_STATE_WAKE_PULSE);
#else
            debug_log("wake ignored with wake outputs disabled: %s", wake_reason);
#endif
        }
        break;

    case POWER_STATE_WAKE_PULSE:
        usb_hid_wake_request_keypress();
#if ENABLE_POWER_BUTTON_WAKE
        if (!power_supervisor_pwr_ok()) {
            debug_log("wake pulse: %s", wake_reason);
            // Buzzer: signal that a power-on event is occurring while PC is off
            buzzer_beep();
            // Assert PS_ON LOW to turn on PSU BEFORE power button pulse
#if PIN_PS_ON_CONTROL >= 0
            ps_on_assert();
            ps_on_active = true;
            debug_log("PS_ON asserted LOW");
#endif
#if PIN_POWER_INDICATOR_LED >= 0
            power_led_on();  // External LED mirrors PC-on state
#endif
            power_supervisor_pulse_power_button_ms(POWER_BUTTON_PULSE_MS);
        } else {
            debug_log("wake pulse skipped: sense already high");
        }
#else
        debug_log("HID wake key requested: %s", wake_reason);
#endif
        cooldown_until = make_timeout_time_ms(POST_WAKE_SENSE_SETTLE_MS);
        state_deadline = make_timeout_time_ms(POST_WAKE_SENSE_SETTLE_MS);
        set_state(POWER_STATE_WAIT_WAKE_SENSE_SETTLE);
        break;

    case POWER_STATE_WAIT_WAKE_SENSE_SETTLE:
        if (power_supervisor_pwr_ok() && power_supervisor_usb_vbus_present()) {
            set_state(POWER_STATE_HOST_ON_USB_HCI);
        } else if (time_reached(state_deadline)) {
            state_deadline = make_timeout_time_ms(120000);
            set_state(POWER_STATE_WAIT_HOST_BOOT);
        }
        break;

    case POWER_STATE_WAIT_HOST_BOOT:
        if (power_supervisor_pwr_ok() && power_supervisor_usb_vbus_present()) {
            set_state(POWER_STATE_HOST_ON_USB_HCI);
        } else if (time_reached(state_deadline)) {
            set_state(POWER_STATE_HOST_OFF);
        }
        break;

    case POWER_STATE_HOST_ON_USB_HCI:
        if (!power_supervisor_pwr_ok()) {
            set_state(POWER_STATE_HOST_SHUTTING_DOWN);
            state_deadline = make_timeout_time_ms(1500);
            wake_requested = false;
        }
        break;

    case POWER_STATE_HOST_SHUTTING_DOWN:
        if (time_reached(state_deadline)) set_state(POWER_STATE_HOST_OFF);
        break;

    case POWER_STATE_FAULT:
#if PIN_LED_FAULT >= 0
        gpio_put(PIN_LED_FAULT, (to_ms_since_boot(get_absolute_time()) / 100u) & 1u);
#endif
        break;

    case POWER_STATE_UNKNOWN:
    default:
        set_state(POWER_STATE_HOST_OFF);
        break;
    }

    // PS_ON shutdown countdown monitoring - independent of state machine
    // NOTE: Only active when PC was ON and transitions to shutting down.
    // Don't run during WAIT states (PC still booting after wake pulse).
#if PIN_PS_ON_CONTROL >= 0
    if (ps_on_active && state == POWER_STATE_HOST_SHUTTING_DOWN) {
        if (!power_supervisor_pwr_ok()) {
            // PWR_OK went LOW - start or continue 5-second grace timer
            if (is_nil_time(ps_on_grace_deadline)) {
                ps_on_grace_deadline = make_timeout_time_ms(5000);
                debug_log("PS_ON shutdown countdown started (5s grace)");
            } else if (time_reached(ps_on_grace_deadline)) {
                ps_on_release();  // Release as input with pullup
#if PIN_POWER_INDICATOR_LED >= 0
                power_led_off();  // External LED mirrors PC-off state
#endif
                ps_on_active = false;
                ps_on_grace_deadline = nil_time;
                debug_log("PS_ON released HIGH (grace period expired)");
            }
        } else {
            // PWR_OK is HIGH - cancel timer if running, keep PS_ON LOW
            if (!is_nil_time(ps_on_grace_deadline)) {
                ps_on_grace_deadline = nil_time;
                debug_log("PS_ON shutdown countdown cancelled (PWR_OK went HIGH)");
            }
        }
    }
#endif

    // Force shutdown enforcement - INDEPENDENT of normal PS_ON logic
#if PIN_PS_ON_CONTROL >= 0 && PIN_MOMENTARY_BUTTON >= 0
    if (force_shutdown_active) {
        // Force PS_ON HIGH - emergency power-off
        gpio_put(PIN_PS_ON_CONTROL, 1);
#if PIN_POWER_INDICATOR_LED >= 0
        power_led_off();  // External LED mirrors PC-off state
#endif
        
        // Recovery path 1: button press immediately releases force shutdown
        if (momentary_button_filtered) {
            force_shutdown_active = false;
            debug_log("force shutdown released by button press");
        }
        // Recovery path 2: wake event when PC is OFF allows startup
        else if (wake_requested && !power_supervisor_pwr_ok()) {
            force_shutdown_active = false;
            debug_log("force shutdown released by wake event: %s", wake_reason);
        }
    }
#endif

    update_status_led();
}
