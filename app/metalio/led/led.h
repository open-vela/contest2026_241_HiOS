/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * LED abstraction — ported from MetalioClaw4 main/led/led.h.
 *
 * The Led base class is the interface every LED driver (SingleLed,
 * GpioLed, CircularStrip, NoLed) implements.  OnStateChanged() is invoked
 * by the application whenever the device state changes; the concrete
 * driver translates that into the appropriate blink / color / breathe
 * pattern.
 */

#ifndef _LED_H_
#define _LED_H_

class Led
{
public:
    virtual ~Led() = default;
    /* Set the led state based on the device state. */
    virtual void OnStateChanged() = 0;
};

class NoLed : public Led
{
public:
    virtual void OnStateChanged() override {}
};

#endif /* _LED_H_ */
