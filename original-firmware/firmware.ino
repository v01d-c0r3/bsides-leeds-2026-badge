#include <avr/io.h>
#include <avr/sleep.h>
#include <avr/pgmspace.h>
#include <EEPROM.h>

#include <ptc.h>
#include <tinyNeoPixel_Static.h>

static const uint8_t WAKE_BUTTON_PIN = PIN_PA3;
static const uint8_t LED_DATA_PIN = PIN_PB3;
static const uint8_t LED_POWER_PIN = PIN_PB2;

static const uint8_t NUM_LEDS = 18;
static const uint8_t LEDS_PER_EYE = 9;
static const uint8_t NUM_TOUCH_BUTTONS = 6;

static const uint32_t WAKE_TIMEOUT_MS = 900000UL;
static const uint16_t MAX_BUTTON_HOLD_MS = 2000;
static const uint16_t SHORT_PRESS_THRESHOLD_MS = 200;
static const uint16_t LONG_PRESS_THRESHOLD_MS = 1200;
static const uint8_t BUTTON_DEBOUNCE_MS = 50;
static const uint8_t TOUCH_POLL_MS = 10;
static const uint8_t SLEEP_DEBOUNCE_CYCLES = 3;
static const uint8_t FIND_SEQUENCE_LENGTH = 7;
static const uint8_t FIND_SEQUENCE_STARTING_LIVES = 10;
static const uint16_t FIND_SEQUENCE_PREVIEW_MS = 3000;


static const uint8_t LEFT_BLUE_MASK = 1 << 0;
static const uint8_t LEFT_RED_MASK = 1 << 1;
static const uint8_t LEFT_GREEN_MASK = 1 << 2;
static const uint8_t RIGHT_GREEN_MASK = 1 << 3;
static const uint8_t RIGHT_RED_MASK = 1 << 4;
static const uint8_t RIGHT_BLUE_MASK = 1 << 5;

struct RgbColor {
  uint8_t red;
  uint8_t green;
  uint8_t blue;
};

static const RgbColor COLOR_OFF = {0, 0, 0};
static const RgbColor COLOR_RED = {30, 0, 0};
static const RgbColor COLOR_GREEN = {0, 30, 0};
static const RgbColor COLOR_BLUE = {0, 0, 30};
static const RgbColor COLOR_ORANGE = {30, 12, 0};

static const uint8_t TOUCH_BUTTON_PINS[NUM_TOUCH_BUTTONS] = {
  PIN_PA4,
  PIN_PA5,
  PIN_PA6,
  PIN_PA7,
  PIN_PB0,
  PIN_PB1,
};

cap_sensor_t touchButtons[NUM_TOUCH_BUTTONS];
byte pixelBuffer[NUM_LEDS * 3];
tinyNeoPixel ledStrip = tinyNeoPixel(NUM_LEDS, LED_DATA_PIN, NEO_GRB, pixelBuffer);

volatile bool shouldProcessPtc = false;
volatile bool rebootOnButtonPress = false;

uint8_t state;

uint16_t randomState = 0xACE1u;

void seedGameRandom()
{
  randomState ^= (uint16_t)millis();
  if (randomState == 0) {
    randomState = 0xACE1u;
  }
}

uint8_t nextRandomByte()
{
  const bool bit = randomState & 1;
  randomState >>= 1;

  if (bit) {
    randomState ^= 0xB400u;
  }

  return (uint8_t)randomState;
}

uint8_t randomColorIndex()
{
  uint8_t value;
  do {
    value = nextRandomByte() & 0x03;
  } while (value > 2);

  return value;
}

uint8_t randomLedIndex()
{
  uint8_t value;
  do {
    value = nextRandomByte() & 0x0F;
  } while (value >= LEDS_PER_EYE);

  return value;
}

uint8_t randomEight()
{
  return nextRandomByte() & 0x07;
}


uint8_t getPressedTouchMask()
{
  uint8_t pressedMask = 0;

  for (uint8_t buttonIndex = 0; buttonIndex < NUM_TOUCH_BUTTONS; ++buttonIndex) {
    if (ptc_get_node_touched(&touchButtons[buttonIndex])) {
      pressedMask |= (1 << buttonIndex);
    }
  }

  return pressedMask;
}

void setupTouchButtons()
{
  for (uint8_t buttonIndex = 0; buttonIndex < NUM_TOUCH_BUTTONS; ++buttonIndex) {
    const uint8_t result = ptc_add_selfcap_node(
      &touchButtons[buttonIndex],
      0,
      PIN_TO_PTC(TOUCH_BUTTON_PINS[buttonIndex])
    );

    if (result != PTC_LIB_SUCCESS) {
      while (true) { }
    }

    ptc_node_set_gain(&touchButtons[buttonIndex], PTC_GAIN_1);
    ptc_node_set_prescaler(&touchButtons[buttonIndex], PTC_PRESC_DIV4_gc);
    ptc_node_set_oversamples(&touchButtons[buttonIndex], 4);
    ptc_node_set_thresholds(&touchButtons[buttonIndex], 80, 10);
  }
}

void setAllLeds(uint8_t red, uint8_t green, uint8_t blue, bool show = false)
{
  for (uint8_t ledIndex = 0; ledIndex < NUM_LEDS; ++ledIndex) {
    ledStrip.setPixelColor(ledIndex, red, green, blue);
  }

  if (show) {
    ledStrip.show();
  }
}

void setAllLeds(RgbColor color, bool show = false)
{
  setAllLeds(color.red, color.green, color.blue, show);
}

void setTouchButtonPins(uint8_t mode)
{
  for (uint8_t buttonIndex = 0; buttonIndex < NUM_TOUCH_BUTTONS; ++buttonIndex) {
    pinMode(TOUCH_BUTTON_PINS[buttonIndex], mode);
  }
}

void disableRtc()
{
  RTC.PITINTCTRL = ~(RTC_PI_bm);
  shouldProcessPtc = false;
}

void enableRtcPtc(uint8_t period = RTC_PERIOD_CYC16_gc)
{
  RTC.PITINTCTRL = RTC_PI_bm;
  RTC.PITCTRLA = period | RTC_PITEN_bm;
  shouldProcessPtc = true;
}

void initRtc()
{
  while (RTC.STATUS > 0) {
    ;
  }

  RTC.CLKSEL = RTC_CLKSEL_INT1K_gc;
}

void enableRebootOnButton()
{
  PORTA.PIN3CTRL = PORT_PULLUPEN_bm | PORT_ISC_LEVEL_gc;
  rebootOnButtonPress = true;
}

void disableRebootOnButton()
{
  PORTA.PIN3CTRL = PORT_PULLUPEN_bm;
  rebootOnButtonPress = false;
}

void miniSleep(uint8_t period = RTC_PERIOD_CYC16_gc)
{
  RTC.PITINTCTRL = RTC_PI_bm;
  RTC.PITCTRLA = period | RTC_PITEN_bm;

  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
  sleep_enable();
  sleep_cpu();

  RTC.PITINTCTRL = ~(RTC_PI_bm);
}

void enterSleep()
{
  disableRtc();
  set_sleep_mode(SLEEP_MODE_PWR_DOWN);

  PORTA.PIN3CTRL = PORT_PULLUPEN_bm | PORT_ISC_LEVEL_gc;
  digitalWrite(LED_POWER_PIN, LOW);
  ADC0.CTRLA &= ~ADC_ENABLE_bm;
  setTouchButtonPins(OUTPUT);

  sleep_enable();
  sleep_cpu();

  // Wake is handled by rebooting through the watchdog, as in the original code.
  _PROTECTED_WRITE(WDT.CTRLA, WDT_PERIOD_8CLK_gc);
  while (true) { }
}

ISR(PORTA_PORT_vect)
{
  PORTA.INTFLAGS = PORT_INT3_bm;

  if (rebootOnButtonPress) {
    _PROTECTED_WRITE(WDT.CTRLA, WDT_PERIOD_8CLK_gc);
    while (true) { }
  }
}

ISR(RTC_PIT_vect)
{
  RTC.PITINTFLAGS = RTC_PI_bm;

  if (shouldProcessPtc) {
    ptc_process(millis());
  }
}

uint16_t measureWakeButtonLowTime(uint16_t maxMs)
{
  if (digitalRead(WAKE_BUTTON_PIN) == HIGH) {
    return 0;
  }

  delay(BUTTON_DEBOUNCE_MS);
  uint16_t elapsedMs = BUTTON_DEBOUNCE_MS;

  while (digitalRead(WAKE_BUTTON_PIN) == LOW) {
    delay(5);
    elapsedMs += 5;

    if (elapsedMs > maxMs) {
      return maxMs;
    }
  }

  return elapsedMs;
}

uint8_t loopingEyes(uint16_t step, uint8_t red, uint8_t green, uint8_t blue)
{
  setAllLeds(COLOR_OFF);

  const uint8_t ledIndex = step % 9;
  ledStrip.setPixelColor(ledIndex, red, green, blue);
  ledStrip.setPixelColor(17 - ledIndex, red, green, blue);
  ledStrip.show();

  return 100;
}

void setLeftEye(uint8_t red, uint8_t green, uint8_t blue, uint8_t offset = 0)
{
  for (uint8_t ledIndex = offset; ledIndex < 9 + offset; ++ledIndex) {
    ledStrip.setPixelColor(ledIndex, red, green, blue);
  }
}

void setRightEye(uint8_t red, uint8_t green, uint8_t blue)
{
  setLeftEye(red, green, blue, 9);
}

void setRightEyeLed(uint8_t ledIndex, uint8_t red, uint8_t green, uint8_t blue)
{
  ledStrip.setPixelColor(ledIndex == 0 ? 17 : ledIndex + 8, red, green, blue);
}

void showTouchedPads()
{
  const uint8_t pressedMask = getPressedTouchMask();

  if (pressedMask & (LEFT_BLUE_MASK | LEFT_RED_MASK | LEFT_GREEN_MASK)) {
    setLeftEye(
      (pressedMask & LEFT_RED_MASK) ? 20 : 0,
      (pressedMask & LEFT_GREEN_MASK) ? 20 : 0,
      (pressedMask & LEFT_BLUE_MASK) ? 20 : 0
    );
  }

  if (pressedMask & (RIGHT_BLUE_MASK | RIGHT_RED_MASK | RIGHT_GREEN_MASK)) {
    setRightEye(
      (pressedMask & RIGHT_RED_MASK) ? 20 : 0,
      (pressedMask & RIGHT_GREEN_MASK) ? 20 : 0,
      (pressedMask & RIGHT_BLUE_MASK) ? 20 : 0
    );
  }

  ledStrip.show();
}

void flashNotification(RgbColor color)
{
  setAllLeds(color, true);
  delay(50);
  setAllLeds(COLOR_OFF, true);
  delay(50);
  setAllLeds(color, true);
  delay(50);
  setAllLeds(COLOR_OFF, true);
  delay(50);
}

void showFailure()
{
  flashNotification(COLOR_RED);
}

void showSuccess()
{
  
  EEPROM.update(0, state);
  for (uint8_t flashIndex = 0; flashIndex < 6; ++flashIndex) {
    flashNotification(COLOR_GREEN);
  }
}

bool isAnyPadPressed()
{
  return getPressedTouchMask() != 0;
}

void waitForAllTouchPadsReleased()
{
  while (getPressedTouchMask() != 0) {
    delay(TOUCH_POLL_MS);
  }
}

void waitForWakeButtonReleased()
{
  while (digitalRead(WAKE_BUTTON_PIN) == LOW) {
    delay(5);
  }
}

static const uint8_t PLAYER_LEFT = 0;
static const uint8_t PLAYER_RIGHT = 1;
static const uint8_t MEMORY_START_LEVEL = 3;
static const uint8_t LEFT_PLAYER_MASK = LEFT_BLUE_MASK | LEFT_RED_MASK | LEFT_GREEN_MASK;
static const uint8_t RIGHT_PLAYER_MASK = RIGHT_BLUE_MASK | RIGHT_RED_MASK | RIGHT_GREEN_MASK;

RgbColor colorForIndex(uint8_t colorIndex)
{
  switch (colorIndex) {
    case 0:
      return COLOR_RED;
    case 1:
      return COLOR_GREEN;
    case 2:
      return COLOR_BLUE;
    default:
      return COLOR_OFF;
  }
}

uint8_t playerMask(uint8_t player)
{
  return player == PLAYER_LEFT ? LEFT_PLAYER_MASK : RIGHT_PLAYER_MASK;
}

void setPlayerEye(uint8_t player, RgbColor color)
{
  if (player == PLAYER_LEFT) {
    setLeftEye(color.red, color.green, color.blue);
    return;
  }

  setRightEye(color.red, color.green, color.blue);
}

void setPlayerEyeLed(uint8_t player, uint8_t ledIndex, RgbColor color)
{
  if (player == PLAYER_LEFT) {
    ledStrip.setPixelColor(ledIndex, color.red, color.green, color.blue);
  } else {
    setRightEyeLed(ledIndex, color.red, color.green, color.blue);
  }
}

void flashPlayerEye(uint8_t player, RgbColor color, uint8_t flashes = 2)
{
  for (uint8_t flashIndex = 0; flashIndex < flashes; ++flashIndex) {
    setPlayerEye(player, color);
    ledStrip.show();
    delay(50);
    setPlayerEye(player, COLOR_OFF);
    ledStrip.show();
    delay(50);
  }
}

void showTwoPlayerResult(uint8_t leftScore, uint8_t rightScore)
{
  setAllLeds(COLOR_OFF);

  if (leftScore > rightScore) {
    setPlayerEye(PLAYER_LEFT, COLOR_GREEN);
    setPlayerEye(PLAYER_RIGHT, COLOR_RED);
  } else if (rightScore > leftScore) {
    setPlayerEye(PLAYER_LEFT, COLOR_RED);
    setPlayerEye(PLAYER_RIGHT, COLOR_GREEN);
  } else {
    setPlayerEye(PLAYER_LEFT, COLOR_GREEN);
    setPlayerEye(PLAYER_RIGHT, COLOR_GREEN);
  }

  ledStrip.show();
  delay(2000);
  setAllLeds(COLOR_OFF, true);
}

bool playStopTheLightLevel(uint8_t iterationIntervalMs)
{
  const uint8_t targetLed = randomEight();

  for (uint8_t round = 0; round < 7; ++round) {
    for (uint8_t ledIndex = 0; ledIndex < 9; ++ledIndex) {
      setAllLeds(COLOR_OFF);
      ledStrip.setPixelColor(targetLed, 0, 30, 0);
      setRightEyeLed(targetLed, 0, 30, 0);
      ledStrip.setPixelColor(ledIndex, 30, 0, 0);
      setRightEyeLed(ledIndex, 30, 0, 0);
      ledStrip.show();

      uint8_t elapsedMs = 0;
      while (elapsedMs < iterationIntervalMs) {
        elapsedMs += 5;

        if (isAnyPadPressed()) {
          return ledIndex == targetLed;
        }

        delay(5);
      }
    }
  }

  return false;
}

bool playStopTheLight()
{
  enableRebootOnButton();
  for (uint8_t intervalMs = 200; intervalMs > 50; intervalMs -= 20) {
    if (!playStopTheLightLevel(intervalMs)) {
      showFailure();
      disableRebootOnButton();
      return false;
    }

    while (isAnyPadPressed()) {
      delay(5);
    }
  }

  state = state & B11111110;
  showSuccess();
  disableRebootOnButton();
  return true;
}

void showStopTheLightTwoPlayerFrame(uint8_t targetLed, uint8_t runnerLed, bool showLeft, bool showRight)
{
  setAllLeds(COLOR_OFF);

  if (showLeft) {
    setPlayerEyeLed(PLAYER_LEFT, targetLed, COLOR_GREEN);
    setPlayerEyeLed(PLAYER_LEFT, runnerLed, COLOR_RED);
  }

  if (showRight) {
    setPlayerEyeLed(PLAYER_RIGHT, targetLed, COLOR_GREEN);
    setPlayerEyeLed(PLAYER_RIGHT, runnerLed, COLOR_RED);
  }

  ledStrip.show();
}

void playStopTheLightTwoPlayerLevel(
  uint8_t iterationIntervalMs,
  bool leftActive,
  bool rightActive,
  bool &leftPassed,
  bool &rightPassed
)
{
  const uint8_t targetLed = randomLedIndex();
  bool leftAnswered = !leftActive;
  bool rightAnswered = !rightActive;

  leftPassed = false;
  rightPassed = false;

  for (uint8_t round = 0; round < 7; ++round) {
    for (uint8_t runnerLed = 0; runnerLed < LEDS_PER_EYE; ++runnerLed) {
      showStopTheLightTwoPlayerFrame(
        targetLed,
        runnerLed,
        leftActive && !leftAnswered,
        rightActive && !rightAnswered
      );

      uint8_t elapsedMs = 0;
      while (elapsedMs < iterationIntervalMs && !(leftAnswered && rightAnswered)) {
        const uint8_t pressedMask = getPressedTouchMask();

        if (leftActive && !leftAnswered && (pressedMask & (LEFT_BLUE_MASK | LEFT_RED_MASK | LEFT_GREEN_MASK))) {
          leftAnswered = true;
          leftPassed = runnerLed == targetLed;
        }

        if (rightActive && !rightAnswered && (pressedMask & (RIGHT_BLUE_MASK | RIGHT_RED_MASK | RIGHT_GREEN_MASK))) {
          rightAnswered = true;
          rightPassed = runnerLed == targetLed;
        }

        if (leftAnswered && rightAnswered) {
          return;
        }

        delay(5);
        elapsedMs += 5;
      }

      if (leftAnswered && rightAnswered) {
        return;
      }
    }
  }
}

bool playStopTheLightTwoPlayer()
{
  enableRebootOnButton();
  bool leftActive = true;
  bool rightActive = true;
  uint8_t leftScore = 0;
  uint8_t rightScore = 0;
  uint8_t level = 0;

  for (uint8_t intervalMs = 200; intervalMs > 50; intervalMs -= 20) {
    if (!leftActive && !rightActive) {
      break;
    }

    bool leftPassed = false;
    bool rightPassed = false;
    playStopTheLightTwoPlayerLevel(intervalMs, leftActive, rightActive, leftPassed, rightPassed);

    if (leftActive) {
      if (leftPassed) {
        leftScore = level + 1;
      } else {
        leftActive = false;
        flashPlayerEye(PLAYER_LEFT, COLOR_RED);
      }
    }

    if (rightActive) {
      if (rightPassed) {
        rightScore = level + 1;
      } else {
        rightActive = false;
        flashPlayerEye(PLAYER_RIGHT, COLOR_RED);
      }
    }

    waitForAllTouchPadsReleased();

    if (leftScore != rightScore || (!leftActive && !rightActive)) {
      break;
    }

    ++level;
  }

  showTwoPlayerResult(leftScore, rightScore);
  disableRebootOnButton();
  return leftScore != rightScore;
}

bool isExpectedSequenceButton(uint8_t expectedColor, uint8_t pressedMask)
{
  switch (expectedColor) {
    case 0:
      return pressedMask == LEFT_RED_MASK || pressedMask == RIGHT_RED_MASK;
    case 1:
      return pressedMask == LEFT_GREEN_MASK || pressedMask == RIGHT_GREEN_MASK;
    case 2:
      return pressedMask == LEFT_BLUE_MASK || pressedMask == RIGHT_BLUE_MASK;
    default:
      return false;
  }
}

void showSequenceColor(uint8_t colorIndex)
{
  setAllLeds(colorForIndex(colorIndex));
}

void showSequenceColorForPlayer(uint8_t player, uint8_t colorIndex)
{
  setAllLeds(COLOR_OFF);
  setPlayerEye(player, colorForIndex(colorIndex));
  ledStrip.show();
}

static const uint8_t NUM_SEQUENCE_LEVELS = 10;

void createRandomSequence(uint8_t sequence[], uint8_t sequenceLength)
{
  for (uint8_t sequenceIndex = 0; sequenceIndex < sequenceLength; ++sequenceIndex) {
    sequence[sequenceIndex] = randomColorIndex();
  }
}

bool sequencesMatch(const uint8_t firstSequence[], const uint8_t secondSequence[], uint8_t sequenceLength)
{
  for (uint8_t sequenceIndex = 0; sequenceIndex < sequenceLength; ++sequenceIndex) {
    if (firstSequence[sequenceIndex] != secondSequence[sequenceIndex]) {
      return false;
    }
  }

  return true;
}

void createDifferentRandomSequence(
  const uint8_t existingSequence[],
  uint8_t newSequence[],
  uint8_t sequenceLength
)
{
  createRandomSequence(newSequence, sequenceLength);

  if (!sequencesMatch(existingSequence, newSequence, sequenceLength)) {
    return;
  }

  newSequence[0] = (newSequence[0] + 1) % 3;
}

bool playFollowTheSequence()
{
  enableRebootOnButton();
  uint8_t sequence[NUM_SEQUENCE_LEVELS];
  createRandomSequence(sequence, NUM_SEQUENCE_LEVELS);

  for (uint8_t level = MEMORY_START_LEVEL; level < NUM_SEQUENCE_LEVELS; ++level) {
    for (uint8_t sequenceIndex = 0; sequenceIndex < level; ++sequenceIndex) {
      showSequenceColor(sequence[sequenceIndex]);
      ledStrip.show();
      delay(400);
      setAllLeds(COLOR_OFF, true);
      delay(200);
    }

    for (uint8_t sequenceIndex = 0; sequenceIndex < level; ++sequenceIndex) {
      uint8_t pressedMask = getPressedTouchMask();

      while (pressedMask == 0) {
        pressedMask = getPressedTouchMask();
        delay(TOUCH_POLL_MS);
      }

      showTouchedPads();

      while (isAnyPadPressed()) {
        delay(TOUCH_POLL_MS);
      }

      if (!isExpectedSequenceButton(sequence[sequenceIndex], pressedMask)) {
        showFailure();
        disableRebootOnButton();
        return false;
      }

      setAllLeds(COLOR_OFF, true);
      delay(500);
    }
  }

  state = state & B11111101;
  showSuccess();
  disableRebootOnButton();
  return true;
}

bool pressedMaskToColorIndex(uint8_t pressedMask, uint8_t &colorIndex)
{
  switch (pressedMask) {
    case LEFT_RED_MASK:
    case RIGHT_RED_MASK:
      colorIndex = 0;
      return true;
    case LEFT_GREEN_MASK:
    case RIGHT_GREEN_MASK:
      colorIndex = 1;
      return true;
    case LEFT_BLUE_MASK:
    case RIGHT_BLUE_MASK:
      colorIndex = 2;
      return true;
    default:
      return false;
  }
}

bool pressedMaskToPlayerColorIndex(uint8_t pressedMask, uint8_t player, uint8_t &colorIndex)
{
  const uint8_t playerPressedMask = pressedMask & playerMask(player);

  if (player == PLAYER_LEFT) {
    switch (playerPressedMask) {
      case LEFT_RED_MASK:
        colorIndex = 0;
        return true;
      case LEFT_GREEN_MASK:
        colorIndex = 1;
        return true;
      case LEFT_BLUE_MASK:
        colorIndex = 2;
        return true;
      default:
        return false;
    }
  }

  switch (playerPressedMask) {
    case RIGHT_RED_MASK:
      colorIndex = 0;
      return true;
    case RIGHT_GREEN_MASK:
      colorIndex = 1;
      return true;
    case RIGHT_BLUE_MASK:
      colorIndex = 2;
      return true;
    default:
      return false;
  }
}

bool waitForPlayerColorGuess(uint8_t player, uint8_t &colorIndex)
{
  const uint8_t activeMask = playerMask(player);
  const uint8_t waitingPlayer = player == PLAYER_LEFT ? PLAYER_RIGHT : PLAYER_LEFT;

  setPlayerEye(waitingPlayer, COLOR_ORANGE);
  ledStrip.show();

  while (getPressedTouchMask() & activeMask) {
    delay(TOUCH_POLL_MS);
  }

  uint8_t pressedMask = getPressedTouchMask();
  while ((pressedMask & activeMask) == 0) {
    delay(TOUCH_POLL_MS);
    pressedMask = getPressedTouchMask();
  }

  const bool isKnownColor = pressedMaskToPlayerColorIndex(pressedMask, player, colorIndex);
  setPlayerEye(player, isKnownColor ? colorForIndex(colorIndex) : COLOR_OFF);
  ledStrip.show();

  while (getPressedTouchMask() & activeMask) {
    delay(TOUCH_POLL_MS);
  }

  return isKnownColor;
}

bool playFollowTheSequenceLevelForPlayer(uint8_t player, const uint8_t sequence[], uint8_t level)
{
  for (uint8_t sequenceIndex = 0; sequenceIndex < level; ++sequenceIndex) {
    showSequenceColorForPlayer(player, sequence[sequenceIndex]);
    delay(400);
    setPlayerEye(player, COLOR_OFF);
    ledStrip.show();
    delay(200);
  }

  for (uint8_t sequenceIndex = 0; sequenceIndex < level; ++sequenceIndex) {
    uint8_t guessedColor = 0;
    const bool hasColorGuess = waitForPlayerColorGuess(player, guessedColor);

    if (!hasColorGuess || guessedColor != sequence[sequenceIndex]) {
      return false;
    }

    setPlayerEye(player, COLOR_OFF);
    ledStrip.show();
    delay(250);
  }

  return true;
}

bool playFollowTheSequenceTwoPlayer()
{
  enableRebootOnButton();
  uint8_t leftSequence[NUM_SEQUENCE_LEVELS];
  uint8_t rightSequence[NUM_SEQUENCE_LEVELS];
  createRandomSequence(leftSequence, NUM_SEQUENCE_LEVELS);
  createDifferentRandomSequence(leftSequence, rightSequence, NUM_SEQUENCE_LEVELS);

  bool leftActive = true;
  bool rightActive = true;
  uint8_t leftScore = 0;
  uint8_t rightScore = 0;

  for (uint8_t level = MEMORY_START_LEVEL; level < NUM_SEQUENCE_LEVELS; ++level) {
    if (!leftActive && !rightActive) {
      break;
    }

    if (leftActive) {
      if (playFollowTheSequenceLevelForPlayer(PLAYER_LEFT, leftSequence, level)) {
        leftScore = level;
        flashPlayerEye(PLAYER_LEFT, COLOR_GREEN, 1);
      } else {
        leftActive = false;
        flashPlayerEye(PLAYER_LEFT, COLOR_RED);
      }
    }

    if (rightActive) {
      if (playFollowTheSequenceLevelForPlayer(PLAYER_RIGHT, rightSequence, level)) {
        rightScore = level;
        flashPlayerEye(PLAYER_RIGHT, COLOR_GREEN, 1);
      } else {
        rightActive = false;
        flashPlayerEye(PLAYER_RIGHT, COLOR_RED);
      }
    }

    if (leftScore != rightScore || (!leftActive && !rightActive)) {
      break;
    }
  }

  showTwoPlayerResult(leftScore, rightScore);
  disableRebootOnButton();
  return leftScore != rightScore;
}

void setSequenceSlotColor(uint8_t slotIndex, uint8_t colorIndex)
{
  const RgbColor color = colorForIndex(colorIndex);
  ledStrip.setPixelColor(slotIndex, color.red, color.green, color.blue);
}

void setPlayerSequenceSlotColor(uint8_t player, uint8_t slotIndex, uint8_t colorIndex)
{
  setPlayerEyeLed(player, slotIndex, colorForIndex(colorIndex));
}

void showFindSequenceProgress(const uint8_t sequence[], uint8_t foundLength)
{
  setAllLeds(COLOR_OFF);

  for (uint8_t sequenceIndex = 0; sequenceIndex < foundLength; ++sequenceIndex) {
    setSequenceSlotColor(sequenceIndex, sequence[sequenceIndex]);
  }

  ledStrip.show();
}

void showFindSequenceProgressForPlayer(uint8_t player, const uint8_t sequence[], uint8_t foundLength)
{
  setPlayerEye(player, COLOR_OFF);

  for (uint8_t sequenceIndex = 0; sequenceIndex < foundLength; ++sequenceIndex) {
    setPlayerSequenceSlotColor(player, sequenceIndex, sequence[sequenceIndex]);
  }

  ledStrip.show();
}

void showFindSequencePreview(const uint8_t sequence[])
{
  setAllLeds(COLOR_OFF);

  for (uint8_t sequenceIndex = 0; sequenceIndex < FIND_SEQUENCE_LENGTH; ++sequenceIndex) {
    setSequenceSlotColor(sequenceIndex, sequence[sequenceIndex]);
  }

  ledStrip.show();
  delay(FIND_SEQUENCE_PREVIEW_MS);
  showFindSequenceProgress(sequence, 0);
}

void showFindSequencePreviewForPlayer(uint8_t player, const uint8_t sequence[])
{
  const uint8_t waitingPlayer = player == PLAYER_LEFT ? PLAYER_RIGHT : PLAYER_LEFT;

  setAllLeds(COLOR_OFF);
  setPlayerEye(waitingPlayer, COLOR_ORANGE);

  for (uint8_t sequenceIndex = 0; sequenceIndex < FIND_SEQUENCE_LENGTH; ++sequenceIndex) {
    setPlayerSequenceSlotColor(player, sequenceIndex, sequence[sequenceIndex]);
  }

  ledStrip.show();
  delay(FIND_SEQUENCE_PREVIEW_MS);
  setAllLeds(COLOR_OFF, true);
}

bool waitForColorGuess(uint8_t &colorIndex)
{
  uint8_t pressedMask = getPressedTouchMask();

  while (pressedMask == 0) {
    pressedMask = getPressedTouchMask();
    delay(TOUCH_POLL_MS);
  }

  showTouchedPads();
  const bool isKnownColor = pressedMaskToColorIndex(pressedMask, colorIndex);
  waitForAllTouchPadsReleased();

  return isKnownColor;
}

bool playFindTheSequence(uint8_t startingLives = FIND_SEQUENCE_STARTING_LIVES)
{
  enableRebootOnButton();
  uint8_t sequence[FIND_SEQUENCE_LENGTH];
  createRandomSequence(sequence, FIND_SEQUENCE_LENGTH);

  uint8_t livesRemaining = startingLives;
  uint8_t foundLength = 0;
  showFindSequencePreview(sequence);

  while (foundLength < FIND_SEQUENCE_LENGTH && livesRemaining > 0) {
    uint8_t guessedColor = 0;
    const bool hasColorGuess = waitForColorGuess(guessedColor);

    if (hasColorGuess && guessedColor == sequence[foundLength]) {
      ++foundLength;
      showFindSequenceProgress(sequence, foundLength);
      delay(500);
      continue;
    }

    --livesRemaining;
    foundLength = 0;
    showFailure();
    showFindSequenceProgress(sequence, foundLength);
  }

  const bool wonGame = foundLength == FIND_SEQUENCE_LENGTH;

  if (wonGame) {
    state = state & B11111011;
    showSuccess();
  }

  disableRebootOnButton();
  return wonGame;
}

uint8_t playFindTheSequenceForPlayer(
  uint8_t player,
  const uint8_t sequence[],
  uint8_t scoreNeededToWin = FIND_SEQUENCE_LENGTH
)
{
  if (scoreNeededToWin > FIND_SEQUENCE_LENGTH) {
    scoreNeededToWin = FIND_SEQUENCE_LENGTH;
  }

  uint8_t foundLength = 0;
  showFindSequenceProgressForPlayer(player, sequence, foundLength);

  while (foundLength < FIND_SEQUENCE_LENGTH) {
    uint8_t guessedColor = 0;
    const bool hasColorGuess = waitForPlayerColorGuess(player, guessedColor);

    if (!hasColorGuess || guessedColor != sequence[foundLength]) {
      showFindSequenceProgressForPlayer(player, sequence, 0);
      flashPlayerEye(player, COLOR_RED);
      return foundLength;
    }

    ++foundLength;
    showFindSequenceProgressForPlayer(player, sequence, foundLength);
    delay(300);

    if (foundLength >= scoreNeededToWin) {
      flashPlayerEye(player, COLOR_GREEN, 2);
      return foundLength;
    }
  }

  return foundLength;
}

bool playFindTheSequenceTwoPlayer()
{
  enableRebootOnButton();
  uint8_t leftSequence[FIND_SEQUENCE_LENGTH];
  uint8_t rightSequence[FIND_SEQUENCE_LENGTH];
  createRandomSequence(leftSequence, FIND_SEQUENCE_LENGTH);
  createDifferentRandomSequence(leftSequence, rightSequence, FIND_SEQUENCE_LENGTH);

  showFindSequencePreviewForPlayer(PLAYER_LEFT, leftSequence);
  const uint8_t leftScore = playFindTheSequenceForPlayer(PLAYER_LEFT, leftSequence);
  delay(300);

  uint8_t rightScoreNeededToWin = leftScore + 1;
  if (rightScoreNeededToWin > FIND_SEQUENCE_LENGTH) {
    rightScoreNeededToWin = FIND_SEQUENCE_LENGTH;
  }

  showFindSequencePreviewForPlayer(PLAYER_RIGHT, rightSequence);
  const uint8_t rightScore = playFindTheSequenceForPlayer(
    PLAYER_RIGHT,
    rightSequence,
    rightScoreNeededToWin
  );

  showTwoPlayerResult(leftScore, rightScore);
  disableRebootOnButton();
  return leftScore != rightScore;
}

const uint8_t KNIGHT_RIDER_LEDS[] PROGMEM = {
  2, 255,
  1, 3,
  0, 4,
  8, 5,
  6, 7,
  7, 10,
  11, 9,
  12, 17,
  13, 16,
  14, 15,
  15, 255,
};

uint8_t knightRider(uint16_t step, uint8_t red, uint8_t green, uint8_t blue)
{
  uint8_t position = step % 20;
  if (position > 10) {
    position = 20 - position;
  }

  const uint8_t tableIndex = position * 2;
  const uint8_t firstLed = pgm_read_byte(&KNIGHT_RIDER_LEDS[tableIndex]);
  const uint8_t secondLed = pgm_read_byte(&KNIGHT_RIDER_LEDS[tableIndex + 1]);

  setAllLeds(COLOR_OFF);
  ledStrip.setPixelColor(firstLed, red, green, blue);

  if (secondLed != 255) {
    ledStrip.setPixelColor(secondLed, red, green, blue);
  }

  ledStrip.show();
  return 100;
}

uint8_t policeMode(uint16_t step)
{
  uint8_t initial = step % 2;
  setAllLeds(10,0,0);
  for (uint8_t i = 0 + initial; i < 18; i = i + 2)
  {
    ledStrip.setPixelColor(i, 0,0,255);
  }
  ledStrip.show();
  return 100;
}

  const uint8_t INFINITI_LEDS[] PROGMEM = {
   7, 8, 0, 1, 2, 3, 4, 5, 6, 10, 9, 17, 16, 15, 14, 13, 12, 11,
};

uint8_t devsecopsMode(uint16_t step)
{
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;

 step = step % 90;
  
  if (step < 18)
  {
    r = 30; 
  }
  else if (step < 36)
  {
    g = 8;
  }
  else if (step < 54)
  {
    r = 20;
  g = 8;
  }
  else if (step < 72)
  {
    r = 6;
    b = 30;
  }
    else if (step < 90)
  {
    g = 5;
    b = 30;
  }
  uint8_t pos = step % 18;
  ledStrip.setPixelColor(pgm_read_byte(&INFINITI_LEDS[pos]), r,g,b);
  ledStrip.show();
  return 50;
}

uint8_t nuclearMode(uint16_t step, uint8_t spacing, uint8_t r, uint8_t g, uint8_t b, uint8_t r2, uint8_t g2, uint8_t b2, uint8_t ret )
{
  uint8_t initial = step % 3;
  setAllLeds(r,g,b);
  for (uint8_t i = 0 + initial; i < 18; i = i + spacing)
  {
    ledStrip.setPixelColor(i, r2, g2, b2);
  }
  ledStrip.show();
  return ret;
}

const uint8_t SPIN_LEDS_LEFT[] PROGMEM = {
    7, 8, 0, 1, 2, 3, 4, 5, 6, 
};

const uint8_t SPIN_LEDS_RIGHT[] PROGMEM = {
  10, 9, 17, 16, 15, 14, 13, 12, 11,
};

uint8_t spinMode(uint16_t step, uint8_t r, uint8_t g, uint8_t b, uint8_t r2, uint8_t g2, uint8_t b2, uint8_t ret, uint8_t offset = 0 )
{
  step = step % 9;
  setAllLeds(0,0,0);
  for (uint8_t i = 0; i < 7; i++ )
  {
    ledStrip.setPixelColor(pgm_read_byte(&SPIN_LEDS_LEFT[(step + i) % 9]), r * i,g * i,b * i);
    ledStrip.setPixelColor(pgm_read_byte(&SPIN_LEDS_RIGHT[(step + i + offset) % 9]), r2 * i,g2 * i,b2 * i);
  }
  ledStrip.show();
  return ret;
}

uint8_t breath(uint16_t step, uint8_t r, uint8_t g, uint8_t b )
{
  step = step % 60;
  if (step > 30)
  {
    step = 60 - step;
  }
  setAllLeds(r * step, g * step, b * step, true);
  if (step == 0 )
  {
    return 200;
  }
  return 30;
}

uint8_t timer(uint16_t step)
{
  const uint8_t frameMs = 100;

  uint16_t multiplier = 63; // tweak for timing accuracy

  const uint8_t eyeLedCount = 9;
  const uint8_t totalMinutes = 9;

  const uint16_t preFlashSteps = 3000 / frameMs;      // 3 seconds
  const uint16_t halfSecondSteps = 500 / frameMs;     // 0.5 seconds
  const uint16_t minuteSteps = multiplier * eyeLedCount;
  const uint16_t timerSteps = minuteSteps * totalMinutes;
  const uint16_t finishedSteps = 5000 / frameMs;      // 5 seconds

  const uint16_t totalSteps = preFlashSteps + timerSteps + finishedSteps;
  step = step % totalSteps;

  // Clear both eyes.
  setAllLeds(0, 0, 0);

  // First 3 seconds: flash on/off every half second.
  if (step < preFlashSteps) {
    const bool flashOn = ((step / halfSecondSteps) % 2) == 0;

    if (flashOn) {
      setAllLeds(10, 0, 0);
    }

    ledStrip.show();
    return frameMs;
  }

  step -= preFlashSteps;

  // Timer phase.
  if (step < timerSteps) {
    const uint8_t completedMinutes = step / minuteSteps;
    const uint16_t currentMinuteStep = step % minuteSteps;

    uint8_t rightEyeLedsLit = (currentMinuteStep / multiplier) + 1;

    if (rightEyeLedsLit > eyeLedCount) {
      rightEyeLedsLit = eyeLedCount;
    }

    // Left eye: one light per completed minute.
    for (uint8_t ledIndex = 0; ledIndex < completedMinutes; ++ledIndex) {
      ledStrip.setPixelColor(ledIndex, 10, 0, 0);
    }

    // Right eye: progress through the current minute.
    for (uint8_t ledIndex = 0; ledIndex < rightEyeLedsLit; ++ledIndex) {
      setRightEyeLed(ledIndex, 10, 0, 0);
    }

    ledStrip.show();
    return frameMs;
  }

  // Finished phase: both eyes green for 5 seconds.
  setAllLeds(0, 30, 0, true);
  return frameMs;
}


int runAnimationMode(uint8_t mode, uint16_t step)
{
  switch (mode) {
    case 0:
      return knightRider(step, 0, 10, 0);
    case 1:
      return breath(step, 1, 0 , 0);
    case 2:
      return loopingEyes(step, 0, 0, 10);
    case 3:
      return knightRider(step, 10, 0, 0);
    case 4:
      return loopingEyes(step, 10, 0, 0);
    case 5:
      if ((state & B00000111) != 0) { return 0; };
      return devsecopsMode(step);
    case 6:
      if ((state & B00000010) != 0) { return 0; };
      return nuclearMode(step, 3, 0, 0, 0, 10, 20, 0, 150 );
    case 7:
      if ((state & B00000100) != 0) { return 0; };
      return nuclearMode(4, 3, 12, 20, 255, 0, 2, 0, 250 ); // york rose
    case 8:
      if ((state & B00000001) != 0) { return 0; };
      return policeMode(step);
    case 9:
      return spinMode(step, 1, 0, 3, 0,0,3,75);
    case 10:
      return timer(step);
    default:
      return -1;
  }
}

void handleWakeButtonPress(
  uint16_t heldMs,
  uint8_t &animationMode,
  uint16_t &animationStep,
  uint32_t &totalIntervalMs
)
{
  if (heldMs > LONG_PRESS_THRESHOLD_MS) {
    setAllLeds(COLOR_RED, true);
    waitForWakeButtonReleased();

    for (uint8_t cycle = 0; cycle < SLEEP_DEBOUNCE_CYCLES; ++cycle) {
      miniSleep();
    }

    enterSleep();
  } else {
    const uint8_t pressedMask = getPressedTouchMask();
    waitForAllTouchPadsReleased();

    if (pressedMask != 0) {
      seedGameRandom();
    }

    switch (pressedMask) {
      case 0:
        ++animationMode;
        break;
      case LEFT_BLUE_MASK:
        playStopTheLight();
        break;
      case LEFT_RED_MASK:
        playFindTheSequence();
        break;
      case LEFT_GREEN_MASK:
        playFollowTheSequence();
        break;
      case RIGHT_BLUE_MASK:
        playStopTheLightTwoPlayer();
        break;
      case RIGHT_RED_MASK:
        playFindTheSequenceTwoPlayer();
        break;
      case RIGHT_GREEN_MASK:
        playFollowTheSequenceTwoPlayer();
        break;
    }
  }

  animationStep = 0;
  totalIntervalMs = 0;
}

void setup()
{
  pinMode(PIN_PA1, OUTPUT);
  pinMode(PIN_PA2, OUTPUT);
  pinMode(PIN_PB2, OUTPUT);
  pinMode(PIN_PB3, OUTPUT);

  pinMode(LED_DATA_PIN, OUTPUT);
  pinMode(WAKE_BUTTON_PIN, INPUT_PULLUP);
  digitalWrite(LED_POWER_PIN, HIGH);

  initRtc();
  setupTouchButtons();

  state = EEPROM.read(0);
}

void loop()
{
  ledStrip.begin();
  enableRtcPtc();

  uint16_t animationStep = 0;
  uint8_t animationMode = 5;
  uint32_t totalIntervalMs = 0;

  while (true) {
    int intervalMs = runAnimationMode(animationMode, animationStep);

    if (intervalMs == 0) {
      animationMode++;
    }

    if (intervalMs < 0) {
      animationMode = 0;
      continue;
    }

    ++animationStep;
    totalIntervalMs += intervalMs;

    while (intervalMs > 0) {
      showTouchedPads();
      delay(TOUCH_POLL_MS);
      intervalMs -= TOUCH_POLL_MS;

      const uint16_t buttonLowTime = measureWakeButtonLowTime(MAX_BUTTON_HOLD_MS);
      if (buttonLowTime > SHORT_PRESS_THRESHOLD_MS) {
        handleWakeButtonPress(buttonLowTime, animationMode, animationStep, totalIntervalMs);
      }
      else if (buttonLowTime > 0) {
        animationStep = 0;
      }
    }

    if (totalIntervalMs > WAKE_TIMEOUT_MS) {
      totalIntervalMs = 0;
      animationStep = 0;
      enterSleep();
    }
  }
}
