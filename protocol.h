#pragma once
#include "scan_metrics.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define FBR_RX_UNVERIFIED (1u << 7)
#define FBR_LINE_SIZE 1536
#define FBR_FIELDS 48

typedef struct {
    char data[FBR_LINE_SIZE];
    size_t used;
    bool dropping;
} FbrReader;
typedef struct {
    uint32_t boot, sequence, scan;
    char bssid[18], ssid[33], auth[65];
    int channel, rssi;
    bool fix;
    uint8_t csv[511];
    size_t csv_size;
} FbrAp;
uint16_t fbr_crc(const char* data, size_t size);
bool fbr_feed(FbrReader* reader, char byte);
size_t fbr_decode(char* line, char** fields);
size_t fbr_encode(const char* payload, char* out, size_t capacity);
bool fbr_u32(const char* text, uint32_t* out);
bool fbr_number(const char* text, double min, double max);
bool fbr_ap(char** fields, size_t count, FbrAp* ap);
bool fbr_status(char** fields, size_t count);
bool fbr_u64(const char* text, uint64_t* out);
bool fbr_observation(char** fields, size_t count, FbrAp* ap);
bool fbr_validate(char** fields, size_t count);
bool fbr_config(const uint32_t values[13]);
bool fbr_ssid(const char* hex, char* out, size_t capacity);
/* CSV is generated from validated fields; csv_hex is never trusted as export. */
size_t fbr_wigle(char** fields, char* out, size_t capacity);
size_t fbr_observation_csv(char** fields, char* out, size_t capacity);
size_t fbr_aggregate_csv(char** fields, char* out, size_t capacity);

bool fbr_utc(const char* text);
