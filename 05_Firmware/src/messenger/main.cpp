/* ===========================================================================
 *  SPDX-License-Identifier: MIT
 *  Copyright (c) 2026 Mixed Signal Development GmbH
 *
 *  Doomsday Comms System (D.C.S., SKU OS2601) — "love notes" point-to-point
 *  messenger
 *
 *  Two identical units. Scroll the wheel to pick a preset message, press to
 *  send it over LoRa to the other unit. Sending previews the animation; when a
 *  message ARRIVES it plays the same animation with a haptic buzz + chime.
 *
 *  Controls (rev-A wheel, post-bodge mapping):
 *     roll  = scroll / adjust      press = send / select      hold = settings
 * =========================================================================== */
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <U8g2lib.h>
#include <Adafruit_DRV2605.h>
#include <RadioLib.h>
#include "nrf_gpio.h"
#include "rtt.h"

/* ---- Display (ST7565 128x64 SW-SPI) -------------------------------------- */
#define DISP_ARGS U8G2_R0, PIN_DISPLAY_SCK, PIN_DISPLAY_MOSI, \
                  PIN_DISPLAY_CS_N, PIN_DISPLAY_RS, PIN_DISPLAY_RES
U8G2_ST7565_ERC12864_F_4W_SW_SPI display(DISP_ARGS);

Adafruit_DRV2605 drv;
static bool g_drvReady = false;

SX1262 radio = new Module(PIN_LORA_CS_N, PIN_DIO1, PIN_LORA_RST_N, PIN_BUSY);
static bool  g_radioReady = false;
static float g_lastRssi   = 0;

/* ---- User settings (kept in RAM; persistence is a future add) ------------ */
static uint8_t  g_backlight = 210;   /* PWM 0..255                          */
static uint8_t  g_contrast  = 24;    /* ST7565 electronic volume 0..63      */
static uint8_t  g_hapticLvl = 2;     /* 0 Off, 1 Soft, 2 Med, 3 Strong      */
static uint8_t  g_beepLvl   = 1;     /* 0 Off, 1 On, 2 Loud                 */
static uint8_t  g_sleepIdx  = 2;     /* screen-off timeout index            */

static const uint16_t    kSleepSecs[] = { 0, 15, 30, 60, 120 };
static const char *const kSleepLbl[]  = { "Off", "15s", "30s", "1m", "2m" };
static const char *const kHapLbl[]    = { "Off", "Soft", "Med", "Strong" };
static const char *const kBeepLbl[]   = { "Off", "On", "Loud" };

static uint32_t g_lastActive = 0;
static bool     g_asleep     = false;
static int      g_battMv     = 0;

/* ===========================================================================
 *  Preset messages + which animation plays for each
 * =========================================================================== */
enum Anim { AN_BEAT, AN_FLOAT, AN_BANNER, AN_STARS };
struct Preset { const char *text; uint8_t anim; };
static const Preset kPresets[] = {
    { "I love you",       AN_BEAT   },
    { "I miss you",       AN_FLOAT  },
    { "Thinking of you",  AN_FLOAT  },
    { "You're cute",      AN_BANNER },
    { "Kiss you!",        AN_BEAT   },
    { "Good morning!",    AN_BANNER },
    { "Good night",       AN_STARS  },
    { "Sweet dreams",     AN_STARS  },
    { "Call me?",         AN_BANNER },
    { "Home soon <3",     AN_FLOAT  },
};
static const int kNumPresets = sizeof(kPresets) / sizeof(kPresets[0]);
#define MSG_MAGIC 0xC5                /* packet: [magic][anim][text...] */

/* Long preamble so a duty-cycled receiver can't miss a packet. ~250 symbols at
 * SF9/BW125 = ~1 s: sets both the wake-up latency and the TX airtime overhead.
 * Bigger = longer battery when idle but slower/longer sends; smaller = snappier
 * but the radio must wake more often (more idle current). */
#define PREAMBLE_SYM 250

/* ===========================================================================
 *  Input — three clean contacts after the rev-A bodge:
 *    P0.12 = PRESS, P0.07 = roll R, P0.13 = roll L
 * =========================================================================== */
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

/* ===========================================================================
 *  Feedback — haptics + piezo, both respect the settings
 * =========================================================================== */
static void backlight(uint8_t d) { analogWrite(PIN_DISPLAY_PWM, d); }

static void drvConfig()               /* (re)apply DRV2605 mode after power-up */
{
    if (!g_drvReady) return;
    drv.useLRA(); drv.selectLibrary(6); drv.setMode(DRV2605_MODE_INTTRIG);
}
static void haptic(uint8_t fx)
{
    if (!g_drvReady || g_hapticLvl == 0) return;
    drv.setWaveform(0, fx); drv.setWaveform(1, 0); drv.go();
}
static void buzz()                    /* feedback buzz scaled by haptic level */
{
    static const uint8_t fx[] = { 0, 1, 16, 47 };
    haptic(fx[g_hapticLvl]);
}

static void beepLoud(uint16_t freq, uint16_t ms)   /* complementary antiphase */
{
    const uint32_t base = 16000000UL;
    uint16_t top = (uint16_t)(base / freq);
    static uint16_t seq[2];
    seq[0] = top / 2; seq[1] = (top / 2) | 0x8000;
    NRF_PWM_Type *pwm = NRF_PWM2;                   /* PWM2: avoid backlight PWM */
    pwm->PSEL.OUT[0] = g_ADigitalPinMap[PIN_BUZ_PLUS];
    pwm->PSEL.OUT[1] = g_ADigitalPinMap[PIN_BUZ_MINUS];
    pwm->ENABLE = 1; pwm->MODE = 0; pwm->PRESCALER = 0; pwm->COUNTERTOP = top; pwm->LOOP = 0;
    pwm->DECODER = (PWM_DECODER_LOAD_Individual << PWM_DECODER_LOAD_Pos) |
                   (PWM_DECODER_MODE_RefreshCount << PWM_DECODER_MODE_Pos);
    pwm->SEQ[0].PTR = (uint32_t)seq; pwm->SEQ[0].CNT = 2;
    pwm->SEQ[0].REFRESH = 0; pwm->SEQ[0].ENDDELAY = 0;
    pwm->TASKS_SEQSTART[0] = 1;
    delay(ms);
    pwm->TASKS_STOP = 1; pwm->ENABLE = 0;
    pinMode(PIN_BUZ_PLUS, OUTPUT);  digitalWrite(PIN_BUZ_PLUS, LOW);
    pinMode(PIN_BUZ_MINUS, OUTPUT); digitalWrite(PIN_BUZ_MINUS, LOW);
}
static void beep(uint16_t f, uint16_t ms)
{
    if (g_beepLvl == 0) return;
    if (g_beepLvl == 2) { beepLoud(f, ms); return; }
    pinMode(PIN_BUZ_MINUS, OUTPUT); digitalWrite(PIN_BUZ_MINUS, LOW);
    tone(PIN_BUZ_PLUS, f); delay(ms);
    noTone(PIN_BUZ_PLUS); digitalWrite(PIN_BUZ_PLUS, LOW);
}

static void chimeIncoming() { buzz(); beep(1568, 90); beep(2093, 90); beep(2637, 150); buzz(); }
static void chimeSent()     { buzz(); beep(2093, 60); beep(2637, 90); }

/* ===========================================================================
 *  Graphics helpers + animations
 * =========================================================================== */
static void textCentered(const char *s, int y, const uint8_t *font)
{
    display.setFont(font);
    display.drawStr((128 - display.getStrWidth(s)) / 2, y, s);
}
static void drawHeart(int cx, int cy, int r)
{
    display.drawDisc(cx - r, cy - r, r); display.drawDisc(cx + r, cy - r, r);
    display.drawTriangle(cx - 2 * r, cy - r, cx + 2 * r, cy - r, cx, cy + 2 * r);
}
static void drawHeartOutline(int cx, int cy, int r)
{
    display.drawCircle(cx - r, cy - r, r); display.drawCircle(cx + r, cy - r, r);
    display.drawTriangle(cx - 2 * r, cy - r, cx + 2 * r, cy - r, cx, cy + 2 * r);
}

static void animBeat(const char *text)
{
    uint32_t end = millis() + 2800;
    for (int f = 0; (int32_t)(end - millis()) > 0; f++) {
        int ph = f % 16, r = 7 + (ph < 8 ? ph : 16 - ph);
        display.clearBuffer();
        drawHeart(64, 24, r / 2 + 4);
        textCentered(text, 60, u8g2_font_7x13B_tr);
        display.sendBuffer(); delay(70);
    }
}
static void animFloat(const char *text)
{
    uint32_t end = millis() + 2800;
    for (int f = 0; (int32_t)(end - millis()) > 0; f++) {
        display.clearBuffer();
        for (int i = 0; i < 7; i++) {
            int x = 10 + i * 17, y = 70 - ((f * 3 + i * 19) % 84);
            if (y > -6 && y < 70) drawHeart(x, y, 2 + (i % 2));
        }
        display.drawBox(0, 26, 128, 14); display.setDrawColor(0);
        textCentered(text, 37, u8g2_font_7x13B_tr); display.setDrawColor(1);
        display.sendBuffer(); delay(60);
    }
}
static void animBanner(const char *text)
{
    uint32_t end = millis() + 2800;
    for (int f = 0; (int32_t)(end - millis()) > 0; f++) {
        display.clearBuffer(); display.drawFrame(0, 0, 128, 64);
        if ((f / 4) % 2) { drawHeart(12, 14, 4); drawHeart(116, 14, 4); }
        else { drawHeartOutline(12, 14, 4); drawHeartOutline(116, 14, 4); }
        textCentered(text, 40, u8g2_font_7x13B_tr);
        display.sendBuffer(); delay(70);
    }
}
static void animStars(const char *text)
{
    static const uint8_t sx[] = { 8, 22, 40, 58, 74, 92, 110, 120, 30, 100 };
    static const uint8_t sy[] = { 8, 18, 6, 20, 10, 4, 16, 26, 30, 30 };
    uint32_t end = millis() + 2800;
    for (int f = 0; (int32_t)(end - millis()) > 0; f++) {
        display.clearBuffer();
        for (int i = 0; i < 10; i++) if ((f + i * 3) % 4) display.drawPixel(sx[i], sy[i]);
        display.drawDisc(104, 14, 9); display.setDrawColor(0); display.drawDisc(100, 11, 8);
        display.setDrawColor(1);
        textCentered(text, 52, u8g2_font_7x13B_tr);
        display.sendBuffer(); delay(90);
    }
}
static void playAnim(uint8_t anim, const char *text)
{
    switch (anim) {
        case AN_FLOAT:  animFloat(text);  break;
        case AN_BANNER: animBanner(text); break;
        case AN_STARS:  animStars(text);  break;
        default:        animBeat(text);   break;
    }
}

/* ===========================================================================
 *  Radio — point-to-point send / receive
 * =========================================================================== */
static volatile bool g_rxFlag = false;
static void onRxDone() { g_rxFlag = true; }

static void radioInit()
{
    SPI.begin();
    int st = radio.begin(868.0, 125.0, 9, 7, 0x2B, 14, 8, 0.0, false);
    if (st != RADIOLIB_ERR_NONE) { g_radioReady = false; Rtt.print(F("radio begin fail ")); Rtt.println(st); return; }
    radio.setDio2AsRfSwitch(true);
    radio.setPreambleLength(PREAMBLE_SYM);   /* long preamble for duty-cycle RX */
    radio.setPacketReceivedAction(onRxDone);
    radio.startReceive();
    g_radioReady = true;
    Rtt.println(F("radio RX ready"));
}

static void sendPreset(int idx)
{
    uint8_t buf[48]; buf[0] = MSG_MAGIC; buf[1] = kPresets[idx].anim;
    int n = 0; const char *t = kPresets[idx].text;
    while (t[n] && n < 40) { buf[2 + n] = (uint8_t)t[n]; n++; }
    display.clearBuffer(); textCentered("Sending...", 30, u8g2_font_7x13B_tr);
    drawHeartOutline(64, 48, 4); display.sendBuffer();

    int st = g_radioReady ? radio.transmit(buf, 2 + n) : RADIOLIB_ERR_UNKNOWN;
    if (g_radioReady) radio.startReceive();

    display.clearBuffer();
    if (st == RADIOLIB_ERR_NONE) { textCentered("Sent", 30, u8g2_font_7x13B_tr); drawHeart(64, 48, 5); chimeSent(); }
    else { textCentered("Send failed", 30, u8g2_font_7x13B_tr); Rtt.print(F("tx fail ")); Rtt.println(st); }
    display.sendBuffer(); delay(500);
    playAnim(kPresets[idx].anim, kPresets[idx].text);   /* preview what you sent */
}

static void wakeScreen();
static void handleIncoming()
{
    uint8_t buf[64]; int len = radio.getPacketLength();
    int st = radio.readData(buf, len); g_lastRssi = radio.getRSSI();
    radio.startReceive();
    if (st != RADIOLIB_ERR_NONE || len < 2 || buf[0] != MSG_MAGIC) return;
    char text[48]; int n = len - 2; if (n > 46) n = 46;
    memcpy(text, buf + 2, n); text[n] = 0;
    Rtt.print(F("RX: ")); Rtt.println(text);
    chimeIncoming();
    playAnim(buf[1], text);
}

/* ===========================================================================
 *  Battery + UI screens
 * =========================================================================== */
static void readBattery()
{
    analogReference(AR_INTERNAL); analogReadResolution(12); analogOversampling(8);
    uint32_t acc = 0; for (int i = 0; i < 8; i++) { acc += analogRead(PIN_VBAT_MON); delay(1); }
    g_battMv = (int)((acc / 8.0f) / 4095.0f * 3600.0f * VBAT_DIVIDER);
}
static int battPct() { int p = (g_battMv - 3300) * 100 / 900; return p < 0 ? 0 : p > 100 ? 100 : p; }

enum Screen { SCR_HOME, SCR_SETTINGS };
static Screen g_screen = SCR_HOME;
static int    g_sel = 0;
static bool   g_dirty = true;

static void drawBattIcon(int x, int y)
{
    int p = battPct();
    display.drawFrame(x, y, 14, 7); display.drawBox(x + 14, y + 2, 2, 3);
    display.drawBox(x + 1, y + 1, p * 12 / 100, 5);
}

static void drawHome()
{
    display.clearBuffer();
    display.setFont(u8g2_font_6x10_tr);
    display.drawStr(2, 9, "Send a note");
    drawBattIcon(110, 2);
    display.drawHLine(0, 12, 128);
    const int rows = 4, rowH = 12, top = 24;
    int first = g_sel - rows + 1; if (first < 0) first = 0;
    if (first > kNumPresets - rows) first = (kNumPresets > rows) ? kNumPresets - rows : 0;
    display.setFont(u8g2_font_7x13_tr);
    for (int i = 0; i < rows && first + i < kNumPresets; i++) {
        int idx = first + i, y = top + i * rowH;
        if (idx == g_sel) {
            display.drawBox(0, y - 10, 128, rowH); display.setDrawColor(0);
            display.drawStr(4, y, kPresets[idx].text); display.setDrawColor(1);
        } else display.drawStr(4, y, kPresets[idx].text);
    }
    display.sendBuffer();
}

/* ---- Settings screen (edit-in-place) ------------------------------------- */
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
    static const char *label[] = { "Backlight", "Contrast", "Sleep after",
                                   "Haptic", "Beep", "< Back" };
    display.clearBuffer();
    display.setFont(u8g2_font_6x12_tr); display.drawStr(2, 10, "Settings");
    display.drawHLine(0, 12, 128);
    const int rows = 4, rowH = 12, top = 24;
    int first = g_setSel - rows + 1; if (first < 0) first = 0;
    if (first > S_COUNT - rows) first = S_COUNT - rows;
    display.setFont(u8g2_font_6x10_tr);
    for (int i = 0; i < rows && first + i < S_COUNT; i++) {
        int idx = first + i, y = top + i * rowH;
        bool selRow = (idx == g_setSel);
        if (selRow && !g_setEdit) { display.drawBox(0, y - 9, 128, rowH); display.setDrawColor(0); }
        display.drawStr(3, y, label[idx]);
        char v[12]; settingValue(idx, v, sizeof(v));
        if (v[0]) {
            char shown[16];
            if (selRow && g_setEdit) snprintf(shown, sizeof(shown), ">%s<", v);
            else                     snprintf(shown, sizeof(shown), "%s", v);
            display.drawStr(128 - display.getStrWidth(shown) - 3, y, shown);
        }
        if (selRow && !g_setEdit) display.setDrawColor(1);
    }
    display.sendBuffer();
}
static void adjustSetting(int item, int dir)   /* dir = +1 / -1 */
{
    switch (item) {
        case S_BL: {
            int v = g_backlight + dir * 15; if (v < 15) v = 15; if (v > 255) v = 255;
            g_backlight = v; backlight(g_backlight); } break;
        case S_CONTRAST: {
            int v = g_contrast + dir * 2; if (v < 0) v = 0; if (v > 63) v = 63;
            g_contrast = v; display.setContrast(g_contrast); } break;
        case S_SLEEP:
            g_sleepIdx = (g_sleepIdx + (dir > 0 ? 1 : (int)(sizeof(kSleepSecs) / 2) - 1)) % (sizeof(kSleepSecs) / 2);
            break;
        case S_HAPTIC:
            g_hapticLvl = (g_hapticLvl + (dir > 0 ? 1 : 3)) % 4; buzz(); break;
        case S_BEEP:
            g_beepLvl = (g_beepLvl + (dir > 0 ? 1 : 2)) % 3; beep(2093, 80); break;
    }
}

/* ===========================================================================
 *  Screen sleep (radio stays in RX so messages still arrive & wake it)
 * =========================================================================== */
static void wakeScreen()
{
    if (!g_asleep) return;
    g_asleep = false;
    digitalWrite(PIN_LRA_EN, HIGH); delay(1); drvConfig();   /* DRV back up */
    display.setPowerSave(0);
    backlight(g_backlight);
    if (g_radioReady) radio.startReceive();                  /* full RX while awake */
    g_dirty = true; g_lastActive = millis();
}
static void sleepScreen()
{
    g_asleep = true;
    backlight(0);
    display.setPowerSave(1);                                 /* LCD sleep */
    digitalWrite(PIN_LRA_EN, LOW);                           /* DRV2605 off */
    /* low-power listening: radio wakes every ~cycle to sniff for a preamble;
     * a packet still fires DIO1 -> g_rxFlag -> wakeScreen() + animation */
    if (g_radioReady) radio.startReceiveDutyCycleAuto(PREAMBLE_SYM, 8);
}

/* ===========================================================================
 *  Fault reporter
 * =========================================================================== */
extern "C" __attribute__((used)) void hardfault_report(uint32_t *frame)
{
    uint32_t pc = frame[6]; uint32_t cfsr = *(volatile uint32_t *)0xE000ED28;
    Rtt.print(F("\n*** HARDFAULT PC=0x")); Rtt.print(pc, HEX);
    Rtt.print(F(" CFSR=0x")); Rtt.println(cfsr, HEX);
    backlight(255); display.setPowerSave(0); display.clearBuffer();
    display.setFont(u8g2_font_6x12_tr); display.drawStr(0, 11, "*** HARD FAULT ***");
    char b[26]; snprintf(b, sizeof(b), "PC   %08lX", (unsigned long)pc); display.drawStr(0, 30, b);
    snprintf(b, sizeof(b), "CFSR %08lX", (unsigned long)cfsr); display.drawStr(0, 46, b);
    display.sendBuffer(); while (1) { }
}
extern "C" __attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile("tst lr,#4\n ite eq\n mrseq r0,msp\n mrsne r0,psp\n"
                   "ldr r1,=hardfault_report\n bx r1\n");
}

static void i2cRecover()
{
    pinMode(PIN_SDA, INPUT_PULLUP); pinMode(PIN_SCL, OUTPUT);
    for (int i = 0; i < 9 && digitalRead(PIN_SDA) == LOW; i++) {
        digitalWrite(PIN_SCL, LOW);  delayMicroseconds(6);
        digitalWrite(PIN_SCL, HIGH); delayMicroseconds(6);
    }
    pinMode(PIN_SDA, OUTPUT);
    digitalWrite(PIN_SDA, LOW);  delayMicroseconds(6);
    digitalWrite(PIN_SCL, HIGH); delayMicroseconds(6);
    digitalWrite(PIN_SDA, HIGH); delayMicroseconds(6);
    pinMode(PIN_SCL, INPUT_PULLUP); pinMode(PIN_SDA, INPUT_PULLUP);
}

/* ===========================================================================
 *  Arduino entry points
 * =========================================================================== */
void setup()
{
    /* Force LFCLK to internal RC (this module has no external 32k crystal). */
    NRF_CLOCK->TASKS_LFCLKSTOP = 1;
    NRF_CLOCK->LFCLKSRC = (CLOCK_LFCLKSRC_SRC_RC << CLOCK_LFCLKSRC_SRC_Pos);
    NRF_CLOCK->EVENTS_LFCLKSTARTED = 0;
    NRF_CLOCK->TASKS_LFCLKSTART = 1;
    for (volatile uint32_t i = 0; i < 2000000 && !NRF_CLOCK->EVENTS_LFCLKSTARTED; i++) { }

    NRF_POWER->DCDCEN = 1;
    Rtt.begin();
    Rtt.println(F("DCS messenger boot"));

    pinMode(PIN_ROLL_R, INPUT_PULLUP);
    pinMode(PIN_ROLL_L, INPUT_PULLUP);
    pinMode(PIN_PRESS,  INPUT_PULLUP);

    display.begin();
    display.setContrast(g_contrast);
    pinMode(PIN_DISPLAY_PWM, OUTPUT);
    backlight(g_backlight);

    display.clearBuffer();
    drawHeart(64, 26, 9);
    textCentered("for you", 54, u8g2_font_7x13B_tr);
    display.sendBuffer();

    pinMode(PIN_LRA_EN, OUTPUT); digitalWrite(PIN_LRA_EN, HIGH);
    delay(2); i2cRecover(); Wire.begin(); Wire.setClock(100000); delay(3);
    if (drv.begin()) { g_drvReady = true; drv.useLRA(); drv.selectLibrary(6); drv.setMode(DRV2605_MODE_INTTRIG); }

    radioInit();
    readBattery();
    delay(800);
    buzz();
    g_screen = SCR_HOME; g_dirty = true;
    g_lastActive = millis();
    Rtt.println(F("ready"));
}

void loop()
{
    uint32_t now = millis();

    /* incoming message wakes the screen and animates */
    if (g_rxFlag) {
        g_rxFlag = false;
        wakeScreen();
        handleIncoming();
        g_lastActive = millis(); g_dirty = true;
    }

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

    /* sleep / wake handling */
    if (g_asleep) {
        if (anyInput) { wakeScreen(); w = WH_NONE; b = BTN_NONE; }  /* consume wake input */
        else { delay(20); return; }
    } else {
        uint16_t to = kSleepSecs[g_sleepIdx];
        if (to && now - g_lastActive > (uint32_t)to * 1000) { sleepScreen(); delay(20); return; }
    }

    /* periodic battery refresh (~10 s) */
    static uint32_t tBatt = 0;
    if (now - tBatt > 10000) { tBatt = now; readBattery(); if (g_screen == SCR_HOME) g_dirty = true; }

    if (g_screen == SCR_HOME) {
        if (w == WH_DOWN) { g_sel = (g_sel + 1) % kNumPresets; g_dirty = true; }
        if (w == WH_UP)   { g_sel = (g_sel - 1 + kNumPresets) % kNumPresets; g_dirty = true; }
        if (b == BTN_SHORT) { sendPreset(g_sel); g_lastActive = millis(); g_dirty = true; }
        if (b == BTN_LONG)  { g_screen = SCR_SETTINGS; g_setSel = 0; g_setEdit = false; g_dirty = true; }
        if (g_dirty) { drawHome(); g_dirty = false; }
    } else { /* SCR_SETTINGS */
        if (g_setEdit) {
            if (w == WH_UP)   { adjustSetting(g_setSel, +1); g_dirty = true; }
            if (w == WH_DOWN) { adjustSetting(g_setSel, -1); g_dirty = true; }
            if (b == BTN_SHORT || b == BTN_LONG) { g_setEdit = false; g_dirty = true; }
        } else {
            if (w == WH_DOWN) { g_setSel = (g_setSel + 1) % S_COUNT; g_dirty = true; }
            if (w == WH_UP)   { g_setSel = (g_setSel - 1 + S_COUNT) % S_COUNT; g_dirty = true; }
            if (b == BTN_SHORT) {
                if (g_setSel == S_BACK) { g_screen = SCR_HOME; g_dirty = true; }
                else { g_setEdit = true; g_dirty = true; }
            }
            if (b == BTN_LONG) { g_screen = SCR_HOME; g_dirty = true; }
        }
        if (g_dirty) { drawSettings(); g_dirty = false; }
    }

    delay(3);
}
