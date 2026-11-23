# User guide

## Navigation

The interface uses native Flipper controls and ASCII English labels.

| View or control | Behavior |
| --- | --- |
| Dashboard: Left | Open the application menu |
| Dashboard: OK | Perform the displayed Connect, Start, Stop, or Retry action |
| Dashboard: Right | Open Networks |
| Menu or network list: Up/Down, OK | Select and open an item |
| Settings: Left/Right | Change the selected setting |
| Text pages: Up/Down | Scroll the full text |
| Back on a subpage | Return to the previous level |
| Back on the dashboard | Exit; an active session requires Stop and exit confirmation |

The menu does not stop recording. Network and information pages are snapshots.
Reopen a page or use **Refresh list** to update it. Returning from network details
preserves list selection.

## Connect and record

The connection always uses the Flipper's USART. After Connect, the application sends
HELLO 2 and reads device information, configuration, memory diagnostics, and state.
It handles an existing journal before allowing a new session and waits for a
confirmed idle device.

When Start is pressed without a GPS fix, **Start without GPS?** asks whether to
record without position. **Cancel** or **Back** returns to the dashboard without
creating a session or sending START_SCAN. **Start** confirms this attempt only.
With a fix, Start proceeds directly. If a fix arrives while the dialog is open,
the message updates and still waits for your Start/Cancel decision.

Starting without a fix does not disable GPS for the session. The journal records
observations without coordinates until valid position data becomes available;
subsequent eligible observations use GPS normally. WiGLE still requires valid
position and UTC for each exported observation.

Start creates a unique session directory after confirmation, using the Flipper's
local clock and a collision suffix. GPS UTC is used only for observation timestamps. Existing
session directories are not overwritten.

Stop prevents additional scans, continues accepting queued observations, then
retrieves the final AP table and statistics. Large AP tables can take time to
archive; progress is shown on the dashboard. Wait for Ready before starting again.

## Settings

Radio and filtering settings come from **GET_CONFIG on the ESP**. They are not
replaced with local defaults on connection.

| Setting | Meaning and supported range |
| --- | --- |
| Cycle gap | Continuous (0 ms, default), 50, 100, or 250 ms of extra post-batch delay |
| Mode | SMART, FULL, or UNIQUE |
| Band | Both bands, 2.4 GHz, or 5 GHz |
| Active probing | OFF passive or explicitly enabled ON (probe) |
| Dwell ms | Channel dwell time, 20–1500 ms |
| Distance m | SMART distance threshold, 1–10000 m |
| RSSI delta dB | SMART signal-change threshold, 1–100 dB |
| Max gap ms | SMART per-BSSID maximum observation gap, 1000–3600000 ms, at least Min gap |
| Min gap ms | SMART per-BSSID minimum observation gap, 0–60000 ms |
| GPS max age ms | Maximum fix age, 100–10000 ms |
| GPS HDOP x10 | HDOP threshold multiplied by ten, 1–500 |
| FULL gap ms | FULL per-BSSID observation gap, 0–60000 ms |
| Baud | 115200, 460800, or 921600 |

**Cycle gap** is the extra delay after a scan batch has been processed, not a
scan period. Continuous does not add a deliberate delay. Passive dwell remains
120 ms in the default firmware configuration, and a full radio sweep still takes
seconds as one radio visits channels sequentially. SMART/FULL per-BSSID gaps do
not set the radio scan cadence.

**Settings → Scan diagnostics** contains longer diagnostic cycle gaps
(1, 2, 5, 10, or 60 seconds; 0 ms resets Continuous) and **Read metrics**.
The metrics page shows measured radio time, ingestion wall/CPU time, actual gap,
start-to-start period, queue pressure, scan capacity, capture readiness, and memory
buffers. Capture-queue drops/cache overflows describe missing reception metadata;
accepted-observation losses remain separate counters. Reopen to refresh.

Older firmware 0.2.0/0.3.x retains its 1000 ms minimum: connected legacy devices
show 1/2/5/10 s presets. Old saved Interval presets migrate to Continuous for 0.4.0;
baud and the active-session recovery path are retained. Newly chosen cycle gaps
are persisted in milliseconds.

Numeric controls increment or decrement the value returned by the device. The
application validates the complete configuration before submitting an update.

During a session, configuration changes follow PAUSE → wait for batch completion
and queue drain → SET_CONFIG → GET_CONFIG → RESUME. An already paused session
stays paused. A mode change preserves the current AP table and recording baseline.

Baud changes require a stopped, drained session. The application receives the full
OK at the old speed, switches UART speed, and negotiates HELLO 2 again. Recovery
retries HELLO at the current speed before trying alternatives. Read-only queries
and PING also tolerate up to two retries; SET_CONFIG is not replayed. The firmware
0.3.1 PC-receiver tests covered all three baud rates, but physical FAP verification
remains pending.
USART ownership is retained until the application exits.

BOTH is a schedule that scans both bands; it does not indicate simultaneous radios
or the instantaneous channel being scanned.

## Networks

Networks fetches the device AP table through GET_AP, 24 slots per page. Use
**Previous page**, **Next page**, and **Refresh list** to navigate without loading
the entire table into Flipper RAM. Before a table snapshot is available, a bounded
recent-observation preview may be shown.

Opening a table entry requests its current aggregate: SSID and original SSID hex,
BSSID, band/channel/frequency, security, PHY mask, last/strongest/mean RSSI,
detection and observation counts, metadata changes, first/last timestamps, and
last/strongest measurement positions. Missing fixes are shown without coordinates.

Observations continue to be journaled while browsing. A list snapshot from an old
session does not query slots belonging to a new session.

## GPS and diagnostics

The GPS page shows fix validity, UTC, latitude/longitude/altitude, satellites,
HDOP, age, speed, course, and horizontal accuracy when available. Reopen to refresh.
HDOP is not a distance measurement and is never converted to meters. The current
RMC/GGA firmware does not provide a measured horizontal accuracy. Missing speed
or course appears as unavailable; a reported numeric zero remains a real zero.

In firmware 0.4.0, a verified observation pairs the strongest captured beacon or
probe response with its software reception-callback timestamp and the GNSS
snapshot available at or before reception. Receiver latency, GPS sample rate,
and position uncertainty still apply; this is not the router's location.

An SDK result without matching reception metadata carries **RX_UNVERIFIED**. It
is journaled and acknowledged after sync, retains its monotonic fallback timestamp,
has no coordinates, and is excluded from WiGLE. A later fix never fills its missing
position. Earlier firmware journals retain their legacy scan-end timing semantics.
A real fix at 0,0 is valid; missing fixes are never replaced with 0,0.

Session status includes full 64-bit statistics, GPS age, recording mode, elapsed
time, truncated/discarded counts, and queue/AP utilization. Memory diagnostics
separates SRAM from PSRAM and shows initialization, storage readiness, free/minimum
free/largest block sizes, reserved buffers, record size, and actual capacities.
An unavailable AP store blocks Start. Firmware 0.2.0 reports that extended memory
diagnostics are unavailable and uses the capacities reported by GET_INFO.

## Session files and exports

After stopping, open **Sessions** to browse SD directories in pages of 16. Select
a directory to read its summary, then use **Export selected session** to recover
its journal and regenerate exports. Archived-session browsing and export require
recording to be stopped. **Session files** shows the selected path and summary.

| File | Contents |
| --- | --- |
| `events.journal` | Original CRC-protected observations and session/control records; retained for recovery |
| `observations.csv` | Every unique durably accepted observation, including observations without GPS |
| `access_points.csv` | Final per-BSSID aggregates fetched after Stop and drain |
| `wigle.csv` | Eligible observations with valid GPS and UTC |
| `metadata.json` | Firmware/configuration, identity, completion state, timing, and loss counters |
| `stats.json` | Device session statistics with integer precision |

Without a complete final AP-table fetch, metadata reports
`aggregates_complete=false` and `access_points.csv` contains only its header.
Filtered detections cannot be reconstructed from observation records alone.

WiGLE export uses two headers with version 0.4.0 and `brand=Sapsan`, 11 columns,
LF, and no BOM. It leaves unavailable
altitude and accuracy empty, skips UNKNOWN authentication, quotes SSIDs, doubles
embedded quotes, and escapes non-text bytes and backslashes as `\xNN`. It does not
substitute a later GPS fix or the AP's strongest measurement position. Actual
WiGLE importer acceptance of empty accuracy remains to be verified.

Preferences are saved in `/ext/apps_data/wardriving/config/settings.conf`.
Errors are logged under `/ext/apps_data/wardriving/logs/errors.log`, with bounded
log growth. Earlier application CSVs remain in `/ext/apps_data/feberis/`.

## Recovery and troubleshooting

| Situation | Action and behavior |
| --- | --- |
| USART busy | Close other applications using the USART interface |
| No response | Check power, TX/RX/GND, USART wiring, and baud; Retry probes the supported speeds |
| RADIO_CAPTURE_INIT | Capture is degraded; SDK scanning and durable journaling continue with unlocalized results. Open Scan diagnostics for separate capture counters. |
| No GPS fix | Recording and durable ACKs continue; observations have empty coordinates and are excluded from WiGLE |
| SD error | Repair/reinsert/free space on the card, then Retry or explicitly Recover and resume |
| Link unknown | No status for five seconds freezes recording and sends PING; recover before continuing |
| ACK timeout | Retry recovers the journal and resumes delivery of retained ESP records |
| Device session has no SD journal | Restore the matching journal/card; the application does not acknowledge an unrelated queue |
| Journal checkpoint mismatch | Preserve the journal and inspect the boot/session/sequence; do not start over the retained data |
| ESP restarted | The old session is interrupted; RAM data lost from the ESP cannot be recovered; explicitly start a new session |
| Scan timeout | If the driver still reports scanning, wait for completion or restart the ESP; a new session stays blocked |
| AP table full | The application stops, drains, and archives the session; a new session can then be started |
| Export failed | Keep `events.journal`; select the session and export again after resolving the SD problem |

**Retry** recovers the journal, reconciles its committed sequence with the ESP,
and stops/drains/archives the existing session. **Recover and resume** explicitly
requests RESUME_SCAN after successful journal and link recovery, provided the
same session is still running. Neither action blindly repeats START.

Exit attempts to stop and drain for up to 3.5 seconds. If full archival cannot
finish, the journal and active-session pointer remain for recovery. A disconnected
ESP cannot confirm that it stopped. See the [hardware checklist](../tests/HARDWARE.md)
for the required physical-device checks.
