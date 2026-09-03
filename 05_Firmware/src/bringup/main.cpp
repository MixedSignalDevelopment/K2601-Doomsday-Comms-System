/* ===========================================================================
 *  SPDX-License-Identifier: MIT
 *  Copyright (c) 2026 Mixed Signal Development GmbH
 *
 *  Doomsday Comms System (D.C.S., SKU OS2601) — bring-up tester (per-peripheral)
 *
 *  On-LCD menu to exercise each part of the board individually. Navigate with
 *  the wheel or the RTT keyboard.
 *
 *  Controls (rev-A wheel, post-bodge mapping):
 *     roll  = move / adjust      press = select      hold = back
 *  RTT keys: w/s = up/down, Enter/space = select, b = back
 * =========================================================================== */
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <U8g2lib.h>
#include <Adafruit_DRV2605.h>
#include <RadioLib.h>
#include "nrf_gpio.h"
#include "rtt.h"

#define DISP_ARGS U8G2_R0, PIN_DISPLAY_SCK, PIN_DISPLAY_MOSI, \
                  PIN_DISPLAY_CS_N, PIN_DISPLAY_RS, PIN_DISPLAY_RES
U8G2_ST7565_ERC12864_F_4W_SW_SPI display(DISP_ARGS);

Adafruit_DRV2605 drv;
SX1262 radio = new Module(PIN_LORA_CS_N, PIN_DIO1, PIN_LORA_RST_N, PIN_BUSY);

static uint8_t  g_contrast   = 24;
static uint8_t  g_backlight  = 210;
static uint8_t  g_hapticFx   = 1;
static uint16_t g_buzFreq    = 4000;
static bool     g_drvReady   = false;
static bool     g_radioReady = false;
static int      g_radioState = 0;

/* ---- input: press=P0.12, roll R=P0.07, roll L=P0.13 ---------------------- */
#define PIN_PRESS   PIN_WHEEL_R
#define PIN_ROLL_R  PIN_PUSH
#define PIN_ROLL_L  PIN_WHEEL_L

enum WheelEv { WH_NONE, WH_DOWN, WH_UP };
enum BtnEv   { BTN_NONE, BTN_SHORT, BTN_LONG };
static bool g_pressHeld = false;

static bool rawR() { return digitalRead(PIN_WHEEL_R) == LOW; }  /* P0.12 (press) */
static bool rawL() { return digitalRead(PIN_WHEEL_L) == LOW; }  /* P0.13 (roll L) */
static bool rawP() { return digitalRead(PIN_PUSH)    == LOW; }  /* P0.07 (roll R) */

static void pollInputs(WheelEv &w, BtnEv &b)
{
    w = WH_NONE; b = BTN_NONE;
    const uint32_t DEB = 12, LONG_MS = 650;
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
    g_pressHeld = (p == LOW);
}

/* ---- feedback ------------------------------------------------------------ */
static void backlight(uint8_t d) { analogWrite(PIN_DISPLAY_PWM, d); }
static void haptic(uint8_t fx)
{
    if (!g_drvReady) return;
    drv.setWaveform(0, fx); drv.setWaveform(1, 0); drv.go();
}
static void beep(uint16_t f, uint16_t ms)
{
    pinMode(PIN_BUZ_MINUS, OUTPUT); digitalWrite(PIN_BUZ_MINUS, LOW);
    tone(PIN_BUZ_PLUS, f); delay(ms);
    noTone(PIN_BUZ_PLUS); digitalWrite(PIN_BUZ_PLUS, LOW);
}

/* ---- small UI helpers ---------------------------------------------------- */
static void header(const char *t) { display.setFont(u8g2_font_6x12_tr); display.drawStr(0, 10, t); display.drawHLine(0, 12, 128); }
static void footer(const char *t) { display.setFont(u8g2_font_5x7_tr); display.drawStr(0, 63, t); }

static void drawList(const char *title, const char *const *items, int n, int sel, const char *foot)
{
    display.clearBuffer();
    display.drawFrame(0, 0, 128, 64);
    header(title);
    display.setFont(u8g2_font_6x10_tr);
    const int rows = 4, rowH = 11, top = 16;
    int first = sel - rows + 1; if (first < 0) first = 0;
    if (first > n - rows) first = (n > rows) ? n - rows : 0;
    for (int i = 0; i < rows && first + i < n; i++) {
        int idx = first + i, y = top + i * rowH;
        if (idx == sel) { display.drawBox(0, y - 9, 128, rowH); display.setDrawColor(0); display.drawStr(2, y, items[idx]); display.setDrawColor(1); }
        else display.drawStr(2, y, items[idx]);
    }
    footer(foot);
    display.sendBuffer();
}
static void drawBar(const char *title, const char *label, int val, int vmax, const char *hint)
{
    display.clearBuffer(); header(title);
    display.setFont(u8g2_font_6x10_tr); display.drawStr(0, 30, label);
    display.drawFrame(0, 36, 128, 12);
    int wv = (int)((long)val * 126 / (vmax ? vmax : 1)); if (wv > 126) wv = 126; if (wv < 0) wv = 0;
    display.drawBox(1, 37, wv, 10);
    footer(hint); display.sendBuffer();
}

/* ---- buzzer sweep / loud ------------------------------------------------- */
static void buzzerSweep()
{
    pinMode(PIN_BUZ_MINUS, OUTPUT); digitalWrite(PIN_BUZ_MINUS, LOW);
    for (int f = 3600; f <= 4400; f += 50) {
        tone(PIN_BUZ_PLUS, f);
        display.clearBuffer(); header("Buzzer sweep");
        display.setFont(u8g2_font_6x10_tr); char t[16]; snprintf(t, sizeof(t), "%d Hz", f);
        display.drawStr(0, 34, t); display.sendBuffer(); delay(90);
    }
    noTone(PIN_BUZ_PLUS); digitalWrite(PIN_BUZ_PLUS, LOW);
}
static void buzzerLoud()
{
    const uint32_t freq = 4000, base = 16000000UL; uint16_t top = (uint16_t)(base / freq);
    static uint16_t seq[2]; seq[0] = top / 2; seq[1] = (top / 2) | 0x8000;
    NRF_PWM_Type *pwm = NRF_PWM2;
    pwm->PSEL.OUT[0] = g_ADigitalPinMap[PIN_BUZ_PLUS]; pwm->PSEL.OUT[1] = g_ADigitalPinMap[PIN_BUZ_MINUS];
    pwm->ENABLE = 1; pwm->MODE = 0; pwm->PRESCALER = 0; pwm->COUNTERTOP = top; pwm->LOOP = 0;
    pwm->DECODER = (PWM_DECODER_LOAD_Individual << PWM_DECODER_LOAD_Pos) | (PWM_DECODER_MODE_RefreshCount << PWM_DECODER_MODE_Pos);
    pwm->SEQ[0].PTR = (uint32_t)seq; pwm->SEQ[0].CNT = 2; pwm->SEQ[0].REFRESH = 0; pwm->SEQ[0].ENDDELAY = 0;
    pwm->TASKS_SEQSTART[0] = 1; delay(900);
    pwm->TASKS_STOP = 1; pwm->ENABLE = 0;
    pinMode(PIN_BUZ_PLUS, OUTPUT); digitalWrite(PIN_BUZ_PLUS, LOW);
    pinMode(PIN_BUZ_MINUS, OUTPUT); digitalWrite(PIN_BUZ_MINUS, LOW);
}

static float readBatteryMv()
{
    analogReference(AR_INTERNAL); analogReadResolution(12); analogOversampling(8);
    uint32_t acc = 0; for (int i = 0; i < 8; i++) { acc += analogRead(PIN_VBAT_MON); delay(1); }
    return (acc / 8.0f) / 4095.0f * 3600.0f * VBAT_DIVIDER;
}
static void radioInit()
{
    SPI.begin();
    g_radioState = radio.begin(868.0, 125.0, 9, 7, 0x2B, 14, 8, 0.0, false);
    if (g_radioState == RADIOLIB_ERR_NONE) { radio.setDio2AsRfSwitch(true); radio.standby(); g_radioReady = true; }
    else g_radioReady = false;
}

/* ===========================================================================
 *  Screens
 * =========================================================================== */
enum Screen { SCR_MENU, SCR_DISPLAY, SCR_BACKLIGHT, SCR_WHEEL, SCR_HAPTIC,
              SCR_BUZ_TONE, SCR_BUZ_SWEEP, SCR_BUZ_LOUD, SCR_BATTERY, SCR_RADIO,
              SCR_DIAG, SCR_SLEEP };
static Screen g_screen = SCR_MENU;
static int    g_sel = 0;
static bool   g_dirty = true;

static const char *kMenu[] = {
    "Display + contrast", "Backlight", "Wheel test", "Haptic (LRA)",
    "Buzzer tone", "Buzzer sweep", "Buzzer LOUD", "Battery",
    "Radio status", "Input diagnostic", "Sleep (screen)"
};
static const Screen kScreen[] = {
    SCR_DISPLAY, SCR_BACKLIGHT, SCR_WHEEL, SCR_HAPTIC, SCR_BUZ_TONE,
    SCR_BUZ_SWEEP, SCR_BUZ_LOUD, SCR_BATTERY, SCR_RADIO, SCR_DIAG, SCR_SLEEP
};
static const int kMenuN = sizeof(kMenu) / sizeof(kMenu[0]);

static void enterScreen(Screen s) { g_screen = s; g_dirty = true; if (s == SCR_RADIO && !g_radioReady) radioInit(); }

static void hMenu(WheelEv w, BtnEv b)
{
    if (w == WH_DOWN) { g_sel = (g_sel + 1) % kMenuN; g_dirty = true; }
    if (w == WH_UP)   { g_sel = (g_sel - 1 + kMenuN) % kMenuN; g_dirty = true; }
    if (b == BTN_SHORT) enterScreen(kScreen[g_sel]);
    static uint32_t t = 0; if (millis() - t > 150) { t = millis(); g_dirty = true; }
    if (g_dirty) {
        char foot[26]; snprintf(foot, sizeof(foot), "raw P%d Rr%d Rl%d  push=go", rawR(), rawP(), rawL());
        drawList("DCS bring-up", kMenu, kMenuN, g_sel, foot);
        g_dirty = false;
    }
}
static void hDisplay(WheelEv w, BtnEv b)
{
    static uint8_t pat = 0;
    if (w == WH_UP   && g_contrast < 63) { g_contrast++; g_dirty = true; }
    if (w == WH_DOWN && g_contrast > 0)  { g_contrast--; g_dirty = true; }
    if (b == BTN_SHORT) { pat = (pat + 1) % 4; g_dirty = true; }
    if (b == BTN_LONG)  { enterScreen(SCR_MENU); return; }
    if (!g_dirty) return; g_dirty = false;
    display.setContrast(g_contrast); display.clearBuffer();
    switch (pat) {
        case 0: header("Display test"); display.setFont(u8g2_font_6x10_tr);
                display.drawStr(0, 30, "ST7565 128x64");
                { char t[20]; snprintf(t, sizeof(t), "contrast %d", g_contrast); display.drawStr(0, 44, t); }
                display.drawBox(0, 0, 3, 3); display.drawBox(125, 0, 3, 3);
                display.drawBox(0, 61, 3, 3); display.drawBox(125, 61, 3, 3);
                footer("roll=contrast push=pat"); break;
        case 1: display.drawBox(0, 0, 128, 64); break;
        case 2: for (int y = 0; y < 64; y += 2) for (int x = (y % 4) ? 0 : 2; x < 128; x += 4) display.drawPixel(x, y); break;
        case 3: for (int x = 0; x < 128; x += 4) display.drawVLine(x, 0, 64); break;
    }
    display.sendBuffer();
}
static void hBacklight(WheelEv w, BtnEv b)
{
    if (w == WH_UP)   { g_backlight = (g_backlight <= 235) ? g_backlight + 20 : 255; g_dirty = true; }
    if (w == WH_DOWN) { g_backlight = (g_backlight >= 20) ? g_backlight - 20 : 0;   g_dirty = true; }
    if (b == BTN_LONG) { enterScreen(SCR_MENU); return; }
    if (!g_dirty) return; g_dirty = false;
    backlight(g_backlight);
    char t[20]; snprintf(t, sizeof(t), "duty %d/255", g_backlight);
    drawBar("Backlight", t, g_backlight, 255, "roll=level  hold=back");
}
static void hWheel(WheelEv w, BtnEv b)
{
    static int cU = 0, cD = 0, cP = 0; static const char *last = "roll / press it";
    if (w == WH_UP)     { cU++; last = "ROLL L (up)";   g_dirty = true; }
    if (w == WH_DOWN)   { cD++; last = "ROLL R (down)"; g_dirty = true; }
    if (b == BTN_SHORT) { cP++; last = "PRESS";         g_dirty = true; }
    if (b == BTN_LONG)  { enterScreen(SCR_MENU); return; }
    static uint32_t t = 0; if (millis() - t > 120) { t = millis(); g_dirty = true; }
    if (!g_dirty) return; g_dirty = false;
    display.clearBuffer(); header("Wheel test"); display.setFont(u8g2_font_6x10_tr);
    char l1[24], l2[24];
    snprintf(l1, sizeof(l1), "up:%d dn:%d press:%d", cU, cD, cP);
    snprintf(l2, sizeof(l2), "raw P%d Rr%d Rl%d %s", rawR(), rawP(), rawL(), g_pressHeld ? "hold" : "");
    display.drawStr(0, 28, l1); display.drawStr(0, 42, last); display.drawStr(0, 54, l2);
    footer("hold = back"); display.sendBuffer();
}
static void hHaptic(WheelEv w, BtnEv b)
{
    if (w == WH_UP   && g_hapticFx < 123) { g_hapticFx++; g_dirty = true; }
    if (w == WH_DOWN && g_hapticFx > 1)   { g_hapticFx--; g_dirty = true; }
    if (b == BTN_SHORT) { haptic(g_hapticFx); g_dirty = true; }
    if (b == BTN_LONG)  { enterScreen(SCR_MENU); return; }
    if (!g_dirty) return; g_dirty = false;
    display.clearBuffer(); header("Haptic (LRA)"); display.setFont(u8g2_font_6x10_tr);
    char t[24];
    if (g_drvReady) { snprintf(t, sizeof(t), "ROM effect: %d", g_hapticFx); display.drawStr(0, 30, t); display.drawStr(0, 44, "push=play roll=effect"); }
    else { display.drawStr(0, 30, "DRV2605 NOT FOUND"); display.drawStr(0, 44, "check I2C 0x5A / EN"); }
    footer("hold = back"); display.sendBuffer();
}
static void hBuzTone(WheelEv w, BtnEv b)
{
    if (w == WH_UP   && g_buzFreq < 5000) { g_buzFreq += 100; g_dirty = true; }
    if (w == WH_DOWN && g_buzFreq > 2000) { g_buzFreq -= 100; g_dirty = true; }
    if (b == BTN_SHORT) { beep(g_buzFreq, 200); g_dirty = true; }
    if (b == BTN_LONG)  { enterScreen(SCR_MENU); return; }
    if (!g_dirty) return; g_dirty = false;
    char t[16]; snprintf(t, sizeof(t), "%d Hz", g_buzFreq);
    drawBar("Buzzer tone", t, g_buzFreq - 2000, 3000, "roll=freq push=beep");
}
static void hBuzSweep(WheelEv, BtnEv b)
{
    if (g_dirty) { display.clearBuffer(); header("Buzzer sweep"); display.setFont(u8g2_font_6x10_tr);
                   display.drawStr(0, 30, "push = 3.6-4.4 kHz"); footer("hold = back"); display.sendBuffer(); g_dirty = false; }
    if (b == BTN_SHORT) { buzzerSweep(); g_dirty = true; }
    if (b == BTN_LONG)  enterScreen(SCR_MENU);
}
static void hBuzLoud(WheelEv, BtnEv b)
{
    if (g_dirty) { display.clearBuffer(); header("Buzzer LOUD"); display.setFont(u8g2_font_6x10_tr);
                   display.drawStr(0, 30, "push = 4kHz antiphase"); footer("hold = back"); display.sendBuffer(); g_dirty = false; }
    if (b == BTN_SHORT) { buzzerLoud(); g_dirty = true; }
    if (b == BTN_LONG)  enterScreen(SCR_MENU);
}
static void hBattery(WheelEv, BtnEv b)
{
    if (b == BTN_LONG) { enterScreen(SCR_MENU); return; }
    static uint32_t t = 0; if (millis() - t < 400 && !g_dirty) return; t = millis(); g_dirty = false;
    float mv = readBatteryMv();
    const char *st = mv >= 4150 ? "full" : mv >= 3500 ? "ok" : mv >= 3300 ? "WARN" : "EMPTY";
    display.clearBuffer(); header("Battery"); display.setFont(u8g2_font_6x12_tr);
    char t2[16]; snprintf(t2, sizeof(t2), "%d mV", (int)mv); display.drawStr(0, 30, t2);
    display.setFont(u8g2_font_6x10_tr); display.drawStr(72, 29, st);
    int pct = (int)((mv - 3300) * 100 / 900); if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    display.drawFrame(0, 38, 128, 12); display.drawBox(1, 39, pct * 126 / 100, 10);
    footer("hold = back"); display.sendBuffer();
}
static void hRadio(WheelEv, BtnEv b)
{
    if (b == BTN_SHORT && g_radioReady) {
        display.clearBuffer(); header("Radio TX"); display.setFont(u8g2_font_6x10_tr);
        display.drawStr(0, 28, "ANTENNA attached?"); display.drawStr(0, 42, "transmitting...");
        display.sendBuffer();
        g_radioState = radio.transmit("DCS hello"); radio.standby(); g_dirty = true;
    }
    if (b == BTN_LONG) { enterScreen(SCR_MENU); return; }
    if (!g_dirty) return; g_dirty = false;
    display.clearBuffer(); header("Radio (SX1262)"); display.setFont(u8g2_font_6x10_tr);
    if (g_radioReady) { display.drawStr(0, 28, "SPI link: OK EU868"); display.drawStr(0, 42, "push=TX (need antenna)"); }
    else { char t[22]; snprintf(t, sizeof(t), "begin fail: %d", g_radioState); display.drawStr(0, 28, "SPI link: FAIL"); display.drawStr(0, 42, t); }
    footer("hold = back"); display.sendBuffer();
}
static void hDiag(WheelEv, BtnEv b)
{
    if (b == BTN_LONG) { enterScreen(SCR_MENU); return; }
    static uint8_t hist[4] = {0}; static uint8_t lastNZ = 0xFF; static int cA = 0, cB = 0, cC = 0, la = 1, lb = 1, lc = 1;
    int A = rawR(), B = rawL(), C = rawP();
    if (A && !la) cA++; if (B && !lb) cB++; if (C && !lc) cC++;
    la = A; lb = B; lc = C;
    uint8_t pat = A | (B << 1) | (C << 2);
    if (pat == 0) lastNZ = 0xFF;
    else if (pat != lastNZ) { lastNZ = pat; hist[0] = hist[1]; hist[1] = hist[2]; hist[2] = hist[3]; hist[3] = pat; }
    static uint32_t t = 0; if (millis() - t < 60) return; t = millis();
    display.clearBuffer(); display.drawFrame(0, 0, 128, 64); display.setFont(u8g2_font_6x10_tr);
    display.drawStr(3, 11, "INPUT DIAG (P0.12/13/07)");
    char b2[28]; snprintf(b2, sizeof(b2), "now  %d %d %d", A, B, C); display.drawStr(3, 24, b2);
    snprintf(b2, sizeof(b2), "cnt  %d %d %d", cA, cB, cC); display.drawStr(3, 36, b2);
    display.drawStr(3, 48, "seen:");
    for (int i = 0; i < 4; i++) { char s[4]; s[0] = (hist[i] & 1) ? '1' : '.'; s[1] = (hist[i] & 2) ? '2' : '.'; s[2] = (hist[i] & 4) ? '7' : '.'; s[3] = 0; display.drawStr(36 + i * 23, 48, s); }
    display.drawStr(3, 61, "hold = back"); display.sendBuffer();
}
static void hSleep(WheelEv, BtnEv b)
{
    if (g_dirty) { display.clearBuffer(); header("Sleep"); display.setFont(u8g2_font_6x10_tr);
                   display.drawStr(0, 30, "push = screen off"); footer("wake: press again"); display.sendBuffer(); g_dirty = false; }
    if (b == BTN_LONG) { enterScreen(SCR_MENU); return; }
    if (b == BTN_SHORT) {
        backlight(0); display.setPowerSave(1);
        while (!rawR()) delay(10);                 /* wait for press (P0.12) */
        while (rawR()) delay(10);
        display.setPowerSave(0); backlight(g_backlight); g_dirty = true;
    }
}

/* ===========================================================================
 *  Fault reporter + I2C recovery
 * =========================================================================== */
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

/* ===========================================================================
 *  setup / loop
 * =========================================================================== */
void setup()
{
    /* internal RC low-freq clock (no external 32k crystal on this module) */
    NRF_CLOCK->TASKS_LFCLKSTOP = 1;
    NRF_CLOCK->LFCLKSRC = (CLOCK_LFCLKSRC_SRC_RC << CLOCK_LFCLKSRC_SRC_Pos);
    NRF_CLOCK->EVENTS_LFCLKSTARTED = 0; NRF_CLOCK->TASKS_LFCLKSTART = 1;
    for (volatile uint32_t i = 0; i < 2000000 && !NRF_CLOCK->EVENTS_LFCLKSTARTED; i++) { }

    NRF_POWER->DCDCEN = 1;
    Rtt.begin(); Rtt.println(F("DCS bring-up tester"));

    pinMode(PIN_ROLL_R, INPUT_PULLUP); pinMode(PIN_ROLL_L, INPUT_PULLUP); pinMode(PIN_PRESS, INPUT_PULLUP);

    display.begin(); display.setContrast(g_contrast);
    pinMode(PIN_DISPLAY_PWM, OUTPUT); backlight(g_backlight);
    display.clearBuffer(); display.setFont(u8g2_font_6x12_tr);
    display.drawStr(20, 30, "DCS tester"); display.sendBuffer();

    pinMode(PIN_LRA_EN, OUTPUT); digitalWrite(PIN_LRA_EN, HIGH);
    delay(2); i2cRecover(); Wire.begin(); Wire.setClock(100000); delay(3);
    if (drv.begin()) { g_drvReady = true; drv.useLRA(); drv.selectLibrary(6); drv.setMode(DRV2605_MODE_INTTRIG); Rtt.println(F("DRV2605 OK")); }
    else Rtt.println(F("DRV2605 not found"));

    delay(600);
    g_screen = SCR_MENU; g_dirty = true;
    Rtt.println(F("menu ready (RTT: w/s Enter b)"));
}

void loop()
{
    WheelEv w; BtnEv b; pollInputs(w, b);
    if (Rtt.available()) {
        char c = (char)Rtt.read();
        if (c == 'w' || c == 'k') w = WH_UP;
        else if (c == 's' || c == 'j') w = WH_DOWN;
        else if (c == '\r' || c == '\n' || c == ' ') b = BTN_SHORT;
        else if (c == 'b') b = BTN_LONG;
    }
    switch (g_screen) {
        case SCR_MENU:      hMenu(w, b);      break;
        case SCR_DISPLAY:   hDisplay(w, b);   break;
        case SCR_BACKLIGHT: hBacklight(w, b); break;
        case SCR_WHEEL:     hWheel(w, b);     break;
        case SCR_HAPTIC:    hHaptic(w, b);    break;
        case SCR_BUZ_TONE:  hBuzTone(w, b);   break;
        case SCR_BUZ_SWEEP: hBuzSweep(w, b);  break;
        case SCR_BUZ_LOUD:  hBuzLoud(w, b);   break;
        case SCR_BATTERY:   hBattery(w, b);   break;
        case SCR_RADIO:     hRadio(w, b);     break;
        case SCR_DIAG:      hDiag(w, b);      break;
        case SCR_SLEEP:     hSleep(w, b);     break;
    }
    delay(3);
}
