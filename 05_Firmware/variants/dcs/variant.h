/* ===========================================================================
 *  SPDX-License-Identifier: MIT
 *  Copyright (c) 2026 Mixed Signal Development GmbH
 *
 *  Doomsday Comms System (D.C.S., SKU OS2601) rev-A — Arduino board variant
 *
 *  Authoritative nRF52840 pin map. This is the single source of truth the
 *  firmware includes; no pin is ever hard-coded elsewhere.
 *
 *  nRF absolute pin number = 32 * port + pin.  e.g. P0.02 = 2, P1.10 = 42.
 *  The Arduino index (what pinMode/digitalWrite take) is the position of the
 *  pin inside g_ADigitalPinMap[] in variant.cpp. The PIN_* macros below ARE
 *  those indices — keep them in lock-step with the array order.
 * =========================================================================== */
#ifndef _VARIANT_DCS_H_
#define _VARIANT_DCS_H_

#include "WVariant.h"

/* -------- clock ---------------------------------------------------------- */
#define VARIANT_MCK       (64000000ul)

/* -------- pin counts (must match g_ADigitalPinMap[] length) -------------- */
#define PINS_COUNT        (28u)
#define NUM_DIGITAL_PINS  (28u)
#define NUM_ANALOG_INPUTS (1u)     /* VBAT_MON on AIN0 */
#define NUM_ANALOG_OUTPUTS (0u)

/* ===========================================================================
 *  Signal -> Arduino index map  (index == position in g_ADigitalPinMap[])
 *  The trailing comment is the physical nRF pin for cross-checking the PCB.
 * =========================================================================== */
#define PIN_VBAT_MON      (0)    /* P0.02 / AIN0  1:1 divider, always-on      */
#define PIN_LORA_SCK      (1)    /* P0.03  SPI SCK to Ra-01SH                 */
#define PIN_FULL          (2)    /* P0.04  MCP73833 STAT2, open-drain, act.lo */
#define PIN_DISPLAY_PWM   (3)    /* P0.05  LCD backlight, active high, PWM ok  */
#define PIN_SDA           (4)    /* P0.06  I2C SDA (DRV2605L)                  */
#define PIN_PUSH          (5)    /* P0.07  wheel press, active low, pullup     */
#define PIN_SCL           (6)    /* P0.08  I2C SCL                            */
#define PIN_WHEEL_R       (7)    /* P0.12  wheel roll R, active low, pullup    */
#define PIN_WHEEL_L       (8)    /* P0.13  wheel roll L, active low, pullup    */
#define PIN_LRA_EN        (9)    /* P0.15  DRV2605L EN, drive low before sleep */
#define PIN_LORA_RST_N    (10)   /* P0.17  SX1262 RESET                        */
#define PIN_DISPLAY_CS_N  (11)   /* P0.20  LCD chip select                     */
#define PIN_BUZ_MINUS     (12)   /* P0.22  piezo terminal B (antiphase)        */
#define PIN_BUZ_PLUS      (13)   /* P0.24  piezo terminal A (single-ended too) */
#define PIN_LORA_MOSI     (14)   /* P0.26  SPI MOSI                            */
/* PIN index 15 = TXEN P0.28 — PRESENT IN MAP ONLY SO NOTHING ELSE REUSES IT.
 * It is the SX1262's own DIO2 through 0R. NEVER configure it as an output.   */
#define PIN_TXEN_DO_NOT_DRIVE (15) /* P0.28  <-- do not pinMode/write, ever    */
#define PIN_DIO1          (16)   /* P0.29  SX1262 IRQ                          */
#define PIN_CHARGING      (17)   /* P0.30  LED net from VBUS; do not rely on it */
/* PIN index 18 = SWO P1.00 — reserved for trace, do not allocate.            */
#define PIN_SWO_RESERVED  (18)   /* P1.00                                      */
#define PIN_DISPLAY_SCK   (19)   /* P1.02  LCD SPI clock                       */
#define PIN_DISPLAY_RS    (20)   /* P1.04  LCD A0/DC                           */
#define PIN_DISPLAY_RES   (21)   /* P1.06  LCD reset                           */
#define PIN_DISPLAY_MOSI  (22)   /* P1.09  LCD SPI data (write-only)           */
#define PIN_BUSY          (23)   /* P1.10  SX1262 BUSY (mandatory flow ctrl)   */
#define PIN_LORA_MISO     (24)   /* P1.11  SPI MISO                            */
#define PIN_LORA_CS_N     (25)   /* P1.13  SPI chip select                     */

/* -------- an LED_BUILTIN alias is expected by some core code -------------- */
/* No dedicated user LED exists on rev A. Point it at the backlight so any    */
/* stray reference is harmless; firmware drives the backlight explicitly.     */
#define LED_BUILTIN       PIN_DISPLAY_PWM
#define LED_STATE_ON      1

/* -------- battery ADC ----------------------------------------------------- */
#define PIN_VBAT          PIN_VBAT_MON
#define VBAT_DIVIDER      (2.0f)   /* 1M/1M -> multiply pin voltage by 2       */

/* ===========================================================================
 *  Wire (I2C) — DRV2605L @ 0x5A
 * =========================================================================== */
#define WIRE_INTERFACES_COUNT (1)
#define PIN_WIRE_SDA      PIN_SDA
#define PIN_WIRE_SCL      PIN_SCL

/* ===========================================================================
 *  SPI — the DEFAULT `SPI` object is the LoRa bus (SX1262).
 *  The display uses its own separate pins via u8g2 software SPI, so there is
 *  no bus contention during bring-up.
 * =========================================================================== */
#define SPI_INTERFACES_COUNT (1)
#define PIN_SPI_MISO      PIN_LORA_MISO
#define PIN_SPI_MOSI      PIN_LORA_MOSI
#define PIN_SPI_SCK       PIN_LORA_SCK
#define PIN_SPI_SS        PIN_LORA_CS_N

/* ===========================================================================
 *  Piezo — stock Meshtastic RTTTL buzzer uses single-ended drive on BUZ+.
 * =========================================================================== */
#define PIN_BUZZER        PIN_BUZ_PLUS

/* ===========================================================================
 *  Button — the wheel press is the wake / power / menu button.
 * =========================================================================== */
#define PIN_BUTTON1       PIN_PUSH

/* ===========================================================================
 *  Serial1 (HW UART) — NOT USED on rev A. The Adafruit core unconditionally
 *  instantiates Serial1, so these must exist. They point at unused NFC pins
 *  (P0.09/P0.10) as inert placeholders; firmware never calls Serial1.begin().
 * =========================================================================== */
#define PIN_SERIAL1_RX    (26)   /* P0.09  (NFC1, unused) placeholder */
#define PIN_SERIAL1_TX    (27)   /* P0.10  (NFC2, unused) placeholder */

#ifdef __cplusplus
extern "C" {
#endif
extern const uint32_t g_ADigitalPinMap[PINS_COUNT];
#ifdef __cplusplus
}
#endif

#endif /* _VARIANT_DCS_H_ */
