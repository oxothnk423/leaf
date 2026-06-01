#include "ui/display/pages/dialogs/page_imu_diagnostic_alert.h"

#include "ui/audio/sound_effects.h"
#include "ui/audio/speaker.h"
#include "ui/display/display.h"
#include "ui/display/display_fields.h"
#include "ui/display/fonts.h"

PageIMUDiagnosticAlert& PageIMUDiagnosticAlert::instance() {
  static PageIMUDiagnosticAlert instance;
  return instance;
}

void PageIMUDiagnosticAlert::show(const char* trigger, const String& detail) {
  auto& page = instance();
  if (!page.showing_) {
    page.setMessage(trigger, detail);
    page.showing_ = true;
    push_page(&page);
  }
}

void PageIMUDiagnosticAlert::setMessage(const char* trigger, const String& detail) {
  trigger_ = trigger;
  detail_ = detail;
}

void PageIMUDiagnosticAlert::draw_extra() {
  display_menuTitle(String(get_title()));

  u8g2.setFont(leaf_6x12);
  u8g2.setCursor(13, 34);
  u8g2.print("IMU DIAGNOSTIC");

  u8g2.setFont(leaf_5x8);
  u8g2.setCursor(0, 52);
  u8g2.print("Trigger:");
  u8g2.setCursor(0, 63);
  u8g2.print(trigger_);

  uint8_t y = 82;
  uint8_t start = 0;
  while (start < detail_.length() && y < 174) {
    int end = detail_.indexOf('\n', start);
    if (end < 0) {
      end = detail_.length();
    }
    String line = detail_.substring(start, end);
    u8g2.setCursor(0, y);
    u8g2.print(line);
    y += 11;
    start = end + 1;
  }

  u8g2.setFont(leaf_6x12);
  u8g2.setCursor(10, 184);
  u8g2.print("Press Back");
}

void PageIMUDiagnosticAlert::setting_change(Button dir, ButtonEvent state, uint8_t count) {
  if (cursor_position == CURSOR_BACK && state == ButtonEvent::CLICKED) {
    pop_page();
    speaker.playSound(fx::confirm);
  }
}

void PageIMUDiagnosticAlert::closed(bool removed_from_Stack) {
  if (removed_from_Stack) {
    showing_ = false;
  }
}
