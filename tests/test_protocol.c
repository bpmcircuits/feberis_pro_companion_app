#include "../protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static size_t decode(const char* payload, char* frame, char** f) {
    size_t n = fbr_encode(payload, frame, FBR_LINE_SIZE);
    assert(n);
    frame[n - 1] = 0;
    return fbr_decode(frame, f);
}
int main(void) {
    assert(fbr_crc("123456789", 9) == 0x29b1);
    char frame[FBR_LINE_SIZE];
    char* f[FBR_FIELDS];
    uint32_t v;
    assert(fbr_u32("4294967295", &v) && v == UINT32_MAX);
    assert(!fbr_u32("4294967296", &v));
    assert(!fbr_u32("-1", &v));
    assert(!fbr_u32("", &v));
    assert(!fbr_number("nan", -1, 1));
    assert(!fbr_number("0x1p0", -1, 1));
    assert(!fbr_number(" 1", -1, 1));
    assert(decode("FBR1\t0\tAP\t\t", frame, f) == 5 && !*f[3] && !*f[4]);
    frame[2] ^= 1;
    assert(!fbr_decode(frame, f));
    assert(decode("FBR1\t4294967296\tPING", frame, f) == 0);
    assert(decode("BOOT\t0\tREADY", frame, f) == 0);
    assert(!fbr_encode("long payload", frame, 5));
    FbrReader r = {0};
    const char* good = "FBR1\t1\tPING";
    size_t len = fbr_encode(good, frame, sizeof(frame));
    for(size_t i = 0; i < len - 1; ++i) assert(!fbr_feed(&r, frame[i]));
    assert(fbr_feed(&r, '\n'));
    for(unsigned i = 0; i < 2000; ++i) assert(!fbr_feed(&r, 'x'));
    assert(!fbr_feed(&r, '\n'));
    for(size_t i = 0; i < len - 1; ++i) assert(!fbr_feed(&r, frame[i]));
    assert(fbr_feed(&r, '\n'));
    assert(!fbr_feed(&r, '\r'));
    assert(!fbr_feed(&r, '\n'));
    char payload[1536] = "FBR1\t0\tX";
    for(unsigned i = 0; i < 46; ++i) strcat(payload, "\tx");
    assert(!decode(payload, frame, f));
    FbrAp ap;
    const char* ap_payload = "FBR1\t0\tAP\t7\t1\t1\tAA:BB:CC:DD:EE:FF\t\t[ESS]\t36\t-42\t1\t2026-"
                             "11-22 12:30:00\t52.1\t21.0\t100.0\t1.2\t8\t100\t612C620A";
    size_t n = decode(ap_payload, frame, f);
    assert(fbr_ap(f, n, &ap) && !*ap.ssid && ap.csv_size == 4);
    f[11] = "0";
    assert(!fbr_ap(f, n, &ap));
    f[19] = "-";
    assert(fbr_ap(f, n, &ap) && ap.csv_size == 0 && !ap.fix);
    f[7] = "0041FF";
    assert(fbr_ap(f, n, &ap) && !strcmp(ap.ssid, "?A?"));
    f[7] = "A";
    assert(!fbr_ap(f, n, &ap));
    f[7] = "GG";
    assert(!fbr_ap(f, n, &ap));
    f[7] = "";
    f[11] = "1";
    f[19] = "610A620A";
    assert(!fbr_ap(f, n, &ap));
    f[19] = "610D0A";
    assert(!fbr_ap(f, n, &ap));
    f[19] = "61";
    assert(!fbr_ap(f, n, &ap));
    f[19] = "-";
    f[13] = "91";
    assert(!fbr_ap(f, n, &ap));
    n = decode("FBR1\t0\tSTATUS\t7\t0\tIDLE\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0.00\t4294967295\t0."
               "0\t0.0\t0.0\t-\t1\t1",
               frame, f);
    assert(n == 24 && fbr_status(f, n));
    f[5] = "???";
    assert(!fbr_status(f, n));
    uint64_t u;
    assert(fbr_u64("18446744073709551615", &u) && u == UINT64_MAX);
    assert(!fbr_u64("18446744073709551616", &u));
    n = decode("FBR1\t0\tOBSERVATION\t7\t4294967295\t1\tAA:BB:CC:DD:EE:FF\t0041225CFF\t[ESS]\t36\t-"
               "42\t0\t2026-11-23 "
               "12:30:00\tnan\tnan\tnan\t1.2\t8\t100\t-"
               "\t5\t5180\t1\t18446744073709551615\tnan\tnan\tnan\t63",
               frame, f);
    assert(n == 28 && fbr_observation(f, n, &ap));
    char out[1536];
    assert(fbr_ssid(f[7], out, sizeof(out)) && !strcmp(out, "\\x00A\"\\x5C\\xFF"));
    assert(!fbr_wigle(f, out, sizeof(out)));
    f[11] = "1";
    assert(!fbr_observation(f, n, &ap));
    f[13] = f[14] = "0";
    assert(fbr_observation(f, n, &ap));
    assert(fbr_wigle(f, out, sizeof(out)) && strstr(out, "\"\\x00A\"\"\\x5C\\xFF\""));
    assert(strstr(out, ",0,0,,,WIFI\n"));
    f[8] = "[UNKNOWN][ESS]";
    assert(!fbr_wigle(f, out, sizeof(out)));
    f[8] = "[ESS]";
    f[11] = "0";
    f[13] = f[14] = "nan";
    f[22] = "129";
    assert(fbr_observation(f, n, &ap) && !fbr_wigle(f, out, sizeof(out)));
    f[22] = "4294967295";
    assert(fbr_observation(f, n, &ap));
    f[22] = "4294967296";
    assert(!fbr_observation(f, n, &ap));
    f[22] = "129";
    f[11] = "1";
    f[13] = f[14] = "0";
    assert(!fbr_observation(f, n, &ap) && !fbr_wigle(f, out, sizeof(out)));
    f[22] = "257";
    assert(fbr_observation(f, n, &ap) && fbr_wigle(f, out, sizeof(out)));
    n = decode("FBR1\t1\tSCAN_"
               "METRICS\t7\t1\t0\t120\t1\t4000\t20\t20\t25\t4020\t1000\t18446744073709551615\t3\t4"
               "\t5\t6\t7\t8\t9\t32768\t65536\t4096\t512",
               frame, f);
    assert(n == 26 && fbr_validate(f, n));
    assert(!fbr_validate(f, n - 1));
    f[7] = "2";
    assert(!fbr_validate(f, n));
    f[7] = "1";
    f[5] = "60001";
    assert(!fbr_validate(f, n));
    f[5] = "0";
    f[25] = "1025";
    assert(!fbr_validate(f, n));
    n = decode("FBR1\t1\tSTATE\t7\t1\t0\t0\t0\t0\t0\t20\t19\t12345678901", frame, f);
    assert(n == 13 && fbr_validate(f, n));
    n = decode("FBR1\t1\tGPS\t0\t-\tnan\tnan\tnan\tnan\t0\t4294967295\tnan\tnan\tnan", frame, f);
    assert(n == 14 && fbr_validate(f, n));
    puts("protocol: CRC, framing, limits, AP, STATUS OK");
}
