#include "protocol.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char* const fbr_scan_metric_names[FBR_SCAN_METRIC_FIELDS] = {"boot",
                                                                   "session",
                                                                   "requested_gap_ms",
                                                                   "dwell_ms",
                                                                   "capture_ready",
                                                                   "radio_ms",
                                                                   "ingest_ms",
                                                                   "gap_ms",
                                                                   "max_gap_ms",
                                                                   "period_ms",
                                                                   "ingest_cpu_us",
                                                                   "captured",
                                                                   "dropped",
                                                                   "malformed",
                                                                   "cache_overflows",
                                                                   "metadata_hits",
                                                                   "metadata_misses",
                                                                   "queue_high_water",
                                                                   "backpressure_ms",
                                                                   "scan_buffer_bytes",
                                                                   "capture_cache_bytes",
                                                                   "main_stack_free",
                                                                   "scan_capacity"};

uint16_t fbr_crc(const char* data, size_t size) {
    uint16_t crc = 0xffff;
    for(size_t i = 0; i < size; ++i) {
        crc ^= (uint16_t)(uint8_t)data[i] << 8;
        for(unsigned b = 0; b < 8; ++b)
            crc = crc & 0x8000 ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}
static int hex(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool fbr_feed(FbrReader* r, char byte) {
    if(byte == '\n') {
        bool ready = !r->dropping && r->used;
        r->data[r->used] = 0;
        r->used = 0;
        r->dropping = false;
        return ready;
    }
    if(r->dropping) return false;
    if(byte == 0 || byte == '\r' || r->used >= FBR_LINE_SIZE - 2) {
        r->dropping = true;
        return false;
    }
    r->data[r->used++] = byte;
    return false;
}
size_t fbr_decode(char* line, char** f) {
    char* star = strchr(line, '*');
    if(!star || strlen(star + 1) != 4 || strncmp(line, "FBR1\t", 5)) return 0;
    uint16_t expected = 0;
    for(unsigned i = 1; i <= 4; ++i) {
        int digit = hex(star[i]);
        if(digit < 0) return 0;
        expected = (expected << 4) | digit;
    }
    for(char* p = line; p < star; ++p)
        if(*p != '\t' && ((uint8_t)*p < 32 || (uint8_t)*p > 126)) return 0;
    if(expected != fbr_crc(line, star - line)) return 0;
    *star = 0;
    size_t n = 1;
    f[0] = line;
    for(char* p = line; p < star; ++p) {
        if(*p == '\t') {
            if(n == FBR_FIELDS) return 0;
            *p = 0;
            f[n++] = p + 1;
        }
    }
    uint32_t id;
    return n >= 3 && fbr_u32(f[1], &id) ? n : 0;
}
size_t fbr_encode(const char* payload, char* out, size_t capacity) {
    int n = snprintf(out, capacity, "%s*%04X\n", payload, fbr_crc(payload, strlen(payload)));
    return n > 0 && (size_t)n < capacity ? (size_t)n : 0;
}
bool fbr_u32(const char* s, uint32_t* out) {
    if(!s || !*s) return false;
    uint32_t v = 0;
    for(; *s; ++s) {
        if(*s < '0' || *s > '9' || v > (UINT32_MAX - (unsigned)(*s - '0')) / 10) return false;
        v = v * 10 + *s - '0';
    }
    *out = v;
    return true;
}
bool fbr_number(const char* s, double min, double max) {
    if(!s || !*s) return false;
    /* Only ordinary decimal notation, no whitespace, NaN, infinity or hex floats. */
    const char* p = s;
    if(*p == '-' || *p == '+') ++p;
    unsigned digits = 0, dots = 0;
    for(; *p; ++p) {
        if(*p == '.')
            ++dots;
        else if(*p >= '0' && *p <= '9')
            ++digits;
        else
            return false;
    }
    if(!digits || dots > 1) return false;
    char* end;
    double v = strtod(s, &end);
    return !*end && isfinite(v) && v >= min && v <= max;
}
static bool uint_range(const char* s, uint32_t min, uint32_t max) {
    uint32_t v;
    return fbr_u32(s, &v) && v >= min && v <= max;
}
bool fbr_utc(const char* s) {
    if(!strcmp(s, "-")) return true;
    if(strlen(s) != 19) return false;
    for(unsigned i = 0; i < 19; ++i) {
        char sep = i == 4 || i == 7 ? '-' : i == 10 ? ' ' : i == 13 || i == 16 ? ':' : 0;
        if(sep ? s[i] != sep : (s[i] < '0' || s[i] > '9')) return false;
    }
    unsigned year = (unsigned)atoi(s), month = (unsigned)atoi(s + 5), day = (unsigned)atoi(s + 8);
    static const unsigned days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    return year >= 1 && month >= 1 && month <= 12 && day >= 1 &&
           day <= days[month - 1] + (month == 2 && leap) && atoi(s + 11) < 24 &&
           atoi(s + 14) < 60 && atoi(s + 17) < 60;
}
static bool optional(const char* s, double min, double max);
static bool position(char** f, unsigned fix, unsigned sat, unsigned hdop, unsigned age,
                     unsigned lat, unsigned lon, unsigned alt, unsigned time) {
    return uint_range(f[fix], 0, 1) && uint_range(f[sat], 0, 255) && optional(f[hdop], 0, 100000) &&
           uint_range(f[age], 0, UINT32_MAX) &&
           (!strcmp(f[fix], "1") ? fbr_number(f[lat], -90, 90) && fbr_number(f[lon], -180, 180)
                                 : optional(f[lat], -90, 90) && optional(f[lon], -180, 180)) &&
           optional(f[alt], -100000, 100000) && fbr_utc(f[time]) &&
           (strcmp(f[fix], "1") || strcmp(f[time], "-"));
}
bool fbr_ap(char** f, size_t n, FbrAp* ap) {
    if(n != 20 || strcmp(f[1], "0") || strcmp(f[2], "AP") || !fbr_u32(f[3], &ap->boot) ||
       !fbr_u32(f[4], &ap->sequence) || !ap->sequence || !fbr_u32(f[5], &ap->scan) ||
       strlen(f[6]) != 17 || strlen(f[7]) > 64 || strlen(f[7]) % 2 || !*f[8] || strlen(f[8]) > 64 ||
       !uint_range(f[9], 1, 196) || !fbr_number(f[10], -127, 0) || strchr(f[10], '.') ||
       !position(f, 11, 17, 16, 18, 13, 14, 15, 12))
        return false;
    for(unsigned i = 0; i < 17; ++i)
        if(i % 3 == 2 ? f[6][i] != ':' : hex(f[6][i]) < 0) return false;
    for(size_t i = 0; i < strlen(f[7]); i += 2) {
        int hi = hex(f[7][i]), lo = hex(f[7][i + 1]);
        if(hi < 0 || lo < 0) return false;
        unsigned c = hi * 16 + lo;
        ap->ssid[i / 2] = c >= 32 && c <= 126 ? (char)c : '?';
    }
    ap->ssid[strlen(f[7]) / 2] = 0;
    strcpy(ap->bssid, f[6]);
    strcpy(ap->auth, f[8]);
    ap->channel = atoi(f[9]);
    ap->rssi = atoi(f[10]);
    ap->fix = !strcmp(f[11], "1");
    ap->csv_size = 0;
    if(strcmp(f[19], "-")) {
        size_t len = strlen(f[19]);
        if(!ap->fix || !len || len % 2 || len > sizeof(ap->csv) * 2) return false;
        for(size_t i = 0; i < len; i += 2) {
            int hi = hex(f[19][i]), lo = hex(f[19][i + 1]);
            if(hi < 0 || lo < 0) return false;
            unsigned c = hi * 16 + lo;
            if(c == '\r' || (c == '\n' && i != len - 2) || !c) return false;
            ap->csv[ap->csv_size++] = c;
        }
        if(ap->csv[ap->csv_size - 1] != '\n') return false;
    }
    return true;
}
bool fbr_status(char** f, size_t n) {
    if(n != 24 || strcmp(f[2], "STATUS") || !uint_range(f[3], 0, UINT32_MAX) ||
       !uint_range(f[4], 0, 1) ||
       (strcmp(f[5], "IDLE") && strcmp(f[5], "SCANNING") && strcmp(f[5], "DRAINING")))
        return false;
    for(unsigned i = 6; i <= 13; ++i)
        if(!uint_range(f[i], 0, UINT32_MAX)) return false;
    return position(f, 14, 15, 16, 17, 18, 19, 20, 21) && uint_range(f[22], 0, 1) &&
           uint_range(f[23], 0, 1);
}

bool fbr_u64(const char* s, uint64_t* out) {
    if(!s || !*s) return false;
    uint64_t v = 0;
    for(; *s; ++s) {
        if(*s < '0' || *s > '9' || v > (UINT64_MAX - (unsigned)(*s - '0')) / 10) return false;
        v = v * 10 + *s - '0';
    }
    *out = v;
    return true;
}
static bool optional(const char* s, double min, double max) {
    return !strcmp(s, "nan") || fbr_number(s, min, max);
}
static bool mac(const char* s) {
    if(strlen(s) != 17) return false;
    for(unsigned i = 0; i < 17; ++i)
        if(i % 3 == 2 ? s[i] != ':' : hex(s[i]) < 0) return false;
    return true;
}
bool fbr_ssid(const char* s, char* out, size_t capacity) {
    size_t len = strlen(s), used = 0;
    if(len > 64 || len % 2) return false;
    for(size_t i = 0; i < len; i += 2) {
        int hi = hex(s[i]), lo = hex(s[i + 1]);
        if(hi < 0 || lo < 0) return false;
        unsigned c = hi * 16 + lo;
        if(c >= 32 && c <= 126 && c != '\\') {
            if(used + 1 >= capacity) return false;
            out[used++] = c;
        } else {
            if(used + 4 >= capacity) return false;
            snprintf(out + used, capacity - used, "\\x%02X", c);
            used += 4;
        }
    }
    out[used] = 0;
    return true;
}
bool fbr_config(const uint32_t v[13]) {
    return v[0] == 1 && v[1] <= 2 && v[2] <= 2 && v[3] <= 1 && v[4] >= 20 && v[4] <= 1500 &&
           v[5] >= 1 && v[5] <= 10000 && v[6] >= 1 && v[6] <= 100 && v[7] >= 1000 &&
           v[7] <= 3600000 && v[8] <= 60000 && v[7] >= v[8] && v[9] >= 100 && v[9] <= 10000 &&
           v[10] >= 1 && v[10] <= 500 && v[11] <= 60000 &&
           (v[12] == 115200 || v[12] == 460800 || v[12] == 921600);
}
static bool coordinates(char** f, unsigned valid, unsigned lat, unsigned lon) {
    return uint_range(f[valid], 0, 1) &&
           (!strcmp(f[valid], "1") ? fbr_number(f[lat], -90, 90) && fbr_number(f[lon], -180, 180)
                                   : optional(f[lat], -90, 90) && optional(f[lon], -180, 180));
}
bool fbr_observation(char** f, size_t n, FbrAp* ap) {
    uint64_t mono;
    char ssid[129];
    if(n != 28 || strcmp(f[1], "0") || strcmp(f[2], "OBSERVATION") || !fbr_u32(f[3], &ap->boot) ||
       !fbr_u32(f[4], &ap->sequence) || !ap->sequence || !fbr_u32(f[5], &ap->scan) || !ap->scan ||
       !mac(f[6]) || !fbr_ssid(f[7], ssid, sizeof(ssid)) || !*f[8] || strlen(f[8]) > 64 ||
       !uint_range(f[9], 1, 196) || !fbr_number(f[10], -127, 0) || strchr(f[10], '.') ||
       !coordinates(f, 11, 13, 14) || !fbr_utc(f[12]) || !optional(f[15], -100000, 100000) ||
       !optional(f[16], 0, 100000) || !uint_range(f[17], 0, 255) ||
       !uint_range(f[18], 0, UINT32_MAX) || (strcmp(f[20], "2") && strcmp(f[20], "5")) ||
       !uint_range(f[21], 2300, 6000) || !uint_range(f[22], 0, UINT32_MAX) ||
       !fbr_u64(f[23], &mono) || !optional(f[24], 0, 10000) || !optional(f[25], 0, 360) ||
       !optional(f[26], 0, 1000000) || !uint_range(f[27], 0, 63))
        return false;
    uint32_t reason;
    fbr_u32(f[22], &reason);
    if((reason & FBR_RX_UNVERIFIED) &&
       (strcmp(f[11], "0") || strcmp(f[13], "nan") || strcmp(f[14], "nan") || strcmp(f[19], "-")))
        return false;
    if(strcmp(f[19], "-")) {
        size_t len = strlen(f[19]);
        if(strcmp(f[11], "1") || !strcmp(f[12], "-") || !len || len % 2 || len > 1022) return false;
        for(size_t i = 0; i < len; i += 2) {
            int hi = hex(f[19][i]), lo = hex(f[19][i + 1]);
            if(hi < 0 || lo < 0) return false;
            unsigned c = hi * 16 + lo;
            if(!c || c == '\r' || (c == '\n' && i != len - 2)) return false;
        }
        if(strcmp(f[19] + len - 2, "0A") && strcmp(f[19] + len - 2, "0a")) return false;
    }
    strcpy(ap->bssid, f[6]);
    /* Compact preview; journal/export preserve every original SSID byte. */
    snprintf(ap->ssid, sizeof(ap->ssid), "%.32s", ssid);
    strcpy(ap->auth, f[8]);
    ap->channel = atoi(f[9]);
    ap->rssi = atoi(f[10]);
    ap->fix = !strcmp(f[11], "1");
    ap->csv_size = 0;
    return true;
}
bool fbr_validate(char** f, size_t n) {
    uint32_t v[15];
    uint64_t u;
    if(n < 3) return false;
    if(!strcmp(f[2], "OBSERVATION")) {
        FbrAp ap;
        return fbr_observation(f, n, &ap);
    }
    if(!strcmp(f[2], "STATUS")) return fbr_status(f, n);
    if(!strcmp(f[2], "CONFIG")) {
        if(n != 16) return false;
        for(unsigned i = 0; i < 13; ++i)
            if(!fbr_u32(f[i + 3], &v[i])) return false;
        return fbr_config(v);
    }
    if(!strcmp(f[2], "INFO")) {
        return n == 10 && strlen(f[3]) < 16 && fbr_u32(f[4], &v[0]) &&
               uint_range(f[5], 115200, 921600) && strlen(f[6]) == 2 &&
               uint_range(f[7], 0, 12000) && uint_range(f[8], 0, 8192) && !strcmp(f[9], "2");
    }
    if(!strcmp(f[2], "MEMORY")) {
        if(n != 18) return false;
        for(unsigned i = 3; i < 18; ++i)
            if(!fbr_u32(f[i], &v[i - 3])) return false;
        return v[0] <= 1 && v[1] <= 1 && v[12] <= v[11] && v[14] <= v[13] && v[11] <= 12000 &&
               v[13] <= 8192;
    }
    if(!strcmp(f[2], "STATE")) {
        if(n != 13) return false;
        for(unsigned i = 3; i <= 11; ++i)
            if(!fbr_u32(f[i], &v[i - 3])) return false;
        return v[2] <= 1 && v[3] <= 1 && v[4] <= 1 && v[5] <= 1 && v[6] <= 8192 && v[8] <= v[7] &&
               fbr_u64(f[12], &u);
    }
    if(!strcmp(f[2], "STATS")) {
        if(n != 26 || !uint_range(f[3], 0, 1) || !fbr_u32(f[4], &v[0])) return false;
        for(unsigned i = 5; i < 26; ++i)
            if(!fbr_u64(f[i], &u)) return false;
        return true;
    }
    if(!strcmp(f[2], "SCAN_METRICS")) {
        if(n != 26 || !uint_range(f[3], 0, UINT32_MAX) || !uint_range(f[4], 0, UINT32_MAX) ||
           !uint_range(f[5], 0, 60000) || !uint_range(f[6], 20, 1500) || !uint_range(f[7], 0, 1) ||
           !uint_range(f[20], 0, 8192) || !uint_range(f[25], 16, 1024))
            return false;
        for(unsigned i = 8; i <= 12; ++i)
            if(!uint_range(f[i], 0, UINT32_MAX)) return false;
        for(unsigned i = 13; i <= 19; ++i)
            if(!fbr_u64(f[i], &u)) return false;
        if(!fbr_u64(f[21], &u)) return false;
        for(unsigned i = 22; i <= 24; ++i)
            if(!uint_range(f[i], 0, UINT32_MAX)) return false;
        return true;
    }
    if(!strcmp(f[2], "GPS")) {
        return n == 14 && coordinates(f, 3, 5, 6) && fbr_utc(f[4]) &&
               optional(f[7], -100000, 100000) && optional(f[8], 0, 100000) &&
               uint_range(f[9], 0, 255) && uint_range(f[10], 0, UINT32_MAX) &&
               optional(f[11], 0, 10000) && optional(f[12], 0, 360) && optional(f[13], 0, 1000000);
    }
    if(!strcmp(f[2], "AP_DISCOVERED") || !strcmp(f[2], "AP_UPDATED")) {
        char ssid[129];
        if(n != 29 || !fbr_u32(f[3], &v[0]) || !fbr_u32(f[4], &v[1]) ||
           !uint_range(f[5], 0, 11999) || !mac(f[6]) || !fbr_ssid(f[7], ssid, sizeof(ssid)) ||
           (strcmp(f[8], "2") && strcmp(f[8], "5")) || !uint_range(f[9], 1, 196) || !*f[10] ||
           strlen(f[10]) > 64)
            return false;
        for(unsigned i = 11; i <= 13; ++i)
            if(!fbr_number(f[i], -127, 0)) return false;
        for(unsigned i = 14; i <= 18; ++i)
            if(!fbr_u64(f[i], &u)) return false;
        return coordinates(f, 19, 20, 21) && coordinates(f, 22, 23, 24) && fbr_utc(f[25]) &&
               fbr_utc(f[26]) && uint_range(f[27], 2300, 6000) && uint_range(f[28], 0, 63);
    }
    return false;
}
/* All free text is CSV quoted, even auth from a future firmware. */
static bool append(char* out, size_t cap, size_t* used, const char* s, bool quote) {
    if(quote) {
        if(*used + 1 >= cap) return false;
        out[(*used)++] = '"';
    }
    for(; *s; ++s) {
        if(*used + (*s == '"' && quote ? 2 : 1) >= cap) return false;
        if(*s == '"' && quote) out[(*used)++] = '"';
        out[(*used)++] = *s;
    }
    if(*used + 2 >= cap) return false;
    if(quote) out[(*used)++] = '"';
    out[(*used)++] = ',';
    out[*used] = 0;
    return true;
}
static size_t csv_fields(const char** fields, unsigned count, unsigned quoted, char* out,
                         size_t cap) {
    size_t used = 0;
    for(unsigned i = 0; i < count; ++i)
        if(!append(out, cap, &used, fields[i], quoted & (1u << i))) return 0;
    out[used - 1] = '\n';
    return used;
}
/* CSV text keeps canonical UTF-8; display escaping remains independent. */
static size_t ssid_utf8_length(const uint8_t* s, size_t remaining) {
    uint8_t c = s[0];
    size_t length = c >= 0xc2 && c <= 0xdf   ? 2
                    : c >= 0xe0 && c <= 0xef ? 3
                    : c >= 0xf0 && c <= 0xf4 ? 4
                                             : 0;
    if(!length || remaining < length) return 0;
    for(size_t i = 1; i < length; ++i)
        if(s[i] < 0x80 || s[i] > 0xbf) return 0;
    if((c == 0xe0 && s[1] < 0xa0) || (c == 0xed && s[1] >= 0xa0) || (c == 0xf0 && s[1] < 0x90) ||
       (c == 0xf4 && s[1] >= 0x90))
        return 0;
    return length;
}
static bool wigle_ssid(const char* encoded, char out[129]) {
    size_t hex_length = strlen(encoded);
    if(hex_length > 64 || hex_length % 2) return false;
    uint8_t bytes[32];
    size_t length = hex_length / 2, used = 0;
    bool hidden = true;
    for(size_t i = 0; i < length; ++i) {
        int hi = hex(encoded[2 * i]), lo = hex(encoded[2 * i + 1]);
        if(hi < 0 || lo < 0) return false;
        bytes[i] = hi * 16 + lo;
        if(bytes[i]) hidden = false;
    }
    for(size_t i = 0; !hidden && i < length; ++i) {
        uint8_t c = bytes[i];
        size_t sequence = ssid_utf8_length(bytes + i, length - i);
        if(c >= 32 && c < 127 && c != '\\') {
            out[used++] = c;
        } else if(sequence) {
            memcpy(out + used, bytes + i, sequence);
            used += sequence;
            i += sequence - 1;
        } else {
            snprintf(out + used, 129 - used, "\\x%02X", c);
            used += 4;
        }
    }
    out[used] = 0;
    return true;
}
size_t fbr_wigle(char** f, char* out, size_t cap) {
    uint32_t reason;
    if(!fbr_u32(f[22], &reason) || (reason & FBR_RX_UNVERIFIED) || strcmp(f[11], "1") ||
       !strcmp(f[12], "-") || strstr(f[8], "UNKNOWN"))
        return 0;
    char ssid[129];
    if(!wigle_ssid(f[7], ssid)) return 0;
    const char* cols[] = {f[6],
                          ssid,
                          f[8],
                          f[12],
                          f[9],
                          f[10],
                          f[13],
                          f[14],
                          strcmp(f[15], "nan") ? f[15] : "",
                          strcmp(f[26], "nan") ? f[26] : "",
                          "WIFI"};
    return csv_fields(cols, 11, (1u << 1) | (1u << 2), out, cap);
}
size_t fbr_observation_csv(char** f, char* out, size_t cap) {
    bool valid = !strcmp(f[11], "1");
    const char* cols[] = {f[3],
                          f[5],
                          f[4],
                          f[6],
                          f[7],
                          strcmp(f[12], "-") ? f[12] : "",
                          f[23],
                          f[10],
                          f[9],
                          f[20],
                          valid ? f[13] : "",
                          valid ? f[14] : "",
                          valid && strcmp(f[15], "nan") ? f[15] : "",
                          valid && strcmp(f[16], "nan") ? f[16] : "",
                          f[17],
                          f[11],
                          f[22],
                          f[21],
                          strcmp(f[24], "nan") ? f[24] : "",
                          strcmp(f[25], "nan") ? f[25] : "",
                          valid && strcmp(f[26], "nan") ? f[26] : "",
                          f[27]};
    return csv_fields(cols, 22, 0, out, cap);
}
size_t fbr_aggregate_csv(char** f, char* out, size_t cap) {
    const char* cols[] = {f[3],
                          f[4],
                          f[6],
                          f[7],
                          f[8],
                          f[9],
                          f[10],
                          f[14],
                          f[15],
                          f[12],
                          f[13],
                          f[11],
                          f[16],
                          f[17],
                          f[22],
                          !strcmp(f[22], "1") ? f[23] : "",
                          !strcmp(f[22], "1") ? f[24] : "",
                          f[19],
                          !strcmp(f[19], "1") ? f[20] : "",
                          !strcmp(f[19], "1") ? f[21] : "",
                          f[18],
                          strcmp(f[25], "-") ? f[25] : "",
                          strcmp(f[26], "-") ? f[26] : "",
                          f[27],
                          f[28]};
    return csv_fields(cols, 25, 1u << 6, out, cap);
}
