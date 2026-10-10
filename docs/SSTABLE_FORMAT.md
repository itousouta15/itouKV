# SSTable and manifest v1

This is a two-tier design: a mutable MemTable (with WAL) and **one** immutable
base table. Manual compaction merges the two streams into a replacement base
table. It is intentionally not a multi-level LSM or a background compactor.

All integers are unsigned little-endian. All checksums use the same CRC32 as
the WAL. Tables contain only live values; tombstones are discarded after merging
against the complete base table, so no older level can resurrect a deleted key.

## Files and generations

```text
LOCK            OS-released exclusive lock; never remove while the DB is open
data.wal        Initial generation's WAL (before any manifest exists)
CURRENT         32-byte pointer to the committed generation and checkpoint
CURRENT.next    Uncommitted manifest candidate, ignored on recovery
TABLE-N.sst     Immutable table at generation N
WAL-N.log       Changes newer than that table's checkpoint
```

The manifest, not file age or the largest filename number, selects live files.
If a referenced file is missing, open fails; it is never silently recreated.
Unreferenced candidates/old files are ignored. Reserved filenames belong to
the engine; the directory should not contain unrelated files with these names.

## CURRENT

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 8 | `IKVCUR01` |
| 8 | 8 | Positive generation number N |
| 16 | 8 | Checkpoint sequence |
| 24 | 4 | Reserved zero |
| 28 | 4 | CRC32 of bytes 0..27 |

The file must be exactly 32 bytes. The referenced SSTable checkpoint must match.
WAL sequence numbers resume at checkpoint + 1.

## TABLE-N.sst header

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 8 | `IKVSST01` |
| 8 | 8 | Checkpoint sequence |
| 16 | 8 | Live row count |
| 24 | 8 | Sparse index offset (end of row data) |
| 32 | 8 | Bloom filter offset (end of index) |
| 40 | 8 | Bloom filter byte length |
| 48 | 8 | Sparse index entry count |
| 56 | 8 | Total key + value bytes (excludes row overhead) |
| 64 | 12 | Reserved zeros |
| 76 | 4 | CRC32 of bytes 0..75 |

Rows start at byte 80. Each row is:

```text
key_length:u32 | value_length:u32 | key | value | row_crc32:u32
```

The row checksum covers its lengths, key and value. Key/value bounds match the
WAL. Rows are strictly increasing under unsigned-byte lexicographic order.

The sparse index contains every 16th row, including row zero. Each entry is:

```text
key_length:u32 | key | row_offset:u64
```

The Bloom filter follows the index. A final `u32` is the CRC32 of **every byte
before it**, including header, rows, index and Bloom bits. Recovery streams the
file through a 64 KiB buffer to validate this checksum before loading the index
and Bloom filter. This avoids loading all values into RAM, but still performs
O(table bytes) startup I/O. Every fetched row is also independently checksummed.

## Bloom filter

Bits: `ceil(max(64, live_rows * 10) / 8)` bytes. Seven probes, with little-endian
bit numbering within each byte. For a key's raw bytes:

1. `h1` is 64-bit FNV-1a (offset 14695981039346656037, prime 1099511628211).
2. Start `h2 = h1 XOR 0x9e3779b97f4a7c15`.
3. `h2 = (h2 XOR (h2 >> 30)) * 0xbf58476d1ce4e5b9`.
4. `h2 = (h2 XOR (h2 >> 27)) * 0x94d049bb133111eb`.
5. `h2 = (h2 XOR (h2 >> 31)) OR 1`.
6. Probe `(h1 + i * h2) mod bit_count`, for i = 0..6.

All arithmetic wraps modulo 2^64 **before** reduction modulo bit_count. Empty
tables have eight zero bytes. Empty keys are legal and use the same hash rules.
The full-file checksum prevents a damaged filter from silently causing false
negatives at open. As elsewhere, files are assumed not to change outside the
engine after validation.

## Publication and failure matrix

The Store mutex and process lock remain held throughout compaction.

| Last completed stage | Recovery source |
| --- | --- |
| New SSTable written and synced | Old CURRENT / initial data.wal |
| New empty WAL synced, directory synced | Old generation |
| CURRENT.next synced | Old generation; temporary manifest ignored |
| CURRENT atomically replaced and directory synced | New generation |
| Old handles closed and old files removed | New generation |

`rename` on POSIX and `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)` on Windows
replace the manifest in the same directory. Referenced files are complete and
flushed before this happens. Old files are not removed until after publication
and directory sync. If publication throws, the Store is poisoned; reopening
chooses whichever complete manifest is present.

The tests interrupt the process at the explicit stage boundaries. They also
inject exceptions and verify the poisoned-instance contract. They do not
simulate every filesystem journal state or power-loss behavior. The flush and
Windows namespace durability qualifications in README.md apply here too.

Compaction reads at most one SSTable row at a time, plus its output buffer;
index/Bloom structures and the MemTable stay in memory. Point reads search the
MemTable, then Bloom, then binary-search the sparse index and scan a short run
of rows. Range scans stream-merge both sorted sources, choosing the MemTable
version when keys match. Compaction uses the same merge iterator, with no limit.
