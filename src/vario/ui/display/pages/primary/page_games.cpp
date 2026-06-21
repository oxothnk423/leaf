#include "ui/display/pages/primary/page_games.h"

#include <Arduino.h>
#include <U8g2lib.h>

#include "games/envelope_expansion.h"
#include "power.h"
#include "ui/audio/sound_effects.h"
#include "ui/audio/speaker.h"
#include "ui/display/display.h"
#include "ui/display/display_fields.h"
#include "ui/display/fonts.h"
#include "ui/input/buttons.h"

void gamesPage_draw() {
  envelopeExpansion.loadBest();

  u8g2.firstPage();
  do {
    display_headerAndFooter(false, false);

    u8g2.setFont(leaf_6x12);
    u8g2.setCursor(30, 27);
    u8g2.print("GAMES");
    u8g2.drawHLine(0, 31, 96);

    envelopeExpansion.draw();
  } while (u8g2.nextPage());
}

void gamesPage_button(Button button, ButtonEvent state, uint8_t count) {
  switch (button) {
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
    default:
      break;
  }

  display.update();
}
