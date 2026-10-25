// Temporary Leaf 3.2.6/3.2.7 input/LED check. No sensor or bus drivers are started.
#include <Arduino.h>
#include <driver/gpio.h>

namespace {
  constexpr gpio_num_t INPUTS[] = {GPIO_NUM_8,  GPIO_NUM_9,  GPIO_NUM_21,
                                   GPIO_NUM_41, GPIO_NUM_42, GPIO_NUM_45};
  constexpr gpio_num_t LED = GPIO_NUM_47;  // Leaf LED is active LOW.
  constexpr gpio_num_t POWER_LATCH = GPIO_NUM_48;
  constexpr uint8_t ALL_INPUTS = (1U << 6) - 1;
  uint32_t lowEvents[6]{};
  uint8_t previousLowMask = 0;

  uint8_t readLowMask() {
    uint8_t mask = 0;
    for (unsigned i = 0; i < 6; ++i)
      if (!gpio_get_level(INPUTS[i])) mask |= 1U << i;
    return mask;
  }

  bool report(uint8_t lowMask) {
    char line[240];
    const int length = snprintf(
        line, sizeof(line),
        "t=%lu GPIO8=%u GPIO9=%u GPIO21=%u GPIO41=%u GPIO42=%u GPIO45=%u LED=%s "
        "low-events=%lu,%lu,%lu,%lu,%lu,%lu\n",
        (unsigned long)millis(), !(lowMask & 1), !(lowMask & 2), !(lowMask & 4), !(lowMask & 8),
        !(lowMask & 16), !(lowMask & 32), lowMask ? "ON" : "OFF", (unsigned long)lowEvents[0],
        (unsigned long)lowEvents[1], (unsigned long)lowEvents[2], (unsigned long)lowEvents[3],
        (unsigned long)lowEvents[4], (unsigned long)lowEvents[5]);
    if (length <= 0 || length >= sizeof(line) || Serial.availableForWrite() < length) return false;
    return Serial.write(reinterpret_cast<const uint8_t*>(line), length) == length;
  }
}  // namespace

void setup() {
  gpio_set_level(POWER_LATCH, 1);
  gpio_set_direction(POWER_LATCH, GPIO_MODE_OUTPUT);
  gpio_set_level(LED, 1);
  gpio_set_direction(LED, GPIO_MODE_OUTPUT);

  gpio_config_t cfg{};
  for (const auto pin : INPUTS) cfg.pin_bit_mask |= 1ULL << pin;
  cfg.mode = GPIO_MODE_INPUT;
  cfg.pull_up_en = GPIO_PULLUP_ENABLE;
  cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  cfg.intr_type = GPIO_INTR_DISABLE;
  ESP_ERROR_CHECK(gpio_config(&cfg));

  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);  // A USB monitor must never gate LED operation or input sampling.
}

void loop() {
  static bool connected = false;
  static uint8_t reportedMask = 0xff;
  static uint32_t lastReport = 0;
  const uint8_t lowMask = readLowMask();
  const uint8_t newLows = lowMask & ~previousLowMask & ALL_INPUTS;
  for (unsigned i = 0; i < 6; ++i)
    if (newLows & (1U << i)) ++lowEvents[i];
  previousLowMask = lowMask;
  gpio_set_level(LED, lowMask ? 0 : 1);

  if (Serial) {
    if (!connected) {
      connected = true;
      Serial.println(
          "LEAF SIX INPUT TEST: GPIO8,9,21,41,42,45 INPUT + weak pull-up; outputs disabled.");
      Serial.println("LED GPIO47: ON while ANY input is LOW; no heartbeat blink or bus scans.");
      Serial.println(
          "low-events counters follow pin order 8,9,21,41,42,45; sampling continues without USB.");
      reportedMask = 0xff;
    }
    // Every state change and a one-second heartbeat show the complete pin snapshot.
    if (lowMask != reportedMask || uint32_t(millis() - lastReport) >= 1000) {
      if (report(lowMask)) {
        reportedMask = lowMask;
        lastReport = millis();
      }
    }
  } else {
    connected = false;
  }
  delay(1);
}
