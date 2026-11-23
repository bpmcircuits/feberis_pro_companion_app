#!/usr/bin/env python3
"""Validate actual C exporter output using Python's independent CSV/JSON parsers."""
import csv
import json
import sys
from pathlib import Path


def unique_object(items):
    result = {}
    for key, value in items:
        assert key not in result, f'duplicate JSON key: {key}'
        result[key] = value
    return result


complete = incomplete = 0
for directory in (Path(sys.argv[1]) / 'apps_data/wardriving/sessions').glob('session_*'):
    metadata_path = directory / 'metadata.json'
    if not metadata_path.exists():
        continue
    meta = json.loads(metadata_path.read_text(), object_pairs_hook=unique_object)
    stats = json.loads((directory / 'stats.json').read_text(), object_pairs_hook=unique_object)
    complete += bool(meta['aggregates_complete'])
    incomplete += not meta['aggregates_complete']
    raw = (directory / 'wigle.csv').read_bytes()
    assert not raw.startswith(b'\xef\xbb\xbf') and b'\r' not in raw
    rows = list(csv.reader(raw.decode().splitlines()))
    assert rows[0] == [
        'WigleWifi-1.4', 'appRelease=0.4.0', 'model=Feberis Pro', 'release=0.4.0',
        'device=ESP32-C5', 'display=Flipper Zero', 'board=Feberis Pro', 'brand=Sapsan',
    ]
    assert all(len(row) == 11 for row in rows[1:])
    for row in rows[2:]:
        assert row[1] == 'A",\\x5C\\x00B'
        assert row[6:10] == ['0', '0', '', '']
        assert row[10] == 'WIFI'
    with (directory / 'observations.csv').open(newline='') as source:
        observations = list(csv.DictReader(source))
    assert meta['rx_unverified_observations'] == sum(bool(int(row['reason_mask']) & 128) for row in observations)
    if meta['firmware'] == '0.4.0':
        assert meta['position_timestamp'] == 'reception_callback_or_scan_end_unverified'
        assert len(rows[2:]) == 1 and len(observations) == 2
        assert meta['scan_metrics']['dropped'] == 3
        assert meta['scan_metrics']['captured'] == 18446744073709551615
        assert stats['uart_dropped_frames'] == 0
    else:
        assert meta['position_timestamp'] == 'scan_end'
    identities = [(row['boot'], row['session'], row['sequence']) for row in observations]
    assert len(identities) == len(set(identities)) == meta['journal_records']
    for row in observations:
        assert row['ssid_hex'] == '41222C5C0042'
        assert row['monotonic_ms'] == '1234567890123'
        assert row['speed_kmh'] == row['course_deg'] == ''
        if row['gps_valid'] == '0':
            assert row['latitude'] == row['longitude'] == row['altitude_m'] == ''
    with (directory / 'access_points.csv').open(newline='') as source:
        aps = list(csv.DictReader(source))
    if meta['aggregates_complete']:
        assert len(aps) == 1 and aps[0]['detection_count'] == '10'
        assert stats['total_detections'] == 18446744073709551615
    else:
        assert not aps
assert complete and incomplete
print('exports: independent CSV/JSON parsing, 11 WiGLE columns, binary SSID, no-GPS, uint64 OK')
