#pragma once
#include <stdbool.h>

extern volatile bool isCapturing;

void cam_controller_init();
void cam_espnow_init();