# Architecture and storage

## Source layout

| File | Responsibility |
| --- | --- |
| `application.fam` | FAP manifest and source list |
| `feberis.c` | UART worker, command/state handling, session lifecycle, configuration, and recovery |
| `protocol.c` / `protocol.h` | Framing, CRC, bounded parsers, numeric validation, and CSV generation |
| `session_store.h` | Internal storage implementation: journal, recovery, summaries, and streaming export |
| `ui.inc` | Native views, navigation, action dispatch, and UI snapshots |
| `ui_model.c` / `ui_model.h` | Shared model, UI state/action rules, count formatting, and compact dashboard snapshot |
| `dashboard.c` / `dashboard.h` | Dashboard drawing |
| `tests/` | Host platform adapter, protocol/session/UI tests, export validation, and optional renderer |

The storage implementation and native UI are included by `feberis.c`; they are
not independent compilation units.

## Concurrency and bounded memory

The connection always acquires `FuriHalSerialIdUsart`; the port is fixed in code.
The UART callback copies received bytes into a **4096-byte stream buffer** and
reports UART errors. It does not parse frames, write SD, or update native views.
The worker parses input, validates records, executes commands, writes the journal,
and processes GUI action requests. Its stack is 8192 bytes; the application entry
stack is 4096 bytes.

The GUI takes a nonblocking protected snapshot of the backend. If SD operations
hold the backend mutex, the previous valid screen remains visible and navigation
continues. The locking dashboard drawing model contains only dashboard fields,
not the full AP list or session details.

The application keeps at most 24 network entries per snapshot and 16 session
names per directory page. The ESP's full AP table is retrieved and exported as a
stream rather than allocated on the Flipper. Parser buffers are bounded to 1536
bytes and 48 fields. Journal seek offsets stay within the SDK's 32-bit range,
limiting a journal to below 4 GiB.

Long journal scans run before HELLO, and long exports run after queue drain or
while browsing stopped sessions. During those operations, UART interrupts consume
periodic telemetry without filling the RX buffer. Recovery requests retained
observations again through HELLO/RESUME_LINK. The live state is rechecked after
final session export.

## Command and session lifecycle

Commands use nonzero request IDs and one tracked request at a time. Replies must
match the pending request; asynchronous events use ID zero. START, STOP, and
SET_CONFIG are not blindly replayed after timeout because the ESP does not cache
responses by request ID. The application queries state before resolving an
uncertain operation. HELLO, PING, and read-only queries are retried at most twice
with the same ID before failing or, for HELLO, probing another baud rate. Retrying
the current speed first handles the bridge losing an initial reply after a baud
switch. Shutdown skips query retries so they cannot delay its STOP attempt.

Connection reads HELLO 2, INFO, CONFIG, MEMORY, and STATE. GET_MEMORY's
UNKNOWN_COMMAND response is accepted only for firmware 0.2.0, which falls back to
GET_INFO capacities. Firmware 0.3.0/0.3.1/0.4.0 require usable, matching memory limits before
Start is enabled.

A Start request is checked against the worker's current fix. Without a fix, it
sets a transient confirmation request for the GUI and does not create a file or
send START_SCAN. Cancel clears the request; confirmation is consumed once and
rechecks connection/session readiness. Errors, reconnect, and exit invalidate it.
This allows a stale GUI snapshot to prompt rather than silently start without GPS,
and prevents an old confirmation from toggling an already running session.

A new session directory and journal are opened before START_SCAN, after any required
no-GPS confirmation. The journal
records the global sequence baseline from STATE and a START_INTENT for the next
session number. SESSION_STARTED binds the live session. This allows recovery when
an application restart occurs between sending START and receiving its response.

STOPPED does not mean all observations have been delivered. Finalization waits
for STATE with `running=0`, `scanning=0`, and `queued=0`, plus legacy STATUS with
`phase=IDLE`. It then retrieves session STATS, exactly `unique_aps` dense GET_AP
slots, and final STATS. Archival progress is visible in the model.

## Durable acknowledgement invariant

A durable observation is identified by **(boot, session, sequence)**.

1. Validate framing, CRC, field count, identity, ranges, MAC, SSID hex, and optional values.
2. Append the original complete frame, including its CRC and LF, to `events.journal`.
3. Check the full byte count and `storage_file_sync` result.
4. Advance the in-memory committed sequence and send `ACK boot session sequence`.

Observations without a fix follow the same durable path. BSSID is not a
deduplication key: multiple observations of the same AP are valid data.
Retransmission of the last committed sequence receives another ACK without
another append. Older sequences and records from a mismatched boot/session are
not acknowledged.

A short write or failed sync does not advance committed. The application tries
to seek/truncate back to the previous record boundary, blocks further ACKs, and
sends NACK STORAGE. Failure to repair the tail marks the journal as damaged.

## Journal format and recovery

`events.journal` contains LF-terminated FBR1 frames. It preserves received INFO,
CONFIG, initial STATE, OBSERVATION, session identity, STOPPED/status, final STATS,
and AP aggregate records. Local CRC-framed records describe START_INTENT,
ARCHIVE_BEGIN, ARCHIVE_END, FINISH_LOCAL, and FINISH.

A recovery scan uses a buffered reader to:

- Verify CRC, record schema, boot/session identity, and increasing observation sequences.
- Recover the committed sequence from the last durable observation or initial STATE baseline.
- Validate ordered aggregate slots and complete archive markers.
- Truncate an incomplete or invalid tail to the last valid boundary.

Only then does link recovery compare the journal checkpoint with GET_STATUS and
send RESUME_LINK. The checkpoint is not reconstructed from a separate counter
file. It may be nonzero even before the first observation of a new session because
transport sequence numbering spans sessions within one ESP boot.

An ESP boot change closes the previous session as interrupted. Automatic START
is never used to hide a restart. Missing final aggregates remain explicitly
incomplete because filtered detections are not present in the observation stream.

## Preferences and the active-session pointer

`config/settings.conf` is an append-only sequence of CRC-protected SETTINGS2
records containing a reserved legacy port field, baud, cycle gap in milliseconds,
and the active journal path. USART is fixed; the port field is always written as
zero. The reader also accepts legacy SETTINGS records: it preserves baud and the
recovery path while migrating the former interval index to Continuous (0 ms).
This avoids accidentally retaining an old default 2000 ms delay in driving mode.
For legacy firmware, the effective START argument is clamped to its 1000 ms minimum.
A torn update leaves the previous valid record readable; the next write removes
the invalid tail before appending. The path locates a journal, while committed is
always recovered from that journal.

This avoids treating a separately replaced pointer file as an atomic checkpoint.
[SDK 1.4.3 implements `storage_common_rename` through copy and remove](https://github.com/flipperdevices/flipperzero-firmware/blob/1.4.3/applications/services/storage/storage_external_api.c#L449),
so replacing a single preferences file with rename would not provide that guarantee.

## Export publication and schema

Exports are regenerated from the journal with bounded buffers. Each output is
written to a temporary file and synchronized, then published under its final name.
The application checks destination size and explicitly synchronizes the destination.
Metadata is published last. This is **not an atomic transaction across the session
directory**; the retained journal is the recovery source if copying or publication
is interrupted.

- `observations.csv`: unique accepted observations, original SSID hex, sequence,
  reason mask, monotonic time, optional UTC, and optional position/motion fields.
- `access_points.csv`: final GET_AP aggregates, one row per AP slot/BSSID. Without
  a complete final table, only the header is exported.
- `wigle.csv`: generated from validated observation fields rather than trusting
  `csv_hex`; only valid fix + UTC + finite WGS84 coordinates are eligible.
- `stats.json`: named session statistics serialized directly as integers; optional
  capture/scan metrics are a separate nested `scan_metrics` object.
- `metadata.json`: schema/protocol versions, firmware, configuration array, boot,
  session, `journal_committed`, record count, actual capacities, interruption and
  aggregate-completion flags, first/last observation UTC, local finish time,
  termination reason, loss/statistics counters, RX_UNVERIFIED record count, and
  optional capture/scan metrics when available. `position_timestamp` preserves
  `scan_end` for older firmware and reports
  `reception_callback_or_scan_end_unverified` for 0.4.0. The per-record reason mask
  identifies unverified fallback timestamps.

The metadata's `journal_committed` represents durable local data. The `committed`
statistic is the ESP's reported acknowledgement state. Local RTC names and local
finish time are never presented as GPS UTC.

## Continuous acquisition and provenance in 0.4.0

The default START argument is zero extra post-batch delay. Local driving presets
are 0/50/100/250 ms; long gaps are confined to diagnostic settings. The ESP handles
its scan slices, capture cache, and queue backpressure independently. The FAP
continues to ACK only synchronized observations, including unverified ones.

Reason masks are parsed as uint32 so future bits do not reject otherwise supported
records. RX_UNVERIFIED is a known provenance flag: its record must have no valid
position and no prepared CSV. It never receives dashboard GPS coordinates and
never enters WiGLE export. Original SSID bytes, including embedded NUL, remain
in the raw journal and SSID hex exports.

GET_SCAN_METRICS is an optional, bounded query. It runs during 0.4.0 connection,
on demand in diagnostics, and before final export. Unsupported/timeout replies
fall back without requiring the extension for basic recording. Accepted metric
snapshots are journaled when a session is open, recovered like other CRC frames,
and exported separately from observation-loss statistics. RADIO_CAPTURE_INIT sets
a persistent degraded-capture indicator without stopping SDK scanning.

## Verification

Run `./tests/run.sh`. C tests use AddressSanitizer and UndefinedBehaviorSanitizer.
They cover framing/CRC, parser bounds, revision 2 negotiation, uint64 counters,
durable ACKs and retransmission, SD faults, journal recovery, queue drain, exports,
configuration changes, baud handling, session pagination, and UI state rules.
`tests/check_exports.py` uses independent Python CSV/JSON parsers to validate the
files generated by the C implementation, including duplicate-key rejection.

The host adapter uses temporary POSIX files and simulated UART, clock, and
storage failures. These tests do not measure physical SD latency, native GUI
behavior, serial signal integrity, or a real WiGLE import. Those checks are tracked
in [HARDWARE.md](../tests/HARDWARE.md).
