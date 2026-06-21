#ifndef PageGames_h
#define PageGames_h

#include <Arduino.h>

#include "ui/input/buttons.h"

void gamesPage_draw(void);
void gamesPage_button(Button button, ButtonEvent state, uint8_t count);

#endif
