/*
 * ============================================================
 *  BambuTagger — ESP32 + RC522 + SH110X OLED + Rotary Encoder
 * ============================================================
 *  Read, clone and write Bambu Lab filament spool RFID tags.
 *  Dump library:  https://github.com/queengooborg/Bambu-Lab-RFID-Library
 *  Tag format:    https://github.com/queengooborg/Bambu-Lab-RFID-Tag-Guide
 *
 *  KEY DERIVATION (KDF):
 *    Keys = HKDF-SHA256(IKM=UID[4], salt=BAMBU_KDF_SALT[16],
 *                       info="RFID-A\0" or "RFID-B\0", L=96)
 *    → 16 × 6-byte sector keys  (one per MIFARE sector)
 *
 *  HARDWARE WIRING:
 *  ┌──────────────────────────────────────────────────────────┐
 *  │  RC522 (SPI)                                             │
 *  │    SDA/CS → GPIO  5    SCK  → GPIO 18                    │
 *  │    MOSI   → GPIO 23    MISO → GPIO 19                    │
 *  │    RST    → GPIO 27    3V3  / GND                        │
 *  ├──────────────────────────────────────────────────────────┤
 *  │  SH110X OLED (I2C 128×64)                                │
 *  │    SDA → GPIO 21   SCL → GPIO 22   3V3 / GND             │
 *  ├──────────────────────────────────────────────────────────┤
 *  │  Rotary Encoder (e.g. KY-040)                            │
 *  │    CLK → GPIO 34   DT  → GPIO 35   BTN → GPIO 32         │
 *  │    VCC → 3V3       GND → GND                             │
 *  │    NOTE: GPIO 34/35 are input-only – no internal         │
 *  │          pull-ups. KY-040 modules supply their own.      │
 *  ├──────────────────────────────────────────────────────────┤
 *  │  WS2812B RGB LED (1 - 3 pixel)                           │
 *  │    DIN → GPIO 26   VCC → 3V3   GND → GND                 │
 *  │    Shows the filament colour after a successful read,    │
 *  │    white while scanning, green on write OK, red on fail  │
 *  └──────────────────────────────────────────────────────────┘
 *
 *  REQUIRED LIBRARIES (Arduino Library Manager):
 *    • MFRC522            (miguelbalboa)
 *    • Adafruit SH110X
 *    • Adafruit GFX Library
 *    • Adafruit NeoPixel  (Adafruit)
 *    • ArduinoJson        (Benoit Blanchon)
 *    mbedTLS is bundled with the ESP32 Arduino core.
 *
 *  WRITE OPERATIONS NEED A "MAGIC" / UID-CHANGEABLE MIFARE CARD
 *    (CUID / FUID / Gen2) so that block 0 (UID) can be written.
 *    Plain factory MIFARE Classic 1K cards keep their UID fixed
 *    and will only have blocks 1-63 updated.
 *
 *  CLONE vs WRITE-FROM-DUMP:
 *    • Clone     – read a live Bambu tag, write raw data to
 *                  a blank magic card (preserving UID).
 *    • Write Dump– download a pre-scanned .bin from GitHub
 *                  via the built-in web interface or directly from 
 *                  the OLED menu, store on SPIFFS,
 *                  then write to a card via encoder menu.
 * ============================================================
 */

// ──────────────────────────────────────────────────────────────
//  Includes
// ──────────────────────────────────────────────────────────────
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Adafruit_NeoPixel.h>

// ── Debug output ──────────────────────────────────────────────
//  Set DEBUG_SERIAL to 0 to strip all debug prints from the build.
#define DEBUG_SERIAL 1
#if DEBUG_SERIAL
  #define DBG(...)     Serial.print(__VA_ARGS__)
  #define DBGLN(...)   Serial.println(__VA_ARGS__)
  #define DBGF(...)    Serial.printf(__VA_ARGS__)
#else
  #define DBG(...)
  #define DBGLN(...)
  #define DBGF(...)
#endif
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include "mbedtls/md.h"
#include <vector>

// ──────────────────────────────────────────────────────────────
//  Pin definitions
// ──────────────────────────────────────────────────────────────
#define PIN_RFID_CS      5
#define PIN_RFID_RST     27
#define PIN_OLED_SDA     21
#define PIN_OLED_SCL     22
#define PIN_ENC_CLK      34
#define PIN_ENC_DT       35
#define PIN_ENC_BTN      32
#define PIN_LED_WS2812   26   // WS2812B data-in pin

// ──────────────────────────────────────────────────────────────
//  Constants
// ──────────────────────────────────────────────────────────────
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define SCREEN_ADDR   0x3C

#define MIFARE_BLOCKS      64      // MIFARE Classic 1K
#define BYTES_PER_BLOCK    16
#define DUMP_SIZE          (MIFARE_BLOCKS * BYTES_PER_BLOCK)   // 1024
#define NUM_SECTORS        16
#define BLOCKS_PER_SECTOR   4

#define AP_SSID   "BambuTagger"
#define AP_PASS   "bambu1234"

#define GITHUB_API_HOST   "api.github.com"
#define GITHUB_RAW_HOST   "raw.githubusercontent.com"
#define GITHUB_REPO_PATH  "/queengooborg/Bambu-Lab-RFID-Library"
#define GITHUB_RAW_PREFIX "https://raw.githubusercontent.com/queengooborg/Bambu-Lab-RFID-Library/main/"

// Bambu Lab HKDF salt (from reverse-engineered KDF)
static const uint8_t BAMBU_KDF_SALT[16] = {
    0x9a, 0x75, 0x9c, 0xf2, 0xc4, 0xf7, 0xca, 0xff,
    0x22, 0x2c, 0xb9, 0x76, 0x9b, 0x41, 0xbc, 0x96
};

// ──────────────────────────────────────────────────────────────
//  Tag data
// ──────────────────────────────────────────────────────────────
struct TagInfo {
    uint8_t  uid[4];
    char     filamentType[17];   // block 2
    char     detailedType[17];   // block 4
    char     variantId[9];       // block 1 bytes 0-7
    char     materialId[9];      // block 1 bytes 8-15
    uint8_t  colorR, colorG, colorB;
    uint16_t spoolWeight;        // grams
    float    diameter;           // mm
    uint16_t minNozzleTemp;      // °C
    uint16_t maxNozzleTemp;      // °C
    uint16_t bedTemp;            // °C
    uint16_t dryTemp;            // °C
    uint16_t dryTime;            // hours
    uint16_t filamentLength;     // metres
    uint8_t  raw[MIFARE_BLOCKS][BYTES_PER_BLOCK];
    bool     valid;
};

TagInfo  currentTag;    // most recently read
TagInfo  sourceTag;     // for clone operation
uint8_t  dumpBuf[DUMP_SIZE];
char     selectedDumpPath[64] = "";

// Pages to display for a read tag
int tagPage = 0;
static const int TAG_PAGES = 4;

// ──────────────────────────────────────────────────────────────
//  Global objects
// ──────────────────────────────────────────────────────────────
MFRC522          rfid(PIN_RFID_CS, PIN_RFID_RST);
Adafruit_SH1106G oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Adafruit_NeoPixel statusLed(3, PIN_LED_WS2812, NEO_GRB + NEO_KHZ800);
WebServer        httpServer(80);
Preferences      prefs;

// ──────────────────────────────────────────────────────────────
//  Application state machine
// ──────────────────────────────────────────────────────────────
enum AppState {
    S_MAIN_MENU,
    S_READ_TAG,
    S_SHOW_TAG,
    S_CLONE_SOURCE,
    S_CLONE_TARGET,
    S_DUMP_SELECT,
    S_DUMP_WRITE,
    S_WIFI_INFO,
    S_GH_BROWSE,       // GitHub OLED browser
    S_GH_DOWNLOAD      // downloading dump file to SPIFFS
};
AppState appState = S_MAIN_MENU;

// ──────────────────────────────────────────────────────────────
//  Menu
// ──────────────────────────────────────────────────────────────
static const char* MENU_ITEMS[] = {
    "1 Read Tag",
    "2 Clone Tag",
    "3 Write Dump",
    "4 GitHub Lib",
    "5 WiFi / Web"
};
static const int MENU_COUNT = 5;
int menuSel    = 0;
int menuScroll = 0;

// ──────────────────────────────────────────────────────────────
//  Rotary encoder (polling, software debounce)
// ──────────────────────────────────────────────────────────────
int  encClkLast   = HIGH;
int  encDelta     = 0;           // +1 CW, -1 CCW, cleared after read
bool encBtnClick  = false;       // true for one frame after release
int  encBtnLast   = HIGH;
int  encBtnState  = HIGH;
unsigned long encBtnDebounce = 0;

void encUpdate() {
    // Rotation
    int clk = digitalRead(PIN_ENC_CLK);
    if (clk != encClkLast) {
        if (clk == LOW) {                          // falling edge
            int dt = digitalRead(PIN_ENC_DT);
            encDelta += (dt == HIGH) ? 1 : -1;
        }
        encClkLast = clk;
    }
    // Button with 50 ms debounce
    int btn = digitalRead(PIN_ENC_BTN);
    if (btn != encBtnLast) {
        encBtnDebounce = millis();
        encBtnLast = btn;
    }
    if ((millis() - encBtnDebounce) > 50 && btn != encBtnState) {
        encBtnState = btn;
        if (encBtnState == LOW) encBtnClick = true; // pressed
    }
}

// Consume and return rotation delta since last call
int encGetDelta() {
    int d = encDelta;
    encDelta = 0;
    return d;
}
// Consume and return button click
bool encGetClick() {
    if (encBtnClick) { encBtnClick = false; return true; }
    return false;
}

// ──────────────────────────────────────────────────────────────
//  HKDF-SHA256  (RFC 5869)
// ──────────────────────────────────────────────────────────────
static void hmacSHA256(const uint8_t* key,  size_t kLen,
                       const uint8_t* data, size_t dLen,
                       uint8_t* out32)
{
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_setup(&ctx, md, 1);
    mbedtls_md_hmac_starts(&ctx, key, kLen);
    mbedtls_md_hmac_update(&ctx, data, dLen);
    mbedtls_md_hmac_finish(&ctx, out32);
    mbedtls_md_free(&ctx);
}

// Generate `okmLen` bytes of keying material
static void hkdf256(const uint8_t* ikm,  size_t ikmLen,
                    const uint8_t* salt, size_t saltLen,
                    const uint8_t* info, size_t infoLen,
                    uint8_t* okm, size_t okmLen)
{
    // Extract
    uint8_t prk[32];
    hmacSHA256(salt, saltLen, ikm, ikmLen, prk);

    // Expand
    uint8_t T[32] = {0};
    size_t  tLen  = 0;
    uint8_t ctr   = 0;
    size_t  done  = 0;

    while (done < okmLen) {
        ctr++;
        // input = T(i-1) || info || ctr
        size_t   inLen  = tLen + infoLen + 1;
        uint8_t* input  = (uint8_t*)malloc(inLen);
        if (!input) return;
        if (tLen) memcpy(input, T, tLen);
        memcpy(input + tLen, info, infoLen);
        input[tLen + infoLen] = ctr;

        hmacSHA256(prk, 32, input, inLen, T);
        free(input);
        tLen = 32;

        size_t n = min((size_t)32, okmLen - done);
        memcpy(okm + done, T, n);
        done += n;
    }
}

/* Derive all 16 A-keys and 16 B-keys from a 4-byte UID.
   keysA[s][0..5] = sector-s Key-A
   keysB[s][0..5] = sector-s Key-B                         */
void bambuDeriveKeys(const uint8_t uid[4],
                     uint8_t keysA[16][6],
                     uint8_t keysB[16][6])
{
    // "RFID-A\0" and "RFID-B\0" – 7 bytes including the null
    static const uint8_t INFO_A[7] = {'R','F','I','D','-','A','\0'};
    static const uint8_t INFO_B[7] = {'R','F','I','D','-','B','\0'};

    uint8_t okm[96];

    hkdf256(uid, 4, BAMBU_KDF_SALT, 16, INFO_A, 7, okm, 96);
    for (int i = 0; i < 16; i++) memcpy(keysA[i], okm + i * 6, 6);

    hkdf256(uid, 4, BAMBU_KDF_SALT, 16, INFO_B, 7, okm, 96);
    for (int i = 0; i < 16; i++) memcpy(keysB[i], okm + i * 6, 6);
}

// ──────────────────────────────────────────────────────────────
//  Tag parsing helpers
// ──────────────────────────────────────────────────────────────
static void trimStr(char* s, int maxLen) {
    for (int i = maxLen - 1; i >= 0; i--) {
        if (s[i] == '\0' || s[i] == ' ') s[i] = '\0';
        else break;
    }
}

static void parseTagBlocks(TagInfo* t) {
    // Filament type  – block 2
    memcpy(t->filamentType, t->raw[2], 16);
    t->filamentType[16] = '\0';  trimStr(t->filamentType, 16);

    // Detailed type  – block 4
    memcpy(t->detailedType, t->raw[4], 16);
    t->detailedType[16] = '\0';  trimStr(t->detailedType, 16);

    // Variant ID     – block 1 bytes 0-7
    memcpy(t->variantId, t->raw[1], 8);
    t->variantId[8] = '\0';      trimStr(t->variantId, 8);

    // Material ID    – block 1 bytes 8-15
    memcpy(t->materialId, t->raw[1] + 8, 8);
    t->materialId[8] = '\0';     trimStr(t->materialId, 8);

    // Color (BGRA, block 5 bytes 0-3)
    t->colorB = t->raw[5][0];
    t->colorG = t->raw[5][1];
    t->colorR = t->raw[5][2];

    // Spool weight   – block 5 bytes 4-5 (little-endian uint16)
    t->spoolWeight = (uint16_t)t->raw[5][4] | ((uint16_t)t->raw[5][5] << 8);

    // Diameter       – block 5 bytes 8-11 (float LE)
    memcpy(&t->diameter, t->raw[5] + 8, 4);

    // Temperatures   – block 6
    t->dryTemp        = (uint16_t)t->raw[6][0]  | ((uint16_t)t->raw[6][1]  << 8);
    t->dryTime        = (uint16_t)t->raw[6][2]  | ((uint16_t)t->raw[6][3]  << 8);
    t->bedTemp        = (uint16_t)t->raw[6][6]  | ((uint16_t)t->raw[6][7]  << 8);
    t->maxNozzleTemp  = (uint16_t)t->raw[6][8]  | ((uint16_t)t->raw[6][9]  << 8);
    t->minNozzleTemp  = (uint16_t)t->raw[6][10] | ((uint16_t)t->raw[6][11] << 8);

    // Filament length – block 14 bytes 4-5
    t->filamentLength = (uint16_t)t->raw[14][4] | ((uint16_t)t->raw[14][5] << 8);
}

// Copy a TagInfo's raw blocks into a flat 1024-byte dump buffer
static void tagToFlat(const TagInfo* t, uint8_t* buf) {
    for (int b = 0; b < MIFARE_BLOCKS; b++)
        memcpy(buf + b * BYTES_PER_BLOCK, t->raw[b], BYTES_PER_BLOCK);
}

// Fill a TagInfo from a flat dump buffer
static void flatToTag(const uint8_t* buf, TagInfo* t) {
    memset(t, 0, sizeof(TagInfo));
    for (int b = 0; b < MIFARE_BLOCKS; b++)
        memcpy(t->raw[b], buf + b * BYTES_PER_BLOCK, BYTES_PER_BLOCK);
    memcpy(t->uid, buf, 4);
    parseTagBlocks(t);
    t->valid = true;
}

// ──────────────────────────────────────────────────────────────
//  RFID operations
// ──────────────────────────────────────────────────────────────
static bool tryAuth(int blockAddr, MFRC522::MIFARE_Key* key, bool useKeyA) {
    uint8_t cmd = useKeyA ? MFRC522::PICC_CMD_MF_AUTH_KEY_A
                          : MFRC522::PICC_CMD_MF_AUTH_KEY_B;
    bool ok = rfid.PCD_Authenticate(cmd, blockAddr, key, &rfid.uid) == MFRC522::STATUS_OK;
    DBGF("[AUTH]  blk=%02d key%s %02X%02X%02X%02X%02X%02X -> %s\n",
         blockAddr, useKeyA ? "A" : "B",
         key->keyByte[0], key->keyByte[1], key->keyByte[2],
         key->keyByte[3], key->keyByte[4], key->keyByte[5],
         ok ? "OK" : "FAIL");
    return ok;
}

/* Read all 64 blocks of a Bambu Lab tag.
   Authenticates each sector using derived keys, falls back to
   the default 0xFF…FF key for blank/overwritten sectors.
   Returns true if at least the first data sector was readable. */
bool rfidReadBambuTag(TagInfo* t) {
    memset(t, 0, sizeof(TagInfo));
    t->valid = false;

    if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial())
        return false;

    if (rfid.uid.size < 4) {
        rfid.PICC_HaltA();
        return false;
    }
    memcpy(t->uid, rfid.uid.uidByte, 4);
    DBGF("[RFID] UID: %02X %02X %02X %02X\n",
         t->uid[0], t->uid[1], t->uid[2], t->uid[3]);

    uint8_t keysA[16][6], keysB[16][6];
    bambuDeriveKeys(t->uid, keysA, keysB);
    DBGLN("[RFID] Key derivation complete.");

    MFRC522::MIFARE_Key mk;
    bool anyRead = false;

    for (int sec = 0; sec < NUM_SECTORS; sec++) {
        int trailer = sec * BLOCKS_PER_SECTOR + 3;

        // Build auth key objects
        MFRC522::MIFARE_Key kA, kB, kDef;
        memcpy(kA.keyByte,   keysA[sec], 6);
        memcpy(kB.keyByte,   keysB[sec], 6);
        memset(kDef.keyByte, 0xFF, 6);

        bool authed = tryAuth(trailer, &kA, true)
                   || tryAuth(trailer, &kB, false)
                   || tryAuth(trailer, &kDef, true);
        DBGF("[READ]  sector %02d auth -> %s\n", sec, authed ? "OK" : "FAIL");
        if (!authed) continue;

        for (int b = 0; b < BLOCKS_PER_SECTOR; b++) {
            int addr = sec * BLOCKS_PER_SECTOR + b;
            uint8_t buf[18]; uint8_t sz = 18;
            if (rfid.MIFARE_Read(addr, buf, &sz) == MFRC522::STATUS_OK) {
                memcpy(t->raw[addr], buf, BYTES_PER_BLOCK);
                anyRead = true;
                DBGF("[READ]    blk %02d: %02X %02X %02X %02X %02X %02X %02X %02X"
                     " %02X %02X %02X %02X %02X %02X %02X %02X\n", addr,
                     buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7],
                     buf[8],buf[9],buf[10],buf[11],buf[12],buf[13],buf[14],buf[15]);
            } else {
                DBGF("[READ]    blk %02d: read FAIL\n", addr);
            }
        }
    }

    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();

    if (!anyRead) {
        DBGLN("[READ] No blocks readable – aborting.");
        return false;
    }
    parseTagBlocks(t);
    t->valid = true;
    DBGF("[READ] Tag OK  type=%s  color=#%06X  wt=%.0fg\n",
         t->filamentType,
         ((uint32_t)t->colorR << 16) | ((uint32_t)t->colorG << 8) | t->colorB,
         t->spoolWeight);
    return true;
}

/* Write a 1024-byte dump to a card.
   Tries the default 0xFF key first (blank factory card), then
   the key embedded in the dump (for already-keyed sectors).
   Pass isMagicCard=true to also write block 0 (UID). */
bool rfidWriteDump(const uint8_t* buf, bool isMagicCard) {
    if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial())
        return false;

    MFRC522::MIFARE_Key kDef;
    memset(kDef.keyByte, 0xFF, 6);

    bool anyWritten = false;

    for (int sec = 0; sec < NUM_SECTORS; sec++) {
        int trailer = sec * BLOCKS_PER_SECTOR + 3;

        // Key A embedded in the dump's trailer block
        MFRC522::MIFARE_Key kDump;
        memcpy(kDump.keyByte, buf + trailer * BYTES_PER_BLOCK, 6);

        bool authed = tryAuth(trailer, &kDef,  true)
                   || tryAuth(trailer, &kDump, true);
        DBGF("[WRITE] sector %02d auth -> %s\n", sec, authed ? "OK" : "FAIL");
        if (!authed) continue;

        // Data blocks
        for (int b = 0; b < 3; b++) {
            int addr = sec * BLOCKS_PER_SECTOR + b;
            if (addr == 0 && !isMagicCard) continue;  // skip UID block on non-magic
            MFRC522::StatusCode ws = rfid.MIFARE_Write(
                addr, (uint8_t*)(buf + addr * BYTES_PER_BLOCK), BYTES_PER_BLOCK);
            DBGF("[WRITE]   blk %02d -> %s\n", addr,
                 ws == MFRC522::STATUS_OK ? "OK" : "FAIL");
            if (ws == MFRC522::STATUS_OK) anyWritten = true;
        }
        // Sector trailer (keys + access bits)
        {
            MFRC522::StatusCode ws = rfid.MIFARE_Write(
                trailer, (uint8_t*)(buf + trailer * BYTES_PER_BLOCK), BYTES_PER_BLOCK);
            DBGF("[WRITE]   trailer blk %02d -> %s\n", trailer,
                 ws == MFRC522::STATUS_OK ? "OK" : "FAIL");
        }
    }

    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    return anyWritten;
}

// ──────────────────────────────────────────────────────────────
//  OLED helpers
// ──────────────────────────────────────────────────────────────
static void oledClear()                    { oled.clearDisplay(); }
static void oledFlush()                    { oled.display(); }
static void oledText(int x, int y, int sz,
                     uint16_t color,
                     const char* msg)
{
    oled.setTextSize(sz);
    oled.setTextColor(color);
    oled.setCursor(x, y);
    oled.print(msg);
}

static void oledTitle(const char* title) {
    oled.setTextSize(1);
    oled.setTextColor(SH110X_WHITE);
    oled.setCursor(0, 0);
    oled.print(title);
    oled.drawFastHLine(0, 9, 128, SH110X_WHITE);
}

void showStatus(const char* msg) {
    oledClear();
    oled.setTextWrap(true);
    oledText(0, 18, 1, SH110X_WHITE, msg);
    oledFlush();
}

void showStatus2(const char* l1, const char* l2) {
    oledClear();
    oled.setTextWrap(true);
    oledText(0, 16, 1, SH110X_WHITE, l1);
    oledText(0, 30, 1, SH110X_WHITE, l2);
    oledFlush();
}

// ──────────────────────────────────────────────────────────────
//  WS2812B LED helpers
// ──────────────────────────────────────────────────────────────

// Set the single LED to an RGB colour at current brightness
static void ledSet(uint8_t r, uint8_t g, uint8_t b) {
    statusLed.setPixelColor(0, statusLed.Color(r, g, b));
    statusLed.setPixelColor(1, statusLed.Color(r, g, b));
    statusLed.setPixelColor(2, statusLed.Color(r, g, b));
    statusLed.show();
}

// Turn LED off
static void ledOff() { ledSet(0, 0, 0); }

// Show the filament colour from a parsed TagInfo
static void ledSetTagColor(const TagInfo* t) {
    ledSet(t->colorR, t->colorG, t->colorB);
}

// Flash a colour n times with 120 ms on / 120 ms off, then restore to `restoreR/G/B`
static void ledFlash(uint8_t r, uint8_t g, uint8_t b,
                     int n,
                     uint8_t restoreR = 0, uint8_t restoreG = 0, uint8_t restoreB = 0) {
    for (int i = 0; i < n; i++) {
        ledSet(r, g, b);   delay(120);
        ledOff();           delay(120);
    }
    ledSet(restoreR, restoreG, restoreB);
}

// Slow dim-blue pulse used while waiting for a card (call in a loop, non-blocking)
// Returns true on each completed up-down cycle
static bool ledScanPulse() {
    static uint8_t val     = 0;
    static int8_t  dir     = 4;
    static unsigned long last = 0;
    if (millis() - last < 18) return false;
    last = millis();
    val  = (uint8_t)constrain((int)val + dir, 0, 80);
    if (val == 80 || val == 0) dir = -dir;
    statusLed.setPixelColor(0, statusLed.Color(0, 0, val));
    statusLed.setPixelColor(1, statusLed.Color(0, 0, val));
    statusLed.setPixelColor(2, statusLed.Color(0, 0, val));
    statusLed.show();
    return (val == 0 && dir > 0);
}

// ──────────────────────────────────────────────────────────────
//  Draw main menu
// ──────────────────────────────────────────────────────────────
void drawMenu() {
    oledClear();
    oledTitle("BambuTagger");

    // WiFi indicator top-right
    oled.setTextSize(1);
    oled.setTextColor(SH110X_WHITE);
    oled.setCursor(96, 0);
    oled.print(WiFi.status() == WL_CONNECTED ? "WiFi" : "AP");

    // 3 visible items
    for (int i = 0; i < 3; i++) {
        int idx = menuScroll + i;
        if (idx >= MENU_COUNT) break;
        int y = 13 + i * 17;
        bool sel = (idx == menuSel);
        if (sel) {
            oled.fillRect(0, y - 1, 128, 15, SH110X_WHITE);
            oled.setTextColor(SH110X_BLACK);
        } else {
            oled.setTextColor(SH110X_WHITE);
        }
        oled.setTextSize(1);
        oled.setCursor(4, y);
        oled.print(MENU_ITEMS[idx]);
        oled.setTextColor(SH110X_WHITE);
    }

    // Scroll arrows
    if (menuScroll > 0) {
        oled.drawTriangle(122, 12, 118, 16, 126, 16, SH110X_WHITE);
    }
    if (menuScroll + 3 < MENU_COUNT) {
        oled.drawTriangle(122, 63, 118, 59, 126, 59, SH110X_WHITE);
    }

    oledFlush();
}

// ──────────────────────────────────────────────────────────────
//  Draw tag info (4 pages, navigate with encoder rotation)
// ──────────────────────────────────────────────────────────────
void drawTagInfo(const TagInfo* t, int page) {
    oledClear();

    char hdr[24];
    snprintf(hdr, sizeof(hdr), "Tag Info %d/%d", page + 1, TAG_PAGES);
    oledTitle(hdr);

    oled.setTextSize(1);
    oled.setTextColor(SH110X_WHITE);
    oled.setTextWrap(false);

    switch (page) {
        case 0:  // Identity
            oled.setCursor(0, 12);
            oled.printf("Type: %s\n", t->filamentType);
            oled.printf("Sub:  %s\n", t->detailedType);
            oled.printf("Var:  %s\n", t->variantId);
            oled.printf("UID:  %02X%02X%02X%02X",
                        t->uid[0], t->uid[1], t->uid[2], t->uid[3]);
            break;
        case 1:  // Physical
            oled.setCursor(0, 12);
            oled.printf("Color:  #%02X%02X%02X\n",
                        t->colorR, t->colorG, t->colorB);
            oled.printf("Weight: %dg\n",  t->spoolWeight);
            oled.printf("Diam:   %.2fmm\n", t->diameter);
            oled.printf("Length: %dm",    t->filamentLength);
            break;
        case 2:  // Temperatures
            oled.setCursor(0, 12);
            oled.printf("Nozzle:  %d-%dC\n",
                        t->minNozzleTemp, t->maxNozzleTemp);
            oled.printf("Bed:     %dC\n",  t->bedTemp);
            oled.printf("Dry:     %dC\n",  t->dryTemp);
            oled.printf("DryTime: %dh",    t->dryTime);
            break;
        case 3:  // IDs / help
            oled.setCursor(0, 12);
            oled.printf("MatID: %s\n",   t->materialId);
            oled.printf("UID:   %02X%02X%02X%02X\n",
                        t->uid[0], t->uid[1], t->uid[2], t->uid[3]);
            oled.print("\n");
            oled.print("Press=back");
            break;
    }

    // Page dots at bottom
    for (int i = 0; i < TAG_PAGES; i++) {
        int x = 52 + i * 8;
        if (i == page) oled.fillCircle(x, 60, 2, SH110X_WHITE);
        else           oled.drawCircle(x, 60, 2, SH110X_WHITE);
    }

    oledFlush();
}

// ──────────────────────────────────────────────────────────────
//  Draw dump-file selection list
// ──────────────────────────────────────────────────────────────
std::vector<String> dumpFiles;
int dumpSel = 0;

// ──────────────────────────────────────────────────────────────
//  GitHub OLED browser state
// ──────────────────────────────────────────────────────────────
#define GH_MAX_ENTRIES 48
struct GhEntry {
    char name[48];   // display name
    char path[128];  // repo-relative path (e.g. "PLA/PLA Basic/Black")
    bool isDir;
};
static GhEntry  ghEntries[GH_MAX_ENTRIES];
static int      ghCount   = 0;   // entries in current level
static int      ghSel     = 0;   // selected index
static int      ghScroll  = 0;   // top-visible index
#define GH_MAX_DEPTH 8
static String   ghStack[GH_MAX_DEPTH];  // path at each navigation depth
static int      ghDepth   = 0;
static String   ghDlStatus;             // result message after download

void drawDumpSelect() {
    oledClear();
    oledTitle("Select Dump");

    oled.setTextSize(1);
    oled.setTextColor(SH110X_WHITE);
    oled.setTextWrap(false);

    if (dumpFiles.empty()) {
        oled.setCursor(0, 16);
        oled.print("No dumps stored.\n");
        oled.print("Use web interface\n");
        oled.print("to download first.");
    } else {
        int scroll = max(0, dumpSel - 1);
        for (int i = 0; i < 3 && (scroll + i) < (int)dumpFiles.size(); i++) {
            int idx = scroll + i;
            int y   = 13 + i * 17;
            bool sel = (idx == dumpSel);
            String name = dumpFiles[idx];
            // Strip leading slash
            if (name[0] == '/') name = name.substring(1);
            if (name.length() > 17) name = name.substring(0, 16) + "~";

            if (sel) {
                oled.fillRect(0, y - 1, 128, 15, SH110X_WHITE);
                oled.setTextColor(SH110X_BLACK);
            } else {
                oled.setTextColor(SH110X_WHITE);
            }
            oled.setCursor(2, y);
            oled.print(name);
            oled.setTextColor(SH110X_WHITE);
        }
    }
    oledFlush();
}

// ──────────────────────────────────────────────────────────────
//  WiFi helpers
// ──────────────────────────────────────────────────────────────
String wifiSSID, wifiPass;
bool   apMode = false;

void wifiLoadCreds() {
    DBGLN("[WiFi]  Loading credentials from SPIFFS...");
    prefs.begin("wifi", true);
    wifiSSID = prefs.getString("ssid", "");
    wifiPass = prefs.getString("pass", "");
    prefs.end();
}

void wifiSaveCreds(const String& ssid, const String& pass) {
    DBGF("[WiFi]  Saving credentials for SSID: %s\n", ssid.c_str());
    prefs.begin("wifi", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.end();
    wifiSSID = ssid;
    wifiPass = pass;
}

bool wifiConnect() {
    DBGF("[WiFi]  Connecting to SSID: %s ...\n", wifiSSID.c_str());
    if (wifiSSID.isEmpty()) return false;
    showStatus2("Connecting WiFi", wifiSSID.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
    for (int i = 0; i < 24 && WiFi.status() != WL_CONNECTED; i++)
        delay(500);
    return WiFi.status() == WL_CONNECTED;
}

void wifiStartAP() {
    DBGLN("[WiFi]  Starting AP mode...");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    apMode = true;
    showStatus2("AP: " AP_SSID, "http://192.168.4.1");
    delay(1500);
}

// ──────────────────────────────────────────────────────────────
//  Embedded web interface HTML  (stored in flash via PROGMEM)
// ──────────────────────────────────────────────────────────────
static const char INDEX_HTML[] PROGMEM = R"HTMLRAW(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>BambuTagger</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Segoe UI',sans-serif;background:#0d1117;color:#c9d1d9;min-height:100vh}
.nav{background:#161b22;border-bottom:1px solid #30363d;padding:12px 20px;display:flex;align-items:center;gap:20px}
.nav h1{color:#58a6ff;font-size:1.2em;flex:1}
.nav .pill{background:#21262d;border-radius:20px;padding:4px 12px;font-size:.8em;cursor:pointer;border:1px solid #30363d;color:#c9d1d9}
.nav .pill.active{background:#1f6feb;border-color:#1f6feb;color:#fff}
.footer{position:absolute;bottom:0px;width:100%;background:#161b22;border-top:1px solid #30363d;padding:12px 20px;align-items:center;gap:20px}
.content{max-width:700px;margin:20px auto;padding:0 16px}
.card{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:16px;margin-bottom:16px}
.card h3{color:#58a6ff;margin-bottom:12px;font-size:1em}
label{display:block;font-size:.8em;color:#8b949e;margin:8px 0 3px}
input[type=text],input[type=password],select{width:100%;padding:8px 10px;border-radius:6px;border:1px solid #30363d;background:#0d1117;color:#c9d1d9;font-size:.9em}
input:focus,select:focus{outline:2px solid #1f6feb;border-color:#1f6feb}
.btn{display:inline-block;padding:8px 18px;border-radius:6px;border:none;cursor:pointer;font-size:.85em;font-weight:600;margin:4px 4px 0 0;transition:.15s}
.btn-primary{background:#1f6feb;color:#fff}.btn-primary:hover{background:#388bfd}
.btn-success{background:#238636;color:#fff}.btn-success:hover{background:#2ea043}
.btn-danger{background:#b62324;color:#fff}.btn-danger:hover{background:#da3633}
.btn-secondary{background:#21262d;color:#c9d1d9;border:1px solid #30363d}
.btn-secondary:hover{background:#30363d}
.status{padding:10px 14px;border-radius:6px;font-size:.85em;margin:10px 0}
.ok{background:#0f3d2c;color:#3fb950;border:1px solid #238636}
.err{background:#3d0a0a;color:#f85149;border:1px solid #b62324}
.info{background:#102030;color:#79c0ff;border:1px solid #1f6feb}
.tree{list-style:none}
.tree li{padding:8px 12px;border-bottom:1px solid #21262d;cursor:pointer;display:flex;align-items:center;gap:8px;transition:.1s}
.tree li:hover{background:#21262d}
.tree .dir::before{content:"📁"}
.tree .file::before{content:"💾"}
.tree .back::before{content:"⬅️"}
.breadcrumb{font-size:.8em;color:#8b949e;margin-bottom:10px}
.breadcrumb a{color:#58a6ff;cursor:pointer;text-decoration:none}
.breadcrumb a:hover{text-decoration:underline}
.file-entry{display:flex;justify-content:space-between;align-items:center;padding:8px 12px;border-bottom:1px solid #21262d}
.file-name{color:#c9d1d9;font-size:.85em;flex:1}
.file-size{color:#8b949e;font-size:.75em;margin-right:12px}
.tag-table td{padding:4px 10px 4px 0;font-size:.85em}
.tag-table td:first-child{color:#8b949e;white-space:nowrap}
.swatch{display:inline-block;width:14px;height:14px;border-radius:3px;border:1px solid #30363d;vertical-align:middle;margin-left:6px}
.spinner{display:inline-block;width:14px;height:14px;border:2px solid #30363d;border-top-color:#58a6ff;border-radius:50%;animation:spin .6s linear infinite;margin-right:6px;vertical-align:middle}
@keyframes spin{to{transform:rotate(360deg)}}
.hidden{display:none}
</style>
</head>
<body>
<div class="nav">
  <h1><img style="vertical-align:middle" src="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACAAAAAgAgMAAAAOFJJnAAABhWlDQ1BJQ0MgcHJvZmlsZQAAKJF9kb9Lw0AcxV9bS6VUHawg4pChOrWLijiWKhbBQmkrtOpgcukvaNKQpLg4Cq4FB38sVh1cnHV1cBUEwR8g/gHipOgiJX4vKbSI8eC4D+/uPe7eAd5WjSlGXxxQVFPPJBNCvrAqBF7hRxAjGERUZIaWyi7m4Dq+7uHh612MZ7mf+3MMyEWDAR6BOM403STeIJ7dNDXO+8RhVhFl4nPiqE4XJH7kuuTwG+eyzV6eGdZzmXniMLFQ7mGph1lFV4hniCOyolK+N++wzHmLs1JrsM49+QtDRXUly3Wa40hiCSmkIUBCA1XUYCJGq0qKgQztJ1z8Y7Y/TS6JXFUwciygDgWi7Qf/g9/dGqXpKScplAD8L5b1MQEEdoF207K+jy2rfQL4noErteuvt4C5T9KbXS1yBAxtAxfXXU3aAy53gNEnTdRFW/LR9JZKwPsZfVMBGL4FgmtOb519nD4AOepq+QY4OAQmy5S97vLu/t7e/j3T6e8HrYRyvp7c8c0AAAAJUExURXIA83m/boC9efRkY8YAAAABdFJOUwBA5thmAAAAAWJLR0QAiAUdSAAAAL1JREFUGNNNkLEKg0AMhv8GHO52H0FR36SbCJHD6XASn+Lazb1XHG8R1Kds7kqLgZAvGZL/D3CJbXCp1sxTAs/cx5HmvWErUJhvYgsA9QJv6AAvzUqeXe5Au5qPNrMyL4GXslCuAppbK4B6AhkUDr6PUDpYBQiUgZw2Bs02KJsn4KVjgWLh+8idQbawVTwaqGeERwttfZv1coKMXrMgR2lFVcX9f2GIm5PUwpBL4omPOdlJBpNT9bOM87y+5AM/WTesHvLO9wAAAABJRU5ErkJggg=="> BambuTagger</h1>
  <div class="pill active"  id="tab-local-btn"  onclick="switchTab('local')">Files</div>
  <div class="pill"         id="tab-github-btn" onclick="switchTab('github')">Library</div>
  <div class="pill"         id="tab-status-btn" onclick="switchTab('status')">Status</div>
  <div class="pill"         id="tab-wifi-btn"   onclick="switchTab('wifi')">WiFi</div>
</div>

<div class="content">
<!-- ── WIFI TAB ─────────────────────────────────────────── -->
<div id="tab-wifi" class="hidden">
  <div class="card">
    <h3>WiFi Configuration</h3>
    <div id="wstatus" class="status info">Checking…</div>
    <label>Network (SSID)</label>
    <input type="text" id="wifi-ssid" placeholder="Your WiFi name">
    <label>Password</label>
    <input type="password" id="wifi-pass" placeholder="Password (leave blank if open)">
    <br><br>
    <button class="btn btn-primary" onclick="saveWifi()">💾 Save &amp; Connect</button>
    <button class="btn btn-secondary" onclick="scanNets()">🔍 Scan</button>
    <div id="nets" style="margin-top:10px"></div>
  </div>
</div>

<!-- ── GITHUB TAB ────────────────────────────────────────── -->
<div id="tab-github" class="hidden">
  <div class="card">
    <h3>Browse Bambu Lab RFID Library</h3>
    <div class="breadcrumb" id="crumb">
      <a onclick="githubNav('')">Root</a>
    </div>
    <div id="gh-tree"><div class="status info">Click a folder to browse…</div></div>
    <div id="dl-msg" style="margin-top:8px"></div>
  </div>
</div>

<!-- ── LOCAL FILES TAB ───────────────────────────────────── -->
<div id="tab-local">

  <!-- Upload card -->
  <div class="card">
    <h3>Upload Dump File</h3>
    <div id="drop-zone"
         ondragover="event.preventDefault();this.classList.add('drag-over')"
         ondragleave="this.classList.remove('drag-over')"
         ondrop="handleDrop(event)"
         onclick="document.getElementById('upload-input').click()">
      📂 Drag &amp; drop a <code>.bin</code> file here, or click to browse
    </div>
    <input type="file" id="upload-input" accept=".bin" style="display:none" onchange="uploadFile(this.files[0])">
    <div id="upload-msg" style="margin-top:8px"></div>
  </div>

  <!-- File list card -->
  <div class="card">
    <h3>Stored Dump Files</h3>
    <div id="local-list"><div class="status info">Loading…</div></div>
    <button class="btn btn-secondary" style="margin-top:8px" onclick="loadLocal()">↻ Refresh</button>
  </div>

</div>

<!-- ── STATUS TAB ────────────────────────────────────────── -->
<div id="tab-status" class="hidden">
  <div class="card">
    <h3>Device Status</h3>
    <div id="dev-status"><div class="status info">Loading…</div></div>
    <button class="btn btn-secondary" style="margin-top:8px" onclick="loadStatus()">↻ Refresh</button>
  </div>
  <div class="card" id="last-tag-card" style="display:none">
    <h3>Last Read Tag</h3>
    <table class="tag-table" id="tag-table"></table>
  </div>
</div>
</div>

<!-- ── FOOTER ─────────────────────────────────────────────── -->
<div class="footer">
  <center>&copy; 2026 by <a href="https://www.vid-pro.de" target=_new>VID-PRO</a> | 
  credits to <a href="https://github.com/Bambu-Research-Group/RFID-Tag-Guide" target=_new>RFID-Tag-Guide</a> |
  Library from <a href="https://github.com/queengooborg/Bambu-Lab-RFID-Library" target=_new>Bambu-Lab-RFID-Library</a>
  </center>
</div>
<!-- /content -->

<script>
let curPath = '';
let pathStack = [];

function switchTab(name) {
  ['wifi','github','local','status'].forEach(t => {
    document.getElementById('tab-'+t).classList.toggle('hidden', t!==name);
    document.getElementById('tab-'+t+'-btn').classList.toggle('active', t===name);
  });
  if(name==='github' && curPath==='' && document.getElementById('gh-tree').textContent.includes('Click')) githubNav('');
  if(name==='local')  loadLocal();
  if(name==='status') loadStatus();
  if(name==='wifi')   loadWifiStatus();
}

// ── WiFi ────────────────────────────────────────────────────
function loadWifiStatus() {
  fetch('/api/status').then(r=>r.json()).then(d=>{
    const el = document.getElementById('wstatus');
    el.className = 'status ' + (d.wifi ? 'ok' : 'err');
    el.innerHTML = d.wifi
      ? `✅ Connected to <b>${d.ssid}</b><br>Device IP: <b>${d.ip}</b>`
      : '❌ Not connected. Enter credentials below.';
    document.getElementById('wifi-ssid').value = d.ssid || '';
  }).catch(()=>{});
}

function saveWifi() {
  const body = JSON.stringify({ssid:document.getElementById('wifi-ssid').value,
                               pass:document.getElementById('wifi-pass').value});
  fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body})
  .then(r=>r.json()).then(d=>{
    const el = document.getElementById('wstatus');
    el.className = 'status '+(d.success?'ok':'err');
    el.innerHTML = d.message;
    if(d.success) setTimeout(loadWifiStatus, 5000);
  });
}

function scanNets() {
  document.getElementById('nets').innerHTML = '<div class="status info"><span class="spinner"></span>Scanning…</div>';
  fetch('/api/scan').then(r=>r.json()).then(arr=>{
    if(!arr.length){document.getElementById('nets').innerHTML='<div class="status info">No networks found</div>';return;}
    document.getElementById('nets').innerHTML = arr.map(n=>
      `<div style="padding:6px 10px;border-bottom:1px solid #21262d;cursor:pointer" onclick="document.getElementById('wifi-ssid').value='${n.ssid.replace(/'/g,"\\'")}'">`+
      `📶 <b>${n.ssid}</b> <span style="color:#8b949e">(${n.rssi} dBm)</span></div>`
    ).join('');
  });
}

// ── GitHub browser ─────────────────────────────────────────
function buildCrumb(path) {
  let html = '<a onclick="githubNav(\'\')">Root</a>';
  if(path){
    let parts = path.split('/'), acc = '';
    parts.forEach(p=>{
      acc = acc ? acc+'/'+p : p;
      const cp = acc;
      html += ' / <a onclick="githubNav(\''+cp+'\')">'+p+'</a>';
    });
  }
  document.getElementById('crumb').innerHTML = html;
}

function githubNav(path) {
  curPath = path;
  buildCrumb(path);
  document.getElementById('gh-tree').innerHTML = '<div class="status info"><span class="spinner"></span>Loading…</div>';
  fetch('/api/list?path='+encodeURIComponent(path))
  .then(r=>r.json()).then(items=>{
    let html = '<ul class="tree">';
    if(path) html += `<li class="back" onclick="githubNav('${path.includes('/')?path.substring(0,path.lastIndexOf('/')):''}')"> Back</li>`;
    items.forEach(it=>{
      if(it.type==='dir'){
        html += `<li class="dir" onclick="githubNav('${it.path}')"> ${it.name}</li>`;
      } else if(it.name.endsWith('.bin')){
        html += `<li class="file"> ${it.name}
          <button class="btn btn-success" style="margin-left:auto;margin-top:0;padding:4px 10px"
            onclick="dlDump('${it.path}','${it.name.replace(/'/g,"\\'")}')">⬇ Download</button></li>`;
      }
    });
    html += '</ul>';
    if(items.length===0) html = '<div class="status info">Empty folder</div>';
    document.getElementById('gh-tree').innerHTML = html;
  }).catch(e=>{
    document.getElementById('gh-tree').innerHTML = '<div class="status err">Error: '+e+'</div>';
  });
}

function dlDump(path, name) {
  document.getElementById('dl-msg').innerHTML = '<div class="status info"><span class="spinner"></span>Downloading '+name+'…</div>';
  fetch('/api/download',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({path,name})})
  .then(r=>r.json()).then(d=>{
    document.getElementById('dl-msg').innerHTML =
      `<div class="status ${d.success?'ok':'err'}">${d.message}</div>`;
  });
}

// ── Local files ────────────────────────────────────────────
function loadLocal() {
  fetch('/api/files').then(r=>r.json()).then(files=>{
    if(!files.length){
      document.getElementById('local-list').innerHTML='<div class="status info">No dumps yet. Use the Library tab.</div>';
      return;
    }
    let html = '';
    files.forEach(f=>{
      const name = f.name.startsWith('/')?f.name.substring(1):f.name;
      html += `<div class="file-entry">
        <span class="file-name">💾 ${name}</span>
        <span class="file-size">${(f.size).toFixed(1)} B</span>
        <button class="btn btn-danger" style="padding:4px 8px;font-size:.75em"
          onclick="delFile('${f.name}')">🗑</button>
      </div>`;
    });
    document.getElementById('local-list').innerHTML = html;
  });
}

function delFile(name) {
  if(!confirm('Delete '+name+'?')) return;
  fetch('/api/delete',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({name})})
  .then(()=>loadLocal());
}

function handleDrop(e) {
  e.preventDefault();
  document.getElementById('drop-zone').classList.remove('drag-over');
  const f = e.dataTransfer.files[0];
  if(f) uploadFile(f);
}

function uploadFile(file) {
  if(!file) return;
  if(!file.name.toLowerCase().endsWith('.bin')) {
    document.getElementById('upload-msg').innerHTML=
      '<div class="status err">Only .bin files are accepted.</div>';
    return;
  }
  const msg = document.getElementById('upload-msg');
  msg.innerHTML='<div class="status info"><span class="spinner"></span>Uploading '+file.name+'…</div>';
  const fd = new FormData();
  fd.append('file', file, file.name);
  fetch('/api/upload',{method:'POST',body:fd})
  .then(r=>r.json()).then(d=>{
    msg.innerHTML='<div class="status '+(d.success?'ok':'err')+'">'+d.message+'</div>';
    if(d.success) loadLocal();
  }).catch(err=>{
    msg.innerHTML='<div class="status err">Upload error: '+err+'</div>';
  });
}

// ── Status ─────────────────────────────────────────────────
function loadStatus() {
  fetch('/api/status').then(r=>r.json()).then(d=>{
    document.getElementById('dev-status').innerHTML = `
      <table class="tag-table">
        <tr><td>WiFi</td><td>${d.wifi?'✅ '+d.ssid:'❌ Not connected'}</td></tr>
        <tr><td>IP</td><td>${d.ip}</td></tr>
        <tr><td>Mode</td><td>${d.ap_mode?'Access Point (AP)':'Station (STA)'}</td></tr>
        <tr><td>Free Heap</td><td>${d.heap} bytes</td></tr>
        <tr><td>SPIFFS</td><td>${d.spiffs_used} / ${d.spiffs_total} bytes</td></tr>
        <tr><td>Selected dump</td><td>${d.selected_dump||'— none —'}</td></tr>
      </table>`;

    if(d.last_tag && d.last_tag.valid) {
      const t = d.last_tag;
      const sw = `<span class="swatch" style="background:#${t.colorR.toString(16).padStart(2,'0')}${t.colorG.toString(16).padStart(2,'0')}${t.colorB.toString(16).padStart(2,'0')}"></span>`;
      document.getElementById('tag-table').innerHTML = `
        <tr><td>UID</td><td>${t.uid}</td></tr>
        <tr><td>Type</td><td>${t.filamentType}</td></tr>
        <tr><td>Sub-type</td><td>${t.detailedType}</td></tr>
        <tr><td>Variant</td><td>${t.variantId}</td></tr>
        <tr><td>Material ID</td><td>${t.materialId}</td></tr>
        <tr><td>Color</td><td>#${t.colorR.toString(16).padStart(2,'0').toUpperCase()}${t.colorG.toString(16).padStart(2,'0').toUpperCase()}${t.colorB.toString(16).padStart(2,'0').toUpperCase()} ${sw}</td></tr>
        <tr><td>Spool weight</td><td>${t.spoolWeight} g</td></tr>
        <tr><td>Diameter</td><td>${t.diameter.toFixed(2)} mm</td></tr>
        <tr><td>Length</td><td>${t.filamentLength} m</td></tr>
        <tr><td>Nozzle temp</td><td>${t.minNozzleTemp}–${t.maxNozzleTemp} °C</td></tr>
        <tr><td>Bed temp</td><td>${t.bedTemp} °C</td></tr>
        <tr><td>Dry temp/time</td><td>${t.dryTemp} °C / ${t.dryTime} h</td></tr>`;
      document.getElementById('last-tag-card').style.display='block';
    }
  });
}

loadWifiStatus();
</script>
</body>
</html>
)HTMLRAW";

// ──────────────────────────────────────────────────────────────
//  Web server – API handlers
// ──────────────────────────────────────────────────────────────

// Helper: serialise a TagInfo to a JSON object in doc
static void tagInfoToJson(JsonObject obj, const TagInfo* t) {
    char uid[9];
    snprintf(uid, sizeof(uid), "%02X%02X%02X%02X",
             t->uid[0], t->uid[1], t->uid[2], t->uid[3]);
    obj["valid"]          = t->valid;
    obj["uid"]            = uid;
    obj["filamentType"]   = t->filamentType;
    obj["detailedType"]   = t->detailedType;
    obj["variantId"]      = t->variantId;
    obj["materialId"]     = t->materialId;
    obj["colorR"]         = t->colorR;
    obj["colorG"]         = t->colorG;
    obj["colorB"]         = t->colorB;
    obj["spoolWeight"]    = t->spoolWeight;
    obj["diameter"]       = t->diameter;
    obj["minNozzleTemp"]  = t->minNozzleTemp;
    obj["maxNozzleTemp"]  = t->maxNozzleTemp;
    obj["bedTemp"]        = t->bedTemp;
    obj["dryTemp"]        = t->dryTemp;
    obj["dryTime"]        = t->dryTime;
    obj["filamentLength"] = t->filamentLength;
}

void apiStatus() {
    DBGLN("[HTTP]  GET /api/status");
    DynamicJsonDocument doc(1024);
    doc["wifi"]          = (WiFi.status() == WL_CONNECTED);
    doc["ssid"]          = wifiSSID;
    doc["ip"]            = apMode ? "192.168.4.1" : WiFi.localIP().toString();
    doc["ap_mode"]       = apMode;
    doc["heap"]          = (int)ESP.getFreeHeap();
    doc["spiffs_total"]  = (int)SPIFFS.totalBytes();
    doc["spiffs_used"]   = (int)SPIFFS.usedBytes();
    doc["selected_dump"] = String(selectedDumpPath);

    JsonObject ltObj = doc.createNestedObject("last_tag");
    tagInfoToJson(ltObj, &currentTag);

    String out; serializeJson(doc, out);
    httpServer.send(200, "application/json", out);
}

void apiWifi() {
    DBGF("[HTTP]  POST /api/wifi  method=%d\n", httpServer.method());
    DynamicJsonDocument doc(256);
    deserializeJson(doc, httpServer.arg("plain"));
    String ssid = doc["ssid"] | "";
    String pass = doc["pass"] | "";

    DynamicJsonDocument resp(128);
    if (ssid.isEmpty()) {
        resp["success"] = false;
        resp["message"] = "SSID cannot be empty";
    } else {
        wifiSaveCreds(ssid, pass);
        resp["success"] = true;
        resp["message"] = "Saved! Reconnecting in background";
    }
    String out; serializeJson(resp, out);
    httpServer.send(200, "application/json", out);

    if (!ssid.isEmpty()) {
        delay(300);
        WiFi.disconnect(true);
        delay(300);
        if (wifiConnect()) {
            apMode = false;
            Serial.println("Reconnected: " + WiFi.localIP().toString());
        }
    }
}

void apiScan() {
    DBGLN("[HTTP]  GET /api/scan");
    WiFi.scanDelete();
    int n = WiFi.scanNetworks(false, false, false, 200);
    DynamicJsonDocument doc(2048);
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < n && i < 20; i++) {
        JsonObject o = arr.createNestedObject();
        o["ssid"] = WiFi.SSID(i);
        o["rssi"] = WiFi.RSSI(i);
    }
    String out; serializeJson(doc, out);
    httpServer.send(200, "application/json", out);
}

/* Fetch GitHub API directory listing and return filtered JSON */
void apiList() {
    DBGLN("[HTTP]  GET /api/list");
    String path = httpServer.arg("path");

    if (WiFi.status() != WL_CONNECTED) {
        httpServer.send(200, "application/json", "[]");
        return;
    }

    WiFiClientSecure client;
    client.setInsecure();   // skip cert verification (ESP32 has no root CA store by default)
    HTTPClient http;
    String url = "https://" GITHUB_API_HOST "/repos" GITHUB_REPO_PATH "/contents/" + path;
    http.begin(client, url);
    http.addHeader("User-Agent", "BambuTagger-ESP32/1.0");
    int code = http.GET();

    if (code != 200) {
        http.end();
        httpServer.send(200, "application/json", "[]");
        return;
    }

    // Filter: only keep name, path, type
    StaticJsonDocument<256> filter;
    filter[0]["name"] = true;
    filter[0]["path"] = true;
    filter[0]["type"] = true;

    DynamicJsonDocument raw(16384);
    DeserializationError err = deserializeJson(raw, http.getStream(),
                                               DeserializationOption::Filter(filter));
    http.end();

    DynamicJsonDocument resp(8192);
    JsonArray arr = resp.to<JsonArray>();

    if (!err) {
        for (JsonObject item : raw.as<JsonArray>()) {
            String type = item["type"] | "";
            String name = item["name"] | "";
            if (type == "file" && !name.endsWith(".bin")) continue;
            JsonObject o = arr.createNestedObject();
            o["name"] = name;
            o["path"] = item["path"] | "";
            o["type"] = type;
        }
    }
    String out; serializeJson(resp, out);
    httpServer.send(200, "application/json", out);
}

void apiDownload() {
    DBGF("[HTTP]  GET /api/download  url=%s\n",
         httpServer.arg("url").c_str());
    DynamicJsonDocument req(256);
    deserializeJson(req, httpServer.arg("plain"));
    String ghPath = req["path"] | "";
    String fname  = req["name"] | "";

    DynamicJsonDocument resp(256);
    auto fail = [&](const char* msg) {
        resp["success"] = false; resp["message"] = msg;
        String out; serializeJson(resp, out);
        httpServer.send(200, "application/json", out);
    };

    if (ghPath.isEmpty() || fname.isEmpty()) { fail("Missing path/name"); return; }
    if (WiFi.status() != WL_CONNECTED)       { fail("WiFi not connected"); return; }

    // Sanitise filename for SPIFFS
    fname.replace(" ", "_");
    if (!fname.startsWith("/")) fname = "/" + fname;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, String(GITHUB_RAW_PREFIX) + ghPath);
    http.addHeader("User-Agent", "BambuTagger-ESP32/1.0");
    int code = http.GET();
    if (code != 200) { fail(("HTTP " + String(code)).c_str()); http.end(); return; }

    int totalSize = http.getSize();
    if (totalSize > 0 && totalSize != DUMP_SIZE) {
        fail(("Bad size: " + String(totalSize)).c_str()); http.end(); return;
    }

    File f = SPIFFS.open(fname, FILE_WRITE);
    if (!f) { fail("SPIFFS open failed"); http.end(); return; }

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[128]; int written = 0;
    unsigned long t0 = millis();
    while (written < DUMP_SIZE && (millis() - t0) < 20000) {
        int avail = stream->available();
        if (avail > 0) {
            int n = stream->readBytes(buf, min(avail, (int)sizeof(buf)));
            f.write(buf, n);
            written += n;
        } else if (!http.connected()) break;
        else delay(1);
    }
    f.close();
    http.end();

    if (written != DUMP_SIZE) {
        SPIFFS.remove(fname);
        fail(("Incomplete: " + String(written) + "/" + String(DUMP_SIZE)).c_str());
        return;
    }

    resp["success"] = true;
    resp["message"] = "Saved as " + fname;
    String out; serializeJson(resp, out);
    httpServer.send(200, "application/json", out);
}

void apiFiles() {
    DBGLN("[HTTP]  GET /api/files");
    DynamicJsonDocument doc(2048);
    JsonArray arr = doc.to<JsonArray>();
    File root = SPIFFS.open("/");
    File f = root.openNextFile();
    while (f) {
        String n = f.name();
        if (!f.isDirectory() && n.endsWith(".bin")) {
            JsonObject o = arr.createNestedObject();
            o["name"] = n;
            o["size"] = (int)f.size();
        }
        f = root.openNextFile();
    }
    String out; serializeJson(doc, out);
    httpServer.send(200, "application/json", out);
}

void apiDelete() {
    DBGF("[HTTP]  POST /api/delete  file=%s\n",
         httpServer.arg("file").c_str());
    DynamicJsonDocument doc(128);
    deserializeJson(doc, httpServer.arg("plain"));
    String name = doc["name"] | "";
    if (!name.startsWith("/")) name = "/" + name;
    bool ok = !name.isEmpty() && SPIFFS.remove(name);
    httpServer.send(200, "application/json",
                    ok ? "{\"success\":true}" : "{\"success\":false}");
}

// ── File upload (/api/upload, multipart/form-data, field "file") ──
static File uploadFile;
static bool uploadOk = false;

void apiUploadHandler() {
    HTTPUpload& upload = httpServer.upload();

    if (upload.status == UPLOAD_FILE_START) {
        uploadOk = false;
        String fname = upload.filename;
        // Keep only the basename, force .bin extension
        if (fname.lastIndexOf('/') >= 0)
            fname = fname.substring(fname.lastIndexOf('/') + 1);
        if (!fname.endsWith(".bin")) fname += ".bin";
        // Sanitise to plain ASCII
        String safe = "/";
        for (char c : fname)
            safe += (isAlphaNumeric(c) || c == '_' || c == '-' || c == '.') ? c : '_';
        Serial.printf("Upload start: %s\n", safe.c_str());
        DBGF("[UPLOAD] Start: %s\n", safe.c_str());
        uploadFile = SPIFFS.open(safe, FILE_WRITE);
        uploadOk   = (bool)uploadFile;

    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (uploadFile)
            uploadFile.write(upload.buf, upload.currentSize);

    } else if (upload.status == UPLOAD_FILE_END) {
        if (uploadFile) {
            uploadFile.close();
            Serial.printf("Upload done: %u bytes\n", upload.totalSize);
            DBGF("[UPLOAD] Done: %u bytes  result=%s\n",
                 upload.totalSize, uploadOk ? "OK" : "FAIL");
        }
    }
}

void apiUploadDone() {
    DynamicJsonDocument doc(128);
    doc["success"] = uploadOk;
    doc["message"] = uploadOk ? "File uploaded successfully." : "Upload failed – check filename / SPIFFS space.";
    String out; serializeJson(doc, out);
    httpServer.send(200, "application/json", out);
}

void setupHTTPServer() {
    httpServer.on("/", HTTP_GET, [](){
        httpServer.send_P(200, "text/html", INDEX_HTML);
    });
    httpServer.on("/api/status",   HTTP_GET,  apiStatus);
    httpServer.on("/api/wifi",     HTTP_POST, apiWifi);
    httpServer.on("/api/scan",     HTTP_GET,  apiScan);
    httpServer.on("/api/list",     HTTP_GET,  apiList);
    httpServer.on("/api/download", HTTP_POST, apiDownload);
    httpServer.on("/api/files",    HTTP_GET,  apiFiles);
    httpServer.on("/api/delete",   HTTP_POST, apiDelete);
    httpServer.on("/api/upload",   HTTP_POST, apiUploadDone, apiUploadHandler);
    httpServer.enableCORS(true);
    httpServer.begin();
    Serial.println("HTTP server started.");
}

// ──────────────────────────────────────────────────────────────
//  SPIFFS dump file list helpers
// ──────────────────────────────────────────────────────────────
std::vector<String> listDumpFiles() {
    std::vector<String> v;
    File root = SPIFFS.open("/");
    File f = root.openNextFile();
    while (f) {
        if (!f.isDirectory() && String(f.name()).endsWith(".bin"))
            v.push_back(String(f.name()));
        f = root.openNextFile();
    }
    return v;
}

// ──────────────────────────────────────────────────────────────
//  State-machine entry points
// ──────────────────────────────────────────────────────────────

// ══════════════════════════════════════════════════════════════
//  GitHub OLED browser
// ══════════════════════════════════════════════════════════════

// Fetch one directory level from the GitHub Contents API.
// Fills ghEntries[] / ghCount.  Returns true on success.
bool ghFetchDir(const String& repoPath) {
    if (WiFi.status() != WL_CONNECTED) {
        DBGLN("[GH]  No WiFi – cannot browse");
        return false;
    }
    String url = "https://" GITHUB_API_HOST "/repos" GITHUB_REPO_PATH "/contents/";
    if (!repoPath.isEmpty()) url += repoPath;

    DBGF("[GH]  Fetching: %s\n", url.c_str());

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, url);
    http.addHeader("User-Agent", "BambuTagger-ESP32/1.0");
    http.addHeader("Accept",     "application/vnd.github.v3+json");
    http.setTimeout(10000);
    int code = http.GET();
    if (code != 200) {
        DBGF("[GH]  HTTP error %d\n", code);
        http.end();
        return false;
    }

    // Parse – only keep name, path, type
    StaticJsonDocument<256> filter;
    filter[0]["name"] = true;
    filter[0]["path"] = true;
    filter[0]["type"] = true;

    DynamicJsonDocument doc(32000); //24576
    DeserializationError err = deserializeJson(doc, http.getStream(),
                                               DeserializationOption::Filter(filter));
    http.end();
    if (err) { DBGF("[GH]  JSON error: %s\n", err.c_str()); return false; }

    ghCount = 0;
    for (JsonObject item : doc.as<JsonArray>()) {
        if (ghCount >= GH_MAX_ENTRIES) break;
        String name = item["name"] | "";
        String path = item["path"] | "";
        String type = item["type"] | "";
        // Skip README and other non-dump files at file level
        // (keep dirs and .json / .bin files)
        if (type == "file" &&
            !name.endsWith(".json") &&
            !name.endsWith(".bin"))  continue;

        strncpy(ghEntries[ghCount].name, name.c_str(), 47);
        ghEntries[ghCount].name[47] = '\0';
        strncpy(ghEntries[ghCount].path, path.c_str(), 127);
        ghEntries[ghCount].path[127] = '\0';
        ghEntries[ghCount].isDir = (type == "dir");
        ghCount++;
    }
    DBGF("[GH]  Loaded %d entries for '%s'\n", ghCount, repoPath.c_str());
    return true;
}

// Parse a GitHub dump.json into raw MIFARE binary (DUMP_SIZE bytes).
// Supports:
//   - Array of 64 hex strings (16 chars each)
//   - Object {"0":"hex...","1":"hex...",...}
//   - Object {"blocks": <above>}
//   - Object {"Cards":[{"Blocks":{...}}]}
// Returns number of bytes written (DUMP_SIZE on success, 0 on failure).
int ghParseJson(const uint8_t* jsonBytes, size_t jsonLen, uint8_t* outBuf) {
    memset(outBuf, 0xFF, DUMP_SIZE);

    DynamicJsonDocument doc(32768);
    DeserializationError err = deserializeJson(doc, jsonBytes, jsonLen);
    if (err) { DBGF("[GH]  JSON parse error: %s\n", err.c_str()); return 0; }

    auto hexToBin = [](const char* hex, uint8_t* dst, int len) -> bool {
        for (int i = 0; i < len; i++) {
            char hi = hex[i * 2], lo = hex[i * 2 + 1];
            if (!isxdigit(hi) || !isxdigit(lo)) return false;
            auto h2n = [](char c) -> uint8_t {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                return 10 + c - 'A';
            };
            dst[i] = (h2n(hi) << 4) | h2n(lo);
        }
        return true;
    };

    int found = 0;

    // Try: plain array of 64 hex strings
    if (doc.is<JsonArray>()) {
        JsonArray arr = doc.as<JsonArray>();
        int blk = 0;
        for (JsonVariant v : arr) {
            if (blk >= 64) break;
            const char* hex = v.as<const char*>();
            if (hex && strlen(hex) >= 32) {
                hexToBin(hex, outBuf + blk * 16, 16);
                found++;
            }
            blk++;
        }
        if (found == 64) return DUMP_SIZE;
    }

    // Try: top-level or nested "blocks"/"Blocks" object {"0":"hex",...}
    JsonVariant blocks; //doc['blocks'] | doc['Blocks'] | doc['Cards'][0]['Blocks'] | doc['Cards'][0]['blocks'];
    if (blocks.isNull()) blocks = doc.as<JsonObject>(); // try root as blocks

    if (blocks.is<JsonObject>()) {
        for (JsonPair kv : blocks.as<JsonObject>()) {
            int blk = atoi(kv.key().c_str());
            if (blk < 0 || blk >= 64) continue;
            const char* hex = kv.value().as<const char*>();
            if (!hex) {
                // Might be array ["AB CD EF..."]
                if (kv.value().is<JsonArray>()) {
                    String joined = "";
                    for (JsonVariant b : kv.value().as<JsonArray>())
                        joined += String(b.as<const char*>() ? b.as<const char*>() : "");
                    joined.replace(" ", "");
                    if (joined.length() >= 32) {
                        hexToBin(joined.c_str(), outBuf + blk * 16, 16);
                        found++;
                    }
                }
                continue;
            }
            // Strip spaces
            String h = String(hex);
            h.replace(" ", "");
            if (h.length() >= 32) {
                hexToBin(h.c_str(), outBuf + blk * 16, 16);
                found++;
            }
        }
        if (found > 0) return DUMP_SIZE;
    }

    DBGF("[GH]  JSON: could not find block data (found=%d)\n", found);
    return 0;
}

// Download a raw URL and save to SPIFFS.  If it's a JSON file, parse it to
// binary first and save as .bin.  Returns true on success.
bool ghSaveFile(const String& rawUrl, const String& localName) {
    DBGF("[GH]  Downloading: %s -> %s\n", rawUrl.c_str(), localName.c_str());

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, rawUrl);
    http.addHeader("User-Agent", "BambuTagger-ESP32/1.0");
    http.setTimeout(15000);
    int code = http.GET();
    if (code != 200) {
        DBGF("[GH]  HTTP error %d\n", code);
        http.end();
        return false;
    }

    int size = http.getSize();
    bool isJson = rawUrl.endsWith(".json");

    if (isJson) {
        // Buffer the JSON (max 32 KB) then convert to binary
        const int MAX_JSON = 32768;
        uint8_t* jbuf = (uint8_t*)malloc(MAX_JSON);
        if (!jbuf) { http.end(); return false; }

        WiFiClient* stream = http.getStreamPtr();
        int got = 0;
        unsigned long t0 = millis();
        while (got < MAX_JSON - 1 && millis() - t0 < 12000) {
            int avail = stream->available();
            if (avail > 0) {
                int n = stream->readBytes(jbuf + got,
                    min(avail, MAX_JSON - 1 - got));
                got += n;
            } else if (!http.connected()) break;
            else delay(1);
        }
        http.end();
        jbuf[got] = '\0';
        DBGF("[GH]  JSON bytes read: %d\n", got);

        uint8_t binBuf[DUMP_SIZE];
        int converted = ghParseJson(jbuf, got, binBuf);
        free(jbuf);

        if (converted != DUMP_SIZE) {
            DBGLN("[GH]  JSON→bin conversion failed");
            return false;
        }

        // Save binary
        String savePath = "/" + localName;
        if (!savePath.endsWith(".bin")) savePath += ".bin";
        File f = SPIFFS.open(savePath, FILE_WRITE);
        if (!f) return false;
        f.write(binBuf, DUMP_SIZE);
        f.close();
        DBGF("[GH]  Saved binary: %s\n", savePath.c_str());
        return true;

    } else {
        // Raw binary download
        if (size > 0 && size != DUMP_SIZE) {
            DBGF("[GH]  Unexpected size %d\n", size);
            http.end();
            return false;
        }
        String savePath = "/" + localName;
        if (!savePath.endsWith(".bin")) savePath += ".bin";
        File f = SPIFFS.open(savePath, FILE_WRITE);
        if (!f) { http.end(); return false; }

        WiFiClient* stream = http.getStreamPtr();
        uint8_t buf[128]; int written = 0;
        unsigned long t0 = millis();
        while (written < DUMP_SIZE && millis() - t0 < 15000) {
            int avail = stream->available();
            if (avail > 0) {
                int n = stream->readBytes(buf, min(avail, (int)sizeof(buf)));
                f.write(buf, n);
                written += n;
            } else if (!http.connected()) break;
            else delay(1);
        }
        f.close();
        http.end();
        if (written != DUMP_SIZE) {
            SPIFFS.remove(savePath);
            DBGF("[GH]  Incomplete: %d/%d\n", written, DUMP_SIZE);
            return false;
        }
        DBGF("[GH]  Saved: %s (%d bytes)\n", savePath.c_str(), written);
        return true;
    }
}

// Draw the GitHub browser screen
void drawGhBrowser() {
    oledClear();

    // ── Title bar ─────────────────────────────────────────────
    oled.fillRect(0, 0, 128, 11, SH110X_WHITE);
    oled.setTextColor(SH110X_BLACK);
    oled.setTextSize(1);
    oled.setTextWrap(false);
    oled.setCursor(2, 2);

    String title = "GitHub Library";
    if (ghDepth > 0 && !ghStack[ghDepth - 1].isEmpty()) {
        // Show last component of path
        String p = ghStack[ghDepth - 1];
        int slash = p.lastIndexOf('/');
        String leaf = (slash >= 0) ? p.substring(slash + 1) : p;
        if (leaf.length() > 10) leaf = leaf.substring(0, 9) + "~";
        title += " >" + leaf;
    }
    if (title.length() > 20) title = title.substring(0, 19) + "~";
    oled.print(title);

    oled.setTextColor(SH110X_WHITE);

    // ── Entry list ────────────────────────────────────────────
    // Visible rows = 3 (rows at y=14, 26, 38)
    // Row 0 at depth>0 is always "[BACK]"
    bool hasBack = (ghDepth > 0);
    int totalRows = ghCount + (hasBack ? 1 : 0);

    if (totalRows == 0) {
        oled.setTextSize(1);
        oled.setCursor(0, 16);
        oled.print("  (empty)");
        oledFlush();
        return;
    }

    for (int row = 0; row < 3; row++) {
        int idx = ghScroll + row;
        if (idx >= totalRows) break;

        int y   = 13 + row * 17;
        bool sel = (idx == ghSel);

        String label;
        bool isDir = true;
        if (hasBack && idx == 0) {
            label = "<BACK";
        } else {
            int eIdx = idx - (hasBack ? 1 : 0);
            label  = String(ghEntries[eIdx].name);
            isDir  = ghEntries[eIdx].isDir;
            // Trim long names
            if (label.length() > 17) label = label.substring(0, 16) + "~";
            // Prefix icon
            label = (isDir ? " " : " ") + label;
        }

        if (sel) {
            oled.fillRect(0, y - 1, 128, 15, SH110X_WHITE);
            oled.setTextColor(SH110X_BLACK);
        } else {
            oled.setTextColor(SH110X_WHITE);
        }
        oled.setTextSize(1);
        oled.setCursor(2, y);
        oled.print(label);
        oled.setTextColor(SH110X_WHITE);
    }

    // Scroll arrows
    if (ghScroll > 0) {
        oled.setCursor(120, 13);
        oled.print("^");
    }
    if (ghScroll + 3 < totalRows) {
        oled.setCursor(120, 55);
        oled.print("v");
    }
    // Item count
    oled.setTextSize(1);
    oled.setCursor(0, 57);
    oled.setTextColor(SH110X_WHITE);
    oled.print(String(ghSel + 1) + "/" + String(totalRows));

    oledFlush();
}

void enterMainMenu() {
    DBGLN("[STATE] -> MAIN_MENU");
    appState = S_MAIN_MENU;
    ledOff();
    drawMenu();
}

// Enter the GitHub browser at a given repo path.
// push=true saves current depth to stack (for BACK navigation).
void enterGhBrowse(const String& repoPath, bool push) {
    DBGF("[GH]  enterGhBrowse path='%s' push=%d\n", repoPath.c_str(), push);
    appState = S_GH_BROWSE;

    if (push && ghDepth < GH_MAX_DEPTH) {
        ghStack[ghDepth++] = repoPath;
    }

    ghSel    = 0;
    ghScroll = 0;

    if (WiFi.status() != WL_CONNECTED) {
        ledSet(255, 80, 0);  // orange = no WiFi
        showStatus2("GitHub Browser", "No WiFi!");
        delay(2000);
        enterMainMenu();
        return;
    }

    ledScanPulse();  // blue while loading
    showStatus2("Loading", "Github Library");

    bool ok = ghFetchDir(repoPath);
    if (!ok) {
        ledFlash(255, 0, 0, 3);
        showStatus2("GitHub", "Fetch failed!");
        delay(2000);
        enterMainMenu();
        return;
    }
    ledSet(0, 0, 40);  // dim blue = browsing
    drawGhBrowser();
}

// Handle encoder input while in S_GH_BROWSE
void handleGhBrowseEncoder() {
    bool hasBack  = (ghDepth > 0);
    int  totalRows = ghCount + (hasBack ? 1 : 0);

    int d = encGetDelta();
    if (d != 0) {
        ghSel = constrain(ghSel + d, 0, totalRows - 1);
        // Keep selection in view
        if (ghSel < ghScroll) ghScroll = ghSel;
        if (ghSel >= ghScroll + 3) ghScroll = ghSel - 2;
        drawGhBrowser();
        return;
    }

    if (!encGetClick()) return;

    // ── BACK ─────────────────────────────────────────────────
    if (hasBack && ghSel == 0) {
        DBGLN("[GH]  BACK");
        ghDepth--;
        String parentPath = (ghDepth > 0) ? ghStack[ghDepth - 1] : "";
        // re-fetch parent without pushing again
        ghSel    = 0;
        ghScroll = 0;
        showStatus2("Loading", "Github Library");
        ghFetchDir(parentPath);
        drawGhBrowser();
        return;
    }

    // ── Select entry ──────────────────────────────────────────
    int eIdx = ghSel - (hasBack ? 1 : 0);
    if (eIdx < 0 || eIdx >= ghCount) return;

    GhEntry& entry = ghEntries[eIdx];

    if (entry.isDir) {
        // Navigate into directory
        enterGhBrowse(String(entry.path), true);
        return;
    }

    // ── It's a file → download ────────────────────────────────
    String fname = String(entry.name);
    String rawUrl;

    if (fname.endsWith(".bin")) {
        rawUrl = GITHUB_RAW_PREFIX + String(entry.path);
    } else if (fname.endsWith(".json")) {
        rawUrl = GITHUB_RAW_PREFIX + String(entry.path);
    } else {
        showStatus2("Skip file", fname.substring(0, 16).c_str());
        delay(1500);
        drawGhBrowser();
        return;
    }

    // Build a safe local name: last path component, ensure .bin extension
    String localName = fname;
    // Prefix with parent folder name to avoid collisions
    String p = String(entry.path);
    int sl = p.lastIndexOf('/');
    if (sl > 0) {
        String folder = p.substring(0, sl);
        int sl2 = folder.lastIndexOf('/');
        String leafFolder = (sl2 >= 0) ? folder.substring(sl2 + 1) : folder;
        leafFolder.replace(" ", "_");
        localName = leafFolder + "_" + fname;
    }
    // Force .bin extension
    if (localName.endsWith(".json"))
        localName = localName.substring(0, localName.length() - 5) + ".bin";
    if (!localName.endsWith(".bin"))
        localName += ".bin";

    // Show download screen
    appState = S_GH_DOWNLOAD;
    ledSet(255, 200, 0);  // yellow = working
    oledClear();
    oled.fillRect(0, 0, 128, 11, SH110X_WHITE);
    oled.setTextColor(SH110X_BLACK);
    oled.setTextSize(1);
    oled.setCursor(2, 2);
    oled.print("Downloading...");
    oled.setTextColor(SH110X_WHITE);
    oled.setCursor(0, 14);
    String shortName = localName;
    if (shortName.length() > 20) shortName = shortName.substring(0, 19) + "~";
    oled.print(shortName);
    oledFlush();

    bool ok = ghSaveFile(rawUrl, localName);

    if (ok) {
        ledFlash(0, 255, 0, 3);
        ghDlStatus = "Saved: " + localName;
        DBGF("[GH]  Download OK: %s\n", localName.c_str());
        showStatus2("Downloaded!", ("Use WriteDump\n" + localName.substring(0, 16)).c_str());
        // Refresh SPIFFS file list
        dumpFiles.clear();
        File root = SPIFFS.open("/");
        File ff   = root.openNextFile();
        while (ff) {
            if (!ff.isDirectory() && String(ff.name()).endsWith(".bin"))
                dumpFiles.push_back(String(ff.name()));
            ff = root.openNextFile();
        }
        dumpSel = max(0, (int)dumpFiles.size() - 1);
    } else {
        ledFlash(255, 0, 0, 3);
        ghDlStatus = "Failed!";
        DBGLN("[GH]  Download FAILED");
        showStatus2("Download FAILED", "See Serial");
    }
    delay(3000);

    // Return to browser (re-fetch current level)
    appState = S_GH_BROWSE;
    String curPath = (ghDepth > 0) ? ghStack[ghDepth - 1] : ""; 
    ghFetchDir(curPath);
    ghSel    = 0;
    ghScroll = 0;
    ledSet(0, 0, 40);
    drawGhBrowser();
}

void enterReadTag() {
    DBGLN("[STATE] -> READ_TAG");
    appState = S_READ_TAG;
    showStatus("Place Bambu tag\non reader\n\nPress to cancel");
}

void enterCloneSource() {
    DBGLN("[STATE] -> CLONE_SOURCE");
    appState = S_CLONE_SOURCE;
    showStatus("CLONE  Step 1/2\nPlace SOURCE tag\non reader\n\nPress to cancel");
}

void enterDumpSelect() {
    DBGLN("[STATE] -> DUMP_SELECT");
    appState  = S_DUMP_SELECT;
    dumpFiles = listDumpFiles();
    dumpSel   = 0;
    drawDumpSelect();
}

void enterWifiInfo() {
    DBGLN("[STATE] -> WIFI_INFO");
    appState = S_WIFI_INFO;
    // Cyan = STA connected, orange = AP mode
    if (WiFi.status() == WL_CONNECTED) {
        ledSet(0, 80, 80);   // cyan
        String ip = "IP: " + WiFi.localIP().toString();
        showStatus2("WiFi OK - Browse:", ip.c_str());
    } else {
        ledSet(80, 40, 0);   // amber
        showStatus2("AP: " AP_SSID, "http://192.168.4.1");
    }
}

// ──────────────────────────────────────────────────────────────
//  Main-menu encoder handler  (non-blocking)
// ──────────────────────────────────────────────────────────────
void handleMenuEncoder() {
    int d = encGetDelta();
    if (d > 0) {
        menuSel = (menuSel + 1) % MENU_COUNT;
        if (menuSel >= menuScroll + 3) menuScroll = menuSel - 2;
        if (menuScroll < 0) menuScroll = 0;
        drawMenu();
    } else if (d < 0) {
        menuSel = (menuSel - 1 + MENU_COUNT) % MENU_COUNT;
        if (menuSel < menuScroll) menuScroll = menuSel;
        drawMenu();
    }
    if (encGetClick()) {
        DBGF("[MENU]  selected item %d\n", menuSel);
        switch (menuSel) {
            case 0: enterReadTag();     break;
            case 1: enterCloneSource(); break;
            case 2: enterDumpSelect();  break;
            case 3: ghDepth = 0; enterGhBrowse("", true); break;
            case 4: enterWifiInfo();    break;
        }
    }
}

// ──────────────────────────────────────────────────────────────
//  Tag-info viewer encoder handler
// ──────────────────────────────────────────────────────────────
void handleTagViewEncoder() {
    int d = encGetDelta();
    if (d > 0) { tagPage = (tagPage + 1) % TAG_PAGES; drawTagInfo(&currentTag, tagPage); }
    if (d < 0) { tagPage = (tagPage - 1 + TAG_PAGES) % TAG_PAGES; drawTagInfo(&currentTag, tagPage); }
    if (encGetClick()) enterMainMenu();
}

// ──────────────────────────────────────────────────────────────
//  Dump-select encoder handler
// ──────────────────────────────────────────────────────────────
void handleDumpSelectEncoder() {
    int d = encGetDelta();
    if (d > 0 && dumpSel < (int)dumpFiles.size() - 1) { dumpSel++; drawDumpSelect(); }
    if (d < 0 && dumpSel > 0)                          { dumpSel--; drawDumpSelect(); }

    if (encGetClick()) {
        if (dumpFiles.empty()) {
            enterMainMenu();
            return;
        }
        // Load dump & confirm
        String path = dumpFiles[dumpSel];
        strncpy(selectedDumpPath, path.c_str(), sizeof(selectedDumpPath) - 1);

        if (!path.startsWith("/")) path = "/" + path;

        DBGF("[DUMP]  Loading file: %s\n", path.c_str());
        File f = SPIFFS.open(path, FILE_READ);
        if (!f || f.size() != DUMP_SIZE) {
            DBGF("[DUMP]  Bad file: size=%u expected=%u\n",
                 f ? (unsigned)f.size() : 0, DUMP_SIZE);
            showStatus("Bad dump file!\n\nPress to return");
            appState = S_WIFI_INFO;  // re-use the "press to go back" state
            return;
        }
        f.read(dumpBuf, DUMP_SIZE);
        f.close();
        DBGLN("[DUMP]  File loaded OK.");

        // Parse so we can show what we're about to write
        TagInfo preview;
        flatToTag(dumpBuf, &preview);

        char msg[128];
        snprintf(msg, sizeof(msg),
                 "Write dump:\n%s\n%s\n\nPlace blank card",
                 preview.filamentType, preview.detailedType);
        showStatus(msg);
        appState = S_DUMP_WRITE;
    }
}

// Generic "any button = back to menu"
void handleBackEncoder() {
    encGetDelta();   // discard rotation
    if (encGetClick()) enterMainMenu();
}

// ──────────────────────────────────────────────────────────────
//  Blocking RFID operations  (called once per state entry)
//  Each calls server.handleClient() in its inner loop so the
//  web interface stays responsive.
// ──────────────────────────────────────────────────────────────

void processReadTag() {
    DBGLN("[RFID] processReadTag: waiting for tag (15 s)...");
    unsigned long deadline = millis() + 15000;
    while (millis() < deadline) {
        httpServer.handleClient();
        encUpdate();
        ledScanPulse();                           // breathing blue while waiting
        if (encGetClick()) { enterMainMenu(); return; }
        if (rfidReadBambuTag(&currentTag)) {
            DBGF("[RFID] Tag read OK: %s / %s  color=#%02X%02X%02X\n",
                 currentTag.filamentType, currentTag.detailedType,
                 currentTag.colorR, currentTag.colorG, currentTag.colorB);
            ledSetTagColor(&currentTag);           // show filament colour
            tagPage = 0;
            appState = S_SHOW_TAG;
            drawTagInfo(&currentTag, 0);
            return;
        }
        delay(18);   // reduced to keep pulse smooth
    }
    DBGLN("[RFID] processReadTag: timeout – no tag.");
    ledFlash(255, 0, 0, 2);                       // two red flashes = no tag
    showStatus("No tag detected.\nPress to return.");
    appState = S_WIFI_INFO;
}

void processCloneSource() {
    DBGLN("[CLONE] processCloneSource: waiting for source tag (15 s)...");
    unsigned long deadline = millis() + 15000;
    while (millis() < deadline) {
        httpServer.handleClient();
        encUpdate();
        ledScanPulse();                            // breathing blue while waiting
        if (encGetClick()) { enterMainMenu(); return; }
        if (rfidReadBambuTag(&sourceTag)) {
            DBGF("[CLONE] Source tag read: %s / %s  UID=%02X%02X%02X%02X\n",
                 sourceTag.filamentType, sourceTag.detailedType,
                 sourceTag.uid[0], sourceTag.uid[1],
                 sourceTag.uid[2], sourceTag.uid[3]);
            ledSetTagColor(&sourceTag);            // flash source colour briefly
            tagToFlat(&sourceTag, dumpBuf);
            showStatus2("Source read OK!", "Place TARGET card\x85");
            delay(1500);
            ledSet(255, 165, 0);                   // orange = waiting for target card
            showStatus("CLONE  Step 2/2\nPlace TARGET card\non reader\x85\n\nPress to cancel");
            appState = S_CLONE_TARGET;
            return;
        }
        delay(18);
    }
    DBGLN("[CLONE] processCloneSource: timeout – no tag.");
    ledFlash(255, 0, 0, 2);
    showStatus("Timeout. No tag.\n\nPress to return.");
    appState = S_WIFI_INFO;
}

void processCloneTarget() {
    DBGLN("[CLONE] processCloneTarget: waiting for target card (15 s)...");
    unsigned long deadline = millis() + 15000;
    while (millis() < deadline) {
        httpServer.handleClient();
        encUpdate();
        ledScanPulse();                            // breathing blue while waiting for target
        if (encGetClick()) { enterMainMenu(); return; }
        if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
            DBGF("[CLONE] Target card UID: %02X %02X %02X %02X – starting write...\n",
                 rfid.uid.uidByte[0], rfid.uid.uidByte[1],
                 rfid.uid.uidByte[2], rfid.uid.uidByte[3]);
            ledSet(255, 255, 0);                   // yellow = writing in progress
            showStatus("Writing\x85");
            bool ok = rfidWriteDump(dumpBuf, true);
            DBGF("[CLONE] Write result: %s\n", ok ? "OK" : "FAIL");
            if (ok) {
                ledFlash(0, 255, 0, 3);            // 3× green = success
            } else {
                ledFlash(255, 0, 0, 3);            // 3× red = fail
            }
            showStatus(ok ? "Clone complete!\n\nPress to return."
                         : "Write failed!\nTry a magic card.\n\nPress to return.");
            appState = S_WIFI_INFO;
            return;
        }
        delay(18);
    }
    DBGLN("[CLONE] processCloneTarget: timeout – no card.");
    ledFlash(255, 0, 0, 2);
    showStatus("Timeout. No card.\n\nPress to return.");
    appState = S_WIFI_INFO;
}

void processDumpWrite() {
    DBGF("[DUMP]  processDumpWrite: file=%s  waiting for card (20 s)...\n",
         selectedDumpPath);
    // Show the dump's filament colour while waiting so the user can preview it
    TagInfo preview;
    flatToTag(dumpBuf, &preview);
    DBGF("[DUMP]  Preview: %s / %s  color=#%02X%02X%02X  wt=%.0fg\n",
         preview.filamentType, preview.detailedType,
         preview.colorR, preview.colorG, preview.colorB,
         preview.spoolWeight);
    ledSetTagColor(&preview);

    unsigned long deadline = millis() + 20000;
    while (millis() < deadline) {
        httpServer.handleClient();
        encUpdate();
        if (encGetClick()) { enterMainMenu(); return; }
        if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
            DBGF("[DUMP]  Card detected UID: %02X %02X %02X %02X – starting write...\n",
                 rfid.uid.uidByte[0], rfid.uid.uidByte[1],
                 rfid.uid.uidByte[2], rfid.uid.uidByte[3]);
            ledSet(255, 255, 0);                   // yellow = writing
            showStatus("Writing dump\x85");
            bool ok = rfidWriteDump(dumpBuf, true);
            DBGF("[DUMP]  Write result: %s\n", ok ? "OK" : "FAIL");
            if (ok) {
                ledFlash(0, 255, 0, 3);            // 3× green = success
                showStatus("Write complete!\n\nPress to return.");
            } else {
                ledFlash(255, 0, 0, 3);            // 3× red = fail
                showStatus("Write failed!\nTry a magic/FUID\ncard.\n\nPress to return.");
            }
            appState = S_WIFI_INFO;
            return;
        }
        delay(18);
    }
    DBGLN("[DUMP]  processDumpWrite: timeout – no card.");
    ledFlash(255, 0, 0, 2);
    showStatus("Timeout. No card.\n\nPress to return.");
    appState = S_WIFI_INFO;
}

// ──────────────────────────────────────────────────────────────
//  Arduino setup()
// ──────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(200);   // let serial settle
    DBGLN("\n\n========================================");
    DBGLN("  BambuTagger  – debug build");
    DBGLN("  Compiled: " __DATE__ "  " __TIME__);
    DBGLN("========================================");

    // ── WS2812B LED ─────────────────────────────────────────
    statusLed.begin();
    statusLed.setBrightness(80);   // 0-255; keep ~80 for 3.3 V direct drive
    statusLed.clear();
    statusLed.show();

    // ── I2C / OLED ──────────────────────────────────────────
    Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
    if (!oled.begin(SCREEN_ADDR, true)) {
        Serial.println(F("SH110X OLED init failed – check wiring!"));
        for (;;) delay(1000);
    }
    oled.clearDisplay();
    // Splash
    oled.setTextSize(2); oled.setTextColor(SH110X_WHITE);
    oled.setCursor(2, 6);  oled.print("BambuTagger");
    oled.setTextSize(1);
    oled.setCursor(18, 28); oled.print("Initialising...");
    oled.display();

    // ── SPI / RC522 ─────────────────────────────────────────
    SPI.begin();
    rfid.PCD_Init();
    rfid.PCD_SetAntennaGain(rfid.RxGain_max);
    Serial.print(F("RC522 firmware: "));
    rfid.PCD_DumpVersionToSerial();

    // ── SPIFFS ──────────────────────────────────────────────
    if (!SPIFFS.begin(true))
        Serial.println(F("SPIFFS mount failed!"));
    else
        Serial.printf("SPIFFS: %u/%u bytes used\n",
                      SPIFFS.usedBytes(), SPIFFS.totalBytes());

    // ── Encoder ─────────────────────────────────────────────
    pinMode(PIN_ENC_CLK, INPUT_PULLUP);
    pinMode(PIN_ENC_DT,  INPUT_PULLUP);
    pinMode(PIN_ENC_BTN, INPUT_PULLUP);
    encClkLast = digitalRead(PIN_ENC_CLK);

    // ── WiFi ────────────────────────────────────────────────
    wifiLoadCreds();
    bool connected = wifiConnect();
    if (connected) {
        apMode = false;
        Serial.println("WiFi connected: " + WiFi.localIP().toString());
    } else {
        wifiStartAP();
    }

    // ── HTTP server ──────────────────────────────────────────
    setupHTTPServer();

    // ── Show main menu ───────────────────────────────────────
    delay(500);
    drawMenu();
}

// ──────────────────────────────────────────────────────────────
//  Arduino loop()
// ──────────────────────────────────────────────────────────────
void loop() {
    httpServer.handleClient();
    encUpdate();

    switch (appState) {

        // ── Main menu – handled by encoder ──────────────────
        case S_MAIN_MENU:
            handleMenuEncoder();
            break;

        // ── Show decoded tag info ────────────────────────────
        case S_SHOW_TAG:
            handleTagViewEncoder();
            break;

        // ── Select dump file from SPIFFS ─────────────────────
        case S_DUMP_SELECT:
            handleDumpSelectEncoder();
            break;

        // ── "Any key returns to menu" states ─────────────────
        case S_WIFI_INFO:
            handleBackEncoder();
            break;

        // ── Active RFID operations (enter blocking loop once) ─
        case S_READ_TAG:
            processReadTag();
            break;

        case S_CLONE_SOURCE:
            processCloneSource();
            break;

        case S_CLONE_TARGET:
            processCloneTarget();
            break;

        case S_DUMP_WRITE:
            processDumpWrite();
            break;

        case S_GH_BROWSE:
            handleGhBrowseEncoder();
            break;

        case S_GH_DOWNLOAD:
            // handled inside handleGhBrowseEncoder; state self-exits
            break;

        default:
            enterMainMenu();
            break;
    }
}
