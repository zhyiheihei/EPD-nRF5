# EPD Dashboard BLE Protocol v1

Status: protocol design; firmware handlers are not implemented yet.

## 1. Goals

This protocol updates the 800x480 dashboard without replacing the existing EPD protocol.

- The device renders the calendar, borders, dates, countdowns, and food/drink icons.
- The host renders arbitrary schedule titles and food names to monochrome 1bpp bitmaps.
- At most 2 schedules and 4 food records are displayed.
- A transaction is committed atomically: incomplete transfers are never refreshed.
- Existing commands `0x00-0x30` and `0x90-0x99` retain their current meaning.

The new commands use the existing service and characteristic:

```text
Service:        62750001-d828-918d-fb46-b6c11c675aec
Characteristic: 62750002-d828-918d-fb46-b6c11c675aec
```

The characteristic continues to support Write, Write Without Response, and Notification.

## 2. Compatibility

New command IDs occupy an unused range:

| ID | Name |
|---:|---|
| `0x40` | DASH_CAPS |
| `0x41` | DASH_BEGIN |
| `0x42` | DASH_BITMAP |
| `0x43` | DASH_COMMIT |
| `0x44` | DASH_ABORT |
| `0xC0` | DASH_RESPONSE notification |

Old firmware ignores these commands. New firmware keeps all legacy behavior unchanged. A host must send `DASH_CAPS` first and fall back to the legacy full-image protocol if it receives no valid response.

## 3. Encoding conventions

- Multi-byte integers are unsigned big-endian unless explicitly marked signed.
- Unix timestamps are UTC seconds.
- Timezone offset is a signed 16-bit number of minutes, for example China Standard Time is `+480` (`0x01E0`).
- Bitmap scanlines are byte padded. The leftmost pixel is bit 7 of the first byte.
- A set bit is black; an unset bit is transparent/white.
- Bitmap CRC is CRC-16/CCITT-FALSE: polynomial `0x1021`, initial value `0xFFFF`, no reflection, xor-out `0x0000`.
- Transaction ID is chosen by the host and must be non-zero. It may wrap from `255` to `1`.

## 4. Response notification

Every command error and every transaction boundary is reported through a binary notification:

```text
Offset  Size  Field
0       1     0xC0
1       1     protocol version (0x01)
2       1     transaction ID; 0 for DASH_CAPS
3       1     request command (0x40-0x44)
4       1     status
5       N     command-specific payload
```

Status values:

| Value | Meaning |
|---:|---|
| `00` | OK |
| `01` | unsupported protocol version |
| `02` | invalid packet length |
| `03` | command invalid in current state |
| `04` | transaction ID mismatch |
| `05` | invalid schedule/food/asset slot |
| `06` | invalid bitmap dimensions or offset |
| `07` | bitmap CRC mismatch |
| `08` | insufficient memory |
| `09` | device/display busy |
| `0A` | unsupported feature |
| `0B` | transaction timeout |

## 5. Capability discovery (`0x40`)

Request:

```text
40 01
```

Response payload after the common response header:

```text
Offset  Size  Field
5       1     application version
6       1     highest dashboard protocol version
7       1     maximum schedules (2)
8       1     maximum foods (4)
9       2     maximum bitmap bytes (1024)
11      2     feature flags
13      2     current ATT payload bytes
15      2     display width
17      2     display height
```

Feature flags:

```text
bit 0: structured metadata
bit 1: 1bpp bitmap assets
bit 2: CRC-16 validation
bit 3: transaction commit
bit 4: local food/drink icons
```

## 6. Begin transaction (`0x41`)

`DASH_BEGIN` transfers all structured metadata. It does not refresh the panel.

```text
Offset  Size  Field
0       1     0x41
1       1     protocol version (0x01)
2       1     transaction ID
3       1     flags; reserved, send 0
4       4     current UTC timestamp
8       2     timezone offset in signed minutes
10      1     week start (0=Sunday ... 6=Saturday)
11      1     schedule count (0-2)
12      1     food count (0-4)
13      N     schedule records followed by food records
```

Schedule record, 10 bytes:

```text
Offset  Size  Field
0       1     slot (0-1)
1       1     flags: bit0=all-day
2       4     start UTC timestamp
6       4     end UTC timestamp
```

Food record, 6 bytes:

```text
Offset  Size  Field
0       1     slot (0-3)
1       1     type: 0=food, 1=drink
2       4     expiry UTC timestamp
```

Expected total request length:

```text
13 + schedule_count * 10 + food_count * 6
```

On success the device prepares the dashboard base layer and responds `OK`. The host must wait for this response before sending bitmap data.

## 7. Bitmap assets (`0x42`)

Asset IDs:

| ID | Contents |
|---:|---|
| `00`, `01` | schedule title 0/1 |
| `10`-`13` | food name 0-3 |

Packet header:

```text
Offset  Size  Field
0       1     0x42
1       1     protocol version (0x01)
2       1     transaction ID
3       1     asset ID
4       1     flags
5       2     bitmap width in pixels
7       1     bitmap height in pixels
8       2     total bitmap byte length
10      2     chunk offset
12      N     chunk bytes
12+N    2     CRC-16, present only when END is set
```

Flags:

```text
bit 0 (0x01): BEGIN; offset must be zero and dimensions are accepted
bit 1 (0x02): END; CRC follows the chunk data
bit 2 (0x04): INVERT bitmap bits
```

Constraints:

- `total_length == ceil(width / 8) * height`
- total length must not exceed 1024 bytes
- chunks must arrive in increasing, non-overlapping offset order
- schedule title recommendation: maximum `320x20` (800 bytes)
- food name recommendation: maximum `192x20` (480 bytes)
- pixels outside the asset's assigned rectangle are never writable through this command

Intermediate chunks may use Write Without Response. The BEGIN packet, every eighth packet, and the END packet should use Write With Response. The device sends a dashboard notification for END success or failure.

## 8. Commit (`0x43`)

```text
Offset  Size  Field
0       1     0x43
1       1     protocol version (0x01)
2       1     transaction ID
3       1     flags
```

Flags:

```text
bit 0 (0x01): refresh display
bit 1 (0x02): put display driver to sleep after refresh
```

Normal value is `0x03`. The device validates transaction state, writes all completed assets, refreshes once, sleeps the EPD, clears transaction RAM, and responds when the operation has completed.

## 9. Abort (`0x44`)

```text
44 01 <transaction_id>
```

The device discards transaction RAM without refreshing. Disconnect and a 30-second inactivity timeout have the same effect.

## 10. Example transaction

```text
# Discover v1 support
40 01

# Begin tx=0x2A, UTC=0x67800000, timezone=+480,
# week starts Monday, 2 schedules, 4 foods
41 01 2A 00 67 80 00 00 01 E0 01 02 04 ...records...

# Schedule title 0, single 320x20 packet sequence
42 01 2A 00 01 01 40 14 03 20 00 00 ...first chunk...
42 01 2A 00 02 01 40 14 03 20 00 F0 ...last chunk... CRC16

# Other title/name assets follow

# Refresh and sleep
43 01 2A 03
```

## 11. Host behavior

1. Connect and enable notifications.
2. Run legacy `INIT (0x01)` so the device reports MTU and current configuration.
3. Send `DASH_CAPS` and verify protocol version, dimensions, and feature flags.
4. Sort CalDAV events and send only the next two.
5. Sort food records by expiry and send only the next four.
6. Send `DASH_BEGIN` and wait for `OK`.
7. Render titles/names using a platform font to 1bpp assets.
8. Send each asset in MTU-sized chunks and verify the END response.
9. Send `DASH_COMMIT` with flags `0x03`.
10. On error or disconnect, abort and restart with a new transaction ID.

## 12. Security note

The existing characteristic has open read/write permissions. Protocol v1 adds integrity checking, not authentication or encryption. Do not send CalDAV credentials to the display. The host should send only already-filtered event metadata and rendered title bitmaps.
