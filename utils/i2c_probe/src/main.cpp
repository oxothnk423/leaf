// Temporary USB bench diagnostic for Leaf 3.2.6/3.2.7. No Leaf application code is linked.
#include <Arduino.h>
#include <Wire.h>
#include <driver/gpio.h>

#ifndef LEAF_PROBE_SDA
#define LEAF_PROBE_SDA 8
#endif
#ifndef LEAF_PROBE_SCL
#define LEAF_PROBE_SCL 9
#endif

namespace {
  constexpr gpio_num_t SDA_PIN = static_cast<gpio_num_t>(LEAF_PROBE_SDA);
  constexpr gpio_num_t SCL_PIN = static_cast<gpio_num_t>(LEAF_PROBE_SCL);
  constexpr uint8_t POWER_LATCH = 48;
  constexpr uint8_t LED_PIN = 47;
  constexpr uint32_t RELEASE_TIMEOUT_US = 2000;
  constexpr uint32_t PULSE_US = 50;
  constexpr unsigned SAMPLE_COUNT = 128;
  bool wireStarted = false;
  bool watching = true;
  bool assistPullups = true;
  const char* watchLabel = "INPUT weak pull-ups";

  struct Levels {
    int sda;
    int scl;
    bool idle() const { return sda && scl; }
  };

  Levels levels() { return {gpio_get_level(SDA_PIN), gpio_get_level(SCL_PIN)}; }

  void printLevels(const char* label) {
    const auto v = levels();
    Serial.printf("%-30s SDA=%d SCL=%d\n", label, v.sda, v.scl);
  }

  void stopWire() {
    if (wireStarted) Wire.end();
    wireStarted = false;
  }

  void configurePins(gpio_mode_t mode, gpio_pullup_t up = GPIO_PULLUP_DISABLE,
                     gpio_pulldown_t down = GPIO_PULLDOWN_DISABLE) {
    gpio_config_t cfg{};
    cfg.pin_bit_mask = (1ULL << SDA_PIN) | (1ULL << SCL_PIN);
    cfg.mode = mode;
    cfg.pull_up_en = up;
    cfg.pull_down_en = down;
    cfg.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&cfg));
  }

  void releasePins() {
    stopWire();
    configurePins(GPIO_MODE_INPUT, assistPullups ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE);
  }

  void inputMode(gpio_pullup_t up, gpio_pulldown_t down, const char* label) {
    stopWire();
    assistPullups = up == GPIO_PULLUP_ENABLE;
    configurePins(GPIO_MODE_INPUT, up, down);
    watchLabel = label;
    watching = true;
    Serial.printf("Input-only mode: GPIO%d=SDA GPIO%d=SCL; both output drivers DISABLED.\n",
                  int(SDA_PIN), int(SCL_PIN));
    printLevels(watchLabel);
  }

  void openDrain() {
    stopWire();
    // Preload release levels before enabling outputs. Never drive either bus line HIGH.
    gpio_set_level(SDA_PIN, 1);
    gpio_set_level(SCL_PIN, 1);
    configurePins(GPIO_MODE_INPUT_OUTPUT_OD,
                  assistPullups ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE);
    gpio_set_drive_capability(SDA_PIN, GPIO_DRIVE_CAP_0);
    gpio_set_drive_capability(SCL_PIN, GPIO_DRIVE_CAP_0);
    delayMicroseconds(PULSE_US);
  }

  bool waitHigh(gpio_num_t pin) {
    const uint32_t start = micros();
    while (!gpio_get_level(pin)) {
      if (uint32_t(micros() - start) >= RELEASE_TIMEOUT_US) return false;
      delayMicroseconds(1);
    }
    return true;
  }

  void sampleInputs(const char* label, gpio_pullup_t up, gpio_pulldown_t down) {
    configurePins(GPIO_MODE_INPUT, up, down);
    delay(2);
    unsigned sdaHigh = 0, sclHigh = 0;
    for (unsigned i = 0; i < SAMPLE_COUNT; ++i) {
      const auto v = levels();
      sdaHigh += v.sda;
      sclHigh += v.scl;
      delayMicroseconds(10);
    }
    Serial.printf("%-30s HIGH samples: SDA=%u/%u SCL=%u/%u\n", label, sdaHigh, SAMPLE_COUNT,
                  sclHigh, SAMPLE_COUNT);
  }

  void passiveProbe() {
    releasePins();
    Serial.println("\n--- Inputs only: external pulls, then weak internal bias ---");
    sampleInputs("No internal pulls", GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE);
    sampleInputs("Weak internal pull-ups", GPIO_PULLUP_ENABLE, GPIO_PULLDOWN_DISABLE);
    sampleInputs("Weak internal pull-downs", GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_ENABLE);
    releasePins();
    Serial.println("LOW even with pull-up: held-low line, unpowered device, leakage or short.");
    Serial.println("Bias-dependent/unstable level: weak/missing pull-up or floating connection.");
    Serial.println("HIGH with pull-down can be normal: external pull-ups are usually stronger.");
  }

  bool stopCondition() {
    gpio_set_level(SCL_PIN, 0);
    gpio_set_level(SDA_PIN, 0);
    delayMicroseconds(PULSE_US);
    gpio_set_level(SCL_PIN, 1);
    const bool clockHigh = waitHigh(SCL_PIN);
    delayMicroseconds(PULSE_US);
    gpio_set_level(SDA_PIN, 1);
    const bool dataHigh = waitHigh(SDA_PIN);
    return clockHigh && dataHigh;
  }

  bool recoverBus() {
    openDrain();
    Serial.println("\n--- Bus clear: up to nine clock pulses, then STOP ---");
    printLevels("Before bus clear");
    if (!waitHigh(SCL_PIN)) {
      Serial.println("FAIL: SCL stays LOW when released; clock-based recovery is impossible.");
      return false;
    }
    unsigned clocks = 0;
    while (!gpio_get_level(SDA_PIN) && clocks < 9) {
      gpio_set_level(SCL_PIN, 0);
      delayMicroseconds(PULSE_US);
      gpio_set_level(SCL_PIN, 1);
      if (!waitHigh(SCL_PIN)) {
        Serial.println("FAIL: SCL did not rise during bus clear.");
        gpio_set_level(SDA_PIN, 1);
        return false;
      }
      delayMicroseconds(PULSE_US);
      ++clocks;
    }
    const bool ok = stopCondition();
    Serial.printf("Bus-clear clocks=%u, result=%s\n", clocks, ok ? "IDLE" : "STILL HELD LOW");
    printLevels("After bus clear");
    return ok;
  }

  bool driveProbe() {
    Serial.println("\n--- Brief open-drain LOW pulses; minimum GPIO drive strength ---");
    if (!levels().idle()) {
      Serial.println("SKIP: bus not idle; independent line test would be ambiguous.");
      return false;
    }
    // Keep SDA transitions under a LOW clock, except the final STOP. No data is written.
    gpio_set_level(SCL_PIN, 0);
    delayMicroseconds(PULSE_US);
    const auto clockLow = levels();
    gpio_set_level(SDA_PIN, 0);
    delayMicroseconds(PULSE_US);
    const auto bothLow = levels();
    gpio_set_level(SCL_PIN, 1);
    const uint32_t start = micros();
    const bool clockRose = waitHigh(SCL_PIN);
    const uint32_t clockRiseUs = micros() - start;
    const auto dataLow = levels();
    gpio_set_level(SDA_PIN, 1);
    const uint32_t dataStart = micros();
    const bool dataRose = waitHigh(SDA_PIN);
    const uint32_t dataRiseUs = micros() - dataStart;
    Serial.printf("Pull SCL LOW, release SDA: SDA=%d SCL=%d (expected 1,0)\n", clockLow.sda,
                  clockLow.scl);
    Serial.printf("Pull both LOW:            SDA=%d SCL=%d (expected 0,0)\n", bothLow.sda,
                  bothLow.scl);
    Serial.printf("Release SCL, hold SDA:    SDA=%d SCL=%d (expected 0,1)\n", dataLow.sda,
                  dataLow.scl);
    Serial.printf(
        "Digital release observations: SCL=%lu us SDA=%lu us (coarse, not scope timing)\n",
        (unsigned long)clockRiseUs, (unsigned long)dataRiseUs);
    printLevels("After releasing both");
    const bool ok = clockLow.sda && !clockLow.scl && !bothLow.sda && !bothLow.scl && !dataLow.sda &&
                    dataLow.scl && clockRose && dataRose;
    Serial.println(ok ? "PASS: independent pull-low/release readback."
                      : "FAIL: unexpected GPIO readback/coupling; inspect wiring and power.");
    return ok;
  }

  const char* expectedDevice(uint8_t address) {
    switch (address) {
      case 0x20:
        return "I/O expander (3.2.7)";
      case 0x26:
        return "I/O expander (3.2.6)";
      case 0x38:
        return "AHT20 temperature/humidity";
      case 0x68:
        return "ICM20948 IMU";
      case 0x69:
        return "alternate IMU address";
      case 0x76:
        return "alternate barometer address";
      case 0x77:
        return "MS5611 barometer";
      default:
        return "unidentified";
    }
  }

  void scan(uint32_t frequency) {
    releasePins();
    delay(2);
    Serial.printf("\n--- I2C address-only scan at %lu Hz ---\n", (unsigned long)frequency);
    if (!levels().idle()) {
      printLevels("SKIP: lines not HIGH");
      return;
    }
    wireStarted = Wire.begin(SDA_PIN, SCL_PIN, frequency);
    if (!wireStarted) {
      Serial.println("FAIL: Wire.begin failed.");
      releasePins();
      return;
    }
    Wire.setTimeOut(20);
    bool found[128]{};
    unsigned devices = 0, nacks = 0, errors = 0;
    for (uint8_t address = 0x08; address <= 0x77; ++address) {
      Wire.beginTransmission(address);
      const uint8_t result = Wire.endTransmission(true);
      if (result == 0) {
        found[address] = true;
        ++devices;
        Serial.printf("ACK 0x%02X  %s (address match, not identity verification)\n", address,
                      expectedDevice(address));
      } else if (result == 2) {
        ++nacks;
      } else {
        ++errors;
        Serial.printf("Address 0x%02X: Wire error=%u\n", address, result);
      }
      if (!waitHigh(SCL_PIN) || !waitHigh(SDA_PIN)) {
        printLevels("ABORT: lines not released");
        break;
      }
      delay(1);
    }
    Serial.printf("Summary: ACK=%u NACK=%u other-errors=%u\n", devices, nacks, errors);
    for (uint8_t address : {0x20, 0x26, 0x38, 0x68, 0x77}) {
      Serial.printf("  0x%02X %-29s %s\n", address, expectedDevice(address),
                    found[address] ? "ACK" : "NO ACK observed");
    }
    if (devices > 100)
      Serial.println("SUSPECT: almost every address ACKed; this is not a valid scan.");
    releasePins();
  }

  void softwareScan() {
    Serial.println("\n--- Independent GPIO software scan (~10 kHz, internal pull-ups) ---");
    openDrain();
    unsigned devices = 0, nacks = 0;
    for (uint8_t address = 0x08; address <= 0x77; ++address) {
      if (!waitHigh(SCL_PIN) || !waitHigh(SDA_PIN)) {
        printLevels("ABORT: GPIO bus not idle");
        break;
      }
      gpio_set_level(SDA_PIN, 0);  // START
      delayMicroseconds(PULSE_US);
      bool clockOk = true;
      const uint8_t addressByte = address << 1;  // Address + write bit; no data bytes.
      for (int bit = 7; bit >= 0; --bit) {
        gpio_set_level(SCL_PIN, 0);
        gpio_set_level(SDA_PIN, (addressByte >> bit) & 1);
        delayMicroseconds(PULSE_US);
        gpio_set_level(SCL_PIN, 1);
        if (!waitHigh(SCL_PIN)) {
          clockOk = false;
          break;
        }
        delayMicroseconds(PULSE_US);
      }
      if (!clockOk) {
        Serial.println("ABORT: software clock failed to rise.");
        break;
      }
      gpio_set_level(SCL_PIN, 0);
      gpio_set_level(SDA_PIN, 1);  // Release SDA for the device's ACK bit.
      delayMicroseconds(PULSE_US);
      gpio_set_level(SCL_PIN, 1);
      clockOk = waitHigh(SCL_PIN);
      delayMicroseconds(PULSE_US);
      const bool ack = clockOk && !gpio_get_level(SDA_PIN);
      const bool stopped = stopCondition();
      if (!clockOk || !stopped) {
        Serial.println("ABORT: software ACK clock/STOP failed.");
        break;
      }
      if (ack) {
        ++devices;
        Serial.printf("GPIO ACK 0x%02X %s\n", address, expectedDevice(address));
      } else {
        ++nacks;
      }
      delay(1);
    }
    Serial.printf("GPIO scan summary: ACK=%u NACK=%u\n", devices, nacks);
    releasePins();
  }

  void runDiagnostic() {
    Serial.printf("\n=== LEAF I2C PROBE BEGIN t=%lu ms ===\n", (unsigned long)millis());
    Serial.printf("Chip=%s eFuse-ID=%012llX SDA=GPIO%d SCL=GPIO%d power-latch=GPIO48 HIGH\n",
                  ESP.getChipModel(), (unsigned long long)ESP.getEfuseMac(), int(SDA_PIN),
                  int(SCL_PIN));
    Serial.println("USB-powered bench firmware. No radio, storage, settings or normal Leaf UI.");
    Serial.println(
        "GPIO readback cannot locate a broken trace or distinguish every hardware fault.");
    assistPullups = false;
    passiveProbe();
    bool usable = false;
    for (bool assisted : {false, true}) {
      assistPullups = assisted;
      Serial.printf("\n=== Electrical pass: %s ===\n",
                    assisted ? "weak internal pull-ups ENABLED" : "external pull-ups only");
      const bool recovered = recoverBus();
      const bool gpioOk = recovered && driveProbe();
      releasePins();
      if (gpioOk) {
        usable = true;
        // Wire may enable its own weak pull-ups; scan results do not certify external pull-ups.
        for (uint32_t frequency : {10000UL, 100000UL, 400000UL}) scan(frequency);
        break;
      }
      Serial.println("Address scans skipped for this electrical pass.");
    }
    if (usable) {
      assistPullups = true;
      softwareScan();
    }
    assistPullups = false;
    releasePins();
    Serial.println("=== LEAF I2C PROBE END ===");
    inputMode(GPIO_PULLUP_ENABLE, GPIO_PULLDOWN_DISABLE, "INPUT weak pull-ups");
    Serial.println(
        "Commands: i=input pull-ups, p=input pull-downs, f=floating inputs, w=watch, ?=help");
  }
}  // namespace

void setup() {
  // The original bus pins must not drive the bus while alternate pins use external jumpers.
  if (SDA_PIN != GPIO_NUM_8 && SCL_PIN != GPIO_NUM_8) {
    gpio_reset_pin(GPIO_NUM_8);
    gpio_set_direction(GPIO_NUM_8, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_NUM_8, GPIO_FLOATING);
  }
  if (SDA_PIN != GPIO_NUM_9 && SCL_PIN != GPIO_NUM_9) {
    gpio_reset_pin(GPIO_NUM_9);
    gpio_set_direction(GPIO_NUM_9, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_NUM_9, GPIO_FLOATING);
  }
  // Hold the same regulator enable as normal operating mode, without using the I/O expander.
  gpio_set_level(GPIO_NUM_48, 1);
  pinMode(POWER_LATCH, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);
  releasePins();
  Serial.begin(115200);
  Serial.setTxTimeoutMs(50);
}

void loop() {
  static bool connected = false;
  static uint32_t lastHeartbeat = 0;
  if (!Serial) {
    connected = false;
    delay(20);
    return;
  }
  if (!connected) {
    connected = true;
    delay(200);
    Serial.println("\n=== LEAF I2C INPUT MONITOR ===");
    Serial.printf("GPIO%d=SDA GPIO%d=SCL; output drivers DISABLED; no automatic bus scans.\n",
                  int(SDA_PIN), int(SCL_PIN));
    Serial.println(
        "i=pull-ups; p=pull-downs; f=no pulls; w=watch; r=active tests (remove grounding "
        "resistors)");
    printLevels(watchLabel);
  }
  while (Serial.available()) {
    switch (Serial.read()) {
      case 'i':
      case 'I':
        inputMode(GPIO_PULLUP_ENABLE, GPIO_PULLDOWN_DISABLE, "INPUT weak pull-ups");
        break;
      case 'p':
      case 'P':
        inputMode(GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_ENABLE, "INPUT weak pull-downs");
        break;
      case 'f':
      case 'F':
        inputMode(GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, "INPUT no pulls");
        break;
      case 'r':
      case 'R':
        runDiagnostic();
        break;
      case 'w':
      case 'W':
        watching = !watching;
        break;
      case '?':
        Serial.println("i=input pull-ups; p=input pull-downs; f=floating inputs; w=watch toggle");
        Serial.println(
            "r=active GPIO/recovery/address test; remove grounding resistors, keep bus jumpers");
        break;
    }
  }
  if (uint32_t(millis() - lastHeartbeat) >= 1000) {
    lastHeartbeat = millis();
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    if (watching) {
      Serial.printf("t=%lu ms ", (unsigned long)millis());
      printLevels(watchLabel);
    }
  }
  delay(10);
}
