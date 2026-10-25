#include <Arduino.h>
#include <SD_MMC.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "diagnostics/diagnostic_logs.h"
#include "dispatch/message_bus.h"
#include "instruments/baro.h"
#include "instruments/imu.h"
#include "runtime.h"
#include "sim/board.h"
#include "ui/audio/speaker.h"
#include "ui/display/display.h"
#include "ui/display/display_fields.h"
#include "ui/settings/settings.h"

extern MessageBus<11> bus;

static bool heardTone = false;

static void runFor(uint32_t ms) {
  const uint32_t start = millis();
  while (uint32_t(millis() - start) < ms) {
    sim::runtime().step();
    if (sim::board().currentTone()) heardTone = true;
  }
}

static double altitude = 1000;

static void fly(uint32_t durationMs, double climb, bool motion = false) {
  const uint32_t start = millis();
  uint32_t previous = start;
  while (uint32_t(millis() - start) < durationMs) {
    runFor(50);
    const uint32_t now = millis();
    altitude += climb * uint32_t(now - previous) * .001;
    previous = now;
    const int32_t pressure = lround(101325 * pow(1 - altitude / 44331., 1 / .190264));
    bus.receive(PressureUpdate(now, pressure));
    if (motion) {
      MotionUpdate m(now);
      m.hasAcceleration = m.hasOrientation = true;
      m.az = 1;
      bus.receive(m);
    }
  }
}

static void checkClimb(double expected) {
  assert(baro.climbRateFilteredValid());
  assert(baro.climbRate1SecAverageValid());
  assert(baro.climbRateAverageValid());
  printf("Expected %.2f m/s, measured %.2f m/s\n", expected, baro.climbRateFiltered() * .01);
  assert(fabs(baro.climbRateFiltered() * .01 - expected) < .2);
}

int main() {
  sim::Runtime::Options options;
  options.httpPort = 0;
  options.speed = 0;
  options.exitOnFatalError = true;
  options.statePath = "sim/build-tests/baro-only/state";
  options.cardRoot = "sim/build-tests/baro-only/card";
  sim::runtime().configure(options);
  sim::runtime().boot();
  settings.loadDefaults();
  settings.log_autoStart = settings.log_autoStop = false;
  settings.system_autoOff = 0;
  settings.system_showWarning = false;

  // The healthy-IMU path still uses motion data and produces valid climb.
  fly(15000, 2, true);
  assert(imu.accelValid());
  checkClimb(2);

  // Simulate the driver's initialization-failure decision and restart instrument warmup.
  baro.sleep();
  baro.wake();
  imu.useBarometerOnly();
  display.showOnSplash();
  settings.dev_mode = true;
  settings.diag_vario = true;  // Diagnostics must tolerate unavailable acceleration too.
  settings.vario_quietMode = false;
  speaker.setVolume(Speaker::SoundChannel::Vario, SpeakerVolume::Low);
  heardTone = false;
  fly(15000, 2);
  assert(display_startupProgressComplete());
  assert(display.lastRenderContext() != DisplayRenderContext::StartupSplash);
  assert(heardTone);
  assert(!imu.accelValid());
  assert(!imu.gLoad().filteredValid(millis()));
  checkClimb(2);
  assert(SD_MMC.exists(diagnostic_logs::VARIO_PATH));
  fly(15000, -3);
  checkClimb(-3);
  fly(15000, 0);
  checkClimb(0);

  // Missing pressure invalidates all climb consumers. A resumed stream warms up again,
  // without differentiating a pressure jump across the outage.
  runFor(400);
  assert(!imu.velocityValid());
  assert(!baro.climbRateFilteredValid());
  altitude += 100;
  fly(50, 0);
  assert(!baro.climbRateFilteredValid());
  fly(15000, 0);
  checkClimb(0);

  // Charging/sleep resets preserve the fallback mode.
  baro.sleep();
  imu.wake();
  runFor(400);
  baro.wake();
  fly(15000, 1);
  checkClimb(1);
  assert(!imu.accelValid());
  puts("Healthy IMU, barometer-only startup/climb/sink, diagnostics, stale data and wake passed");
}
