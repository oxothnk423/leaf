/*
  DRV2667 Leaf-style vario tone simulator for ESP32-S3.

  Hardware:
    ESP32-S3 WROOM-1 dev kit
    DRV2667 SDA -> GPIO8
    DRV2667 SCL -> GPIO9

  Serial commands:
    signed decimal  Set simulated climb rate in m/s, for example 0.1, 2.3, -4.9
    1.0,6.0,1.0    Slew through climb rates, using ramp time per segment
    ramp 5000       Set ramp time per segment in ms
    sweep 500 1600 1 250
                    Sweep start/end Hz, synth-byte step, beep ms
    climb 1.0       Same as above, useful for whole-number climb rates
    0..99           Set amplitude, 0 = off, 99 = max
    gain 0..3       Set DRV2667 output gain, 0 = lowest
    drv/pwm/both    Select DRV2667 synth, ESP32 square PWM, or both
    pwmtest 1000    Play PWM pins directly for 2 seconds
    beep            Play one datasheet-style test beep
    stop/off        Stop vario simulation
    scan/dump/init  I2C diagnostics
    ?/help          Print help

  This sketch uses the DRV2667 waveform synthesizer, not FIFO streaming. It
  converts Leaf's current climb-rate-to-frequency and play/rest math into
  one-shot synth chunks with a soft envelope.
*/

#include <Arduino.h>
#include <Wire.h>

static constexpr uint8_t I2C_SDA_PIN = 8;
static constexpr uint8_t I2C_SCL_PIN = 9;
static constexpr uint8_t PWM_OUT_A_PIN = 10;
static constexpr uint8_t PWM_OUT_B_PIN = 11;
static constexpr uint32_t I2C_CLOCK_HZ = 200000;

static constexpr uint8_t DRV2667_ADDR = 0x59;
static constexpr uint8_t REG_STATUS = 0x00;
static constexpr uint8_t REG_CONTROL_1 = 0x01;
static constexpr uint8_t REG_CONTROL_2 = 0x02;
static constexpr uint8_t REG_WAVEFORM_0 = 0x03;
static constexpr uint8_t REG_WAVEFORM_1 = 0x04;
static constexpr uint8_t REG_PAGE = 0xFF;

static constexpr uint16_t TICK_MS = 40;

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
static constexpr uint8_t RAMP_MAX_POINTS = 8;
static constexpr uint16_t RAMP_PRINT_INTERVAL_MS = 500;
static constexpr uint8_t PWM_RESOLUTION_BITS = 10;
static constexpr uint8_t PWM_CHANNEL_A = 0;
static constexpr uint8_t PWM_CHANNEL_B = 1;

enum OutputMode {
  OUTPUT_DRV2667,
  OUTPUT_PWM,
  OUTPUT_BOTH
};

static uint8_t volumePercent = 50;
static uint8_t gainSetting = 0;
static OutputMode outputMode = OUTPUT_DRV2667;
static int32_t climbRateCms = 0;
static bool varioEnabled = false;
static bool inRest = false;
static uint32_t nextVarioEventMs = 0;
static int32_t rampPointsCms[RAMP_MAX_POINTS];
static uint8_t rampPointCount = 0;
static uint8_t rampSegmentIndex = 0;
static uint32_t rampSegmentStartMs = 0;
static uint32_t rampSegmentMs = 5000;
static uint32_t lastRampPrintMs = 0;
static bool rampActive = false;
static bool sweepActive = false;
static int16_t sweepCurrentByte = 0;
static int16_t sweepEndByte = 0;
static int8_t sweepStepBytes = 1;
static uint16_t sweepBeepMs = 250;
static uint16_t sweepGapMs = 80;
static uint32_t nextSweepEventMs = 0;
static bool pwmToneActive = false;
static uint32_t pwmToneOffAtMs = 0;

struct VarioTone {
  bool active;
  uint16_t frequencyHz;
  uint16_t playMs;
  uint16_t restMs;
};

const __FlashStringHelper *i2cErrorName(uint8_t code) {
  switch (code) {
    case 0: return F("OK");
    case 1: return F("data too long");
    case 2: return F("NACK address");
    case 3: return F("NACK data");
    case 4: return F("other error");
    case 5: return F("timeout");
    default: return F("unknown");
  }
}

bool writeReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(DRV2667_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool writeBlock(uint8_t startReg, const uint8_t *data, size_t len) {
  Wire.beginTransmission(DRV2667_ADDR);
  Wire.write(startReg);
  for (size_t i = 0; i < len; i++) {
    Wire.write(data[i]);
  }
  return Wire.endTransmission() == 0;
}

bool selectPage(uint8_t page) {
  return writeReg(REG_PAGE, page);
}

bool readReg(uint8_t reg, uint8_t &value) {
  Wire.beginTransmission(DRV2667_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(DRV2667_ADDR, uint8_t(1)) != 1) {
    return false;
  }
  value = Wire.read();
  return true;
}

uint8_t probeAddress(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission();
}

uint8_t synthFrequencyByte(uint16_t hz) {
  uint32_t rounded = (uint32_t(hz) * 128UL + 500UL) / 1000UL;
  return uint8_t(constrain(rounded, 1UL, 255UL));
}

uint16_t synthFrequencyHz(uint8_t freqByte) {
  return uint16_t((uint32_t(freqByte) * 1000UL + 64UL) / 128UL);
}

uint8_t amplitudeByte() {
  return uint8_t((uint16_t(volumePercent) * 255U + 49U) / 99U);
}

bool stopPlayback() {
  bool ok = true;
  ok &= selectPage(0x00);
  ok &= writeReg(REG_CONTROL_2, 0x00);
  ledcWriteTone(PWM_OUT_A_PIN, 0);
  ledcWriteTone(PWM_OUT_B_PIN, 0);
  pwmToneActive = false;
  return ok;
}

void startPwmTone(uint16_t frequencyHz, uint16_t durationMs) {
  if (volumePercent == 0 || frequencyHz == 0 || durationMs == 0) {
    ledcWriteTone(PWM_OUT_A_PIN, 0);
    ledcWriteTone(PWM_OUT_B_PIN, 0);
    pwmToneActive = false;
    return;
  }

  ledcWriteTone(PWM_OUT_A_PIN, frequencyHz);
  ledcWriteTone(PWM_OUT_B_PIN, frequencyHz);
  Serial.print(F("PWM GPIO"));
  Serial.print(PWM_OUT_A_PIN);
  Serial.print(F("/GPIO"));
  Serial.print(PWM_OUT_B_PIN);
  Serial.print(F(" "));
  Serial.print(frequencyHz);
  Serial.print(F(" Hz for "));
  Serial.print(durationMs);
  Serial.println(F(" ms"));
  pwmToneActive = true;
  pwmToneOffAtMs = millis() + durationMs;
}

void updatePwmTone() {
  if (pwmToneActive && int32_t(millis() - pwmToneOffAtMs) >= 0) {
    ledcWriteTone(PWM_OUT_A_PIN, 0);
    ledcWriteTone(PWM_OUT_B_PIN, 0);
    pwmToneActive = false;
  }
}

void dumpStatus() {
  uint8_t status = 0;
  uint8_t control2 = 0;
  selectPage(0x00);
  if (readReg(REG_STATUS, status) && readReg(REG_CONTROL_2, control2)) {
    Serial.print(F("Status 0x00=0x"));
    if (status < 0x10) Serial.print('0');
    Serial.print(status, HEX);
    Serial.print(F(" Control 0x02=0x"));
    if (control2 < 0x10) Serial.print('0');
    Serial.println(control2, HEX);
    if (status & 0x04) Serial.println(F("ILLEGAL_ADDR set."));
  }
}

bool initDrv2667() {
  delay(2);
  uint8_t err = probeAddress(DRV2667_ADDR);
  Serial.print(F("DRV2667 probe 0x59: "));
  Serial.println(i2cErrorName(err));
  if (err != 0) return false;

  writeReg(REG_CONTROL_2, 0x80);
  delay(2);

  bool ok = true;
  ok &= selectPage(0x00);
  ok &= writeReg(REG_CONTROL_2, 0x00);
  ok &= writeReg(REG_CONTROL_1, gainSetting & 3);
  ok &= writeReg(REG_WAVEFORM_0, 0x01);
  ok &= writeReg(REG_WAVEFORM_1, 0x00);

  uint8_t idReg = 0;
  if (readReg(REG_CONTROL_1, idReg)) {
    Serial.print(F("DRV2667 control reg 0x01 = 0x"));
    if (idReg < 0x10) Serial.print('0');
    Serial.println(idReg, HEX);
  } else {
    ok = false;
  }
  return ok;
}

VarioTone leafToneForClimb(int32_t verticalRate) {
  VarioTone tone{false, 0, 0, 0};

  if (verticalRate > DEFAULT_CLIMB_START_CMS) {
    uint16_t playSamples;
    uint16_t restSamples;
    uint16_t note;

    if (verticalRate >= CLIMB_MAX) {
      note = verticalRate * (CLIMB_NOTE_MAX - CLIMB_NOTE_MIN) / CLIMB_MAX + CLIMB_NOTE_MIN;
      if (note > CLIMB_NOTE_MAXMAX) note = CLIMB_NOTE_MAXMAX;
      playSamples = CLIMB_PLAY_SAMPLES_MIN;
      restSamples = 0;
    } else {
      note = verticalRate * (CLIMB_NOTE_MAX - CLIMB_NOTE_MIN) / CLIMB_MAX + CLIMB_NOTE_MIN;
      playSamples = CLIMB_PLAY_SAMPLES_MAX -
                    (verticalRate * (CLIMB_PLAY_SAMPLES_MAX - CLIMB_PLAY_SAMPLES_MIN) / CLIMB_MAX);
      restSamples = CLIMB_REST_SAMPLES_MAX -
                    (verticalRate * (CLIMB_REST_SAMPLES_MAX - CLIMB_REST_SAMPLES_MIN) / CLIMB_MAX);
    }

    tone.active = true;
    tone.frequencyHz = note;
    tone.playMs = playSamples * TICK_MS;
    tone.restMs = restSamples * TICK_MS;
  } else if (verticalRate < DEFAULT_SINK_ALARM_CMS) {
    uint16_t playSamples;
    uint16_t restSamples;
    uint16_t note;

    if (verticalRate <= SINK_MAX) {
      note = SINK_NOTE_MIN - verticalRate * (SINK_NOTE_MIN - SINK_NOTE_MAX) / SINK_MAX;
      if (note < SINK_NOTE_MAXMAX || note > SINK_NOTE_MAX) note = SINK_NOTE_MAXMAX;
      playSamples = SINK_PLAY_SAMPLES_MAX;
      restSamples = 0;
    } else {
      note = SINK_NOTE_MIN - verticalRate * (SINK_NOTE_MIN - SINK_NOTE_MAX) / SINK_MAX;
      playSamples = SINK_PLAY_SAMPLES_MIN +
                    (verticalRate * (SINK_PLAY_SAMPLES_MAX - SINK_PLAY_SAMPLES_MIN) / SINK_MAX);
      restSamples = SINK_REST_SAMPLES_MIN +
                    (verticalRate * (SINK_REST_SAMPLES_MAX - SINK_REST_SAMPLES_MIN) / SINK_MAX);
    }

    tone.active = true;
    tone.frequencyHz = note;
    tone.playMs = playSamples * TICK_MS;
    tone.restMs = restSamples * TICK_MS;
  }

  return tone;
}

bool playSynthBeep(uint16_t requestedHz, uint16_t requestedMs, bool softEnvelope) {
  if (volumePercent == 0 || requestedHz == 0 || requestedMs == 0) {
    return stopPlayback();
  }

  uint8_t freqByte = synthFrequencyByte(requestedHz);
  uint16_t actualHz = synthFrequencyHz(freqByte);
  uint16_t cycles = uint16_t((uint32_t(requestedMs) * actualHz + 500UL) / 1000UL);
  cycles = constrain(cycles, uint16_t(1), uint16_t(255));

  uint16_t actualMs = uint16_t((uint32_t(cycles) * 1000UL + actualHz / 2U) / actualHz);
  uint8_t envelope = (softEnvelope && actualMs >= 90) ? 0x11 : 0x00;  // 32 ms attack/release.

  const uint8_t ram[] = {
    0x05,             // Header size for one waveform ID.
    0x80,             // Synth mode, start page 1.
    0x06,             // Start address 0x006.
    0x00,             // Stop page 1.
    0x09,             // Stop address 0x009.
    0x01,             // Repeat once.
    amplitudeByte(),  // Amplitude.
    freqByte,         // Frequency = 7.8125 Hz * byte.
    uint8_t(cycles),  // Duration in cycles.
    envelope          // Envelope.
  };

  bool ok = true;
  if (outputMode == OUTPUT_DRV2667 || outputMode == OUTPUT_BOTH) {
    ok &= selectPage(0x00);
    ok &= writeReg(REG_CONTROL_2, 0x00);
    ok &= writeReg(REG_CONTROL_1, gainSetting & 3);
    ok &= writeReg(REG_WAVEFORM_0, 0x01);
    ok &= writeReg(REG_WAVEFORM_1, 0x00);
    ok &= selectPage(0x01);
    ok &= writeBlock(0x00, ram, sizeof(ram));
    ok &= selectPage(0x00);
    ok &= writeReg(REG_CONTROL_2, 0x01);
  } else {
    selectPage(0x00);
    writeReg(REG_CONTROL_2, 0x00);
  }

  if (outputMode == OUTPUT_PWM || outputMode == OUTPUT_BOTH) {
    startPwmTone(actualHz, actualMs);
  }

  Serial.print(F("Beep "));
  Serial.print(actualHz);
  Serial.print(F(" Hz "));
  Serial.print(actualMs);
  Serial.print(F(" ms amp "));
  Serial.print(volumePercent);
  Serial.print(F("% env 0x"));
  if (envelope < 0x10) Serial.print('0');
  Serial.println(envelope, HEX);
  return ok;
}

void printVarioTone(const VarioTone &tone) {
  Serial.print(F("Leaf vario: "));
  Serial.print(float(climbRateCms) / 100.0f, 2);
  Serial.print(F(" m/s -> "));
  if (!tone.active) {
    Serial.println(F("silent"));
    return;
  }
  Serial.print(tone.frequencyHz);
  Serial.print(F(" Hz, play "));
  Serial.print(tone.playMs);
  Serial.print(F(" ms, rest "));
  Serial.print(tone.restMs);
  Serial.println(F(" ms"));
}

void applyClimbRateCms(int32_t newClimbRateCms, bool verbose, bool resetBeepCycle) {
  climbRateCms = newClimbRateCms;
  VarioTone tone = leafToneForClimb(climbRateCms);
  if (verbose) printVarioTone(tone);

  varioEnabled = tone.active && volumePercent > 0;
  if (resetBeepCycle) {
    inRest = false;
    nextVarioEventMs = millis();
  }
  if (!varioEnabled) stopPlayback();
}

void setClimbRate(float climbMps) {
  rampActive = false;
  applyClimbRateCms(int32_t(lroundf(climbMps * 100.0f)), true, true);
}

void updateRamp() {
  if (!rampActive || rampPointCount < 2) return;

  uint32_t now = millis();
  while (rampActive && uint32_t(now - rampSegmentStartMs) >= rampSegmentMs) {
    rampSegmentIndex++;
    rampSegmentStartMs += rampSegmentMs;
    if (rampSegmentIndex >= rampPointCount - 1) {
      rampActive = false;
      applyClimbRateCms(rampPointsCms[rampPointCount - 1], true, false);
      Serial.println(F("Ramp complete."));
      return;
    }
  }

  uint32_t elapsed = now - rampSegmentStartMs;
  int32_t startCms = rampPointsCms[rampSegmentIndex];
  int32_t endCms = rampPointsCms[rampSegmentIndex + 1];
  int32_t newCms = startCms + int32_t((int64_t(endCms - startCms) * elapsed) / rampSegmentMs);
  applyClimbRateCms(newCms, false, false);

  if (uint32_t(now - lastRampPrintMs) >= RAMP_PRINT_INTERVAL_MS) {
    lastRampPrintMs = now;
    VarioTone tone = leafToneForClimb(climbRateCms);
    printVarioTone(tone);
  }
}

void startSweep(uint16_t startHz, uint16_t endHz, uint8_t stepBytes, uint16_t beepMs) {
  rampActive = false;
  varioEnabled = false;
  stopPlayback();

  uint8_t startByte = synthFrequencyByte(startHz);
  uint8_t endByte = synthFrequencyByte(endHz);
  sweepCurrentByte = startByte;
  sweepEndByte = endByte;
  sweepStepBytes = startByte <= endByte ? int8_t(stepBytes) : -int8_t(stepBytes);
  sweepBeepMs = constrain(beepMs, uint16_t(30), uint16_t(2000));
  sweepActive = true;
  nextSweepEventMs = millis();

  Serial.print(F("Sweep "));
  Serial.print(synthFrequencyHz(startByte));
  Serial.print(F(" Hz to "));
  Serial.print(synthFrequencyHz(endByte));
  Serial.print(F(" Hz, step "));
  Serial.print(stepBytes);
  Serial.print(F(" synth byte(s), "));
  Serial.print(sweepBeepMs);
  Serial.println(F(" ms each."));
}

void updateSweep() {
  if (!sweepActive) return;
  if (int32_t(millis() - nextSweepEventMs) < 0) return;

  bool done = sweepStepBytes > 0 ? sweepCurrentByte > sweepEndByte : sweepCurrentByte < sweepEndByte;
  if (done) {
    sweepActive = false;
    stopPlayback();
    Serial.println(F("Sweep complete."));
    return;
  }

  uint8_t freqByte = uint8_t(constrain(sweepCurrentByte, int16_t(1), int16_t(255)));
  uint16_t actualHz = synthFrequencyHz(freqByte);
  Serial.print(F("Sweep byte 0x"));
  if (freqByte < 0x10) Serial.print('0');
  Serial.print(freqByte, HEX);
  Serial.print(F(" -> "));
  Serial.print(actualHz);
  Serial.println(F(" Hz"));

  playSynthBeep(actualHz, sweepBeepMs, true);
  sweepCurrentByte += sweepStepBytes;
  nextSweepEventMs = millis() + sweepBeepMs + sweepGapMs;
}

bool startRampSequence(String command) {
  rampPointCount = 0;
  command.trim();

  while (command.length() > 0 && rampPointCount < RAMP_MAX_POINTS) {
    int comma = command.indexOf(',');
    String part = comma >= 0 ? command.substring(0, comma) : command;
    part.trim();

    float value = 0;
    if (!parseFloatStrict(part, value)) {
      Serial.print(F("Bad ramp point: "));
      Serial.println(part);
      return false;
    }
    rampPointsCms[rampPointCount++] = int32_t(lroundf(value * 100.0f));

    if (comma < 0) break;
    command.remove(0, comma + 1);
  }

  if (rampPointCount < 2) {
    Serial.println(F("Use at least two ramp points, for example: 1.0,6.0"));
    return false;
  }
  if (command.indexOf(',') >= 0) {
    Serial.println(F("Too many ramp points; max is 8."));
    return false;
  }

  rampSegmentIndex = 0;
  rampSegmentStartMs = millis();
  lastRampPrintMs = 0;
  rampActive = true;
  applyClimbRateCms(rampPointsCms[0], true, true);

  Serial.print(F("Ramp started: "));
  Serial.print(rampPointCount);
  Serial.print(F(" points, "));
  Serial.print(rampSegmentMs);
  Serial.println(F(" ms per segment."));
  return true;
}

void updateVarioSimulator() {
  if (!varioEnabled || volumePercent == 0) return;
  if (int32_t(millis() - nextVarioEventMs) < 0) return;

  VarioTone tone = leafToneForClimb(climbRateCms);
  if (!tone.active) {
    varioEnabled = false;
    stopPlayback();
    return;
  }

  if (inRest) {
    inRest = false;
    nextVarioEventMs = millis();
    return;
  }

  playSynthBeep(tone.frequencyHz, tone.playMs, true);
  uint16_t maxChunkMs = uint16_t((255UL * 1000UL) / synthFrequencyHz(synthFrequencyByte(tone.frequencyHz)));
  uint16_t playWindowMs = min<uint16_t>(tone.playMs, maxChunkMs);
  if (tone.restMs > 0) {
    inRest = true;
    nextVarioEventMs = millis() + playWindowMs + tone.restMs;
  } else {
    nextVarioEventMs = millis() + playWindowMs;
  }
}

void setVolume(uint8_t pct) {
  volumePercent = constrain(pct, 0, 99);
  Serial.print(F("Volume "));
  Serial.print(volumePercent);
  Serial.println(F("%"));
  if (volumePercent == 0) {
    varioEnabled = false;
    stopPlayback();
  }
}

void setGain(uint8_t gain) {
  gainSetting = constrain(gain, 0, 3);
  Serial.print(F("Gain "));
  Serial.println(gainSetting);
  selectPage(0x00);
  writeReg(REG_CONTROL_1, gainSetting & 3);
}

void setOutputMode(OutputMode mode) {
  outputMode = mode;
  Serial.print(F("Output mode "));
  switch (outputMode) {
    case OUTPUT_DRV2667:
      Serial.println(F("DRV2667 synth only"));
      ledcWriteTone(PWM_OUT_A_PIN, 0);
      ledcWriteTone(PWM_OUT_B_PIN, 0);
      pwmToneActive = false;
      break;
    case OUTPUT_PWM:
      Serial.println(F("ESP32 complementary square PWM only"));
      stopPlayback();
      break;
    case OUTPUT_BOTH:
      Serial.println(F("DRV2667 synth + ESP32 complementary square PWM"));
      break;
  }
}

void scanI2cBus() {
  Serial.println(F("Scanning I2C bus..."));
  uint8_t found = 0;
  for (uint8_t address = 1; address < 127; address++) {
    uint8_t err = probeAddress(address);
    if (err == 0) {
      Serial.print(F("  ACK at 0x"));
      if (address < 0x10) Serial.print('0');
      Serial.println(address, HEX);
      found++;
    }
  }
  if (found == 0) Serial.println(F("  No I2C devices ACKed."));
}

void dumpDrv2667Regs() {
  uint8_t value = 0;
  Serial.println(F("DRV2667 register dump:"));
  for (uint8_t reg : {uint8_t(0x00), uint8_t(0x01), uint8_t(0x02)}) {
    Serial.print(F("  0x"));
    if (reg < 0x10) Serial.print('0');
    Serial.print(reg, HEX);
    Serial.print(F(": "));
    if (readReg(reg, value)) {
      Serial.print(F("0x"));
      if (value < 0x10) Serial.print('0');
      Serial.println(value, HEX);
    } else {
      Serial.println(F("read failed"));
    }
  }
}

bool parseFloatStrict(const String &text, float &value) {
  char *endPtr = nullptr;
  value = strtof(text.c_str(), &endPtr);
  return endPtr != text.c_str() && *endPtr == '\0';
}

void printHelp() {
  Serial.println();
  Serial.println(F("DRV2667 Leaf vario synth simulator"));
  Serial.println(F("  0.1, 2.3, -4.9  set simulated climb in m/s"));
  Serial.println(F("  1.0,6.0,1.0     ramp through values"));
  Serial.println(F("  ramp 5000        set ms per ramp segment"));
  Serial.println(F("  sweep 500 1600 1 250"));
  Serial.println(F("                    sweep Hz range; step is synth-byte count"));
  Serial.println(F("  climb 1.0        set climb in m/s"));
  Serial.println(F("  0..99            set amplitude"));
  Serial.println(F("  gain 0..3 / g0   set DRV2667 gain"));
  Serial.println(F("  drv pwm both     choose output path"));
  Serial.println(F("  pwmtest 1000     direct PWM pin test"));
  Serial.println(F("  beep             one test beep"));
  Serial.println(F("  stop/off         stop simulator"));
  Serial.println(F("  scan dump init   diagnostics"));
  Serial.println();
}

void handleCommand(String command) {
  command.trim();
  command.toLowerCase();
  if (command.length() == 0) return;

  if (command == "?" || command == "h" || command == "help") {
    printHelp();
    return;
  }
  if (command == "scan") {
    scanI2cBus();
    return;
  }
  if (command == "dump") {
    dumpDrv2667Regs();
    return;
  }
  if (command == "init") {
    Serial.println(initDrv2667() ? F("DRV2667 init OK.") : F("DRV2667 init failed."));
    return;
  }
  if (command == "beep") {
    playSynthBeep(523, 300, true);
    dumpStatus();
    return;
  }
  if (command == "stop" || command == "off" || command == "s") {
    sweepActive = false;
    rampActive = false;
    varioEnabled = false;
    stopPlayback();
    Serial.println(F("Stopped."));
    return;
  }
  if (command == "drv") {
    setOutputMode(OUTPUT_DRV2667);
    return;
  }
  if (command == "pwm") {
    setOutputMode(OUTPUT_PWM);
    return;
  }
  if (command == "both") {
    setOutputMode(OUTPUT_BOTH);
    return;
  }
  if (command.startsWith("pwmtest")) {
    command.remove(0, 7);
    command.trim();
    long hz = command.length() ? strtol(command.c_str(), nullptr, 10) : 1000;
    if (hz < 20 || hz > 20000) {
      Serial.println(F("Use: pwmtest 1000  (20..20000 Hz)"));
      return;
    }
    startPwmTone(uint16_t(hz), 2000);
    return;
  }
  if (command.startsWith("gain") || command.startsWith("g")) {
    command.remove(0, command.startsWith("gain") ? 4 : 1);
    command.trim();
    long value = strtol(command.c_str(), nullptr, 10);
    if (value >= 0 && value <= 3) setGain(uint8_t(value));
    else Serial.println(F("Gain must be 0..3."));
    return;
  }
  if (command.startsWith("climb")) {
    rampActive = false;
    command.remove(0, 5);
    command.trim();
    float climb = 0;
    if (parseFloatStrict(command, climb)) setClimbRate(climb);
    else Serial.println(F("Use: climb 1.0"));
    return;
  }
  if (command.startsWith("ramp")) {
    command.remove(0, 4);
    command.trim();
    char *endPtr = nullptr;
    long value = strtol(command.c_str(), &endPtr, 10);
    if (*endPtr == '\0' && value >= 100 && value <= 60000) {
      rampSegmentMs = uint32_t(value);
      Serial.print(F("Ramp segment time "));
      Serial.print(rampSegmentMs);
      Serial.println(F(" ms"));
    } else {
      Serial.println(F("Use: ramp 5000  (100..60000 ms per segment)"));
    }
    return;
  }
  if (command.startsWith("sweep")) {
    command.remove(0, 5);
    command.trim();
    long startHz = 500;
    long endHz = 1600;
    long stepBytes = 1;
    long beepMs = 250;
    int parsed = sscanf(command.c_str(), "%ld %ld %ld %ld", &startHz, &endHz, &stepBytes, &beepMs);
    if (command.length() > 0 && parsed < 2) {
      Serial.println(F("Use: sweep 500 1600 1 250"));
      return;
    }
    if (startHz < 8 || startHz > 1992 || endHz < 8 || endHz > 1992 || stepBytes < 1 ||
        stepBytes > 32 || beepMs < 30 || beepMs > 2000) {
      Serial.println(F("Sweep ranges: Hz 8..1992, step 1..32, beep 30..2000 ms"));
      return;
    }
    startSweep(uint16_t(startHz), uint16_t(endHz), uint8_t(stepBytes), uint16_t(beepMs));
    return;
  }

  if (command.indexOf(',') >= 0) {
    startRampSequence(command);
    return;
  }

  if (command.indexOf('.') >= 0 || command[0] == '-' || command[0] == '+') {
    float climb = 0;
    if (parseFloatStrict(command, climb)) setClimbRate(climb);
    else Serial.print(F("Unknown command: "));
    if (!parseFloatStrict(command, climb)) Serial.println(command);
    return;
  }

  char *endPtr = nullptr;
  long value = strtol(command.c_str(), &endPtr, 10);
  if (*endPtr == '\0' && value >= 0 && value <= 99) {
    setVolume(uint8_t(value));
  } else {
    Serial.print(F("Unknown command: "));
    Serial.println(command);
  }
}

void pollSerial() {
  static String line;
  while (Serial.available() > 0) {
    char c = char(Serial.read());
    if (c == '\r' || c == '\n') {
      handleCommand(line);
      line = "";
    } else if (line.length() < 48) {
      line += c;
    }
  }
}

void setup() {
  Serial.begin(115200);
  uint32_t serialStart = millis();
  while (!Serial && millis() - serialStart < 2500) delay(10);

  Serial.println();
  Serial.println(F("Starting DRV2667 Leaf vario simulator..."));
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(I2C_CLOCK_HZ);
  Wire.setTimeOut(50);

  pinMode(PWM_OUT_A_PIN, OUTPUT);
  pinMode(PWM_OUT_B_PIN, OUTPUT);
  bool pwmAOk = ledcAttachChannel(PWM_OUT_A_PIN, 1000, PWM_RESOLUTION_BITS, PWM_CHANNEL_A);
  bool pwmBOk = ledcAttachChannel(PWM_OUT_B_PIN, 1000, PWM_RESOLUTION_BITS, PWM_CHANNEL_B);
  ledcOutputInvert(PWM_OUT_B_PIN, true);
  ledcWriteTone(PWM_OUT_A_PIN, 0);
  ledcWriteTone(PWM_OUT_B_PIN, 0);
  Serial.print(F("PWM pins GPIO"));
  Serial.print(PWM_OUT_A_PIN);
  Serial.print(F("/GPIO"));
  Serial.print(PWM_OUT_B_PIN);
  Serial.print(F(" attach "));
  Serial.println((pwmAOk && pwmBOk) ? F("OK") : F("FAILED"));

  scanI2cBus();
  bool ok = initDrv2667();
  printHelp();
  Serial.println(ok ? F("Ready.") : F("Ready, but DRV2667 did not ACK."));
}

void loop() {
  pollSerial();
  updatePwmTone();
  updateSweep();
  if (!sweepActive) {
    updateRamp();
    updateVarioSimulator();
  }
}
