/* ===========================================================================
 *  SPDX-License-Identifier: MIT
 *  Copyright (c) 2026 Mixed Signal Development GmbH
 *
 *  RTT console — Arduino Stream wrapper over the SEGGER RTT implementation
 *  that ships inside the Adafruit nRF52 core (cores/nRF5/sysview/SEGGER).
 *
 *  We deliberately reuse the core's RTT control block rather than defining our
 *  own: there can be only one "_SEGGER_RTT" block for OpenOCD to attach to.
 *  The core's up buffer 0 ("Terminal") is non-blocking (skip-if-full), so the
 *  firmware runs fine with no debugger attached.
 * =========================================================================== */
#include "rtt.h"
#include "SEGGER_RTT.h"

void RttSerial::begin(unsigned long)
{
    SEGGER_RTT_Init();
}

int RttSerial::available()
{
    if (_peeked >= 0) return 1;
    return SEGGER_RTT_HasKey();          /* 1 if a byte is waiting, else 0 */
}

int RttSerial::read()
{
    if (_peeked >= 0) { int c = _peeked; _peeked = -1; return c; }
    return SEGGER_RTT_GetKey();          /* -1 if none */
}

int RttSerial::peek()
{
    if (_peeked < 0) _peeked = SEGGER_RTT_GetKey();
    return _peeked;
}

size_t RttSerial::write(uint8_t c)
{
    return SEGGER_RTT_Write(0, &c, 1);
}

size_t RttSerial::write(const uint8_t *buf, size_t size)
{
    return SEGGER_RTT_Write(0, buf, size);
}

RttSerial Rtt;
