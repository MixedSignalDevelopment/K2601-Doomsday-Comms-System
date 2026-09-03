/* ===========================================================================
 *  SPDX-License-Identifier: MIT
 *  Copyright (c) 2026 Mixed Signal Development GmbH
 *
 *  Doomsday Comms System (D.C.S., SKU OS2601) — plain point-to-point messenger
 *
 *  Everyday preset messages, no animations. Now with:
 *    - Chat history (persisted to internal flash, survives power-off)
 *    - Delivery acknowledgements (receiver auto-ACKs; sender shows a
 *      single tick on send, double tick once the ACK is heard)
 *    - Persistent settings (backlight/contrast/sleep/haptic/beep)
 *
 *  Controls (rev-A wheel, post-bodge mapping):
 *     roll  = scroll / adjust      press = send / select      hold = menu
 * =========================================================================== */
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <U8g2lib.h>
#include <Adafruit_DRV2605.h>
#include <RadioLib.h>
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
#include <HardwarePWM.h>
#include "nrf_gpio.h"
#include "rtt.h"

using namespace Adafruit_LittleFS_Namespace;

#define DISP_ARGS U8G2_R0, PIN_DISPLAY_SCK, PIN_DISPLAY_MOSI, \
                  PIN_DISPLAY_CS_N, PIN_DISPLAY_RS, PIN_DISPLAY_RES
U8G2_ST7565_ERC12864_F_4W_SW_SPI display(DISP_ARGS);

Adafruit_DRV2605 drv;
static bool g_drvReady = false;

SX1262 radio = new Module(PIN_LORA_CS_N, PIN_DIO1, PIN_LORA_RST_N, PIN_BUSY);
static bool  g_radioReady = false;
static float g_lastRssi   = 0;

/* ---- settings (persisted) ------------------------------------------------ */
static uint8_t  g_backlight = 100;
static uint8_t  g_contrast  = 10;
static uint8_t  g_hapticLvl = 2;     /* 0 Off,1 Soft,2 Med,3 Strong */
static uint8_t  g_beepLvl   = 1;     /* 0 Off,1 On,2 Loud */
static uint8_t  g_sleepIdx  = 2;

static const uint16_t    kSleepSecs[] = { 0, 15, 30, 60, 120 };
static const char *const kSleepLbl[]  = { "Off", "15s", "30s", "1m", "2m" };
static const char *const kHapLbl[]    = { "Off", "Soft", "Med", "Strong" };
static const char *const kBeepLbl[]   = { "Off", "On", "Loud" };

static uint32_t g_lastActive = 0;
static bool     g_asleep     = false;
static int      g_battMv     = 0;

/* ---- everyday preset messages ------------------------------------------- */
static const char *const kPresets[] = {
    "On my way",
    "Running late",
    "5 minutes",
    "Almost there",
    "Where are you?",
    "At home",
    "Call me",
    "Yes",
    "No",
    "OK",
    "Lunch?",
    "Need anything?",
    "IOU",
    "Dinner?",
    "Coffee?",
    "Miss you",
    "Thanks!",
};
static const int kNumPresets = sizeof(kPresets) / sizeof(kPresets[0]);

/* ---- wire protocol -------------------------------------------------------
 *   data:  [MSG_MAGIC][PKT_DATA][seq][text...]
 *   ack:   [MSG_MAGIC][PKT_ACK ][seq]
 * MSG_MAGIC differs from the love-messenger (0xC5) so the two pairs never
 * cross-trigger. */
#define MSG_MAGIC    0xC6
#define PKT_DATA     0x00
#define PKT_ACK      0x01

/* ---- radio profile: MAX RANGE -------------------------------------------
 * BOTH boards MUST run this identical build or they can't hear each other at
 * all (a board on SF12 cannot demodulate one on SF9). Flash BOTH before they
 * separate — never change SF/BW/CR/preamble on one alone.
 *
 * SF12 / BW125 / CR4-8 / 22 dBm — the practical maximum range for THIS
 * hardware. Vs the old SF9/14 dBm this is ~+8 dB from SF and +8 dB from power
 * (~+16 dB total, roughly 3-4x range in the open). BW stays 125 kHz on
 * purpose: the modules have no TCXO (plain crystal), and below ~125 kHz the
 * crystal's frequency drift exceeds the receiver's tolerance and the link
 * drops intermittently. 22 dBm is the SX1262 max; note EU868 caps ERP at
 * 14 dBm on 868.0 (fine for personal/field use, your call). */
#define LORA_FREQ    868.0
#define LORA_BW      125.0
#define LORA_SF      12
#define LORA_CR      8
#define LORA_SYNC    0x2B
#define LORA_TX_DBM  22
/* Preamble ~= 1 s of airtime at SF12 (symbol ~32.8 ms) so the duty-cycle RX
 * wake window still catches an incoming message. */
#define PREAMBLE_SYM 32
#define ACK_TIMEOUT  8000            /* ms; round-trip airtime is larger at SF12 */

/* ---- input: press=P0.12, roll R=P0.07, roll L=P0.13 --------------------- */
#define PIN_PRESS   PIN_WHEEL_R
#define PIN_ROLL_R  PIN_PUSH
#define PIN_ROLL_L  PIN_WHEEL_L

enum WheelEv { WH_NONE, WH_DOWN, WH_UP };
enum BtnEv   { BTN_NONE, BTN_SHORT, BTN_LONG };

static void pollInputs(WheelEv &w, BtnEv &b)
{
    w = WH_NONE; b = BTN_NONE;
    const uint32_t DEB = 12, LONG_MS = 600;
    uint32_t now = millis();
    static int lastR = HIGH, lastL = HIGH; static uint32_t tR = 0, tL = 0;
    int r = digitalRead(PIN_ROLL_R), l = digitalRead(PIN_ROLL_L);
    if (r != lastR && now - tR > DEB) { tR = now; lastR = r; if (r == LOW) w = WH_DOWN; }
    if (l != lastL && now - tL > DEB) { tL = now; lastL = l; if (l == LOW) w = WH_UP;   }
    static int lastP = HIGH; static uint32_t tP = 0, downAt = 0; static bool longFired = false;
    int p = digitalRead(PIN_PRESS);
    if (p != lastP && now - tP > DEB) {
        tP = now; lastP = p;
        if (p == LOW) { downAt = now; longFired = false; }
        else if (!longFired) b = BTN_SHORT;
    }
    if (p == LOW && !longFired && now - downAt >= LONG_MS) { longFired = true; b = BTN_LONG; }
}
static bool pressDown() { return digitalRead(PIN_PRESS) == LOW; }

/* ---- haptics ------------------------------------------------------------- */
static void drvConfig() { if (g_drvReady) { drv.useLRA(); drv.selectLibrary(6); drv.setMode(DRV2605_MODE_INTTRIG); } }
static void haptic(uint8_t fx)
{
    if (!g_drvReady || g_hapticLvl == 0) return;
    drv.setWaveform(0, fx); drv.setWaveform(1, 0); drv.go();
}
static void buzz() { static const uint8_t fx[] = { 0, 1, 16, 47 }; haptic(fx[g_hapticLvl]); }

/* ---- piezo --------------------------------------------------------------
 * Both beep modes go through the core's HardwarePWM ownership manager on PWM2
 * (the same peripheral tone() uses). Loud = complementary antiphase on
 * BUZ+/BUZ- for +6 dB; normal = single-ended on BUZ+ with BUZ- held low.
 *
 * Why the manager and not raw registers: takeOwnership() refuses PWM2 while
 * ANY of its PSEL channels is still connected. The old loud path poked PWM2
 * raw and left its pins connected, so afterwards nothing could re-acquire the
 * peripheral and every later beep went silent until a power cycle. Going
 * through addPin/removeAllPins/releaseOwnership guarantees the peripheral and
 * its pins are handed back clean after every beep. */
#define BEEP_TOKEN 0x5A454542UL          /* 'BEEZ' — our PWM2 ownership token */
static void hwBeep(uint16_t freq, uint16_t ms, bool loud)
{
    if (freq == 0 || ms == 0) return;
    HardwarePWM *pwm = HwPWMx[2];
    if (!pwm->isOwner(BEEP_TOKEN) && !pwm->takeOwnership(BEEP_TOKEN)) return;  /* busy */

    uint16_t top = (uint16_t)(16000000UL / freq);      /* DIV_1 = 16 MHz clock */
    pwm->setClockDiv(PWM_PRESCALER_PRESCALER_DIV_1);
    pwm->setMaxValue(top);
    pwm->addPin(PIN_BUZ_PLUS);
    if (loud) pwm->addPin(PIN_BUZ_MINUS);
    else { pinMode(PIN_BUZ_MINUS, OUTPUT); digitalWrite(PIN_BUZ_MINUS, LOW); }
    pwm->writePin(PIN_BUZ_PLUS, top / 2, false);
    if (loud) pwm->writePin(PIN_BUZ_MINUS, top / 2, true);   /* antiphase pair */

    delay(ms);

    pwm->removeAllPins();                 /* disconnect PSEL channels... */
    pwm->stop();                          /* ...disable the peripheral... */
    pwm->releaseOwnership(BEEP_TOKEN);     /* ...and hand it back clean */
    pinMode(PIN_BUZ_PLUS, OUTPUT);  digitalWrite(PIN_BUZ_PLUS, LOW);
    pinMode(PIN_BUZ_MINUS, OUTPUT); digitalWrite(PIN_BUZ_MINUS, LOW);
}
static void beep(uint16_t f, uint16_t ms)
{
    if (g_beepLvl == 0) return;
    hwBeep(f, ms, g_beepLvl == 2);
}
static void chimeIncoming() { buzz(); beep(1760, 90); beep(1319, 130); buzz(); }
static void chimeSent()     { beep(2093, 60); beep(2637, 80); }

/* ---- feedback ------------------------------------------------------------ */
static void backlight(uint8_t d) { analogWrite(PIN_DISPLAY_PWM, d); }

/* ---- text helpers -------------------------------------------------------- */
static void textCentered(const char *s, int y, const uint8_t *font)
{
    display.setFont(font);
    display.drawStr((128 - display.getStrWidth(s)) / 2, y, s);
}
static void drawTick(int x, int y)
{
    display.drawLine(x, y + 2, x + 2, y + 4);
    display.drawLine(x + 2, y + 4, x + 6, y);
}
static void drawTicks(int x, int y, bool dbl)   /* single = sent, double = delivered */
{
    drawTick(x, y); if (dbl) drawTick(x + 4, y);
}

/* ---- chat history (ring buffer, persisted) ------------------------------ */
#define HIST_MAX 20
struct Msg {
    uint8_t dir;         /* 0 = received, 1 = sent */
    uint8_t delivered;   /* sent only: 1 once ACK heard */
    uint8_t seq;         /* sequence id (matches ack) */
    char    text[42];
};
static Msg g_hist[HIST_MAX];
static uint8_t g_histCount = 0;   /* number stored (<= HIST_MAX) */
static uint8_t g_histHead  = 0;   /* next write slot */
static uint8_t g_txSeq     = 0;
static uint16_t g_unread   = 0;

/* ---- persistence (LittleFS on internal flash) --------------------------- */
#define PERSIST_FILE "/msgr.bin"
#define P_MAGIC 0x4B323601UL      /* persistence format magic + ver tag */
#define P_VER   1
struct PHeader {
    uint32_t magic;
    uint8_t  ver, backlight, contrast, hapticLvl, beepLvl, sleepIdx, histCount, histHead, txSeq;
    uint16_t unread;
};
static bool g_fsReady = false;

static void persistSave()
{
    if (!g_fsReady) return;
    InternalFS.remove(PERSIST_FILE);
    File f(InternalFS);
    if (!f.open(PERSIST_FILE, FILE_O_WRITE)) return;
    PHeader h;
    h.magic = P_MAGIC; h.ver = P_VER;
    h.backlight = g_backlight; h.contrast = g_contrast; h.hapticLvl = g_hapticLvl;
    h.beepLvl = g_beepLvl; h.sleepIdx = g_sleepIdx;
    h.histCount = g_histCount; h.histHead = g_histHead; h.txSeq = g_txSeq; h.unread = g_unread;
    f.write((const uint8_t *)&h, sizeof(h));
    f.write((const uint8_t *)g_hist, sizeof(g_hist));
    f.close();
}
static void persistLoad()
{
    if (!g_fsReady) return;
    File f(InternalFS);
    if (!f.open(PERSIST_FILE, FILE_O_READ)) return;
    PHeader h;
    if (f.read((uint8_t *)&h, sizeof(h)) == (int)sizeof(h) && h.magic == P_MAGIC && h.ver == P_VER &&
        h.histCount <= HIST_MAX && h.histHead < HIST_MAX &&
        f.read((uint8_t *)g_hist, sizeof(g_hist)) == (int)sizeof(g_hist)) {
        g_backlight = h.backlight; g_contrast = h.contrast; g_hapticLvl = h.hapticLvl;
        g_beepLvl = h.beepLvl; g_sleepIdx = h.sleepIdx;
        g_histCount = h.histCount; g_histHead = h.histHead; g_txSeq = h.txSeq; g_unread = h.unread;
        /* clamp anything corrupt so a bad blob can never wedge the UI */
        if (g_sleepIdx >= sizeof(kSleepSecs) / 2) g_sleepIdx = 2;
        if (g_hapticLvl > 3) g_hapticLvl = 2;
        if (g_beepLvl  > 2) g_beepLvl  = 1;
        if (g_contrast > 63) g_contrast = 24;
        if (g_backlight < 15) g_backlight = 210;
    }
    f.close();
}

static uint8_t nextSeq() { g_txSeq++; if (g_txSeq == 0) g_txSeq = 1; return g_txSeq; }
static Msg *getHist(int sel) { int i = (g_histHead - 1 - sel + 2 * HIST_MAX) % HIST_MAX; return &g_hist[i]; }
static void histAdd(uint8_t dir, const char *text, uint8_t delivered, uint8_t seq)
{
    Msg &m = g_hist[g_histHead];
    m.dir = dir; m.delivered = delivered; m.seq = seq;
    strncpy(m.text, text, sizeof(m.text) - 1); m.text[sizeof(m.text) - 1] = 0;
    g_histHead = (g_histHead + 1) % HIST_MAX;
    if (g_histCount < HIST_MAX) g_histCount++;
    persistSave();
}
static void histMarkDelivered(uint8_t seq)
{
    for (int i = 0; i < HIST_MAX; i++)
        if (g_hist[i].dir == 1 && g_hist[i].seq == seq && !g_hist[i].delivered) {
            g_hist[i].delivered = 1; persistSave(); return;
        }
}

/* ---- radio --------------------------------------------------------------- */
static volatile bool g_rxFlag = false;
static void onRxDone() { g_rxFlag = true; }
static void radioInit()
{
    SPI.begin();
    int st = radio.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR, LORA_SYNC, LORA_TX_DBM, 8, 0.0, false);
    if (st != RADIOLIB_ERR_NONE) { g_radioReady = false; Rtt.print(F("radio begin fail ")); Rtt.println(st); return; }
    radio.setDio2AsRfSwitch(true);
    radio.setPreambleLength(PREAMBLE_SYM);
    radio.setPacketReceivedAction(onRxDone);
    radio.startReceive();
    g_radioReady = true;
    Rtt.println(F("radio RX ready"));
}
static void sendAck(uint8_t seq)
{
    if (!g_radioReady) return;
    uint8_t a[3] = { MSG_MAGIC, PKT_ACK, seq };
    radio.transmit(a, 3);
    radio.startReceive();
}

/* ---- battery ------------------------------------------------------------- */
static void readBattery()
{
    analogReference(AR_INTERNAL); analogReadResolution(12); analogOversampling(8);
    uint32_t acc = 0; for (int i = 0; i < 8; i++) { acc += analogRead(PIN_VBAT_MON); delay(1); }
    g_battMv = (int)((acc / 8.0f) / 4095.0f * 3600.0f * VBAT_DIVIDER);
}
static int battPct() { int p = (g_battMv - 3300) * 100 / 900; return p < 0 ? 0 : p > 100 ? 100 : p; }
static void drawBattIcon(int x, int y)
{
    int p = battPct();
    display.drawFrame(x, y, 14, 7); display.drawBox(x + 14, y + 2, 2, 3);
    display.drawBox(x + 1, y + 1, p * 12 / 100, 5);
}

/* ---- screens ------------------------------------------------------------- */
enum Screen { SCR_HOME, SCR_MENU, SCR_HISTORY, SCR_MSGVIEW, SCR_SETTINGS };
static Screen g_screen  = SCR_HOME;
static int    g_sel     = 0;   /* compose selection */
static int    g_menuSel = 0;
static int    g_histSel = 0;
static bool   g_dirty   = true;

/* Full-screen incoming note: text + chime, held ~6 s or until a press. */
static void showMessage(const char *text)
{
    display.clearBuffer();
    display.drawFrame(0, 0, 128, 64);
    display.setFont(u8g2_font_6x10_tr);
    display.drawStr(4, 11, "New message");
    display.drawHLine(0, 14, 128);
    textCentered(text, 40, u8g2_font_7x13B_tr);
    display.setFont(u8g2_font_5x7_tr);
    char b[20]; snprintf(b, sizeof(b), "RSSI %d dBm", (int)g_lastRssi); display.drawStr(4, 61, b);
    display.drawStr(92, 61, "press=ok");
    display.sendBuffer();
    uint32_t end = millis() + 6000;
    while ((int32_t)(end - millis()) > 0) { if (pressDown()) break; delay(20); }
}

static void drawHome()
{
    display.clearBuffer();
    display.setFont(u8g2_font_6x10_tr);
    display.drawStr(2, 9, "Messages");
    if (g_unread > 0) { char b[10]; snprintf(b, sizeof(b), "* %d", g_unread); display.setFont(u8g2_font_5x7_tr); display.drawStr(70, 9, b); }
    drawBattIcon(110, 2);
    display.drawHLine(0, 12, 128);
    const int rows = 4, rowH = 12, top = 24;
    int first = g_sel - rows + 1; if (first < 0) first = 0;
    if (first > kNumPresets - rows) first = (kNumPresets > rows) ? kNumPresets - rows : 0;
    display.setFont(u8g2_font_7x13_tr);
    for (int i = 0; i < rows && first + i < kNumPresets; i++) {
        int idx = first + i, y = top + i * rowH;
        if (idx == g_sel) { display.drawBox(0, y - 10, 128, rowH); display.setDrawColor(0); display.drawStr(4, y, kPresets[idx]); display.setDrawColor(1); }
        else display.drawStr(4, y, kPresets[idx]);
    }
    display.sendBuffer();
}

enum { M_HIST, M_SET, M_BACK, M_COUNT };
static void drawMenu()
{
    static const char *const items[] = { "History", "Settings", "< Back" };
    display.clearBuffer();
    display.setFont(u8g2_font_6x12_tr); display.drawStr(2, 10, "Menu"); display.drawHLine(0, 12, 128);
    display.setFont(u8g2_font_7x13_tr);
    const int rowH = 14, top = 28;
    for (int i = 0; i < M_COUNT; i++) {
        int y = top + i * rowH; bool sel = (i == g_menuSel);
        char line[20];
        if (i == M_HIST && g_unread > 0) snprintf(line, sizeof(line), "History (%d)", g_unread);
        else snprintf(line, sizeof(line), "%s", items[i]);
        if (sel) { display.drawBox(0, y - 11, 128, rowH); display.setDrawColor(0); display.drawStr(6, y, line); display.setDrawColor(1); }
        else display.drawStr(6, y, line);
    }
    display.sendBuffer();
}

static void drawHistory()
{
    display.clearBuffer();
    display.setFont(u8g2_font_6x10_tr);
    display.drawStr(2, 9, "History"); drawBattIcon(110, 2); display.drawHLine(0, 12, 128);
    if (g_histCount == 0) { textCentered("(no messages)", 38, u8g2_font_7x13_tr); display.sendBuffer(); return; }
    const int rows = 4, rowH = 12, top = 23;
    int first = g_histSel - rows + 1; if (first < 0) first = 0;
    if (first > g_histCount - rows) first = (g_histCount > rows) ? g_histCount - rows : 0;
    display.setFont(u8g2_font_6x10_tr);
    for (int i = 0; i < rows && first + i < g_histCount; i++) {
        int idx = first + i, y = top + i * rowH; bool sel = (idx == g_histSel);
        Msg *m = getHist(idx);
        if (sel) { display.drawBox(0, y - 9, 128, rowH); display.setDrawColor(0); }
        char line[26]; snprintf(line, sizeof(line), "%s %s", m->dir ? ">" : "<", m->text);
        display.drawStr(3, y, line);
        if (m->dir) drawTicks(118, y - 8, m->delivered);
        if (sel) display.setDrawColor(1);
    }
    display.sendBuffer();
}

static void drawMsgView()
{
    Msg *m = getHist(g_histSel);
    display.clearBuffer(); display.drawFrame(0, 0, 128, 64);
    display.setFont(u8g2_font_6x10_tr);
    display.drawStr(4, 11, m->dir ? "Sent" : "Received");
    if (m->dir) drawTicks(112, 4, m->delivered);
    display.drawHLine(0, 14, 128);
    textCentered(m->text, 40, u8g2_font_7x13B_tr);
    display.setFont(u8g2_font_5x7_tr);
    if (m->dir) display.drawStr(4, 61, m->delivered ? "delivered" : "not acked");
    display.drawStr(84, 61, "press=back");
    display.sendBuffer();
}

/* ---- send + wait for delivery ack --------------------------------------- */
static void deliverWait(const char *text, uint8_t seq)
{
    bool delivered = false, done = false;
    uint32_t start = millis(), lastDraw = 0;
    while (!done) {
        uint32_t now = millis();
        if (now - lastDraw > 120 || delivered) {
            lastDraw = now;
            display.clearBuffer(); display.drawFrame(0, 0, 128, 64);
            display.setFont(u8g2_font_6x10_tr); display.drawStr(4, 11, "Sent");
            drawTicks(110, 4, delivered);
            display.drawHLine(0, 14, 128);
            textCentered(text, 40, u8g2_font_7x13B_tr);
            display.setFont(u8g2_font_5x7_tr);
            display.drawStr(4, 61, delivered ? "Delivered" : "Sending...");
            display.drawStr(92, 61, "press=ok");
            display.sendBuffer();
        }
        if (delivered) {
            uint32_t e = now + 900;
            while ((int32_t)(e - millis()) > 0) { if (pressDown()) break; delay(15); }
            break;
        }
        if (g_rxFlag) {
            g_rxFlag = false;
            uint8_t b[64]; int l = radio.getPacketLength(); int s = radio.readData(b, l);
            g_lastRssi = radio.getRSSI(); radio.startReceive();
            if (s == RADIOLIB_ERR_NONE && l >= 3 && b[0] == MSG_MAGIC) {
                if (b[1] == PKT_ACK && b[2] == seq) delivered = true;
                else if (b[1] == PKT_DATA) {           /* a note arrived while we waited */
                    uint8_t sq = b[2]; char t[48]; int n = l - 3; if (n > 46) n = 46;
                    memcpy(t, b + 3, n); t[n] = 0; sendAck(sq);
                    g_unread++; histAdd(0, t, 1, sq);
                }
            }
        }
        if (pressDown()) { delay(120); done = true; }
        if (!done && (now - start > ACK_TIMEOUT)) done = true;
        delay(8);
    }
    if (delivered) histMarkDelivered(seq);
}

static void sendPreset(int idx)
{
    uint8_t seq = nextSeq();
    uint8_t buf[48]; buf[0] = MSG_MAGIC; buf[1] = PKT_DATA; buf[2] = seq;
    int n = 0; const char *t = kPresets[idx];
    while (t[n] && n < 40) { buf[3 + n] = (uint8_t)t[n]; n++; }
    display.clearBuffer(); textCentered("Sending...", 34, u8g2_font_7x13B_tr); display.sendBuffer();
    int st = g_radioReady ? radio.transmit(buf, 3 + n) : RADIOLIB_ERR_UNKNOWN;
    if (g_radioReady) radio.startReceive();
    if (st == RADIOLIB_ERR_NONE) {
        chimeSent();
        histAdd(1, kPresets[idx], 0, seq);
        deliverWait(kPresets[idx], seq);
    } else {
        display.clearBuffer(); textCentered("Send failed", 34, u8g2_font_7x13B_tr); display.sendBuffer();
        Rtt.print(F("tx fail ")); Rtt.println(st); delay(800);
    }
}

static void wakeScreen();
static void handleIncoming()
{
    uint8_t buf[64]; int len = radio.getPacketLength();
    int st = radio.readData(buf, len); g_lastRssi = radio.getRSSI();
    if (st != RADIOLIB_ERR_NONE || len < 3 || buf[0] != MSG_MAGIC) { radio.startReceive(); return; }
    if (buf[1] == PKT_ACK) { radio.startReceive(); histMarkDelivered(buf[2]); return; }
    /* PKT_DATA */
    uint8_t seq = buf[2];
    char text[48]; int n = len - 3; if (n > 46) n = 46;
    memcpy(text, buf + 3, n); text[n] = 0;
    Rtt.print(F("RX: ")); Rtt.println(text);
    sendAck(seq);                       /* ack before the (blocking) view */
    g_unread++; histAdd(0, text, 0, seq);
    chimeIncoming();
    showMessage(text);
}

/* ---- settings ------------------------------------------------------------ */
enum { S_BL, S_CONTRAST, S_SLEEP, S_HAPTIC, S_BEEP, S_BACK, S_COUNT };
static int  g_setSel = 0;
static bool g_setEdit = false;

static void settingValue(int item, char *out, int n)
{
    switch (item) {
        case S_BL:       snprintf(out, n, "%d%%", g_backlight * 100 / 255); break;
        case S_CONTRAST: snprintf(out, n, "%d", g_contrast); break;
        case S_SLEEP:    snprintf(out, n, "%s", kSleepLbl[g_sleepIdx]); break;
        case S_HAPTIC:   snprintf(out, n, "%s", kHapLbl[g_hapticLvl]); break;
        case S_BEEP:     snprintf(out, n, "%s", kBeepLbl[g_beepLvl]); break;
        default:         out[0] = 0; break;
    }
}
static void drawSettings()
{
    static const char *label[] = { "Backlight", "Contrast", "Sleep after", "Haptic", "Beep", "< Back" };
    display.clearBuffer();
    display.setFont(u8g2_font_6x12_tr); display.drawStr(2, 10, "Settings"); display.drawHLine(0, 12, 128);
    const int rows = 4, rowH = 12, top = 24;
    int first = g_setSel - rows + 1; if (first < 0) first = 0;
    if (first > S_COUNT - rows) first = S_COUNT - rows;
    display.setFont(u8g2_font_6x10_tr);
    for (int i = 0; i < rows && first + i < S_COUNT; i++) {
        int idx = first + i, y = top + i * rowH; bool selRow = (idx == g_setSel);
        if (selRow && !g_setEdit) { display.drawBox(0, y - 9, 128, rowH); display.setDrawColor(0); }
        display.drawStr(3, y, label[idx]);
        char v[12]; settingValue(idx, v, sizeof(v));
        if (v[0]) { char shown[16]; if (selRow && g_setEdit) snprintf(shown, sizeof(shown), ">%s<", v); else snprintf(shown, sizeof(shown), "%s", v);
                    display.drawStr(128 - display.getStrWidth(shown) - 3, y, shown); }
        if (selRow && !g_setEdit) display.setDrawColor(1);
    }
    display.sendBuffer();
}
static void adjustSetting(int item, int dir)
{
    switch (item) {
        case S_BL: { int v = g_backlight + dir * 15; if (v < 15) v = 15; if (v > 255) v = 255; g_backlight = v; backlight(g_backlight); } break;
        case S_CONTRAST: { int v = g_contrast + dir * 2; if (v < 0) v = 0; if (v > 63) v = 63; g_contrast = v; display.setContrast(g_contrast); } break;
        case S_SLEEP: g_sleepIdx = (g_sleepIdx + (dir > 0 ? 1 : (int)(sizeof(kSleepSecs) / 2) - 1)) % (sizeof(kSleepSecs) / 2); break;
        case S_HAPTIC: g_hapticLvl = (g_hapticLvl + (dir > 0 ? 1 : 3)) % 4; buzz(); break;
        case S_BEEP: g_beepLvl = (g_beepLvl + (dir > 0 ? 1 : 2)) % 3; beep(2093, 80); break;
    }
}

/* ---- screen sleep (radio stays in duty-cycled RX) ------------------------ */
static void wakeScreen()
{
    if (!g_asleep) return;
    g_asleep = false;
    digitalWrite(PIN_LRA_EN, HIGH); delay(1); drvConfig();
    display.setPowerSave(0); backlight(g_backlight);
    if (g_radioReady) radio.startReceive();
    g_dirty = true; g_lastActive = millis();
}
static void sleepScreen()
{
    g_asleep = true; backlight(0); display.setPowerSave(1);
    digitalWrite(PIN_LRA_EN, LOW);
    if (g_radioReady) radio.startReceiveDutyCycleAuto(PREAMBLE_SYM, 8);
}

/* ---- fault reporter + I2C recovery -------------------------------------- */
extern "C" __attribute__((used)) void hardfault_report(uint32_t *frame)
{
    uint32_t pc = frame[6]; uint32_t cfsr = *(volatile uint32_t *)0xE000ED28;
    Rtt.print(F("\n*** HARDFAULT PC=0x")); Rtt.print(pc, HEX); Rtt.print(F(" CFSR=0x")); Rtt.println(cfsr, HEX);
    backlight(255); display.setPowerSave(0); display.clearBuffer();
    display.setFont(u8g2_font_6x12_tr); display.drawStr(0, 11, "*** HARD FAULT ***");
    char b[26]; snprintf(b, sizeof(b), "PC   %08lX", (unsigned long)pc); display.drawStr(0, 30, b);
    snprintf(b, sizeof(b), "CFSR %08lX", (unsigned long)cfsr); display.drawStr(0, 46, b);
    display.sendBuffer(); while (1) { }
}
extern "C" __attribute__((naked)) void HardFault_Handler(void)
{ __asm volatile("tst lr,#4\n ite eq\n mrseq r0,msp\n mrsne r0,psp\n ldr r1,=hardfault_report\n bx r1\n"); }

static void i2cRecover()
{
    pinMode(PIN_SDA, INPUT_PULLUP); pinMode(PIN_SCL, OUTPUT);
    for (int i = 0; i < 9 && digitalRead(PIN_SDA) == LOW; i++) { digitalWrite(PIN_SCL, LOW); delayMicroseconds(6); digitalWrite(PIN_SCL, HIGH); delayMicroseconds(6); }
    pinMode(PIN_SDA, OUTPUT); digitalWrite(PIN_SDA, LOW); delayMicroseconds(6);
    digitalWrite(PIN_SCL, HIGH); delayMicroseconds(6); digitalWrite(PIN_SDA, HIGH); delayMicroseconds(6);
    pinMode(PIN_SCL, INPUT_PULLUP); pinMode(PIN_SDA, INPUT_PULLUP);
}

/* ---- setup / loop -------------------------------------------------------- */
void setup()
{
    NRF_CLOCK->TASKS_LFCLKSTOP = 1;
    NRF_CLOCK->LFCLKSRC = (CLOCK_LFCLKSRC_SRC_RC << CLOCK_LFCLKSRC_SRC_Pos);
    NRF_CLOCK->EVENTS_LFCLKSTARTED = 0; NRF_CLOCK->TASKS_LFCLKSTART = 1;
    for (volatile uint32_t i = 0; i < 2000000 && !NRF_CLOCK->EVENTS_LFCLKSTARTED; i++) { }

    NRF_POWER->DCDCEN = 1;
    Rtt.begin(); Rtt.println(F("DCS plain messenger boot"));

    g_fsReady = InternalFS.begin();
    persistLoad();
    Rtt.print(F("fs ")); Rtt.print(g_fsReady ? F("ok") : F("FAIL"));
    Rtt.print(F(" hist=")); Rtt.println(g_histCount);

    pinMode(PIN_ROLL_R, INPUT_PULLUP); pinMode(PIN_ROLL_L, INPUT_PULLUP); pinMode(PIN_PRESS, INPUT_PULLUP);

    display.begin(); display.setContrast(g_contrast);
    pinMode(PIN_DISPLAY_PWM, OUTPUT); backlight(g_backlight);
    display.clearBuffer(); display.setFont(u8g2_font_7x13B_tr);
    textCentered("Messenger", 30, u8g2_font_7x13B_tr); display.sendBuffer();

    pinMode(PIN_LRA_EN, OUTPUT); digitalWrite(PIN_LRA_EN, HIGH);
    delay(2); i2cRecover(); Wire.begin(); Wire.setClock(100000); delay(3);
    if (drv.begin()) { g_drvReady = true; drvConfig(); }

    radioInit(); readBattery(); delay(700); buzz();
    g_screen = SCR_HOME; g_dirty = true; g_lastActive = millis();
    Rtt.println(F("ready"));
}

void loop()
{
    uint32_t now = millis();

    if (g_rxFlag) { g_rxFlag = false; wakeScreen(); handleIncoming(); g_lastActive = millis(); g_dirty = true; }

    WheelEv w; BtnEv b; pollInputs(w, b);
    if (Rtt.available()) {
        char c = (char)Rtt.read();
        if (c == 'w' || c == 'k') w = WH_UP;
        else if (c == 's' || c == 'j') w = WH_DOWN;
        else if (c == '\r' || c == '\n' || c == ' ') b = BTN_SHORT;
        else if (c == 'b') b = BTN_LONG;
    }
    bool anyInput = (w != WH_NONE) || (b != BTN_NONE);
    if (anyInput) g_lastActive = now;

    if (g_asleep) {
        if (anyInput) { wakeScreen(); w = WH_NONE; b = BTN_NONE; }
        else { delay(20); return; }
    } else {
        uint16_t to = kSleepSecs[g_sleepIdx];
        if (to && now - g_lastActive > (uint32_t)to * 1000) { sleepScreen(); delay(20); return; }
    }

    static uint32_t tBatt = 0;
    if (now - tBatt > 10000) { tBatt = now; readBattery(); if (g_screen == SCR_HOME) g_dirty = true; }

    switch (g_screen) {
    case SCR_HOME:
        if (w == WH_DOWN) { g_sel = (g_sel + 1) % kNumPresets; g_dirty = true; }
        if (w == WH_UP)   { g_sel = (g_sel - 1 + kNumPresets) % kNumPresets; g_dirty = true; }
        if (b == BTN_SHORT) { sendPreset(g_sel); g_lastActive = millis(); g_dirty = true; }
        if (b == BTN_LONG)  { g_screen = SCR_MENU; g_menuSel = 0; g_dirty = true; }
        break;

    case SCR_MENU:
        if (w == WH_DOWN) { g_menuSel = (g_menuSel + 1) % M_COUNT; g_dirty = true; }
        if (w == WH_UP)   { g_menuSel = (g_menuSel - 1 + M_COUNT) % M_COUNT; g_dirty = true; }
        if (b == BTN_SHORT) {
            if (g_menuSel == M_HIST) { g_screen = SCR_HISTORY; g_histSel = 0; if (g_unread) { g_unread = 0; persistSave(); } }
            else if (g_menuSel == M_SET) { g_screen = SCR_SETTINGS; g_setSel = 0; g_setEdit = false; }
            else g_screen = SCR_HOME;
            g_dirty = true;
        }
        if (b == BTN_LONG) { g_screen = SCR_HOME; g_dirty = true; }
        break;

    case SCR_HISTORY:
        if (g_histCount) {
            if (w == WH_DOWN) { g_histSel = (g_histSel + 1) % g_histCount; g_dirty = true; }
            if (w == WH_UP)   { g_histSel = (g_histSel - 1 + g_histCount) % g_histCount; g_dirty = true; }
            if (b == BTN_SHORT) { g_screen = SCR_MSGVIEW; g_dirty = true; }
        }
        if (b == BTN_LONG) { g_screen = SCR_MENU; g_dirty = true; }
        break;

    case SCR_MSGVIEW:
        if (b == BTN_SHORT || b == BTN_LONG) { g_screen = SCR_HISTORY; g_dirty = true; }
        break;

    case SCR_SETTINGS:
        if (g_setEdit) {
            if (w == WH_UP)   { adjustSetting(g_setSel, +1); g_dirty = true; }
            if (w == WH_DOWN) { adjustSetting(g_setSel, -1); g_dirty = true; }
            if (b == BTN_SHORT || b == BTN_LONG) { g_setEdit = false; persistSave(); g_dirty = true; }
        } else {
            if (w == WH_DOWN) { g_setSel = (g_setSel + 1) % S_COUNT; g_dirty = true; }
            if (w == WH_UP)   { g_setSel = (g_setSel - 1 + S_COUNT) % S_COUNT; g_dirty = true; }
            if (b == BTN_SHORT) { if (g_setSel == S_BACK) { persistSave(); g_screen = SCR_MENU; g_dirty = true; } else { g_setEdit = true; g_dirty = true; } }
            if (b == BTN_LONG) { persistSave(); g_screen = SCR_MENU; g_dirty = true; }
        }
        break;
    }

    /* Draw AFTER input handling, dispatched on the current screen — so a
     * screen change (e.g. long-press -> Menu) paints immediately instead of
     * waiting for the next input to re-set g_dirty. */
    if (g_dirty) {
        switch (g_screen) {
        case SCR_HOME:     drawHome();     break;
        case SCR_MENU:     drawMenu();     break;
        case SCR_HISTORY:  drawHistory();  break;
        case SCR_MSGVIEW:  drawMsgView();  break;
        case SCR_SETTINGS: drawSettings(); break;
        }
        g_dirty = false;
    }
    delay(3);
}
