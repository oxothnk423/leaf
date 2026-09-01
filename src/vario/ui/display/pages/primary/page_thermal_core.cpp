#include "ui/display/pages/primary/page_thermal_core.h"

#include <Arduino.h>
#include <U8g2lib.h>
#include <math.h>

#include "hardware/buttons.h"
#include "instruments/baro.h"
#include "logging/log.h"
#include "navigation/thermal_core.h"
#include "power.h"
#include "ui/audio/sound_effects.h"
#include "ui/audio/speaker.h"
#include "ui/display/display.h"
#include "ui/display/display_fields.h"
#include "ui/display/fonts.h"
#include "ui/display/utils.h"
#include "ui/input/buttons.h"
#include "ui/settings/settings.h"

namespace {
  constexpr int16_t MAP_LEFT = 0;
  constexpr int16_t MAP_TOP = 76;
  constexpr int16_t MAP_SIZE = 96;
  constexpr int16_t MAP_CENTER_Y = MAP_TOP + MAP_SIZE / 2;
  constexpr int16_t AIRCRAFT_Y = MAP_CENTER_Y - 9;
  constexpr int16_t LEFT_AIRCRAFT_X = 20;
  constexpr int16_t RIGHT_AIRCRAFT_X = 76;
  constexpr int16_t CENTER_AIRCRAFT_X = 48;
  constexpr int16_t TARGET_RADIUS = 38;
  constexpr int16_t TARGET_SHIFT_MAX = 19;
  constexpr uint8_t VARIO_BAR_TOP = 16;
  constexpr uint8_t VARIO_BAR_WIDTH = 17;
  constexpr uint8_t VARIO_BAR_HALF_HEIGHT = 30;
  constexpr uint8_t ALT_LABEL_X = VARIO_BAR_WIDTH + 2;
  constexpr uint8_t ALT_X = 28;
  constexpr uint8_t ALT_BASELINE_Y = 39;
  constexpr uint8_t ALT_VALUE_BASELINE_Y = ALT_BASELINE_Y + 4;
  constexpr uint8_t ALT_INFO_BASELINE_Y = ALT_VALUE_BASELINE_Y - 20;
  constexpr uint8_t CLIMB_X = 30;
  constexpr uint8_t CLIMB_BASELINE_Y = 63;

  enum ThermalCorePageItem : uint8_t {
    cursor_thermalCorePage_none,
    cursor_thermalCorePage_alt,
    cursor_thermalCorePage_timer,
  };
  constexpr uint8_t THERMAL_CORE_CURSOR_MAX = cursor_thermalCorePage_timer;
  constexpr uint8_t THERMAL_CORE_CURSOR_TIMEOUT = 8;
  int8_t thermalCorePageCursor = cursor_thermalCorePage_none;
  uint8_t thermalCorePageCursorTimeCount = 0;

  bool thermalCorePageVolumeShortcut(Button button, ButtonEvent state) {
    if (!settings.volumeShortcut || (button != Button::UP && button != Button::DOWN) ||
        state != ButtonEvent::INCREMENTED) {
      return false;
    }

    if (!settings.adjustShortcutVolume(button)) buttons.consumeButton();
    return true;
  }

  void thermalCorePageCursorMove(Button button) {
    if (button == Button::UP) {
      thermalCorePageCursor--;
      if (thermalCorePageCursor < 0) thermalCorePageCursor = THERMAL_CORE_CURSOR_MAX;
    }
    if (button == Button::DOWN) {
      thermalCorePageCursor++;
      if (thermalCorePageCursor > THERMAL_CORE_CURSOR_MAX) thermalCorePageCursor = 0;
    }
    speaker.playSound(thermalCorePageCursor == cursor_thermalCorePage_none ? fx::doubleClick
                                                                           : fx::click);
  }

  void stampCross3(int16_t x, int16_t y) {
    u8g2.drawVLine(x, y - 1, 3);
    u8g2.drawHLine(x - 1, y, 3);
  }

  void stampRing5(int16_t x, int16_t y) {
    u8g2.drawHLine(x - 1, y - 2, 3);
    u8g2.drawPixel(x - 2, y - 1);
    u8g2.drawPixel(x + 2, y - 1);
    u8g2.drawPixel(x - 2, y);
    u8g2.drawPixel(x + 2, y);
    u8g2.drawPixel(x - 2, y + 1);
    u8g2.drawPixel(x + 2, y + 1);
    u8g2.drawHLine(x - 1, y + 2, 3);
  }

  void stampRing7(int16_t x, int16_t y) {
    u8g2.drawHLine(x - 1, y - 3, 3);
    u8g2.drawHLine(x - 2, y - 2, 2);
    u8g2.drawHLine(x + 1, y - 2, 2);
    u8g2.drawHLine(x - 3, y - 1, 2);
    u8g2.drawHLine(x + 2, y - 1, 2);
    u8g2.drawPixel(x - 3, y);
    u8g2.drawPixel(x + 3, y);
    u8g2.drawHLine(x - 3, y + 1, 2);
    u8g2.drawHLine(x + 2, y + 1, 2);
    u8g2.drawHLine(x - 2, y + 2, 2);
    u8g2.drawHLine(x + 1, y + 2, 2);
    u8g2.drawHLine(x - 1, y + 3, 3);
  }

  void stampRing9(int16_t x, int16_t y) {
    u8g2.drawHLine(x - 1, y - 4, 3);
    u8g2.drawHLine(x - 3, y - 3, 7);
    u8g2.drawHLine(x - 3, y - 2, 2);
    u8g2.drawHLine(x + 2, y - 2, 2);
    u8g2.drawHLine(x - 4, y - 1, 2);
    u8g2.drawHLine(x + 3, y - 1, 2);
    u8g2.drawHLine(x - 4, y, 2);
    u8g2.drawHLine(x + 3, y, 2);
    u8g2.drawHLine(x - 4, y + 1, 2);
    u8g2.drawHLine(x + 3, y + 1, 2);
    u8g2.drawHLine(x - 3, y + 2, 2);
    u8g2.drawHLine(x + 2, y + 2, 2);
    u8g2.drawHLine(x - 3, y + 3, 7);
    u8g2.drawHLine(x - 1, y + 4, 3);
  }

  void stampRing11(int16_t x, int16_t y) {
    u8g2.drawHLine(x - 1, y - 5, 3);
    u8g2.drawHLine(x - 3, y - 4, 7);
    u8g2.drawHLine(x - 4, y - 3, 9);
    u8g2.drawHLine(x - 4, y - 2, 3);
    u8g2.drawHLine(x + 2, y - 2, 3);
    u8g2.drawHLine(x - 5, y - 1, 3);
    u8g2.drawHLine(x + 3, y - 1, 3);
    u8g2.drawHLine(x - 5, y, 3);
    u8g2.drawHLine(x + 3, y, 3);
    u8g2.drawHLine(x - 5, y + 1, 3);
    u8g2.drawHLine(x + 3, y + 1, 3);
    u8g2.drawHLine(x - 4, y + 2, 3);
    u8g2.drawHLine(x + 2, y + 2, 3);
    u8g2.drawHLine(x - 4, y + 3, 9);
    u8g2.drawHLine(x - 3, y + 4, 7);
    u8g2.drawHLine(x - 1, y + 5, 3);
  }

  uint8_t markerRadius(const ThermalCoreMarker& marker) {
    switch (marker.glyph) {
      case ThermalCoreMarkerGlyph::Cross3:
        return 1;
      case ThermalCoreMarkerGlyph::Ring5:
        return 2;
      case ThermalCoreMarkerGlyph::Ring7:
        return 3;
      case ThermalCoreMarkerGlyph::Ring9:
        return 4;
      case ThermalCoreMarkerGlyph::Ring11:
        return 5;
    }
    return 5;
  }

  bool markerFitsMap(const ThermalCoreMarker& marker) {
    const uint8_t radius = markerRadius(marker);
    return marker.x - radius > MAP_LEFT && marker.x + radius < MAP_LEFT + MAP_SIZE - 1 &&
           marker.y - radius > MAP_TOP && marker.y + radius < MAP_TOP + MAP_SIZE - 1;
  }

  void drawMarker(const ThermalCoreMarker& marker) {
    if (!marker.visible) return;
    if (!markerFitsMap(marker)) return;
    switch (marker.glyph) {
      case ThermalCoreMarkerGlyph::Cross3:
        stampCross3(marker.x, marker.y);
        break;
      case ThermalCoreMarkerGlyph::Ring5:
        stampRing5(marker.x, marker.y);
        break;
      case ThermalCoreMarkerGlyph::Ring7:
        stampRing7(marker.x, marker.y);
        break;
      case ThermalCoreMarkerGlyph::Ring9:
        stampRing9(marker.x, marker.y);
        break;
      case ThermalCoreMarkerGlyph::Ring11:
        stampRing11(marker.x, marker.y);
        break;
    }
  }

  void drawAircraft(int16_t x, int16_t y) {
    u8g2.drawTriangle(x, y - 12, x + 8, y + 9, x - 8, y + 9);
    u8g2.setDrawColor(0);
    u8g2.drawTriangle(x, y + 1, x + 4, y + 8, x - 4, y + 8);
    u8g2.setDrawColor(1);
    u8g2.drawLine(x, y - 12, x + 8, y + 9);
    u8g2.drawLine(x, y - 12, x - 8, y + 9);
    u8g2.drawLine(x - 8, y + 9, x + 8, y + 9);
  }

  void drawTargetScale(int16_t cx, int16_t cy) {
    const int16_t left = cx - 25;
    const int16_t right = cx + 25;
    const int16_t top = cy - 10;
    const int16_t bottom = cy + 10;
    const int16_t innerTop = cy - 4;
    const int16_t innerBottom = cy + 4;

    u8g2.setDrawColor(0);
    u8g2.drawBox(left, innerTop, 51, 9);
    u8g2.drawBox(cx - 5, top, 11, 21);
    u8g2.setDrawColor(1);

    u8g2.drawLine(left, innerTop, cx - 5, innerTop);
    u8g2.drawLine(cx - 5, innerTop, cx - 5, top);
    u8g2.drawLine(cx - 5, top, cx + 5, top);
    u8g2.drawLine(cx + 5, top, cx + 5, innerTop);
    u8g2.drawLine(cx + 5, innerTop, right, innerTop);
    u8g2.drawLine(right, innerTop, right, innerBottom);
    u8g2.drawLine(right, innerBottom, cx + 5, innerBottom);
    u8g2.drawLine(cx + 5, innerBottom, cx + 5, bottom);
    u8g2.drawLine(cx + 5, bottom, cx - 5, bottom);
    u8g2.drawLine(cx - 5, bottom, cx - 5, innerBottom);
    u8g2.drawLine(cx - 5, innerBottom, left, innerBottom);
    u8g2.drawLine(left, innerBottom, left, innerTop);
  }

  void drawAltitudeField() {
    const uint8_t altType = settings.disp_thmPageAltType == altType_MSL ? altType_MSL : altType_GPS;
    u8g2.setFont(leaf_labels);
    u8g2.setCursor(ALT_LABEL_X, ALT_INFO_BASELINE_Y);
    u8g2.print(settings.units_alt ? "ft" : "m");
    u8g2.print(' ');
    print_alt_label(altType);

    display_alt_type(ALT_X, ALT_VALUE_BASELINE_Y, leaf_21h, altType);

    if (thermalCorePageCursor == cursor_thermalCorePage_alt) {
      display_selectionBox(ALT_LABEL_X - 1, ALT_VALUE_BASELINE_Y - 23, 96 - (ALT_LABEL_X - 1), 25,
                           6);
    }
  }

  void drawClimbRateField(int32_t climbRate) {
    u8g2.setFont(leaf_8x14);
    u8g2.setCursor(CLIMB_X, CLIMB_BASELINE_Y);
    if (climbRate >= 0) {
      u8g2.print('+');
    } else {
      u8g2.print('-');
      climbRate *= -1;
    }

    if (settings.units_climb) {
      climbRate = climbRate * 197 / 1000 * 10;
      if (climbRate < 1000) u8g2.print(" ");
      if (climbRate < 100) u8g2.print(" ");
      if (climbRate < 10) u8g2.print(" ");
      u8g2.print(climbRate);
    } else {
      climbRate = (climbRate + 5) / 10;
      const float climbInMS = static_cast<float>(climbRate) / 10;
      if (climbInMS < 10) u8g2.print(" ");
      u8g2.print(climbInMS, 1);
    }

    u8g2.setFont(leaf_5h);
    u8g2.print(" ");
    u8g2.setFont(leaf_6x12);
    u8g2.print(settings.units_climb ? "fpm" : "m/s");
  }

  void drawGuidanceLine(int16_t cx, int16_t cy) {
    u8g2.setDrawColor(0);
    u8g2.drawBox(cx - 2, cy - 8, 6, 16);
    u8g2.setDrawColor(1);
    u8g2.drawBox(cx - 1, cy - 7, 4, 14);
  }

  void drawSmallGuidanceArrow(int16_t lineLeft, int16_t cy, int8_t direction) {
    const int16_t baseX = direction > 0 ? lineLeft + 5 : lineLeft - 2;
    u8g2.drawPixel(baseX, cy - 2);
    u8g2.drawPixel(baseX, cy + 2);
    u8g2.drawHLine(direction > 0 ? baseX : baseX - 1, cy - 1, 2);
    u8g2.drawHLine(direction > 0 ? baseX : baseX - 1, cy + 1, 2);
    u8g2.drawHLine(direction > 0 ? baseX : baseX - 2, cy, 3);
  }

  void drawWideGuidanceArrow(int16_t baseX, int16_t cy, int8_t direction) {
    u8g2.drawHLine(direction > 0 ? baseX : baseX - 1, cy - 2, 2);
    u8g2.drawHLine(direction > 0 ? baseX : baseX - 1, cy + 2, 2);
    u8g2.drawHLine(direction > 0 ? baseX : baseX - 3, cy - 1, 4);
    u8g2.drawHLine(direction > 0 ? baseX : baseX - 3, cy + 1, 4);
    u8g2.drawHLine(direction > 0 ? baseX : baseX - 5, cy, 6);
  }

  void drawGuidanceReticle(int16_t cx, int16_t cy, int16_t neutralCx) {
    drawGuidanceLine(cx, cy);

    const int16_t error = neutralCx - cx;
    const int16_t distance = abs(error);
    if (distance <= 2) return;

    const int8_t direction = error > 0 ? 1 : -1;
    const int16_t lineLeft = cx - 1;
    if (distance <= 6) {
      drawSmallGuidanceArrow(lineLeft, cy, direction);
      return;
    }

    const int16_t firstBaseX = direction > 0 ? lineLeft + 5 : lineLeft - 2;
    drawWideGuidanceArrow(firstBaseX, cy, direction);
    if (distance >= 14) drawWideGuidanceArrow(firstBaseX + direction * 7, cy, direction);
  }

  void toggleAltitudeType() {
    if (settings.disp_thmPageAltType == altType_MSL)
      settings.disp_thmPageAltType = altType_GPS;
    else
      settings.disp_thmPageAltType = altType_MSL;
    speaker.playSound(fx::neutral);
  }

  void drawThermalCoreContent() {
    const int32_t climbRate = baro.climbRateFilteredValid() ? baro.climbRateFiltered() : 0;
    const int32_t displayClimbRate = baro.climbRateFilteredValid() ? baro.climbRateForDisplay() : 0;
    const ThermalCoreEstimate& estimate = thermalCore.estimate();
    const int8_t direction = estimate.direction;
    const int8_t turnSide = direction < 0 ? -1 : direction > 0 ? 1 : 0;
    const int16_t aircraftX = direction < 0   ? RIGHT_AIRCRAFT_X
                              : direction > 0 ? LEFT_AIRCRAFT_X
                                              : CENTER_AIRCRAFT_X;
    const int16_t adviceShift = estimate.valid ? (estimate.adviceQ7 * TARGET_SHIFT_MAX) / 127 : 0;
    const int16_t dynamicRadius =
        min<int16_t>(TARGET_RADIUS + TARGET_SHIFT_MAX,
                     max<int16_t>(TARGET_RADIUS - TARGET_SHIFT_MAX, TARGET_RADIUS + adviceShift));
    const int16_t targetCx = turnSide == 0 ? aircraftX : aircraftX + turnSide * dynamicRadius;
    const bool hasTurn = turnSide != 0;
    const bool hasGuidance = estimate.valid;

    display_varioBar(VARIO_BAR_TOP, VARIO_BAR_HALF_HEIGHT, VARIO_BAR_HALF_HEIGHT, VARIO_BAR_WIDTH,
                     climbRate);
    drawAltitudeField();
    drawClimbRateField(displayClimbRate);

    for (uint8_t i = 0; i < estimate.markerCount; ++i) {
      drawMarker(estimate.markers[i]);
    }

    const int16_t neutralTargetCx =
        hasTurn ? aircraftX + turnSide * TARGET_RADIUS : CENTER_AIRCRAFT_X;
    if (hasTurn) drawTargetScale(neutralTargetCx, MAP_CENTER_Y);

    if (hasGuidance) drawGuidanceReticle(targetCx, MAP_CENTER_Y, neutralTargetCx);
    drawAircraft(aircraftX, AIRCRAFT_Y);
    u8g2.drawFrame(MAP_LEFT, MAP_TOP, MAP_SIZE, MAP_SIZE);
  }
}  // namespace

void thermalCorePage_draw() {
  if (thermalCorePageCursor != cursor_thermalCorePage_none &&
      thermalCorePageCursorTimeCount++ >= THERMAL_CORE_CURSOR_TIMEOUT) {
    thermalCorePageCursor = cursor_thermalCorePage_none;
    thermalCorePageCursorTimeCount = 0;
  }

  u8g2.firstPage();
  do {
    display_headerAndFooter(thermalCorePageCursor == cursor_thermalCorePage_timer, false);
    drawThermalCoreContent();
  } while (u8g2.nextPage());
}

void thermalCorePage_button(Button button, ButtonEvent state, uint8_t count) {
  thermalCorePageCursorTimeCount = 0;

  switch (thermalCorePageCursor) {
    case cursor_thermalCorePage_none:
      switch (button) {
        case Button::UP:
        case Button::DOWN:
          if (thermalCorePageVolumeShortcut(button, state)) {
            break;
          } else if (state == ButtonEvent::CLICKED) {
            thermalCorePageCursorMove(button);
          }
          break;
        case Button::RIGHT:
          if (state == ButtonEvent::CLICKED) {
            display.turnPage(PageAction::Next);
            speaker.playSound(fx::increase);
          }
          break;
        case Button::LEFT:
          if (state == ButtonEvent::CLICKED) {
            display.turnPage(PageAction::Prev);
            speaker.playSound(fx::decrease);
          }
          break;
        case Button::CENTER:
          if (state == ButtonEvent::INCREMENTED && count == 2) {
            power.shutdown();
            return;
          }
          break;
      }
      break;
    case cursor_thermalCorePage_alt:
      switch (button) {
        case Button::UP:
        case Button::DOWN:
          if (state == ButtonEvent::CLICKED) thermalCorePageCursorMove(button);
          break;
        case Button::LEFT:
        case Button::RIGHT:
          break;
        case Button::CENTER:
          if (state == ButtonEvent::CLICKED) toggleAltitudeType();
          break;
      }
      break;
    case cursor_thermalCorePage_timer:
      switch (button) {
        case Button::UP:
        case Button::DOWN:
          if (state == ButtonEvent::CLICKED) thermalCorePageCursorMove(button);
          break;
        case Button::LEFT:
        case Button::RIGHT:
          break;
        case Button::CENTER:
          if (state == ButtonEvent::CLICKED && !flightTimer_isRunning()) {
            flightTimer_start();
            thermalCorePageCursor = cursor_thermalCorePage_none;
          } else if (state == ButtonEvent::HELD && flightTimer_isRunning()) {
            buttons.consumeButton();
            flightTimer_stop();
            thermalCorePageCursor = cursor_thermalCorePage_none;
          }
          break;
      }
      break;
  }
  display.update();
}
