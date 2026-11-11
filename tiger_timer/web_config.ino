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

// Config Mode auto-exit (see enterConfigMode()) - it blocks normal screen/deep sleep, so it must
// end on its own when abandoned.
const unsigned long CONFIG_NO_CLIENT_TIMEOUT_MS  = 5UL * 60UL * 1000UL;  // nobody connected for 5 min
const unsigned long CONFIG_NO_REQUEST_TIMEOUT_MS = 30UL * 60UL * 1000UL; // connected but no request for 30 min
unsigned long configLastRequestMs = 0; // stamped at the top of every route handler below

// Same category/level naming used by the trivia game itself (triviaFilePath) - kept in sync manually
// since they're in different tabs of the same sketch.
const char* CONFIG_TRIVIA_CATEGORIES[TRIVIA_REAL_CATEGORY_COUNT] = {"sports", "geography", "science", "history", "general", "mythology", "art", "animals"};
const char* CONFIG_TRIVIA_LEVELS[TRIVIA_LEVEL_COUNT] = {"junior", "curious", "master"};
const char* CONFIG_TRIVIA_FIELD_NAMES[15] = {
  "en_q", "en_correct", "en_wrong1", "en_wrong2", "en_wrong3",
  "fr_q", "fr_correct", "fr_wrong1", "fr_wrong2", "fr_wrong3",
  "es_q", "es_correct", "es_wrong1", "es_wrong2", "es_wrong3"
};

// ---------------- Saved-settings migration ----------------
// Saved settings ("startGame", "menuOrd", "gameEn") store raw game ids, so whenever the id list
// changes they're converted once at boot, one version at a time; "idVer" records how far along a
// device is (absent = 1).
//   v1 -> v2: Turn Order (id 5) removed; ids 6-12 moved down by one (13 -> 12 games, 15 -> 14 menu entries).
//   v2 -> v3: Pong added as id 12 (12 -> 13 games, 14 -> 15 menu entries); it's inserted into a
//             saved menu order right after Snake, and enabled.
// Keeps the user's menu order, enabled/disabled games and start-up game across updates.
void migrateSavedGameIds() {
  int ver = prefs.getUChar("idVer", 1);
  if (ver >= 3) return;

  if (ver < 2) {
    const int OLD_TURNORDER = 5, V1_MENU = 15, V1_GAMES = 13, V2_MENU = 14, V2_GAMES = 12;
    uint8_t oldMenu[V1_MENU];
    if (prefs.getBytes("menuOrd", oldMenu, V1_MENU) == V1_MENU) {
      uint8_t newMenu[V2_MENU];
      int n = 0;
      for (int i = 0; i < V1_MENU; i++) {
        int id = oldMenu[i];
        if (id == OLD_TURNORDER) continue;
        if (id > OLD_TURNORDER && id < V1_GAMES) id--;
        if (n < V2_MENU) newMenu[n++] = (uint8_t)id;
      }
      if (n == V2_MENU) prefs.putBytes("menuOrd", newMenu, V2_MENU);
      else prefs.remove("menuOrd"); // unexpected shape - fall back to the default order
    }
    uint8_t oldEn[V1_GAMES];
    if (prefs.getBytes("gameEn", oldEn, V1_GAMES) == V1_GAMES) {
      uint8_t newEn[V2_GAMES];
      int n = 0;
      for (int i = 0; i < V1_GAMES; i++) if (i != OLD_TURNORDER && n < V2_GAMES) newEn[n++] = oldEn[i];
      prefs.putBytes("gameEn", newEn, V2_GAMES);
    }
    if (prefs.isKey("startGame")) {
      int g = prefs.getUChar("startGame", GAME_TIMER);
      if (g == OLD_TURNORDER) { prefs.putUChar("startGame", GAME_TIMER); prefs.putUChar("startMode", 0); } // its game is gone - boot to the menu
      else if (g > OLD_TURNORDER) prefs.putUChar("startGame", (uint8_t)(g - 1));
    }
  }

  // v2 -> v3: add Pong
  {
    const int V2_MENU = 14, V2_GAMES = 12, SNAKE_ID = 10, PONG_ID = 12;
    uint8_t oldMenu[V2_MENU];
    if (prefs.getBytes("menuOrd", oldMenu, V2_MENU) == V2_MENU) {
      uint8_t newMenu[V2_MENU + 1];
      int n = 0;
      bool placed = false;
      for (int i = 0; i < V2_MENU; i++) {
        newMenu[n++] = oldMenu[i];
        if (oldMenu[i] == SNAKE_ID) { newMenu[n++] = PONG_ID; placed = true; }
      }
      if (!placed) { // no Snake in the saved list (shouldn't happen) - put Pong first
        for (int i = V2_MENU; i > 0; i--) newMenu[i] = newMenu[i - 1];
        newMenu[0] = PONG_ID;
      }
      prefs.putBytes("menuOrd", newMenu, V2_MENU + 1);
    }
    uint8_t oldEn[V2_GAMES];
    if (prefs.getBytes("gameEn", oldEn, V2_GAMES) == V2_GAMES) {
      uint8_t newEn[V2_GAMES + 1];
      for (int i = 0; i < V2_GAMES; i++) newEn[i] = oldEn[i];
      newEn[V2_GAMES] = 1; // Pong starts enabled
      prefs.putBytes("gameEn", newEn, V2_GAMES + 1);
    }
  }

  prefs.putUChar("idVer", 3);
}

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
// Games hidden from the menu on a fresh box (nothing saved yet) - turned on from Remote Config's
// game list. A box that already has saved choices keeps them.
bool gameHiddenByDefault(int id) {
  return id == GAME_TRIVIA || id == GAME_TETRIS || id == GAME_STARDROP ||
         id == GAME_SNAKE || id == GAME_PONG || id == GAME_SIMPLEMORSE;
}

void loadGameEnabledFromPrefs() {
  uint8_t stored[GAME_COUNT];
  size_t got = prefs.getBytes("gameEn", stored, GAME_COUNT);
  for (int i = 0; i < GAME_COUNT; i++) {
    gameEnabled[i] = (got == GAME_COUNT) ? (stored[i] != 0) : !gameHiddenByDefault(i);
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
  configLastRequestMs = millis(); // any request counts as "someone is using Config Mode" - see enterConfigMode()
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
  configLastRequestMs = millis(); // any request counts as "someone is using Config Mode" - see enterConfigMode()
  JsonDocument doc;

  JsonArray menu = doc["menuOrder"].to<JsonArray>();
  for (int i = 0; i < MENU_COUNT; i++) menu.add(MENU_ORDER[i]);

  JsonArray games = doc["games"].to<JsonArray>();
  const int allGameIds[GAME_COUNT] = {GAME_TIMER, GAME_REFLEX, GAME_DICE, GAME_CHESS, GAME_HOTPOTATO, GAME_STOPWATCH, GAME_TRIVIA, GAME_TOOTHBRUSH, GAME_TETRIS, GAME_STARDROP, GAME_SNAKE, GAME_SIMPLEMORSE, GAME_PONG};
  const char* allGameNames[GAME_COUNT] = {"Timer", "Reflex", "Dice", "Chess Clock", "Pass the Bomb", "Stopwatch", "Trivia", "Toothbrush Timer", "Tetris", "Star Drop", "Snake", "SimpleMorse", "Pong"};
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
  highScores["pong"] = pongHighScore;

  String out;
  serializeJson(doc, out);
  configServer.send(200, "application/json", out);
}

// ---------------- Route: POST /api/config - saves menu order / startup choice / default timer ----------------
void handlePostConfig() {
  configLastRequestMs = millis(); // any request counts as "someone is using Config Mode" - see enterConfigMode()
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
  configLastRequestMs = millis(); // any request counts as "someone is using Config Mode" - see enterConfigMode()
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
  configLastRequestMs = millis(); // any request counts as "someone is using Config Mode" - see enterConfigMode()
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

// ---------------- Route: POST /api/reset-highscore - body {"game":"tetris"|"stardrop"|"snake"|"pong"} -
// resets that one game's persisted high score to 0. A dedicated endpoint rather than folding this
// into POST /api/config, since it's a one-shot action, not a settings save. ----------------
void handleResetHighScore() {
  configLastRequestMs = millis(); // any request counts as "someone is using Config Mode" - see enterConfigMode()
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
  } else if (game == "pong") {
    pongHighScore = 0;
    prefs.putUShort("pgHigh", 0);
  } else {
    configServer.send(400, "application/json", "{\"ok\":false,\"error\":\"unknown game\"}");
    return;
  }
  configServer.send(200, "application/json", "{\"ok\":true}");
}

// ---------------- Route: POST /api/trivia?category=..&level=.. - rewrites that CSV from submitted rows ----------------
void handlePostTrivia() {
  configLastRequestMs = millis(); // any request counts as "someone is using Config Mode" - see enterConfigMode()
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
  int skipped = 0;
  for (JsonVariant rv : rows) {
    JsonObject row = rv.as<JsonObject>();
    String line;
    bool hasQuestionText = false;
    for (int i = 0; i < 15; i++) {
      String val = row[CONFIG_TRIVIA_FIELD_NAMES[i]].is<const char*>() ? String((const char*)row[CONFIG_TRIVIA_FIELD_NAMES[i]]) : String("");
      String clean = csvSanitizeField(utf8ToCp437(val)); // page sends UTF-8, the device font needs CP437
      if (i % 5 == 0 && clean.length() > 0) hasQuestionText = true; // columns 0/5/10 = en/fr/es question
      line += clean;
      if (i < 14) line += ",";
    }
    // A row with no question text in any language (e.g. "+ Add question" never filled in) would
    // still count as a question on the device and show up blank - drop it instead.
    if (!hasQuestionText) { skipped++; continue; }
    f.println(line);
  }
  f.close();

  if (skipped > 0) {
    String resp = "{\"ok\":true,\"skipped\":" + String(skipped) + "}";
    configServer.send(200, "application/json", resp);
    return;
  }

  configServer.send(200, "application/json", "{\"ok\":true}");
}

// Any request the OS makes to check for a captive portal (or any other unknown path) gets
// redirected straight to the config page. Combined with the DNS server answering every lookup
// with our own IP, this is what makes phones pop the page open automatically on connect,
// instead of needing you to type the address in by hand.
void handleConfigNotFound() {
  configLastRequestMs = millis(); // any request counts as "someone is using Config Mode" - see enterConfigMode()
  configServer.sendHeader("Location", "http://" + configApIpStr + "/", true);
  configServer.send(302, "text/plain", "");
}

// ---------------- WiFi QR code: draws the modules directly with fillRect, one square per module.
// Uses the standard "WIFI:T:nopass;S:<ssid>;;" payload that iOS/Android camera apps recognize
// natively to auto-join an open network - no extra app needed. Modules are drawn as lit (white)
// pixels on the display's normal black background, so nothing extra is drawn for the quiet zone -
// it's already black/blank, same as an unset module, and just needs a bit of clear space around it.
// Every module is exactly modulePx x modulePx pixels: a fractional size (e.g. 48px / 29 modules)
// mixes 1px and 2px modules, which scanners at this tiny size struggle to decode. ----------------
const int QR_VERSION = 3;   // 29x29 modules - comfortably fits our ~35-byte payload at low ECC
const int QR_MODULE_PX = 2; // 29 * 2 = 58px, the largest whole-pixel size that fits the 64px height

void drawWifiQR(int originX, int originY, int modulePx) {
  String payload = String("WIFI:T:nopass;S:") + CONFIG_AP_SSID + ";;";

  QRCode qrcode;
  uint8_t qrcodeData[qrcode_getBufferSize(QR_VERSION)];
  qrcode_initText(&qrcode, qrcodeData, QR_VERSION, ECC_LOW, payload.c_str());

  for (uint8_t qy = 0; qy < qrcode.size; qy++) {
    for (uint8_t qx = 0; qx < qrcode.size; qx++) {
      if (qrcode_getModule(&qrcode, qx, qy)) {
        display.fillRect(originX + qx * modulePx, originY + qy * modulePx, modulePx, modulePx, SH110X_WHITE);
      }
    }
  }
}

// ---------------- OLED refresh rate for camera scanning ----------------
// The SH1106 is a passive-matrix panel: it lights one row at a time, and at its stock ~100 Hz
// frame rate a phone camera's short rolling-shutter exposure only catches part of the scan - the
// QR comes out with a dark diagonal band and won't decode (seen on a Pixel 10 Pro). While the QR is
// on screen we push the internal oscillator to its maximum (+50%) and shorten the pre-charge
// phases, which together roughly double the frame rate so any phone's exposure spans whole frames.
// Restored on exit so normal screens look exactly as before.
void setOledFastRefresh(bool fast) {
  display.oled_command(SH110X_SETDISPLAYCLOCKDIV); // 0xD5
  display.oled_command(fast ? 0xF0 : 0x80);        // high nibble = oscillator (0xF = +50%), low = divide by 1; 0x80 = library default
  display.oled_command(SH110X_SETPRECHARGE);       // 0xD9
  display.oled_command(fast ? 0x22 : 0x1F);        // 2+2 DCLK phases vs library default 15+1
}

// ---------------- Config Mode screen: QR code at full whole-pixel size on the left, with the
// network name and connect instructions stacked in the column beside it ----------------
void drawConfigModeScreen(const char* ssid, const char* ip) {
  (void)ssid; // no longer drawn dynamically - see the fixed "TigerTimer"/"-Config" lines below, split to fit the column
  display.clearDisplay();
  display.setTextSize(1);

  const int qrSize = QR_VERSION * 4 + 17; // modules per side (29)
  const int qrPx = qrSize * QR_MODULE_PX;  // 58px
  drawWifiQR(1, (SCREEN_HEIGHT - qrPx) / 2, QR_MODULE_PX);

  // Text column: x=63 leaves 65px = 11 characters (the 11th glyph's trailing spacing column falls
  // off-screen), enough for "192.168.4.1". Every string in this column must stay <= 11 chars.
  int textX = 1 + qrPx + 4;
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

  // Routes are registered once per boot: WebServer keeps its handler list across stop()/begin(),
  // and calling on() again on every visit would just append duplicate handlers (leaking memory).
  static bool routesRegistered = false;
  if (!routesRegistered) {
    configServer.on("/", HTTP_GET, handleConfigRoot);
    configServer.on("/api/config", HTTP_GET, handleGetConfig);
    configServer.on("/api/config", HTTP_POST, handlePostConfig);
    configServer.on("/api/trivia", HTTP_GET, handleGetTrivia);
    configServer.on("/api/trivia", HTTP_POST, handlePostTrivia);
    configServer.on("/api/trivia/csv", HTTP_GET, handleGetTriviaCsv);
    configServer.on("/api/reset-highscore", HTTP_POST, handleResetHighScore);
    configServer.onNotFound(handleConfigNotFound); // also serves as the captive-portal redirect
    routesRegistered = true;
  }
  configServer.begin();

  setOledFastRefresh(true); // so phone cameras can capture the QR - see setOledFastRefresh()
  drawConfigModeScreen(CONFIG_AP_SSID, configApIpStr.c_str());

  // Blocking loop: nothing else needs to run while Config Mode is active (so normal screen/deep
  // sleep can't run either). Exits on a press of either the arcade button or the encoder click,
  // or automatically once nobody is using it - so a forgotten Config Mode can't keep WiFi and the
  // screen on until the battery is flat:
  //   - no phone/laptop connected to the access point for CONFIG_NO_CLIENT_TIMEOUT_MS, or
  //   - no web request at all for CONFIG_NO_REQUEST_TIMEOUT_MS, even if something stays connected.
  unsigned long lastClientSeenMs = millis();
  configLastRequestMs = millis();
  while (digitalRead(BUTTON_PIN) == HIGH && digitalRead(ENCODER_SW) == HIGH) {
    configDns.processNextRequest();
    configServer.handleClient();
    if (WiFi.softAPgetStationNum() > 0) lastClientSeenMs = millis();
    if (millis() - lastClientSeenMs >= CONFIG_NO_CLIENT_TIMEOUT_MS) break;
    if (millis() - configLastRequestMs >= CONFIG_NO_REQUEST_TIMEOUT_MS) break;
    delay(2);
  }

  // Wait for whichever button ended it to be fully released (plus a settle delay), same idea as
  // the splash skip (FSD 3.6): a fixed delay alone let a slightly-long exit press still be down
  // when loop() resumed, which read it as a fresh click and reopened Settings.
  while (digitalRead(BUTTON_PIN) == LOW || digitalRead(ENCODER_SW) == LOW) delay(2);
  delay(50);

  setOledFastRefresh(false);
  configServer.stop();
  configDns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);

  registerActivity();
}
