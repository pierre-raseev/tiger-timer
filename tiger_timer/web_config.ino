// ======================================================================================
// Config Mode: WiFi access point + on-device web server for editing settings from a phone
// or laptop browser, without a USB cable. Entered only from Settings > Config Mode, and
// fully torn down (WiFi off) when you press the button to exit - normal gameplay never
// pays any WiFi battery cost. See /data/config.html for the actual page this serves.
//
// Requires the "ArduinoJson" library, version 7.x specifically (Library Manager - search
// "ArduinoJson" by Benoit Blanchon, install the latest 7.x - the JsonDocument API used below
// is v7-only and won't compile against a 6.x install).
// ======================================================================================

WebServer configServer(80);
DNSServer configDns;
const byte DNS_PORT = 53;

const char* CONFIG_AP_SSID = "TigerTimer-Config"; // open network - no password, so phones can captive-portal straight in
String configApIpStr; // set in enterConfigMode(), used to build the captive-portal redirect target

// Same category/level naming used by the trivia game itself (triviaFilePath) - kept in sync manually
// since they're in different tabs of the same sketch.
const char* CONFIG_TRIVIA_CATEGORIES[TRIVIA_REAL_CATEGORY_COUNT] = {"sports", "geography", "science", "history", "general", "mythology", "art", "animals"};
const char* CONFIG_TRIVIA_LEVELS[TRIVIA_LEVEL_COUNT] = {"junior", "curious", "master"};
const char* CONFIG_TRIVIA_FIELD_NAMES[15] = {
  "en_q", "en_correct", "en_wrong1", "en_wrong2", "en_wrong3",
  "fr_q", "fr_correct", "fr_wrong1", "fr_wrong2", "fr_wrong3",
  "es_q", "es_correct", "es_wrong1", "es_wrong2", "es_wrong3"
};

// ---------------- Menu order: load/save/validate ----------------
void loadMenuOrderFromPrefs() {
  uint8_t stored[MENU_COUNT];
  size_t got = prefs.getBytes("menuOrd", stored, MENU_COUNT);

  bool valid = (got == MENU_COUNT);
  if (valid) {
    bool seen[MENU_COUNT];
    for (int i = 0; i < MENU_COUNT; i++) seen[i] = false;
    for (int i = 0; i < MENU_COUNT && valid; i++) {
      int pos = -1;
      for (int j = 0; j < MENU_COUNT; j++) {
        if (DEFAULT_MENU_ORDER[j] == (int)stored[i]) { pos = j; break; }
      }
      if (pos < 0 || seen[pos]) { valid = false; break; } // not a recognized id, or a duplicate
      seen[pos] = true;
    }
  }

  for (int i = 0; i < MENU_COUNT; i++) {
    MENU_ORDER[i] = valid ? (int)stored[i] : DEFAULT_MENU_ORDER[i];
  }
}

void saveMenuOrderToPrefs() {
  uint8_t toSave[MENU_COUNT];
  for (int i = 0; i < MENU_COUNT; i++) toSave[i] = (uint8_t)MENU_ORDER[i];
  prefs.putBytes("menuOrd", toSave, MENU_COUNT);
}

// ---------------- Per-game enable/disable: load/save/apply ----------------
void loadGameEnabledFromPrefs() {
  uint8_t stored[GAME_COUNT];
  size_t got = prefs.getBytes("gameEn", stored, GAME_COUNT);
  for (int i = 0; i < GAME_COUNT; i++) {
    gameEnabled[i] = (got == GAME_COUNT) ? (stored[i] != 0) : true; // default: everything enabled
  }
}

void saveGameEnabledToPrefs() {
  uint8_t toSave[GAME_COUNT];
  for (int i = 0; i < GAME_COUNT; i++) toSave[i] = gameEnabled[i] ? 1 : 0;
  prefs.putBytes("gameEn", toSave, GAME_COUNT);
}

// ---------------- Pass the Bomb categories: load/save/apply - same pattern as game enable/disable ----------------
void loadHpCategoryEnabledFromPrefs() {
  uint8_t stored[HP_CATEGORY_COUNT];
  size_t got = prefs.getBytes("hpCatEn", stored, HP_CATEGORY_COUNT);
  for (int i = 0; i < HP_CATEGORY_COUNT; i++) {
    hpCategoryEnabled[i] = (got == HP_CATEGORY_COUNT) ? (stored[i] != 0) : true; // default: everything enabled
  }
}

void saveHpCategoryEnabledToPrefs() {
  uint8_t toSave[HP_CATEGORY_COUNT];
  for (int i = 0; i < HP_CATEGORY_COUNT; i++) toSave[i] = hpCategoryEnabled[i] ? 1 : 0;
  prefs.putBytes("hpCatEn", toSave, HP_CATEGORY_COUNT);
}

// Rebuilds visibleMenuOrder from MENU_ORDER + gameEnabled - call this after either changes.
// Settings is never filtered out. Falls back to showing just Settings if somehow everything else
// ended up disabled, so the device is never left with an empty, unusable menu.
void rebuildVisibleMenu() {
  visibleMenuCount = 0;
  for (int i = 0; i < MENU_COUNT; i++) {
    int id = MENU_ORDER[i];
    bool show = (id == MENU_SETTINGS) || (id == MENU_INSTRUCTIONS) || (id >= 0 && id < GAME_COUNT && gameEnabled[id]);
    if (show) visibleMenuOrder[visibleMenuCount++] = id;
  }
  if (visibleMenuCount == 0) {
    visibleMenuOrder[0] = MENU_SETTINGS;
    visibleMenuCount = 1;
  }
}

// ---------------- Text encoding: device files are CP437, the web page is UTF-8 (see cp437_utf8.h) ----------------
String cp437ToUtf8(const String& in) {
  String out;
  out.reserve(in.length() + 16);
  char buf[4];
  for (size_t i = 0; i < in.length(); i++) {
    uint8_t b = (uint8_t)in[i];
    uint32_t cp = (b < 0x80) ? b : CP437_HIGH_TO_UNICODE[b - 0x80];
    int n = tt_utf8Encode(cp, buf);
    for (int k = 0; k < n; k++) out += buf[k];
  }
  return out;
}

String utf8ToCp437(const String& in) {
  String out;
  out.reserve(in.length());
  const uint8_t* s = (const uint8_t*)in.c_str();
  size_t len = in.length();
  size_t i = 0;
  char buf[3];
  while (i < len) {
    uint32_t cp;
    i += tt_utf8Decode(s, len, i, &cp);
    int n = tt_unicodeToCp437(cp, buf);
    for (int k = 0; k < n; k++) out += buf[k];
  }
  return out;
}

// ---------------- CSV helpers ----------------
// Strips characters that would break the sketch's plain (unquoted) CSV format. The on-device
// trivia parser splits purely on commas and lines, so commas/newlines inside a field are not
// supported - sanitize rather than corrupt the file if the web page sends any through.
String csvSanitizeField(const String& in) {
  String out;
  out.reserve(in.length());
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == ',' || c == '\n' || c == '\r') continue;
    out += c;
  }
  out.trim();
  return out;
}

// ---------------- Route: GET / - serves the config page itself from LittleFS ----------------
void handleConfigRoot() {
  File f = LittleFS.open("/config.html", "r");
  if (!f) {
    configServer.send(500, "text/plain", "config.html not found on LittleFS. Did you upload the data folder?");
    return;
  }
  configServer.streamFile(f, "text/html");
  f.close();
}

// ---------------- Route: GET /api/config - current menu order, startup choice, default timer, and
// the static list of games/categories/levels the page needs to build its UI ----------------
void handleGetConfig() {
  JsonDocument doc;

  JsonArray menu = doc["menuOrder"].to<JsonArray>();
  for (int i = 0; i < MENU_COUNT; i++) menu.add(MENU_ORDER[i]);

  JsonArray games = doc["games"].to<JsonArray>();
  const int allGameIds[GAME_COUNT] = {GAME_TIMER, GAME_REFLEX, GAME_DICE, GAME_CHESS, GAME_HOTPOTATO, GAME_TURNORDER, GAME_STOPWATCH, GAME_TRIVIA, GAME_TOOTHBRUSH, GAME_TETRIS, GAME_STARDROP, GAME_SNAKE, GAME_SIMPLEMORSE};
  const char* allGameNames[GAME_COUNT] = {"Timer", "Reflex", "Dice", "Chess Clock", "Pass the Bomb", "Turn Order", "Stopwatch", "Trivia", "Toothbrush Timer", "Tetris", "Star Drop", "Snake", "SimpleMorse"};
  for (int i = 0; i < GAME_COUNT; i++) {
    JsonObject g = games.add<JsonObject>();
    g["id"] = allGameIds[i];
    g["name"] = allGameNames[i];
    g["enabled"] = gameEnabled[allGameIds[i]];
  }
  doc["settingsId"] = MENU_SETTINGS;
  doc["instructionsId"] = MENU_INSTRUCTIONS;

  doc["startMode"] = prefs.getUChar("startMode", 0);
  doc["startGame"] = prefs.getUChar("startGame", (uint8_t)GAME_TIMER);
  doc["defaultTimerSeconds"] = prefs.getUShort("deftime", (uint16_t)DEFAULT_TIME);
  doc["minTimerSeconds"] = (int)MIN_TIME;
  doc["maxTimerSeconds"] = (int)MAX_TIME;

  JsonArray cats = doc["categories"].to<JsonArray>();
  for (int i = 0; i < TRIVIA_REAL_CATEGORY_COUNT; i++) cats.add(CONFIG_TRIVIA_CATEGORIES[i]);
  JsonArray lvls = doc["levels"].to<JsonArray>();
  for (int i = 0; i < TRIVIA_LEVEL_COUNT; i++) lvls.add(CONFIG_TRIVIA_LEVELS[i]);

  JsonArray hpCats = doc["hpCategories"].to<JsonArray>();
  for (int i = 0; i < HP_CATEGORY_COUNT; i++) {
    JsonObject c = hpCats.add<JsonObject>();
    c["id"] = i;
    c["en"] = cp437ToUtf8(TRANSLATIONS[HP_CATEGORY_STRINGS[i]][LANG_EN]);
    c["fr"] = cp437ToUtf8(TRANSLATIONS[HP_CATEGORY_STRINGS[i]][LANG_FR]);
    c["es"] = cp437ToUtf8(TRANSLATIONS[HP_CATEGORY_STRINGS[i]][LANG_ES]);
    c["enabled"] = hpCategoryEnabled[i];
  }

  JsonObject highScores = doc["highScores"].to<JsonObject>();
  highScores["tetris"] = tetrisHighScore;
  highScores["stardrop"] = stardropHighScore;
  highScores["snake"] = snakeHighScore;

  String out;
  serializeJson(doc, out);
  configServer.send(200, "application/json", out);
}

// ---------------- Route: POST /api/config - saves menu order / startup choice / default timer ----------------
void handlePostConfig() {
  if (!configServer.hasArg("plain")) {
    configServer.send(400, "application/json", "{\"ok\":false,\"error\":\"missing body\"}");
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, configServer.arg("plain"));
  if (err) {
    configServer.send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
    return;
  }

  if (doc["menuOrder"].is<JsonArray>()) {
    JsonArray arr = doc["menuOrder"].as<JsonArray>();
    if ((int)arr.size() == MENU_COUNT) {
      int candidate[MENU_COUNT];
      bool seen[MENU_COUNT];
      for (int i = 0; i < MENU_COUNT; i++) seen[i] = false;
      bool valid = true;
      int i = 0;
      for (JsonVariant v : arr) {
        int id = v.as<int>();
        int pos = -1;
        for (int j = 0; j < MENU_COUNT; j++) if (DEFAULT_MENU_ORDER[j] == id) { pos = j; break; }
        if (pos < 0 || seen[pos]) { valid = false; break; }
        seen[pos] = true;
        candidate[i++] = id;
      }
      if (valid) {
        for (int k = 0; k < MENU_COUNT; k++) MENU_ORDER[k] = candidate[k];
        saveMenuOrderToPrefs();
        rebuildVisibleMenu();
      }
    }
  }

  bool enabledRejected = false;
  if (doc["disabledGames"].is<JsonArray>()) {
    bool candidate[GAME_COUNT];
    for (int i = 0; i < GAME_COUNT; i++) candidate[i] = true;
    for (JsonVariant v : doc["disabledGames"].as<JsonArray>()) {
      int id = v.as<int>();
      if (id >= 0 && id < GAME_COUNT) candidate[id] = false;
    }
    bool anyEnabled = false;
    for (int i = 0; i < GAME_COUNT; i++) if (candidate[i]) anyEnabled = true;

    if (anyEnabled) {
      for (int i = 0; i < GAME_COUNT; i++) gameEnabled[i] = candidate[i];
      saveGameEnabledToPrefs();
      rebuildVisibleMenu();
    } else {
      enabledRejected = true; // at least one game must stay enabled - ignored, previous state kept
    }
  }

  bool hpEnabledRejected = false;
  if (doc["disabledHpCategories"].is<JsonArray>()) {
    bool candidate[HP_CATEGORY_COUNT];
    for (int i = 0; i < HP_CATEGORY_COUNT; i++) candidate[i] = true;
    for (JsonVariant v : doc["disabledHpCategories"].as<JsonArray>()) {
      int id = v.as<int>();
      if (id >= 0 && id < HP_CATEGORY_COUNT) candidate[id] = false;
    }
    bool anyEnabled = false;
    for (int i = 0; i < HP_CATEGORY_COUNT; i++) if (candidate[i]) anyEnabled = true;

    if (anyEnabled) {
      for (int i = 0; i < HP_CATEGORY_COUNT; i++) hpCategoryEnabled[i] = candidate[i];
      saveHpCategoryEnabledToPrefs();
    } else {
      hpEnabledRejected = true; // at least one category must stay enabled - ignored, previous state kept
    }
  }

  if (doc["startMode"].is<int>()) {
    uint8_t m = constrain((int)doc["startMode"], 0, 1);
    prefs.putUChar("startMode", m);
  }
  if (doc["startGame"].is<int>()) {
    int g = constrain((int)doc["startGame"], 0, GAME_COUNT - 1);
    prefs.putUChar("startGame", (uint8_t)g);
  }
  if (doc["defaultTimerSeconds"].is<int>()) {
    long t = constrain((long)doc["defaultTimerSeconds"], MIN_TIME, MAX_TIME);
    prefs.putUShort("deftime", (uint16_t)t);
  }

  if (enabledRejected && hpEnabledRejected) {
    configServer.send(200, "application/json", "{\"ok\":true,\"warning\":\"At least one game and one category must stay enabled - those changes were ignored\"}");
  } else if (enabledRejected) {
    configServer.send(200, "application/json", "{\"ok\":true,\"warning\":\"At least one game must stay enabled - that change was ignored\"}");
  } else if (hpEnabledRejected) {
    configServer.send(200, "application/json", "{\"ok\":true,\"warning\":\"At least one category must stay enabled - that change was ignored\"}");
  } else {
    configServer.send(200, "application/json", "{\"ok\":true}");
  }
}

// ---------------- Route: GET /api/trivia?category=..&level=.. - returns that CSV's rows as JSON ----------------
void handleGetTrivia() {
  if (!configServer.hasArg("category") || !configServer.hasArg("level")) {
    configServer.send(400, "application/json", "{\"ok\":false,\"error\":\"missing category/level\"}");
    return;
  }
  String category = configServer.arg("category");
  String level = configServer.arg("level");

  int catIdx = -1, lvlIdx = -1;
  for (int i = 0; i < TRIVIA_REAL_CATEGORY_COUNT; i++) if (category == CONFIG_TRIVIA_CATEGORIES[i]) catIdx = i;
  for (int i = 0; i < TRIVIA_LEVEL_COUNT; i++) if (level == CONFIG_TRIVIA_LEVELS[i]) lvlIdx = i;
  if (catIdx < 0 || lvlIdx < 0) {
    configServer.send(400, "application/json", "{\"ok\":false,\"error\":\"unknown category/level\"}");
    return;
  }

  char path[40];
  snprintf(path, sizeof(path), "/%s_%s.csv", CONFIG_TRIVIA_CATEGORIES[catIdx], CONFIG_TRIVIA_LEVELS[lvlIdx]);

  JsonDocument doc;
  JsonArray rows = doc["questions"].to<JsonArray>();

  File f = LittleFS.open(path, "r");
  if (f) {
    if (f.available()) f.readStringUntil('\n'); // skip header
    while (f.available()) {
      String line = f.readStringUntil('\n');
      line.trim();
      if (line.length() == 0) continue;

      String fields[15];
      int fieldIdx = 0, fieldStart = 0, len = line.length();
      for (int i = 0; i <= len && fieldIdx < 15; i++) {
        if (i == len || line[i] == ',') {
          fields[fieldIdx] = line.substring(fieldStart, i);
          fields[fieldIdx].trim();
          fieldIdx++;
          fieldStart = i + 1;
        }
      }
      JsonObject row = rows.add<JsonObject>();
      for (int i = 0; i < 15; i++) row[CONFIG_TRIVIA_FIELD_NAMES[i]] = cp437ToUtf8(fields[i]);
    }
    f.close();
  }
  doc["ok"] = true;

  String out;
  serializeJson(doc, out);
  configServer.send(200, "application/json", out);
}

// ---------------- Route: GET /api/trivia/csv?category=..&level=.. - the exact on-device CSV file,
// streamed back as a real file download (Content-Disposition: attachment). This is what the config
// page's "Download CSV" button actually points at now - a plain HTTP download works even in very
// restricted browsers (like Android's captive-portal sign-in screen), where a JS Blob-URL download
// often silently does nothing. Downloads whatever's currently saved on the device - hit "Save
// questions" first if you've made edits you want included. ----------------
void handleGetTriviaCsv() {
  if (!configServer.hasArg("category") || !configServer.hasArg("level")) {
    configServer.send(400, "text/plain", "missing category/level");
    return;
  }
  String category = configServer.arg("category");
  String level = configServer.arg("level");

  int catIdx = -1, lvlIdx = -1;
  for (int i = 0; i < TRIVIA_REAL_CATEGORY_COUNT; i++) if (category == CONFIG_TRIVIA_CATEGORIES[i]) catIdx = i;
  for (int i = 0; i < TRIVIA_LEVEL_COUNT; i++) if (level == CONFIG_TRIVIA_LEVELS[i]) lvlIdx = i;
  if (catIdx < 0 || lvlIdx < 0) {
    configServer.send(400, "text/plain", "unknown category/level");
    return;
  }

  char path[40];
  snprintf(path, sizeof(path), "/%s_%s.csv", CONFIG_TRIVIA_CATEGORIES[catIdx], CONFIG_TRIVIA_LEVELS[lvlIdx]);

  File f = LittleFS.open(path, "r");
  if (!f) {
    configServer.send(404, "text/plain", "not found on the device");
    return;
  }

  // Converted to UTF-8 (with a byte-order mark, so Excel/Numbers detect the encoding and show
  // accents correctly) - the file on the device stays CP437. Files are a few KB, so building the
  // whole response in RAM is fine.
  String body = "\xEF\xBB\xBF";
  body += cp437ToUtf8(f.readString());
  f.close();

  char cd[48];
  snprintf(cd, sizeof(cd), "attachment; filename=\"%s_%s.csv\"", CONFIG_TRIVIA_CATEGORIES[catIdx], CONFIG_TRIVIA_LEVELS[lvlIdx]);
  configServer.sendHeader("Content-Disposition", cd);
  configServer.send(200, "text/csv; charset=utf-8", body);
}

// ---------------- Route: POST /api/reset-highscore - body {"game":"tetris"|"stardrop"|"snake"} -
// resets that one game's persisted high score to 0. A dedicated endpoint rather than folding this
// into POST /api/config, since it's a one-shot action, not a settings save. ----------------
void handleResetHighScore() {
  if (!configServer.hasArg("plain")) {
    configServer.send(400, "application/json", "{\"ok\":false,\"error\":\"missing body\"}");
    return;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, configServer.arg("plain"));
  if (err || !doc["game"].is<const char*>()) {
    configServer.send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
    return;
  }

  String game = doc["game"].as<const char*>();
  if (game == "tetris") {
    tetrisHighScore = 0;
    prefs.putUShort("ttHigh", 0);
  } else if (game == "stardrop") {
    stardropHighScore = 0;
    prefs.putUShort("sdHigh", 0);
  } else if (game == "snake") {
    snakeHighScore = 0;
    prefs.putUShort("skHigh", 0);
  } else {
    configServer.send(400, "application/json", "{\"ok\":false,\"error\":\"unknown game\"}");
    return;
  }
  configServer.send(200, "application/json", "{\"ok\":true}");
}

// ---------------- Route: POST /api/trivia?category=..&level=.. - rewrites that CSV from submitted rows ----------------
void handlePostTrivia() {
  if (!configServer.hasArg("category") || !configServer.hasArg("level") || !configServer.hasArg("plain")) {
    configServer.send(400, "application/json", "{\"ok\":false,\"error\":\"missing category/level/body\"}");
    return;
  }
  String category = configServer.arg("category");
  String level = configServer.arg("level");

  int catIdx = -1, lvlIdx = -1;
  for (int i = 0; i < TRIVIA_REAL_CATEGORY_COUNT; i++) if (category == CONFIG_TRIVIA_CATEGORIES[i]) catIdx = i;
  for (int i = 0; i < TRIVIA_LEVEL_COUNT; i++) if (level == CONFIG_TRIVIA_LEVELS[i]) lvlIdx = i;
  if (catIdx < 0 || lvlIdx < 0) {
    configServer.send(400, "application/json", "{\"ok\":false,\"error\":\"unknown category/level\"}");
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, configServer.arg("plain"));
  if (err || !doc["questions"].is<JsonArray>()) {
    configServer.send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
    return;
  }

  char path[40];
  snprintf(path, sizeof(path), "/%s_%s.csv", CONFIG_TRIVIA_CATEGORIES[catIdx], CONFIG_TRIVIA_LEVELS[lvlIdx]);

  File f = LittleFS.open(path, "w"); // truncates and rewrites
  if (!f) {
    configServer.send(500, "application/json", "{\"ok\":false,\"error\":\"could not open file for writing\"}");
    return;
  }

  f.println("en_question,en_correct,en_wrong1,en_wrong2,en_wrong3,fr_question,fr_correct,fr_wrong1,fr_wrong2,fr_wrong3,es_question,es_correct,es_wrong1,es_wrong2,es_wrong3");

  JsonArray rows = doc["questions"].as<JsonArray>();
  for (JsonVariant rv : rows) {
    JsonObject row = rv.as<JsonObject>();
    String line;
    for (int i = 0; i < 15; i++) {
      String val = row[CONFIG_TRIVIA_FIELD_NAMES[i]].is<const char*>() ? String((const char*)row[CONFIG_TRIVIA_FIELD_NAMES[i]]) : String("");
      line += csvSanitizeField(utf8ToCp437(val)); // page sends UTF-8, the device font needs CP437
      if (i < 14) line += ",";
    }
    f.println(line);
  }
  f.close();

  configServer.send(200, "application/json", "{\"ok\":true}");
}

// Any request the OS makes to check for a captive portal (or any other unknown path) gets
// redirected straight to the config page. Combined with the DNS server answering every lookup
// with our own IP, this is what makes phones pop the page open automatically on connect,
// instead of needing you to type the address in by hand.
void handleConfigNotFound() {
  configServer.sendHeader("Location", "http://" + configApIpStr + "/", true);
  configServer.send(302, "text/plain", "");
}

// ---------------- WiFi QR code: draws the modules directly with fillRect, one square per module.
// Uses the standard "WIFI:T:nopass;S:<ssid>;;" payload that iOS/Android camera apps recognize
// natively to auto-join an open network - no extra app needed. Modules are drawn as lit (white)
// pixels on the display's normal black background, so nothing extra is drawn for the quiet zone -
// it's already black/blank, same as an unset module, and just needs a bit of clear space around it.
// targetSize is the overall pixel footprint (not a per-module multiplier) - each module's edges are
// rounded to the nearest pixel, so the whole code can be sized to anything, not just exact multiples
// of its 29 modules. ----------------
void drawWifiQR(int originX, int originY, int targetSize) {
  const int qrVersion = 3; // 29x29 modules - comfortably fits our ~30-byte payload at low ECC
  String payload = String("WIFI:T:nopass;S:") + CONFIG_AP_SSID + ";;";

  QRCode qrcode;
  uint8_t qrcodeData[qrcode_getBufferSize(qrVersion)];
  qrcode_initText(&qrcode, qrcodeData, qrVersion, ECC_LOW, payload.c_str());

  float cellSize = (float)targetSize / qrcode.size;

  for (uint8_t qy = 0; qy < qrcode.size; qy++) {
    int y0 = originY + (int)roundf(qy * cellSize);
    int y1 = originY + (int)roundf((qy + 1) * cellSize);
    for (uint8_t qx = 0; qx < qrcode.size; qx++) {
      if (qrcode_getModule(&qrcode, qx, qy)) {
        int x0 = originX + (int)roundf(qx * cellSize);
        int x1 = originX + (int)roundf((qx + 1) * cellSize);
        display.fillRect(x0, y0, max(1, x1 - x0), max(1, y1 - y0), SH110X_WHITE);
      }
    }
  }
}

// ---------------- Config Mode screen: QR code sized to fill most of the screen height (with a
// little breathing room so the IP address text beside it has enough width to not wrap), with the
// network name and connect instructions stacked in the column beside it ----------------
void drawConfigModeScreen(const char* ssid, const char* ip) {
  (void)ssid; // no longer drawn dynamically - see the fixed "TigerTimer"/"-Config" lines below, split to fit the column
  display.clearDisplay();
  display.setTextSize(1);

  const int qrSize = 48; // slightly smaller than the max (58px) that would fit the height, to leave
                          // enough width for the IP address text beside it without wrapping
  drawWifiQR(2, (SCREEN_HEIGHT - qrSize) / 2, qrSize);

  int textX = 2 + qrSize + 4; // just right of the QR code
  display.setTextColor(SH110X_WHITE);

  // Network name - manually split across lines to fit the narrow column; if CONFIG_AP_SSID
  // changes, these two fixed strings need to be re-split to match.
  display.setCursor(textX, 2);  display.print(T(STR_CONFIG_WIFI_LABEL));
  display.setCursor(textX, 11); display.print("TigerTimer");
  display.setCursor(textX, 20); display.print("-Config");

  display.setCursor(textX, 32); display.print(T(STR_CONFIG_CONNECT_OR));
  display.setCursor(textX, 41); display.print(T(STR_CONFIG_VISIT));
  display.setCursor(textX, 50); display.print(ip);

  display.display();
}

// ---------------- Config Mode: entry point, called from confirmSettingsSelection() ----------------
void enterConfigMode() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(2, 26);
  display.print(T(STR_CONFIG_STARTING));
  display.display();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(CONFIG_AP_SSID); // no password argument = open network
  IPAddress ip = WiFi.softAPIP();
  configApIpStr = ip.toString();

  configDns.start(DNS_PORT, "*", ip); // answers every DNS lookup with our own IP

  configServer.on("/", HTTP_GET, handleConfigRoot);
  configServer.on("/api/config", HTTP_GET, handleGetConfig);
  configServer.on("/api/config", HTTP_POST, handlePostConfig);
  configServer.on("/api/trivia", HTTP_GET, handleGetTrivia);
  configServer.on("/api/trivia", HTTP_POST, handlePostTrivia);
  configServer.on("/api/trivia/csv", HTTP_GET, handleGetTriviaCsv);
  configServer.on("/api/reset-highscore", HTTP_POST, handleResetHighScore);
  configServer.onNotFound(handleConfigNotFound); // also serves as the captive-portal redirect
  configServer.begin();

  drawConfigModeScreen(CONFIG_AP_SSID, configApIpStr.c_str());

  // Blocking loop: nothing else needs to run while Config Mode is active. Exits on a press of
  // either the arcade button or the encoder click (debounce isn't critical here - it's a
  // one-shot exit signal either way).
  while (digitalRead(BUTTON_PIN) == HIGH && digitalRead(ENCODER_SW) == HIGH) {
    configDns.processNextRequest();
    configServer.handleClient();
    delay(2);
  }
  delay(250); // let the press settle so it isn't also seen as a button event once we're back in loop()

  configServer.stop();
  configDns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);

  registerActivity();
}
