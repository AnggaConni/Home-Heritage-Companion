#pragma once
#include <Arduino.h>
#include <lvgl.h>

void musicPlayerInit();
void musicPlayerLoop();
void musicPlayerPauseForFetch();
void musicPlayerResumeAfterFetch();
void musicPlayerShow(lv_obj_t *root, lv_event_cb_t backCallback);
