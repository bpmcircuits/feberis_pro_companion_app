#include "dashboard.h"
#ifndef FBR_UI_RENDER
#include <gui/elements.h>
#endif
#include <stdio.h>
#include <string.h>

static void fit(Canvas* c, int x, int y, unsigned width, const char* source) {
    char text[64];
    snprintf(text, sizeof(text), "%s", source);
    if(canvas_string_width(c, text) > width) {
        size_t len = strlen(text);
        while(len && canvas_string_width(c, text) + canvas_string_width(c, "...") > width)
            text[--len] = 0;
        memcpy(text + len, "...", 4);
    }
    canvas_draw_str(c, x, y, text);
}
static void stat(Canvas* c, int x, uint64_t value, const char* label) {
    char text[16];
    ui_format_count(value, text, sizeof(text));
    canvas_set_font(c, FontPrimary);
    fit(c, x, 29, 56, text);
    canvas_set_font(c, FontSecondary);
    canvas_draw_str(c, x, 40, label);
}
void dashboard_draw(Canvas* c, void* context) {
    const DashboardSnapshot* s = context;
    const DashboardSnapshot* m = s;
    UiState state = s->state;
    static const char* const band_names[] = {"Both", "2.4G", "5G"};
    canvas_clear(c);
    canvas_set_color(c, ColorBlack);
    canvas_set_font(c, FontPrimary);
    fit(c, 4, 10, (state == UiNoWifi || state == UiNoMemory) ? 120 : 90, ui_state_title(state));
    canvas_set_font(c, FontSecondary);
    if(state != UiOffline && state != UiNoWifi && state != UiNoMemory)
        canvas_draw_str_aligned(c, 124, 10, AlignRight, AlignBottom, band_names[m->band]);
    canvas_draw_line(c, 4, 14, 123, 14);
    if(state == UiOffline) {
        canvas_draw_str(c, 4, 28, "Feberis WiFi wardriver");
        canvas_draw_str(c, 4, 40, "UART: USART");
        canvas_draw_str(c, 4, 50, "Press OK to connect");
    } else if(state == UiError || state == UiNoWifi || state == UiNoMemory) {
        canvas_draw_str(c, 4, 27,
                        (state == UiNoWifi || state == UiNoMemory) ? "Cannot start a scan"
                                                                   : "Recording stopped");
        fit(c, 4, 39, 120, m->message);
        canvas_draw_str(c, 4, 50, "Menu > Status for details");
    } else if(state == UiConnecting || state == UiStopping || state == UiWorking) {
        const char* detail = state == UiStopping  ? "Saving final records"
                             : state == UiWorking ? "Waiting for device"
                                                  : "Checking Feberis Pro";
        canvas_draw_str(c, 4, 28, detail);
        fit(c, 4, 40, 120, m->message);
        canvas_draw_str(c, 4, 50,
                        state == UiStopping ? "Wait for scan to finish" : "FBR1 revision 2");
    } else {
        stat(c, 4, m->unique, "Unique AP");
        stat(c, 68, m->rows, "SD saved");
        char gps[48];
        if(m->capture_degraded)
            snprintf(gps, sizeof(gps), "Capture off; see diagnostics");
        else if(m->fix)
            snprintf(gps, sizeof(gps), "GPS fix  %lu sat  H:%.5s", (unsigned long)m->satellites,
                     m->hdop);
        else
            snprintf(gps, sizeof(gps), "No fix; records kept on SD");
        fit(c, 4, 50, 120, gps);
    }
    elements_button_left(c, "Menu");
    const char* action = ui_primary_action(state);
    if(action) elements_button_center(c, action);
    elements_button_right(c, "AP");
}
