# FBR1 revision 2 integration reference

This document describes the protocol consumed by companion application 0.4 for
Feberis Pro firmware 0.4.0. The authoritative firmware contract is
[FLIPPER_INTEGRATION.md in the firmware project](https://github.com/bpmcircuits/feberis_pro_wardriver/blob/HEAD/docs/FLIPPER_INTEGRATION.md).
The companion always negotiates revision 2; it does not mix legacy AP delivery
with OBSERVATION delivery during a session.

## Transport and framing

UART: 8N1, 3.3 V, no flow control. Supported baud rates are 115200, 460800, and
921600. The upstream 0.3.1 firmware test verified all three using a PC receiver;
the companion FAP and its wiring still require physical testing.

```text
FBR1<TAB>id<TAB>type<TAB>field...*HHHH<LF>
```

CRC is **CRC-16/CCITT-FALSE**, polynomial `0x1021`, initial value `0xFFFF`, no
reflection, no final XOR. It covers the first `F` through the byte before `*`.
The check vector `123456789` produces `29B1`.

Frames are ASCII. TAB separates fields, including meaningful empty fields. Raw
TAB/CR/LF/asterisk cannot occur inside a field; binary data is hex encoded. The
buffer is 1536 bytes including termination, with up to 48 fields. On overflow,
discard the entire line until LF. Frames with an invalid CRC are ignored without
NACK because their request ID cannot be trusted.

Request IDs are uint32 greater than zero. Events use ID zero. Ordinary responses
are `OK code` or `ERR code`; typed replies use INFO, CONFIG, MEMORY, STATE, GPS,
STATS, or AP aggregate types. ACK is one-way. The ESP does not cache responses by
request ID, so state-changing commands must not be blindly repeated after timeout.

## Negotiation and commands

Connection begins with `HELLO 2`, followed by GET_INFO, GET_CONFIG, GET_MEMORY,
and GET_STATUS. The companion also uses legacy STATUS to confirm phase IDLE.

The table omits the common `FBR1 id` prefix. Spaces between displayed arguments
represent TAB-separated wire fields.

| Command | Reply or purpose |
| --- | --- |
| `HELLO 2` | `OK HELLO version boot baud country max_scan_records ack_ms retries` |
| `GET_INFO` | `INFO version boot baud country ap_capacity queue_capacity revision` |
| `GET_CONFIG` | `CONFIG` followed by 13 configuration fields |
| `GET_SCAN_METRICS` | Optional 26-field SCAN_METRICS reply, described below |
| `GET_MEMORY` | `MEMORY` followed by the 15 fields below |
| `GET_STATUS` | `STATE boot session running paused scanning link_wait queued last_sequence committed elapsed_ms` |
| `STATUS` | The 24-field compatibility status frame |
| `GET_GPS` | `GPS valid utc lat lon altitude_m hdop satellites age_ms speed_kmh course_deg horizontal_accuracy_m` |
| `GET_STATS` | Current-session STATS |
| `GET_AP slot` | AP_DISCOVERED or AP_UPDATED for an occupied slot; `OK EMPTY` otherwise |
| `START_SCAN [gap_ms]` | `OK START`; extra post-batch delay 0–60000 ms, default 0 |
| `STOP_SCAN` | `OK STOP`; stop future scans and preserve accepted queued records |
| `PAUSE_SCAN` | `OK PAUSE_SCAN`; complete/process the current batch and block future scans |
| `RESUME_SCAN` | `OK RESUME_SCAN`; continue the same running session |
| `SET_CONFIG` + 13 fields | `OK SET_CONFIG` after validation and NVS save |
| `ACK boot session sequence` | Confirm the durably stored queue head |
| `RESUME_LINK boot session committed` | `OK RESUME_LINK` after checkpoint reconciliation |
| `NACK STORAGE` | `OK NACK`; pause scans and transport while retaining queued data |
| `PING` | `OK PONG` |

Firmware additionally provides compatibility aliases and operations such as
`GET_STATS TOTAL`, `RESET_STATS`, and `BAND`; their availability does not imply a
corresponding companion UI action. See the upstream contract for those operations.

HELLO's response retains its compatible layout. INFO must report revision 2.
The companion accepts firmware versions 0.4.0, 0.3.1, 0.3.0, and 0.2.0. Only firmware 0.2.0 may
return UNKNOWN_COMMAND for GET_MEMORY; the companion then uses INFO capacities
and reports that extended diagnostics are unavailable.

## Configuration

CONFIG and SET_CONFIG contain 16 fields including the common prefix, ID, and type.

```text
version mode band active dwell_ms min_distance_m rssi_delta_db max_gap_ms
min_gap_ms gps_max_age_ms gps_max_hdop_x10 full_gap_ms baud
```

Default firmware values:

```text
1 0 0 0 120 15 6 30000 2000 3000 50 2000 115200
```

- `mode`: 0 SMART, 1 FULL, 2 UNIQUE.
- `band`: 0 BOTH, 1 2.4 GHz, 2 5 GHz.
- `active`: 0 passive, 1 active probing, enabled only through an explicit GUI choice.
- Numeric ranges are listed in the [user guide](USER_GUIDE.md#settings).

Change configuration between completed batches. During recording the companion
pauses, waits for no active scan, no queued observations, and phase IDLE, applies
SET_CONFIG, reads CONFIG back, then resumes only if it was not already paused.

Changing baud requires a stopped session and empty queue. The ESP replies at the
old baud, waits for transmit completion, and switches. The companion switches
after receiving the complete OK and sends HELLO 2 again. If the OK is lost, recovery
retries HELLO at the current speed before probing alternatives. HELLO/PING and
read-only GET/STATUS queries have at most two retries with the same request ID.
START, STOP, SET_CONFIG, and other state-changing operations are not automatically
replayed. This covers a first reply lost while the serial bridge settles, without
applying SET_CONFIG again. The configured baud survives ESP restart through NVS.

## Memory diagnostics

MEMORY has 18 fields including prefix, ID, and type:

```text
FBR1 id MEMORY storage_ready psram_initialized psram_total_bytes psram_free_bytes
psram_minimum_free_bytes psram_largest_free_block internal_free_bytes
internal_minimum_free_bytes internal_largest_free_block storage_reserved_bytes
ap_record_bytes ap_capacity ap_count queue_capacity queued
```

Use actual capacities from INFO/MEMORY rather than assuming a fixed AP or queue
limit. For firmware 0.3.0/0.3.1/0.4.0, the companion requires storage readiness and usable,
consistent limits before Start. SRAM and PSRAM values remain separate; total heap
statistics are not substitutes for these diagnostics. NVS and flash capacity do
not make queued PSRAM observations persistent after power loss.

## Observations

OBSERVATION has **28 fields**, with zero-based indexes:

| Index | Field |
| --- | --- |
| 0–2 | FBR1, 0, OBSERVATION |
| 3–5 | boot, sequence, session |
| 6–8 | bssid, ssid_hex, auth |
| 9–12 | channel, rssi, fix_valid, utc |
| 13–19 | latitude, longitude, altitude_m, hdop, satellites, age_ms, csv_hex |
| 20–23 | band, frequency_mhz, reason_mask, detected_monotonic_ms |
| 24–27 | speed_kmh, course_deg, horizontal_accuracy_m, phy_mask |

BSSID is a colon-separated MAC. SSID hex represents zero to 32 original bytes.
UTC is `YYYY-MM-DD HH:MM:SS` or `-`. Band is 2 or 5. Sequence is a nonzero uint32
and does not wrap within a boot; monotonic time is uint64. Optional numeric values
may be `nan`; a valid position requires finite in-range coordinates.

Reason bits: 1 FIRST, 2 DISTANCE, 4 RSSI, 8 INTERVAL, 16 METADATA,
32 GPS_RECOVERED, 64 FULL_SAMPLE. Bit 128 RX_UNVERIFIED is provenance, not a write
trigger. Unknown reason bits are retained as uint32 and do not reject otherwise
supported observations. PHY bits: bit 0 11b, bit 1 11g, bit 2 11n,
bit 3 11a, bit 4 11ac, bit 5 11ax.

No fix means missing coordinates, not 0,0. Fresh UTC can exist without valid
geolocation. In 0.4.0, verified position/RSSI/time come from the strongest captured
beacon/probe response, its software receive-callback timestamp, and the latest
historical GNSS snapshot available no later than reception. Receiver/sample-rate
uncertainty still applies. No later fix is substituted on retry.

RX_UNVERIFIED means no matching reception metadata: fix_valid=0, latitude/longitude
are `nan`, csv_hex is `-`, and monotonic time falls back to scan completion. Retain
it in the journal and exclude it from WiGLE. Legacy firmware uses scan-end position
timestamps. RMC/GGA currently reports no measured horizontal accuracy;
HDOP is never converted to meters. Missing RMC speed/course is `nan`, not a
measured zero; the GUI shows it as unavailable and observation CSV fields are empty.

The companion preserves the raw frame in the journal and generates CSV from the
validated observation fields. It does not use `csv_hex` as its exported WiGLE row.

## AP aggregates

GET_AP replies contain **29 fields**:

```text
FBR1 id AP_DISCOVERED|AP_UPDATED boot session slot bssid ssid_hex band channel auth
last_rssi strongest_rssi mean_rssi detection_count observation_count first_seen_ms
last_seen_ms metadata_change_count last_fix_valid last_lat last_lon best_fix_valid
best_lat best_lon first_utc last_utc frequency_mhz phy_mask
```

AP_DISCOVERED/AP_UPDATED are request responses, not a separate reliable event
stream. Slots are dense from 0 through `unique_aps-1` and stable within a session.
After Stop and queue drain, fetch exactly those slots for the final table.
Detection counts and mean RSSI include filtered detections, so they cannot be
reconstructed solely from OBSERVATION records.

A strongest measurement point is not an estimate of router installation location.
Metadata changes may indicate a changed device, spoofing, or BSSID reuse; a BSSID
is not a guarantee of permanent physical identity.

## Statistics and compatibility status

STATS has **26 fields**:

```text
FBR1 id STATS scope session total_detections unique_aps observations_saved
observations_filtered gps_valid_observations gps_invalid_observations wifi_2g_aps
wifi_5g_aps open_aps secured_aps scan_cycles scan_errors uart_retransmissions
uart_dropped_frames storage_errors queue_overflows table_overflows queued committed
free_heap minimum_free_heap
```

Scope 0 is the current session; scope 1 is the current ESP boot. Event counters
are uint64. `observations_saved` means accepted into the ESP queue, not durable SD
storage. The companion's SD saved counter is based on successfully synchronized
journal records. Device committed describes ACK progress.

Compatibility STATUS has **24 fields**:

```text
FBR1 id STATUS boot running phase scan_id cycles observations acknowledged rows
no_gps truncated discarded fix_valid satellites hdop age_ms latitude longitude
altitude_m utc gps_ready wifi_ready
```

Its legacy counters are uint32 and boot-scoped. The companion uses STATS for new
64-bit statistics and STATUS for GPS/initialization/phase information. STATUS and
revision 2 STATS are emitted periodically by the firmware. Five seconds without
status makes link state unknown and triggers PING and explicit recovery.

## Events and reliable delivery

Events use request ID zero:

```text
READY version boot
SESSION_STARTED boot session
SESSION_STOPPED boot session queued_at_stop
SCAN_BEGIN boot scan_id
SCAN_DONE boot scan_id found processed truncated
STOPPED boot reason discarded_total
ERROR code [esp_error_number]
```

SCAN_DONE means batch processing, not durable storage. Neither STOPPED nor
SESSION_STOPPED means the transport queue is empty.

The ESP keeps one unacknowledged record in flight, with a 2000 ms ACK window and
up to three identical retransmissions. ACK_TIMEOUT pauses delivery while retaining
RAM. HELLO 2 / RESUME_LINK can restart delivery. An ACK is sent only after journal
write and sync. Its durable identity is (boot, session, sequence).

For recovery, scan and repair the journal first, then negotiate HELLO, query STATE,
and reconcile committed with RESUME_LINK. Do not deduplicate by BSSID or infer a
new-session baseline of zero. A different boot cannot restore lost ESP RAM and
must not automatically start a replacement recording session.

## Firmware 0.3.1 compatibility changes

Revision 2 framing and the OBSERVATION, STATUS, STATS, STATE, CONFIG, AP, and MEMORY
layouts remain unchanged. The companion's compatibility update includes:

- Accept version 0.3.1 during HELLO while retaining 0.3.0/0.2.0 support.
- Export the exact WiGLE metadata below, including **brand=Sapsan**.
- Retry bounded idempotent queries after baud changes; never blindly replay mutations.
- Preserve missing speed/course as unavailable rather than displaying measured zero.

```csv
WigleWifi-1.4,appRelease=0.3.1,model=Feberis Pro,release=0.3.1,device=ESP32-C5,display=Flipper Zero,board=Feberis Pro,brand=Sapsan
MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type
```

The firmware now accepts RESUME_LINK only for its already committed sequence or
an actually transmitted queue head. The companion continues to derive this value
from its durable journal; a rejected checkpoint leaves that journal intact.
The ESP's `rows` counter is based on actual prepared CSV, while the companion's
SD saved count remains its durable observation count, including no-GPS records.

SCAN_TIMEOUT can occur after PAUSE/STOP. If STATE still reports scanning=1, the
companion continues waiting and keeps the journal open. Only driver completion
(or explicit ESP restart and interrupted-session recovery) can unblock a new scan.
The NMEA parsing and strongest-location fixes are implemented in the firmware;
they do not require a new frame layout or reconstructing positions in the FAP.

## Continuous driving and metrics in 0.4.0

The companion sends `START_SCAN 0` by default. Optional explicit gap values are
0–60000 ms. Its driving settings show **Cycle gap** with Continuous (0 ms), 50,
100, or 250 ms. Longer gaps are in diagnostic settings. The argument is deliberate
delay after batch processing, not a start-to-start period. Radio sweeps still take
seconds at passive dwell 120 ms. SMART min_gap_ms and FULL full_gap_ms limit repeat
observations per BSSID rather than scan starts.

Legacy 0.2.0/0.3.x remains supported with its 1000 ms minimum and legacy gap presets.
INFO's capacities are authoritative; the 0.4.0 SDK results buffer defaults to 512
(build range 16–1024), so the companion does not assume the old 128-result limit.
Accepted observations remain subject to durable ACK even under queue pressure.

GET_SCAN_METRICS returns exactly 26 fields:

```text
FBR1 id SCAN_METRICS boot session requested_gap_ms dwell_ms capture_ready
radio_ms ingest_ms gap_ms max_gap_ms period_ms ingest_cpu_us
captured dropped malformed cache_overflows metadata_hits metadata_misses
queue_high_water backpressure_ms scan_buffer_bytes capture_cache_bytes
main_stack_free scan_capacity
```

Timing/size/high-water fields are uint32; ingest_cpu_us, capture counters,
metadata_hits/misses, and backpressure_ms are uint64. The reply is validated for
field count, numeric range, boot, and session. It is optional for basic operation.

- radio_ms and ingest_ms describe the last completed batch; ingest_ms includes
  waiting/scheduling while ingest_cpu_us accumulates actual slice wall time.
- gap_ms is scan completion to next start, including processing, requested delay,
  and queue pressure. period_ms is consecutive start-to-start time. Adjacent-cycle
  values may be mixed while scanning; the first cycle may report zeros. Pause
  resets the timing anchor.
- captured/dropped/malformed/cache_overflows count reception metadata and its
  queue/cache losses. They are separate from accepted-observation loss counters.
- metadata_hits/misses count SDK results with/without matched reception metadata.
- queue_high_water is the maximum accepted-observation queue awaiting durable ACK;
  backpressure_ms counts completed intervals spent with that queue full.
- scan_buffer_bytes and capture_cache_bytes are additional fixed PSRAM buffers.
  GET_MEMORY.storage_reserved retains its previous three-buffer meaning.
- main_stack_free is the ESP-IDF stack high-water value in bytes. Counts/high-water
  values reset with a new session.

RADIO_CAPTURE_INIT explicitly reports degraded capture. SDK scanning can continue,
but unmatched observations are unlocalized. The FAP shows the degraded state and
exports metrics as a separate object without combining capture loss and SD/queue
loss counters.

Current WiGLE metadata is exactly:

```csv
WigleWifi-1.4,appRelease=0.4.0,model=Feberis Pro,release=0.4.0,device=ESP32-C5,display=Flipper Zero,board=Feberis Pro,brand=Sapsan
MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type
```
