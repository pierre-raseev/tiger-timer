// ======================================================================================
// Metronome: a steady click at 30-240 BPM in 4/4, the first beat of every bar higher-pitched
// (the accent), like a commercial metronome. The arcade button's LED flashes on every beat
// (longer on the accent). Turn = tempo (also while running), arcade button or encoder click =
// start/stop, hold 1 s = leave, as everywhere else.
// Look: a pyramid (pendulum) metronome for the menu icon and the 1 s splash, its arm swinging to
// one end on each beat; while running, a ball hops from pad to pad, landing on one per beat.
// Sound: a dry wood-block tick, a fifth higher on the accent.
// ======================================================================================

const int METRO_MIN_BPM = 30;
const int METRO_MAX_BPM = 240;
const int METRO_DEFAULT_BPM = 100;
const int METRO_BEATS = 4;               // beats per bar - beat 0 is the accent
const unsigned long METRO_LED_ACCENT_MS = 160;
const unsigned long METRO_LED_BEAT_MS = 70;
const unsigned long METRO_FRAME_MS = 40; // ~25 fps for the moving parts between beats
// A full screen transfer takes ~28 ms. The "ball has landed" frame for the next beat is started
// this long before the beat, so it has finished arriving on the panel when the click sounds;
// in-between frames stop early enough never to run into it.
const long METRO_LANDING_LEAD_US = 34000;
const long METRO_LANDING_MIN_US = 30000; // closer than this to the beat: too late to start it, skip it
const long METRO_FRAME_GUARD_US = METRO_LANDING_LEAD_US + 32000;

unsigned long metroBeatUs = 0;    // micros() when the last beat sounded
unsigned long metroNextBeatUs = 0;
unsigned long metroLastFrameMs = 0;
bool metroLandingShown = false; // the next beat's landing frame is already on screen
unsigned long metroLastDetentMs = 0;
// metroBpm, metroRunning, metroBeat, metroTestPhase live in tiger_timer.ino (the
// bench-test commands there use them, and that tab is compiled before this one)

unsigned long metroIntervalUs() { return 60000000UL / (unsigned long)metroBpm; }

bool metroIsRunning() { return metroRunning; }

// how far through the current beat we are, 0..1 (0 while stopped)
float metroPhase() {
  if (metroTestPhase >= 0) return metroTestPhase;
  if (!metroRunning) return 0;
  float p = (float)(micros() - metroBeatUs) / (float)metroIntervalUs();
  return constrain(p, 0.0f, 1.0f);
}

// ---------------- The click itself: a dry wood-block tick, higher on the accent ----------------
void metroClick(bool accent) {
  playNote(accent ? 1568 : 1046, 18);
}

// ---------------- Game state ----------------
void initMetronome() {
  metroRunning = false;
  metroBeat = 0;
  metroTestPhase = -1;
  metroBpm = constrain((int)prefs.getUShort("metBpm", METRO_DEFAULT_BPM), METRO_MIN_BPM, METRO_MAX_BPM);
  digitalWrite(BUTTON_LED, LOW);
}

// leaving the game: stop
void leaveMetronome() {
  metroRunning = false;
}

// arcade button / encoder click: start or stop
void metroToggle() {
  if (metroRunning) {
    metroRunning = false;
    digitalWrite(BUTTON_LED, LOW);
    render();
    return;
  }
  if (prefs.getUShort("metBpm", METRO_DEFAULT_BPM) != metroBpm) prefs.putUShort("metBpm", (uint16_t)metroBpm);
  metroRunning = true;
  metroLandingShown = false;
  metroBeat = METRO_BEATS - 1;     // so the first beat to sound is the accent
  metroNextBeatUs = micros();      // ...right away
}

// encoder detent: tempo, 1 BPM per detent, 4 per detent when spun fast - also while running,
// the new tempo taking effect from the next beat
void metroAdjustBpm(int direction) {
  int step = (millis() - metroLastDetentMs < 60) ? 4 : 1;
  metroLastDetentMs = millis();
  metroBpm = constrain(metroBpm + direction * step, METRO_MIN_BPM, METRO_MAX_BPM);
}

// ---------------- Called every loop() pass while the Metronome is open ----------------
void updateMetronome() {
  if (!metroRunning) return;
  unsigned long now = micros();
  bool canDraw = !screenAsleep && swState != LOW && inGameScreen(); // never draw over the volume overlay or a dialog

  if ((long)(now - metroNextBeatUs) >= 0) {
    metroBeat = (metroBeat + 1) % METRO_BEATS;
    metroBeatUs = metroNextBeatUs;
    metroNextBeatUs += metroIntervalUs();
    if ((long)(now - metroNextBeatUs) >= 0) { // fell a whole beat behind (blocked) - resync instead of rattling off the missed ones
      metroBeatUs = now;
      metroNextBeatUs = now + metroIntervalUs();
    }
    digitalWrite(BUTTON_LED, HIGH);
    metroClick(metroBeat == 0);
    // the landing frame is normally already showing; draw it now only if it couldn't be sent ahead
    // (first beat after starting, or the screen just came back)
    if (!metroLandingShown && !screenAsleep && swState != LOW) { render(); metroLastFrameMs = millis(); }
    metroLandingShown = false;
    return;
  }

  unsigned long sinceMs = (now - metroBeatUs) / 1000;
  if (sinceMs >= (metroBeat == 0 ? METRO_LED_ACCENT_MS : METRO_LED_BEAT_MS)) digitalWrite(BUTTON_LED, LOW);
  if (!canDraw) return;

  long untilNextUs = (long)(metroNextBeatUs - now);
  if (!metroLandingShown && untilNextUs <= METRO_LANDING_LEAD_US) {
    if (untilNextUs >= METRO_LANDING_MIN_US) { // still time to get it on the panel before the click
      metroLandingShown = true;                // drawMetronomeScreen() now draws the next beat, phase 0
      render();
      metroLastFrameMs = millis();
    }
    return;
  }
  if (!metroLandingShown && untilNextUs > METRO_FRAME_GUARD_US && millis() - metroLastFrameMs >= METRO_FRAME_MS) {
    render();
    metroLastFrameMs = millis();
  }
}

// ======================================================================================
// Drawing
// ======================================================================================

// header: icon, text on the right, divider
void drawMetroHeader(const char* right) {
  drawMetronomeIcon(7, 7, 6, SH110X_WHITE);
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(right, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(SCREEN_WIDTH - w - 2, 4);
  display.print(right);
  display.drawLine(0, 14, SCREEN_WIDTH, 14, SH110X_WHITE);
}

// text centred horizontally in [x0, x0 + width)
void drawMetroCentered(const char* s, int x0, int width, int y, int size) {
  display.setTextSize(size);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(x0 + (width - (int)w) / 2, y);
  display.print(s);
}

// ---------------- Pyramid metronome (icon-sized drawing is separate, below). The arm pivots near the base; angle in radians,
// 0 = upright, + = leaning right. weightAt = how far up the arm the sliding weight sits. ----------------
void drawPyramidMetronome(int cx, int topY, int baseY, int halfTop, int halfBase, float angle, int armLen, int weightAt) {
  display.drawLine(cx - halfTop, topY, cx - halfBase, baseY, SH110X_WHITE);
  display.drawLine(cx + halfTop, topY, cx + halfBase, baseY, SH110X_WHITE);
  display.drawFastHLine(cx - halfTop, topY, 2 * halfTop + 1, SH110X_WHITE);
  display.drawFastHLine(cx - halfBase - 2, baseY, 2 * halfBase + 5, SH110X_WHITE); // base, a little wider
  display.drawFastHLine(cx - halfBase - 2, baseY + 1, 2 * halfBase + 5, SH110X_WHITE);
  int py = baseY - 5;
  float sx = sinf(angle), cy = cosf(angle);
  int tx = cx + (int)lroundf(sx * armLen), ty = py - (int)lroundf(cy * armLen);
  display.drawLine(cx, py, tx, ty, SH110X_WHITE);
  display.drawLine(cx + 1, py, tx + 1, ty, SH110X_WHITE);
  int wx = cx + (int)lroundf(sx * weightAt), wy = py - (int)lroundf(cy * weightAt);
  display.fillRect(wx - 3, wy - 2, 7, 5, SH110X_WHITE);
  display.fillCircle(cx, py, 2, SH110X_WHITE); // pivot
}

const float METRO_SWING = 0.52f; // ~30 degrees each side

// ---------------- Running screen: a ball hopping from pad to pad, one landing per beat ----------------
const int METRO_PAD_X[METRO_BEATS] = {16, 48, 80, 112};
const int METRO_FLOOR_Y = 60;

void drawMetroPads(int litPad, bool impact) {
  for (int i = 0; i < METRO_BEATS; i++) {
    int half = (i == 0) ? 12 : 9;
    if (i == litPad) display.fillRect(METRO_PAD_X[i] - half, METRO_FLOOR_Y, 2 * half + 1, 4, SH110X_WHITE);
    else display.drawRect(METRO_PAD_X[i] - half, METRO_FLOOR_Y, 2 * half + 1, 4, SH110X_WHITE);
  }
  if (impact && litPad >= 0) { // little impact lines either side of the landing spot
    int x = METRO_PAD_X[litPad];
    display.drawLine(x - 7, METRO_FLOOR_Y - 2, x - 10, METRO_FLOOR_Y - 5, SH110X_WHITE);
    display.drawLine(x + 7, METRO_FLOOR_Y - 2, x + 10, METRO_FLOOR_Y - 5, SH110X_WHITE);
  }
}

void drawMetronomeScreen() {
  char buf[10];
  snprintf(buf, sizeof(buf), "%d BPM", metroBpm);
  drawMetroHeader(buf);
  if (!metroRunning) {
    drawMetroPads(-1, false);
    display.fillCircle(METRO_PAD_X[0], METRO_FLOOR_Y - 4, 3, SH110X_WHITE);
    display.setTextColor(SH110X_WHITE);
    drawMetroCentered(T(STR_PRESS_TO_START), 0, SCREEN_WIDTH, 32, 1);
    return;
  }
  float p = metroPhase();
  int from = metroBeat;
  if (metroLandingShown) { from = (metroBeat + 1) % METRO_BEATS; p = 0; } // drawn just ahead of that beat
  int to = (from + 1) % METRO_BEATS;
  float x = METRO_PAD_X[from] + (METRO_PAD_X[to] - METRO_PAD_X[from]) * p;
  float height = (to == 0) ? 40.0f : 28.0f; // the jump back to the first pad is a big one
  float y = (METRO_FLOOR_Y - 4) - height * 4.0f * p * (1.0f - p);
  drawMetroPads(p < 0.25f ? from : -1, p < 0.12f);
  display.fillCircle((int)lroundf(x), (int)lroundf(y), 3, SH110X_WHITE);
}


// ---------------- Menu icon (13 px): a pyramid metronome with its arm leaning right ----------------
void drawMetronomeIcon(int cx, int cy, int r, uint16_t color) {
  display.drawLine(cx - 1, cy - 6, cx - 5, cy + 5, color);
  display.drawLine(cx + 1, cy - 6, cx + 5, cy + 5, color);
  display.drawFastHLine(cx - 1, cy - 6, 3, color);
  display.drawFastHLine(cx - 6, cy + 6, 13, color);
  display.drawLine(cx, cy + 3, cx + 4, cy - 5, color);
  display.fillRect(cx + 1, cy - 2, 3, 2, color);
}

// ---------------- Splash (12 x 83 ms, above the game's name): the big pyramid metronome, its arm
// reaching an end every 4 frames, with tick marks at that end ----------------
void drawMetronomeSplashFrame(int f) {
  float a = METRO_SWING * cosf(PI * f / 4.0f);
  drawPyramidMetronome(64, 2, 40, 4, 17, a, 34, 22);
  if (f % 4 == 0) {
    int side = (f % 8 == 0) ? 1 : -1;
    int tx = 64 + side * 22;
    display.drawLine(tx + side * 3, 4, tx + side * 6, 1, SH110X_WHITE);
    display.drawLine(tx + side * 4, 8, tx + side * 8, 7, SH110X_WHITE);
  }
}
