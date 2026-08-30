/*
 * rpm_feedback.c
 *
 * Simple Hall-sensor RPM measurement for an unmodified T41U5XBB.
 *
 * Hardware:
 *   T41U5XBB ST3 -> Teensy 4.1 pin 35
 *   3144E Hall sensor
 *   One magnet on spindle
 *
 * Purpose:
 *   - RPM display
 *   - spindle-stall detection
 *   - commanded-versus-actual RPM comparison
 *
 * Not suitable for:
 *   - rigid tapping
 *   - synchronized threading
 *   - precise spindle angular position
 Add this to src\grbl\plugins_init.h
  #if RPM_FEEDBACK_ENABLE
    extern void rpm_feedback_init (void);
    rpm_feedback_init();
  #endif
 */

#

#include "grbl/stream.h"
#include "grbl/hal.h"
#include "grbl/ioports.h"
#include "grbl/protocol.h"
#include "driver.h"
#include <Arduino.h>

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#if RPM_FEEDBACK_ENABLE == 1

#define RPM_PHYSICAL_PIN 35u
#define RPM_PULSES_PER_REV 1u
#define RPM_MIN_PERIOD_US 500u
#define RPM_STOP_TIMEOUT_US 500000u

static uint8_t rpm_port = IOPORT_UNASSIGNED;

static volatile uint32_t rpm_pulse_count = 0;
static volatile uint32_t rpm_last_pulse_us = 0;
static volatile uint32_t rpm_period_us = 0;

static on_report_options_ptr on_report_options;
static on_realtime_report_ptr on_realtime_report;

typedef struct
{
    float programmed_rpm;
    float target_rpm;
    float actual_rpm;
    float error_rpm;
    float error_percent;
    bool spindle_on;
} rpm_status_t;

static bool find_st3(
    xbar_t *properties,
    uint8_t port,
    void *data)
{
    uint8_t *found_port = (uint8_t *)data;

    /*
     * On the Teensy driver, properties->pin is the physical
     * Teensy pin number.
     */
    if (properties->pin == RPM_PHYSICAL_PIN)
    {
        *found_port = port;
        return true;
    }

    return false;
}

static void on_rpm_pulse(
    uint8_t port,
    bool state)
{
    if (port != rpm_port)
        return;

    uint32_t now = micros();

    if (rpm_last_pulse_us == 0u)
    {

        rpm_last_pulse_us = now;
        rpm_pulse_count++;
    }
    else
    {

        uint32_t period = now - rpm_last_pulse_us;

        if (period >= RPM_MIN_PERIOD_US)
        {
            rpm_period_us = period;
            rpm_last_pulse_us = now;
            rpm_pulse_count++;
        }
    }
}

bool port_init(void)
{
    xbar_t *pin;

    rpm_port = IOPORT_UNASSIGNED;

    /*
     * Find an unclaimed digital input corresponding to
     * physical Teensy pin 35.
     */
    if (!ioports_enumerate(
            Port_Digital,
            Port_Input,
            (pin_cap_t){
                .irq_mode = IRQ_Mode_Falling,
                .claimable = On},
            find_st3,
            &rpm_port))
    {
        return false;
    }

    if (rpm_port == IOPORT_UNASSIGNED)
        return false;

    /*
     * Claim the port for exclusive plugin use.
     *
     * ioport_claim() may update rpm_port to the claimed/user
     * port number that must be used afterward.
     */
    pin = ioport_claim(
        Port_Digital,
        Port_Input,
        &rpm_port,
        "Spindle RPM");

    if (pin == NULL)
        return false;

    rpm_pulse_count = 0u;
    rpm_last_pulse_us = 0u;
    rpm_period_us = 0u;

    /*
     * Ask the grblHAL driver to configure and own the IRQ.
     */
    if (!ioport_enable_irq(
            rpm_port,
            IRQ_Mode_Falling,
            on_rpm_pulse))
    {
        rpm_port = IOPORT_UNASSIGNED;
        return false;
    }

    return true;
}

// float rpm_load_percent(float requested_rpm, float actual_rpm)
// {
//     if (requested_rpm <= 0.0f)
//         return 0.0f;

//     float load = 100.0f * (requested_rpm - actual_rpm) / requested_rpm;

//     if (load < 0.0f)
//         load = 0.0f;
//     else if (load > 100.0f)
//         load = 100.0f;

//     return load;
// }

float rpm_feedback_get_rpm(void)
{
    uint32_t last_pulse;
    uint32_t period;

    noInterrupts();

    last_pulse = rpm_last_pulse_us;
    period = rpm_period_us;

    interrupts();

    if (last_pulse == 0u || period == 0u)
        return 0.0f;

    if ((uint32_t)(micros() - last_pulse) > RPM_STOP_TIMEOUT_US)
    {
        return 0.0f;
    }

    return 60000000.0f /
           ((float)period *
            (float)RPM_PULSES_PER_REV);
}

static rpm_status_t rpm_get_status(void)
{
    rpm_status_t result = {0};

    spindle_ptrs_t *spindle = spindle_get(0);

    result.actual_rpm = rpm_feedback_get_rpm();

    if (spindle == NULL || spindle->param == NULL)
        return result;

    result.programmed_rpm = spindle->param->rpm;
    result.target_rpm = spindle->param->rpm_overridden;
    result.spindle_on = spindle->param->state.on;

    if (result.spindle_on && result.target_rpm > 0.0f)
    {
        result.error_rpm =
            result.target_rpm - result.actual_rpm;

        result.error_percent =
            100.0f * result.error_rpm /
            result.target_rpm;
    }

    return result;
}

static void onRealtimeReport(stream_write_ptr stream_write, report_tracking_flags_t report)
{
    if (on_realtime_report)
        on_realtime_report(stream_write, report);

    // Report realtime speed
    if (settings.status_report.feed_speed)
    {

        rpm_status_t rpm = rpm_get_status();

        char buffer[96];
        snprintf(
            buffer,
            sizeof(buffer),
            "|SR:%.0f|AR:%.0f",
            rpm.target_rpm,
            rpm.actual_rpm);
        stream_write(buffer);
    }
}

static void onReportOptions(bool newopt)
{
    on_report_options(newopt);

    if (!newopt)
    {
        report_plugin("rpm_feedback plugin", "0.1");
    }
}

// Plugin initialization
void rpm_feedback_init(void)
{
    on_report_options = grbl.on_report_options;
    grbl.on_report_options = onReportOptions;

    on_realtime_report = grbl.on_realtime_report;
    grbl.on_realtime_report = onRealtimeReport;

    port_init();
}

#endif