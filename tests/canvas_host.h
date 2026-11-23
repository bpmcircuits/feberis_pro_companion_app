#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <u8g2.h>
typedef enum { FontPrimary, FontSecondary } Font;
typedef enum { ColorWhite, ColorBlack } Color;
typedef enum { AlignLeft, AlignRight, AlignTop, AlignBottom, AlignCenter } Align;
typedef struct {
    u8g2_t fb;
    unsigned char pixels[64][128];
    Font font;
} Canvas;
void canvas_clear(Canvas* c);
void canvas_set_color(Canvas* c, Color color);
void canvas_set_font(Canvas* c, Font font);
size_t canvas_string_width(Canvas* c, const char* s);
void canvas_draw_str(Canvas* c, int x, int y, const char* s);
void canvas_draw_str_aligned(Canvas* c, int x, int y, Align h, Align v, const char* s);
void canvas_draw_line(Canvas* c, int x, int y, int x2, int y2);
void elements_button_left(Canvas* c, const char* s);
void elements_button_center(Canvas* c, const char* s);
void elements_button_right(Canvas* c, const char* s);
