/* Uses the production dashboard.c and the exact firmware 1.4.3 fonts/decoder.
 * Pixel output is host-side; it does not emulate a physical LCD or native menus. */
#include "../dashboard.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void pixel(Canvas* c, int x, int y) {
    if(x >= 0 && x < 128 && y >= 0 && y < 64) c->pixels[y][x] = c->fb.draw_color;
}
void u8g2_DrawHVLine(u8g2_t* u, u8g2_uint_t x, u8g2_uint_t y, u8g2_uint_t len, uint8_t dir) {
    Canvas* c = (Canvas*)u;
    for(unsigned i = 0; i < len; ++i) pixel(c, x + (dir == 0 ? i : 0), y + (dir == 1 ? i : 0));
}
uint8_t u8g2_IsIntersection(u8g2_t* u, u8g2_uint_t x, u8g2_uint_t y, u8g2_uint_t x1,
                            u8g2_uint_t y1) {
    (void)u;
    (void)x;
    (void)y;
    (void)x1;
    (void)y1;
    return 1;
}
void canvas_clear(Canvas* c) { memset(c->pixels, 0, sizeof(c->pixels)); }
void canvas_set_color(Canvas* c, Color color) { c->fb.draw_color = color; }
void canvas_set_font(Canvas* c, Font font) {
    c->font = font;
    u8g2_SetFont(&c->fb, font == FontPrimary ? u8g2_font_helvB08_tr : u8g2_font_haxrcorp4089_tr);
    u8g2_SetFontPosBaseline(&c->fb);
    u8g2_SetFontMode(&c->fb, 1);
}
size_t canvas_string_width(Canvas* c, const char* s) { return u8g2_GetStrWidth(&c->fb, s); }
void canvas_draw_str(Canvas* c, int x, int y, const char* s) {
    assert(x >= 0 && y >= 0 && y <= 64);
    assert(x + (int)canvas_string_width(c, s) <= 128);
    u8g2_DrawStr(&c->fb, x, y, s);
}
void canvas_draw_str_aligned(Canvas* c, int x, int y, Align h, Align v, const char* s) {
    (void)v;
    if(h == AlignRight) x -= canvas_string_width(c, s);
    canvas_draw_str(c, x, y, s);
}
void canvas_draw_line(Canvas* c, int x, int y, int x2, int y2) {
    int dx = abs(x2 - x), sx = x < x2 ? 1 : -1, dy = -abs(y2 - y), sy = y < y2 ? 1 : -1,
        err = dx + dy;
    for(;;) {
        pixel(c, x, y);
        if(x == x2 && y == y2) break;
        int e = 2 * err;
        if(e >= dy) {
            err += dy;
            x += sx;
        }
        if(e <= dx) {
            err += dx;
            y += sy;
        }
    }
}
static void box(Canvas* c, int x, int y, int w, int h) {
    for(int i = 0; i < w; ++i)
        for(int j = 0; j < h; ++j) pixel(c, x + i, y + j);
}
/* Native footer geometry from elements.c; tiny key glyphs are drawn directly. */
static void arrow(Canvas* c, int x, int y, bool right) {
    for(int i = 0; i < 4; ++i) {
        int len = 7 - 2 * i;
        for(int j = 0; j < len; ++j) pixel(c, x + (right ? 3 - i : i), y + i + j);
    }
}
static void button(Canvas* c, const char* s, int side) {
    int width = canvas_string_width(c, s) + (side ? 13 : 12);
    int x = side < 0 ? 0 : side > 0 ? 128 - width : (128 - width) / 2;
    box(c, x, 52, width, 12);
    if(side >= 0)
        for(int i = 1; i <= 3; ++i) canvas_draw_line(c, x - i, 64, x - i, 51 + i);
    if(side <= 0)
        for(int i = 0; i < 3; ++i) canvas_draw_line(c, x + width + i, 64, x + width + i, 52 + i);
    c->fb.draw_color = 0;
    if(side < 0) {
        arrow(c, x + 3, 54, false);
        canvas_draw_str(c, x + 10, 61, s);
    } else if(side > 0) {
        canvas_draw_str(c, x + 3, 61, s);
        arrow(c, 121, 54, true);
    } else {
        canvas_draw_line(c, x + 1, 54, x + 7, 54);
        canvas_draw_line(c, x + 1, 60, x + 7, 60);
        canvas_draw_line(c, x + 1, 54, x + 1, 60);
        canvas_draw_line(c, x + 7, 54, x + 7, 60);
        canvas_draw_str(c, x + 11, 61, s);
    }
    c->fb.draw_color = 1;
}
void elements_button_left(Canvas* c, const char* s) { button(c, s, -1); }
void elements_button_center(Canvas* c, const char* s) { button(c, s, 0); }
void elements_button_right(Canvas* c, const char* s) { button(c, s, 1); }
static void save(Canvas* c, const char* directory, const char* name) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.pbm", directory, name);
    FILE* file = fopen(path, "wb");
    assert(file);
    fprintf(file, "P1\n128 64\n");
    for(unsigned y = 0; y < 64; ++y) {
        for(unsigned x = 0; x < 128; ++x) fprintf(file, "%u ", c->pixels[y][x]);
        fputc('\n', file);
    }
    fclose(file);
}
static void draw_snapshot(Canvas* canvas, const UiSnapshot* snapshot) {
    DashboardSnapshot compact;
    ui_dashboard_snapshot(snapshot, &compact);
    dashboard_draw(canvas, &compact);
}
int main(int argc, char** argv) {
    assert(argc == 2);
    Canvas c = {0};
    UiSnapshot s = {0};
    draw_snapshot(&c, &s);
    save(&c, argv[1], "offline");
    s.serial_active = s.startup = true;
    strcpy(s.data.message, "HELLO 2 / checking device");
    draw_snapshot(&c, &s);
    save(&c, argv[1], "connecting");
    s.startup = false;
    s.data.ready = s.data.wifi = s.data.idle = s.data.storage_ready = true;
    draw_snapshot(&c, &s);
    save(&c, argv[1], "ready");
    s.data.running = s.data.file_open = s.data.fix = true;
    s.data.unique = 1837;
    s.data.rows = 1294;
    s.data.satellites = 12;
    strcpy(s.data.hdop, "0.85");
    draw_snapshot(&c, &s);
    save(&c, argv[1], "recording");
    s.data.stopping = true;
    draw_snapshot(&c, &s);
    save(&c, argv[1], "stopping");
    s.failed = true;
    strcpy(s.data.message, "SD error; repair SD and Retry");
    draw_snapshot(&c, &s);
    save(&c, argv[1], "error");
    s.failed = s.data.stopping = false;
    s.data.unique = s.data.rows = UINT64_MAX;
    s.data.satellites = 255;
    strcpy(s.data.hdop, "100000.0000");
    draw_snapshot(&c, &s);
    save(&c, argv[1], "large_counts");
    s.data.running = s.data.file_open = s.data.wifi = false;
    draw_snapshot(&c, &s);
    save(&c, argv[1], "wifi_error");
    puts("dashboard: 8 production frames rendered with firmware fonts; text fits 128x64");
}
