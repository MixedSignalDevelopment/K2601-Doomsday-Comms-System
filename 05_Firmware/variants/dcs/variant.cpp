/* ===========================================================================
 *  SPDX-License-Identifier: MIT
 *  Copyright (c) 2026 Mixed Signal Development GmbH
 *
 *  Doomsday Comms System (D.C.S., SKU OS2601) rev-A — Arduino board variant
 *  implementation
 *
 *  g_ADigitalPinMap[] maps Arduino index -> nRF absolute pin (32*port + pin).
 *  The order here MUST match the PIN_* indices in variant.h.
 *
 *  Also instantiates the default Wire (I2C) and SPI (LoRa) bus objects on the
 *  correct nRF pins, as the Adafruit nRF52 core expects the variant to do.
 * =========================================================================== */
#include "variant.h"
#include "wiring_constants.h"
#include "wiring_digital.h"
#include "nrf.h"

const uint32_t g_ADigitalPinMap[PINS_COUNT] =
{
     2,   /* [0]  PIN_VBAT_MON     P0.02 / AIN0 */
     3,   /* [1]  PIN_LORA_SCK     P0.03 */
     4,   /* [2]  PIN_FULL         P0.04 */
     5,   /* [3]  PIN_DISPLAY_PWM  P0.05 */
     6,   /* [4]  PIN_SDA          P0.06 */
     7,   /* [5]  PIN_PUSH         P0.07 */
     8,   /* [6]  PIN_SCL          P0.08 */
    12,   /* [7]  PIN_WHEEL_R      P0.12 */
    13,   /* [8]  PIN_WHEEL_L      P0.13 */
    15,   /* [9]  PIN_LRA_EN       P0.15 */
    17,   /* [10] PIN_LORA_RST_N   P0.17 */
    20,   /* [11] PIN_DISPLAY_CS_N P0.20 */
    22,   /* [12] PIN_BUZ_MINUS    P0.22 */
    24,   /* [13] PIN_BUZ_PLUS     P0.24 */
    26,   /* [14] PIN_LORA_MOSI    P0.26 */
    28,   /* [15] PIN_TXEN         P0.28  <-- NEVER drive as output */
    29,   /* [16] PIN_DIO1         P0.29 */
    30,   /* [17] PIN_CHARGING     P0.30 */
    32,   /* [18] PIN_SWO          P1.00  reserved */
    34,   /* [19] PIN_DISPLAY_SCK  P1.02 */
    36,   /* [20] PIN_DISPLAY_RS   P1.04 */
    38,   /* [21] PIN_DISPLAY_RES  P1.06 */
    41,   /* [22] PIN_DISPLAY_MOSI P1.09 */
    42,   /* [23] PIN_BUSY         P1.10 */
    43,   /* [24] PIN_LORA_MISO    P1.11 */
    45,   /* [25] PIN_LORA_CS_N    P1.13 */
     9,   /* [26] PIN_SERIAL1_RX   P0.09  NFC1, unused placeholder */
    10,   /* [27] PIN_SERIAL1_TX   P0.10  NFC2, unused placeholder */
};

/* NOTE: the default Wire (I2C) and SPI (LoRa) bus objects are NOT defined here.
 * PlatformIO's bundled Wire/SPI libraries already instantiate them using this
 * variant's PIN_WIRE_SDA/SCL and PIN_SPI_* macros, so defining them again
 * would be a duplicate-symbol link error. The pin map below is all we own. */

void initVariant(void)
{
    /* Nothing forced high at boot. The backlight FET has a hardware pulldown
     * so the screen powers up dark until firmware asks for it. We deliberately
     * leave TXEN (P0.28) completely untouched — it is a module output. */
}
