# WAL v1

This document describes the original single-operation records, still emitted
by put/delete in 0.3. The decoder also accepts interleaved `IKV2` atomic-batch
records; see [WRITE_BATCH.md](WRITE_BATCH.md). Sequence now consistently means
one commit (one single operation or one complete batch).

`data.wal` (generation zero) or `WAL-N.log` (after compaction) is a sequence of records. An empty WAL contains no mutations since its checkpoint. All
integers are unsigned little-endian; strings are raw bytes. No native structs
are serialized. Sequence numbers begin at 1 for generation zero, or at the
manifest's checkpoint sequence + 1 for later generations, and must be contiguous.

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 4 | ASCII `IKV1` (magic and format version) |
| 4 | 4 | Payload length = key length + value length |
| 8 | 8 | Sequence number |
| 16 | 4 | Key length, at most 65,536 |
| 20 | 4 | Value length, at most 16,777,216 |
| 24 | 1 | Operation: 1 = put, 2 = delete |
| 25 | 3 | Reserved, must be zero |
| 28 | 4 | CRC32 of bytes 0..27 |
| 32 | key length | Key |
| next | value length | Value (delete requires length zero) |
| last | 4 | CRC32 of the complete header and payload |

CRC32 uses the reflected IEEE polynomial `0xedb88320`, initial value
`0xffffffff` and final XOR `0xffffffff`. It detects accidental corruption,
not malicious modification. Header checksum is validated **before** trusting
length fields or allocating payload storage.

## Recovery

1. Acquire the directory's OS file lock. If CURRENT exists, validate the manifest
   and referenced SSTable; begin with its checkpoint sequence and live state.
2. Scan from offset zero, validating each full header.
3. Validate bounded lengths, operation, reserved fields and next sequence.
4. If the final record lacks its declared body/checksum, stop before that record.
5. Validate complete-record CRC and apply a value or tombstone to the MemTable.
6. If an incomplete tail was found, truncate to the last valid boundary and
   durably flush the truncation before accepting new mutations.

No magic-byte scanning or corruption resynchronization is attempted. Complete
corrupt records cause an error without truncation. A short final header cannot
be distinguished from arbitrary short trailing garbage; it is always treated
as an incomplete tail. External deletion of a complete suffix is likewise not
detectable without an independently durable per-commit high-water mark, which
this format does not have (CURRENT only records a compaction checkpoint).
The recovery contract covers our append-only writer, not arbitrary file
tampering or storage devices violating flush.

## Concurrency and failure

Every API call takes one mutex. A mutation appends under that mutex, flushes,
then publishes to the map before releasing the mutex. Reads return owned copies.
An exclusive OS lock protects the entire directory for the Store's lifetime.
Reads consult the MemTable first and then the immutable SSTable. Compaction
publishes a new table/WAL pair through CURRENT; see SSTABLE_FORMAT.md.

A failed mutation may already have reached durable storage, so callers must
not assume exceptions imply rollback. The instance is poisoned to prevent a
later write from appending beyond an incomplete record or a read from observing
stale state. Reopen resolves the outcome by replaying the WAL.

The mutex gives single-Store operations a linear order. This is not a
multi-key transaction protocol or a multi-process reader/writer architecture.
