/*
  LeafAudioExperiement

  ESP32-S3 vario audio test rig using Leaf's current square-wave PWM method.

  Hardware:
    GPIO10 -> piezo terminal A
    GPIO11 -> piezo terminal B, complementary/inverted output
    GPIO4  -> analog climb-rate input

  Analog mapping:
    0.0 V -> -10.65 m/s
    3.098 V -> +12.02 m/s

  Keep the analog input between GND and 3.3 V. If using an adjustable bench
  supply that can exceed 3.3 V, use a divider and common ground.
*/

#include <Arduino.h>

static constexpr uint8_t PWM_OUT_A_PIN = 10;
static constexpr uint8_t PWM_OUT_B_PIN = 11;
static constexpr uint8_t ANALOG_CLIMB_PIN = 4;

static constexpr uint8_t PWM_CHANNEL_A = 0;
static constexpr uint8_t PWM_CHANNEL_B = 1;
static constexpr uint8_t PWM_RESOLUTION_BITS = 10;
static constexpr uint16_t ADC_MAX_MV = 3098;

static constexpr int32_t CLIMB_INPUT_MIN_CMS = -1065;
static constexpr int32_t CLIMB_INPUT_MAX_CMS = 1202;

// Copied from Leaf src/vario/ui/audio/speaker.cpp.
static constexpr unsigned long NOTE_DURATION_MS = 40;
static constexpr unsigned long TIMING_TOLERANCE_MS = 2;
static constexpr bool QUANTIZE_TO_QUARTERTONE = true;
static constexpr uint8_t QUARTERTONE_HYSTERESIS_PERCENT = 35;
static constexpr uint16_t MIN_PITCH_SEGMENT_MS = 80;

// Copied from Leaf src/vario/ui/audio/dynamic_effects.h and default settings.
static constexpr int32_t CLIMB_MAX = 800;
static constexpr int32_t CLIMB_NOTE_MIN = 523;
static constexpr int32_t CLIMB_NOTE_MAX = 1568;
static constexpr uint16_t CLIMB_NOTE_MAXMAX = 2093;

static constexpr int32_t SINK_MAX = -800;
static constexpr int32_t SINK_NOTE_MIN = 392;
static constexpr int32_t SINK_NOTE_MAX = 196;
static constexpr uint16_t SINK_NOTE_MAXMAX = 131;

static constexpr uint16_t CLIMB_PLAY_SAMPLES_MAX = 10;
static constexpr uint16_t CLIMB_PLAY_SAMPLES_MIN = 1;
static constexpr uint16_t CLIMB_REST_SAMPLES_MAX = 6;
static constexpr uint16_t CLIMB_REST_SAMPLES_MIN = 1;

static constexpr int32_t SINK_PLAY_SAMPLES_MIN = 8;
static constexpr int32_t SINK_PLAY_SAMPLES_MAX = 8;
static constexpr int32_t SINK_REST_SAMPLES_MIN = 20;
static constexpr int32_t SINK_REST_SAMPLES_MAX = 20;

static constexpr int32_t DEFAULT_CLIMB_START_CMS = 5;
static constexpr int32_t DEFAULT_SINK_ALARM_CMS = -250;

static constexpr uint16_t QUARTERTONE_NOTES[] = {
    131, 135, 139, 143, 147, 151, 156, 160, 165, 170, 175, 180,
    185, 191, 196, 202, 208, 214, 220, 227, 233, 240, 247, 255,
    262, 270, 278, 286, 294, 303, 312, 321, 330, 340, 350, 360,
    371, 381, 393, 404, 416, 428, 441, 454, 467, 481, 495, 509,
    524, 539, 555, 571, 588, 605, 623, 641, 660, 680, 699, 720,
    741, 763, 785, 808, 832, 856, 881, 907, 934, 961, 989, 1018,
    1048, 1079, 1110, 1143, 1176, 1211, 1246, 1283, 1320, 1359, 1399, 1440,
    1482, 1526, 1570, 1616, 1664, 1712, 1763, 1814, 1867, 1922, 1978, 2036,
    2093};

static uint16_t rawVarioNote = 0;
static uint16_t pendingVarioNote = 0;
static uint16_t latchedVarioNote = 0;
static int8_t quartertoneNoteIndex = -1;
static uint16_t varioPlaySamples = CLIMB_PLAY_SAMPLES_MAX;
static uint16_t latchedVarioPlaySamples = CLIMB_PLAY_SAMPLES_MAX;
static uint16_t varioRestSamples = CLIMB_REST_SAMPLES_MAX;
static uint16_t varioPlaySampleCount = 0;
static uint16_t varioRestSampleCount = 0;
static uint16_t pitchSegmentSampleCount = 0;
static uint16_t lastToneHz = 0;
static bool betweenVarioBeeps = false;

static unsigned long tLastUpdate = 0;
static uint32_t lastStatusPrintMs = 0;
static int32_t verticalRateCms = 0;
static uint32_t analogMv = 0;

void playTone(uint16_t freqHz) {
  if (freqHz == lastToneHz) {
    return;
  }

  if (freqHz == 0) {
    ledcWriteTone(PWM_OUT_A_PIN, 0);
    ledcWriteTone(PWM_OUT_B_PIN, 0);
  } else {
    ledcWriteTone(PWM_OUT_A_PIN, freqHz);
    ledcWriteTone(PWM_OUT_B_PIN, freqHz);
  }

  lastToneHz = freqHz;
}

bool shouldUpdateSpeaker() {
  unsigned long tNow = millis();
  uint32_t n = (tNow + TIMING_TOLERANCE_MS - tLastUpdate) / NOTE_DURATION_MS;
  if (n == 0) {
    return false;
  }
  tLastUpdate += n * NOTE_DURATION_MS;
  return true;
}

int32_t climbFromAnalogMv(uint32_t mv) {
  if (mv > ADC_MAX_MV) {
    mv = ADC_MAX_MV;
  }

  int32_t span = CLIMB_INPUT_MAX_CMS - CLIMB_INPUT_MIN_CMS;
  return CLIMB_INPUT_MIN_CMS + int32_t((int64_t(mv) * span + ADC_MAX_MV / 2) / ADC_MAX_MV);
}

size_t quartertoneNoteCount() {
  return sizeof(QUARTERTONE_NOTES) / sizeof(QUARTERTONE_NOTES[0]);
}

int8_t nearestQuartertoneIndex(uint16_t hz) {
  int8_t bestIndex = 0;
  uint16_t bestDelta = abs(int(hz) - int(QUARTERTONE_NOTES[0]));
  for (size_t i = 1; i < quartertoneNoteCount(); i++) {
    uint16_t delta = abs(int(hz) - int(QUARTERTONE_NOTES[i]));
    if (delta < bestDelta) {
      bestIndex = int8_t(i);
      bestDelta = delta;
    }
  }
  return bestIndex;
}

uint16_t quantizeToQuartertone(uint16_t hz) {
  if (hz == 0) {
    quartertoneNoteIndex = -1;
    return 0;
  }

  if (quartertoneNoteIndex < 0) {
    quartertoneNoteIndex = nearestQuartertoneIndex(hz);
    return QUARTERTONE_NOTES[quartertoneNoteIndex];
  }

  int8_t desiredIndex = quartertoneNoteIndex;
  while (desiredIndex > 0) {
    uint16_t lowerNote = QUARTERTONE_NOTES[desiredIndex - 1];
    uint16_t currentNote = QUARTERTONE_NOTES[desiredIndex];
    uint16_t stepHz = currentNote - lowerNote;
    uint16_t hysteresisHz = uint16_t((uint32_t(stepHz) * QUARTERTONE_HYSTERESIS_PERCENT + 50U) / 100U);
    uint16_t switchDownHz = (lowerNote + currentNote) / 2U - hysteresisHz;
    if (hz >= switchDownHz) {
      break;
    }
    desiredIndex--;
  }

  while (size_t(desiredIndex + 1) < quartertoneNoteCount()) {
    uint16_t currentNote = QUARTERTONE_NOTES[desiredIndex];
    uint16_t upperNote = QUARTERTONE_NOTES[desiredIndex + 1];
    uint16_t stepHz = upperNote - currentNote;
    uint16_t hysteresisHz = uint16_t((uint32_t(stepHz) * QUARTERTONE_HYSTERESIS_PERCENT + 50U) / 100U);
    uint16_t switchUpHz = (currentNote + upperNote) / 2U + hysteresisHz;
    if (hz <= switchUpHz) {
      break;
    }
    desiredIndex++;
  }

  quartertoneNoteIndex = desiredIndex;

  return QUARTERTONE_NOTES[quartertoneNoteIndex];
}

void updateVarioNote(int32_t verticalRate) {
  uint16_t newVarioNote = 0;
  uint16_t newVarioPlaySamples = varioPlaySamples;
  uint16_t newVarioRestSamples = varioRestSamples;

  if (verticalRate > DEFAULT_CLIMB_START_CMS) {
    if (verticalRate >= CLIMB_MAX) {
      newVarioNote = verticalRate * (CLIMB_NOTE_MAX - CLIMB_NOTE_MIN) / CLIMB_MAX + CLIMB_NOTE_MIN;
      if (newVarioNote > CLIMB_NOTE_MAXMAX) {
        newVarioNote = CLIMB_NOTE_MAXMAX;
      }
      newVarioPlaySamples = CLIMB_PLAY_SAMPLES_MIN;
      newVarioRestSamples = 0;
    } else {
      newVarioNote = verticalRate * (CLIMB_NOTE_MAX - CLIMB_NOTE_MIN) / CLIMB_MAX + CLIMB_NOTE_MIN;
      newVarioPlaySamples =
          CLIMB_PLAY_SAMPLES_MAX -
          (verticalRate * (CLIMB_PLAY_SAMPLES_MAX - CLIMB_PLAY_SAMPLES_MIN) / CLIMB_MAX);
      newVarioRestSamples =
          CLIMB_REST_SAMPLES_MAX -
          (verticalRate * (CLIMB_REST_SAMPLES_MAX - CLIMB_REST_SAMPLES_MIN) / CLIMB_MAX);
    }
  } else if (verticalRate < DEFAULT_SINK_ALARM_CMS) {
    if (verticalRate <= SINK_MAX) {
      newVarioNote = SINK_NOTE_MIN - verticalRate * (SINK_NOTE_MIN - SINK_NOTE_MAX) / SINK_MAX;
      if (newVarioNote < SINK_NOTE_MAXMAX || newVarioNote > SINK_NOTE_MAX) {
        newVarioNote = SINK_NOTE_MAXMAX;
      }
      newVarioPlaySamples = SINK_PLAY_SAMPLES_MAX;
      newVarioRestSamples = 0;
    } else {
      newVarioNote = SINK_NOTE_MIN - verticalRate * (SINK_NOTE_MIN - SINK_NOTE_MAX) / SINK_MAX;
      newVarioPlaySamples =
          SINK_PLAY_SAMPLES_MIN +
          (verticalRate * (SINK_PLAY_SAMPLES_MAX - SINK_PLAY_SAMPLES_MIN) / SINK_MAX);
      newVarioRestSamples =
          SINK_REST_SAMPLES_MIN +
          (verticalRate * (SINK_REST_SAMPLES_MAX - SINK_REST_SAMPLES_MIN) / SINK_MAX);
    }
  } else {
    newVarioNote = 0;
  }

  rawVarioNote = newVarioNote;
  pendingVarioNote = QUANTIZE_TO_QUARTERTONE ? quantizeToQuartertone(newVarioNote) : newVarioNote;
  varioPlaySamples = newVarioPlaySamples;
  varioRestSamples = newVarioRestSamples;
}

uint16_t minPitchSegmentSamples() {
  uint16_t oneThirdSamples = latchedVarioPlaySamples / 3U;
  uint16_t minMsSamples = (MIN_PITCH_SEGMENT_MS + NOTE_DURATION_MS - 1U) / NOTE_DURATION_MS;
  return max<uint16_t>(oneThirdSamples, minMsSamples);
}

bool canChangePitchWithinBeep() {
  if (latchedVarioPlaySamples < 4) {
    return false;
  }

  uint16_t minSegmentSamples = minPitchSegmentSamples();
  uint16_t remainingSamples = latchedVarioPlaySamples - varioPlaySampleCount;
  return pitchSegmentSampleCount >= minSegmentSamples && remainingSamples >= minSegmentSamples;
}

void updateVarioAudio() {
  if (!shouldUpdateSpeaker()) {
    return;
  }

  if (pendingVarioNote == 0) {
    playTone(0);
    latchedVarioNote = 0;
    varioPlaySampleCount = 0;
    varioRestSampleCount = 0;
    pitchSegmentSampleCount = 0;
    betweenVarioBeeps = false;
    return;
  }

  if (betweenVarioBeeps) {
    playTone(0);
    if (++varioRestSampleCount >= varioRestSamples) {
      varioRestSampleCount = 0;
      betweenVarioBeeps = false;
      latchedVarioNote = 0;
      pitchSegmentSampleCount = 0;
    }
  } else {
    if (varioPlaySampleCount == 0) {
      latchedVarioPlaySamples = varioPlaySamples;
      latchedVarioNote = pendingVarioNote;
      pitchSegmentSampleCount = 0;
    } else if (latchedVarioNote != pendingVarioNote && canChangePitchWithinBeep()) {
      latchedVarioNote = pendingVarioNote;
      pitchSegmentSampleCount = 0;
    }

    if (latchedVarioNote == 0) {
      playTone(0);
      return;
    }

    playTone(latchedVarioNote);
    pitchSegmentSampleCount++;
    if (++varioPlaySampleCount >= latchedVarioPlaySamples) {
      varioPlaySampleCount = 0;
      latchedVarioNote = 0;
      pitchSegmentSampleCount = 0;
      if (varioRestSamples) {
        betweenVarioBeeps = true;
      }
    }
  }
}

void printStatus() {
  if (millis() - lastStatusPrintMs < 1000) {
    return;
  }
  lastStatusPrintMs = millis();

  Serial.print(F("ADC "));
  Serial.print(analogMv);
  Serial.print(F(" mV -> "));
  Serial.print(float(verticalRateCms) / 100.0f, 2);
  Serial.print(F(" m/s, raw "));
  Serial.print(rawVarioNote);
  Serial.print(F(" Hz, pending "));
  Serial.print(pendingVarioNote);
  Serial.print(F(" Hz, latched "));
  Serial.print(latchedVarioNote);
  Serial.print(F(" Hz, play target "));
  Serial.print(varioPlaySamples * NOTE_DURATION_MS);
  Serial.print(F(" ms, play latched "));
  Serial.print(latchedVarioPlaySamples * NOTE_DURATION_MS);
  Serial.print(F(" ms, pitch segment min "));
  Serial.print(minPitchSegmentSamples() * NOTE_DURATION_MS);
  Serial.print(F(" ms, rest "));
  Serial.print(varioRestSamples * NOTE_DURATION_MS);
  Serial.println(F(" ms"));
}

void setup() {
  Serial.begin(115200);
  uint32_t serialStart = millis();
  while (!Serial && millis() - serialStart < 2500) {
    delay(10);
  }

  pinMode(PWM_OUT_A_PIN, OUTPUT);
  pinMode(PWM_OUT_B_PIN, OUTPUT);
  bool pwmAOk = ledcAttachChannel(PWM_OUT_A_PIN, 1000, PWM_RESOLUTION_BITS, PWM_CHANNEL_A);
  bool pwmBOk = ledcAttachChannel(PWM_OUT_B_PIN, 1000, PWM_RESOLUTION_BITS, PWM_CHANNEL_B);
  ledcOutputInvert(PWM_OUT_B_PIN, true);
  playTone(0);

  analogReadResolution(12);
  analogSetPinAttenuation(ANALOG_CLIMB_PIN, ADC_11db);

  tLastUpdate = NOTE_DURATION_MS * (millis() / NOTE_DURATION_MS);

  Serial.println();
  Serial.println(F("LeafAudioExperiement PWM vario rig"));
  Serial.print(F("PWM GPIO"));
  Serial.print(PWM_OUT_A_PIN);
  Serial.print(F("/GPIO"));
  Serial.print(PWM_OUT_B_PIN);
  Serial.print(F(" attach "));
  Serial.println((pwmAOk && pwmBOk) ? F("OK") : F("FAILED"));
  Serial.print(F("Analog climb input GPIO"));
  Serial.println(ANALOG_CLIMB_PIN);
  Serial.println(F("Input mapping: 0.0 V = -10.65 m/s, 3.098 V = +12.02 m/s"));
}

void loop() {
  analogMv = analogReadMilliVolts(ANALOG_CLIMB_PIN);
  verticalRateCms = climbFromAnalogMv(analogMv);
  updateVarioNote(verticalRateCms);
  updateVarioAudio();
  printStatus();
}
