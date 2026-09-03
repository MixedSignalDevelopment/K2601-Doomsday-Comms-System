/* ===========================================================================
 *  SPDX-License-Identifier: MIT
 *  Copyright (c) 2026 Mixed Signal Development GmbH
 *
 *  Minimal SEGGER-RTT-compatible console for SWD-only bring-up.
 *
 *  The rev-A board is programmed/debugged through an ST-Link V2 over SWD with
 *  no USB data link, so there is no USB CDC serial port. RTT carries a
 *  bidirectional text console over the SWD link instead: the target keeps a
 *  ring-buffer control block in RAM (tagged "SEGGER RTT") that OpenOCD's
 *  `rtt` server reads and writes over the debug connection.
 *
 *  This is an independent, minimal implementation (not SEGGER's sources) that
 *  is wire-compatible with the control-block layout OpenOCD scans for. It is
 *  intentionally non-blocking: with no debugger attached, up-buffer writes are
 *  dropped so the firmware still runs standalone.
 *
 *  `Rtt` is an Arduino Stream, so existing Serial-style code (print/println/
 *  available/read) works with a one-word substitution.
 * =========================================================================== */
#ifndef _RTT_CONSOLE_H_
#define _RTT_CONSOLE_H_

#include <Arduino.h>
#include <Stream.h>

class RttSerial : public Stream {
public:
    void begin(unsigned long = 0);        /* signature compatibility with Serial */
    void end() {}
    operator bool() { return true; }      /* always "ready" — never blocks boot */

    /* Stream / Print interface */
    virtual int    available() override;
    virtual int    read() override;
    virtual int    peek() override;
    virtual void   flush() override {}
    virtual size_t write(uint8_t c) override;
    virtual size_t write(const uint8_t *buf, size_t size) override;
    using Print::write;

private:
    int _peeked = -1;                     /* one-char pushback for peek() */
};

extern RttSerial Rtt;

#endif /* _RTT_CONSOLE_H_ */
