#pragma once

#include <Arduino.h>

#include "ui/display/menu_page.h"

class PageIMUDiagnosticAlert : public SimpleSettingsMenuPage {
 public:
  const char* get_title() const override { return "! IMU DIAG !"; }

  static void show(const char* trigger, const String& detail);

  void draw_extra() override;
  void setting_change(Button dir, ButtonEvent state, uint8_t count) override;

 protected:
  void closed(bool removed_from_Stack) override;

 private:
  PageIMUDiagnosticAlert() {}

  static PageIMUDiagnosticAlert& instance();

  void setMessage(const char* trigger, const String& detail);

  String trigger_;
  String detail_;
  bool showing_ = false;
};
