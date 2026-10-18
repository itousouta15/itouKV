import test from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const cli = process.env.ITOUKV_NOTES;
assert.ok(cli, 'Run through ctest, or set ITOUKV_NOTES');

function workdir(t) {
  const path = mkdtempSync(join(tmpdir(), 'itoukv-notes-'));
  t.after(() => rmSync(path, { recursive: true, force: true }));
  return path;
}

function run(db, args, input) {
  const result = spawnSync(cli, [db, ...args], { input, timeout: 120000, maxBuffer: 40 * 1024 * 1024 });
  assert.ifError(result.error);
  return { ...result, text: result.stdout.toString('utf8'), error: result.stderr.toString('utf8') };
}

function ok(result) {
  assert.equal(result.status, 0, result.error);
  return result;
}

test('CRUD survives restarts and tag indexes follow edits and deletes', t => {
  const dir = workdir(t);
  const db = join(dir, 'db');
  ok(run(db, ['add', 'one', '第一篇', '-', 'work', 'daily'], 'hello\n雪'));
  assert.equal(run(db, ['add', 'one', 'duplicate', '-'], 'x').status, 1);
  ok(run(db, ['add', 'two', 'Second', '-', 'work'], 'body'));
  assert.equal(run(db, ['list']).text, 'one\t第一篇\tdaily,work\ntwo\tSecond\twork\n');
  assert.equal(run(db, ['list', 'daily']).text, 'one\t第一篇\tdaily,work\n');
  assert.equal(run(db, ['show', 'one']).text, 'ID: one\nTitle: 第一篇\nTags: daily, work\n\nhello\n雪');

  const file = join(dir, 'body.txt');
  writeFileSync(file, 'revised\n');
  ok(run(db, ['edit', 'one', '更新', file, 'other', 'work']));
  assert.equal(run(db, ['list', 'daily']).text, '');
  assert.equal(run(db, ['list', 'other']).text, 'one\t更新\tother,work\n');
  assert.match(run(db, ['show', 'one']).text, /revised\n$/);
  ok(run(db, ['compact']));
  assert.equal(run(db, ['list', 'work']).text, 'one\t更新\tother,work\ntwo\tSecond\twork\n');
  ok(run(db, ['delete', 'one']));
  assert.equal(run(db, ['show', 'one']).status, 2);
  assert.equal(run(db, ['edit', 'one', 'missing', '-'], '').status, 2);
  assert.equal(run(db, ['delete', 'one']).status, 2);
  assert.equal(run(db, ['list', 'other']).text, '');
  assert.equal(run(db, ['list', 'work']).text, 'two\tSecond\twork\n');
});

test('export/import round trip preserves binary bodies and rejects invalid archives before writing', t => {
  const dir = workdir(t);
  const source = join(dir, 'source');
  const target = join(dir, 'target');
  const archive = join(dir, 'notes.tsv');
  const body = Buffer.from([0, 10, 13, 255, 9, 0]);
  ok(run(source, ['add', 'binary', '位元組', '-', 'sample'], body));
  ok(run(source, ['add', 'empty', 'Empty', '-'], Buffer.alloc(0)));
  ok(run(source, ['export', archive]));
  const exported = readFileSync(archive);
  assert.match(exported.toString('utf8'), /^ITOUKV_NOTES_V1\n/);
  ok(run(target, ['import', archive]));
  assert.deepEqual(run(target, ['show', 'binary']).stdout,
    Buffer.concat([Buffer.from('ID: binary\nTitle: 位元組\nTags: sample\n\n'), body]));
  assert.equal(run(target, ['list']).text, 'binary\t位元組\tsample\nempty\tEmpty\t\n');
  const restored = join(dir, 'restored.tsv');
  ok(run(target, ['export', restored]));
  assert.deepEqual(readFileSync(restored), exported);
  assert.equal(run(target, ['import', archive]).status, 1);
  assert.equal(run(target, ['list']).text, 'binary\t位元組\tsample\nempty\tEmpty\t\n');

  const invalid = join(dir, 'invalid.tsv');
  writeFileSync(invalid, 'ITOUKV_NOTES_V1\nnew\t4e6577\t626f6479\tx\nbad\tnot-hex\t00\t\n');
  assert.match(run(target, ['import', invalid]).error, /archive line 3/);
  assert.equal(run(target, ['show', 'new']).status, 2);
  writeFileSync(invalid, 'ITOUKV_NOTES_V1\nnew\t4e6577\t00\t\nnew\t4e6577\t00\t\n');
  assert.match(run(target, ['import', invalid]).error, /duplicate note id/);
  assert.equal(run(target, ['show', 'new']).status, 2);
});

test('large archive crosses WriteBatch boundaries without losing notes or tags', t => {
  const dir = workdir(t);
  const db = join(dir, 'db');
  const archive = join(dir, 'many.tsv');
  const lines = ['ITOUKV_NOTES_V1'];
  for (let i = 0; i < 2060; i++) lines.push(`n${i}\t4e\t\tgroup`);
  writeFileSync(archive, lines.join('\n') + '\n');
  ok(run(db, ['import', archive]));
  assert.equal(run(db, ['list', 'group']).text.trim().split('\n').length, 2060);
  assert.equal(run(db, ['show', 'n2059']).text, 'ID: n2059\nTitle: N\nTags: group\n\n');
  const exported = join(dir, 'many-export.tsv');
  ok(run(db, ['export', exported]));
  const restored = join(dir, 'restored');
  ok(run(restored, ['import', exported]));
  assert.equal(run(restored, ['list', 'group']).text.trim().split('\n').length, 2060);
});
