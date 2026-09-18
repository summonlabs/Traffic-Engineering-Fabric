# Persistence and recovery

## What is persisted

Only state that belongs to this runtime:

* committed traffic-engineering plans, including their full authority vector, allocation, feasibility
  result and explanation digest;
* supersession lineage (each plan record carries @supersedes@ and @superseded_by@);
* durable plan provenance and audit records;
* stable configuration this runtime owns: policies and objective profiles;
* fencing records;
* coordinator epoch state.

## What is never persisted

Dynamic evidence and liveness are owned by adjacent systems and are **not** restored by this
runtime:

* capacity snapshots, resource generations and link-state evidence;
* reservation snapshots;
* path-authority results and candidate sets;
* traffic-demand sets;
* publisher registrations, leases, sessions and worker authority;
* telemetry freshness of any kind.

A plan restored from durable storage is a *historical fact*, not live authority. It is reopened with
applicability @REVALIDATION_REQUIRED@ and can only become @CURRENT@ after the exact authority it was
bound to is submitted again and @Engine::revalidate@ confirms it field by field.

## On-disk layout

```
<directory>/manifest.tefm     magic + durable format version; created on first open
<directory>/snapshot.tefs     full rewrite of the durable record log
<directory>/journal.tefj      append-only record log after the snapshot
<directory>/*.tmp             in-progress atomic replacements, discarded on open
```

The file names are fixed constants chosen by the library. No user-supplied string ever becomes a
path component.

### Journal file

```
header  (32 bytes)
  magic u32 'TEFJ' | format_version u16 | reserved u16
  start_sequence u64 | reserved u64 | header_crc u32 (CRC-32C of the first 24 bytes) | reserved u32
records
  magic u32 'TEFR' | format_version u16 | record_type u16 | payload_len u32
  sequence u64 | previous_digest 32 bytes | payload_crc u32            (56-byte header)
  payload (payload_len bytes)
  record_digest 32 bytes = SHA-256 over the 56-byte header and the payload
```

### Snapshot file

```
header (40 bytes)
  magic u32 'TEFS' | format_version u16 | reserved u16
  snapshot_sequence u64 | journal_start_sequence u64 | payload_len u64
  payload_crc u32 | reserved u32
payload  = concatenated record images, each with its own framing and digest
digest   = SHA-256 over the 40-byte header and the payload
```

## Transactional sequence

A durable mutation follows validate, bind authority, plan, reserve, write, verify, commit:

1. the engine validates the operation against the live authority;
2. it builds the complete record (plan, fence, epoch or audit) in canonical form;
3. @PlanRepository::append@ assigns the next sequence number and the previous record's digest,
   producing an unbroken hash chain;
4. it writes the framed record and, when @Options::fsync@ is set, flushes and synchronises the file
   before returning. **A successful return therefore means the record survives the failure
   boundary.** No durable mutation is acknowledged before its record is durable;
5. the in-memory indexes are updated only after the write succeeds.

Compaction writes @snapshot.tefs.tmp@, synchronises it, atomically replaces @snapshot.tefs@, then
resets @journal.tefj@ the same way. Every crash window is recoverable:

| Crash point | Recovery outcome |
| --- | --- |
| during the snapshot temp write | the temp file is discarded on open; the previous snapshot and the intact journal are used |
| after the snapshot rename, before the journal reset | the snapshot covers every record; the old journal's records all have sequence <= snapshot sequence and are verified then skipped |
| during the journal temp write | the new snapshot already covers everything; the temp file is discarded |
| mid-record append | the torn tail fails its CRC and digest checks; replay stops at the last valid record and the file is truncated there |

## Recovery

`PlanRepository::open@:

1. creates the directory if needed and deletes any @*.tmp@ left by an interrupted replacement;
2. reads and validates the manifest; creates it if absent; rejects an unsupported format version;
3. loads the snapshot, verifying the payload CRC, the whole-file digest, every record digest and the
   whole hash chain. A snapshot that fails any check is **refused**: the repository does not open, and
   damaged bytes are never reinterpreted as state;
4. replays the journal, verifying framing, format version, record type range, payload length bound,
   CRC, digest and hash chain for every record. Records already covered by the snapshot are verified
   and skipped. The first invalid record stops replay; the file is truncated at the last valid offset
   and the truncated byte count is reported;
5. rebuilds the fence set and the highest coordinator epoch observed in the log.

`Engine::recover_from_repository@ then collapses the log to the latest record per plan identity,
marks every live-state record @STALE@ (a plan that was still advancing when the process died is not
resumed, because the authority that justified it is not restored either), marks every other record
@REVALIDATION_REQUIRED@, and leaves the engine with no live authority until a snapshot is submitted.

## Fencing and coordinator epochs

* The coordinator epoch is persisted and **advanced on every open**, so frames issued by a previous
  coordinator incarnation fail the epoch check and are rejected before they can touch state.
* A publisher boot identity that has been replaced is fenced permanently. Fences are durable facts:
  they survive restarts and are re-loaded on open.
* Liveness is never durable. Registrations, sessions and worker authority live only in memory, so a
  restarted coordinator requires every publisher to re-register under the new epoch.

## Bounds

`PlanRepository::Options@ bounds the record count (@max_records@) and the compaction interval
(@compact_after_records@). Any durable file larger than 512 MiB is refused rather than read. Record
payloads are bounded by @Limits::max_frame_payload@, and every count encoded in a record is checked
against its documented bound before a container is resized.
