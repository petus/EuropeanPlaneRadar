// =============================================================================
//  PlaneRadar
//  Screen 1: aircraft radar - interface.
//
//  Project: PlaneRadar - live aircraft radar on a round touchscreen
//  Author:  Petr / chiptron.cz
//  Web:     https://chiptron.cz
//  Board:   Waveshare ESP32-S3-Touch-LCD-2.1 (round 480x480 display, ST7701)
// =============================================================================
#pragma once
#include <Arduino.h>

void ScreenPlanes_Enter();
void ScreenPlanes_Draw();
bool ScreenPlanes_Tick();                    // true = needs a redraw

// Short tap - select an aircraft / close the detail panel.
bool ScreenPlanes_HandleTap(int x, int y);
// Long press - change the range.
bool ScreenPlanes_HandleLongPress(int x, int y);

// Is the aircraft detail open? (main then blocks swipe-to-switch)
bool ScreenPlanes_DetailOpen();
