#pragma once
#include "ui_model.h"
#ifdef FBR_UI_RENDER
#include "tests/canvas_host.h"
#else
#include <gui/canvas.h>
#endif
void dashboard_draw(Canvas* canvas, void* model);
