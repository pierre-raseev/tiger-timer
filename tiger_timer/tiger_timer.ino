/*
  ============================================================================
  TIGER TIMER  --  firmware for a battery-powered, pocket party-game console
  ============================================================================
  Hardware: Seeed XIAO ESP32C6. Full hardware spec, wiring, and every
  feature's exact intended behavior are documented in the companion
  functional specification document, TigerTimer_FSD.md - if this header and
  that document ever disagree, the FSD is the source of truth; please update
  one or the other rather than letting them drift.

  This comment is intentionally a *summary*, not a duplicate of the FSD - see
  TigerTimer_FSD.md for exact numbers, timings, and per-game rules; see
  TigerTimer_DevPlan.md for how this was originally broken into build phases.

  --- Quick wiring reference (see FSD Sec.2 for the authoritative version) ---
    Arcade button switch   -> D1 + GND   (INPUT_PULLUP, LOW = pressed)
    Arcade button LED (5V) -> D3 + GND   (direct GPIO drive, no boost needed)
    Buzzer (passive piezo) -> D2 + GND   (PWM-driven via LEDC, see playNote())
    OLED SH1106 128x64 I2C (addr 0x3C)  SDA->D4  SCL->D5  VCC->3V3  GND->GND
    EC11 rotary encoder     CLK->D10  DT->D8  SW->D9
    Power: 3.7V LiPo + separate charge module (independent of the ESP32, so
    charging still works during deep sleep) + a physical power switch.

  Libraries needed (Arduino Library Manager):
    - "Adafruit SH110X" (pulls in Adafruit GFX Library + Adafruit BusIO)
    - "ArduinoJson" v7.x specifically (used only by web_config.ino)
  Plus two files vendored directly into this sketch (not Library-Manager
  installs - see the comment at the top of each for why):
    - tigertimer_qrcode.h/.c  - a renamed copy of the "QRCode" library, to avoid
      a real collision with a same-named header the ESP32 core bundles
    - press_start_2p_font.h - a custom Adafruit_GFX bitmap font, converted
      from a user-provided TTF, used for the splash/title-bar text

  --- The one input model, used everywhere (FSD Sec.3.2 has the full table) ---
  There are exactly two physical controls and their meaning is consistent
  across every screen except where a specific game overrides it:
    - Arcade button, short press  = confirm / that screen's primary action.
    - Encoder, short click        = same as above in menu-type screens; a
                                     few games (Timer, Chess) repurpose it as
                                     pause/resume instead, since there's
                                     nothing to "confirm" mid-round.
    - Encoder, held + turned      = adjust buzzer volume, from anywhere, any
                                     time - shows a full-screen live indicator
                                     while held, restores the prior screen on
                                     release.
    - Encoder, held 1s (no turn)  = open "Go back to menu?" if inside a game;
                                     does nothing if already in the menu
                                     hierarchy. This is the ONLY way to leave
                                     a game - there is deliberately no
                                     un-confirmed direct exit, so a mid-round
                                     game can never be lost by accident.
  A hold that gets "claimed" by rotation (volume) never also fires a click or
  the long-press menu gesture afterward - see readEncoderButton()/readEncoder()
  and the comment on swClaimedByVolume/swLongFired for exactly how that's
  arbitrated.

  --- What's in this file vs. the other tabs ---
    tiger_timer.ino     - everything except the web config tool: drivers,
                           menu/settings, every game, localization, splash
                           screens, the volume system.
    web_config.ino       - "Config Mode": turns the device into its own WiFi
                           AP serving a small web app (data/config.html) for
                           editing things a single knob can't reasonably
                           edit - menu order/enable, trivia question content,
                           Pass-the-Bomb category list, startup behavior.
                           Entered only from Settings, off otherwise, so it
                           costs zero battery during normal play.
    tigertimer_qrcode (.h/.c), press_start_2p_font.h - vendored assets, see above.
    data/*.csv           - trivia question content (15-column format, see
                           the comment above loadTriviaQuestion() and FSD
                           Sec.5.8).
    data/config.html      - the Config Mode web page itself.

  --- Notable design decisions worth knowing before changing things ---
    - MENU_ORDER is a runtime array (not compile-time const) specifically so
      Config Mode can rewrite and persist it; see rebuildVisibleMenu() for
      how disabled games get filtered out of it without losing their saved
      position.
    - Games list their labels by looking up the game's *id*, not by array
      position (see menuLabelForGame()) - this was a real bug fixed early
      on: a positional label array silently mismatches once the menu order
      becomes user-configurable.
    - Word-wrapping is always done by this code's own logic with the display
      library's built-in auto-wrap explicitly disabled first - measuring
      text width with auto-wrap still enabled returns a size that already
      reflects the (possibly mid-word-broken) wrapped layout, not the true
      single-line width, which silently breaks wrap decisions. See
      drawRotatedWrappedText()/drawWrappedText() for the fix.
    - Tetris and Star Drop temporarily call display.setRotation(1) to use
      the panel's long axis as their vertical "falling" axis, and always
      restore rotation(0) before returning - every draw function that does
      this says so explicitly in its own comment; if you add a new rotated
      screen, follow the same bracket-tightly-and-always-restore pattern.
    - Config Mode's WiFi network is deliberately open (no password) so
      phones can captive-portal straight in without typing anything.
    - The trivia CSV format has no quote/escape support (plain comma-split)
      - question/answer text must not contain literal commas or newlines;
      the web editor strips them defensively on save.
  ============================================================================
*/


#include <Wire.h>
#include <Adafruit_GFX.h>
#include "press_start_2p_font.h" // generated from the user-provided PressStart2P-Regular.ttf - same font as the boot splash's TIGER/TIMER wordmark
#include <Adafruit_SH110X.h>
#include <Preferences.h>
#include <LittleFS.h>
#include "esp_sleep.h"
#include "driver/rtc_io.h" // LP-IO pull-up for the deep-sleep wake pin
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h> // used by web_config.ino for the captive-portal redirect
#include <ArduinoJson.h>
#include "tigertimer_qrcode.h" // vendored QR encoder (see tigertimer_qrcode.h/.c) - avoids a name collision
                             // with the ESP32 core's own bundled qrcode.h (used for WiFi provisioning)
#include "cp437_utf8.h" // CP437 <-> UTF-8 conversion for the Remote Config web page (web_config.ino)
#include <math.h> // roundf(), used when drawing the WiFi QR code at an arbitrary pixel size // Library Manager: "ArduinoJson" by Benoit Blanchon - used only by the
                          // Config Mode web server (web_config.ino) to parse/build the config page's requests

// ---- Pins ----
const int BUTTON_PIN   = D1;
const int BUTTON_LED   = D3;
const int BUZZER_PIN    = D2;
const int ENCODER_CLK  = D10;
const int ENCODER_DT   = D8;
const int ENCODER_SW   = D9;

// ---- OLED ----
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_I2C_ADDR 0x3C
Adafruit_SH1106G display = Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ---- Splash screen: tiger logo bitmaps (80x53), closed-mouth and open-mouth "roar" frames ----
const int SPLASH_TIGER_WIDTH = 80;
const int SPLASH_TIGER_HEIGHT = 53;
// 80x53 pixels, 530 bytes each
const unsigned char PROGMEM TIGER_BITMAP[] = {
  0x00, 0x00, 0xFC, 0x00, 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00, 0x01,
  0x8E, 0x00, 0x00, 0x00, 0x00, 0x71, 0x80, 0x00, 0x00, 0x01, 0x03, 0x80,
  0x3F, 0xFC, 0x01, 0xE0, 0x80, 0x00, 0x00, 0x01, 0x01, 0xE0, 0x3F, 0xFC,
  0x07, 0xC0, 0xC0, 0x00, 0x00, 0x01, 0x39, 0xF1, 0xC3, 0xC3, 0x8F, 0x9C,
  0xC0, 0x00, 0x00, 0x01, 0x3C, 0xFF, 0x03, 0xC0, 0x7F, 0x1C, 0x80, 0x00,
  0x00, 0x01, 0x3C, 0x3C, 0x7F, 0xFE, 0x1E, 0x3C, 0xC0, 0x00, 0x00, 0x01,
  0x9F, 0x31, 0xFF, 0xFF, 0x8C, 0x7D, 0x80, 0x00, 0x00, 0x00, 0x9F, 0xE2,
  0x03, 0xC0, 0x47, 0xF9, 0x00, 0x00, 0x00, 0x00, 0xCF, 0x80, 0x0F, 0xF0,
  0x00, 0xF3, 0x00, 0x00, 0x00, 0x00, 0x6E, 0x00, 0x1F, 0xFC, 0x00, 0x76,
  0x00, 0x00, 0x00, 0x00, 0x3C, 0x30, 0x79, 0xCE, 0x04, 0x3C, 0x00, 0x00,
  0x00, 0x00, 0x38, 0x78, 0xC3, 0xC3, 0x1E, 0x1C, 0x00, 0x00, 0x00, 0x00,
  0x70, 0xC0, 0x07, 0xE0, 0x01, 0x0E, 0x00, 0x00, 0x00, 0x00, 0x61, 0x80,
  0x1F, 0xF8, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0xC3, 0xC0, 0x39, 0xDC,
  0x03, 0xC3, 0x00, 0x00, 0x00, 0x01, 0xD3, 0xC0, 0x21, 0xC4, 0x03, 0xCB,
  0x80, 0x00, 0x00, 0x03, 0xE7, 0xF0, 0x03, 0xC0, 0x07, 0xE7, 0xC0, 0x00,
  0x00, 0x07, 0xE4, 0x78, 0x07, 0xE0, 0x1F, 0x27, 0xE0, 0x00, 0x00, 0x07,
  0xC0, 0x7E, 0x04, 0x30, 0x3F, 0x03, 0xE0, 0x00, 0x00, 0x01, 0xC0, 0x6F,
  0x00, 0x00, 0xF7, 0x03, 0x80, 0x00, 0x00, 0x03, 0x80, 0x6D, 0xC0, 0x03,
  0xB6, 0x01, 0xC0, 0x00, 0x00, 0x03, 0x83, 0x37, 0x70, 0x0E, 0xEE, 0x40,
  0xC0, 0x00, 0x00, 0x07, 0x02, 0x38, 0xF0, 0x0F, 0x1C, 0x40, 0xE0, 0x00,
  0x00, 0x07, 0x33, 0x0F, 0xF0, 0x0F, 0xF0, 0xC4, 0xE0, 0x00, 0x00, 0x0F,
  0x73, 0x87, 0x30, 0x04, 0xE1, 0xC6, 0xF0, 0x00, 0x00, 0x0F, 0xE3, 0x80,
  0x60, 0x06, 0x01, 0xC3, 0xF8, 0x00, 0x00, 0x0B, 0xE3, 0xE0, 0x40, 0x02,
  0x07, 0xC3, 0xD8, 0x00, 0x00, 0x03, 0xE1, 0xFE, 0x00, 0x00, 0x3F, 0x83,
  0xC0, 0x00, 0x00, 0x03, 0xE0, 0xFC, 0x00, 0x00, 0x3F, 0x03, 0xE0, 0x00,
  0x00, 0x0F, 0xF8, 0x38, 0x00, 0x00, 0x1E, 0x1F, 0xF0, 0x00, 0x00, 0x7C,
  0x7E, 0x38, 0x38, 0x1C, 0x0C, 0x3E, 0x3F, 0x00, 0x01, 0x80, 0x3E, 0x70,
  0x1F, 0xF8, 0x0F, 0x7C, 0x01, 0xC0, 0x03, 0x00, 0x0F, 0xF8, 0x0F, 0xF0,
  0x0F, 0xF0, 0x00, 0x60, 0x0C, 0x00, 0x07, 0xFC, 0x07, 0xE0, 0x1F, 0xE0,
  0x00, 0x30, 0x18, 0x00, 0x03, 0xFE, 0x00, 0x00, 0x7F, 0xC0, 0x00, 0x18,
  0x30, 0x00, 0x01, 0xFE, 0x00, 0x00, 0x7F, 0x80, 0x00, 0x0C, 0x60, 0x00,
  0x00, 0xF0, 0x7F, 0xFE, 0x0F, 0x80, 0x00, 0x06, 0xC0, 0x63, 0x08, 0xE3,
  0xFF, 0xFF, 0xC7, 0x98, 0xC6, 0x03, 0xCC, 0x61, 0x8C, 0xCF, 0xFF, 0xFF,
  0xF3, 0xB1, 0xC6, 0x31, 0xCC, 0x61, 0xCF, 0x8F, 0xFF, 0xFF, 0xF1, 0xB3,
  0xC7, 0x3B, 0xDC, 0x73, 0xCF, 0x0F, 0xFF, 0xFF, 0xF0, 0xF3, 0xCE, 0x39,
  0xDC, 0xF1, 0xDE, 0x63, 0xFF, 0xFF, 0xC6, 0x7B, 0xCF, 0x39, 0xDD, 0xE3,
  0xD8, 0xF0, 0xFF, 0xFF, 0x0F, 0x19, 0xC7, 0xBB, 0x7D, 0xE3, 0xF1, 0xB0,
  0x00, 0x00, 0x1F, 0x8F, 0xC7, 0xBE, 0x1D, 0xB2, 0x63, 0xB0, 0x00, 0x00,
  0xFD, 0xC6, 0x4D, 0xBC, 0x07, 0x9E, 0x03, 0xB0, 0xC0, 0x03, 0xFD, 0xC0,
  0x79, 0xE0, 0x03, 0x8E, 0x03, 0x9C, 0xFF, 0xFF, 0xF9, 0xC0, 0x71, 0xC0,
  0x00, 0x00, 0x03, 0xCF, 0xFF, 0xFF, 0xF3, 0xC0, 0x00, 0x00, 0x00, 0x00,
  0x01, 0xE0, 0xFF, 0xFF, 0x07, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7C,
  0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7F, 0x00, 0x00,
  0xFE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F, 0xFF, 0xFF, 0xF8, 0x00,
  0x00, 0x00,
};

// ---- Splash screen: "TIGER"/"TIMER" wordmark bitmaps (40x8 each), rendered in the Press Start 2P
// pixel font, used for the slide-in animation ----
const int SPLASH_WORD_WIDTH = 40;
const int SPLASH_WORD_HEIGHT = 8;
// 40x8 pixels each, 40 bytes each
const unsigned char PROGMEM WORD_TIGER_BITMAP[] = {
  0x7E, 0x7E, 0x3E, 0xFE, 0xFC, 0x18, 0x18, 0x60, 0xC0, 0xC6, 0x18, 0x18,
  0xC0, 0xC0, 0xC6, 0x18, 0x18, 0xCE, 0xFC, 0xCE, 0x18, 0x18, 0xC6, 0xC0,
  0xF8, 0x18, 0x18, 0x66, 0xC0, 0xDC, 0x18, 0x7E, 0x3E, 0xFE, 0xCE, 0x00,
  0x00, 0x00, 0x00, 0x00,
};

const unsigned char PROGMEM WORD_TIMER_BITMAP[] = {
  0x7E, 0x7E, 0xC6, 0xFE, 0xFC, 0x18, 0x18, 0xEE, 0xC0, 0xC6, 0x18, 0x18,
  0xFE, 0xC0, 0xC6, 0x18, 0x18, 0xD6, 0xFC, 0xCE, 0x18, 0x18, 0xD6, 0xC0,
  0xF8, 0x18, 0x18, 0xC6, 0xC0, 0xDC, 0x18, 0x7E, 0xC6, 0xFE, 0xCE, 0x00,
  0x00, 0x00, 0x00, 0x00,
};



// ---- Game modes ----
const int GAME_TIMER      = 0;
const int GAME_REFLEX     = 1;
const int GAME_DICE       = 2;
const int GAME_CHESS      = 3;
const int GAME_HOTPOTATO  = 4; // "Pass the Bomb" - internal name kept from its original Hot Potato build
const int GAME_TURNORDER  = 5;
const int GAME_STOPWATCH  = 6;
const int GAME_TRIVIA     = 7;
const int GAME_TOOTHBRUSH = 8;
const int GAME_TETRIS     = 9;
const int GAME_STARDROP   = 10;
const int GAME_SNAKE      = 11;
const int GAME_SIMPLEMORSE = 12;
const int GAME_COUNT      = 13;
int currentGame = GAME_TIMER;

const int MENU_SETTINGS = 100; // sentinel: a menu entry that isn't a playable game
const int MENU_INSTRUCTIONS = 101; // sentinel: another non-game entry, the global instructions screen
const int MENU_COUNT = 15;
// Default/fallback order, used on first boot and whenever a saved order in flash is missing or invalid.
const int DEFAULT_MENU_ORDER[MENU_COUNT] = {GAME_TIMER, GAME_TOOTHBRUSH, GAME_DICE, GAME_REFLEX, GAME_CHESS, GAME_HOTPOTATO, GAME_TURNORDER, GAME_STOPWATCH, GAME_TRIVIA, GAME_TETRIS, GAME_STARDROP, GAME_SNAKE, GAME_SIMPLEMORSE, MENU_INSTRUCTIONS, MENU_SETTINGS};
// Actual display order - no longer const: Config Mode (web_config.ino) can rewrite this at runtime
// and persist it to flash. Everything that reads MENU_ORDER elsewhere in the sketch is unaffected,
// since it's still just an array of GAME_* ids in display order.
int MENU_ORDER[MENU_COUNT];

// Filtered view of MENU_ORDER with disabled games left out (Settings is never filtered out) - this,
// not MENU_ORDER directly, is what the menu screen actually navigates/draws. Rebuilt by
// rebuildVisibleMenu() (web_config.ino) whenever MENU_ORDER or a game's enabled state changes.
int visibleMenuOrder[MENU_COUNT];
int visibleMenuCount = MENU_COUNT;

// Per-game enable/disable, indexed by GAME_* id. A disabled game is skipped in the on-device
// menu (see visibleMenuOrder above) but keeps its saved position in MENU_ORDER, so re-enabling
// it later puts it back where it was. Settings can't be disabled and isn't represented here.
bool gameEnabled[GAME_COUNT];

int menuPositionForGame(int gameId) {
  for (int i = 0; i < visibleMenuCount; i++) {
    if (visibleMenuOrder[i] == gameId) return i;
  }
  return 0;
}

bool inMenu = false;
int menuSelection = GAME_TIMER;

// ---- "Go back to menu?" confirmation - shown before actually leaving a game, so an accidental
// long-press can't lose game progress ----
bool inLeaveConfirm = false;
int leaveConfirmSelection = 0; // 0 = Yes (default), 1 = No

// ---- Instructions screen (top of the main menu) ----
bool inInstructions = false;

// ---- Settings submenu ----
const int SETTINGS_LANGUAGE    = 0;
const int SETTINGS_TIMER_STEP  = 1;
const int SETTINGS_CONFIG_MODE = 2; // opens Remote Config: starts the WiFi AP + web config server
const int SETTINGS_COUNT = 3;
bool inSettingsMenu = false;
int settingsSelection = SETTINGS_LANGUAGE;

// ---- Language / translations ----
const int LANG_EN = 0;
const int LANG_FR = 1;
const int LANG_ES = 2;
const int LANG_COUNT = 3;
int currentLanguage = LANG_EN; // overwritten from flash in setup()
bool inLanguageSelect = false;

// ---- Timer step selection ----
const long TIMER_STEP_AUTO = 0; // sentinel: 0 means "use the same ramped step as the Chess clock"
const long TIMER_STEP_OPTIONS[] = {TIMER_STEP_AUTO, 1, 5, 10, 30, 60};
const int TIMER_STEP_OPTION_COUNT = 6;
const int TIMER_STEP_DEFAULT_INDEX = 0; // Auto
int currentTimerStepIndex = TIMER_STEP_DEFAULT_INDEX; // overwritten from flash in setup()
bool inTimerStepSelect = false;

Preferences prefs;

const char* LANGUAGE_NAMES[LANG_COUNT] = {"English", "Fran\x87" "ais", "Espa\xA4ol"}; // native names, never translated

const int STR_SELECT_GAME = 0;
const int STR_MENU_TIMER = 1;
const int STR_MENU_DICE = 2;
const int STR_MENU_REFLEX = 3;
const int STR_MENU_LANGUAGE = 4;
const int STR_SET_TIME = 5;
const int STR_READY = 6;
const int STR_GO_STATUS = 7;
const int STR_GO_BIG = 8;
const int STR_TOO_SOON = 9;
const int STR_WAIT_BIG = 10;
const int STR_DONT_HIT_YET = 11;
const int STR_PRESS_TO_START = 12;
const int STR_BEST = 13;
const int STR_CHOOSE_DICE_TYPE = 14;
const int STR_PRESS_TO_ROLL = 15;
const int STR_TOTAL = 16;
const int STR_ROLLING = 17;
const int STR_CHOOSE_LANGUAGE = 18;
const int STR_MENU_SETTINGS = 19;
const int STR_SETTINGS_TITLE = 20;
const int STR_MENU_TIMER_STEP = 21;
const int STR_CHOOSE_TIMER_STEP = 22;
const int STR_MENU_CHESS = 23;
const int STR_CHOOSE_START_TIME = 24;
const int STR_CHOOSE_INCREMENT = 25;
const int STR_PAUSED = 26;
const int STR_OUT_OF_TIME = 27;
const int STR_PUSH = 28;
const int STR_AUTO = 29;
const int STR_PLAYER = 30;
const int STR_WHITE_STARTING = 31;
const int STR_WHITE_TURN = 32;
const int STR_BLACK_TURN = 33;
const int STR_WHITE = 34;
const int STR_BLACK = 35;
const int STR_MENU_HOTPOTATO = 36;
const int STR_HP_ARM = 37;
const int STR_HP_ARMED = 38;
const int STR_HP_BOOM = 39;
const int STR_MENU_TURNORDER = 40;
const int STR_CHOOSE_PLAYERS = 41;
const int STR_GOES_FIRST = 42;
const int STR_SPINNING = 43;
const int STR_MENU_STOPWATCH = 44;
const int STR_LAP = 45;
const int STR_MENU_TRIVIA = 46;
const int STR_CHOOSE_CATEGORY = 47;
const int STR_CHOOSE_LEVEL = 48;
const int STR_CAT_FOOTBALL = 49;
const int STR_CAT_GEOGRAPHY = 50;
const int STR_CAT_SCIENCE = 51;
const int STR_CAT_HISTORY = 52;
const int STR_LEVEL_JUNIOR = 53;
const int STR_LEVEL_CURIOUS = 54;
const int STR_LEVEL_MASTER = 55;
const int STR_CAT_RANDOM = 56;
const int STR_CAT_GENERAL = 57;
const int STR_CAT_MYTHOLOGY = 58;
const int STR_CAT_ART = 59;
const int STR_CAT_ANIMALS = 60;
const int STR_QUESTION_LABEL = 61;
const int STR_ANSWERS_LABEL = 62;
const int STR_TURN_ANSWERS = 63;
const int STR_TURN_QUESTION = 64;
const int STR_CLICK_NEXT = 65;
const int STR_MENU_TOOTHBRUSH = 66;
const int STR_CHOOSE_BRUSH_TIME = 67;
const int STR_ZONE_UPPER = 68; // toothbrush zone label, line 1
const int STR_ZONE_LOWER = 69;
const int STR_ZONE_LEFT = 70;  // toothbrush zone label, line 2
const int STR_ZONE_RIGHT = 71;
const int STR_BRUSHING_DONE = 72;
const int STR_MENU_CONFIG_MODE = 73;
const int STR_CONFIG_STARTING = 74;
const int STR_CONFIG_WIFI_LABEL = 75;
const int STR_CONFIG_CONNECT_OR = 76;
const int STR_CONFIG_VISIT = 77;
const int STR_CONFIG_PRESS_EXIT = 78; // no longer shown on the Config Mode screen itself (no room next to the QR code), kept for potential reuse
const int STR_MENU_INSTRUCTIONS = 79;
const int STR_LEAVE_CONFIRM_TITLE = 80;
const int STR_YES = 81;
const int STR_NO = 82;
const int STR_INSTR_MENU = 83;
const int STR_INSTR_VOLUME = 84;
const int STR_INSTR_SETTINGS = 85;
const int STR_TRIVIA_PREV = 86;
const int STR_TRIVIA_ANS = 87;
const int STR_TRIVIA_QUES = 88;
const int STR_TRIVIA_NEXT = 89;
const int STR_MENU_TETRIS = 90;
const int STR_TETRIS_PRESS_START = 91;
const int STR_TETRIS_GAME_OVER = 92;
const int STR_TETRIS_SCORE = 93;
const int STR_MENU_STARDROP = 94;
const int STR_STARDROP_PRESS_START = 95;
const int STR_STARDROP_GAME_OVER = 96;
const int STR_STARDROP_SCORE = 97;
const int STR_STARDROP_HIGH = 98;
const int STR_HP_STARTING_WITH = 99; // "starting with" (joins category + letter, e.g. "Country starting with A")
const int STR_HP_CAT_COUNTRIES = 100;
const int STR_HP_CAT_CITIES = 101;
const int STR_HP_CAT_FOOD = 102;
const int STR_HP_CAT_CELEBRITIES = 103;
const int STR_HP_CAT_CARTOON = 104;
const int STR_HP_CAT_HOME = 105;
const int STR_HP_CAT_SCHOOL = 106;
const int STR_HP_CAT_WEAR = 107;
const int STR_HP_CAT_SPORTS = 108;
const int STR_HP_CAT_BODYPARTS = 109;
const int STR_HP_CAT_INSTRUMENTS = 110;
const int STR_HP_CAT_RED = 111;
const int STR_HP_CAT_BLUE = 112;
const int STR_HP_CAT_GREEN = 113;
const int STR_HP_CAT_COLD = 114;
const int STR_HP_CAT_WARM = 115;
const int STR_HP_CAT_FLY = 116;
const int STR_HP_CAT_COLOURS = 117;
const int STR_HP_CAT_NOISE = 118;
const int STR_HP_CAT_BREAK = 119;
const int STR_HP_LETTER_LABEL = 120;
const int STR_HP_ROTATE_HINT = 121;
const int STR_BRUSH_ACTIVE = 122;
const int STR_MENU_SNAKE = 123;
const int STR_MENU_SIMPLEMORSE = 124;
const int STR_SIMON_SENTENCE_LABEL = 125;
const int STR_SIMON_WATCH = 126;
const int STR_SIMON_YOUR_TURN = 127;
const int STR_SIMON_COMPLETE = 128;
const int STR_SIMON_ROUND = 129;
const int STR_ZONE_FRONT = 130; // toothbrush zone label, line 2
const int NUM_STRINGS = 131;

const char* TRANSLATIONS[NUM_STRINGS][LANG_COUNT] = {
  /* STR_SELECT_GAME          */{"Choose Option", "Choisir option", "Elegir opcion"},
  /* STR_MENU_TIMER           */{"Timer", "Minuteur", "Temporizador"},
  /* STR_MENU_DICE            */{"Dice", "D\x82s", "Dados"},
  /* STR_MENU_REFLEX          */{"Reflex", "R\x82" "flexe", "Reflejos"},
  /* STR_MENU_LANGUAGE        */{"Language", "Langue", "Idioma"},
  /* STR_SET_TIME             */{"Set time", "R\x82gler temps", "Ajustar tiempo"},
  /* STR_READY                */{"Ready", "Pr\x88t", "Listo"},
  /* STR_GO_STATUS            */{"GO!", "GO !", "YA!"},
  /* STR_GO_BIG               */{"GO", "GO", "YA"},
  /* STR_TOO_SOON             */{"TOO SOON", "TROP TOT", "MUY PRONTO"},
  /* STR_WAIT_BIG             */{"WAIT", "ATTENDS", "ESPERA"},
  /* STR_DONT_HIT_YET         */{"Don't hit yet!", "N'appuie pas !", "No pulses aun!"},
  /* STR_PRESS_TO_START       */{"Press to start", "Appuyez pour jouer", "Pulsa para empezar"},
  /* STR_BEST                 */{"Best", "Meilleur", "Mejor"},
  /* STR_CHOOSE_DICE_TYPE     */{"Choose Dice Type", "Type de d\x82", "Tipo de dado"},
  /* STR_PRESS_TO_ROLL        */{"Press to roll", "Appuyez lancer", "Pulsa para tirar"},
  /* STR_TOTAL                */{"Total", "Total", "Total"},
  /* STR_ROLLING              */{"Rolling...", "Lancer...", "Tirando..."},
  /* STR_CHOOSE_LANGUAGE      */{"Choose Language", "Choisir langue", "Elegir idioma"},
  /* STR_MENU_SETTINGS        */{"Settings", "Param\x8Atres", "Ajustes"},
  /* STR_SETTINGS_TITLE       */{"Settings", "Param\x8Atres", "Ajustes"},
  /* STR_MENU_TIMER_STEP      */{"Timer Step", "Intervalle", "Intervalo"},
  /* STR_CHOOSE_TIMER_STEP    */{"Choose Timer Step", "Choisir intervalle", "Elegir intervalo"},
  /* STR_MENU_CHESS           */{"Chess Clock", "Horloge \x82" "checs", "Reloj ajedrez"},
  /* STR_CHOOSE_START_TIME    */{"Choose Start Time", "Choisir temps", "Elegir tiempo"},
  /* STR_CHOOSE_INCREMENT     */{"Choose Increment", "Choisir incr\x82ment", "Elegir incremento"},
  /* STR_PAUSED               */{"Paused", "Pause", "Pausado"},
  /* STR_OUT_OF_TIME          */{"out of time", "temps \x82" "coul\x82", "sin tiempo"},
  /* STR_PUSH                 */{"Push", "Pousse", "Pulsa"},
  /* STR_AUTO                 */{"Auto", "Auto", "Auto"},
  /* STR_PLAYER               */{"Player", "Joueur", "Jugador"},
  /* STR_WHITE_STARTING       */{"White is starting", "Blancs commencent", "Empiezan blancas"},
  /* STR_WHITE_TURN           */{"White Turn", "Tour blancs", "Turno blancas"},
  /* STR_BLACK_TURN           */{"Black Turn", "Tour noirs", "Turno negras"},
  /* STR_WHITE                */{"White", "Blancs", "Blancas"},
  /* STR_BLACK                */{"Black", "Noirs", "Negras"},
  /* STR_MENU_HOTPOTATO       */{"Pass the Bomb", "Passe la bombe", "Pasa la bomba"},
  /* STR_HP_ARM               */{"Press to arm", "Appuyez pour armer", "Pulsa para armar"},
  /* STR_HP_ARMED             */{"Pass it around!", "Faites tourner !", "Pasala rapido!"},
  /* STR_HP_BOOM              */{"BOOM!", "BOOM !", "BOOM!"},
  /* STR_MENU_TURNORDER       */{"Turn Order", "Ordre de jeu", "Orden de turno"},
  /* STR_CHOOSE_PLAYERS       */{"Choose Players", "Choisir joueurs", "Elegir jugadores"},
  /* STR_GOES_FIRST           */{"goes first!", "commence !", "empieza!"},
  /* STR_SPINNING             */{"Spinning...", "Tirage...", "Girando..."},
  /* STR_MENU_STOPWATCH       */{"Stopwatch", "Chronom\x8Atre", "Cronometro"},
  /* STR_LAP                  */{"Lap", "Tour", "Vuelta"},
  /* STR_MENU_TRIVIA          */{"Trivia", "Quiz", "Trivia"},
  /* STR_CHOOSE_CATEGORY      */{"Choose Category", "Choisir cat\x82gorie", "Elegir categoria"},
  /* STR_CHOOSE_LEVEL         */{"Choose Level", "Choisir niveau", "Elegir nivel"},
  /* STR_CAT_FOOTBALL         */{"Sports", "Sport", "Deportes"},
  /* STR_CAT_GEOGRAPHY        */{"Geography", "G\x82ographie", "Geografia"},
  /* STR_CAT_SCIENCE          */{"Science", "Sciences", "Ciencias"},
  /* STR_CAT_HISTORY          */{"History", "Histoire", "Historia"},
  /* STR_LEVEL_JUNIOR         */{"Junior", "Junior", "Junior"},
  /* STR_LEVEL_CURIOUS        */{"Curious", "Curieux", "Curioso"},
  /* STR_LEVEL_MASTER         */{"Master", "Maitre", "Maestro"},
  /* STR_CAT_RANDOM           */{"Random", "Al\x82" "atoire", "Aleatorio"},
  /* STR_CAT_GENERAL          */{"General", "General", "General"},
  /* STR_CAT_MYTHOLOGY        */{"Mythology", "Mythologie", "Mitologia"},
  /* STR_CAT_ART              */{"Art", "Art", "Arte"},
  /* STR_CAT_ANIMALS          */{"Animals", "Animaux", "Animales"},
  /* STR_QUESTION_LABEL       */{"Question", "Question", "Pregunta"},
  /* STR_ANSWERS_LABEL        */{"Answers", "Reponses", "Respuestas"},
  /* STR_TURN_ANSWERS         */{"Q/A", "Q/R", "P/R"},
  /* STR_TURN_QUESTION        */{"Turn for Question", "Tourner: Question", "Girar: Pregunta"},
  /* STR_CLICK_NEXT           */{"Next", "Suivant", "Siguiente"},
  /* STR_MENU_TOOTHBRUSH      */{"Toothbrush", "Brossage", "Cepillado"},
  /* STR_CHOOSE_BRUSH_TIME    */{"Choose brush time", "Choisir la duree", "Elegir duracion"},
  /* STR_ZONE_UPPER           */{"Upper", "Haut", "Arriba"},
  /* STR_ZONE_LOWER           */{"Lower", "Bas", "Abajo"},
  /* STR_ZONE_LEFT            */{"left", "gauche", "izqda."},
  /* STR_ZONE_RIGHT           */{"right", "droite", "derecha"},
  /* STR_BRUSHING_DONE        */{"All done!", "Termin\x82 !", "Listo!"},
  /* STR_MENU_CONFIG_MODE     */{"Remote Config", "Config distante", "Config remota"},
  /* STR_CONFIG_STARTING      */{"Starting WiFi...", "Demarrage WiFi...", "Iniciando WiFi..."},
  /* STR_CONFIG_WIFI_LABEL    */{"WiFi:", "WiFi :", "WiFi:"},
  /* STR_CONFIG_CONNECT_OR    */{"Connect or", "Connectez ou", "Conecta o"},
  /* STR_CONFIG_VISIT         */{"visit", "visitez", "visita"},
  /* STR_CONFIG_PRESS_EXIT    */{"Press to exit", "Appuyez pour quitter", "Pulsa para salir"},
  /* STR_MENU_INSTRUCTIONS    */{"Instructions", "Instructions", "Instrucciones"},
  /* STR_LEAVE_CONFIRM_TITLE  */{"Go back to menu?", "Retour au menu ?", "Volver al menu?"},
  /* STR_YES                  */{"Yes", "Oui", "Si"},
  /* STR_NO                   */{"No", "Non", "No"},
  /* STR_INSTR_MENU           */{"Menu: hold knob 1s", "Menu: tenir 1s", "Menu: mant. 1s"},
  /* STR_INSTR_VOLUME         */{"Volume: hold+turn", "Volume: tenir+tourner", "Volumen: mant.+gira"},
  /* STR_INSTR_SETTINGS       */{"More: see Settings", "Plus: voir Reglages", "Mas: ver Ajustes"},
  /* STR_TRIVIA_PREV          */{"Prev", "Prec.", "Ant."},
  /* STR_TRIVIA_ANS           */{"Ans", "Rep.", "Resp."},
  /* STR_TRIVIA_QUES          */{"Ques", "Quest.", "Preg."},
  /* STR_TRIVIA_NEXT          */{"Next", "Suiv.", "Sig."},
  /* STR_MENU_TETRIS          */{"Tetris", "Tetris", "Tetris"},
  /* STR_TETRIS_PRESS_START   */{"Press to start", "Appuyez pour jouer", "Pulsa para jugar"},
  /* STR_TETRIS_GAME_OVER     */{"Game Over", "Partie terminee", "Fin del juego"},
  /* STR_TETRIS_SCORE         */{"Score", "Score", "Puntos"},
  /* STR_MENU_STARDROP        */{"Star Drop", "Star Drop", "Star Drop"},
  /* STR_STARDROP_PRESS_START */{"Press to start", "Appuyez pour jouer", "Pulsa para jugar"},
  /* STR_STARDROP_GAME_OVER   */{"Game Over", "Partie terminee", "Fin del juego"},
  /* STR_STARDROP_SCORE       */{"Score", "Score", "Puntos"},
  /* STR_STARDROP_HIGH        */{"Best", "Record", "Record"},
  /* STR_HP_STARTING_WITH     */{"starting with", "qui commence par", "que empiece con"},
  /* STR_HP_CAT_COUNTRIES     */{"Country", "Pays", "Pais"},
  /* STR_HP_CAT_CITIES        */{"City", "Ville", "Ciudad"},
  /* STR_HP_CAT_FOOD          */{"Food", "Nourriture", "Comida"},
  /* STR_HP_CAT_CELEBRITIES   */{"Celebrity", "Celebrite", "Famoso"},
  /* STR_HP_CAT_CARTOON       */{"Cartoon character", "Perso. de dessin anime", "Personaje de dibujos"},
  /* STR_HP_CAT_HOME          */{"Thing at home", "Objet de la maison", "Cosa de casa"},
  /* STR_HP_CAT_SCHOOL        */{"Thing at school", "Objet de l'ecole", "Cosa del colegio"},
  /* STR_HP_CAT_WEAR          */{"Thing you wear", "Vetement", "Cosa que te pones"},
  /* STR_HP_CAT_SPORTS        */{"Sport", "Sport", "Deporte"},
  /* STR_HP_CAT_BODYPARTS     */{"Body part", "Partie du corps", "Parte del cuerpo"},
  /* STR_HP_CAT_INSTRUMENTS   */{"Musical instrument", "Instrument de musique", "Instrumento musical"},
  /* STR_HP_CAT_RED           */{"Red thing", "Chose rouge", "Cosa roja"},
  /* STR_HP_CAT_BLUE          */{"Blue thing", "Chose bleue", "Cosa azul"},
  /* STR_HP_CAT_GREEN         */{"Green thing", "Chose verte", "Cosa verde"},
  /* STR_HP_CAT_COLD          */{"Cold thing", "Chose froide", "Cosa fria"},
  /* STR_HP_CAT_WARM          */{"Warm thing", "Chose chaude", "Cosa caliente"},
  /* STR_HP_CAT_FLY           */{"Thing that flies", "Chose qui vole", "Cosa que vuela"},
  /* STR_HP_CAT_COLOURS       */{"Colour", "Couleur", "Color"},
  /* STR_HP_CAT_NOISE         */{"Thing that's noisy", "Chose qui fait du bruit", "Cosa que hace ruido"},
  /* STR_HP_CAT_BREAK         */{"Thing that breaks", "Chose qui se casse", "Cosa que se rompe"},
  /* STR_HP_LETTER_LABEL      */{"Letter", "Lettre", "Letra"},
  /* STR_HP_ROTATE_HINT       */{"Rotate: shuffle", "Tourner: m\x82langer", "Girar: mezclar"},
  /* STR_BRUSH_ACTIVE         */{"Brush!", "Brosse !", "Cepilla!"},
  /* STR_MENU_SNAKE           */{"Snake", "Snake", "Snake"},
  /* STR_MENU_SIMPLEMORSE     */{"SimpleMorse", "SimpleMorse", "SimpleMorse"},
  /* STR_SIMON_SENTENCE_LABEL */{"Word in Morse", "Mot en morse", "Palabra en morse"},
  /* STR_SIMON_WATCH          */{"Watch!", "Regarde !", "Mira!"},
  /* STR_SIMON_YOUR_TURN      */{"Your turn", "A toi", "Tu turno"},
  /* STR_SIMON_COMPLETE       */{"Complete!", "Reussi !", "Completo!"},
  /* STR_SIMON_ROUND          */{"Round", "Manche", "Ronda"},
  /* STR_ZONE_FRONT           */{"front", "devant", "frente"},

};

const char* T(int stringId) {
  if (stringId < 0 || stringId >= NUM_STRINGS) return "";
  int lang = constrain(currentLanguage, 0, LANG_COUNT - 1);
  return TRANSLATIONS[stringId][lang];
}

// ---- Timer settings ----
const long DEFAULT_TIME = 120; // 2:00
const long MIN_TIME     = 10;  // 0:10
const long MAX_TIME     = 5400; // 90:00

long presetTime    = DEFAULT_TIME; // chosen length, used each time the button starts a run
long remainingTime = DEFAULT_TIME; // seconds left, counts down while running
long runStartTime  = DEFAULT_TIME; // snapshot of remainingTime at the moment the run started (for blink-rate %)
bool running = false;
bool timerPaused = false; // pauses the countdown without resetting it, toggled by the encoder's click

unsigned long lastSecondTick = 0;

// blink state
bool ledBlinkState = false;
unsigned long lastBlinkToggle = 0;

// button debounce
int lastButtonFlicker = HIGH; // raw reading from the previous loop pass
int buttonState       = HIGH; // confirmed/debounced state
unsigned long lastButtonEdgeTime = 0;
const unsigned long BUTTON_DEBOUNCE_MS = 20; // raised back up from 10ms after flaky/double-trigger behavior
unsigned long buttonPressStart = 0;
bool buttonPressConsumed = false; // true once this press already did its job on the way DOWN (e.g. a Reflex
                                  // false start, or waking the screen) - its release then dispatches nothing

// encoder state
int lastClkState;

// encoder push-button: plain click = context action (confirm / pause / etc); hold+turn = volume;
// hold 1s alone (no turn) = open/close the menu
int lastSwFlicker = HIGH;
int swState        = HIGH;
unsigned long lastSwEdgeTime = 0;
const unsigned long SW_DEBOUNCE_MS = 40;
bool swClaimedByVolume = false; // true once a rotation-while-held adjusts volume, so release doesn't also fire a click
unsigned long swPressStart = 0;
bool swLongFired = false; // true once the 1s hold has already opened/closed the menu, so release doesn't also fire a click
bool swWakeConsumed = false; // true when this hold woke the sleeping screen - it then fires no click or long-press
const unsigned long ENCODER_LONG_PRESS_MS = 1000;

// ---- Reflex game state ----
const int REFLEX_READY   = 0; // waiting for a button press to arm a round
const int REFLEX_WAITING = 1; // armed, light not on yet - pressing now is a false start
const int REFLEX_LIT     = 2; // light is on, waiting for the hit
int reflexState = REFLEX_READY;
unsigned long reflexArmTime = 0;
unsigned long reflexDelay = 0;
unsigned long reflexLightOnTime = 0;
long lastReactionMs = -1;      // -1 = no result yet
long reflexHighScoreMs = -1;   // -1 = no best time yet this session
unsigned long falseStartUntil = 0; // show "Too soon!" until this millis() timestamp
const unsigned long REFLEX_MIN_DELAY_MS = 1500;
const unsigned long REFLEX_MAX_DELAY_MS = 4000;
const unsigned long FALSE_START_DISPLAY_MS = 1500;

// ---- Dice Roll state ----
const int DICE_SIDES[]  = {4, 6, 8, 10, 12, 20};
const int DICE_TYPE_COUNT = 6;
const int MAX_DICE_COUNT = 5;

const int DICE_SELECT_TYPE = 0; // choosing the die type, shown right after entering the game
const int DICE_ROLLING     = 1; // main screen: encoder picks dice count, button rolls
int diceScreenState = DICE_SELECT_TYPE;

int currentDieIndex = 1;  // default d6
int diceCount = 1;        // how many dice to roll, 1-5

int diceResults[MAX_DICE_COUNT]; // results from the most recent roll
int diceResultSides = 6;         // die type used for that roll
int diceResultCount = 0;         // how many dice were rolled (0 = no roll yet since choosing the type)

// ---- Chess Timer state ----
const int CHESS_SETUP_TIME      = 0; // choosing starting minutes per player
const int CHESS_SETUP_INCREMENT = 1; // choosing the increment added back after each move
const int CHESS_PLAYING         = 2; // one player's clock running (or paused)
const int CHESS_GAME_OVER       = 3; // a player's clock hit 0
int chessScreenState = CHESS_SETUP_TIME;

long chessStartSeconds = 300; // 30 (0:30) to 5400 (90:00), stepped via rampedTimeStep() below
const long CHESS_START_MIN_SECONDS = 30;
const long CHESS_START_MAX_SECONDS = 5400; // 90:00
int chessIncrementSeconds = 0; // 0-60, steps of 10

long chessTimeP1 = 0; // seconds remaining, each player
long chessTimeP2 = 0;
int chessActivePlayer = 1; // 1 or 2 - whoever's clock is currently running
bool chessStarted = false; // false = showing P1's time, waiting for the first press to actually start
bool chessPaused = false;
unsigned long chessLastTick = 0;
long chessMaxP1 = 0; // highest time each player has ever had (starting time + increments) - bar reference
long chessMaxP2 = 0;
int chessLoser = 0; // 0 = no result yet, 1 or 2 = who ran out of time
unsigned long chessLockoutUntil = 0; // ignore arcade-button passes until this time, to absorb accidental double-taps
const unsigned long CHESS_PASS_LOCKOUT_MS = 400;

// ---- Hot Potato state ----
const int HP_READY   = 0; // idle, press to arm
const int HP_ARMED   = 1; // hidden countdown running, don't get caught
const int HP_EXPLODED = 2; // showing the result
int hpState = HP_READY;
unsigned long hpArmTime = 0;
unsigned long hpDelay = 0;
const unsigned long HP_MIN_DELAY_MS = 10000;
const unsigned long HP_MAX_DELAY_MS = 60000;
unsigned long hpLastBlinkToggle = 0; // steady LED blink while armed - deliberately NOT tied to time remaining
bool hpBlinkState = false;
const unsigned long HP_BLINK_INTERVAL_MS = 400;
unsigned long hpExplodedAt = 0; // when the bomb went off - drives the auto-reshuffle pause below
const unsigned long HP_EXPLODED_PAUSE_MS = 1000; // how long "BOOM!" stays up before auto-reshuffling

// Category + starting letter challenge (e.g. "Country starting with A"). Rerolled by turning the
// encoder while in HP_READY; fixed once armed. hpCategoryEnabled[] is loaded from/saved to flash
// via the web config page (web_config.ino), same pattern as gameEnabled[].
const int HP_CATEGORY_COUNT = 20;
bool hpCategoryEnabled[HP_CATEGORY_COUNT];
int hpCategory = 0;   // 0..HP_CATEGORY_COUNT-1
int hpLetter = 0;     // 0..25, maps to 'A'..'Z'

// ---- Turn Order state ----
int turnOrderPlayerCount = 4; // 2-10
int turnOrderResult = -1;     // -1 = no spin yet

// ---- Stopwatch state ----
const int STOPWATCH_READY   = 0; // 0:00, not started
const int STOPWATCH_RUNNING = 1;
const int STOPWATCH_STOPPED = 2; // paused, showing elapsed time
int stopwatchState = STOPWATCH_READY;
unsigned long stopwatchStartMillis = 0;
unsigned long stopwatchElapsedMs = 0; // accumulated time, updated on stop
long stopwatchLapSeconds = -1; // -1 = no lap recorded yet

// ---- Toothbrush state ----
const int TOOTHBRUSH_SELECT_TIME = 0;
const int TOOTHBRUSH_RUNNING     = 1;
const int TOOTHBRUSH_DONE        = 2;
int toothbrushScreenState = TOOTHBRUSH_SELECT_TIME;
int toothbrushMinutes = 2; // 1-5, default 2 (matches the standard dental recommendation)
long toothbrushTotalSeconds = 0;     // set when the run starts, used for the zone math
long toothbrushRemainingSeconds = 0;
bool toothbrushPaused = false;
unsigned long toothbrushLastTick = 0;       // for the 1-second countdown
unsigned long toothbrushLastBrushSound = 0; // for the periodic "scrub" click
bool toothbrushPulseState = false;          // toggles each click, for the LED + active-zone blink + brush scrub
int toothbrushLastZone = -1;                // tracks the previous zone, so a change can be detected and beeped
unsigned int toothbrushFrame = 0;           // counts scrub clicks, drives the foam-bubble positions
const int TOOTHBRUSH_ZONE_COUNT = 6;        // upper left/front/right, then lower left/front/right

// ---- Tetris state ----
// Board is drawn with the display temporarily rotated 90 degrees (see drawTetris*Screen), so the
// physical 128px-long axis becomes the vertical "falling" axis (16 rows) and the 64px axis becomes
// the board's width (8 columns) - a small but proportionally correct playing field for this screen.
const int TETRIS_BOARD_COLS = 8;
const int TETRIS_BOARD_ROWS = 15; // one row shorter than a full 16 to make room for the HUD strip below
const int TETRIS_CELL_SIZE = 8;   // pixels per cell
const int TETRIS_HUD_HEIGHT = 8;  // top strip (score/lines): 8 (HUD) + 15*8 (board) = 128 - exact fit

const int TETRIS_READY     = 0;
const int TETRIS_PLAYING   = 1;
const int TETRIS_GAME_OVER = 2;
int tetrisState = TETRIS_READY;

uint8_t tetrisBoard[TETRIS_BOARD_ROWS][TETRIS_BOARD_COLS]; // 0 = empty, 1 = filled

int tetrisPieceType = 0;     // 0-6: I, O, T, S, Z, J, L
int tetrisPieceRotation = 0; // 0-3
int tetrisPieceRow = 0;      // top-left of the piece's 4x4 bounding box, in board coordinates
int tetrisPieceCol = 0;

unsigned long tetrisLastDropTime = 0;
int tetrisScore = 0;
int tetrisHighScore = 0; // loaded from flash in setup()
int tetrisLinesCleared = 0;

// Tetromino shapes: 7 pieces x 4 rotations, each a 4x4 grid packed into 16 bits (bit index =
// row*4+col, bit set = filled). I and S and Z only have 2 visually distinct states, so their R2/R3
// just repeat R0/R1 - simpler table, and rotating them still "does something" rather than locking up.
const uint16_t TETRIS_SHAPES[7][4] = {
  { 0x00F0, 0x4444, 0x00F0, 0x4444 }, // I
  { 0x0066, 0x0066, 0x0066, 0x0066 }, // O
  { 0x0072, 0x0262, 0x0270, 0x0232 }, // T
  { 0x0036, 0x0462, 0x0036, 0x0462 }, // S
  { 0x0063, 0x0264, 0x0063, 0x0264 }, // Z
  { 0x0071, 0x0226, 0x0470, 0x0322 }, // J
  { 0x0074, 0x0622, 0x0170, 0x0223 }, // L
};

// ---- Star Drop state ----
// Same rotated-screen approach as Tetris: the panel's 128px axis becomes the vertical "falling"
// axis, the 64px axis becomes the 4 catching lanes (16px each).
const int STARDROP_LANES = 4;
const int STARDROP_LANE_WIDTH = 16;  // 4 * 16 = 64, exact fit
const int STARDROP_HUD_HEIGHT = 8;
const int STARDROP_CATCHER_Y = 116;  // top edge of the catcher's zone, near the bottom of the 128px play area
const int STARDROP_MAX_OBJECTS = 6;  // how many can be falling at once
const int STARDROP_START_LIVES = 3;

const int STARDROP_READY     = 0;
const int STARDROP_PLAYING   = 1;
const int STARDROP_GAME_OVER = 2;
int stardropState = STARDROP_READY;

int stardropCatcherLane = 1; // 0-3

bool stardropObjActive[STARDROP_MAX_OBJECTS];
int stardropObjLane[STARDROP_MAX_OBJECTS];
float stardropObjY[STARDROP_MAX_OBJECTS];
int stardropObjType[STARDROP_MAX_OBJECTS]; // 0=star, 1=planet, 2=moon

int stardropScore = 0;
int stardropHighScore = 0; // loaded from flash in setup()
int stardropCatches = 0;
int stardropLives = STARDROP_START_LIVES;

unsigned long stardropLastTick = 0;
unsigned long stardropLastSpawn = 0;

// ---- Snake state ----
// Plays in the normal (unrotated) orientation, unlike Tetris/Star Drop - a wide grid suits the
// panel's natural landscape shape fine, so there's no need for the setRotation(1) trick here.
const int SNAKE_CELL_SIZE = 4;   // pixels per cell
const int SNAKE_HUD_HEIGHT = 16; // content starts just below the standard header/divider at y=14
const int SNAKE_COLS = SCREEN_WIDTH / SNAKE_CELL_SIZE;                        // 32
const int SNAKE_ROWS = (SCREEN_HEIGHT - SNAKE_HUD_HEIGHT) / SNAKE_CELL_SIZE;  // 12
const int SNAKE_MAX_LENGTH = 200; // well under the 32*12=384 total cells - a very long snake already

const int SNAKE_READY     = 0;
const int SNAKE_PLAYING   = 1;
const int SNAKE_PAUSED    = 2;
const int SNAKE_GAME_OVER = 3;
int snakeState = SNAKE_READY;

// heading, as a compass-style index: 0=up, 1=right, 2=down, 3=left. Turning is always relative
// to this (encoder left/right rotates it -1/+1 mod 4) rather than absolute, which is what makes
// "turn left/turn right" a natural fit for a single rotary control. A single 90-degree turn can
// never reverse into the snake's own neck - but two turns applied before the next move happens
// can add up to a 180, so only one turn is accepted per move tick (see snakeTurnedThisTick).
int snakeDirection = 1;
bool snakeTurnedThisTick = false; // caps turning to once per move - see updateSnake()

int snakeRow[SNAKE_MAX_LENGTH]; // index 0 = head
int snakeCol[SNAKE_MAX_LENGTH];
int snakeLength = 0;

int snakeFoodRow = 0;
int snakeFoodCol = 0;

int snakeScore = 0;
int snakeHighScore = 0; // loaded from flash in setup()
unsigned long snakeLastMoveTime = 0;

// ---- SimpleMorse state ----
// Standard International Morse code for A-Z, as dot/dash strings.
const char* const MORSE_TABLE[26] = {
  ".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---", "-.-", ".-..", "--",
  "-.", "---", ".--.", "--.-", ".-.", "...", "-", "..-", "...-", ".--", "-..-", "-.--", "--.."
};

// Each word's total dot/dash count (summed across its letters) was checked to land between
// 10 and 15 - the "10 to 15 presses" requirement - see the constant SIMON_MAX_LENGTH below.
const char* const SIMON_WORDS[] = {
  "TIGER", "SNAKE", "PARTY", "BOMB", "CHESS", "DICE", "PLAY", "QUIZ", "LOSE", "BUZZ",
  "ROLL", "TRIVIA", "PASS", "MORSE", "SIMON", "TETRIS", "DROP"
};
const int SIMON_WORD_COUNT = 17;
const int SIMON_MAX_LENGTH = 15; // longest any word's flattened dot/dash sequence can be

const int SIMON_READY     = 0; // showing the chosen word, waiting to start
const int SIMON_PLAYBACK  = 1; // device is playing back the sequence so far (blocking animation)
const int SIMON_INPUT     = 2; // waiting for the player to repeat it via short/long presses
const int SIMON_GAME_OVER = 3; // wrong input - wounded walrus
const int SIMON_COMPLETE  = 4; // player made it through the whole sequence
int simonState = SIMON_READY;

int simonSequence[SIMON_MAX_LENGTH]; // 0 = dot (short press), 1 = dash (long press)
int simonSequenceLength = 0;         // total symbols in the current word
int simonRound = 0;                  // how many symbols the player must currently repeat (1..simonSequenceLength)
int simonInputIndex = 0;             // how many correct presses received so far this round
char simonWord[16] = "";             // the chosen word, for display
int simonLetterEndIndex[15];         // cumulative symbol count once each letter is complete (parallel to simonWord)
int simonLetterCount = 0;            // how many letters simonLetterEndIndex actually covers
const unsigned long SIMON_DASH_THRESHOLD_MS = 400; // held at least this long = a dash, otherwise a dot

const unsigned long TOOTHBRUSH_BRUSH_INTERVAL_MS = 180; // roughly a "scrub" rhythm

// ---- Trivia state ----
const int TRIVIA_SELECT_CATEGORY = 0;
const int TRIVIA_SELECT_LEVEL    = 1;
const int TRIVIA_PLAYING         = 2;
int triviaScreenState = TRIVIA_SELECT_CATEGORY;

const int TRIVIA_REAL_CATEGORY_COUNT = 8; // Sports, Geography, Science, History, General, Mythology, Art, Animals
const int TRIVIA_CATEGORY_RANDOM = 8;     // sentinel: pulls from any of the 8 real categories
const int TRIVIA_CATEGORY_COUNT = 9;      // the 8 real categories, plus Random
int triviaCategory = 0;
int triviaActiveCategory = 0; // the real category (0-3) the CURRENTLY LOADED question actually belongs to -
                                // always equals triviaCategory, except in Random mode where it varies per question
const int TRIVIA_LEVEL_COUNT = 3; // Junior, Curious, Master
int triviaLevel = 0;
const int TRIVIA_MAX_QUESTIONS = 2500; // upper bound the shuffle array is sized for (covers Random pulling from all 8 categories at once)

int triviaQuestionOrder[TRIVIA_MAX_QUESTIONS]; // shuffled once per round, fixed after that -
                                                 // so turning back always returns to the same question.
                                                 // In Random mode, each entry is packed as (category * 10000 + index).
int triviaQuestionCount = 0;  // how many questions are actually in the current category/level's file
int triviaOrderPosition = 0; // where we are within that shuffled order
char triviaQuestionBuf[110] = ""; // sized for the 5-line display's ~95-char visual capacity, plus margin -
                                    // translations often run longer than the English original
const int TRIVIA_MAX_OPTIONS = 4;
char triviaOptionBuf[TRIVIA_MAX_OPTIONS][40] = {"", "", "", ""}; // shuffled display order - longer than a
                                                                    // row shows (19 chars); overflow scrolls
int triviaOptionCount = 0;      // 2-4 depending on how many non-empty options this question has
int triviaCorrectOption = -1;   // index into triviaOptionBuf that's the correct answer, after shuffling
bool triviaAnswerVisible = false;
bool triviaAnswering = false; // true = within the 5s answer window, LED blinking
bool triviaShowingAnswers = false; // false = Question view, true = Answers view
unsigned long triviaAnswerRevealTime = 0;
int triviaLastBipSecond = 0; // tracks which second-mark within the answer window we last bipped for
const unsigned long TRIVIA_ANSWER_WINDOW_MS = 5000;
bool triviaBlinkState = false;
unsigned long lastTriviaBlinkToggle = 0;

// Text too long for the screen scrolls automatically instead of being cut off (translations often
// run longer than the English): a question needing more than 4 wrapped lines scrolls up a line at a
// time, an answer longer than its 19-character row scrolls sideways a character at a time. Both
// hold still at the start and the end, then loop. Timing restarts whenever the view changes.
const int TRIVIA_QUESTION_VISIBLE_LINES = 4;
const int TRIVIA_QUESTION_CHARS_PER_LINE = 21;
const int TRIVIA_ANSWER_VISIBLE_CHARS = 19;
const unsigned long TRIVIA_SCROLL_HOLD_MS = 1500;      // pause at each end before/after scrolling
const unsigned long TRIVIA_SCROLL_CHAR_MS = 250;       // answers: one character per step
const unsigned long TRIVIA_SCROLL_LINE_MS = 1800;      // questions: one line per step
unsigned long triviaViewShownAt = 0; // reset whenever the Question/Answers view or question changes
long triviaLastScrollFrame = -1;

// volume: 0 = mute, 1 = low, 2 = high. Low is the default.
int volumeLevel = 1;
const int VOLUME_LOW_DUTY  = 40;  // out of 255, quieter
const int VOLUME_HIGH_DUTY = 128; // out of 255, ~full loudness (matches old tone() output)
const int BUZZER_PWM_BITS  = 8;   // 0-255 duty range

bool midpointPlayed = false; // reset each time a run starts, so the midpoint chime only fires once

// short ascending end-of-timer jingle
const int END_MELODY_NOTES[]     = {880, 988, 1175, 1568}; // A5, B5, D6, G6
const int END_MELODY_DURATIONS[] = {120, 120, 120, 220};
const int END_MELODY_LENGTH = 4;

// short "go" jingle when the countdown starts
const int START_MELODY_NOTES[]     = {523, 659, 784}; // C5, E5, G5
const int START_MELODY_DURATIONS[] = {90, 90, 140};
const int START_MELODY_LENGTH = 3;

// short two-tone chime at the halfway point
const int MID_MELODY_NOTES[]     = {784, 659}; // G5, E5
const int MID_MELODY_DURATIONS[] = {100, 140};
const int MID_MELODY_LENGTH = 2;

// short jingle when a reflex round is armed
const int REFLEX_ARM_NOTES[]     = {659, 988}; // E5, B5
const int REFLEX_ARM_DURATIONS[] = {80, 110};
const int REFLEX_ARM_LENGTH = 2;

// single sharp cue played right as the light turns on
const int REFLEX_LIGHT_FREQ = 1800;
const int REFLEX_LIGHT_DURATION_MS = 60;

// short descending "no new record" sound
const int REFLEX_LOSE_NOTES[]     = {392, 311}; // G4, D#4
const int REFLEX_LOSE_DURATIONS[] = {140, 220};
const int REFLEX_LOSE_LENGTH = 2;

// short ascending "new record" fanfare
const int REFLEX_WIN_NOTES[]     = {659, 784, 988, 1319}; // E5, G5, B5, E6
const int REFLEX_WIN_DURATIONS[] = {90, 90, 90, 200};
const int REFLEX_WIN_LENGTH = 4;

// ---- Power management: screen sleeps after inactivity, unless something time-critical is happening ----
const unsigned long SCREEN_TIMEOUT_MS = 5UL * 60UL * 1000UL;      // 5 minutes: OLED off
#ifndef TT_DEEP_SLEEP_TIMEOUT_MS // build-time override, only for bench-testing sleep without waiting 30 min
#define TT_DEEP_SLEEP_TIMEOUT_MS (30UL * 60UL * 1000UL)
#endif
const unsigned long DEEP_SLEEP_TIMEOUT_MS = TT_DEEP_SLEEP_TIMEOUT_MS; // 30 minutes: full chip deep sleep
unsigned long lastActivityTime = 0;
bool screenAsleep = false;

// ---- Explicit function prototypes ----
// Arduino normally auto-generates these, but its scanner can get confused by the CP437 hex-escape
// sequences in the translation strings above (e.g. "Fran\x87" "ais"), silently skipping some
// functions. Declaring them explicitly here avoids relying on that auto-generation at all.
int menuPositionForGame(int gameId);
const char* menuLabelForGame(int gameId);
const char* T(int stringId);
bool registerActivity();
bool isTimeCriticalActive();
void checkScreenSleep();
void checkDeepSleep();
void enterDeepSleep();
void readEncoder();
void initGameState(int game);
void confirmMenuSelection();
void playGameSplashScreen(int game);
void confirmSettingsSelection();
void confirmLanguageSelection();
void confirmTimerStepSelection();
void confirmDiceType();
void confirmChessStartTime();
long rampedTimeStep(long fromSeconds, int direction);
void confirmChessIncrement();
void toggleChessPause();
void toggleTimerPause();
void doChessPass();
int hpRandomEnabledCategory();
void hpNewCombo();
void hpRerollWithAnimation();
void handleHotPotatoPress();
void updateHotPotato();
void doTurnOrderSpin();
void handleStopwatchPress();
void updateStopwatch();
void recordStopwatchLap();
void handleToothbrushPress();
void updateToothbrush();
bool tetrisCollision(int pieceType, int rotation, int row, int col);
void tetrisSpawnPiece();
void playTetrisGameOverAnimation();
void tetrisLockPiece();
void tetrisFlashLines(int* rows, int count);
void tetrisMove(int direction);
void tetrisRotate();
void tetrisHardDrop();
int tetrisGhostRow();
void handleTetrisButtonPress();
void updateTetris();
void stardropStart();
int stardropPointsForType(int type);
void stardropSpawnObject();
void stardropMoveCatcher(int direction);
void handleStarDropButtonPress();
void stardropGameOver();
void playStarDropGameOverAnimation();
void updateStarDrop();
void snakeSpawnFood();
void snakeStart();
void handleSnakeButtonPress();
void snakeGameOver();
void updateSnake();
void simonNewSequence();
void simonPlayback();
void handleSimonButtonPress();
void drawWalrusFace(int cx, int cy, int headR, bool wounded, uint16_t color);
void playWoundedWalrusAnimation();
void drawSimpleMorseIcon(int cx, int cy, int r, uint16_t color);
int toothbrushCurrentZone();
void playToothbrushZoneChangeBeep();
void playToothbrushDoneAnimation();
void drawToothbrushSelectScreen();
void drawToothbrushRunningScreen();
void drawTetrisReadyScreen();
int drawRotatedInvertedTitle(const char* title);
int drawRotatedWrappedText(const char* text, int yStart, int lineHeight);
void drawTetrisPlayScreen();
void drawTetrisGameOverScreen();
void drawStarDropReadyScreen();
void drawStarDropPlayScreen();
void drawStarDropGameOverScreen();
void drawToothbrushIcon(int cx, int cy, int r, uint16_t color);
void drawTetrisIcon(int cx, int cy, int r, uint16_t color);
void drawStarDropIcon(int cx, int cy, int r, uint16_t color);
void drawSnakeIcon(int cx, int cy, int r, uint16_t color);
void drawSnakeScreen();
void drawSimonScreen();
void drawWordWithCode(const char* word, int yTop);
void drawWordProgress(const char* word, int yTop);
void drawStarDropObjectIcon(int cx, int cy, int type, uint16_t color);
void drawStarDropTelescopeIcon(int laneCenterX, int baseY, uint16_t color);
bool loadTriviaQuestion(int category, int index);
void triviaFilePath(int category, char* out, size_t outSize);
int countTriviaQuestions(int category);
void shuffleTriviaOrder();
void loadCurrentTriviaOrderEntry();
void moveTriviaStep(int direction);
void confirmTriviaCategory();
void confirmTriviaLevel();
void handleTriviaButtonPress();
void updateTrivia();
void drawTriviaCategoryScreen();
void drawTriviaLevelScreen();
void drawTriviaPlayScreen();
void drawTriviaIcon(int cx, int cy, int r, uint16_t color);
void invertScreenRegion(int x0, int y0, int x1, int y1);
void drawArrowGlyph(int cx, int cy, uint16_t color, bool pointRight);
int drawWrappedText(const char* text, int yStart, int maxLines, int lineHeight, int skipLines = 0);
int wrappedLineCount(const char* text, int charsPerLine);
int triviaScrollOffset(int overflow, unsigned long stepMs);
bool triviaContentOverflows();
int drawWrappedTextInColumn(const char* text, int xStart, int colWidth, int yStart, int maxLines, int lineHeight);
void drawHotPotatoScreen();
void drawTurnOrderScreen();
void drawStopwatchScreen();
void drawHotPotatoIcon(int cx, int cy, int r, uint16_t color);
void drawRotateIcon(int cx, int cy, int r, uint16_t color);
void drawTurnOrderIcon(int cx, int cy, int r, uint16_t color);
void drawStopwatchIcon(int cx, int cy, int r, uint16_t color);
bool tryConfirmSelectionScreens();
bool inGameScreen();
void leaveToMenu();
void openLeaveConfirm();
void confirmLeaveConfirmSelection();
void toggleMenuFromLongPress();
void closeInstructions();
void dispatchButtonShortPress();
void readButton();
void readEncoderButton();
void playNote(int freq, int durationMs);
void startTone(int freq);
void stopTone();
void playEndMelody();
void playStartMelody();
void playMidMelody();
void playArmSound();
void playLightCue();
void playLoseSound();
void playWinSound();
void updateCountdown();
void updateReflexGame();
void updateChessGame();
void playBombAnimation();
void drawBurst(int cx, int cy, int r);
void playFireworkAnimation();
bool splashSkipRequested();
void playSplashScreen();
void handleReflexButtonPress();
int diceFaceSize(int count);
int diceYStart(int count, int faceSize);
void doDiceRoll();
void updateLedBlink();
void render();
void drawMenuScreen();
void drawCenteredAutoFit(const char* text, int y, int maxSize);
void drawTimerScreen();
void drawReflexScreen();
void drawDiceSelectScreen();
void drawDiceScreen();
void drawDieFace(int fx, int fy, int s, int sides, int value, uint16_t color);
void drawChessSetupTimeScreen();
void drawChessSetupIncrementScreen();
void drawChessPlayScreen();
void drawLanguageScreen();
void drawLeaveConfirmScreen();
void drawInstructionsScreen();
void drawSettingsScreen();
void drawTimerStepScreen();
void drawTimerIcon(int cx, int cy, int r, uint16_t color);
void drawReflexIcon(int cx, int cy, int r, uint16_t color);
void drawDiceIcon(int cx, int cy, int r, uint16_t color);
void drawChessIcon(int cx, int cy, int r, uint16_t color);
void drawLanguageIcon(int cx, int cy, int r, uint16_t color);
void drawSettingsIcon(int cx, int cy, int r, uint16_t color);
void drawConfigModeIcon(int cx, int cy, int r, uint16_t color);
void enterConfigMode(); // web_config.ino
void loadMenuOrderFromPrefs(); // web_config.ino
void saveMenuOrderToPrefs(); // web_config.ino
void loadGameEnabledFromPrefs(); // web_config.ino
void loadHpCategoryEnabledFromPrefs(); // web_config.ino
void saveGameEnabledToPrefs(); // web_config.ino
void rebuildVisibleMenu(); // web_config.ino
void drawTimerStepIcon(int cx, int cy, int r, uint16_t color);
void drawGameIcon(int gameIndex, int cx, int cy, int r, uint16_t color);
void drawInstructionsIcon(int cx, int cy, int r, uint16_t color);
void drawSoundArc(int cx, int cy, int r, uint16_t color);
void drawThickX(int cx, int cy, int size, uint16_t color);
int drawSpeakerShape(int boxLeftX, int cy, int boxSize, int boxRadius, int coneLen, int wideHalfH, int cornerR, uint16_t color);
void drawVolumeIconAt(int x, int y);
void drawVolumeOverlay();

void setup() {
  // After a deep-sleep wake the button pin is still owned by the LP-IO block (see enterDeepSleep) -
  // hand it back to the normal GPIO matrix before configuring it, or digitalRead() can't see it.
  rtc_gpio_hold_dis((gpio_num_t)digitalPinToGPIONumber(BUTTON_PIN));
  rtc_gpio_deinit((gpio_num_t)digitalPinToGPIONumber(BUTTON_PIN));
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUTTON_LED, OUTPUT);
  ledcAttach(BUZZER_PIN, 2000, BUZZER_PWM_BITS); // attach buzzer to LEDC so duty (volume) is controllable
  pinMode(ENCODER_CLK, INPUT_PULLUP);
  pinMode(ENCODER_DT, INPUT_PULLUP);
  pinMode(ENCODER_SW, INPUT_PULLUP);

  digitalWrite(BUTTON_LED, LOW);
  lastClkState = digitalRead(ENCODER_CLK);

  Serial.begin(115200);
  randomSeed(micros()); // seed for the reflex game's random light delay

  prefs.begin("gametimer", false);
  currentLanguage = constrain((int)prefs.getUChar("lang", LANG_EN), 0, LANG_COUNT - 1);
  currentTimerStepIndex = constrain((int)prefs.getUChar("step", TIMER_STEP_DEFAULT_INDEX), 0, TIMER_STEP_OPTION_COUNT - 1);
  loadMenuOrderFromPrefs(); // web_config.ino - fills MENU_ORDER from flash, or DEFAULT_MENU_ORDER if unset/invalid
  loadGameEnabledFromPrefs(); // web_config.ino - fills gameEnabled[] from flash, or all-enabled if unset
  rebuildVisibleMenu(); // web_config.ino - filters MENU_ORDER by gameEnabled into visibleMenuOrder
  stardropHighScore = prefs.getUShort("sdHigh", 0);
  tetrisHighScore = prefs.getUShort("ttHigh", 0);
  snakeHighScore = prefs.getUShort("skHigh", 0);
  loadHpCategoryEnabledFromPrefs(); // web_config.ino

  long defaultTimerSeconds = constrain((long)prefs.getUShort("deftime", (uint16_t)DEFAULT_TIME), MIN_TIME, MAX_TIME);
  presetTime = defaultTimerSeconds;
  remainingTime = defaultTimerSeconds;
  runStartTime = defaultTimerSeconds;

  bool fsOk = LittleFS.begin(true); // true = format if mount fails (e.g. first run)
  if (!fsOk) {
    Serial.println("LittleFS mount failed - trivia questions won't load. Did you upload the data folder?");
  }

  Wire.begin(D4, D5); // SDA, SCL
  bool oledOk = display.begin(OLED_I2C_ADDR, true);
  if (!oledOk) {
    Serial.println("OLED not found - check wiring/address");
  }

  // One-line boot summary on USB serial - lets a connected computer confirm a healthy boot
  // (and that the trivia data / config page are present) without looking at the screen.
  Serial.printf("[boot] Tiger Timer - reset=%d wake=%d, OLED %s, LittleFS %s (%u/%u KB used), config.html %s, sports_junior.csv %s, lang=%d\n",
                (int)esp_reset_reason(), (int)esp_sleep_get_wakeup_cause(),
                oledOk ? "ok" : "MISSING", fsOk ? "ok" : "FAILED",
                fsOk ? (unsigned)(LittleFS.usedBytes() / 1024) : 0u, fsOk ? (unsigned)(LittleFS.totalBytes() / 1024) : 0u,
                (fsOk && LittleFS.exists("/config.html")) ? "present" : "missing",
                (fsOk && LittleFS.exists("/sports_junior.csv")) ? "present" : "missing",
                currentLanguage);
  display.setRotation(0);
  display.cp437(true); // use the corrected CP437 character mapping, needed for accented characters

  playSplashScreen(); // returns early if the arcade button is pressed during it, to skip straight in

  // ---- Startup behavior: go to the menu, or boot straight into a chosen game ----
  uint8_t startMode = prefs.getUChar("startMode", 0); // 0 = menu (default), 1 = specific game
  uint8_t startGame = prefs.getUChar("startGame", (uint8_t)GAME_TIMER);
  bool validStartGame = ((int)startGame >= 0 && (int)startGame < GAME_COUNT && gameEnabled[startGame]);
  if (startMode == 1 && validStartGame) {
    currentGame = (int)startGame;
    inMenu = false;
    initGameState(currentGame);
    if (currentGame == GAME_HOTPOTATO) {
      hpRerollWithAnimation(); // first combo drawn with the same shuffle animation as a manual reroll
    }
  } else {
    inMenu = true;
    menuSelection = 0; // highlight the first entry in the list, not wherever Timer happens to be
  }

  lastActivityTime = millis();
  render();
}

#ifdef TT_SCREEN_DUMP
// ---------------- Bench-test only (never in a normal build): drive the toothbrush screen over
// USB serial and dump the OLED buffer as text, so a computer can check the drawing without
// eyes on the panel. t = open Toothbrush, p = arcade press, f = skip 20s ahead, d = dump. ----------------
void testSerialCommands() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 't') { inMenu = false; currentGame = GAME_TOOTHBRUSH; initGameState(GAME_TOOTHBRUSH); render(); }
    else if (c == 'p') { handleToothbrushPress(); }
    else if (c == 'f') { toothbrushRemainingSeconds = max(1L, toothbrushRemainingSeconds - 20); toothbrushLastTick = millis() - 1000; }
    else if (c == 'd') {
      Serial.println("[dump begin]");
      for (int y = 0; y < SCREEN_HEIGHT; y++) {
        char line[SCREEN_WIDTH + 1];
        for (int x = 0; x < SCREEN_WIDTH; x++) line[x] = display.getPixel(x, y) ? '#' : '.';
        line[SCREEN_WIDTH] = 0;
        Serial.println(line);
      }
      Serial.println("[dump end]");
    }
  }
}
#endif

void loop() {
#ifdef TT_SCREEN_DUMP
  testSerialCommands();
#endif
  readEncoder();
  readButton();
  readEncoderButton();

  if (!inMenu && currentGame == GAME_TIMER) {
    updateCountdown();
    updateLedBlink();
  }
  if (!inMenu && currentGame == GAME_REFLEX) {
    updateReflexGame();
  }
  if (!inMenu && currentGame == GAME_CHESS) {
    updateChessGame();
  }
  if (!inMenu && currentGame == GAME_HOTPOTATO) {
    updateHotPotato();
  }
  if (!inMenu && currentGame == GAME_STOPWATCH) {
    updateStopwatch();
  }
  if (!inMenu && currentGame == GAME_TRIVIA) {
    updateTrivia();
  }
  if (!inMenu && currentGame == GAME_TOOTHBRUSH) {
    updateToothbrush();
  }
  if (!inMenu && currentGame == GAME_TETRIS) {
    updateTetris();
  }
  if (!inMenu && currentGame == GAME_STARDROP) {
    updateStarDrop();
  }
  if (!inMenu && currentGame == GAME_SNAKE) {
    updateSnake();
  }

  checkScreenSleep();
  checkDeepSleep();
}

// ---------------- Power management: call whenever real user input happens. Returns true if this
// input just woke a sleeping screen - input handlers then swallow that gesture, so a press or turn
// made on a dark screen only wakes it instead of also doing something the user couldn't see. ----------------
bool registerActivity() {
  lastActivityTime = millis();
  if (screenAsleep) {
    display.oled_command(SH110X_DISPLAYON);
    screenAsleep = false;
    return true;
  }
  return false;
}

// ---------------- Power management: true while a countdown, an armed reflex round, or a running chess clock is in progress ----------------
bool isTimeCriticalActive() {
  return (currentGame == GAME_TIMER && running && !timerPaused) ||
         (currentGame == GAME_REFLEX && reflexState != REFLEX_READY) ||
         (currentGame == GAME_CHESS && chessScreenState == CHESS_PLAYING && chessStarted && !chessPaused) ||
         (currentGame == GAME_HOTPOTATO && hpState == HP_ARMED) ||
         (currentGame == GAME_STOPWATCH && stopwatchState == STOPWATCH_RUNNING) ||
         (currentGame == GAME_TRIVIA && triviaAnswering) ||
         (currentGame == GAME_TOOTHBRUSH && toothbrushScreenState == TOOTHBRUSH_RUNNING && !toothbrushPaused) ||
         (currentGame == GAME_TETRIS && tetrisState == TETRIS_PLAYING) ||
         (currentGame == GAME_STARDROP && stardropState == STARDROP_PLAYING) ||
         (currentGame == GAME_SNAKE && snakeState == SNAKE_PLAYING) ||
         (currentGame == GAME_SIMPLEMORSE && simonState == SIMON_INPUT);
}

// ---------------- Power management: sleeps the OLED after 5 minutes idle, unless something time-critical is running ----------------
void checkScreenSleep() {
  if (!screenAsleep && !isTimeCriticalActive() && (millis() - lastActivityTime >= SCREEN_TIMEOUT_MS)) {
    display.oled_command(SH110X_DISPLAYOFF);
    screenAsleep = true;
  }
}

// ---------------- Power management: after 30 minutes idle, puts the whole chip into deep sleep.
// Wakes instantly on an arcade button press (D1) via ext1 - confirmed as a working wake pin on this
// board. Deep sleep does not affect USB charging, since the charge management chip is separate
// hardware from the ESP32C6 and runs independently of its sleep state.
// NOTE: this is effectively a reboot on wake - setup() runs again, all normal RAM state resets to
// defaults (session high scores etc.), same as any power-up. Settings saved to flash (language,
// timer step) are unaffected either way. ----------------
void checkDeepSleep() {
  if (!isTimeCriticalActive() && (millis() - lastActivityTime >= DEEP_SLEEP_TIMEOUT_MS)) {
    enterDeepSleep();
  }
}

void enterDeepSleep() {
  digitalWrite(BUTTON_LED, LOW);
  display.oled_command(SH110X_DISPLAYOFF);
  prefs.end();

  // ext1 hands the pin over to the low-power (RTC/LP) IO block for sleep, which has its OWN
  // pull-up/pull-down - the pinMode(INPUT_PULLUP) set in setup() is the digital one and doesn't
  // carry over. Without enabling the LP pull-up here the pin floats, reads LOW, and the chip
  // wakes (= reboots, chime and all) the instant it goes to sleep. The RTC_PERIPH domain stays
  // on so that pull-up stays powered through sleep.
  gpio_num_t wakePin = (gpio_num_t)digitalPinToGPIONumber(BUTTON_PIN);
  rtc_gpio_init(wakePin);
  rtc_gpio_set_direction(wakePin, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pulldown_dis(wakePin);
  rtc_gpio_pullup_en(wakePin);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  esp_sleep_enable_ext1_wakeup(1ULL << wakePin, ESP_EXT1_WAKEUP_ANY_LOW);
#ifdef TT_SLEEP_TEST_TIMER_WAKE_S // bench-test only: also wake on a timer, so a test can confirm the chip stays asleep then comes back
  esp_sleep_enable_timer_wakeup((uint64_t)TT_SLEEP_TEST_TIMER_WAKE_S * 1000000ULL);
#endif
  esp_deep_sleep_start();
}

// ---------------- Encoder: hold the SW button and turn for volume; otherwise navigates whichever screen is active ----------------
void readEncoder() {
  int clkState = digitalRead(ENCODER_CLK);

  // Only act on the falling edge of CLK (HIGH -> LOW). A full detent toggles
  // CLK twice (once each direction), so reacting to every change double-counts.
  if (clkState == LOW && lastClkState == HIGH) {
    if (registerActivity()) { // this detent only woke the screen - don't also act on it
      lastClkState = clkState;
      return;
    }
    int dtState = digitalRead(ENCODER_DT);
    int direction = (dtState == HIGH) ? 1 : -1; // +1 clockwise, -1 counter-clockwise

    if (swState == LOW && !swLongFired) {
      // encoder button is currently held (and hasn't already opened the menu via long-press) -
      // this turn adjusts volume instead of anything else, and claims the hold so releasing it
      // afterward doesn't also fire a click action
      volumeLevel = constrain(volumeLevel + direction, 0, 2);
      ledcWrite(BUZZER_PIN, 0); // silence anything currently sounding
      swClaimedByVolume = true;
      drawVolumeOverlay(); // full-screen, live - readEncoderButton() restores the normal screen on release
      lastClkState = clkState;
      return;
    } else if (inLeaveConfirm) {
      leaveConfirmSelection = 1 - leaveConfirmSelection; // only Yes/No - either direction toggles
    } else if (inInstructions) {
      // static screen, nothing to select - rotation has no effect
    } else if (inLanguageSelect) {
      currentLanguage = constrain(currentLanguage + direction, 0, LANG_COUNT - 1);
    } else if (inTimerStepSelect) {
      currentTimerStepIndex = constrain(currentTimerStepIndex + direction, 0, TIMER_STEP_OPTION_COUNT - 1);
    } else if (inSettingsMenu) {
      settingsSelection = constrain(settingsSelection + direction, 0, SETTINGS_COUNT - 1);
    } else if (inMenu) {
      menuSelection = constrain(menuSelection + direction, 0, visibleMenuCount - 1);
    } else if (currentGame == GAME_TIMER) {
      if (!running) {
        long step = (TIMER_STEP_OPTIONS[currentTimerStepIndex] == TIMER_STEP_AUTO) ? rampedTimeStep(presetTime, direction) : TIMER_STEP_OPTIONS[currentTimerStepIndex];
        presetTime = constrain(presetTime + direction * step, MIN_TIME, MAX_TIME);
        remainingTime = presetTime; // preview the new setting on screen
      }
      // while running (or paused): rotation has no effect, to avoid accidentally
      // changing the time mid-game
    } else if (currentGame == GAME_DICE) {
      if (diceScreenState == DICE_SELECT_TYPE) {
        currentDieIndex = constrain(currentDieIndex + direction, 0, DICE_TYPE_COUNT - 1);
      } else {
        diceCount = constrain(diceCount + direction, 1, MAX_DICE_COUNT);
      }
    } else if (currentGame == GAME_CHESS) {
      if (chessScreenState == CHESS_SETUP_TIME) {
        long step = rampedTimeStep(chessStartSeconds, direction);
        chessStartSeconds = constrain(chessStartSeconds + direction * step, CHESS_START_MIN_SECONDS, CHESS_START_MAX_SECONDS);
      } else if (chessScreenState == CHESS_SETUP_INCREMENT) {
        chessIncrementSeconds = constrain(chessIncrementSeconds + direction * 10, 0, 60);
      }
      // CHESS_PLAYING / CHESS_GAME_OVER: rotation has no effect
    } else if (currentGame == GAME_TURNORDER) {
      turnOrderPlayerCount = constrain(turnOrderPlayerCount + direction, 2, 10);
      turnOrderResult = -1; // changing player count invalidates any previous spin
    } else if (currentGame == GAME_TRIVIA) {
      if (triviaScreenState == TRIVIA_SELECT_CATEGORY) {
        triviaCategory = constrain(triviaCategory + direction, 0, TRIVIA_CATEGORY_COUNT - 1);
      } else if (triviaScreenState == TRIVIA_SELECT_LEVEL) {
        triviaLevel = constrain(triviaLevel + direction, 0, TRIVIA_LEVEL_COUNT - 1);
      } else if (triviaScreenState == TRIVIA_PLAYING) {
        moveTriviaStep(direction); // Question <-> Answers <-> next/previous Question, one step per detent
        lastClkState = clkState;
        return; // moveTriviaStep() already renders and plays its own confirmation sound
      }
    } else if (currentGame == GAME_TOOTHBRUSH && toothbrushScreenState == TOOTHBRUSH_SELECT_TIME) {
      toothbrushMinutes = constrain(toothbrushMinutes + direction, 1, 5);
    } else if (currentGame == GAME_TETRIS) {
      tetrisMove(direction); // already renders itself only if the move was actually valid
      lastClkState = clkState;
      return;
    } else if (currentGame == GAME_STARDROP) {
      stardropMoveCatcher(direction); // already renders itself
      lastClkState = clkState;
      return;
    } else if (currentGame == GAME_SNAKE && snakeState == SNAKE_PLAYING) {
      if (!snakeTurnedThisTick) {
        snakeDirection = ((snakeDirection + direction) % 4 + 4) % 4; // relative turn: +1 = right, -1 = left
        snakeTurnedThisTick = true;
      }
    } else if (currentGame == GAME_HOTPOTATO && hpState == HP_READY) {
      hpRerollWithAnimation(); // blocking - already renders itself throughout
      lastClkState = clkState;
      return;
    } else if (currentGame == GAME_HOTPOTATO && hpState == HP_EXPLODED) {
      hpState = HP_READY;
      hpRerollWithAnimation(); // skips straight past the post-explosion pause in updateHotPotato()
      digitalWrite(BUTTON_LED, LOW);
      lastClkState = clkState;
      return;
    }
    // GAME_REFLEX, GAME_STOPWATCH: encoder rotation has no effect

    render();
  }

  lastClkState = clkState;
}

// ---------------- Menu: confirms the highlighted entry - a game, or opens the settings submenu ----------------
// ---------------- Shared: resets a game to its just-opened state - the same reset whether you
// reached it by picking it from the menu, or by booting straight into it (the "Start with a
// specific game" startup option in Config Mode uses this too). Does NOT touch currentGame/inMenu
// or call render() - callers handle those. ----------------
void initGameState(int game) {
  if (game == GAME_REFLEX) {
    reflexState = REFLEX_READY;
    falseStartUntil = 0;
    digitalWrite(BUTTON_LED, LOW);
  } else if (game == GAME_DICE) {
    diceScreenState = DICE_SELECT_TYPE;
    diceResultCount = 0;
  } else if (game == GAME_CHESS) {
    chessScreenState = CHESS_SETUP_TIME;
    chessPaused = false;
    digitalWrite(BUTTON_LED, LOW);
  } else if (game == GAME_HOTPOTATO) {
    hpState = HP_READY;
    hpNewCombo();
    volumeLevel = 2; // make sure the explosion is actually heard - this is the one game where that really matters
    digitalWrite(BUTTON_LED, LOW);
  } else if (game == GAME_STOPWATCH) {
    stopwatchState = STOPWATCH_READY;
    stopwatchElapsedMs = 0;
    stopwatchLapSeconds = -1;
  } else if (game == GAME_TURNORDER) {
    turnOrderResult = -1;
  } else if (game == GAME_TRIVIA) {
    triviaScreenState = TRIVIA_SELECT_CATEGORY;
    triviaAnswering = false;
    triviaAnswerVisible = false;
    digitalWrite(BUTTON_LED, LOW);
  } else if (game == GAME_TOOTHBRUSH) {
    toothbrushScreenState = TOOTHBRUSH_SELECT_TIME;
    toothbrushPaused = false;
    digitalWrite(BUTTON_LED, LOW);
  } else if (game == GAME_TETRIS) {
    tetrisState = TETRIS_READY;
  } else if (game == GAME_STARDROP) {
    stardropState = STARDROP_READY;
  } else if (game == GAME_SNAKE) {
    snakeState = SNAKE_READY;
  } else if (game == GAME_SIMPLEMORSE) {
    simonState = SIMON_READY;
    simonRound = 0;
    simonNewSequence();
  }
}

void confirmMenuSelection() {
  int selected = visibleMenuOrder[menuSelection];

  if (selected == MENU_SETTINGS) {
    inMenu = false;
    inSettingsMenu = true;
    render();
    return;
  }

  if (selected == MENU_INSTRUCTIONS) {
    inMenu = false;
    inInstructions = true;
    render();
    return;
  }

  currentGame = selected;
  inMenu = false;
  initGameState(currentGame);
  playGameSplashScreen(currentGame);
  if (currentGame == GAME_HOTPOTATO) {
    hpRerollWithAnimation(); // first combo drawn with the same shuffle animation as a manual reroll
  }
  render();
}

// ---------------- Brief (1s) splash shown right after picking a game from the menu: its icon big
// on the left, the game's name next to it. Blocking, like the sketch's other short animations. ----------------
void playGameSplashScreen(int game) {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);

  // logo up top, similar scale to the boot splash's tiger bitmap
  int iconCx = SCREEN_WIDTH / 2;
  int iconCy = 22;
  int iconR = 20;
  drawGameIcon(game, iconCx, iconCy, iconR, SH110X_WHITE);

  // name at the bottom, in the same Press Start 2P font as the boot splash's TIGER/TIMER wordmark.
  // At its native 8px size this comfortably fits every current game name on one line (that's what
  // was getting cut off before, e.g. Spanish "Temporizador") - the wrap below is just a safety net.
  const char* name = menuLabelForGame(game);
  display.setFont(&PressStart2P8pt);
  display.setTextSize(1);

  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(name, 0, 0, &x1, &y1, &w, &h);

  if ((int)w <= SCREEN_WIDTH - 4) {
    display.setCursor((SCREEN_WIDTH - (int)w) / 2 - x1, 48 - y1);
    display.print(name);
  } else {
    // split at whichever space sits closest to the middle - not currently needed by any game
    // name, but keeps a very long future name readable instead of overflowing
    String s(name);
    int mid = s.length() / 2;
    int splitAt = -1;
    for (int i = 0; i < (int)s.length(); i++) {
      if (s[i] == ' ' && (splitAt < 0 || abs(i - mid) < abs(splitAt - mid))) splitAt = i;
    }
    String line1 = (splitAt >= 0) ? s.substring(0, splitAt) : s;
    String line2 = (splitAt >= 0) ? s.substring(splitAt + 1) : "";

    uint16_t w1, h1, w2, h2;
    display.getTextBounds(line1.c_str(), 0, 0, &x1, &y1, &w1, &h1);
    display.setCursor((SCREEN_WIDTH - (int)w1) / 2 - x1, 44 - y1);
    display.print(line1);
    if (line2.length() > 0) {
      display.getTextBounds(line2.c_str(), 0, 0, &x1, &y1, &w2, &h2);
      display.setCursor((SCREEN_WIDTH - (int)w2) / 2 - x1, 56 - y1);
      display.print(line2);
    }
  }

  display.setFont(); // back to the normal built-in font for every other screen
  display.display();
  delay(1000);
}

// ---------------- Instructions screen: closes back to the menu (arcade button, or encoder click
// via the generic "close menu hierarchy" handling in readEncoderButton) ----------------
void closeInstructions() {
  inInstructions = false;
  inMenu = true;
  menuSelection = menuPositionForGame(MENU_INSTRUCTIONS);
  render();
}

// ---------------- Settings: confirms the highlighted entry, opening Language or Timer Step selection ----------------
void confirmSettingsSelection() {
  if (settingsSelection == SETTINGS_CONFIG_MODE) {
    enterConfigMode(); // blocks here (own loop) until the user presses a button to exit
    inSettingsMenu = false;
    inMenu = true;
    menuSelection = menuPositionForGame(MENU_SETTINGS);
    render();
    return;
  }
  inSettingsMenu = false;
  if (settingsSelection == SETTINGS_LANGUAGE) {
    inLanguageSelect = true;
  } else if (settingsSelection == SETTINGS_TIMER_STEP) {
    inTimerStepSelect = true;
  }
  render();
}

// ---------------- Language: saves the chosen language to flash and returns to the main menu ----------------
void confirmLanguageSelection() {
  prefs.putUChar("lang", (uint8_t)currentLanguage);
  inLanguageSelect = false;
  inMenu = true;
  menuSelection = menuPositionForGame(MENU_SETTINGS);
  render();
}

// ---------------- Timer Step: saves the chosen step to flash and returns to the main menu ----------------
void confirmTimerStepSelection() {
  prefs.putUChar("step", (uint8_t)currentTimerStepIndex);
  inTimerStepSelect = false;
  inMenu = true;
  menuSelection = menuPositionForGame(MENU_SETTINGS);
  render();
}

// ---------------- Dice Roll: confirms the chosen die type and moves to the rolling screen ----------------
void confirmDiceType() {
  diceScreenState = DICE_ROLLING;
  diceResultCount = 0; // fresh start on the roll screen
  render();
  playArmSound();
}

// ---------------- Shared: ramped step size mirroring standard chess clock presets - fine-grained
// near bullet/blitz values, coarser toward classical values:
//   under 1:00 -> 10s, 1:00-5:00 -> 30s, 5:00-10:00 -> 1min, 10:00+ -> 5min
// Direction-aware so it also feels correct right at each boundary in both directions. Used by the
// Chess Timer's starting-time dial, and by the Timer game when its step setting is "Auto". ----------------
long rampedTimeStep(long fromSeconds, int direction) {
  long probe = (direction > 0) ? fromSeconds : fromSeconds - 1;
  if (probe < 60) return 10;
  if (probe < 300) return 30;
  if (probe < 600) return 60;
  return 300;
}

// ---------------- Chess Timer: confirms starting time, moves to the increment screen ----------------
void confirmChessStartTime() {
  chessScreenState = CHESS_SETUP_INCREMENT;
  render();
  playArmSound();
}

// ---------------- Chess Timer: confirms the increment, shows P1's time, but doesn't start ticking yet ----------------
void confirmChessIncrement() {
  chessTimeP1 = chessStartSeconds;
  chessTimeP2 = chessStartSeconds;
  chessMaxP1 = chessTimeP1; // reset the bar's reference for a new game
  chessMaxP2 = chessTimeP2;
  chessActivePlayer = 1;
  chessStarted = false; // waiting for the first press to actually start the clock
  chessPaused = false;
  chessLoser = 0;
  chessLockoutUntil = 0;
  chessScreenState = CHESS_PLAYING;
  render();
  playStartMelody();
}

// ---------------- Chess Timer: encoder click while playing - toggles pause ----------------
void toggleChessPause() {
  chessPaused = !chessPaused;
  if (!chessPaused) {
    chessLastTick = millis(); // resume cleanly, don't count the paused time as elapsed
  } else {
    digitalWrite(BUTTON_LED, LOW);
  }
  render();
}

// ---------------- Timer game: encoder click while running - toggles pause ----------------
void toggleTimerPause() {
  timerPaused = !timerPaused;
  if (!timerPaused) {
    lastSecondTick = millis(); // resume cleanly, don't count the paused time as elapsed
  } else {
    digitalWrite(BUTTON_LED, LOW);
  }
  render();
}

// ---------------- Chess Timer: arcade button press - starts the clock on the very first press, then passes turns ----------------
void doChessPass() {
  if (chessScreenState == CHESS_GAME_OVER) {
    chessScreenState = CHESS_SETUP_TIME;
    render();
    return;
  }
  if (chessPaused || millis() < chessLockoutUntil) return; // ignore while paused, or right after a pass (debounce/anti-double-tap)

  if (!chessStarted) {
    // first press: just starts P1's clock, nobody has moved yet so no increment/switch
    chessStarted = true;
    chessLastTick = millis();
    chessLockoutUntil = millis() + CHESS_PASS_LOCKOUT_MS;
    render();
    playNote(800, 60);
    return;
  }

  if (chessActivePlayer == 1) {
    chessTimeP1 += chessIncrementSeconds;
    if (chessTimeP1 > chessMaxP1) chessMaxP1 = chessTimeP1;
  } else {
    chessTimeP2 += chessIncrementSeconds;
    if (chessTimeP2 > chessMaxP2) chessMaxP2 = chessTimeP2;
  }

  chessActivePlayer = (chessActivePlayer == 1) ? 2 : 1;
  chessLastTick = millis();
  chessLockoutUntil = millis() + CHESS_PASS_LOCKOUT_MS;
  digitalWrite(BUTTON_LED, LOW);
  render();

  // 1 beep for Player 1's turn starting, 2 beeps for Player 2's
  for (int i = 0; i < chessActivePlayer; i++) {
    playNote(900, 50);
    if (i < chessActivePlayer - 1) delay(60);
  }
}

// ================= Hot Potato =================
// ---------------- Hot Potato: arms a hidden countdown, or does nothing while armed (the danger
// is real regardless of pressing) - press again after it explodes to reset. ----------------
const int HP_CATEGORY_STRINGS[HP_CATEGORY_COUNT] = {
  STR_HP_CAT_COUNTRIES, STR_HP_CAT_CITIES, STR_HP_CAT_FOOD, STR_HP_CAT_CELEBRITIES, STR_HP_CAT_CARTOON,
  STR_HP_CAT_HOME, STR_HP_CAT_SCHOOL, STR_HP_CAT_WEAR, STR_HP_CAT_SPORTS, STR_HP_CAT_BODYPARTS,
  STR_HP_CAT_INSTRUMENTS, STR_HP_CAT_RED, STR_HP_CAT_BLUE, STR_HP_CAT_GREEN, STR_HP_CAT_COLD,
  STR_HP_CAT_WARM, STR_HP_CAT_FLY, STR_HP_CAT_COLOURS, STR_HP_CAT_NOISE, STR_HP_CAT_BREAK,
};

// ---------------- Picks a random ENABLED category (falls back to category 0 in the pathological
// case that every category is disabled, which the web config page's own validation should prevent) ----------------
int hpRandomEnabledCategory() {
  bool anyEnabled = false;
  for (int i = 0; i < HP_CATEGORY_COUNT; i++) if (hpCategoryEnabled[i]) { anyEnabled = true; break; }
  if (!anyEnabled) return 0;

  int cat;
  do {
    cat = random(0, HP_CATEGORY_COUNT);
  } while (!hpCategoryEnabled[cat]);
  return cat;
}

// ---------------- Picks a brand new category+letter combo, silently (no animation) - used when
// first entering the game ----------------
void hpNewCombo() {
  hpCategory = hpRandomEnabledCategory();
  hpLetter = random(0, 26);
}

// ---------------- Turning the encoder in HP_READY rerolls the combo, with a quick slot-machine
// style flicker through random combos before settling on the final one ----------------
void hpRerollWithAnimation() {
  if (hpState != HP_READY) return;

  const int FRAMES = 14;
  for (int f = 0; f < FRAMES; f++) {
    hpCategory = hpRandomEnabledCategory();
    hpLetter = random(0, 26);
    render();
    int delayMs = 30 + (f * f); // starts fast, decelerates - classic slot-machine settle
    delay(delayMs);
    playNote(500 + random(0, 400), 15);
  }
  render();
}

void handleHotPotatoPress() {
  if (hpState == HP_READY) {
    hpState = HP_ARMED;
    hpArmTime = millis();
    hpDelay = random(HP_MIN_DELAY_MS, HP_MAX_DELAY_MS);
    hpLastBlinkToggle = millis();
    hpBlinkState = true;
    digitalWrite(BUTTON_LED, HIGH);
    render();
    playArmSound();
  } else if (hpState == HP_EXPLODED) {
    hpState = HP_READY;
    hpRerollWithAnimation(); // same shuffle animation as a manual reroll or the very first combo
    digitalWrite(BUTTON_LED, LOW);
  }
  // HP_ARMED: pressing does nothing - the bomb doesn't know it was "passed"
}

// ---------------- Pass the Bomb: checks if the hidden timer has run out, and blinks the LED at a
// steady rate the whole time it's armed - deliberately NOT speeding up or otherwise hinting how
// much time is left, so there's no visual or audible warning before it goes off. Once it has gone
// off, waits HP_EXPLODED_PAUSE_MS before automatically reshuffling into a fresh round - turning
// the encoder (handled in readEncoder()) skips straight past that wait. ----------------
void updateHotPotato() {
  if (hpState == HP_EXPLODED) {
    if (millis() - hpExplodedAt >= HP_EXPLODED_PAUSE_MS) {
      hpState = HP_READY;
      hpRerollWithAnimation();
      digitalWrite(BUTTON_LED, LOW);
    }
    return;
  }

  if (hpState != HP_ARMED) return;

  unsigned long elapsed = millis() - hpArmTime;
  if (elapsed >= hpDelay) {
    hpState = HP_EXPLODED;
    hpExplodedAt = millis();
    digitalWrite(BUTTON_LED, LOW);
    playBombAnimation();
    render();
    return;
  }

  if (millis() - hpLastBlinkToggle >= HP_BLINK_INTERVAL_MS) {
    hpLastBlinkToggle = millis();
    hpBlinkState = !hpBlinkState;
    digitalWrite(BUTTON_LED, hpBlinkState ? HIGH : LOW);
  }
}

// ================= Turn Order =================
// ---------------- Turn Order: spins and lands on a random starting player ----------------
void doTurnOrderSpin() {
  const int FRAMES = 12;
  for (int f = 0; f < FRAMES; f++) {
    turnOrderResult = random(1, turnOrderPlayerCount + 1);
    render();
    playNote(700 + random(0, 300), 20);
    delay(30 + f * 8);
  }
  turnOrderResult = random(1, turnOrderPlayerCount + 1);
  render();
  playNote(1300, 100);
}

// ================= Stopwatch =================
// ---------------- Stopwatch: press cycles Ready -> Running -> Stopped -> Ready ----------------
void handleStopwatchPress() {
  if (stopwatchState == STOPWATCH_READY) {
    stopwatchState = STOPWATCH_RUNNING;
    stopwatchStartMillis = millis();
    stopwatchElapsedMs = 0;
    stopwatchLapSeconds = -1;
    render();
    playArmSound();
  } else if (stopwatchState == STOPWATCH_RUNNING) {
    stopwatchElapsedMs += millis() - stopwatchStartMillis;
    stopwatchState = STOPWATCH_STOPPED;
    render();
    playNote(800, 60);
  } else { // STOPWATCH_STOPPED -> reset
    stopwatchState = STOPWATCH_READY;
    stopwatchElapsedMs = 0;
    stopwatchLapSeconds = -1;
    render();
  }
}

// ---------------- Stopwatch: re-renders often enough while running to keep the hundredths live ----------------
unsigned long stopwatchLastRenderMs = 0;
const unsigned long STOPWATCH_RENDER_INTERVAL_MS = 50; // 20fps - smooth enough for hundredths, not too heavy on the display
void updateStopwatch() {
  if (stopwatchState != STOPWATCH_RUNNING) return;
  if (millis() - stopwatchLastRenderMs >= STOPWATCH_RENDER_INTERVAL_MS) {
    stopwatchLastRenderMs = millis();
    render();
  }
}

// ---------------- Stopwatch: records the current elapsed time as a lap, while running ----------------
void recordStopwatchLap() {
  unsigned long totalMs = stopwatchElapsedMs + (millis() - stopwatchStartMillis);
  stopwatchLapSeconds = totalMs / 1000;
  playNote(1000, 40);
  render();
}

// ================= Toothbrush =================
// ---------------- Toothbrush: which of the 6 mouth zones is "up" right now, based on how far
// through the total brushing time we are (each zone gets an even sixth). Order: upper left,
// upper front, upper right, lower left, lower front, lower right (0-5). ----------------
int toothbrushCurrentZone() {
  if (toothbrushTotalSeconds <= 0) return 0;
  long elapsed = toothbrushTotalSeconds - toothbrushRemainingSeconds;
  int z = (int)((elapsed * TOOTHBRUSH_ZONE_COUNT) / toothbrushTotalSeconds);
  return constrain(z, 0, TOOTHBRUSH_ZONE_COUNT - 1);
}

// ---------------- Toothbrush: how far through the current zone we are, 0..1 (for the zone bar) ----------------
float toothbrushZoneFraction() {
  if (toothbrushTotalSeconds <= 0) return 0;
  long elapsed = toothbrushTotalSeconds - toothbrushRemainingSeconds;
  float zoneLen = (float)toothbrushTotalSeconds / TOOTHBRUSH_ZONE_COUNT;
  float f = (elapsed - toothbrushCurrentZone() * zoneLen) / zoneLen;
  return constrain(f, 0.0f, 1.0f);
}

// ---------------- Toothbrush: select-time confirm starts the run; running press toggles pause;
// done press resets back to the select screen ----------------
void handleToothbrushPress() {
  if (toothbrushScreenState == TOOTHBRUSH_SELECT_TIME) {
    toothbrushTotalSeconds = (long)toothbrushMinutes * 60;
    toothbrushRemainingSeconds = toothbrushTotalSeconds;
    toothbrushPaused = false;
    toothbrushLastZone = 0; // always starts on the upper-left zone
    toothbrushFrame = 0;
    toothbrushScreenState = TOOTHBRUSH_RUNNING;
    toothbrushLastTick = millis();
    toothbrushLastBrushSound = millis();
    digitalWrite(BUTTON_LED, HIGH);
    render();
    playStartMelody();
  } else if (toothbrushScreenState == TOOTHBRUSH_RUNNING) {
    toothbrushPaused = !toothbrushPaused;
    digitalWrite(BUTTON_LED, toothbrushPaused ? LOW : HIGH);
    render();
  } else { // TOOTHBRUSH_DONE
    toothbrushScreenState = TOOTHBRUSH_SELECT_TIME;
    digitalWrite(BUTTON_LED, LOW);
    render();
  }
}

// ---------------- Toothbrush: 1-second countdown tick, a zone-change beep whenever the zone
// advances, plus a periodic short click (with the LED, active zone and brush moving in time with
// it) simulating a brushing rhythm - this hardware's buzzer can't play real audio, so this is a
// rhythmic approximation, not a recording ----------------
void updateToothbrush() {
  if (toothbrushScreenState != TOOTHBRUSH_RUNNING || toothbrushPaused) return;

  if (millis() - toothbrushLastTick >= 1000) {
    toothbrushLastTick = millis();
    toothbrushRemainingSeconds--;
    if (toothbrushRemainingSeconds <= 0) {
      toothbrushRemainingSeconds = 0;
      toothbrushScreenState = TOOTHBRUSH_DONE;
      digitalWrite(BUTTON_LED, LOW);
      render();
      playToothbrushDoneAnimation();
      render();
      return;
    }

    int newZone = toothbrushCurrentZone();
    if (newZone != toothbrushLastZone) {
      toothbrushLastZone = newZone;
      playToothbrushZoneChangeBeep();
    }
    render();
  }

  if (millis() - toothbrushLastBrushSound >= TOOTHBRUSH_BRUSH_INTERVAL_MS) {
    toothbrushLastBrushSound = millis();
    toothbrushPulseState = !toothbrushPulseState;
    toothbrushFrame++;
    digitalWrite(BUTTON_LED, toothbrushPulseState ? HIGH : LOW);
    playNote(600 + random(0, 100), 25); // short soft click, slight pitch variation for a more organic feel
    render();
  }
}

// ---------------- Toothbrush: a distinct, longer low-to-high two-tone beep for zone changes.
// The low note (450Hz) sits BELOW the brushing clicks' pitch range (600-700Hz), and the high note
// (2200Hz) sits well ABOVE it - bracketing the brushing sound's pitch entirely, rather than just
// being a bit higher than it. Each note is also ~8x longer than a brushing click (25ms), giving it
// a completely different rhythm/timbre, not just a different pitch - so it can't blend into the
// background clicking pattern. ----------------
void playToothbrushZoneChangeBeep() {
  playNote(450, 180);
  delay(40);
  playNote(2200, 220);
}

// ---------------- Toothbrush: played once when the countdown finishes - reuses the exact same
// firework celebration as the Reflex game's new-high-score moment, rather than a separate
// bespoke animation ----------------
void playToothbrushDoneAnimation() {
  playFireworkAnimation();
}

// ---------------- Draws a simple walrus face: round head, eyes, two tusks. Wounded = X eyes,
// shorter drooping tusks, and a few small impact-star marks around the head. Shared by the
// SimpleMorse menu icon (tiny, r~5-6) and its game-over illustration (much bigger). ----------------
void drawWalrusFace(int cx, int cy, int headR, bool wounded, uint16_t color) {
  display.drawCircle(cx, cy, headR, color);

  if (wounded) {
    int es = max(1, headR / 4);
    for (int i = 0; i < 2; i++) {
      int ex = cx + (i == 0 ? -headR / 2 : headR / 2);
      int ey = cy - headR / 3;
      display.drawLine(ex - es, ey - es, ex + es, ey + es, color);
      display.drawLine(ex - es, ey + es, ex + es, ey - es, color);
    }
  } else {
    int ex1 = cx - headR / 2, ex2 = cx + headR / 2, ey = cy - headR / 4;
    display.fillCircle(ex1, ey, max(1, headR / 8), color);
    display.fillCircle(ex2, ey, max(1, headR / 8), color);
  }

  int tuskLen = wounded ? headR / 3 : (headR * 2) / 3;
  int tx = headR / 2, ty = cy + headR / 2;
  display.drawLine(cx - tx, ty, cx - tx - headR / 6, ty + tuskLen, color);
  display.drawLine(cx + tx, ty, cx + tx + headR / 6, ty + tuskLen, color);
}

// ---------------- SimpleMorse wrong-input result: a quick shake of the (still-fine) walrus, a
// sad descending tone, then it settles on the wounded face - which stays on screen afterward as
// the Game Over illustration itself. Blocking, same style as this sketch's other short animations. ----------------
void playWoundedWalrusAnimation() {
  stopTone(); // defensive: guarantee no lingering held-tone before this animation's own sound cues
  int cx = SCREEN_WIDTH / 2, cy = 38, headR = 18;

  for (int shake = 0; shake < 3; shake++) {
    display.clearDisplay();
    drawWalrusFace(cx + (shake % 2 == 0 ? -3 : 3), cy, headR, false, SH110X_WHITE);
    display.display();
    playNote(300, 60);
    delay(70);
  }

  display.clearDisplay();
  drawWalrusFace(cx, cy, headR, true, SH110X_WHITE);
  display.display();
  playNote(300, 200);
  playNote(220, 250);
  delay(300);
}

// ================= Tetris =================
// ---------------- Returns true if any filled cell of the given piece/rotation/position would be
// outside the board or overlapping an already-locked cell ----------------
bool tetrisCollision(int pieceType, int rotation, int row, int col) {
  uint16_t shape = TETRIS_SHAPES[pieceType][rotation];
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      if (!(shape & (1 << (r * 4 + c)))) continue;
      int boardRow = row + r;
      int boardCol = col + c;
      if (boardCol < 0 || boardCol >= TETRIS_BOARD_COLS || boardRow >= TETRIS_BOARD_ROWS) return true;
      if (boardRow >= 0 && tetrisBoard[boardRow][boardCol]) return true;
    }
  }
  return false;
}

// ---------------- Spawns a new random piece centered at the top of the board. If it immediately
// collides, the stack is too tall - game over. ----------------
void tetrisSpawnPiece() {
  tetrisPieceType = random(0, 7);
  tetrisPieceRotation = 0;
  tetrisPieceRow = 0;
  tetrisPieceCol = (TETRIS_BOARD_COLS - 4) / 2;

  if (tetrisCollision(tetrisPieceType, tetrisPieceRotation, tetrisPieceRow, tetrisPieceCol)) {
    tetrisState = TETRIS_GAME_OVER;
    digitalWrite(BUTTON_LED, LOW);
    if (tetrisScore > tetrisHighScore) {
      tetrisHighScore = tetrisScore;
      prefs.putUShort("ttHigh", (uint16_t)min(tetrisHighScore, 65535)); // stored as 16-bit - clamp instead of wrapping
    }
    playTetrisGameOverAnimation();
  }
}

// ---------------- Game over animation: the board fills solid from the bottom row up (like
// "topping out" in classic Tetris), then flashes a few times, with a descending three-note cue
// timed to the flashes. Blocking, like the other games' celebration/end animations - runs once,
// then the normal render() takes over and shows the Game Over screen. ----------------
void playTetrisGameOverAnimation() {
  display.setRotation(1);

  for (int r = TETRIS_BOARD_ROWS - 1; r >= 0; r--) {
    display.fillRect(0, TETRIS_HUD_HEIGHT + r * TETRIS_CELL_SIZE, TETRIS_BOARD_COLS * TETRIS_CELL_SIZE, TETRIS_CELL_SIZE, SH110X_WHITE);
    display.display();
    delay(18);
  }

  const int notes[3] = {400, 300, 200};
  for (int i = 0; i < 3; i++) {
    display.fillRect(0, TETRIS_HUD_HEIGHT, TETRIS_BOARD_COLS * TETRIS_CELL_SIZE, TETRIS_BOARD_ROWS * TETRIS_CELL_SIZE, SH110X_BLACK);
    display.display();
    playNote(notes[i], 110);
    display.fillRect(0, TETRIS_HUD_HEIGHT, TETRIS_BOARD_COLS * TETRIS_CELL_SIZE, TETRIS_BOARD_ROWS * TETRIS_CELL_SIZE, SH110X_WHITE);
    display.display();
    delay(90);
  }

  display.setRotation(0);
}

// ---------------- Locks the current piece into the board, clears any completed rows, and scores
// them, then spawns the next piece ----------------
// ---------------- Quick blink animation for completed rows: draws the current locked board once,
// then toggles just those row(s) between empty and filled a couple of times, before they actually
// get removed. Blocking, like the game's other brief animations. ----------------
void tetrisFlashLines(int* rows, int count) {
  display.setRotation(1);

  display.fillRect(0, 0, display.width(), display.height(), SH110X_BLACK);
  for (int r = 0; r < TETRIS_BOARD_ROWS; r++) {
    for (int c = 0; c < TETRIS_BOARD_COLS; c++) {
      if (tetrisBoard[r][c]) {
        display.fillRect(c * TETRIS_CELL_SIZE, TETRIS_HUD_HEIGHT + r * TETRIS_CELL_SIZE, TETRIS_CELL_SIZE - 1, TETRIS_CELL_SIZE - 1, SH110X_WHITE);
      }
    }
  }
  display.display();
  delay(70);

  for (int blink = 0; blink < 2; blink++) {
    for (int i = 0; i < count; i++) {
      display.fillRect(0, TETRIS_HUD_HEIGHT + rows[i] * TETRIS_CELL_SIZE, TETRIS_BOARD_COLS * TETRIS_CELL_SIZE, TETRIS_CELL_SIZE, SH110X_BLACK);
    }
    display.display();
    delay(80);
    for (int i = 0; i < count; i++) {
      display.fillRect(0, TETRIS_HUD_HEIGHT + rows[i] * TETRIS_CELL_SIZE, TETRIS_BOARD_COLS * TETRIS_CELL_SIZE, TETRIS_CELL_SIZE, SH110X_WHITE);
    }
    display.display();
    delay(80);
  }

  display.setRotation(0);
}

void tetrisLockPiece() {
  uint16_t shape = TETRIS_SHAPES[tetrisPieceType][tetrisPieceRotation];
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      if (!(shape & (1 << (r * 4 + c)))) continue;
      int boardRow = tetrisPieceRow + r;
      int boardCol = tetrisPieceCol + c;
      if (boardRow >= 0 && boardRow < TETRIS_BOARD_ROWS) tetrisBoard[boardRow][boardCol] = 1;
    }
  }

  // find completed rows first, without removing them yet, so they can be flashed in place
  int completedRows[TETRIS_BOARD_ROWS];
  int cleared = 0;
  for (int row = 0; row < TETRIS_BOARD_ROWS; row++) {
    bool full = true;
    for (int c = 0; c < TETRIS_BOARD_COLS; c++) {
      if (!tetrisBoard[row][c]) { full = false; break; }
    }
    if (full) completedRows[cleared++] = row;
  }

  if (cleared > 0) {
    tetrisFlashLines(completedRows, cleared);

    // now actually remove them and shift everything above down
    for (int row = TETRIS_BOARD_ROWS - 1; row >= 0; row--) {
      bool full = true;
      for (int c = 0; c < TETRIS_BOARD_COLS; c++) {
        if (!tetrisBoard[row][c]) { full = false; break; }
      }
      if (full) {
        for (int r2 = row; r2 > 0; r2--) {
          for (int c = 0; c < TETRIS_BOARD_COLS; c++) tetrisBoard[r2][c] = tetrisBoard[r2 - 1][c];
        }
        for (int c = 0; c < TETRIS_BOARD_COLS; c++) tetrisBoard[0][c] = 0;
        row++; // re-check this row index, now filled by the row above
      }
    }

    const int SCORE_TABLE[5] = {0, 40, 100, 300, 1200};
    int level = tetrisLinesCleared / 10;
    tetrisScore += SCORE_TABLE[min(cleared, 4)] * (level + 1);
    tetrisLinesCleared += cleared;
    playNote(900, 60);
    playNote(1200, 80);
  }

  if (tetrisState == TETRIS_PLAYING) tetrisSpawnPiece(); // don't spawn on top of a game-over from this same lock
}

// ---------------- Encoder rotation: moves the piece left/right ----------------
void tetrisMove(int direction) {
  if (tetrisState != TETRIS_PLAYING) return;
  int newCol = tetrisPieceCol + direction;
  if (!tetrisCollision(tetrisPieceType, tetrisPieceRotation, tetrisPieceRow, newCol)) {
    tetrisPieceCol = newCol;
    render();
  }
}

// ---------------- Arcade button: rotates the piece one step clockwise, if there's room ----------------
void tetrisRotate() {
  if (tetrisState != TETRIS_PLAYING) return;
  int newRotation = (tetrisPieceRotation + 1) % 4;
  if (!tetrisCollision(tetrisPieceType, newRotation, tetrisPieceRow, tetrisPieceCol)) {
    tetrisPieceRotation = newRotation;
    render();
  }
}

// ---------------- Encoder click: drops the piece straight down and locks it immediately - a
// single discrete action, so (unlike the old hold-to-soft-drop) it can never compete with the
// encoder's own 1s-hold-for-menu gesture. ----------------
void tetrisHardDrop() {
  if (tetrisState != TETRIS_PLAYING) return;
  while (!tetrisCollision(tetrisPieceType, tetrisPieceRotation, tetrisPieceRow + 1, tetrisPieceCol)) {
    tetrisPieceRow++;
  }
  tetrisLockPiece();
  tetrisLastDropTime = millis();
  render();
}

// ---------------- Ghost piece: where the current piece would land if hard-dropped right now,
// from its current column/rotation - same resting-row logic as tetrisHardDrop, just without
// actually moving or locking anything. Drawn as an outline on the play screen. ----------------
int tetrisGhostRow() {
  int ghostRow = tetrisPieceRow;
  while (!tetrisCollision(tetrisPieceType, tetrisPieceRotation, ghostRow + 1, tetrisPieceCol)) {
    ghostRow++;
  }
  return ghostRow;
}

// ---------------- Arcade button short-press: starts a new game from Ready, restarts from Game
// Over, or rotates the piece during active play ----------------
void handleTetrisButtonPress() {
  if (tetrisState == TETRIS_READY) {
    for (int r = 0; r < TETRIS_BOARD_ROWS; r++) {
      for (int c = 0; c < TETRIS_BOARD_COLS; c++) tetrisBoard[r][c] = 0;
    }
    tetrisScore = 0;
    tetrisLinesCleared = 0;
    tetrisState = TETRIS_PLAYING;
    tetrisSpawnPiece();
    tetrisLastDropTime = millis();
    render();
  } else if (tetrisState == TETRIS_GAME_OVER) {
    tetrisState = TETRIS_READY;
    render();
  } else {
    tetrisRotate();
  }
}

// ---------------- Called every loop() pass: handles ordinary gravity (speeds up gradually as
// lines clear) - no more hold-to-accelerate, since a hard drop is now an instant encoder click ----------------
void updateTetris() {
  if (tetrisState != TETRIS_PLAYING) return;
  if (inLeaveConfirm) return; // paused while the "Go back to menu?" dialog is up

  int level = tetrisLinesCleared / 10;
  unsigned long interval = max(120, 700 - level * 50);

  if (millis() - tetrisLastDropTime < interval) return;
  tetrisLastDropTime = millis();

  int newRow = tetrisPieceRow + 1;
  if (!tetrisCollision(tetrisPieceType, tetrisPieceRotation, newRow, tetrisPieceCol)) {
    tetrisPieceRow = newRow;
  } else {
    tetrisLockPiece();
  }
  render();
}

// ================= Star Drop =================
// ---------------- Resets the board and starts a new round ----------------
void stardropStart() {
  for (int i = 0; i < STARDROP_MAX_OBJECTS; i++) stardropObjActive[i] = false;
  stardropCatcherLane = STARDROP_LANES / 2;
  stardropScore = 0;
  stardropCatches = 0;
  stardropLives = STARDROP_START_LIVES;
  stardropState = STARDROP_PLAYING;
  stardropLastTick = millis();
  stardropLastSpawn = millis();
  render();
}

// ---------------- Points per object type: stars are common/low value, moons are rare/high value ----------------
int stardropPointsForType(int type) {
  const int POINTS[3] = {10, 20, 30}; // star, planet, moon
  return POINTS[type];
}

// ---------------- Spawns a new falling object in a random lane, if a free slot exists.
// Type is weighted so stars (common, low value) appear more often than moons (rare, high value). ----------------
void stardropSpawnObject() {
  int slot = -1;
  for (int i = 0; i < STARDROP_MAX_OBJECTS; i++) {
    if (!stardropObjActive[i]) { slot = i; break; }
  }
  if (slot < 0) return; // pool full, skip this spawn

  int roll = random(0, 10);
  int type = (roll < 6) ? 0 : (roll < 9) ? 1 : 2; // 60% star, 30% planet, 10% moon

  stardropObjActive[slot] = true;
  stardropObjLane[slot] = random(0, STARDROP_LANES);
  stardropObjY[slot] = STARDROP_HUD_HEIGHT;
  stardropObjType[slot] = type;
}

// ---------------- Encoder: moves the telescope one lane left/right ----------------
void stardropMoveCatcher(int direction) {
  if (stardropState != STARDROP_PLAYING) return;
  stardropCatcherLane = constrain(stardropCatcherLane + direction, 0, STARDROP_LANES - 1);
  render();
}

// ---------------- Arcade button: starts from Ready, restarts from Game Over ----------------
void handleStarDropButtonPress() {
  if (stardropState == STARDROP_READY || stardropState == STARDROP_GAME_OVER) {
    stardropStart();
  }
}

// ---------------- Ends the round: updates the high score in flash if beaten, plays the animation ----------------
void stardropGameOver() {
  stardropState = STARDROP_GAME_OVER;
  digitalWrite(BUTTON_LED, LOW);
  if (stardropScore > stardropHighScore) {
    stardropHighScore = stardropScore;
    prefs.putUShort("sdHigh", (uint16_t)min(stardropHighScore, 65535)); // stored as 16-bit - clamp instead of wrapping
  }
  playStarDropGameOverAnimation();
}

// ---------------- Game over animation: a few quick screen-wide flashes with a descending cue,
// simpler than Tetris's wipe (there's no "board" to fill here) ----------------
void playStarDropGameOverAnimation() {
  display.setRotation(1);
  const int notes[3] = {400, 300, 200};
  for (int i = 0; i < 3; i++) {
    display.fillRect(0, 0, display.width(), display.height(), SH110X_BLACK);
    display.display();
    playNote(notes[i], 110);
    display.fillRect(0, 0, display.width(), display.height(), SH110X_WHITE);
    display.display();
    delay(90);
  }
  display.setRotation(0);
}

// ---------------- Called every loop() pass: spawns new objects, advances the fall, and resolves
// catches/misses when an object reaches the catcher's row ----------------
void updateStarDrop() {
  if (stardropState != STARDROP_PLAYING) return;
  if (inLeaveConfirm) return;

  int level = stardropCatches / 8;
  unsigned long fallInterval = max(30, 95 - level * 6);         // ms per 1px fall step - starts faster, still speeds up with catches
  unsigned long spawnInterval = max(650, 1400 - level * 50);    // ms between spawns - more spaced out, tightens up a bit as you level up

  if (millis() - stardropLastSpawn >= spawnInterval) {
    stardropLastSpawn = millis();
    stardropSpawnObject();
  }

  if (millis() - stardropLastTick < fallInterval) return;
  stardropLastTick = millis();

  for (int i = 0; i < STARDROP_MAX_OBJECTS; i++) {
    if (!stardropObjActive[i]) continue;
    stardropObjY[i] += 1;

    if (stardropObjY[i] >= STARDROP_CATCHER_Y) {
      stardropObjActive[i] = false;
      if (stardropObjLane[i] == stardropCatcherLane) {
        stardropScore += stardropPointsForType(stardropObjType[i]);
        stardropCatches++;
        playNote(900, 40);
        playNote(1300, 50);
      } else {
        stardropLives--;
        playNote(250, 120);
        if (stardropLives <= 0) {
          stardropGameOver();
          render();
          return;
        }
      }
    }
  }
  render();
}

// ================= Snake =================
// ---------------- Places food on a random cell that isn't currently occupied by the snake ----------------
void snakeSpawnFood() {
  while (true) {
    int r = random(0, SNAKE_ROWS);
    int c = random(0, SNAKE_COLS);
    bool onSnake = false;
    for (int i = 0; i < snakeLength; i++) {
      if (snakeRow[i] == r && snakeCol[i] == c) { onSnake = true; break; }
    }
    if (!onSnake) {
      snakeFoodRow = r;
      snakeFoodCol = c;
      return;
    }
  }
}

// ---------------- Resets the board and starts a new round: a 3-segment snake in the middle,
// heading right ----------------
void snakeStart() {
  snakeLength = 3;
  int startRow = SNAKE_ROWS / 2;
  int startCol = SNAKE_COLS / 2;
  for (int i = 0; i < snakeLength; i++) {
    snakeRow[i] = startRow;
    snakeCol[i] = startCol - i; // head at startCol, body trailing to the left
  }
  snakeDirection = 1; // heading right
  snakeTurnedThisTick = false;
  snakeScore = 0;
  snakeState = SNAKE_PLAYING;
  snakeLastMoveTime = millis();
  snakeSpawnFood();
  render();
}

// ---------------- Arcade button (and encoder click, see readEncoderButton): start from Ready,
// restart from Game Over, or pause/resume during play - Snake has no other button-driven action,
// since turning is the encoder's job, so the button is free for this instead of the encoder click
// pulling double duty like it does in Timer/Chess. ----------------
void handleSnakeButtonPress() {
  if (snakeState == SNAKE_READY || snakeState == SNAKE_GAME_OVER) {
    snakeStart();
  } else if (snakeState == SNAKE_PLAYING) {
    snakeState = SNAKE_PAUSED;
    render();
  } else if (snakeState == SNAKE_PAUSED) {
    snakeState = SNAKE_PLAYING;
    snakeLastMoveTime = millis(); // don't let the paused time count as a missed move
    render();
  }
}

void snakeGameOver() {
  snakeState = SNAKE_GAME_OVER;
  digitalWrite(BUTTON_LED, LOW);
  if (snakeScore > snakeHighScore) {
    snakeHighScore = snakeScore;
    prefs.putUShort("skHigh", (uint16_t)min(snakeHighScore, 65535)); // stored as 16-bit - clamp instead of wrapping
  }
  playNote(300, 90);
  playNote(220, 140);
}

// ---------------- Called every loop() pass: moves the snake one cell in its current heading at a
// fixed tick rate that gradually speeds up with score, same ramp shape as Star Drop's fall speed ----------------
void updateSnake() {
  if (snakeState != SNAKE_PLAYING) return;
  if (inLeaveConfirm) return;

  int level = snakeScore / 5;
  unsigned long interval = max(80, 220 - level * 10);
  if (millis() - snakeLastMoveTime < interval) return;
  snakeLastMoveTime = millis();
  snakeTurnedThisTick = false; // this move is happening now - a fresh turn is allowed again until the next one

  int headRow = snakeRow[0], headCol = snakeCol[0];
  // direction: 0=up, 1=right, 2=down, 3=left
  int newRow = headRow + (snakeDirection == 2) - (snakeDirection == 0);
  int newCol = headCol + (snakeDirection == 1) - (snakeDirection == 3);

  // wrap around at the edges instead of dying - the only way to lose is running into your own tail
  newRow = (newRow + SNAKE_ROWS) % SNAKE_ROWS;
  newCol = (newCol + SNAKE_COLS) % SNAKE_COLS;

  bool ateFood = (newRow == snakeFoodRow && newCol == snakeFoodCol);
  int growTo = ateFood ? min(snakeLength + 1, SNAKE_MAX_LENGTH) : snakeLength;

  // Self-collision. When the snake isn't growing this move, its tail cell is vacated in the same
  // tick, so moving into it is legal - it's excluded from the check.
  int checkCount = (growTo == snakeLength) ? snakeLength - 1 : snakeLength;
  for (int i = 0; i < checkCount; i++) {
    if (snakeRow[i] == newRow && snakeCol[i] == newCol) {
      snakeGameOver();
      render();
      return;
    }
  }
  for (int i = growTo - 1; i > 0; i--) {
    snakeRow[i] = snakeRow[i - 1];
    snakeCol[i] = snakeCol[i - 1];
  }
  snakeRow[0] = newRow;
  snakeCol[0] = newCol;
  snakeLength = growTo;

  if (ateFood) {
    snakeScore++;
    playNote(1100, 30);
    snakeSpawnFood();
  }

  render();
}

// ================= SimpleMorse =================
// ---------------- Picks a random word and flattens its Morse code into a single dot/dash
// sequence (letter boundaries aren't preserved - the whole word is one continuous pattern to
// repeat, matching how the "10 to 15 presses" requirement was specified). ----------------
void simonNewSequence() {
  int idx = random(0, SIMON_WORD_COUNT);
  strncpy(simonWord, SIMON_WORDS[idx], sizeof(simonWord) - 1);
  simonWord[sizeof(simonWord) - 1] = '\0';

  simonSequenceLength = 0;
  simonLetterCount = 0;
  for (int i = 0; simonWord[i] != '\0' && simonSequenceLength < SIMON_MAX_LENGTH; i++) {
    char ch = simonWord[i];
    if (ch < 'A' || ch > 'Z') continue;
    const char* code = MORSE_TABLE[ch - 'A'];
    for (int j = 0; code[j] != '\0' && simonSequenceLength < SIMON_MAX_LENGTH; j++) {
      simonSequence[simonSequenceLength++] = (code[j] == '-') ? 1 : 0;
    }
    if (simonLetterCount < (int)(sizeof(simonLetterEndIndex) / sizeof(simonLetterEndIndex[0]))) {
      simonLetterEndIndex[simonLetterCount++] = simonSequenceLength;
    }
  }
}

// ---------------- Plays back the first simonRound symbols of the sequence: LED + tone for each,
// dashes held noticeably longer than dots, with a gap between symbols. Blocking, same style as
// this sketch's other short animations - simplest way to guarantee the timing is exactly right. ----------------
void simonPlayback() {
  stopTone(); // defensive: guarantee no lingering held-tone from the input that triggered this
  simonState = SIMON_PLAYBACK;
  render();
  delay(400); // beat before playback starts, so it doesn't feel like it's cutting in right after input

  for (int i = 0; i < simonRound; i++) {
    bool isDash = simonSequence[i] == 1;
    int toneDur = isDash ? 550 : 220;
    digitalWrite(BUTTON_LED, HIGH);
    playNote(880, toneDur); // blocks for toneDur, then silences
    digitalWrite(BUTTON_LED, LOW);
    delay(220);
  }

  simonState = SIMON_INPUT;
  simonInputIndex = 0;
  render();
}

// ---------------- Arcade button: start, retry after a result, or (while SIMON_INPUT) the actual
// gameplay input - classified short/long from how long the button was just held, using the same
// buttonPressStart timestamp the Reflex game already relies on for its own timing. ----------------
void handleSimonButtonPress() {
  if (simonState == SIMON_READY) {
    simonRound = 1;
    simonPlayback();
    return;
  }
  if (simonState == SIMON_GAME_OVER || simonState == SIMON_COMPLETE) {
    simonNewSequence();
    simonRound = 0;
    simonState = SIMON_READY;
    render();
    return;
  }
  if (simonState != SIMON_INPUT) return;
  if (simonInputIndex < 0 || simonInputIndex >= simonSequenceLength || simonInputIndex >= SIMON_MAX_LENGTH) {
    // defensive: should never happen, but guarantees simonSequence[] is never read out of bounds
    simonState = SIMON_GAME_OVER;
    render();
    return;
  }

  unsigned long heldMs = millis() - buttonPressStart;
  int pressed = (heldMs >= SIMON_DASH_THRESHOLD_MS) ? 1 : 0;

  if (pressed != simonSequence[simonInputIndex]) {
    simonState = SIMON_GAME_OVER;
    playWoundedWalrusAnimation(); // ends with the wounded walrus already drawn and on screen
    delay(1500); // hold that result before revealing the solution
    render(); // now shows the solution (word + code), per drawSimonScreen()
    return;
  }

  // correct so far - the live tone+LED during the hold (readButton()) already told the player
  // what they just did, so there's no separate confirmation sound here - just move on to waiting
  // for the next press, or (round complete) advance to a longer playback / finish the sequence
  simonInputIndex++;

  if (simonInputIndex < simonRound) {
    render();
    return;
  }

  if (simonRound >= simonSequenceLength) {
    simonState = SIMON_COMPLETE;
    stopTone(); // defensive: guarantee no lingering held-tone before the celebration's own sounds
    playToothbrushDoneAnimation(); // ends with its own final celebratory frame on screen
    delay(1500); // hold that result before revealing the solution
    render(); // now shows the solution (word + code), per drawSimonScreen()
  } else {
    simonRound++;
    delay(500);
    simonPlayback();
  }
}


// ---------------- Trivia: builds the LittleFS path for the given real category (0-3) and current level ----------------
void triviaFilePath(int category, char* out, size_t outSize) {
  const char* catNames[TRIVIA_REAL_CATEGORY_COUNT] = {"sports", "geography", "science", "history", "general", "mythology", "art", "animals"};
  const char* lvlNames[TRIVIA_LEVEL_COUNT] = {"junior", "curious", "master"};
  snprintf(out, outSize, "/%s_%s.csv", catNames[category], lvlNames[triviaLevel]);
}

// ---------------- Trivia: counts how many non-empty rows are actually in the given category/level's
// file - so the game uses everything you've added, not just a hardcoded assumption. ----------------
int countTriviaQuestions(int category) {
  char path[40];
  triviaFilePath(category, path, sizeof(path));

  File f = LittleFS.open(path, "r");
  if (!f) return 0;

  if (f.available()) f.readStringUntil('\n'); // skip the header row

  int count = 0;
  while (f.available() && count < TRIVIA_MAX_QUESTIONS) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) count++; // ignore a trailing blank line at the end of the file
  }
  f.close();
  return count;
}

// ---------------- Trivia: loads question #index from the given real category's CSV file at the
// current level. Each row has 15 columns, 5 per language: question,correct,wrong1,wrong2,wrong3
// (en block, then fr block, then es block). Wrong slots may be blank (e.g. True/False questions
// only have 1 wrong answer) - those are simply skipped. Builds a freshly-shuffled display order
// each time a question loads, so the correct answer isn't always in the same position. ----------------
bool loadTriviaQuestion(int category, int index) {
  char path[40];
  triviaFilePath(category, path, sizeof(path));

  File f = LittleFS.open(path, "r");
  if (!f) {
    snprintf(triviaQuestionBuf, sizeof(triviaQuestionBuf), "Missing file: %s", path);
    triviaOptionCount = 0;
    triviaCorrectOption = -1;
    return false;
  }

  if (f.available()) f.readStringUntil('\n'); // skip the header row - index 0 is the first real question

  int lineNum = 0;
  String line = "";
  while (f.available()) {
    line = f.readStringUntil('\n');
    if (lineNum == index) break;
    lineNum++;
  }
  f.close();

  const int FIELDS_PER_ROW = 15;
  String fields[FIELDS_PER_ROW];
  int fieldIdx = 0;
  int fieldStart = 0;
  int len = line.length();
  for (int i = 0; i <= len && fieldIdx < FIELDS_PER_ROW; i++) {
    if (i == len || line[i] == ',') {
      fields[fieldIdx] = line.substring(fieldStart, i);
      fields[fieldIdx].trim(); // drop trailing \r and stray whitespace
      fieldIdx++;
      fieldStart = i + 1;
    }
  }

  int blockStart = currentLanguage * 5;
  fields[blockStart].toCharArray(triviaQuestionBuf, sizeof(triviaQuestionBuf));

  // gather whichever options are actually present (correct + up to 3 non-blank wrong ones)
  String presentOptions[TRIVIA_MAX_OPTIONS];
  bool presentIsCorrect[TRIVIA_MAX_OPTIONS];
  int presentCount = 0;

  presentOptions[presentCount] = fields[blockStart + 1];
  presentIsCorrect[presentCount] = true;
  presentCount++;

  for (int w = 2; w <= 4; w++) {
    String opt = fields[blockStart + w];
    if (opt.length() > 0) {
      presentOptions[presentCount] = opt;
      presentIsCorrect[presentCount] = false;
      presentCount++;
    }
  }

  // shuffle the present options together, tracking where the correct one lands
  for (int i = presentCount - 1; i > 0; i--) {
    int j = random(0, i + 1);
    String tmpOpt = presentOptions[i]; presentOptions[i] = presentOptions[j]; presentOptions[j] = tmpOpt;
    bool tmpCorrect = presentIsCorrect[i]; presentIsCorrect[i] = presentIsCorrect[j]; presentIsCorrect[j] = tmpCorrect;
  }

  triviaOptionCount = presentCount;
  triviaCorrectOption = -1;
  for (int i = 0; i < presentCount; i++) {
    presentOptions[i].toCharArray(triviaOptionBuf[i], sizeof(triviaOptionBuf[i]));
    if (presentIsCorrect[i]) triviaCorrectOption = i;
  }

  return true;
}

// ---------------- Trivia: shuffles the question order for a fresh round - fixed once set, so
// turning back always lands on the exact same question, not a new random pick. In Random category
// mode, combines all 4 real categories' questions into one shuffled pool (packed as category*10000+index). ----------------
void shuffleTriviaOrder() {
  if (triviaCategory == TRIVIA_CATEGORY_RANDOM) {
    int pos = 0;
    for (int cat = 0; cat < TRIVIA_REAL_CATEGORY_COUNT; cat++) {
      int count = countTriviaQuestions(cat);
      for (int i = 0; i < count && pos < TRIVIA_MAX_QUESTIONS; i++) {
        triviaQuestionOrder[pos++] = cat * 10000 + i;
      }
    }
    triviaQuestionCount = pos;
  } else {
    triviaQuestionCount = countTriviaQuestions(triviaCategory);
    for (int i = 0; i < triviaQuestionCount; i++) triviaQuestionOrder[i] = i;
  }
  if (triviaQuestionCount < 1) triviaQuestionCount = 1; // safety floor - avoids a divide-by-zero if a file is empty/missing

  for (int i = triviaQuestionCount - 1; i > 0; i--) {
    int j = random(0, i + 1);
    int tmp = triviaQuestionOrder[i];
    triviaQuestionOrder[i] = triviaQuestionOrder[j];
    triviaQuestionOrder[j] = tmp;
  }
}

// ---------------- Trivia: loads whatever's at the current shuffle position - unpacking the real
// category from the packed value first, if in Random mode ----------------
void loadCurrentTriviaOrderEntry() {
  int packed = triviaQuestionOrder[triviaOrderPosition];
  if (triviaCategory == TRIVIA_CATEGORY_RANDOM) {
    triviaActiveCategory = packed / 10000;
    loadTriviaQuestion(triviaActiveCategory, packed % 10000);
  } else {
    triviaActiveCategory = triviaCategory;
    loadTriviaQuestion(triviaActiveCategory, packed);
  }
}

// ---------------- Trivia: moves one step through the single Question<->Answers<->next Question
// sequence - direction +1 goes Question->Answers->(next Question), direction -1 goes the same
// path backwards. Reuses the existing shuffled order, so revisiting a question you've already
// been to always shows the exact same content rather than re-randomizing it. ----------------
void moveTriviaStep(int direction) {
  bool changedQuestion = false;
  triviaViewShownAt = millis(); // any view change restarts long-text scrolling from the start

  if (direction > 0) {
    if (!triviaShowingAnswers) {
      triviaShowingAnswers = true; // Question -> Answers, same question
    } else {
      triviaOrderPosition = (triviaOrderPosition + 1) % triviaQuestionCount; // Answers -> next Question
      triviaShowingAnswers = false;
      changedQuestion = true;
    }
  } else {
    if (triviaShowingAnswers) {
      triviaShowingAnswers = false; // Answers -> Question, same question
    } else {
      triviaOrderPosition = (triviaOrderPosition - 1 + triviaQuestionCount) % triviaQuestionCount; // Question -> previous Answers
      triviaShowingAnswers = true;
      changedQuestion = true;
    }
  }

  if (changedQuestion) {
    triviaAnswering = false;
    triviaAnswerVisible = false;
    loadCurrentTriviaOrderEntry();
    digitalWrite(BUTTON_LED, HIGH); // ready to buzz on the new question
    playNote(700, 40); // short click confirming the move
  }
  render();
}

// ---------------- Trivia: confirms the category, moves to the level screen ----------------
void confirmTriviaCategory() {
  triviaScreenState = TRIVIA_SELECT_LEVEL;
  render();
  playArmSound();
}

// ---------------- Trivia: confirms the level, shuffles a fresh order (across all 4 categories if
// Random was picked), and starts the round ----------------
void confirmTriviaLevel() {
  triviaScreenState = TRIVIA_PLAYING;
  shuffleTriviaOrder();
  triviaOrderPosition = 0;
  triviaAnswering = false;
  triviaAnswerVisible = false;
  triviaShowingAnswers = false;
  loadCurrentTriviaOrderEntry();
  triviaViewShownAt = millis();
  digitalWrite(BUTTON_LED, HIGH); // ready to buzz
  render();
  playStartMelody();
}

// ---------------- Trivia: category/level confirm, or buzzing in on the play screen ----------------
void handleTriviaButtonPress() {
  if (triviaScreenState == TRIVIA_SELECT_CATEGORY) {
    confirmTriviaCategory();
  } else if (triviaScreenState == TRIVIA_SELECT_LEVEL) {
    confirmTriviaLevel();
  } else {
    // buzz in - whether this is the first press or an interrupt of an already-in-progress
    // answer window, the effect is the same: reveal the answer and (re)start the 5s window
    // fresh from zero, so a second buzz-in restarts the countdown rather than being ignored
    if (!triviaShowingAnswers) triviaViewShownAt = millis(); // switching to the Answers view
    triviaAnswerVisible = true;
    triviaAnswering = true;
    triviaShowingAnswers = true;
    triviaAnswerRevealTime = millis();
    triviaLastBipSecond = 0;
    triviaBlinkState = true;
    digitalWrite(BUTTON_LED, HIGH);
    lastTriviaBlinkToggle = millis();
    render();
    playLightCue();
  }
}

// ---------------- Trivia: blinks the LED during the 5s answer window, bips once per second,
// then plays a distinct descending sound and resets when the window ends ----------------
void updateTrivia() {
  // keep long questions/answers scrolling - re-render only when the scroll step actually changes,
  // and only while something on screen is too long to fit
  if (triviaScreenState == TRIVIA_PLAYING && inGameScreen() && !screenAsleep && triviaContentOverflows()) {
    long frame = (long)((millis() - triviaViewShownAt) / TRIVIA_SCROLL_CHAR_MS);
    if (frame != triviaLastScrollFrame) {
      triviaLastScrollFrame = frame;
      render();
    }
  }

  if (triviaScreenState != TRIVIA_PLAYING || !triviaAnswering) return;

  unsigned long elapsed = millis() - triviaAnswerRevealTime;
  if (elapsed >= TRIVIA_ANSWER_WINDOW_MS) {
    triviaAnswering = false;
    triviaAnswerVisible = false;
    digitalWrite(BUTTON_LED, HIGH); // solid on again = ready for the next buzz-in
    render();
    // distinct descending 3-note "turn over" cue - clearly different in character from the
    // neutral per-second bips, so it's unmistakable when the window actually ends
    playNote(1000, 80);
    playNote(700, 80);
    playNote(400, 200);
    return;
  }

  int secondsElapsed = (int)(elapsed / 1000);
  if (secondsElapsed > triviaLastBipSecond && secondsElapsed < 5) {
    triviaLastBipSecond = secondsElapsed;
    playNote(800, 40); // once-per-second bip while the window is running
  }

  const unsigned long blinkInterval = 150;
  if (millis() - lastTriviaBlinkToggle >= blinkInterval) {
    lastTriviaBlinkToggle = millis();
    triviaBlinkState = !triviaBlinkState;
    digitalWrite(BUTTON_LED, triviaBlinkState ? HIGH : LOW);
  }
}

// ---------------- Selection screens: confirms whichever overlay is active (language, timer step, settings, menu).
// Returns true if one was handled, false if none of these were active. ----------------
bool tryConfirmSelectionScreens() {
  if (inLeaveConfirm) {
    confirmLeaveConfirmSelection();
    return true;
  }
  if (inInstructions) {
    closeInstructions();
    return true;
  }
  if (inLanguageSelect) {
    confirmLanguageSelection();
    return true;
  }
  if (inTimerStepSelect) {
    confirmTimerStepSelection();
    return true;
  }
  if (inSettingsMenu) {
    confirmSettingsSelection();
    return true;
  }
  if (inMenu) {
    confirmMenuSelection();
    return true;
  }
  return false;
}

// ---------------- True when a game's own screen is showing - not the menu, Settings, a picker,
// Instructions, or the leave-confirm dialog drawn over the game ----------------
bool inGameScreen() {
  return !inMenu && !inLeaveConfirm && !inInstructions && !inSettingsMenu && !inLanguageSelect && !inTimerStepSelect;
}

// ---------------- Leaves whatever's currently happening and jumps to the main game list, pausing
// or resetting anything in progress so nothing is lost or left in a stuck state. Only called after
// the user has confirmed via the "Go back to menu?" dialog (see openLeaveConfirm/confirmLeaveConfirmSelection). ----------------
void leaveToMenu() {
  inMenu = true;
  inSettingsMenu = false;
  inLanguageSelect = false;
  inTimerStepSelect = false;
  inInstructions = false;
  menuSelection = menuPositionForGame(currentGame);
  running = false;              // pause the timer if it was running
  reflexState = REFLEX_READY;   // reset any armed reflex round
  chessPaused = true;           // freeze the chess clock rather than losing track of it
  hpState = HP_READY;           // defuse any armed bomb
  if (stopwatchState == STOPWATCH_RUNNING) {
    stopwatchElapsedMs += millis() - stopwatchStartMillis;
    stopwatchState = STOPWATCH_STOPPED; // pause rather than lose the elapsed time
  }
  triviaAnswering = false;      // cancel any active answer window
  triviaAnswerVisible = false;
  toothbrushScreenState = TOOTHBRUSH_SELECT_TIME; // stop a running brush timer rather than leaving it "running" forever
  toothbrushPaused = false;
  tetrisState = TETRIS_READY;       // these don't have partial progress worth preserving like
  stardropState = STARDROP_READY;   // Chess/Stopwatch do - leaving mid-round just ends the round,
  snakeState = SNAKE_READY;         // same as a game over would
  simonState = SIMON_READY;
  stopTone(); // in case the arcade button was mid-hold (live dot/dash tone) when this happened
  digitalWrite(BUTTON_LED, LOW);
  render();
}

// ---------------- Opens the "Go back to menu?" dialog - the long press's action from inside a
// game, before actually leaving it via leaveToMenu() ----------------
void openLeaveConfirm() {
  inLeaveConfirm = true;
  leaveConfirmSelection = 0; // Yes, default
  render();
}

// ---------------- Confirms whichever of Yes/No is currently highlighted in the leave-confirm dialog ----------------
void confirmLeaveConfirmSelection() {
  inLeaveConfirm = false;
  if (leaveConfirmSelection == 0) {
    leaveToMenu();
  } else {
    render(); // No - just redraw the game screen underneath, nothing changes
  }
}

// ---------------- Encoder long press (1s): asks "Go back to menu?" from inside a game; from the
// Settings list specifically, jumps straight back to the main menu with no confirmation, since
// there's no game progress to lose there; does nothing from anywhere else in the menu hierarchy
// (Language/Timer Step/Instructions already have their own short-press way back out), or if the
// dialog is already open ----------------
void toggleMenuFromLongPress() {
  if (inSettingsMenu) {
    inSettingsMenu = false;
    inMenu = true;
    menuSelection = menuPositionForGame(MENU_SETTINGS);
    render();
    return;
  }

  bool inMenuHierarchy = inMenu || inLanguageSelect || inTimerStepSelect || inInstructions;
  if (inMenuHierarchy || inLeaveConfirm) {
    return; // already in the menu (or already being asked) - long press does nothing here
  }
  openLeaveConfirm();
}

// ---------------- Button: dispatches a short press depending on the current screen ----------------
void dispatchButtonShortPress() {
  if (tryConfirmSelectionScreens()) return;

  if (currentGame == GAME_TIMER) {
    remainingTime = presetTime;
    runStartTime = presetTime;
    running = true;
    timerPaused = false;
    midpointPlayed = false;
    ledcWrite(BUZZER_PIN, 0);
    render();
    playStartMelody();
    lastSecondTick = millis(); // start the 1s ticks right after the jingle finishes
  } else if (currentGame == GAME_REFLEX) {
    handleReflexButtonPress();
  } else if (currentGame == GAME_DICE) {
    if (diceScreenState == DICE_SELECT_TYPE) {
      confirmDiceType();
    } else {
      doDiceRoll();
    }
  } else if (currentGame == GAME_CHESS) {
    if (chessScreenState == CHESS_SETUP_TIME) {
      confirmChessStartTime();
    } else if (chessScreenState == CHESS_SETUP_INCREMENT) {
      confirmChessIncrement();
    } else {
      doChessPass();
    }
  } else if (currentGame == GAME_HOTPOTATO) {
    handleHotPotatoPress();
  } else if (currentGame == GAME_TURNORDER) {
    doTurnOrderSpin();
  } else if (currentGame == GAME_STOPWATCH) {
    handleStopwatchPress();
  } else if (currentGame == GAME_TRIVIA) {
    handleTriviaButtonPress();
  } else if (currentGame == GAME_TOOTHBRUSH) {
    handleToothbrushPress();
  } else if (currentGame == GAME_TETRIS) {
    handleTetrisButtonPress();
  } else if (currentGame == GAME_STARDROP) {
    handleStarDropButtonPress();
  } else if (currentGame == GAME_SNAKE) {
    handleSnakeButtonPress();
  } else { // GAME_SIMPLEMORSE
    handleSimonButtonPress();
  }
}

// ---------------- Button: start/restart the timer, select/confirm, or interact with the reflex game.
// A short press dispatches its action on release - no long-press behavior anymore (that's now the
// encoder click's job, see readEncoderButton). ----------------
void readButton() {
  int raw = digitalRead(BUTTON_PIN);

  // raw flickering vs. the previous pass resets the debounce timer
  if (raw != lastButtonFlicker) {
    lastButtonEdgeTime = millis();
  }

  // only accept the new state once it's been stable for BUTTON_DEBOUNCE_MS
  if ((millis() - lastButtonEdgeTime) > BUTTON_DEBOUNCE_MS) {
    if (raw != buttonState) {
      buttonState = raw;

      if (buttonState == LOW) {
        bool wokeScreen = registerActivity();
        buttonPressStart = millis(); // also used by the Reflex game to measure reaction time
        buttonPressConsumed = wokeScreen; // a press that only woke the screen does nothing else
        if (wokeScreen) {
          // nothing else - see registerActivity()
        } else if (currentGame == GAME_REFLEX && reflexState == REFLEX_WAITING && inGameScreen()) {
          // A false start has to be judged the instant the button goes DOWN. Every other press
          // dispatches on release, and if this one did too, holding the button through the
          // light coming on would land in REFLEX_LIT and score a negative "reaction time".
          buttonPressConsumed = true;
          handleReflexButtonPress();
        } else if (currentGame == GAME_SIMPLEMORSE && simonState == SIMON_INPUT) {
          startTone(1000); // one constant pitch for the whole hold - duration is what tells dot from dash, not a pitch change
          digitalWrite(BUTTON_LED, HIGH);
        }
      } else {
        // stopTone() is unconditional, unlike the start above: if simonState changed for any
        // reason between this press and this release (it legitimately can - see the note on
        // handleSimonButtonPress), a symmetric "only if still SIMON_INPUT" check here would skip
        // stopping a tone that's actually still playing. stopTone() is harmless to call when
        // nothing's playing, so make the stop safe unconditionally rather than trying to keep two
        // separate checks in sync. The LED write stays scoped to SimpleMorse though, since other
        // games manage the LED's state themselves and an unconditional LOW here would fight them.
        stopTone();
        if (currentGame == GAME_SIMPLEMORSE) {
          digitalWrite(BUTTON_LED, LOW);
        }
        if (!buttonPressConsumed) dispatchButtonShortPress();
        buttonPressConsumed = false;
      }
    }
  }

  lastButtonFlicker = raw;
}

// ---------------- Encoder push-button: a short click confirms/selects (same as the arcade button)
// or does a per-game context action (pause/lap); holding it 1s is the only way to open the menu.
// Hold+turn is claimed for volume in readEncoder() and disqualifies both of those for this hold. ----------------
void readEncoderButton() {
  int raw = digitalRead(ENCODER_SW);

  if (raw != lastSwFlicker) {
    lastSwEdgeTime = millis();
  }

  if ((millis() - lastSwEdgeTime) > SW_DEBOUNCE_MS) {
    if (raw != swState) {
      swState = raw;

      if (swState == LOW) {
        // just confirmed pressed - start timing for a possible long press
        swWakeConsumed = registerActivity(); // a hold that woke the screen fires neither click nor long-press
        swClaimedByVolume = false;
        swPressStart = millis();
        swLongFired = false;
      } else {
        // just confirmed released - only a click action if this hold wasn't already used for
        // volume, and didn't already fire the long-press menu toggle
        if (swClaimedByVolume) {
          render(); // clear the full-screen volume overlay, back to the normal screen
        } else if (!swLongFired && !swWakeConsumed) {
          if (!tryConfirmSelectionScreens()) {
            if (currentGame == GAME_DICE && diceScreenState == DICE_SELECT_TYPE) {
              confirmDiceType();
            } else if (currentGame == GAME_CHESS && chessScreenState == CHESS_PLAYING) {
              toggleChessPause();
            } else if (currentGame == GAME_TIMER && running) {
              toggleTimerPause();
            } else if (currentGame == GAME_STOPWATCH && stopwatchState == STOPWATCH_RUNNING) {
              recordStopwatchLap();
            } else if (currentGame == GAME_TETRIS && tetrisState == TETRIS_PLAYING) {
              tetrisHardDrop();
            } else if (currentGame == GAME_TETRIS && (tetrisState == TETRIS_READY || tetrisState == TETRIS_GAME_OVER)) {
              handleTetrisButtonPress(); // start/restart, same as the arcade button
            } else if (currentGame == GAME_STARDROP && (stardropState == STARDROP_READY || stardropState == STARDROP_GAME_OVER)) {
              handleStarDropButtonPress(); // start/restart, same as the arcade button
            } else if (currentGame == GAME_SNAKE) {
              handleSnakeButtonPress(); // start/restart/pause/resume, same as the arcade button
            }
            // otherwise: no assigned action on this screen, click does nothing
          }
          render();
        }
        swClaimedByVolume = false;
        swLongFired = false;
        swWakeConsumed = false;
      }
    }
  }

  // long-press: checked continuously while held, not just on release - and only if this hold
  // hasn't already been claimed for a volume turn (or used to wake the screen)
  if (swState == LOW && !swClaimedByVolume && !swLongFired && !swWakeConsumed && (millis() - swPressStart >= ENCODER_LONG_PRESS_MS)) {
    swLongFired = true;
    toggleMenuFromLongPress();
  }

  lastSwFlicker = raw;
}

// ---------------- Buzzer: play one note at the current volume level ----------------
// Blocks for durationMs (needed to control duty cycle directly for volume, unlike tone()).
void playNote(int freq, int durationMs) {
  if (volumeLevel == 0) return; // muted: skip entirely, no delay
  int duty = (volumeLevel == 1) ? VOLUME_LOW_DUTY : VOLUME_HIGH_DUTY;
  ledcWriteTone(BUZZER_PIN, freq);
  ledcWrite(BUZZER_PIN, duty);
  delay(durationMs);
  ledcWrite(BUZZER_PIN, 0); // silence, but stay attached to the pin
}

// ---------------- Non-blocking tone start/stop - unlike playNote() above, these don't delay(),
// so the tone can persist across many loop() passes (e.g. for as long as a button stays held)
// instead of being fixed to one duration. Used by SimpleMorse's live press feedback. ----------------
void startTone(int freq) {
  if (volumeLevel == 0) return;
  int duty = (volumeLevel == 1) ? VOLUME_LOW_DUTY : VOLUME_HIGH_DUTY;
  ledcWriteTone(BUZZER_PIN, freq);
  ledcWrite(BUZZER_PIN, duty);
}

void stopTone() {
  ledcWrite(BUZZER_PIN, 0);
}

// ---------------- Buzzer: short ascending jingle when the timer ends ----------------
void playEndMelody() {
  for (int i = 0; i < END_MELODY_LENGTH; i++) {
    playNote(END_MELODY_NOTES[i], END_MELODY_DURATIONS[i]);
    delay(30); // brief gap between notes; blocks ~600ms total, only happens once per run
  }
}

// ---------------- Buzzer: short "go" jingle when the countdown starts ----------------
void playStartMelody() {
  for (int i = 0; i < START_MELODY_LENGTH; i++) {
    playNote(START_MELODY_NOTES[i], START_MELODY_DURATIONS[i]);
    delay(20); // blocks briefly (~350ms), only happens once per run
  }
}

// ---------------- Buzzer: short two-tone chime at the halfway point ----------------
void playMidMelody() {
  for (int i = 0; i < MID_MELODY_LENGTH; i++) {
    playNote(MID_MELODY_NOTES[i], MID_MELODY_DURATIONS[i]);
    delay(20); // blocks briefly (~260ms), only happens once per run
  }
}

// ---------------- Buzzer: short jingle when a reflex round is armed (button hit to start) ----------------
void playArmSound() {
  for (int i = 0; i < REFLEX_ARM_LENGTH; i++) {
    playNote(REFLEX_ARM_NOTES[i], REFLEX_ARM_DURATIONS[i]);
    delay(15);
  }
}

// ---------------- Buzzer: single sharp cue right when the light turns on ----------------
void playLightCue() {
  playNote(REFLEX_LIGHT_FREQ, REFLEX_LIGHT_DURATION_MS);
}

// ---------------- Buzzer: short descending sound when you don't beat the high score ----------------
void playLoseSound() {
  for (int i = 0; i < REFLEX_LOSE_LENGTH; i++) {
    playNote(REFLEX_LOSE_NOTES[i], REFLEX_LOSE_DURATIONS[i]);
    delay(20);
  }
}

// ---------------- Buzzer: short ascending fanfare when you beat the high score ----------------
void playWinSound() {
  for (int i = 0; i < REFLEX_WIN_LENGTH; i++) {
    playNote(REFLEX_WIN_NOTES[i], REFLEX_WIN_DURATIONS[i]);
    delay(25);
  }
}

void updateCountdown() {
  if (!running || timerPaused) return;

  if (millis() - lastSecondTick >= 1000) {
    lastSecondTick += 1000;
    remainingTime--;

    // halfway chime, fires once per run
    if (!midpointPlayed && remainingTime > 0 && remainingTime <= runStartTime / 2) {
      midpointPlayed = true;
      playMidMelody();
    }

    // short beep each second for the last 5 seconds (briefly blocking - needed for volume control)
    if (remainingTime > 0 && remainingTime <= 5) {
      playNote(1500, 150);
    }

    if (remainingTime <= 0) {
      remainingTime = 0;
      running = false;
      registerActivity(); // the countdown finishing on its own counts as activity - don't let sleep
                           // have already been "due" the instant it ends just because it ran a while
      digitalWrite(BUTTON_LED, HIGH); // solid on = time's up
      playEndMelody();
    }

    render();
  }
}

// ---------------- Reflex game: checks if it's time to turn the light on ----------------
void updateReflexGame() {
  if (reflexState == REFLEX_WAITING && millis() - reflexArmTime >= reflexDelay) {
    reflexLightOnTime = millis();
    digitalWrite(BUTTON_LED, HIGH);
    reflexState = REFLEX_LIT;
    render();
    playLightCue();
  }
}

// ---------------- Chess Timer: ticks the active player's clock once per second while playing and unpaused ----------------
void updateChessGame() {
  if (chessScreenState != CHESS_PLAYING || chessPaused || !chessStarted) return;

  if (millis() - chessLastTick >= 1000) {
    chessLastTick += 1000;

    if (chessActivePlayer == 1) chessTimeP1--; else chessTimeP2--;
    long activeRemaining = (chessActivePlayer == 1) ? chessTimeP1 : chessTimeP2;

    // short beep each second for the last 5 seconds of the active player's clock
    if (activeRemaining > 0 && activeRemaining <= 5) {
      playNote(1500, 150);
    }

    if (activeRemaining <= 0) {
      if (chessActivePlayer == 1) chessTimeP1 = 0; else chessTimeP2 = 0;
      chessLoser = chessActivePlayer;
      chessScreenState = CHESS_GAME_OVER;
      digitalWrite(BUTTON_LED, HIGH); // solid on = game over
      playEndMelody();
    }

    render();
  }
}

// ---------------- Reflex game: bomb animation for a false start - lit fuse, then it detonates ----------------
void playBombAnimation() {
  int cx = SCREEN_WIDTH / 2;
  int cy = 36;

  // Phase 1: round bomb body with a curved fuse and a blinking spark, sizzling
  const int FUSE_FRAMES = 4;
  for (int f = 0; f < FUSE_FRAMES; f++) {
    display.clearDisplay();
    display.setTextColor(SH110X_WHITE);

    display.fillCircle(cx, cy + 4, 14, SH110X_WHITE); // bomb body

    // fuse: two segments curving up and to the right from the top of the body
    display.drawLine(cx - 2, cy - 10, cx + 6, cy - 18, SH110X_WHITE);
    display.drawLine(cx + 6, cy - 18, cx + 13, cy - 14, SH110X_WHITE);

    if (f % 2 == 0) { // blinking spark at the fuse tip
      int sx = cx + 13, sy = cy - 14;
      display.drawLine(sx - 3, sy, sx + 3, sy, SH110X_WHITE);
      display.drawLine(sx, sy - 3, sx, sy + 3, SH110X_WHITE);
      display.drawLine(sx - 2, sy - 2, sx + 2, sy + 2, SH110X_WHITE);
      display.drawLine(sx - 2, sy + 2, sx + 2, sy - 2, SH110X_WHITE);
    }

    display.display();
    playNote(1400 + random(0, 300), 40); // fizzling spark
    delay(140);
  }

  // Phase 2: detonation - growing blast with radiating spikes, descending boom pitch
  const int BLAST_FRAMES = 5;
  for (int f = 0; f < BLAST_FRAMES; f++) {
    display.clearDisplay();
    display.setTextColor(SH110X_WHITE);

    int r = 6 + f * 7;
    display.fillCircle(cx, cy, r, SH110X_WHITE);

    for (int a = 0; a < 10; a++) {
      float angle = a * (360.0 / 10) * PI / 180.0;
      int x2 = cx + (int)(cos(angle) * (r + 12));
      int y2 = cy + (int)(sin(angle) * (r + 12));
      display.drawLine(cx, cy, x2, y2, SH110X_WHITE);
    }

    display.display();
    playNote(200 - f * 20, 60); // pitch drops each frame, deepening boom
    delay(70);
  }

  // Phase 3: bright flash, then a low final thud as it clears
  display.fillRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SH110X_WHITE);
  display.display();
  playNote(80, 180);
  delay(150);
  display.clearDisplay();
  display.display();
}

// ---------------- Reflex game: one radiating spark burst, used by the firework animation ----------------
void drawBurst(int cx, int cy, int r) {
  const int SPIKES = 8;
  for (int a = 0; a < SPIKES; a++) {
    float angle = a * (360.0 / SPIKES) * PI / 180.0;
    int x2 = cx + (int)(cos(angle) * r);
    int y2 = cy + (int)(sin(angle) * r);
    display.drawLine(cx, cy, x2, y2, SH110X_WHITE);
  }
}

// ---------------- Reflex game: firework animation for beating the high score ----------------
void playFireworkAnimation() {
  const int burstX[] = {30, 64, 95};
  const int burstY[] = {22, 30, 22};

  for (int p = 0; p < 3; p++) {
    for (int r = 3; r <= 15; r += 4) {
      display.clearDisplay();
      display.setTextColor(SH110X_WHITE);
      drawBurst(burstX[p], burstY[p], r);
      display.display();
      playNote(1000 + p * 150 + r * 10, 30);
      delay(50);
    }
  }
  delay(150);
}

// ---------------- Splash screen: shown once at power-on, before the normal menu takes over.
// "TIGER" slides in from the left, then once it's in place "TIMER" slides in from the right -
// 1 second total for both slides combined (500ms each), with the tiger's mouth-cavity blinking
// continuously throughout via the same cover-and-uncover technique as before (no second bitmap
// frame needed). Once both words are in their final position, the blinking stops - the whole
// scene (tiger, both words, mouth uncovered) holds static for another 0.5 seconds. ----------------
// ---------------- Splash: true if the arcade button is currently pressed - and if so, blocks
// until it's released (plus a short settle delay) before returning. That wait matters: without
// it, a press held through the splash is still down (or its release hasn't settled) the instant
// loop() starts, and readButton()'s debounce logic - which assumes the button starts unpressed -
// reads that as a brand-new press-then-release, auto-confirming whatever's highlighted in the
// menu instead of actually landing on the configured startup screen. ----------------
bool splashSkipRequested() {
  if (digitalRead(BUTTON_PIN) != LOW) return false;
  while (digitalRead(BUTTON_PIN) == LOW) delay(2);
  delay(30);
  return true;
}

void playSplashScreen() {
  int tigerX = (SCREEN_WIDTH - SPLASH_TIGER_WIDTH) / 2;
  int tigerY = 0;

  // the mouth-cavity oval's bounding box within the artwork, used to blink it on/off in place -
  // covering it with black "hides" it, letting the bitmap show through again "reveals" it
  int mouthX = tigerX + 24;
  int mouthY = tigerY + 35;
  int mouthW = 31;
  int mouthH = 10;

  const int wordGap = 4;
  int combinedWidth = SPLASH_WORD_WIDTH * 2 + wordGap;
  int tigerWordFinalX = (SCREEN_WIDTH - combinedWidth) / 2;
  int timerWordFinalX = tigerWordFinalX + SPLASH_WORD_WIDTH + wordGap;
  int wordY = 55;

  int tigerStartX = -SPLASH_WORD_WIDTH - 5; // fully off-screen left
  int timerStartX = SCREEN_WIDTH + 5;       // fully off-screen right

  const int FRAME_MS = 25;
  const int PHASE_FRAMES = 20; // 20 x 25ms = 500ms per phase, 1000ms combined
  const int BLINK_EVERY_N_FRAMES = 2; // ~50ms toggle - a rapid blink

  int frameCounter = 0;
  bool mouthVisible = true;

  // Phase 1: TIGER slides in from the left; TIMER isn't shown yet at all
  for (int f = 0; f < PHASE_FRAMES; f++) {
    if (splashSkipRequested()) return; // button held/pressed - skip straight past the whole animation

    float progress = (float)f / (PHASE_FRAMES - 1);
    int curTigerX = tigerStartX + (int)(progress * (tigerWordFinalX - tigerStartX));

    display.clearDisplay();
    display.drawBitmap(tigerX, tigerY, TIGER_BITMAP, SPLASH_TIGER_WIDTH, SPLASH_TIGER_HEIGHT, SH110X_WHITE);
    if (!mouthVisible) {
      display.fillRoundRect(mouthX, mouthY, mouthW, mouthH, 4, SH110X_BLACK);
    }
    display.drawBitmap(curTigerX, wordY, WORD_TIGER_BITMAP, SPLASH_WORD_WIDTH, SPLASH_WORD_HEIGHT, SH110X_WHITE);
    display.display();
    delay(FRAME_MS);

    frameCounter++;
    if (frameCounter % BLINK_EVERY_N_FRAMES == 0) mouthVisible = !mouthVisible;
  }

  // Phase 2: TIGER fixed in its final spot; TIMER slides in from the right
  for (int f = 0; f < PHASE_FRAMES; f++) {
    if (splashSkipRequested()) return; // skip here too

    float progress = (float)f / (PHASE_FRAMES - 1);
    int curTimerX = timerStartX + (int)(progress * (timerWordFinalX - timerStartX));

    display.clearDisplay();
    display.drawBitmap(tigerX, tigerY, TIGER_BITMAP, SPLASH_TIGER_WIDTH, SPLASH_TIGER_HEIGHT, SH110X_WHITE);
    if (!mouthVisible) {
      display.fillRoundRect(mouthX, mouthY, mouthW, mouthH, 4, SH110X_BLACK);
    }
    display.drawBitmap(tigerWordFinalX, wordY, WORD_TIGER_BITMAP, SPLASH_WORD_WIDTH, SPLASH_WORD_HEIGHT, SH110X_WHITE);
    display.drawBitmap(curTimerX, wordY, WORD_TIMER_BITMAP, SPLASH_WORD_WIDTH, SPLASH_WORD_HEIGHT, SH110X_WHITE);
    display.display();
    delay(FRAME_MS);

    frameCounter++;
    if (frameCounter % BLINK_EVERY_N_FRAMES == 0) mouthVisible = !mouthVisible;
  }

  // Hold: everything in its final position, mouth uncovered and no longer blinking. The
  // achievement chime plays right here - the instant the logo (both words) is fully in place -
  // then the scene holds for the rest of the 0.5s after the sound finishes.
  display.clearDisplay();
  display.drawBitmap(tigerX, tigerY, TIGER_BITMAP, SPLASH_TIGER_WIDTH, SPLASH_TIGER_HEIGHT, SH110X_WHITE);
  display.drawBitmap(tigerWordFinalX, wordY, WORD_TIGER_BITMAP, SPLASH_WORD_WIDTH, SPLASH_WORD_HEIGHT, SH110X_WHITE);
  display.drawBitmap(timerWordFinalX, wordY, WORD_TIMER_BITMAP, SPLASH_WORD_WIDTH, SPLASH_WORD_HEIGHT, SH110X_WHITE);
  display.display();
  playNote(784, 70);
  playNote(1047, 70);
  playNote(1319, 220);
  delay(500);
}

// ---------------- Reflex game: handles a button press depending on the current round state ----------------
void handleReflexButtonPress() {
  // Defensive: a press that started before the light came on is a false start, whatever state
  // we're in by the time it's processed (readButton() already catches this on the press itself).
  if (reflexState == REFLEX_LIT && (long)(buttonPressStart - reflexLightOnTime) < 0) {
    reflexState = REFLEX_WAITING;
  }
  if (reflexState == REFLEX_WAITING) {
    // pressed before the light came on
    digitalWrite(BUTTON_LED, LOW);
    reflexState = REFLEX_READY;
    playBombAnimation();
    falseStartUntil = millis() + FALSE_START_DISPLAY_MS;
    render();
  } else if (reflexState == REFLEX_LIT) {
    // use the moment the button was actually pressed, not "now" - dispatch is deferred to
    // release (to allow long-press detection), but the press instant is what's fair to measure
    long reaction = (long)(buttonPressStart - reflexLightOnTime);
    lastReactionMs = reaction;
    bool isNewRecord = (reflexHighScoreMs < 0 || reaction < reflexHighScoreMs);
    if (isNewRecord) {
      reflexHighScoreMs = reaction;
    }
    digitalWrite(BUTTON_LED, LOW);
    reflexState = REFLEX_READY;
    if (isNewRecord) {
      playFireworkAnimation();
      render();
      playWinSound();
    } else {
      render();
      playLoseSound();
    }
  } else { // REFLEX_READY -> arm a new round
    reflexState = REFLEX_WAITING;
    reflexArmTime = millis();
    reflexDelay = random(REFLEX_MIN_DELAY_MS, REFLEX_MAX_DELAY_MS);
    falseStartUntil = 0; // clear any previous false-start message
    digitalWrite(BUTTON_LED, LOW);
    render();
    playArmSound();
  }
}

// ---------------- Dice Roll: how large each die face should be, sized to fill the screen width ----------------
int diceFaceSize(int count) {
  const int availableWidth = 120; // leaves a small margin on each side of the 128px screen
  const int gap = 4;
  int faceSizeByWidth = (availableWidth - (count - 1) * gap) / count;
  int faceSize = min(faceSizeByWidth, 38); // capped so the total line always has room below
  if (faceSize < 12) faceSize = 12;        // safety floor, shouldn't trigger with count <= 5
  return faceSize;
}

// ---------------- Dice Roll: vertical position for the row of dice, leaving room for the total line if needed ----------------
int diceYStart(int count, int faceSize) {
  int bandTop = 16;
  int bandBottom = (count > 1) ? 52 : 60; // reserve space below for "Total: N" when rolling more than one
  int bandHeight = bandBottom - bandTop;
  int y = bandTop + (bandHeight - faceSize) / 2;
  if (y < bandTop) y = bandTop;
  return y;
}

// ---------------- Dice Roll: animates all selected dice tumbling together, then settles on results ----------------
void doDiceRoll() {
  int sides = DICE_SIDES[currentDieIndex];
  int count = diceCount;
  const int FRAMES = 14;

  int faceSize = diceFaceSize(count);
  int gap = 4;
  int totalWidth = count * faceSize + (count - 1) * gap;
  int xStart = (SCREEN_WIDTH - totalWidth) / 2;
  int yStart = diceYStart(count, faceSize);

  int frameFaces[MAX_DICE_COUNT];

  for (int f = 0; f < FRAMES; f++) {
    display.clearDisplay();
    display.setTextColor(SH110X_WHITE);
    drawDiceIcon(7, 7, 6, SH110X_WHITE);

    display.setTextSize(1);
    const char* status = T(STR_ROLLING);
    int16_t sx1, sy1;
    uint16_t sw, sh;
    display.getTextBounds(status, 0, 0, &sx1, &sy1, &sw, &sh);
    display.setCursor(SCREEN_WIDTH - sw - 2, 4);
    display.print(status);
    display.drawLine(0, 14, SCREEN_WIDTH, 14, SH110X_WHITE);

    for (int i = 0; i < count; i++) {
      frameFaces[i] = random(1, sides + 1);
      drawDieFace(xStart + i * (faceSize + gap), yStart, faceSize, sides, frameFaces[i], SH110X_WHITE);
    }

    display.display();
    digitalWrite(BUTTON_LED, f % 2 == 0 ? HIGH : LOW); // flicker in sync with each frame, slows down with the roll
    playNote(700 + random(0, 300), 20);
    delay(30 + f * 10); // increasing delay = slowing down like a real roll
  }

  digitalWrite(BUTTON_LED, LOW);

  for (int i = 0; i < count; i++) {
    diceResults[i] = random(1, sides + 1);
  }
  diceResultSides = sides;
  diceResultCount = count;

  render();
  playNote(1300, 100);
}

// ---------------- LED: blinks faster as remainingTime approaches 0 ----------------
void updateLedBlink() {
  if (!running || timerPaused) return; // solid on (timeout), off (idle), or off (paused) handled elsewhere

  float fraction = (runStartTime > 0) ? (float)remainingTime / (float)runStartTime : 0;
  fraction = constrain(fraction, 0.0, 1.0);

  // 800ms interval when just started, down to 100ms right before time's up
  unsigned long blinkInterval = 100 + (unsigned long)(fraction * 700);

  if (millis() - lastBlinkToggle >= blinkInterval) {
    lastBlinkToggle = millis();
    ledBlinkState = !ledBlinkState;
    digitalWrite(BUTTON_LED, ledBlinkState ? HIGH : LOW);
  }
}

// ---------------- Display: dispatches to whichever screen is currently active ----------------
void render() {
  if (swClaimedByVolume && swState == LOW) {
    // volume is actively being adjusted (encoder still held) - keep the full-screen overlay up
    // no matter who called render(); this is what stops a periodic per-game update (e.g. the
    // toothbrush timer's running-state redraw) from flickering the normal screen back over it
    // mid-adjustment. The moment the encoder is released, swState is no longer LOW here, so this
    // falls through to the normal dispatch below and correctly restores the underlying screen.
    drawVolumeOverlay();
    return;
  }

  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);

  if (inLeaveConfirm) {
    drawLeaveConfirmScreen();
  } else if (inInstructions) {
    drawInstructionsScreen();
  } else if (inLanguageSelect) {
    drawLanguageScreen();
  } else if (inTimerStepSelect) {
    drawTimerStepScreen();
  } else if (inSettingsMenu) {
    drawSettingsScreen();
  } else if (inMenu) {
    drawMenuScreen();
  } else if (currentGame == GAME_TIMER) {
    drawTimerScreen();
  } else if (currentGame == GAME_REFLEX) {
    drawReflexScreen();
  } else if (currentGame == GAME_DICE) {
    if (diceScreenState == DICE_SELECT_TYPE) {
      drawDiceSelectScreen();
    } else {
      drawDiceScreen();
    }
  } else if (currentGame == GAME_CHESS) {
    if (chessScreenState == CHESS_SETUP_TIME) {
      drawChessSetupTimeScreen();
    } else if (chessScreenState == CHESS_SETUP_INCREMENT) {
      drawChessSetupIncrementScreen();
    } else {
      drawChessPlayScreen();
    }
  } else if (currentGame == GAME_HOTPOTATO) {
    drawHotPotatoScreen();
  } else if (currentGame == GAME_TURNORDER) {
    drawTurnOrderScreen();
  } else if (currentGame == GAME_STOPWATCH) {
    drawStopwatchScreen();
  } else if (currentGame == GAME_TRIVIA) {
    if (triviaScreenState == TRIVIA_SELECT_CATEGORY) {
      drawTriviaCategoryScreen();
    } else if (triviaScreenState == TRIVIA_SELECT_LEVEL) {
      drawTriviaLevelScreen();
    } else {
      drawTriviaPlayScreen();
    }
  } else if (currentGame == GAME_TOOTHBRUSH) {
    if (toothbrushScreenState == TOOTHBRUSH_SELECT_TIME) {
      drawToothbrushSelectScreen();
    } else {
      drawToothbrushRunningScreen();
    }
  } else if (currentGame == GAME_TETRIS) {
    if (tetrisState == TETRIS_READY) {
      drawTetrisReadyScreen();
    } else if (tetrisState == TETRIS_GAME_OVER) {
      drawTetrisGameOverScreen();
    } else {
      drawTetrisPlayScreen();
    }
  } else if (currentGame == GAME_STARDROP) {
    if (stardropState == STARDROP_READY) {
      drawStarDropReadyScreen();
    } else if (stardropState == STARDROP_GAME_OVER) {
      drawStarDropGameOverScreen();
    } else {
      drawStarDropPlayScreen();
    }
  } else if (currentGame == GAME_SNAKE) {
    drawSnakeScreen();
  } else { // GAME_SIMPLEMORSE
    drawSimonScreen();
  }

  display.display();
}

// ---------------- Display: draws text centered horizontally, shrinking the size if it wouldn't fit ----------------
// Needed because translated strings vary a lot in length - a word that fits at size 4 in
// English might not fit at all in French or Spanish.
void drawCenteredAutoFit(const char* text, int y, int maxSize) {
  int size = maxSize;
  int16_t x1, y1;
  uint16_t w, h;
  while (size > 1) {
    display.setTextSize(size);
    display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
    if (w <= SCREEN_WIDTH - 16) break;
    size--;
  }
  display.setTextSize(size);
  display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, y);
  display.print(text);
}

// ---------------- Display: word-wraps text across up to maxLines centered lines - used for the
// trivia question, which can't always fit on one line even when kept short. Returns how many
// lines it actually drew, so a caller can position anything below dynamically. ----------------
// skipLines: wrapped lines to skip before drawing (used to scroll a long trivia question).
int drawWrappedText(const char* text, int yStart, int maxLines, int lineHeight, int skipLines) {
  display.setTextSize(1);
  int len = strlen(text);
  const int charsPerLine = 21; // ~128px / 6px per character at size 1
  int pos = 0;
  int lineNum = 0; // counts lines actually drawn
  int skipped = 0;

  while (pos < len && lineNum < maxLines) {
    int remaining = len - pos;
    int take = min(charsPerLine, remaining);
    if (take < remaining) {
      int breakAt = take;
      while (breakAt > 0 && text[pos + breakAt] != ' ') breakAt--;
      if (breakAt > 0) take = breakAt;
    }

    if (skipped < skipLines) {
      skipped++;
    } else {
      char lineBuf[24];
      int copyLen = min(take, (int)sizeof(lineBuf) - 1);
      strncpy(lineBuf, text + pos, copyLen);
      lineBuf[copyLen] = '\0';

      int16_t x1, y1;
      uint16_t w, h;
      display.getTextBounds(lineBuf, 0, 0, &x1, &y1, &w, &h);
      display.setCursor((SCREEN_WIDTH - w) / 2, yStart + lineNum * lineHeight);
      display.print(lineBuf);
      lineNum++;
    }

    pos += take;
    while (pos < len && text[pos] == ' ') pos++; // skip the space the line broke on
  }
  return lineNum;
}

// ---------------- How many lines drawWrappedText's word-wrap would produce for this text, without
// drawing anything - same break rules, so the two always agree ----------------
int wrappedLineCount(const char* text, int charsPerLine) {
  int len = strlen(text);
  int pos = 0;
  int lines = 0;
  while (pos < len) {
    int remaining = len - pos;
    int take = min(charsPerLine, remaining);
    if (take < remaining) {
      int breakAt = take;
      while (breakAt > 0 && text[pos + breakAt] != ' ') breakAt--;
      if (breakAt > 0) take = breakAt;
    }
    pos += take;
    while (pos < len && text[pos] == ' ') pos++;
    lines++;
  }
  return lines;
}

// ---------------- Trivia scrolling: current offset (0..overflow) for something that's `overflow`
// steps too long - holds at 0, steps once per stepMs, holds at the end, then loops ----------------
int triviaScrollOffset(int overflow, unsigned long stepMs) {
  if (overflow <= 0) return 0;
  unsigned long cycle = 2 * TRIVIA_SCROLL_HOLD_MS + (unsigned long)overflow * stepMs;
  unsigned long t = (millis() - triviaViewShownAt) % cycle;
  if (t < TRIVIA_SCROLL_HOLD_MS) return 0;
  t -= TRIVIA_SCROLL_HOLD_MS;
  if (t < (unsigned long)overflow * stepMs) return (int)(t / stepMs);
  return overflow;
}

// ---------------- Trivia scrolling: true if anything in the current view is too long to show at once ----------------
bool triviaContentOverflows() {
  if (!triviaShowingAnswers) {
    return wrappedLineCount(triviaQuestionBuf, TRIVIA_QUESTION_CHARS_PER_LINE) > TRIVIA_QUESTION_VISIBLE_LINES;
  }
  for (int i = 0; i < triviaOptionCount; i++) {
    if ((int)strlen(triviaOptionBuf[i]) > TRIVIA_ANSWER_VISIBLE_CHARS) return true;
  }
  return false;
}

// ---------------- Same word-wrap approach as drawWrappedText, but confined to and centered within
// an arbitrary column instead of the full screen width - used for Pass the Bomb's category text,
// which shares the screen with a big letter off to one side. ----------------
int drawWrappedTextInColumn(const char* text, int xStart, int colWidth, int yStart, int maxLines, int lineHeight) {
  display.setTextSize(1);
  int len = strlen(text);
  int charsPerLine = max(1, (colWidth - 4) / 6); // ~6px per character at size 1
  int pos = 0;
  int lineNum = 0;

  while (pos < len && lineNum < maxLines) {
    int remaining = len - pos;
    int take = min(charsPerLine, remaining);
    if (take < remaining) {
      int breakAt = take;
      while (breakAt > 0 && text[pos + breakAt] != ' ') breakAt--;
      if (breakAt > 0) take = breakAt;
    }

    char lineBuf[24];
    int copyLen = min(take, (int)sizeof(lineBuf) - 1);
    strncpy(lineBuf, text + pos, copyLen);
    lineBuf[copyLen] = '\0';

    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(lineBuf, 0, 0, &x1, &y1, &w, &h);
    display.setCursor(xStart + (colWidth - (int)w) / 2, yStart + lineNum * lineHeight);
    display.print(lineBuf);

    pos += take;
    while (pos < len && text[pos] == ' ') pos++; // skip the space the line broke on
    lineNum++;
  }
  return lineNum;
}

// ---------------- Timer screen: remaining time as M:SS, largest text that fits ----------------
void drawTimerScreen() {
  long t = running ? remainingTime : presetTime;
  int minutes = t / 60;
  int seconds = t % 60;

  char buf[6];
  snprintf(buf, sizeof(buf), "%d:%02d", minutes, seconds);

  drawTimerIcon(7, 7, 6, SH110X_WHITE); // top-left: which game this is

  // small status line at the top, right-aligned in the space freed up by the removed mute icon
  display.setTextSize(1);
  const char* statusText;
  if (!running) statusText = T(STR_SET_TIME);
  else if (timerPaused) statusText = T(STR_PAUSED);
  else statusText = T(STR_GO_STATUS);
  int16_t sx1, sy1;
  uint16_t sw, sh;
  display.getTextBounds(statusText, 0, 0, &sx1, &sy1, &sw, &sh);
  display.setCursor(SCREEN_WIDTH - sw - 2, 4);
  display.print(statusText);

  display.drawLine(0, 14, SCREEN_WIDTH, 14, SH110X_WHITE); // always drawn - the progress sweep below (while running) correctly inverts whatever portion falls within its boundary

  // big countdown, size 4 (~24x32px per char) centered roughly
  display.setTextSize(4);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
  int x = (SCREEN_WIDTH - w) / 2;
  display.setCursor(x, 24);
  display.print(buf);

  // whole-screen progress "bar": while running, everything left of a moving boundary is
  // color-inverted (white bg, black content) - starts as the entire screen, and the boundary
  // recedes from the right edge as time runs out. Works regardless of what's drawn underneath
  // by inverting actual pixels after the fact, rather than trying to color text/icons separately.
  if (running) {
    float fraction = (runStartTime > 0) ? (float)remainingTime / (float)runStartTime : 0;
    fraction = constrain(fraction, 0.0, 1.0);
    int boundary = (int)(SCREEN_WIDTH * fraction);

    for (int yy = 0; yy < SCREEN_HEIGHT; yy++) {
      for (int xx = 0; xx < boundary; xx++) {
        uint16_t px = display.getPixel(xx, yy);
        display.drawPixel(xx, yy, px ? SH110X_BLACK : SH110X_WHITE);
      }
    }
  }
}

// ---------------- Reflex screen: arm/wait/light states, result, and session-best ----------------
void drawReflexScreen() {
  bool showFalseStart = (millis() < falseStartUntil);

  if (reflexState == REFLEX_WAITING && !showFalseStart) {
    // inverted, full-screen "don't hit yet" state - unmistakably different from every other screen
    display.fillRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SH110X_WHITE);

    int16_t x1, y1;
    uint16_t w, h;

    display.setTextColor(SH110X_BLACK);
    drawCenteredAutoFit(T(STR_WAIT_BIG), 16, 3);

    display.setTextSize(1);
    const char* sub = T(STR_DONT_HIT_YET);
    display.getTextBounds(sub, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 50);
    display.print(sub);

    return; // skip the normal icon/status layout below entirely
  }

  drawReflexIcon(7, 7, 6, SH110X_WHITE); // top-left: which game this is

  display.setTextSize(1);
  const char* statusText;
  if (showFalseStart) statusText = "";
  else if (reflexState == REFLEX_LIT) statusText = T(STR_GO_STATUS);
  else statusText = T(STR_READY);

  int16_t sx1, sy1;
  uint16_t sw, sh;
  if (statusText[0] != '\0') {
    display.getTextBounds(statusText, 0, 0, &sx1, &sy1, &sw, &sh);
    display.setCursor(SCREEN_WIDTH - sw - 2, 4);
    display.print(statusText);
  }
  display.drawLine(0, 14, SCREEN_WIDTH, 14, SH110X_WHITE);

  // main area
  if (showFalseStart) {
    display.setTextSize(2);
    const char* tooSoonText = T(STR_TOO_SOON);
    display.getTextBounds(tooSoonText, 0, 0, &sx1, &sy1, &sw, &sh);
    display.setCursor((SCREEN_WIDTH - sw) / 2, 28);
    display.print(tooSoonText);
  } else if (reflexState == REFLEX_LIT) {
    drawCenteredAutoFit(T(STR_GO_BIG), 24, 4);
  } else if (lastReactionMs >= 0) {
    char buf[12];
    snprintf(buf, sizeof(buf), "%ldms", lastReactionMs);
    display.setTextSize(3);
    display.getTextBounds(buf, 0, 0, &sx1, &sy1, &sw, &sh);
    display.setCursor((SCREEN_WIDTH - sw) / 2, 26);
    display.print(buf);
  } else {
    drawCenteredAutoFit(T(STR_PUSH), 24, 4);
  }

  // session-best line near the bottom
  display.setTextSize(1);
  char hs[24];
  if (reflexHighScoreMs >= 0) snprintf(hs, sizeof(hs), "%s: %ldms", T(STR_BEST), reflexHighScoreMs);
  else snprintf(hs, sizeof(hs), "%s: --", T(STR_BEST));
  display.getTextBounds(hs, 0, 0, &sx1, &sy1, &sw, &sh);
  display.setCursor((SCREEN_WIDTH - sw) / 2, 54);
  display.print(hs);
}

// ---------------- Dice Roll: die-type selection screen, shown first when entering the game ----------------
void drawDiceSelectScreen() {
  drawDiceIcon(7, 7, 6, SH110X_WHITE);
  display.drawLine(0, 14, SCREEN_WIDTH, 14, SH110X_WHITE);

  char buf[6];
  snprintf(buf, sizeof(buf), "d%d", DICE_SIDES[currentDieIndex]);
  int16_t sx1, sy1;
  uint16_t sw, sh;
  display.setTextSize(4);
  display.getTextBounds(buf, 0, 0, &sx1, &sy1, &sw, &sh);
  display.setCursor((SCREEN_WIDTH - sw) / 2, 22);
  display.print(buf);

  display.setTextSize(1);
  const char* label = T(STR_CHOOSE_DICE_TYPE);
  display.getTextBounds(label, 0, 0, &sx1, &sy1, &sw, &sh);
  display.setCursor((SCREEN_WIDTH - sw) / 2, 56);
  display.print(label);
}

// ---------------- Dice Roll screen: chosen die type, dice count, and the last roll's results ----------------
void drawDiceScreen() {
  drawDiceIcon(7, 7, 6, SH110X_WHITE);

  display.setTextSize(1);
  char status[10];
  snprintf(status, sizeof(status), "d%d x%d", DICE_SIDES[currentDieIndex], diceCount);
  int16_t sx1, sy1;
  uint16_t sw, sh;
  display.getTextBounds(status, 0, 0, &sx1, &sy1, &sw, &sh);
  display.setCursor(SCREEN_WIDTH - sw - 2, 4);
  display.print(status);
  display.drawLine(0, 14, SCREEN_WIDTH, 14, SH110X_WHITE);

  if (diceResultCount == 0) {
    display.setTextSize(1);
    const char* msg = T(STR_PRESS_TO_ROLL);
    display.getTextBounds(msg, 0, 0, &sx1, &sy1, &sw, &sh);
    display.setCursor((SCREEN_WIDTH - sw) / 2, 32);
    display.print(msg);
    return;
  }

  int faceSize = diceFaceSize(diceResultCount);
  int gap = 4;
  int totalWidth = diceResultCount * faceSize + (diceResultCount - 1) * gap;
  int xStart = (SCREEN_WIDTH - totalWidth) / 2;
  int yStart = diceYStart(diceResultCount, faceSize);

  int sum = 0;
  for (int i = 0; i < diceResultCount; i++) {
    drawDieFace(xStart + i * (faceSize + gap), yStart, faceSize, diceResultSides, diceResults[i], SH110X_WHITE);
    sum += diceResults[i];
  }

  if (diceResultCount > 1) {
    display.setTextSize(1);
    char sumStr[20];
    snprintf(sumStr, sizeof(sumStr), "%s: %d", T(STR_TOTAL), sum);
    display.getTextBounds(sumStr, 0, 0, &sx1, &sy1, &sw, &sh);
    int totalY = yStart + faceSize + 2;
    if (totalY > 56) totalY = 56; // keep it on-screen even in edge cases
    display.setCursor((SCREEN_WIDTH - sw) / 2, totalY);
    display.print(sumStr);
  }
}

// ---------------- Dice Roll: draws one die as a die-shaped outline, with pips (d6) or a number (other types) ----------------
void drawDieFace(int fx, int fy, int s, int sides, int value, uint16_t color) {
  display.drawRoundRect(fx, fy, s, s, max(2, s / 8), color);

  if (sides == 6) {
    int pr = max(1, s / 10);
    int cx1 = fx + s / 4, cx2 = fx + s / 2, cx3 = fx + (s * 3) / 4;
    int cy1 = fy + s / 4, cy2 = fy + s / 2, cy3 = fy + (s * 3) / 4;

    if (value == 1 || value == 3 || value == 5) display.fillCircle(cx2, cy2, pr, color);
    if (value >= 2) { display.fillCircle(cx1, cy1, pr, color); display.fillCircle(cx3, cy3, pr, color); }
    if (value == 4 || value == 5) { display.fillCircle(cx3, cy1, pr, color); display.fillCircle(cx1, cy3, pr, color); }
    if (value == 6) {
      display.fillCircle(cx1, cy2, pr, color);
      display.fillCircle(cx3, cy1, pr, color);
      display.fillCircle(cx3, cy2, pr, color);
      display.fillCircle(cx1, cy3, pr, color);
    }
  } else {
    char buf[4];
    snprintf(buf, sizeof(buf), "%d", value);
    int textSize = (s >= 40) ? 3 : (s >= 26) ? 2 : 1;
    display.setTextSize(textSize);
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
    display.setTextColor(color);
    display.setCursor(fx + (s - w) / 2, fy + (s - h) / 2 - y1);
    display.print(buf);
  }
}

// ---------------- Chess Timer: screen 1 - pick starting minutes per player ----------------
void drawChessSetupTimeScreen() {
  drawChessIcon(7, 7, 6, SH110X_WHITE);
  display.drawLine(0, 14, SCREEN_WIDTH, 14, SH110X_WHITE);

  char buf[8];
  snprintf(buf, sizeof(buf), "%ld:%02ld", chessStartSeconds / 60, chessStartSeconds % 60);
  display.setTextSize(4);
  int16_t bx1, by1;
  uint16_t bw, bh;
  display.getTextBounds(buf, 0, 0, &bx1, &by1, &bw, &bh);
  display.setCursor((SCREEN_WIDTH - bw) / 2, 22);
  display.print(buf);

  display.setTextSize(1);
  const char* label = T(STR_CHOOSE_START_TIME);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(label, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 56);
  display.print(label);
}

// ---------------- Chess Timer: screen 2 - pick the increment added back after each move ----------------
void drawChessSetupIncrementScreen() {
  drawChessIcon(7, 7, 6, SH110X_WHITE);
  display.drawLine(0, 14, SCREEN_WIDTH, 14, SH110X_WHITE);

  char buf[8];
  snprintf(buf, sizeof(buf), "+%ds", chessIncrementSeconds);
  drawCenteredAutoFit(buf, 22, 4);

  display.setTextSize(1);
  const char* label = T(STR_CHOOSE_INCREMENT);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(label, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 56);
  display.print(label);
}

// ---------------- Chess Timer: main play screen - big time (matches Timer game sizing), full-screen progress invert, bottom player label ----------------
void drawChessPlayScreen() {
  drawChessIcon(7, 7, 6, SH110X_WHITE);

  int16_t x1, y1;
  uint16_t w, h;

  if (chessScreenState == CHESS_GAME_OVER) {
    display.setTextSize(4);
    display.getTextBounds("0:00", 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 24);
    display.print("0:00");

    display.setTextSize(1);
    char status[24];
    const char* colorName = (chessLoser == 1) ? T(STR_WHITE) : T(STR_BLACK);
    snprintf(status, sizeof(status), "%s: %s", colorName, T(STR_OUT_OF_TIME));
    display.getTextBounds(status, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 56);
    display.print(status);
    display.drawLine(0, 14, SCREEN_WIDTH, 14, SH110X_WHITE);
    return;
  }

  // top status: whose turn it is once the clock has started, or just the game name before then
  display.setTextSize(1);
  const char* headerText = chessStarted ? ((chessActivePlayer == 1) ? T(STR_WHITE_TURN) : T(STR_BLACK_TURN)) : menuLabelForGame(GAME_CHESS);
  display.getTextBounds(headerText, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(SCREEN_WIDTH - w - 2, 4);
  display.print(headerText);
  display.drawLine(0, 14, SCREEN_WIDTH, 14, SH110X_WHITE); // always drawn - the full-screen invert below (during White's turn) correctly flips it

  // big time, fixed size 4 centered - same size and position as the Timer game's countdown
  long activeRemaining = (chessActivePlayer == 1) ? chessTimeP1 : chessTimeP2;
  char timeBuf[8];
  snprintf(timeBuf, sizeof(timeBuf), "%ld:%02ld", activeRemaining / 60, activeRemaining % 60);
  display.setTextSize(4);
  display.getTextBounds(timeBuf, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 24);
  display.print(timeBuf);

  // bottom slot: "White is starting" / "Paused" when relevant, otherwise the progress bar (below)
  const char* bottomMsg = NULL;
  if (!chessStarted) bottomMsg = T(STR_WHITE_STARTING);
  else if (chessPaused) bottomMsg = T(STR_PAUSED);

  if (bottomMsg != NULL) {
    display.setTextSize(1);
    display.getTextBounds(bottomMsg, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 56);
    display.print(bottomMsg);
  } else {
    // small progress bar - starts full, empties from the right as the active player's time runs out
    int barX = 4, barY = 56, barW = SCREEN_WIDTH - 8, barH = 6;
    long activePlayerMax = (chessActivePlayer == 1) ? chessMaxP1 : chessMaxP2;
    float fraction = (activePlayerMax > 0) ? (float)activeRemaining / (float)activePlayerMax : 0;
    fraction = constrain(fraction, 0.0, 1.0);

    display.drawRect(barX, barY, barW, barH, SH110X_WHITE);
    int filledW = (int)((barW - 2) * fraction);
    if (filledW > 0) {
      display.fillRect(barX + 1, barY + 1, filledW, barH - 2, SH110X_WHITE);
    }
  }

  // whole-screen color indicates whose turn it is: White (Player 1, who always starts) inverts
  // the entire screen to a white background with black content; Black (Player 2) leaves it
  // normal. Same per-pixel invert technique as the Timer game, just applied to the full screen
  // rather than a receding boundary, since here it's a binary player indicator, not a progress bar.
  if (chessStarted && chessActivePlayer == 1) {
    for (int yy = 0; yy < SCREEN_HEIGHT; yy++) {
      for (int xx = 0; xx < SCREEN_WIDTH; xx++) {
        uint16_t px = display.getPixel(xx, yy);
        display.drawPixel(xx, yy, px ? SH110X_BLACK : SH110X_WHITE);
      }
    }
  }
}

// ---------------- Hot Potato screen ----------------
void drawHotPotatoScreen() {
  drawHotPotatoIcon(7, 7, 6, SH110X_WHITE);

  int16_t x1, y1;
  uint16_t w, h;
  display.setTextSize(1);
  const char* status;
  if (hpState == HP_READY) status = T(STR_HP_ARM);
  else if (hpState == HP_ARMED) status = T(STR_HP_ARMED);
  else status = T(STR_HP_BOOM);
  display.getTextBounds(status, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(SCREEN_WIDTH - w - 2, 4);
  display.print(status);
  display.drawLine(0, 14, SCREEN_WIDTH, 14, SH110X_WHITE);

  if (hpState == HP_EXPLODED) {
    return; // same as the Reflex false-start screen: just the "BOOM!" text above, no icon
  }

  // content area is shrunk to y=16..48 (was 16..62) to leave room for the bottom hint row below,
  // whether or not that row is actually shown right now - keeps the layout visually consistent
  // between HP_READY (row shown) and HP_ARMED (row hidden) rather than jumping between the two
  const int contentTop = 16;
  const int contentBottom = 48;

  // left 2/3: category name, word-wrapped within its column and vertically centered in the
  // content area - measured the same way drawWrappedTextInColumn decides its own line count
  // (character-count-based, matching its word-break logic) so this stays in sync with it
  const int leftColWidth = (SCREEN_WIDTH * 2) / 3;
  const char* catText = T(HP_CATEGORY_STRINGS[hpCategory]);
  const int catLineHeight = 12;
  int catCharsPerLine = max(1, (leftColWidth - 4) / 6);
  int catLines = ((int)strlen(catText) <= catCharsPerLine) ? 1 : 2;
  int catYStart = contentTop + ((contentBottom - contentTop) - catLines * catLineHeight) / 2 + 3;
  drawWrappedTextInColumn(catText, 0, leftColWidth, catYStart, 2, catLineHeight);

  // right 1/3: "Letter" label above the letter itself, shown big
  const int rightColX = leftColWidth;
  const int rightColWidth = SCREEN_WIDTH - leftColWidth;
  display.drawLine(leftColWidth, 16, leftColWidth, contentBottom, SH110X_WHITE); // divider between the two columns

  display.setTextSize(1);
  const char* letterLabel = T(STR_HP_LETTER_LABEL);
  display.getTextBounds(letterLabel, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(rightColX + (rightColWidth - (int)w) / 2, 18);
  display.print(letterLabel);

  char letterBuf[2] = { (char)('A' + hpLetter), '\0' };
  display.setTextSize(3);
  display.getTextBounds(letterBuf, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(rightColX + (rightColWidth - (int)w) / 2, 27);
  display.print(letterBuf);
  display.setTextSize(1);

  // bottom hint row: only while HP_READY, since rerolling is only possible then - same 14px
  // inverted-band technique as Trivia's top row (draw normally, then invert the whole strip).
  // Icon on the right, close to where the physical encoder actually is; text to its left.
  if (hpState == HP_READY) {
    const int rowTop = SCREEN_HEIGHT - 14;
    const int rowCenterY = rowTop + 7;
    const int iconCx = SCREEN_WIDTH - 9;
    drawRotateIcon(iconCx, rowCenterY, 5, SH110X_WHITE);

    const char* hint = T(STR_HP_ROTATE_HINT);
    display.getTextBounds(hint, 0, 0, &x1, &y1, &w, &h);
    display.setCursor(2, rowTop + (14 - (int)h) / 2 - 1);
    display.print(hint);

    invertScreenRegion(0, rowTop, SCREEN_WIDTH, SCREEN_HEIGHT);
  }
}

// ---------------- Turn Order screen ----------------
void drawTurnOrderScreen() {
  drawTurnOrderIcon(7, 7, 6, SH110X_WHITE);

  display.setTextSize(1);
  const char* headerText = menuLabelForGame(GAME_TURNORDER);
  int16_t hx1, hy1;
  uint16_t hw, hh;
  display.getTextBounds(headerText, 0, 0, &hx1, &hy1, &hw, &hh);
  display.setCursor(SCREEN_WIDTH - hw - 2, 4);
  display.print(headerText);
  display.drawLine(0, 14, SCREEN_WIDTH, 14, SH110X_WHITE);

  int16_t x1, y1;
  uint16_t w, h;

  if (turnOrderResult < 0) {
    // selection mode: player count shown big and centered, same size/position as other pickers
    char countBuf[4];
    snprintf(countBuf, sizeof(countBuf), "%d", turnOrderPlayerCount);
    display.setTextSize(4);
    display.getTextBounds(countBuf, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 22);
    display.print(countBuf);

    display.setTextSize(1);
    const char* label = T(STR_CHOOSE_PLAYERS);
    display.getTextBounds(label, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 56);
    display.print(label);
  } else {
    // result mode: the winning player number, big
    char buf[4];
    snprintf(buf, sizeof(buf), "%d", turnOrderResult);
    display.setTextSize(4);
    display.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 22);
    display.print(buf);

    display.setTextSize(1);
    const char* msg = T(STR_GOES_FIRST);
    display.getTextBounds(msg, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 56);
    display.print(msg);
  }
}

// ---------------- Stopwatch screen ----------------
void drawStopwatchScreen() {
  drawStopwatchIcon(7, 7, 6, SH110X_WHITE);

  display.setTextSize(1);
  const char* headerText = menuLabelForGame(GAME_STOPWATCH);
  int16_t hx1, hy1;
  uint16_t hw, hh;
  display.getTextBounds(headerText, 0, 0, &hx1, &hy1, &hw, &hh);
  display.setCursor(SCREEN_WIDTH - hw - 2, 4);
  display.print(headerText);
  display.drawLine(0, 14, SCREEN_WIDTH, 14, SH110X_WHITE);

  unsigned long totalMs = stopwatchElapsedMs;
  if (stopwatchState == STOPWATCH_RUNNING) totalMs += millis() - stopwatchStartMillis;
  unsigned long totalSec = totalMs / 1000;
  unsigned long hundredths = (totalMs / 10) % 100;

  char mainBuf[8];
  snprintf(mainBuf, sizeof(mainBuf), "%lu:%02lu", totalSec / 60, totalSec % 60);
  char decBuf[4];
  snprintf(decBuf, sizeof(decBuf), ".%02lu", hundredths);

  int16_t x1, y1;
  uint16_t mainW, mainH, decW, decH;
  display.setTextSize(4);
  display.getTextBounds(mainBuf, 0, 0, &x1, &y1, &mainW, &mainH);
  display.setTextSize(1);
  display.getTextBounds(decBuf, 0, 0, &x1, &y1, &decW, &decH);

  int startX = (SCREEN_WIDTH - (mainW + decW)) / 2;

  display.setTextSize(4);
  display.setCursor(startX, 24);
  display.print(mainBuf);

  display.setTextSize(1);
  display.setCursor(startX + mainW, 24 + mainH - decH); // bottom-aligned, like a smaller trailing decimal
  display.print(decBuf);

  uint16_t w, h;
  display.setTextSize(1);
  if (stopwatchLapSeconds >= 0) {
    char lapBuf[20];
    snprintf(lapBuf, sizeof(lapBuf), "%s: %ld:%02ld", T(STR_LAP), stopwatchLapSeconds / 60, stopwatchLapSeconds % 60);
    display.getTextBounds(lapBuf, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 56);
    display.print(lapBuf);
  }
}

// ---------------- Toothbrush: pick 1-5 minutes, default 2 ----------------
void drawToothbrushSelectScreen() {
  drawToothbrushIcon(7, 7, 6, SH110X_WHITE);

  display.setTextSize(1);
  const char* headerText = menuLabelForGame(GAME_TOOTHBRUSH);
  int16_t hx1, hy1;
  uint16_t hw, hh;
  display.getTextBounds(headerText, 0, 0, &hx1, &hy1, &hw, &hh);
  display.setCursor(SCREEN_WIDTH - hw - 2, 4);
  display.print(headerText);
  display.drawLine(0, 14, SCREEN_WIDTH, 14, SH110X_WHITE);

  char buf[8];
  snprintf(buf, sizeof(buf), "%d min", toothbrushMinutes);
  drawCenteredAutoFit(buf, 22, 4);

  display.setTextSize(1);
  const char* label = T(STR_CHOOSE_BRUSH_TIME);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(label, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 56);
  display.print(label);
}

// ---------------- Toothbrush: teeth drawing ----------------
// A front view of an open mouth, teeth only: the upper arch curves over the top half of the
// left 77px, the lower arch is its mirror image below. Each tooth is a rounded rectangle whose
// crown edge (toward the open mouth) is rounded and gum edge is square; molars/premolars are
// rounded all round. Coordinates are for the upper-left half of the upper arch; the other three
// quarters are mirrored. Teeth are separated by at least 1px so filled teeth never merge.
struct ToothShape { int8_t x, y, w, h, r; char round; int8_t zoneSide; }; // zoneSide: 0 = side zone, 1 = front zone
const ToothShape TOOTH_UPPER_LEFT_HALF[] = {
  {31, 1, 7, 12, 3, 'b', 1}, // central incisor
  {24, 2, 6, 10, 3, 'b', 1}, // lateral incisor
  {18, 3, 5, 11, 2, 'b', 0}, // canine (side zone, so every zone has 4 teeth)
  {11, 6, 6,  9, 2, 'a', 0}, // premolar 1
  { 4,13, 6,  8, 2, 'a', 0}, // premolar 2
  { 0,22, 9,  8, 2, 'a', 0}, // molar
};
const int TOOTH_HALF_COUNT = sizeof(TOOTH_UPPER_LEFT_HALF) / sizeof(TOOTH_UPPER_LEFT_HALF[0]);
const int TEETH_MIRROR_X = 76;  // right half: x' = 76 - (x + w - 1)
const int TEETH_MIRROR_Y = 63;  // lower arch: y' = 63 - (y + h - 1)
const int TOOTHBRUSH_PANEL_X = 78; // vertical divider; time + zone label to its right (teeth end at x=76)

// is pixel (i,j) of a w x h tooth inside its outline? round: 'b' = bottom corners rounded,
// 't' = top corners rounded, 'a' = all four
bool toothContains(int i, int j, int w, int h, int r, char round) {
  if (i < 0 || j < 0 || i >= w || j >= h) return false;
  bool roundTop = (round != 'b'), roundBottom = (round != 't');
  int cx = -1, cy = -1;
  if (i < r) cx = r; else if (i > w - 1 - r) cx = w - 1 - r;
  if (j < r && roundTop) cy = r; else if (j > h - 1 - r && roundBottom) cy = h - 1 - r;
  if (cx < 0 || cy < 0) return true;
  int dx = i - cx, dy = j - cy;
  return (dx * dx + dy * dy) * 5 <= r * r * 5 + r * 4; // r^2 + 0.8r, the same rounding as the mockup
}

void drawTooth(int x, int y, int w, int h, int r, char round, bool filled) {
  for (int j = 0; j < h; j++) {
    for (int i = 0; i < w; i++) {
      if (!toothContains(i, j, w, h, r, round)) continue;
      bool edge = !toothContains(i - 1, j, w, h, r, round) || !toothContains(i + 1, j, w, h, r, round) ||
                  !toothContains(i, j - 1, w, h, r, round) || !toothContains(i, j + 1, w, h, r, round);
      if (filled || edge) display.drawPixel(x + i, y + j, SH110X_WHITE);
    }
  }
}

// ---------------- Toothbrush: the brush sprite, scrubbing on the active zone ----------------
// Built from small rects for the upper-left / upper-front zones, then mirrored for the others.
// Each rect is first cleared with a 1px black margin so the brush reads clearly against teeth.
struct BrushRect { int8_t x, y, w, h; };
BrushRect toothbrushSprite[24]; // global, not a parameter: Arduino's auto-generated prototypes sit above this struct
int buildBrushSprite(int zone, int offset) {
  BrushRect* out = toothbrushSprite;
  int n = 0;
  int side = zone % 3; // 0 left, 1 front, 2 right
  if (side == 1) { // flat brush under the front teeth, bristles up, handle to the right
    int hx = 29 + offset;
    for (int i = 0; i < 16; i += 2) out[n++] = {(int8_t)(hx + i), 14, 1, 2}; // bristles
    out[n++] = {(int8_t)hx, 16, 16, 3};        // head
    out[n++] = {(int8_t)(hx + 16), 17, 4, 1};  // neck
    out[n++] = {(int8_t)(hx + 19), 17, 14, 2}; // handle
  } else {          // upright brush beside the side teeth, bristles out, handle into the mouth
    int hy = 19 + offset;
    for (int j = 0; j < 11; j += 2) out[n++] = {11, (int8_t)(hy + j), 2, 1}; // bristles
    out[n++] = {13, (int8_t)hy, 3, 11};                                      // head
    for (int k = 0; k < 13; k++) out[n++] = {(int8_t)(16 + k), (int8_t)(hy + 6 + (k * 9 + 10) / 20), 1, 2}; // handle, sloping down
  }
  for (int i = 0; i < n; i++) {
    if (side == 2) out[i].x = TEETH_MIRROR_X - (out[i].x + out[i].w - 1);
    if (zone >= 3) out[i].y = TEETH_MIRROR_Y - (out[i].y + out[i].h - 1);
  }
  return n;
}

// ---------------- Toothbrush: running screen - two arches of teeth (left) split into 6 zones;
// outlined = still to brush, solid = brushed, the active zone blinks with the scrub clicks while
// a brush scrubs over it with a little foam. Right panel: "Brush!"/"Paused", remaining time,
// two-line zone name, and a bar for progress through the current zone. The finished screen
// keeps the usual header + static bursts. ----------------
void drawToothbrushRunningScreen() {
  int16_t x1, y1;
  uint16_t w, h;

  if (toothbrushScreenState == TOOTHBRUSH_DONE) {
    drawToothbrushIcon(7, 7, 6, SH110X_WHITE);
    display.setTextSize(1);
    const char* headerText = T(STR_BRUSH_ACTIVE);
    display.getTextBounds(headerText, 0, 0, &x1, &y1, &w, &h);
    display.setCursor(SCREEN_WIDTH - w - 2, 4);
    display.print(headerText);
    display.drawLine(0, 14, SCREEN_WIDTH, 14, SH110X_WHITE);
    // the firework animation already played once (see playToothbrushDoneAnimation) - this is the
    // persistent screen shown afterward, reusing the same drawBurst() helper
    drawBurst(34, 28, 12);
    drawBurst(SCREEN_WIDTH / 2, 34, 12);
    drawBurst(SCREEN_WIDTH - 34, 28, 12);
    const char* doneText = T(STR_BRUSHING_DONE);
    display.getTextBounds(doneText, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 57);
    display.print(doneText);
    return;
  }

  int zone = toothbrushCurrentZone();
  bool blinkOn = toothbrushPaused || toothbrushPulseState;

  // teeth: 4 mirrored copies of the upper-left half
  for (int arch = 0; arch < 2; arch++) {          // 0 upper, 1 lower
    for (int half = 0; half < 2; half++) {        // 0 left, 1 right
      for (int t = 0; t < TOOTH_HALF_COUNT; t++) {
        const ToothShape& s = TOOTH_UPPER_LEFT_HALF[t];
        int x = half ? TEETH_MIRROR_X - (s.x + s.w - 1) : s.x;
        int y = arch ? TEETH_MIRROR_Y - (s.y + s.h - 1) : s.y;
        char round = s.round;
        if (arch && round == 'b') round = 't';
        int toothZone = arch * 3 + (s.zoneSide ? 1 : (half ? 2 : 0));
        bool filled = (toothZone < zone) || (toothZone == zone && blinkOn);
        drawTooth(x, y, s.w, s.h, s.r, round, filled);
      }
    }
  }

  // brush + foam
  int offset = toothbrushPaused ? 0 : (toothbrushPulseState ? 2 : -2);
  int n = buildBrushSprite(zone, offset);
  const BrushRect* brush = toothbrushSprite;
  for (int i = 0; i < n; i++) display.fillRect(brush[i].x - 1, brush[i].y - 1, brush[i].w + 2, brush[i].h + 2, SH110X_BLACK);
  for (int i = 0; i < n; i++) display.fillRect(brush[i].x, brush[i].y, brush[i].w, brush[i].h, SH110X_WHITE);
  if (!toothbrushPaused) {
    const BrushRect* head = nullptr;
    for (int i = 0; i < n && !head; i++) if (brush[i].w >= 3 && brush[i].h >= 3) head = &brush[i];
    int side = zone % 3;
    bool upper = zone < 3;
    for (int b = 0; head && b < 3; b++) {
      int along = (toothbrushFrame * 5 + b * 7) % max((int)head->w, (int)head->h);
      int away = 3 + (toothbrushFrame + b * 2) % 5;
      int px, py;
      if (side == 1) { px = head->x + along; py = upper ? head->y + head->h + away : head->y - 1 - away; }
      else { py = head->y + along; px = (side == 0) ? head->x + head->w + away : head->x - 1 - away; }
      int r = ((toothbrushFrame + b) % 3 == 0) ? 1 : 0;
      bool clear = true; // only foam into empty space, never onto a tooth
      for (int j = -r - 1; j <= r + 1 && clear; j++)
        for (int i = -r - 1; i <= r + 1; i++) {
          int X = px + i, Y = py + j;
          if (X < 0 || Y < 0 || X >= TOOTHBRUSH_PANEL_X || Y >= SCREEN_HEIGHT || display.getPixel(X, Y)) { clear = false; break; }
        }
      if (!clear) continue;
      if (r) display.drawCircle(px, py, 1, SH110X_WHITE); else display.drawPixel(px, py, SH110X_WHITE);
    }
  }

  // right panel
  display.drawFastVLine(TOOTHBRUSH_PANEL_X, 0, SCREEN_HEIGHT, SH110X_WHITE);
  const int panelCenter = (TOOTHBRUSH_PANEL_X + 1 + SCREEN_WIDTH) / 2; // texts centered on their visible width
  display.setTextSize(1);
  const char* topText = toothbrushPaused ? T(STR_PAUSED) : T(STR_BRUSH_ACTIVE);
  display.getTextBounds(topText, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(panelCenter - ((int)w - 1) / 2, 2);
  display.print(topText);

  char timeBuf[6];
  snprintf(timeBuf, sizeof(timeBuf), "%ld:%02ld", toothbrushRemainingSeconds / 60, toothbrushRemainingSeconds % 60);
  display.setTextSize(2);
  display.getTextBounds(timeBuf, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(panelCenter - ((int)w - 2) / 2, 15); // w includes the font's trailing blank column (2px at size 2)
  display.print(timeBuf);

  display.setTextSize(1);
  if (!toothbrushPaused) {
    const int line2[3] = {STR_ZONE_LEFT, STR_ZONE_FRONT, STR_ZONE_RIGHT};
    const char* l1 = T(zone < 3 ? STR_ZONE_UPPER : STR_ZONE_LOWER);
    const char* l2 = T(line2[zone % 3]);
    display.getTextBounds(l1, 0, 0, &x1, &y1, &w, &h);
    display.setCursor(panelCenter - ((int)w - 1) / 2, 36);
    display.print(l1);
    display.getTextBounds(l2, 0, 0, &x1, &y1, &w, &h);
    display.setCursor(panelCenter - ((int)w - 1) / 2, 45);
    display.print(l2);
  }
  display.drawRect(81, 57, 45, 6, SH110X_WHITE);
  display.fillRect(82, 58, (int)(43 * toothbrushZoneFraction() + 0.5f), 4, SH110X_WHITE);
}

// ================= Tetris screens =================
// All three temporarily rotate the display 90 degrees so the panel's long 128px axis becomes the
// vertical "falling" axis and the short 64px axis becomes the board's width - meant to be played
// with the whole box turned on its side. Each function resets the rotation back to normal (0)
// before returning, so nothing else on the device is ever affected by it.
//
// NOTE: rotation direction wasn't verifiable on real hardware while building this. If, once
// rotated, the board's "down" ends up on the screen's LEFT instead of the RIGHT, change the
// argument below from setRotation(1) to setRotation(3) (all three functions, same fix each time).
// If left/right feels backwards, flip the sign of "direction" in tetrisMove() instead.

// ---------------- Shared: an inverted (filled white, black text) title bar spanning the full
// rotated width - used by the Ready/Game Over screens of both Tetris and Star Drop for a bit of
// visual polish. Must be called with rotation already set to 1 (or 3); leaves text color as white
// (the normal drawing color) when it returns, ready for whatever content follows underneath. ----------------
int drawRotatedInvertedTitle(const char* title) {
  display.setTextSize(1);
  display.setTextWrap(false); // otherwise getTextBounds() on an over-width string returns a size
                               // that already accounts for GFX's own mid-word wrap, breaking the checks below
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(title, 0, 0, &x1, &y1, &w, &h);
  int rw = display.width();

  if ((int)w <= rw - 4) {
    // fits comfortably on one line
    int barHeight = (int)h + 8;
    display.fillRect(0, 0, rw, barHeight, SH110X_WHITE);
    display.setTextColor(SH110X_BLACK);
    display.setCursor((rw - (int)w) / 2, (barHeight - (int)h) / 2);
    display.print(title);
    display.setTextColor(SH110X_WHITE);
    display.setTextWrap(true);
    return barHeight;
  }

  // too wide for one line (some translations, e.g. French "Partie terminee", are longer than the
  // 64px rotated width) - split at whichever space sits closest to the middle of the string
  String s(title);
  int mid = s.length() / 2;
  int splitAt = -1;
  for (int i = 0; i < (int)s.length(); i++) {
    if (s[i] == ' ' && (splitAt < 0 || abs(i - mid) < abs(splitAt - mid))) splitAt = i;
  }
  String line1 = (splitAt >= 0) ? s.substring(0, splitAt) : s;
  String line2 = (splitAt >= 0) ? s.substring(splitAt + 1) : "";

  uint16_t w1, h1, w2, h2;
  display.getTextBounds(line1.c_str(), 0, 0, &x1, &y1, &w1, &h1);
  display.getTextBounds(line2.c_str(), 0, 0, &x1, &y1, &w2, &h2);

  int lineH = (int)h1;
  int barHeight = lineH * 2 + 10;
  display.fillRect(0, 0, rw, barHeight, SH110X_WHITE);
  display.setTextColor(SH110X_BLACK);
  display.setCursor((rw - (int)w1) / 2, 3);
  display.print(line1);
  if (line2.length() > 0) {
    display.setCursor((rw - (int)w2) / 2, 3 + lineH + 2);
    display.print(line2);
  }
  display.setTextColor(SH110X_WHITE);
  display.setTextWrap(true);
  return barHeight;
}

// ---------------- Shared: word-wrapped, centered text for the rotated screens - same word-boundary
// logic as drawWrappedText, but sized to display.width() (dynamic under rotation) instead of the
// fixed 128px SCREEN_WIDTH, so long translated strings wrap between words instead of mid-word.
// Returns the number of lines drawn, so callers can space subsequent content accordingly. ----------------
int drawRotatedWrappedText(const char* text, int yStart, int lineHeight) {
  display.setTextSize(1);
  display.setTextWrap(false);
  int rw = display.width();
  int charsPerLine = max(1, (rw - 4) / 6); // ~6px per character at size 1
  int len = strlen(text);
  int pos = 0;
  int lineNum = 0;

  while (pos < len) {
    int remaining = len - pos;
    int take = min(charsPerLine, remaining);
    if (take < remaining) {
      int breakAt = take;
      while (breakAt > 0 && text[pos + breakAt] != ' ') breakAt--;
      if (breakAt > 0) take = breakAt;
    }

    char lineBuf[24];
    int copyLen = min(take, (int)sizeof(lineBuf) - 1);
    strncpy(lineBuf, text + pos, copyLen);
    lineBuf[copyLen] = '\0';

    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(lineBuf, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((rw - (int)w) / 2, yStart + lineNum * lineHeight);
    display.print(lineBuf);

    pos += take;
    while (pos < len && text[pos] == ' ') pos++; // skip the space the line broke on
    lineNum++;
  }
  display.setTextWrap(true);
  return lineNum;
}

// ---------------- Tetris: start screen ----------------
void drawTetrisReadyScreen() {
  display.setRotation(1);
  int barHeight = drawRotatedInvertedTitle("TETRIS");

  int rh = display.height(); // 128 while rotated
  int rw = display.width();

  drawTetrisIcon(rw / 2, barHeight + (rh - barHeight) / 2 - 10, 14, SH110X_WHITE);

  const char* prompt = T(STR_TETRIS_PRESS_START);
  drawRotatedWrappedText(prompt, rh - 46, 10);

  if (tetrisScore > 0) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%s: %d", T(STR_TETRIS_SCORE), tetrisScore);
    drawRotatedWrappedText(buf, rh - 16, 10);
  } else if (tetrisHighScore > 0) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%s: %d", T(STR_STARDROP_HIGH), tetrisHighScore);
    drawRotatedWrappedText(buf, rh - 16, 10);
  }

  display.setRotation(0);
}

// ---------------- Tetris: main play screen - a thin HUD strip (score/lines) at the top, with the
// board filling the rest of the rotated screen below it ----------------
void drawTetrisPlayScreen() {
  display.setRotation(1);

  display.setTextSize(1);
  char hud[16];
  snprintf(hud, sizeof(hud), "S%d L%d", tetrisScore, tetrisLinesCleared);
  display.setCursor(2, 0);
  display.print(hud);
  display.drawLine(0, TETRIS_HUD_HEIGHT - 1, display.width(), TETRIS_HUD_HEIGHT - 1, SH110X_WHITE);

  for (int r = 0; r < TETRIS_BOARD_ROWS; r++) {
    for (int c = 0; c < TETRIS_BOARD_COLS; c++) {
      if (tetrisBoard[r][c]) {
        display.fillRect(c * TETRIS_CELL_SIZE, TETRIS_HUD_HEIGHT + r * TETRIS_CELL_SIZE, TETRIS_CELL_SIZE - 1, TETRIS_CELL_SIZE - 1, SH110X_WHITE);
      }
    }
  }

  uint16_t shape = TETRIS_SHAPES[tetrisPieceType][tetrisPieceRotation];

  // ghost piece: outline showing where it would land from here - drawn first so the actual
  // (filled) piece below naturally covers it wherever they overlap near the end of the fall
  int ghostRow = tetrisGhostRow();
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      if (!(shape & (1 << (r * 4 + c)))) continue;
      int boardRow = ghostRow + r;
      int boardCol = tetrisPieceCol + c;
      if (boardRow >= 0) {
        display.drawRect(boardCol * TETRIS_CELL_SIZE, TETRIS_HUD_HEIGHT + boardRow * TETRIS_CELL_SIZE, TETRIS_CELL_SIZE - 1, TETRIS_CELL_SIZE - 1, SH110X_WHITE);
      }
    }
  }

  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      if (!(shape & (1 << (r * 4 + c)))) continue;
      int boardRow = tetrisPieceRow + r;
      int boardCol = tetrisPieceCol + c;
      if (boardRow >= 0) {
        display.fillRect(boardCol * TETRIS_CELL_SIZE, TETRIS_HUD_HEIGHT + boardRow * TETRIS_CELL_SIZE, TETRIS_CELL_SIZE - 1, TETRIS_CELL_SIZE - 1, SH110X_WHITE);
      }
    }
  }

  display.setRotation(0);
}

// ---------------- Tetris: game over screen ----------------
void drawTetrisGameOverScreen() {
  display.setRotation(1);
  int barHeight = drawRotatedInvertedTitle(T(STR_TETRIS_GAME_OVER));

  int rh = display.height();
  int midY = barHeight + (rh - barHeight) / 2;

  char buf[24];
  snprintf(buf, sizeof(buf), "%s: %d", T(STR_TETRIS_SCORE), tetrisScore);
  drawRotatedWrappedText(buf, midY - 20, 10);

  snprintf(buf, sizeof(buf), "%s: %d", T(STR_STARDROP_HIGH), tetrisHighScore);
  drawRotatedWrappedText(buf, midY + 4, 10);

  const char* prompt = T(STR_TETRIS_PRESS_START);
  drawRotatedWrappedText(prompt, midY + 28, 10);

  display.setRotation(0);
}

// ================= Star Drop screens =================
// Same rotated-display approach as Tetris - see the note there about setRotation(1) vs (3) if the
// orientation ends up backwards on real hardware; the fix is identical here.

// ---------------- Star Drop: start screen ----------------
void drawStarDropReadyScreen() {
  display.setRotation(1);
  int barHeight = drawRotatedInvertedTitle("STAR DROP");

  int rw = display.width();
  int rh = display.height();
  int midY = barHeight + (rh - barHeight) / 2;

  drawStarDropObjectIcon(rw / 2 - 10, midY - 16, 0, SH110X_WHITE);
  drawStarDropObjectIcon(rw / 2 + 8, midY - 24, 1, SH110X_WHITE);
  drawStarDropObjectIcon(rw / 2, midY, 2, SH110X_WHITE);

  const char* prompt = T(STR_STARDROP_PRESS_START);
  drawRotatedWrappedText(prompt, rh - 46, 10);

  char buf[24];
  snprintf(buf, sizeof(buf), "%s: %d", T(STR_STARDROP_HIGH), stardropHighScore);
  drawRotatedWrappedText(buf, rh - 16, 10);

  display.setRotation(0);
}

// ---------------- Star Drop: main play screen - HUD strip (score + lives) at the top, falling
// objects and the telescope catcher below ----------------
void drawStarDropPlayScreen() {
  display.setRotation(1);

  display.setTextSize(1);
  char hud[20];
  snprintf(hud, sizeof(hud), "S%d x%d", stardropScore, stardropLives);
  display.setCursor(2, 0);
  display.print(hud);
  display.drawLine(0, STARDROP_HUD_HEIGHT - 1, display.width(), STARDROP_HUD_HEIGHT - 1, SH110X_WHITE);

  for (int i = 0; i < STARDROP_MAX_OBJECTS; i++) {
    if (!stardropObjActive[i]) continue;
    int cx = stardropObjLane[i] * STARDROP_LANE_WIDTH + STARDROP_LANE_WIDTH / 2;
    int cy = (int)stardropObjY[i];
    drawStarDropObjectIcon(cx, cy, stardropObjType[i], SH110X_WHITE);
  }

  int catcherX = stardropCatcherLane * STARDROP_LANE_WIDTH + STARDROP_LANE_WIDTH / 2;
  drawStarDropTelescopeIcon(catcherX, display.height() - 2, SH110X_WHITE);

  display.setRotation(0);
}

// ---------------- Star Drop: game over screen ----------------
void drawStarDropGameOverScreen() {
  display.setRotation(1);
  int barHeight = drawRotatedInvertedTitle(T(STR_STARDROP_GAME_OVER));

  int rh = display.height();
  int midY = barHeight + (rh - barHeight) / 2;

  char buf[24];
  snprintf(buf, sizeof(buf), "%s: %d", T(STR_STARDROP_SCORE), stardropScore);
  drawRotatedWrappedText(buf, midY - 20, 10);

  snprintf(buf, sizeof(buf), "%s: %d", T(STR_STARDROP_HIGH), stardropHighScore);
  drawRotatedWrappedText(buf, midY + 4, 10);

  const char* prompt = T(STR_STARDROP_PRESS_START);
  drawRotatedWrappedText(prompt, midY + 28, 10);

  display.setRotation(0);
}

// ================= Snake screen =================
// One function handles all 4 states - simple enough not to need Tetris/Star Drop's separate
// Ready/Play/Game Over functions, and it never rotates the display (see the state block's note
// on why a wide grid suits the normal orientation fine here).
void drawSnakeScreen() {
  drawSnakeIcon(7, 7, 6, SH110X_WHITE);

  display.setTextSize(1);
  char scoreBuf[16];
  const char* status;
  if (snakeState == SNAKE_READY) {
    status = T(STR_READY);
  } else if (snakeState == SNAKE_GAME_OVER) {
    status = T(STR_TETRIS_GAME_OVER); // "Game Over" - generic, already reused by Tetris
  } else if (snakeState == SNAKE_PAUSED) {
    status = T(STR_PAUSED);
  } else {
    snprintf(scoreBuf, sizeof(scoreBuf), "%s: %d", T(STR_TETRIS_SCORE), snakeScore);
    status = scoreBuf;
  }
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(status, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(SCREEN_WIDTH - w - 2, 4);
  display.print(status);
  display.drawLine(0, 14, SCREEN_WIDTH, 14, SH110X_WHITE);

  if (snakeState == SNAKE_READY) {
    const char* prompt = T(STR_TETRIS_PRESS_START);
    display.getTextBounds(prompt, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 30);
    display.print(prompt);

    if (snakeHighScore > 0) {
      char buf[24];
      snprintf(buf, sizeof(buf), "%s: %d", T(STR_STARDROP_HIGH), snakeHighScore);
      display.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
      display.setCursor((SCREEN_WIDTH - w) / 2, 44);
      display.print(buf);
    }
    return;
  }

  if (snakeState == SNAKE_GAME_OVER) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%s: %d", T(STR_TETRIS_SCORE), snakeScore);
    display.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 24);
    display.print(buf);

    snprintf(buf, sizeof(buf), "%s: %d", T(STR_STARDROP_HIGH), snakeHighScore);
    display.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 38);
    display.print(buf);

    const char* prompt = T(STR_TETRIS_PRESS_START);
    display.getTextBounds(prompt, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 52);
    display.print(prompt);
    return;
  }

  // PLAYING or PAUSED: draw the board - snake body as filled cells, food as a small circle
  for (int i = 0; i < snakeLength; i++) {
    int px = snakeCol[i] * SNAKE_CELL_SIZE;
    int py = SNAKE_HUD_HEIGHT + snakeRow[i] * SNAKE_CELL_SIZE;
    display.fillRect(px, py, SNAKE_CELL_SIZE - 1, SNAKE_CELL_SIZE - 1, SH110X_WHITE);
  }
  int fx = snakeFoodCol * SNAKE_CELL_SIZE + SNAKE_CELL_SIZE / 2 - 1;
  int fy = SNAKE_HUD_HEIGHT + snakeFoodRow * SNAKE_CELL_SIZE + SNAKE_CELL_SIZE / 2 - 1;
  display.fillCircle(fx, fy, max(1, SNAKE_CELL_SIZE / 2 - 1), SH110X_WHITE);
}

// ================= SimpleMorse screen =================
// ---------------- Draws a word with each letter's Morse code centered underneath it, one column
// per letter (column width = screen width / letter count, so it scales for any word length).
// Used by both the Ready screen and the post-result solution reveal, so the two look identical. ----------------
void drawWordWithCode(const char* word, int yTop) {
  int len = strlen(word);
  if (len == 0) return;
  int colWidth = SCREEN_WIDTH / len;

  display.setTextSize(1);
  int16_t x1, y1;
  uint16_t w, h;
  for (int i = 0; i < len; i++) {
    char letterStr[2] = { word[i], '\0' };
    int colX = i * colWidth;

    display.getTextBounds(letterStr, 0, 0, &x1, &y1, &w, &h);
    display.setCursor(colX + (colWidth - (int)w) / 2, yTop);
    display.print(letterStr);

    if (word[i] >= 'A' && word[i] <= 'Z') {
      const char* code = MORSE_TABLE[word[i] - 'A'];
      display.getTextBounds(code, 0, 0, &x1, &y1, &w, &h);
      display.setCursor(colX + (colWidth - (int)w) / 2, yTop + 10);
      display.print(code);
    }
  }
}

// ---------------- Draws the word as one box per letter: an outline (letter shown normally
// inside it) until that letter's Morse code has been fully and correctly repeated this round, at
// which point the box fills solid and the letter switches to inverted text - same "filled
// selection" technique used for the menu's highlighted row, just applied per letter here. Never
// shows the actual code/dots-and-dashes - only WHICH letters are done, not HOW they were done,
// so repeating still has to be from memory/ear, not read off the screen. ----------------
void drawWordProgress(const char* word, int yTop) {
  int len = strlen(word);
  if (len == 0) return;
  int colWidth = SCREEN_WIDTH / len;
  int boxSize = min(colWidth - 4, 20);

  display.setTextSize(1);
  int16_t x1, y1;
  uint16_t w, h;
  for (int i = 0; i < len; i++) {
    int colX = i * colWidth;
    int boxX = colX + (colWidth - boxSize) / 2;
    bool completed = (i < simonLetterCount) && (simonInputIndex >= simonLetterEndIndex[i]);

    if (completed) {
      display.fillRect(boxX, yTop, boxSize, boxSize, SH110X_WHITE);
    } else {
      display.drawRect(boxX, yTop, boxSize, boxSize, SH110X_WHITE);
    }

    char letterStr[2] = { word[i], '\0' };
    display.getTextBounds(letterStr, 0, 0, &x1, &y1, &w, &h);
    display.setTextColor(completed ? SH110X_BLACK : SH110X_WHITE);
    display.setCursor(boxX + (boxSize - (int)w) / 2, yTop + (boxSize - (int)h) / 2 - 1);
    display.print(letterStr);
    display.setTextColor(SH110X_WHITE); // restore default
  }
}

void drawSimonScreen() {
  drawSimpleMorseIcon(7, 7, 6, SH110X_WHITE);

  display.setTextSize(1);
  char headerBuf[16];
  const char* status;
  if (simonState == SIMON_READY) {
    status = T(STR_READY);
  } else if (simonState == SIMON_PLAYBACK) {
    status = T(STR_SIMON_WATCH);
  } else if (simonState == SIMON_INPUT) {
    snprintf(headerBuf, sizeof(headerBuf), "%s %d", T(STR_SIMON_ROUND), simonRound);
    status = headerBuf;
  } else if (simonState == SIMON_GAME_OVER) {
    status = T(STR_TETRIS_GAME_OVER); // "Game Over" - generic, already reused elsewhere
  } else {
    status = T(STR_SIMON_COMPLETE);
  }
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(status, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(SCREEN_WIDTH - w - 2, 4);
  display.print(status);
  display.drawLine(0, 14, SCREEN_WIDTH, 14, SH110X_WHITE);

  if (simonState == SIMON_READY) {
    const char* label = T(STR_SIMON_SENTENCE_LABEL);
    display.getTextBounds(label, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 18);
    display.print(label);

    drawWordWithCode(simonWord, 30);

    const char* prompt = T(STR_TETRIS_PRESS_START);
    display.getTextBounds(prompt, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 54);
    display.print(prompt);
    return;
  }

  if (simonState == SIMON_PLAYBACK) {
    return; // the LED/tone blink during the blocking playback is the whole show here
  }

  if (simonState == SIMON_INPUT) {
    const char* prompt = T(STR_SIMON_YOUR_TURN);
    display.getTextBounds(prompt, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 20);
    display.print(prompt);

    drawWordProgress(simonWord, 32);
    return;
  }

  // SIMON_GAME_OVER or SIMON_COMPLETE: the win/lose animation (walrus or celebration) already had
  // its own moment right when this state was entered - see handleSimonButtonPress(), which plays
  // it and holds it on screen for a beat before calling render(). From then on (including any
  // later re-render, e.g. the volume overlay closing), this screen always shows the solution.
  const char* solutionLabel = T(STR_SIMON_SENTENCE_LABEL);
  display.getTextBounds(solutionLabel, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 22);
  display.print(solutionLabel);
  drawWordWithCode(simonWord, 36);
}

// ---------------- Trivia: category selection screen ----------------
void drawTriviaCategoryScreen() {
  drawTriviaIcon(7, 7, 6, SH110X_WHITE);

  display.setTextSize(1);
  const char* headerText = menuLabelForGame(GAME_TRIVIA);
  int16_t hx1, hy1;
  uint16_t hw, hh;
  display.getTextBounds(headerText, 0, 0, &hx1, &hy1, &hw, &hh);
  display.setCursor(SCREEN_WIDTH - hw - 2, 4);
  display.print(headerText);
  display.drawLine(0, 14, SCREEN_WIDTH, 14, SH110X_WHITE);

  int16_t x1, y1;
  uint16_t w, h;
  const int catStrings[TRIVIA_CATEGORY_COUNT] = {STR_CAT_FOOTBALL, STR_CAT_GEOGRAPHY, STR_CAT_SCIENCE, STR_CAT_HISTORY, STR_CAT_GENERAL, STR_CAT_MYTHOLOGY, STR_CAT_ART, STR_CAT_ANIMALS, STR_CAT_RANDOM};
  const char* catText = T(catStrings[triviaCategory]);
  display.setTextSize(2); // fixed size for every category/language - no per-string shrinking, so it never jumps around
  display.getTextBounds(catText, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 26);
  display.print(catText);

  display.setTextSize(1);
  const char* label = T(STR_CHOOSE_CATEGORY);
  display.getTextBounds(label, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 56);
  display.print(label);
}

// ---------------- Trivia: Junior/Curious/Master level selection screen ----------------
void drawTriviaLevelScreen() {
  drawTriviaIcon(7, 7, 6, SH110X_WHITE);

  display.setTextSize(1);
  const char* headerText = menuLabelForGame(GAME_TRIVIA);
  int16_t hx1, hy1;
  uint16_t hw, hh;
  display.getTextBounds(headerText, 0, 0, &hx1, &hy1, &hw, &hh);
  display.setCursor(SCREEN_WIDTH - hw - 2, 4);
  display.print(headerText);
  display.drawLine(0, 14, SCREEN_WIDTH, 14, SH110X_WHITE);

  int16_t x1, y1;
  uint16_t w, h;
  const int levelStrings[TRIVIA_LEVEL_COUNT] = {STR_LEVEL_JUNIOR, STR_LEVEL_CURIOUS, STR_LEVEL_MASTER};
  const char* levelText = T(levelStrings[triviaLevel]);
  display.setTextSize(2); // fixed - same size as the category screen, so switching between them doesn't jump
  display.getTextBounds(levelText, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 26);
  display.print(levelText);

  display.setTextSize(1);
  const char* label = T(STR_CHOOSE_LEVEL);
  display.getTextBounds(label, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 56);
  display.print(label);
}

// ---------------- Trivia: main play screen - question on top, answer revealed below once buzzed in ----------------
void drawTriviaPlayScreen() {
  // top row: icon on the left, a right-aligned label saying which view this is - drawn normally
  // first, then the whole row (edge to edge) gets pixel-inverted, same technique used for the
  // Timer/Chess full-screen invert, just scoped to this one row. Icon and label are nudged up 1px
  // each so the whole row fits within a shorter 14px band without clipping.
  drawTriviaIcon(9, 7, 6, SH110X_WHITE);
  display.setTextSize(1);
  const char* baseWord = triviaShowingAnswers ? T(STR_ANSWERS_LABEL) : T(STR_QUESTION_LABEL);
  char viewLabel[24];
  snprintf(viewLabel, sizeof(viewLabel), "%s %d/%d", baseWord, triviaOrderPosition + 1, triviaQuestionCount);
  int16_t lx1, ly1;
  uint16_t lw, lh;
  display.getTextBounds(viewLabel, 0, 0, &lx1, &ly1, &lw, &lh);
  display.setCursor(SCREEN_WIDTH - lw - 2, 3);
  display.print(viewLabel);
  const int topRowBottom = 14;
  invertScreenRegion(0, 0, SCREEN_WIDTH, topRowBottom);

  // bottom row: an arrow plus a short label on each side, split by a vertical separator - left
  // half describes what turning left does, right half what turning right does. Which two words
  // show depends on whether we're looking at the Question or the Answers of the current question.
  display.setTextSize(1);
  const char* leftLabel = triviaShowingAnswers ? T(STR_TRIVIA_QUES) : T(STR_TRIVIA_PREV);
  const char* rightLabel = triviaShowingAnswers ? T(STR_TRIVIA_NEXT) : T(STR_TRIVIA_ANS);

  const int bottomRowTop = 53;
  const int halfWidth = SCREEN_WIDTH / 2;
  const int arrowSpan = 10; // horizontal space reserved for each arrow glyph
  const int labelGap = 4;
  const int arrowCenterY = bottomRowTop + (SCREEN_HEIGHT - bottomRowTop) / 2;

  int16_t tx1, ty1;
  uint16_t leftTextW, rightTextW, th;
  display.getTextBounds(leftLabel, 0, 0, &tx1, &ty1, &leftTextW, &th);
  display.getTextBounds(rightLabel, 0, 0, &tx1, &ty1, &rightTextW, &th);
  const int textY = arrowCenterY - (int)th / 2 + 1;

  // left group: [arrow] [gap] [label], as a block centered within the left half
  int leftGroupW = arrowSpan + labelGap + (int)leftTextW;
  int leftGroupX = (halfWidth - leftGroupW) / 2;
  drawArrowGlyph(leftGroupX + arrowSpan / 2, arrowCenterY, SH110X_WHITE, false);
  display.setCursor(leftGroupX + arrowSpan + labelGap, textY);
  display.print(leftLabel);

  // right group: [label] [gap] [arrow], as a block centered within the right half
  int rightGroupW = (int)rightTextW + labelGap + arrowSpan;
  int rightGroupX = halfWidth + (halfWidth - rightGroupW) / 2;
  display.setCursor(rightGroupX, textY);
  display.print(rightLabel);
  drawArrowGlyph(rightGroupX + (int)rightTextW + labelGap + arrowSpan / 2, arrowCenterY, SH110X_WHITE, true);

  // separator between the two halves, full height of the row
  display.drawLine(halfWidth, bottomRowTop, halfWidth, SCREEN_HEIGHT - 1, SH110X_WHITE);

  invertScreenRegion(0, bottomRowTop, SCREEN_WIDTH, SCREEN_HEIGHT);

  // middle content area: either the question, or the answer list
  if (!triviaShowingAnswers) {
    // ---- Question view ---- (16 to 53, 37px - up to 4 lines x 9px, 1px margin)
    const int questionTop = 16;
    const int lineHeight = 9;
    int extraLines = wrappedLineCount(triviaQuestionBuf, TRIVIA_QUESTION_CHARS_PER_LINE) - TRIVIA_QUESTION_VISIBLE_LINES;
    int skip = triviaScrollOffset(extraLines, TRIVIA_SCROLL_LINE_MS); // 0 unless it's too long to fit
    drawWrappedText(triviaQuestionBuf, questionTop, TRIVIA_QUESTION_VISIBLE_LINES, lineHeight, skip);
  } else {
    // ---- Answers view: single column, full width - each answer gets roughly double the room
    // it had in the old 2x2 grid, with a small bullet in front of each in place of a letter prefix,
    // and a 1px gap between rows. Only the highlight changes on buzz-in; the list itself is always
    // visible. Same start position as the question view. ----
    const int answersTop = 16;
    const int rowHeight = 7;
    const int rowGap = 2;
    const int rowPitch = rowHeight + rowGap;
    const int leftMargin = 8;
    const int rightMargin = 2;
    const int rowWidth = SCREEN_WIDTH - leftMargin - rightMargin;

    for (int i = 0; i < triviaOptionCount; i++) {
      int y = answersTop + i * rowPitch;

      // too-long answers scroll sideways (one character per step) rather than being cut off
      int optLen = strlen(triviaOptionBuf[i]);
      int charOffset = triviaScrollOffset(optLen - TRIVIA_ANSWER_VISIBLE_CHARS, TRIVIA_SCROLL_CHAR_MS);
      char displayText[40];
      strncpy(displayText, triviaOptionBuf[i] + charOffset, sizeof(displayText) - 1);
      displayText[sizeof(displayText) - 1] = '\0';

      int16_t tx1, ty1;
      uint16_t tw, th;
      display.getTextBounds(displayText, 0, 0, &tx1, &ty1, &tw, &th);
      int len = strlen(displayText);
      while (tw > (uint16_t)rowWidth && len > 0) {
        len--;
        displayText[len] = '\0';
        display.getTextBounds(displayText, 0, 0, &tx1, &ty1, &tw, &th);
      }

      bool highlight = triviaAnswerVisible && (i == triviaCorrectOption);
      if (highlight) {
        display.fillRect(leftMargin - 5, y - 1, rowWidth + 5, rowHeight + 2, SH110X_WHITE);
        display.setTextColor(SH110X_BLACK);
      }

      display.fillCircle(leftMargin - 4, y + 2, 1, highlight ? SH110X_BLACK : SH110X_WHITE);
      display.setCursor(leftMargin, y);
      display.print(displayText);

      if (highlight) {
        display.setTextColor(SH110X_WHITE);
      }
    }
  }
}

// ---------------- Menu screen: pick a game (or Settings), scrolling list, icon + label per row ----------------
// ---------------- Menu: translated label for a given game id (or Settings) - looked up by id rather
// than by position, so it stays correct no matter what order MENU_ORDER is in (Config Mode can
// change that order at runtime) ----------------
const char* menuLabelForGame(int gameId) {
  switch (gameId) {
    case GAME_TIMER:      return T(STR_MENU_TIMER);
    case GAME_TOOTHBRUSH: return T(STR_MENU_TOOTHBRUSH);
    case GAME_TETRIS:     return T(STR_MENU_TETRIS);
    case GAME_STARDROP:   return T(STR_MENU_STARDROP);
    case GAME_SNAKE:      return T(STR_MENU_SNAKE);
    case GAME_SIMPLEMORSE: return T(STR_MENU_SIMPLEMORSE);
    case GAME_DICE:       return T(STR_MENU_DICE);
    case GAME_REFLEX:     return T(STR_MENU_REFLEX);
    case GAME_CHESS:      return T(STR_MENU_CHESS);
    case GAME_HOTPOTATO:  return T(STR_MENU_HOTPOTATO);
    case GAME_TURNORDER:  return T(STR_MENU_TURNORDER);
    case GAME_STOPWATCH:  return T(STR_MENU_STOPWATCH);
    case GAME_TRIVIA:     return T(STR_MENU_TRIVIA);
    case MENU_SETTINGS:   return T(STR_MENU_SETTINGS);
    case MENU_INSTRUCTIONS: return T(STR_MENU_INSTRUCTIONS);
    default:               return "";
  }
}

void drawMenuScreen() {
  drawVolumeIconAt(2, 1);

  display.setTextSize(1);
  const char* title = T(STR_SELECT_GAME);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(title, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 2);
  display.print(title);

  const int VISIBLE_ROWS = 3;
  const int ROW_HEIGHT = 16;
  const int LIST_TOP = 14;

  int maxScroll = max(0, visibleMenuCount - VISIBLE_ROWS);
  int scrollTop = constrain(menuSelection - 1, 0, maxScroll);

  for (int row = 0; row < VISIBLE_ROWS; row++) {
    int i = scrollTop + row;
    if (i >= visibleMenuCount) break;

    int rowTop = LIST_TOP + row * ROW_HEIGHT;
    int rowCenter = rowTop + ROW_HEIGHT / 2;
    bool selected = (i == menuSelection);

    if (selected) {
      display.fillRect(10, rowTop, 108, ROW_HEIGHT - 2, SH110X_WHITE);
    }
    uint16_t fg = selected ? SH110X_BLACK : SH110X_WHITE;

    drawGameIcon(visibleMenuOrder[i], 24, rowCenter, 6, fg);

    display.setTextColor(fg);
    display.setCursor(40, rowCenter - 4);
    display.print(menuLabelForGame(visibleMenuOrder[i]));
  }

  display.setTextColor(SH110X_WHITE); // restore default in case anything draws after this
}

// ---------------- Language screen: pick a language, shown natively (not translated) so it's always readable ----------------
// ---------------- "Go back to menu?" dialog: centered title, Yes/No side by side, Yes is the default ----------------
void drawLeaveConfirmScreen() {
  display.setTextSize(1);
  int16_t x1, y1;
  uint16_t w, h;

  const char* title = T(STR_LEAVE_CONFIRM_TITLE);
  display.getTextBounds(title, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 20);
  display.print(title);

  const int boxW = 40, boxH = 20, gap = 10;
  const int pairW = boxW * 2 + gap;
  const int startX = (SCREEN_WIDTH - pairW) / 2;
  const int boxY = 38;

  const char* labels[2] = {T(STR_YES), T(STR_NO)};
  for (int i = 0; i < 2; i++) {
    int boxX = startX + i * (boxW + gap);
    bool selected = (leaveConfirmSelection == i);

    if (selected) {
      display.fillRect(boxX, boxY, boxW, boxH, SH110X_WHITE);
    } else {
      display.drawRect(boxX, boxY, boxW, boxH, SH110X_WHITE);
    }

    display.getTextBounds(labels[i], 0, 0, &x1, &y1, &w, &h);
    display.setTextColor(selected ? SH110X_BLACK : SH110X_WHITE);
    display.setCursor(boxX + (boxW - w) / 2, boxY + (boxH - h) / 2 - 1);
    display.print(labels[i]);
  }
  display.setTextColor(SH110X_WHITE);
}

// ---------------- Instructions screen: the global controls, always reachable from the top of the menu ----------------
void drawInstructionsScreen() {
  drawInstructionsIcon(7, 7, 6, SH110X_WHITE);
  display.drawLine(0, 14, SCREEN_WIDTH, 14, SH110X_WHITE);

  display.setTextSize(1);
  int16_t x1, y1;
  uint16_t w, h;

  const char* lines[3] = {T(STR_INSTR_MENU), T(STR_INSTR_VOLUME), T(STR_INSTR_SETTINGS)};
  int ys[3] = {22, 36, 50};
  for (int i = 0; i < 3; i++) {
    display.getTextBounds(lines[i], 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, ys[i]);
    display.print(lines[i]);
  }
}

void drawLanguageScreen() {
  drawLanguageIcon(7, 7, 6, SH110X_WHITE);
  display.drawLine(0, 14, SCREEN_WIDTH, 14, SH110X_WHITE);

  drawCenteredAutoFit(LANGUAGE_NAMES[currentLanguage], 22, 3);

  display.setTextSize(1);
  const char* label = T(STR_CHOOSE_LANGUAGE);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(label, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 56);
  display.print(label);
}

// ---------------- Settings screen: pick Language or Timer Step, two-row list ----------------
void drawSettingsScreen() {
  display.setTextSize(1);
  const char* title = T(STR_SETTINGS_TITLE);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(title, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 2);
  display.print(title);

  const char* labels[SETTINGS_COUNT] = {T(STR_MENU_LANGUAGE), T(STR_MENU_TIMER_STEP), T(STR_MENU_CONFIG_MODE)};
  int rowCenters[SETTINGS_COUNT] = {22, 38, 54};

  for (int i = 0; i < SETTINGS_COUNT; i++) {
    bool selected = (i == settingsSelection);
    int rowTop = rowCenters[i] - 7;

    if (selected) {
      display.fillRect(10, rowTop, 108, 14, SH110X_WHITE);
    }
    uint16_t fg = selected ? SH110X_BLACK : SH110X_WHITE;

    if (i == SETTINGS_LANGUAGE) drawLanguageIcon(24, rowCenters[i], 5, fg);
    else if (i == SETTINGS_TIMER_STEP) drawTimerStepIcon(24, rowCenters[i], 5, fg);
    else drawConfigModeIcon(24, rowCenters[i], 5, fg);

    display.setTextColor(fg);
    display.setCursor(40, rowCenters[i] - 3);
    display.print(labels[i]);
  }

  display.setTextColor(SH110X_WHITE); // restore default in case anything draws after this
}

// ---------------- Timer Step screen: pick the encoder's seconds-per-detent for the Timer game ----------------
void drawTimerStepScreen() {
  drawTimerStepIcon(7, 7, 6, SH110X_WHITE);
  display.drawLine(0, 14, SCREEN_WIDTH, 14, SH110X_WHITE);

  char buf[6];
  long stepValue = TIMER_STEP_OPTIONS[currentTimerStepIndex];
  const char* displayStr;
  if (stepValue == TIMER_STEP_AUTO) {
    displayStr = T(STR_AUTO);
  } else {
    snprintf(buf, sizeof(buf), "%lds", stepValue);
    displayStr = buf;
  }
  drawCenteredAutoFit(displayStr, 22, 4);

  display.setTextSize(1);
  const char* label = T(STR_CHOOSE_TIMER_STEP);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(label, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 56);
  display.print(label);
}

// ---------------- Game icons ----------------
void drawTimerIcon(int cx, int cy, int r, uint16_t color) {
  display.drawCircle(cx, cy, r, color);
  display.drawLine(cx, cy, cx, cy - r + 2, color);     // minute hand
  display.drawLine(cx, cy, cx + r - 3, cy, color);      // hour hand
}

void drawReflexIcon(int cx, int cy, int r, uint16_t color) {
  display.drawCircle(cx, cy, r, color);   // target ring
  display.fillCircle(cx, cy, 2, color);   // bullseye center
}

void drawDiceIcon(int cx, int cy, int r, uint16_t color) {
  display.drawRoundRect(cx - r, cy - r, r * 2, r * 2, 3, color);
  int off = r / 2;
  int pr = max(1, r / 5);
  display.fillCircle(cx - off, cy - off, pr, color);
  display.fillCircle(cx + off, cy - off, pr, color);
  display.fillCircle(cx - off, cy + off, pr, color);
  display.fillCircle(cx + off, cy + off, pr, color);
  display.fillCircle(cx, cy, pr, color);
}

void drawChessIcon(int cx, int cy, int r, uint16_t color) {
  int off = r / 2;
  display.drawCircle(cx - off, cy, r - 2, color); // two overlapping clock faces
  display.drawCircle(cx + off, cy, r - 2, color);
}

void drawHotPotatoIcon(int cx, int cy, int r, uint16_t color) {
  int bodyR = max(3, r / 2);   // smaller body relative to r than before
  int bodyCy = cy + r / 6;     // nudge the body down a touch to make room for the longer fuse above

  display.drawCircle(cx, bodyCy, bodyR, color); // bomb body

  // fuse: longer relative to the body, two segments curving up and to the right
  int fuseLen = max(6, (r * 7) / 10);
  int fx0 = cx, fy0 = bodyCy - bodyR;
  int fx1 = cx + max(2, bodyR / 2), fy1 = fy0 - fuseLen;
  int fx2 = fx1 + max(2, bodyR / 2), fy2 = fy1 + max(2, bodyR / 2);
  display.drawLine(fx0, fy0, fx1, fy1, color);
  display.drawLine(fx1, fy1, fx2, fy2, color);

  // spark at the fuse tip
  int s = max(2, r / 7);
  display.drawLine(fx2 - s, fy2, fx2 + s, fy2, color);
  display.drawLine(fx2, fy2 - s, fx2, fy2 + s, color);
}

// ---------------- Rotate/refresh icon: a ~270-degree circular arc with a small arrowhead at its
// open end (tangent to the direction of travel), used by Pass the Bomb's "rotate to shuffle
// again" hint row - built from connected line segments, same technique as drawSoundArc/the WiFi
// icon, since Adafruit_GFX has no arc primitive. ----------------
void drawRotateIcon(int cx, int cy, int r, uint16_t color) {
  const int SEGMENTS = 10;
  const float startAngle = -100.0 * PI / 180.0;
  const float endAngle = 170.0 * PI / 180.0; // sweeps ~270 degrees

  int prevX = cx + (int)(r * cos(startAngle));
  int prevY = cy + (int)(r * sin(startAngle));
  for (int i = 1; i <= SEGMENTS; i++) {
    float t = startAngle + (endAngle - startAngle) * i / SEGMENTS;
    int x = cx + (int)(r * cos(t));
    int y = cy + (int)(r * sin(t));
    display.drawLine(prevX, prevY, x, y, color);
    prevX = x;
    prevY = y;
  }

  // arrowhead at the sweep's end, oriented along the tangent (direction of travel)
  float tangentAngle = endAngle + PI / 2.0;
  float tx = cos(tangentAngle), ty = sin(tangentAngle);
  float nx = -ty, ny = tx; // perpendicular to the tangent, for the arrowhead's base
  float size = max(3, r - 1);
  int p1x = prevX + (int)(tx * size),        p1y = prevY + (int)(ty * size);
  int p2x = prevX - (int)(nx * size * 0.6),  p2y = prevY - (int)(ny * size * 0.6);
  int p3x = prevX + (int)(nx * size * 0.6),  p3y = prevY + (int)(ny * size * 0.6);
  display.fillTriangle(p1x, p1y, p2x, p2y, p3x, p3y, color);
}

void drawTurnOrderIcon(int cx, int cy, int r, uint16_t color) {
  // simple spinner: circle with a pointer
  display.drawCircle(cx, cy, r, color);
  display.drawLine(cx, cy, cx, cy - r, color);
  display.drawLine(cx, cy - r, cx - 2, cy - r + 3, color);
  display.drawLine(cx, cy - r, cx + 2, cy - r + 3, color);
}

void drawStopwatchIcon(int cx, int cy, int r, uint16_t color) {
  display.drawCircle(cx, cy, r - 2, color);
  display.fillRect(cx - 2, cy - r - 2, 4, 2, color); // top button
  display.drawLine(cx, cy, cx, cy - r + 3, color);   // hand
}

void drawToothbrushIcon(int cx, int cy, int r, uint16_t color) {
  // angled handle, from lower-left to upper-right, with a few short bristle ticks at the top
  int hx0 = cx - r, hy0 = cy + r;
  int hx1 = cx + r / 3, hy1 = cy - r / 3;
  display.drawLine(hx0, hy0, hx1, hy1, color); // handle
  for (int i = -1; i <= 1; i++) {
    display.drawLine(hx1 + i * 2, hy1 - 1, hx1 + i * 2 - 2, hy1 - r / 2, color); // bristles
  }
}

// small S-tetromino silhouette - instantly reads as "Tetris" at a glance
void drawTetrisIcon(int cx, int cy, int r, uint16_t color) {
  int s = max(2, r / 2); // block size
  display.fillRect(cx, cy - s, s, s, color);
  display.fillRect(cx + s, cy - s, s, s, color);
  display.fillRect(cx - s, cy, s, s, color);
  display.fillRect(cx, cy, s, s, color);
}

// small 4-point sparkle - menu icon for Star Drop
void drawStarDropIcon(int cx, int cy, int r, uint16_t color) {
  display.drawLine(cx - r, cy, cx + r, cy, color);
  display.drawLine(cx, cy - r, cx, cy + r, color);
  int d = max(1, r / 2);
  display.drawLine(cx - d, cy - d, cx + d, cy + d, color);
  display.drawLine(cx - d, cy + d, cx + d, cy - d, color);
}

// ---------------- Snake icon: a zigzag body with a small round head at one end ----------------
void drawSnakeIcon(int cx, int cy, int r, uint16_t color) {
  display.drawLine(cx - r, cy + r / 2, cx - r / 3, cy - r / 2, color);
  display.drawLine(cx - r / 3, cy - r / 2, cx + r / 3, cy + r / 2, color);
  display.drawLine(cx + r / 3, cy + r / 2, cx + r, cy - r / 2, color);
  int hr = max(1, r / 4);
  display.fillCircle(cx + r, cy - r / 2, hr, color);
}

// ---------------- SimpleMorse menu icon: a tiny walrus - reuses drawWalrusFace, just always the
// non-wounded version and at icon scale ----------------
void drawSimpleMorseIcon(int cx, int cy, int r, uint16_t color) {
  drawWalrusFace(cx, cy - 2, max(3, r - 2), false, color);
}

// ---------------- Star Drop: falling-object glyphs, drawn small (radius ~3px) so several can be
// on screen at once. type: 0=star, 1=planet, 2=moon ----------------
void drawStarDropObjectIcon(int cx, int cy, int type, uint16_t color) {
  if (type == 0) { // star: small 4-point sparkle
    display.drawLine(cx - 3, cy, cx + 3, cy, color);
    display.drawLine(cx, cy - 3, cx, cy + 3, color);
    display.drawLine(cx - 2, cy - 2, cx + 2, cy + 2, color);
    display.drawLine(cx - 2, cy + 2, cx + 2, cy - 2, color);
  } else if (type == 1) { // planet: filled body with a ring across it
    display.fillCircle(cx, cy, 3, color);
    display.drawLine(cx - 5, cy + 1, cx + 5, cy - 1, color);
  } else { // moon: crescent (filled circle with an offset circle "bitten" out of it)
    display.fillCircle(cx, cy, 3, color);
    display.fillCircle(cx + 2, cy - 1, 3, SH110X_BLACK);
  }
}

// ---------------- Star Drop: the telescope catcher, drawn at the bottom of the active lane ----------------
void drawStarDropTelescopeIcon(int laneCenterX, int baseY, uint16_t color) {
  display.drawLine(laneCenterX - 6, baseY, laneCenterX + 2, baseY - 8, color);
  display.drawLine(laneCenterX - 4, baseY, laneCenterX + 4, baseY - 8, color);
  display.drawLine(laneCenterX - 6, baseY, laneCenterX - 4, baseY, color);
  display.drawLine(laneCenterX + 2, baseY - 8, laneCenterX + 4, baseY - 8, color);
}

// ---------------- Display: inverts every pixel within a rectangular region - same technique as
// the Timer/Chess full-screen invert, just scoped to an arbitrary box instead of the whole screen ----------------
void invertScreenRegion(int x0, int y0, int x1, int y1) {
  for (int yy = y0; yy < y1; yy++) {
    for (int xx = x0; xx < x1; xx++) {
      uint16_t px = display.getPixel(xx, yy);
      display.drawPixel(xx, yy, px ? SH110X_BLACK : SH110X_WHITE);
    }
  }
}

void drawTriviaIcon(int cx, int cy, int r, uint16_t color) {
  display.setTextColor(color);
  display.setTextSize(1);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds("?", 0, 0, &x1, &y1, &w, &h);
  display.setCursor(cx - w / 2, cy - h / 2 - y1);
  display.print("?");
}

// single-direction arrow glyph, for the trivia play screen's bottom row (left = back, right = forward
// through the Question <-> Answers <-> next Question sequence)
void drawArrowGlyph(int cx, int cy, uint16_t color, bool pointRight) {
  if (pointRight) {
    display.fillTriangle(cx - 3, cy - 4, cx - 3, cy + 4, cx + 4, cy, color);
  } else {
    display.fillTriangle(cx + 3, cy - 4, cx + 3, cy + 4, cx - 4, cy, color);
  }
}

void drawLanguageIcon(int cx, int cy, int r, uint16_t color) {
  display.drawCircle(cx, cy, r, color);           // globe outline
  display.drawLine(cx - r, cy, cx + r, cy, color); // equator
  display.drawLine(cx, cy - r, cx, cy + r, color); // meridian
}

void drawSettingsIcon(int cx, int cy, int r, uint16_t color) {
  display.drawCircle(cx, cy, r - 2, color); // gear body
  display.drawLine(cx, cy - r, cx, cy - r + 3, color);     // teeth, N/E/S/W
  display.drawLine(cx, cy + r - 3, cx, cy + r, color);
  display.drawLine(cx - r, cy, cx - r + 3, cy, color);
  display.drawLine(cx + r - 3, cy, cx + r, cy, color);
  display.fillCircle(cx, cy, 1, color); // center dot
}

// simple WiFi glyph: two nested arcs plus a dot, drawn with short line segments (no arc primitive
// available on this display library)
// ---------------- Remote Config icon: the classic WiFi glyph - a small dot plus two thick
// concentric arcs opening upward. Deliberately reuses the exact same arc technique as the volume
// icon's sound-wave arcs (100-degree span, drawn at two adjacent radii for a visibly thick
// stroke) just rotated 90 degrees to open upward instead of rightward, so the two icons read as
// clearly related. Proportions were checked in a scaled-up preview before settling on them - a
// tighter arc gap than this reads as one blob rather than two distinct rings at this icon size. ----------------
void drawConfigModeIcon(int cx, int cy, int r, uint16_t color) {
  int baseY = cy + r;
  display.fillCircle(cx, baseY, 1, color);

  const float startAngle = -140.0 * PI / 180.0; // 100-degree span (same width as drawSoundArc), rotated to open upward
  const float endAngle = -40.0 * PI / 180.0;
  const int SEGMENTS = 8;
  int radii[2] = {2, max(4, r + 1)};
  for (int ring = 0; ring < 2; ring++) {
    int baseR = radii[ring];
    for (int rr = baseR; rr <= baseR + 1; rr++) { // two adjacent radii = a thick stroke
      int prevX = cx + (int)(rr * cos(startAngle));
      int prevY = baseY + (int)(rr * sin(startAngle));
      for (int i = 1; i <= SEGMENTS; i++) {
        float t = startAngle + (endAngle - startAngle) * i / SEGMENTS;
        int x = cx + (int)(rr * cos(t));
        int y = baseY + (int)(rr * sin(t));
        display.drawLine(prevX, prevY, x, y, color);
        prevX = x;
        prevY = y;
      }
    }
  }
}

void drawTimerStepIcon(int cx, int cy, int r, uint16_t color) {
  int bottom = cy + r;
  int bw = max(2, r / 3);
  display.fillRect(cx - r, bottom - r / 2, bw, r / 2, color);              // short step
  display.fillRect(cx - bw / 2, bottom - r, bw, r, color);                 // medium step
  display.fillRect(cx + r - bw, bottom - (r * 3) / 2, bw, (r * 3) / 2, color); // tall step
}

// dispatches to the right icon function by menu entry - used by the scrolling menu list
void drawInstructionsIcon(int cx, int cy, int r, uint16_t color) {
  display.drawCircle(cx, cy, r, color);
  display.fillCircle(cx, cy - r / 2, 1, color);            // dot of the "i"
  display.drawLine(cx, cy - r / 5, cx, cy + r / 2, color); // stem of the "i"
}

void drawGameIcon(int gameIndex, int cx, int cy, int r, uint16_t color) {
  if (gameIndex == GAME_TIMER) drawTimerIcon(cx, cy, r, color);
  else if (gameIndex == GAME_REFLEX) drawReflexIcon(cx, cy, r, color);
  else if (gameIndex == GAME_DICE) drawDiceIcon(cx, cy, r, color);
  else if (gameIndex == GAME_CHESS) drawChessIcon(cx, cy, r, color);
  else if (gameIndex == GAME_HOTPOTATO) drawHotPotatoIcon(cx, cy, r, color);
  else if (gameIndex == GAME_TURNORDER) drawTurnOrderIcon(cx, cy, r, color);
  else if (gameIndex == GAME_STOPWATCH) drawStopwatchIcon(cx, cy, r, color);
  else if (gameIndex == GAME_TRIVIA) drawTriviaIcon(cx, cy, r, color);
  else if (gameIndex == GAME_TOOTHBRUSH) drawToothbrushIcon(cx, cy, r, color);
  else if (gameIndex == GAME_TETRIS) drawTetrisIcon(cx, cy, r, color);
  else if (gameIndex == GAME_STARDROP) drawStarDropIcon(cx, cy, r, color);
  else if (gameIndex == GAME_SNAKE) drawSnakeIcon(cx, cy, r, color);
  else if (gameIndex == GAME_SIMPLEMORSE) drawSimpleMorseIcon(cx, cy, r, color);
  else if (gameIndex == MENU_INSTRUCTIONS) drawInstructionsIcon(cx, cy, r, color);
  else drawSettingsIcon(cx, cy, r, color); // MENU_SETTINGS
}

// ---------------- Volume/mute icon: speaker box, cone pointing right, then 0-2 wave marks (or an
// "x" if muted) - same size and style as the icon that used to sit at the top-right of every game
// screen, now only shown at the top-left of the menu list ----------------
// ---------------- Draws one curved "sound wave" arc, centered at (cx,cy), opening to the right -
// approximated with line segments (Adafruit_GFX has no arc primitive), and drawn at two adjacent
// radii to read as a clearly thick stroke rather than a hairline. Shared by both the small menu
// icon and the big full-screen overlay, just called at different radii. ----------------
void drawSoundArc(int cx, int cy, int r, uint16_t color) {
  const float startAngle = -50.0 * PI / 180.0;
  const float endAngle = 50.0 * PI / 180.0;
  const int SEGMENTS = 8;
  for (int rr = r; rr <= r + 1; rr++) { // two adjacent radii = a thick stroke
    int prevX = cx + (int)(rr * cos(startAngle));
    int prevY = cy + (int)(rr * sin(startAngle));
    for (int i = 1; i <= SEGMENTS; i++) {
      float t = startAngle + (endAngle - startAngle) * i / SEGMENTS;
      int x = cx + (int)(rr * cos(t));
      int y = cy + (int)(rr * sin(t));
      display.drawLine(prevX, prevY, x, y, color);
      prevX = x;
      prevY = y;
    }
  }
}

// ---------------- Draws a thick X mark, centered at (cx,cy) with the given half-size ----------------
void drawThickX(int cx, int cy, int size, uint16_t color) {
  display.drawLine(cx - size, cy - size, cx + size, cy + size, color);
  display.drawLine(cx - size + 1, cy - size, cx + size + 1, cy + size, color);
  display.drawLine(cx - size, cy + size, cx + size, cy - size, color);
  display.drawLine(cx - size + 1, cy + size, cx + size + 1, cy - size, color);
}

// ---------------- Speaker shape: a rounded box flush against a steep flare (a 6-sided trapezoid
// with its two outer corners rounded), vertically centered at cy. The box and flare share an edge
// (same height, no point/notch), and the flare's outer corners are rounded by inset polygon
// vertices plus two tangent filled circles - the standard rounded-corner construction, just built
// from the triangle/circle primitives Adafruit_GFX actually has (no arbitrary polygon fill).
// Returns the x coordinate of the flare's flat outer edge, for positioning arcs/X next to it. ----------------
int drawSpeakerShape(int boxLeftX, int cy, int boxSize, int boxRadius, int coneLen, int wideHalfH, int cornerR, uint16_t color) {
  int boxY = cy - boxSize / 2;
  display.fillRoundRect(boxLeftX, boxY, boxSize, boxSize, boxRadius, color);

  int narrowX = boxLeftX + boxSize;
  int narrowHalfH = boxSize / 2;
  int wideX = narrowX + coneLen;

  int p0x = narrowX,          p0y = cy - narrowHalfH;
  int p1x = narrowX,          p1y = cy + narrowHalfH;
  int p2x = wideX - cornerR,  p2y = cy + wideHalfH;
  int p3x = wideX,            p3y = cy + wideHalfH - cornerR;
  int p4x = wideX,            p4y = cy - wideHalfH + cornerR;
  int p5x = wideX - cornerR,  p5y = cy - wideHalfH;

  display.fillTriangle(p0x, p0y, p1x, p1y, p2x, p2y, color);
  display.fillTriangle(p0x, p0y, p2x, p2y, p3x, p3y, color);
  display.fillTriangle(p0x, p0y, p3x, p3y, p4x, p4y, color);
  display.fillTriangle(p0x, p0y, p4x, p4y, p5x, p5y, color);

  display.fillCircle(wideX - cornerR, cy - wideHalfH + cornerR, cornerR, color);
  display.fillCircle(wideX - cornerR, cy + wideHalfH - cornerR, cornerR, color);

  return wideX;
}

// ---------------- Small volume icon (menu top-left): the exact original design, unmirrored -
// same relative geometry as the icon that used to sit at the top-right of every game screen,
// just repositioned here instead of re-oriented ----------------
void drawVolumeIconAt(int x, int y) {
  int markAnchor = x + 8;
  int apexY = y + 5;

  display.fillRect(markAnchor - 3, y + 2, 3, 6, SH110X_WHITE);
  display.fillTriangle(markAnchor - 3, y, markAnchor - 3, y + 10, x, apexY, SH110X_WHITE);

  display.setTextSize(1);
  if (volumeLevel == 0) {
    display.setCursor(markAnchor + 3, y + 1);
    display.print("x");
  } else if (volumeLevel == 1) {
    display.setCursor(markAnchor + 2, y + 2);
    display.print(")");
  } else {
    display.setCursor(markAnchor + 2, y + 2);
    display.print(")");
    display.setCursor(markAnchor + 6, y + 2);
    display.print(")");
  }
}

// ---------------- Full-screen volume indicator, shown live while holding the encoder and
// turning it - a big version of the same speaker+waves icon, so it's obvious what's changing.
// Replaces the normal screen only while actively adjusting; the caller restores the normal
// screen (a plain render()) once the encoder is released. ----------------
void drawVolumeOverlay() {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);

  int cy = SCREEN_HEIGHT / 2;
  int wideX = drawSpeakerShape(26, cy, 22, 5, 22, 26, 5, SH110X_WHITE);

  if (volumeLevel == 0) {
    drawThickX(wideX + 24, cy, 16, SH110X_WHITE);
  } else if (volumeLevel == 1) {
    drawSoundArc(wideX, cy, 20, SH110X_WHITE);
  } else {
    drawSoundArc(wideX, cy, 16, SH110X_WHITE);
    drawSoundArc(wideX, cy, 32, SH110X_WHITE);
  }

  display.display();
}
