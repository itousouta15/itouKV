import test from 'node:test';
import assert from 'node:assert/strict';
import { spawn, spawnSync } from 'node:child_process';
import { mkdtempSync, rmSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { once } from 'node:events';
import { crc32 } from 'node:zlib';

const cli = process.env.ITOUKV_CLI;
const helper = process.env.ITOUKV_TEST_HELPER;
assert.ok(cli && helper, 'Run through ctest, or set ITOUKV_CLI and ITOUKV_TEST_HELPER');
function run(db, ...args) {
  const result = spawnSync(cli, [db, ...args], { encoding: 'utf8', timeout: 10000 });
  assert.ifError(result.error);
  return result;
}
function database(t) {
  const path = mkdtempSync(join(tmpdir(), 'itoukv-process-'));
  t.after(() => rmSync(path, { recursive: true, force: true }));
  return path;
}
function batch(db, input) {
  const result = spawnSync(cli, [db, 'batch', '-'], { input, encoding: 'utf8', timeout: 10000 });
  assert.ifError(result.error);
  return result;
}

test('CLI UTF-8, empty values, scans, exit statuses and binary bytes', t => {
  const db = database(t);
  assert.equal(run(db, 'put', '名稱', '伊藤').status, 0);
  assert.equal(run(db, 'get', '名稱').stdout.trim(), '伊藤');
  assert.equal(run(db, 'put', '', '').status, 0);
  assert.equal(run(db, 'get', '').stdout, '\n');
  assert.equal(run(db, 'get', 'absent').status, 2);
  assert.equal(run(db, 'delete', 'absent').status, 2);
  const binary = spawnSync(cli, ['--hex', db, 'put', '00ff', '000180ff'], { encoding: 'utf8' });
  assert.equal(binary.status, 0, binary.stderr);
  const read = spawnSync(cli, ['--hex', db, 'get', '00ff'], { encoding: 'utf8' });
  assert.equal(read.stdout.trim(), '000180ff');
  const scan = JSON.parse(run(db, 'scan').stdout);
  assert.equal(scan.length, 3);
  assert.deepEqual(scan[1], { key_hex: '00ff', value_hex: '000180ff' });
  assert.equal(run(db, 'scan', '', '-1').status, 1);
  assert.equal(run(db, 'unknown').status, 64);
});

test('WAL, manifest and SSTable match an independent Node/zlib format oracle', t => {
  const db = database(t);
  assert.equal(run(db, 'put', 'alpha', 'one').status, 0);
  assert.equal(run(db, 'delete', 'alpha').status, 0);
  assert.equal(run(db, 'put', 'beta', '雪').status, 0);
  function frame(sequence, operation, keyText, valueText) {
    const key = Buffer.from(keyText), value = Buffer.from(valueText);
    const header = Buffer.alloc(32);
    header.write('IKV1'); header.writeUInt32LE(key.length + value.length, 4);
    header.writeBigUInt64LE(BigInt(sequence), 8);
    header.writeUInt32LE(key.length, 16); header.writeUInt32LE(value.length, 20);
    header[24] = operation; header.writeUInt32LE(crc32(header.subarray(0, 28)), 28);
    const body = Buffer.concat([header, key, value]);
    const checksum = Buffer.alloc(4); checksum.writeUInt32LE(crc32(body));
    return Buffer.concat([body, checksum]);
  }
  assert.deepEqual(readFileSync(join(db, 'data.wal')), Buffer.concat([
    frame(1, 1, 'alpha', 'one'), frame(2, 2, 'alpha', ''), frame(3, 1, 'beta', '雪'),
  ]));
  assert.equal(run(db, 'compact').status, 0);
  const manifest = readFileSync(join(db, 'CURRENT'));
  assert.equal(manifest.subarray(0, 8).toString(), 'IKVCUR01');
  assert.equal(manifest.readBigUInt64LE(8), 1n);
  assert.equal(manifest.readBigUInt64LE(16), 3n);
  assert.equal(manifest.readUInt32LE(28), crc32(manifest.subarray(0, 28)));
  const table = readFileSync(join(db, 'TABLE-1.sst'));
  assert.equal(table.subarray(0, 8).toString(), 'IKVSST01');
  assert.equal(table.readBigUInt64LE(8), 3n);
  assert.equal(table.readBigUInt64LE(16), 1n);
  assert.equal(table.readUInt32LE(76), crc32(table.subarray(0, 76)));
  assert.equal(table.readUInt32LE(table.length - 4), crc32(table.subarray(0, -4)));
  assert.equal(table.readUInt32LE(80), 4);
  assert.equal(table.readUInt32LE(84), 3);
  assert.equal(table.subarray(88, 95).toString(), 'beta雪');
  assert.equal(table.readUInt32LE(95), crc32(table.subarray(80, 95)));
  assert.equal(table.readBigUInt64LE(24), 99n);
  assert.equal(readFileSync(join(db, 'WAL-1.log')).length, 0);
});

test('checksummed but oversized WAL lengths are rejected without allocation or truncation', t => {
  const db = database(t);
  assert.equal(run(db, 'put', 'key', 'value').status, 0);
  const bytes = readFileSync(join(db, 'data.wal'));
  bytes.writeUInt32LE(0xffffffff, 4);
  bytes.writeUInt32LE(0xffffffff, 16);
  bytes.writeUInt32LE(0, 20);
  bytes.writeUInt32LE(crc32(bytes.subarray(0, 28)), 28);
  writeFileSync(join(db, 'data.wal'), bytes);
  const opened = run(db, 'get', 'key');
  assert.equal(opened.status, 1);
  assert.match(opened.stderr, /invalid lengths/);
  assert.deepEqual(readFileSync(join(db, 'data.wal')), bytes);
});

for (const compacted of [false, true]) {
for (const point of ['before_append', 'after_header', 'after_payload', 'after_append', 'after_sync']) {
  test(`process dies at ${point} (SSTable=${compacted}): acknowledged prefix survives`, t => {
    const db = database(t);
    assert.equal(run(db, 'put', 'stable', 'acknowledged').status, 0);
    if (compacted) assert.equal(run(db, 'compact').status, 0);
    const crashed = spawnSync(helper, ['--crash-worker', db, point], { encoding: 'utf8', timeout: 10000 });
    assert.equal(crashed.status, 99, crashed.stderr);
    assert.equal(run(db, 'get', 'stable').stdout.trim(), 'acknowledged');
    const pending = run(db, 'get', 'crash');
    if (point === 'after_sync') assert.equal(pending.stdout.trim(), 'candidate');
    else if (point === 'after_append') {
      // An unacknowledged complete write is allowed to survive. A process exit
      // is not a power cut, and the OS page cache normally retains these bytes.
      assert.ok(pending.status === 0 || pending.status === 2);
      if (pending.status === 0) assert.equal(pending.stdout.trim(), 'candidate');
    } else assert.equal(pending.status, 2);
    assert.equal(run(db, 'put', 'next', 'after-recovery').status, 0);
    assert.equal(run(db, 'get', 'next').stdout.trim(), 'after-recovery');
  });
}
}

for (const point of ['after_sstable_sync', 'after_new_wal_sync', 'after_manifest_temp_sync', 'after_manifest_publish', 'after_compaction_cleanup']) {
  test(`compaction process dies at ${point}: manifest selects a complete generation`, t => {
    const db = database(t);
    assert.equal(run(db, 'put', 'stable', 'original').status, 0);
    assert.equal(run(db, 'put', 'deleted', 'old-value').status, 0);
    assert.equal(run(db, 'compact').status, 0);
    assert.equal(run(db, 'put', 'stable', 'updated').status, 0);
    assert.equal(run(db, 'delete', 'deleted').status, 0);
    assert.equal(run(db, 'put', 'added', 'new-value').status, 0);
    const crashed = spawnSync(helper, ['--compact-worker', db, point], { encoding: 'utf8', timeout: 10000 });
    assert.equal(crashed.status, 99, crashed.stderr);
    assert.equal(run(db, 'get', 'stable').stdout.trim(), 'updated');
    assert.equal(run(db, 'get', 'added').stdout.trim(), 'new-value');
    assert.equal(run(db, 'get', 'deleted').status, 2);
    const stats = JSON.parse(run(db, 'stats').stdout);
    assert.equal(stats.sequence, 5);
    assert.equal(stats.generation, ['after_manifest_publish', 'after_compaction_cleanup'].includes(point) ? 2 : 1);
    assert.equal(run(db, 'compact').status, 0); // Reuses/overwrites orphan candidates safely.
    assert.equal(run(db, 'put', 'next', 'after-recovery').status, 0);
    assert.equal(run(db, 'get', 'next').stdout.trim(), 'after-recovery');
  });
}

test('exclusive lock is cross-process and released by OS after forced termination', async t => {
  const db = database(t);
  const holder = spawn(helper, ['--hold-lock', db], { stdio: ['pipe', 'pipe', 'pipe'] });
  t.after(() => { if (holder.exitCode === null) holder.kill(); });
  let text = '';
  await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('lock holder timeout')), 10000);
    holder.once('error', error => { clearTimeout(timer); reject(error); });
    holder.once('exit', code => { clearTimeout(timer); reject(new Error(`early exit ${code}`)); });
    holder.stdout.on('data', chunk => {
      text += chunk.toString();
      if (text.includes('LOCKED')) { clearTimeout(timer); resolve(); }
    });
  });
  const blocked = run(db, 'put', 'blocked', 'value');
  assert.equal(blocked.status, 1);
  assert.match(blocked.stderr, /already open/);
  const exited = once(holder, 'exit');
  holder.kill('SIGKILL');
  await exited;
  assert.equal(run(db, 'put', 'reopened', 'value').status, 0);
  assert.equal(run(db, 'get', 'blocked').status, 2);
});

test('CLI batch validates the complete input before writing and commits binary mutations once', t => {
  const db = database(t);
  assert.equal(run(db, 'put', 'keep', 'original').status, 0);
  const invalid = batch(db, 'put\t6b656570\t6368616e676564\nput\tzz\t00\n');
  assert.equal(invalid.status, 1);
  assert.match(invalid.stderr, /batch line 2/);
  assert.equal(run(db, 'get', 'keep').stdout.trim(), 'original');
  assert.equal(JSON.parse(run(db, 'stats').stdout).sequence, 1);
  const result = batch(db, '# hex TSV\r\nput\t00ff\t0001ff\r\ndelete\t6b656570\r\nput\t\t\r\n');
  assert.equal(result.status, 0, result.stderr);
  assert.equal(result.stdout.trim(), 'COMMITTED 3');
  assert.equal(JSON.parse(run(db, 'stats').stdout).sequence, 2);
  assert.equal(run(db, 'get', 'keep').status, 2);
  assert.deepEqual(JSON.parse(run(db, 'scan').stdout), [{ key_hex: '', value_hex: '' }, { key_hex: '00ff', value_hex: '0001ff' }]);
  assert.equal(batch(db, '# empty batch\n').status, 0);
  assert.equal(JSON.parse(run(db, 'stats').stdout).sequence, 2);
});

test('batch v2 frame has one sequence and independently verifiable checksums and entry lengths', t => {
  const db = database(t);
  assert.equal(batch(db, 'put\t61\t6263\ndelete\t64\n').status, 0);
  const actual = readFileSync(join(db, 'data.wal'));
  const put = Buffer.alloc(12); put[0] = 1; put.writeUInt32LE(1, 1); put.writeUInt32LE(2, 5); put.write('abc', 9);
  const del = Buffer.alloc(10); del[0] = 2; del.writeUInt32LE(1, 1); del.write('d', 9);
  const header = Buffer.alloc(32); header.write('IKV2'); header.writeUInt32LE(22, 4);
  header.writeBigUInt64LE(1n, 8); header.writeUInt32LE(2, 16); header[24] = 3;
  header.writeUInt32LE(crc32(header.subarray(0, 28)), 28);
  const body = Buffer.concat([header, put, del]);
  const checksum = Buffer.alloc(4); checksum.writeUInt32LE(crc32(body));
  assert.deepEqual(actual, Buffer.concat([body, checksum]));
  // Keep both CRCs valid, but forge the last entry's value length. The decoder
  // must reject it, not publish the already-decoded first operation.
  actual.writeUInt32LE(1, 32 + 12 + 5);
  actual.writeUInt32LE(crc32(actual.subarray(0, -4)), actual.length - 4);
  writeFileSync(join(db, 'data.wal'), actual);
  assert.match(run(db, 'get', 'a').stderr, /invalid batch entry/);
  assert.deepEqual(readFileSync(join(db, 'data.wal')), actual);
});

for (const compacted of [false, true]) {
  for (const point of ['before_append', 'after_header', 'after_batch_entry', 'after_payload', 'after_append', 'after_sync']) {
    test(`batch dies at ${point} (SSTable=${compacted}): all or none`, t => {
      const db = database(t);
      for (const key of ['left', 'gone', 'right']) assert.equal(run(db, 'put', key, 'old').status, 0);
      if (compacted) assert.equal(run(db, 'compact').status, 0);
      const crashed = spawnSync(helper, ['--batch-worker', db, point], { encoding: 'utf8', timeout: 10000 });
      assert.equal(crashed.status, 99, crashed.stderr);
      const actual = JSON.parse(run(db, 'scan').stdout);
      const decode = rows => Object.fromEntries(rows.map(row => [Buffer.from(row.key_hex, 'hex').toString(), Buffer.from(row.value_hex, 'hex').toString()]));
      const state = decode(actual);
      const before = { gone: 'old', left: 'old', right: 'old' }, after = { left: 'new', right: 'new' };
      if (point === 'after_sync') assert.deepEqual(state, after);
      else if (point === 'after_append') assert.ok(JSON.stringify(state) === JSON.stringify(before) || JSON.stringify(state) === JSON.stringify(after));
      else assert.deepEqual(state, before);
      const sequence = JSON.parse(run(db, 'stats').stdout).sequence;
      assert.equal(sequence, state.left === 'new' ? 4 : 3);
      assert.equal(batch(db, 'put\t6c656674\t646f6e65\nput\t7269676874\t646f6e65\n').status, 0);
      assert.equal(run(db, 'get', 'left').stdout.trim(), 'done');
      assert.equal(run(db, 'get', 'right').stdout.trim(), 'done');
    });
  }
}
