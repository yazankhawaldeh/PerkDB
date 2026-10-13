'use strict';
// Node CRUD test for the N-API addon. Uses only Node built-ins.
// Run with: node --expose-gc tests/test_addon_crud.js  (after `npm run build`)
// Without --expose-gc the file re-spawns itself with the flag.

const assert = require('node:assert');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

if (typeof global.gc !== 'function' && !process.env.PERKDB_CRUD_CHILD) {
  const r = spawnSync(process.execPath, ['--expose-gc', __filename], {
    stdio: 'inherit',
    env: { ...process.env, PERKDB_CRUD_CHILD: '1' },
  });
  if (r.error) {
    console.error(`could not respawn with --expose-gc: ${r.error.message}`);
    process.exit(1);
  }
  process.exit(r.status === null ? 1 : r.status);
}

const root = path.resolve(__dirname, '..');
const { Database, PerkError, constants, openErrmsg } = require(root);

const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'perkdb-crud-'));
process.on('exit', () => fs.rmSync(tmpDir, { recursive: true, force: true }));
let fileCounter = 0;
const tmpFile = () => path.join(tmpDir, `db${fileCounter++}.sqlite`);

const tests = [];
const test = (name, fn) => tests.push([name, fn]);

// ---- helpers ----
function perkErr(code) {
  return (e) => {
    assert.ok(e instanceof PerkError, `expected PerkError, got ${e && e.name}: ${e && e.message}`);
    assert.ok(e instanceof Error);
    assert.strictEqual(e.code, code, `code ${e.code} != ${code}`);
    assert.strictEqual(e.errno, code === 'PERK_ERR_CLOSED' ? constants.PERK_ERR_MISUSE : constants[code]);
    assert.strictEqual(typeof e.message, 'string');
    return true;
  };
}
function typeErr(code = 'ERR_INVALID_ARG_TYPE') {
  return (e) => {
    assert.ok(e instanceof TypeError, `expected TypeError, got ${e && e.name}: ${e && e.message}`);
    assert.strictEqual(e.code, code);
    return true;
  };
}
function rangeErr(e) {
  assert.ok(e instanceof RangeError);
  assert.strictEqual(e.code, 'ERR_OUT_OF_RANGE');
  return true;
}
function mem() {
  const db = new Database(':memory:');
  db.createTable('t', [
    { name: 'id', type: 'INTEGER', primaryKey: true },
    { name: 'name', type: 'TEXT' },
    { name: 'age', type: 'INTEGER' },
  ]);
  return db;
}
function users(db) {
  db.insert('t', { name: 'Yazan', age: 21 });
  db.insert('t', { name: 'Sam', age: 30 });
  db.insert('t', { name: 'Lee', age: 17 });
}
function valueTable() {
  const db = new Database(':memory:');
  db.createTable('v', [
    { name: 'id', type: 'INTEGER', primaryKey: true },
    { name: 'x' },
  ]);
  return db;
}
function roundTrip(v) {
  const db = valueTable();
  const id = db.insert('v', { x: v });
  const row = db.selectById('v', 'id', id);
  db.close();
  return row.x;
}
const settle = () => new Promise((r) => setImmediate(r));
async function collect(times = 8) {
  for (let i = 0; i < times; i++) {
    global.gc();
    await settle();
    await new Promise((r) => setTimeout(r, 5));
  }
}

// ---- module surface ----
test('exports and constants', () => {
  assert.strictEqual(typeof Database, 'function');
  assert.strictEqual(typeof PerkError, 'function');
  assert.strictEqual(typeof openErrmsg, 'function');
  for (const k of ['PERK_OK', 'PERK_ERR_OPEN', 'PERK_ERR_SQL', 'PERK_ERR_BUSY', 'PERK_ERR_BADNAME',
    'PERK_ERR_NOMEM', 'PERK_ERR_MISUSE', 'PERK_ERR_EXISTS', 'PERK_ERR_NOT_FOUND',
    'PERK_COL_PK', 'PERK_COL_NOTNULL', 'PERK_COL_UNIQUE']) {
    assert.strictEqual(typeof constants[k], 'number', k);
  }
  assert.strictEqual(constants.PERK_OK, 0);
  assert.ok(Object.isFrozen(constants));
});

test('Database.open and new Database are equivalent; class needs new', () => {
  const a = Database.open(':memory:');
  const b = new Database(':memory:');
  assert.ok(a instanceof Database && b instanceof Database);
  assert.strictEqual(a.isOpen, true);
  a.close();
  assert.strictEqual(a.isOpen, false);
  assert.strictEqual(b.isOpen, true);
  b.close();
  assert.throws(() => Database(':memory:'), TypeError);
});

// ---- open ----
test('open: bad path -> PERK_ERR_OPEN and openErrmsg set', () => {
  const bad = path.join(tmpDir, 'no', 'such', 'dir', 'x.db');
  assert.throws(() => new Database(bad), perkErr('PERK_ERR_OPEN'));
  assert.strictEqual(typeof openErrmsg(), 'string');
  assert.ok(openErrmsg().length > 0);
  assert.throws(() => Database.open(bad), perkErr('PERK_ERR_OPEN'));
});

test('open: wrong arg types and NUL in path', () => {
  for (const bad of [undefined, null, 1, {}, [], Buffer.from('x'), 1n, Symbol('s'), () => {}]) {
    assert.throws(() => new Database(bad), typeErr());
  }
  assert.throws(() => new Database(), typeErr());
  assert.throws(() => new Database('a\0b'), typeErr('ERR_INVALID_ARG_VALUE'));
});

test('open: empty path does not crash (temp db or PerkError)', () => {
  try {
    const db = new Database('');
    db.close();
  } catch (e) {
    assert.ok(e instanceof PerkError);
  }
});

test('open: file db persists across connections', () => {
  const f = tmpFile();
  const a = new Database(f);
  a.createTable('t', [{ name: 'k', type: 'TEXT' }]);
  a.insert('t', { k: 'hello' });
  a.close();
  const b = new Database(f);
  assert.deepStrictEqual(b.select('t'), [{ k: 'hello' }]);
  b.close();
});

// ---- createTable ----
test('createTable: happy path, constraints flags', () => {
  const db = new Database(':memory:');
  assert.strictEqual(
    db.createTable('c', [
      { name: 'id', type: 'INTEGER', primaryKey: true },
      { name: 'u', type: 'TEXT', unique: true },
      { name: 'n', type: 'TEXT', notNull: true },
      { name: 'f', type: 'TEXT', flags: constants.PERK_COL_NOTNULL },
      { name: 'plain' },
    ]),
    undefined
  );
  db.insert('c', { u: 'a', n: 'x', f: 'y' });
  assert.throws(() => db.insert('c', { u: 'a', n: 'x', f: 'y' }), perkErr('PERK_ERR_SQL'));
  assert.throws(() => db.insert('c', { u: 'b', f: 'y' }), perkErr('PERK_ERR_SQL'));
  assert.throws(() => db.insert('c', { u: 'b', n: 'x' }), perkErr('PERK_ERR_SQL'));
  assert.strictEqual(db.select('c').length, 1);
  db.close();
});

test('createTable: duplicate -> PERK_ERR_EXISTS, data intact', () => {
  const db = mem();
  users(db);
  assert.throws(() => db.createTable('t', [{ name: 'z' }]), perkErr('PERK_ERR_EXISTS'));
  assert.strictEqual(db.select('t').length, 3);
  db.close();
});

test('createTable: invalid names and types -> BADNAME', () => {
  const db = new Database(':memory:');
  const badNames = ["x'; DROP TABLE y;--", '', '1abc', 'a b', 'a-b', 'a;b', '"', 'a"b', 'x) ; --'];
  for (const n of badNames) {
    assert.throws(() => db.createTable(n, [{ name: 'a' }]), perkErr('PERK_ERR_BADNAME'), JSON.stringify(n));
    assert.throws(() => db.createTable('ok', [{ name: n }]), perkErr('PERK_ERR_BADNAME'), JSON.stringify(n));
  }
  assert.throws(
    () => db.createTable('ok', [{ name: 'a', type: 'TEXT); DROP TABLE ok;--' }]),
    perkErr('PERK_ERR_BADNAME')
  );
  // nothing was created by the failed attempts
  db.createTable('ok', [{ name: 'a' }]);
  db.close();
});

test('createTable: wrong types / empty column list', () => {
  const db = new Database(':memory:');
  for (const n of [undefined, null, 5, {}, [], Buffer.from('t')]) {
    assert.throws(() => db.createTable(n, [{ name: 'a' }]), typeErr());
  }
  assert.throws(() => db.createTable('a\0b', [{ name: 'a' }]), typeErr('ERR_INVALID_ARG_VALUE'));
  for (const c of [undefined, null, 'a', {}, 5]) {
    assert.throws(() => db.createTable('x', c), typeErr());
  }
  for (const c of [[null], [1], ['a'], [[]], [{}], [{ name: 5 }], [{ name: 'a', type: 5 }],
    [{ name: 'a', primaryKey: 'yes' }], [{ name: 'a', flags: 'x' }], [{ name: 'a\0' }]]) {
    assert.throws(() => db.createTable('x', c), (e) => e instanceof TypeError && typeof e.code === 'string');
  }
  assert.throws(() => db.createTable('x', []), (e) => e instanceof PerkError);
  assert.throws(() => db.select('x'), perkErr('PERK_ERR_SQL'), 'failed creates leave no table');
  db.close();
});

// ---- insert ----
test('insert: object and array forms return increasing rowids', () => {
  const db = mem();
  const a = db.insert('t', { name: 'A', age: 1 });
  const b = db.insert('t', ['name', 'age'], ['B', 2]);
  assert.strictEqual(typeof a, 'number');
  assert.strictEqual(b, a + 1);
  assert.deepStrictEqual(db.select('t', { columns: ['name', 'age'] }), [
    { name: 'A', age: 1 },
    { name: 'B', age: 2 },
  ]);
  db.close();
});

test('insert: errors', () => {
  const db = mem();
  assert.throws(() => db.insert('nope', { a: 1 }), perkErr('PERK_ERR_SQL'));
  assert.throws(() => db.insert('t', { nocol: 1 }), perkErr('PERK_ERR_SQL'));
  assert.throws(() => db.insert('t; DROP TABLE t', { name: 'x' }), perkErr('PERK_ERR_BADNAME'));
  assert.throws(() => db.insert('t', { 'name) VALUES (1); DROP TABLE t;--': 'x' }), perkErr('PERK_ERR_BADNAME'));
  assert.throws(() => db.insert('t', ['name'], ['a', 'b']), typeErr('ERR_INVALID_ARG_VALUE'));
  assert.throws(() => db.insert('t', ['name']), typeErr());
  assert.throws(() => db.insert('t', ['name'], 'a'), typeErr());
  for (const bad of [undefined, null, 'x', 5, () => {}, Buffer.from('a'), new Uint8Array(2)]) {
    assert.throws(() => db.insert('t', bad), typeErr(), String(bad));
  }
  assert.throws(() => db.insert(undefined, { name: 'x' }), typeErr());
  assert.throws(() => db.insert(), typeErr());
  assert.throws(() => db.insert('t', {}), (e) => e instanceof PerkError);
  assert.throws(() => db.insert('t', [], []), (e) => e instanceof PerkError);
  assert.strictEqual(db.select('t').length, 0);
  db.close();
});

test('insert: injection payloads in values are stored literally', () => {
  const db = mem();
  const payloads = ["'; DROP TABLE t;--", "\" OR 1=1 --", "Robert'); DROP TABLE t;--", '%', '\\'];
  for (const p of payloads) db.insert('t', { name: p });
  const names = db.select('t').map((r) => r.name);
  assert.deepStrictEqual(names, payloads);
  db.close();
});

// ---- value mapping ----
test('values: integers', () => {
  for (const n of [0, 1, -1, 42, -42, 2147483647, -2147483648, 4294967296]) {
    const out = roundTrip(n);
    assert.strictEqual(out, n);
    assert.strictEqual(typeof out, 'number');
  }
  assert.ok(Object.is(roundTrip(-0), 0) || Object.is(roundTrip(-0), -0));
});

test('values: safe integer boundary -> number, beyond -> BigInt', () => {
  const MAX = Number.MAX_SAFE_INTEGER;
  assert.strictEqual(roundTrip(MAX), MAX);
  assert.strictEqual(typeof roundTrip(MAX), 'number');
  assert.strictEqual(roundTrip(-MAX), -MAX);
  assert.strictEqual(typeof roundTrip(-MAX), 'number');
  assert.strictEqual(roundTrip(BigInt(MAX)), MAX);
  assert.strictEqual(roundTrip(BigInt(MAX) + 1n), BigInt(MAX) + 1n);
  assert.strictEqual(typeof roundTrip(BigInt(MAX) + 1n), 'bigint');
  assert.strictEqual(roundTrip(-BigInt(MAX) - 1n), -BigInt(MAX) - 1n);
  assert.strictEqual(typeof roundTrip(-BigInt(MAX) - 1n), 'bigint');
  assert.strictEqual(roundTrip(0n), 0);
  assert.strictEqual(roundTrip(-5n), -5);
});

test('values: int64 min/max and out-of-range BigInt', () => {
  const MIN = -(2n ** 63n);
  const MAXI = 2n ** 63n - 1n;
  assert.strictEqual(roundTrip(MIN), MIN);
  assert.strictEqual(roundTrip(MAXI), MAXI);
  const db = valueTable();
  for (const big of [MAXI + 1n, MIN - 1n, 2n ** 64n, -(2n ** 100n)]) {
    assert.throws(() => db.insert('v', { x: big }), rangeErr);
    assert.throws(() => db.insert('v', ['x'], [big]), rangeErr);
    assert.throws(() => db.update('v', { x: big }), (e) => e instanceof RangeError || e instanceof PerkError);
    assert.throws(() => db.select('v', { where: 'x = ?', args: [big] }), rangeErr);
    assert.throws(() => db.selectById('v', 'id', big), rangeErr);
  }
  assert.strictEqual(db.select('v').length, 0);
  db.close();
});

test('values: large integral numbers beyond int64 become REAL', () => {
  assert.strictEqual(roundTrip(2 ** 63), 2 ** 63);
  assert.strictEqual(roundTrip(1e300), 1e300);
  assert.strictEqual(roundTrip(-1e300), -1e300);
  assert.strictEqual(typeof roundTrip(1e300), 'number');
});

test('values: REAL', () => {
  for (const r of [0.5, -0.5, 3.14159, 1e-300, Number.MIN_VALUE, Number.MAX_VALUE, Number.EPSILON]) {
    assert.strictEqual(roundTrip(r), r);
  }
});

test('values: NaN is stored as NULL; Infinity round-trips', () => {
  assert.strictEqual(roundTrip(NaN), null);
  assert.strictEqual(roundTrip(Infinity), Infinity);
  assert.strictEqual(roundTrip(-Infinity), -Infinity);
});

test('values: strings (unicode, embedded NUL, empty)', () => {
  const samples = ['hello', '', 'héllo wörld', '日本語テキスト', '😀🎉 emoji', 'a\0b', '\0', 'x\0', '\0\0\0',
    'line1\nline2\r\n\ttab', 'a'.repeat(100000)];
  for (const s of samples) {
    const out = roundTrip(s);
    assert.strictEqual(out, s, JSON.stringify(s.slice(0, 20)));
    assert.strictEqual(out.length, s.length);
  }
});

test('values: blobs (Buffer, Uint8Array, empty, embedded zeros)', () => {
  const cases = [
    Buffer.from([1, 2, 3]),
    Buffer.alloc(0),
    Buffer.from([0, 0, 0]),
    Buffer.from([0xff, 0, 0xfe, 0, 1]),
    new Uint8Array([9, 0, 8]),
    new Uint8Array(0),
    Buffer.alloc(100000, 7),
    new Uint8Array(new ArrayBuffer(8), 2, 4).fill(5),
  ];
  for (const b of cases) {
    const out = roundTrip(b);
    assert.ok(Buffer.isBuffer(out), 'blob comes back as Buffer');
    assert.deepStrictEqual([...out], [...b]);
  }
});

test('values: blob input is copied, not aliased', () => {
  const db = valueTable();
  const buf = Buffer.from([1, 2, 3]);
  const id = db.insert('v', { x: buf });
  buf.fill(9);
  assert.deepStrictEqual([...db.selectById('v', 'id', id).x], [1, 2, 3]);
  db.close();
});

test('values: booleans -> 1/0', () => {
  assert.strictEqual(roundTrip(true), 1);
  assert.strictEqual(roundTrip(false), 0);
});

test('values: null/undefined -> null', () => {
  assert.strictEqual(roundTrip(null), null);
  assert.strictEqual(roundTrip(undefined), null);
});

test('values: unsupported types -> TypeError', () => {
  const db = valueTable();
  const bads = [new Date(), Symbol('s'), new Float32Array(2), new Int8Array(2), new Uint16Array(1),
    new Uint8ClampedArray(1), new DataView(new ArrayBuffer(2)), new ArrayBuffer(4), {}, { a: 1 }, [1, 2],
    () => {}, /re/, new Map(), new Set(), Object(1)];
  for (const bad of bads) {
    assert.throws(() => db.insert('v', { x: bad }), typeErr(), String(Object.prototype.toString.call(bad)));
    assert.throws(() => db.insert('v', ['x'], [bad]), typeErr());
    assert.throws(() => db.update('v', { x: bad }), typeErr());
    assert.throws(() => db.select('v', { where: 'x = ?', args: [bad] }), typeErr());
    assert.throws(() => db.selectById('v', 'id', bad), typeErr());
    assert.throws(() => db.delete('v', 'x = ?', [bad]), typeErr());
  }
  assert.strictEqual(db.select('v').length, 0);
  db.close();
});

// ---- select ----
test('select: list-all, columns subset, where + args', () => {
  const db = mem();
  users(db);
  assert.strictEqual(db.select('t').length, 3);
  assert.strictEqual(db.select('t', {}).length, 3);
  assert.strictEqual(db.select('t', null).length, 3);
  assert.strictEqual(db.select('t', undefined).length, 3);
  assert.deepStrictEqual(db.select('t', { columns: ['name'] }).map((r) => Object.keys(r)), [['name'], ['name'], ['name']]);
  assert.deepStrictEqual(db.select('t', { columns: ['age', 'name'], where: 'age > ?', args: [18] }), [
    { age: 21, name: 'Yazan' },
    { age: 30, name: 'Sam' },
  ]);
  assert.deepStrictEqual(db.select('t', { where: 'name = ? AND age = ?', args: ['Sam', 30] }), [
    { id: 2, name: 'Sam', age: 30 },
  ]);
  assert.deepStrictEqual(db.select('t', { where: 'name = ?', args: [null] }), []);
  const all = db.select('t');
  assert.deepStrictEqual(Object.keys(all[0]), ['id', 'name', 'age']);
  db.close();
});

test('select: empty result is [] (not an error)', () => {
  const db = mem();
  assert.deepStrictEqual(db.select('t'), []);
  users(db);
  assert.deepStrictEqual(db.select('t', { where: 'age > ?', args: [1000] }), []);
  db.close();
});

test('select: errors', () => {
  const db = mem();
  users(db);
  assert.throws(() => db.select('nope'), perkErr('PERK_ERR_SQL'));
  assert.throws(() => db.select('t', { columns: ['nocol'] }), perkErr('PERK_ERR_SQL'));
  assert.throws(() => db.select('t; DROP TABLE t'), perkErr('PERK_ERR_BADNAME'));
  assert.throws(() => db.select('t', { columns: ['name FROM t;--'] }), perkErr('PERK_ERR_BADNAME'));
  assert.throws(() => db.select('t', { where: 'age > ?', args: [] }), perkErr('PERK_ERR_MISUSE'));
  assert.throws(() => db.select('t', { where: 'age > 1', args: [1] }), perkErr('PERK_ERR_MISUSE'));
  assert.throws(() => db.select('t', { where: 'age >' }), (e) => e instanceof PerkError);
  assert.throws(() => db.select('t', { where: '1=1; DROP TABLE t' }), (e) => e instanceof PerkError);
  assert.strictEqual(db.select('t').length, 3);
  for (const bad of ['x', 5, true, () => {}]) {
    assert.throws(() => db.select('t', bad), typeErr());
  }
  assert.throws(() => db.select('t', { columns: 'name' }), typeErr());
  assert.throws(() => db.select('t', { columns: [1] }), typeErr());
  assert.throws(() => db.select('t', { args: 'x' }), typeErr());
  assert.throws(() => db.select('t', { where: 5 }), typeErr());
  assert.throws(() => db.select('t', { where: 'a\0b' }), typeErr('ERR_INVALID_ARG_VALUE'));
  assert.throws(() => db.select('t', { columns: ['a\0b'] }), typeErr('ERR_INVALID_ARG_VALUE'));
  assert.throws(() => db.select(), typeErr());
  assert.throws(() => db.select(null), typeErr());
  assert.strictEqual(db.select('t', { columns: [] }).length, 3, 'empty columns list selects every column');
  db.close();
});

test('select: injection payloads in args and where leave data unchanged', () => {
  const db = mem();
  users(db);
  const payloads = ["'; DROP TABLE t;--", '" OR 1=1', "' OR '1'='1", '1 OR 1=1'];
  for (const p of payloads) {
    assert.deepStrictEqual(db.select('t', { where: 'name = ?', args: [p] }), []);
    assert.throws(() => db.selectById('t', 'id', p), perkErr('PERK_ERR_NOT_FOUND'));
    assert.throws(() => db.update('t', { age: 1 }, 'name = ?', [p]), perkErr('PERK_ERR_NOT_FOUND'));
    assert.throws(() => db.delete('t', 'name = ?', [p]), perkErr('PERK_ERR_NOT_FOUND'));
    assert.throws(() => db.select(p), (e) => e instanceof PerkError);
    assert.throws(() => db.select('t', { columns: [p] }), (e) => e instanceof PerkError);
    assert.throws(() => db.insert(p, { name: 'x' }), (e) => e instanceof PerkError);
    assert.throws(() => db.insert('t', { [p]: 'x' }), (e) => e instanceof PerkError);
    assert.throws(() => db.update(p, { age: 1 }, 'id = ?', [1]), (e) => e instanceof PerkError);
    assert.throws(() => db.update('t', { [p]: 1 }, 'id = ?', [1]), (e) => e instanceof PerkError);
    assert.throws(() => db.delete(p, 'id = ?', [1]), (e) => e instanceof PerkError);
    assert.throws(() => db.selectById(p, 'id', 1), (e) => e instanceof PerkError);
    assert.throws(() => db.selectById('t', p, 1), (e) => e instanceof PerkError);
    assert.throws(() => db.createTable(p, [{ name: 'a' }]), (e) => e instanceof PerkError);
  }
  assert.throws(() => db.select('t', { where: "name = 'x'; DROP TABLE t;--" }), (e) => e instanceof PerkError);
  assert.throws(() => db.delete('t', '1=1; DELETE FROM t'), (e) => e instanceof PerkError);
  assert.deepStrictEqual(
    db.select('t').map((r) => r.name),
    ['Yazan', 'Sam', 'Lee']
  );
  db.close();
});

test('select: __proto__ column name is a plain data property', () => {
  const db = new Database(':memory:');
  db.createTable('p', [{ name: '__proto__', type: 'TEXT' }, { name: 'constructor', type: 'TEXT' }]);
  db.insert('p', { ['__proto__']: 'x', constructor: 'y' });
  const [row] = db.select('p');
  assert.strictEqual(Object.getPrototypeOf(row), Object.prototype);
  assert.ok(Object.prototype.hasOwnProperty.call(row, '__proto__'));
  assert.strictEqual(Object.getOwnPropertyDescriptor(row, '__proto__').value, 'x');
  assert.strictEqual(row.constructor, 'y');
  assert.strictEqual({}.x, undefined);
  assert.strictEqual(Object.prototype.polluted, undefined);
  db.close();
});

test('insert via JSON-parsed __proto__ key does not pollute', () => {
  const db = new Database(':memory:');
  db.createTable('p', [{ name: '__proto__', type: 'TEXT' }]);
  db.insert('p', JSON.parse('{"__proto__": "z"}'));
  assert.strictEqual(db.select('p')[0]['__proto__'], 'z');
  assert.strictEqual({}.z, undefined);
  db.close();
});

// ---- selectById ----
test('selectById: happy path, columns, missing -> NOT_FOUND', () => {
  const db = mem();
  users(db);
  assert.deepStrictEqual(db.selectById('t', 'id', 2), { id: 2, name: 'Sam', age: 30 });
  assert.deepStrictEqual(db.selectById('t', 'id', 2, { columns: ['name'] }), { name: 'Sam' });
  assert.deepStrictEqual(db.selectById('t', 'name', 'Lee', null), { id: 3, name: 'Lee', age: 17 });
  assert.throws(() => db.selectById('t', 'id', 999), perkErr('PERK_ERR_NOT_FOUND'));
  assert.throws(() => db.selectById('t', 'id', -1), perkErr('PERK_ERR_NOT_FOUND'));
  assert.throws(() => db.selectById('t', 'id', null), perkErr('PERK_ERR_NOT_FOUND'));
  assert.throws(() => db.selectById('t', 'id', undefined), perkErr('PERK_ERR_NOT_FOUND'));
  db.close();
});

test('selectById: errors', () => {
  const db = mem();
  assert.throws(() => db.selectById('nope', 'id', 1), perkErr('PERK_ERR_SQL'));
  assert.throws(() => db.selectById('t', 'nocol', 1), perkErr('PERK_ERR_SQL'));
  assert.throws(() => db.selectById('t', 'id; x', 1), perkErr('PERK_ERR_BADNAME'));
  assert.throws(() => db.selectById('t', '', 1), perkErr('PERK_ERR_BADNAME'));
  assert.throws(() => db.selectById('', 'id', 1), perkErr('PERK_ERR_BADNAME'));
  assert.throws(() => db.selectById(5, 'id', 1), typeErr());
  assert.throws(() => db.selectById('t', 5, 1), typeErr());
  assert.throws(() => db.selectById('t', undefined, 1), typeErr());
  assert.throws(() => db.selectById('t', 'id', 1, 'x'), typeErr());
  assert.throws(() => db.selectById('t', 'id', 1, { columns: 'a' }), typeErr());
  assert.throws(() => db.selectById(), typeErr());
  db.close();
});

// ---- update ----
test('update: happy path returns count; where-less affects all', () => {
  const db = mem();
  users(db);
  assert.strictEqual(db.update('t', { age: 22 }, 'id = ?', [1]), 1);
  assert.strictEqual(db.selectById('t', 'id', 1).age, 22);
  assert.strictEqual(db.update('t', { age: 50, name: 'Z' }, 'age > ?', [18]), 2);
  assert.strictEqual(db.update('t', { age: 0 }), 3);
  assert.strictEqual(db.update('t', { age: 1 }, null, null), 3);
  assert.deepStrictEqual(db.select('t').map((r) => r.age), [1, 1, 1]);
  assert.strictEqual(db.update('t', { name: null }, 'id = ?', [1]), 1);
  assert.strictEqual(db.selectById('t', 'id', 1).name, null);
  db.close();
});

test('update: no match -> NOT_FOUND; other errors', () => {
  const db = mem();
  assert.throws(() => db.update('t', { age: 1 }), perkErr('PERK_ERR_NOT_FOUND'), 'empty table, no where');
  users(db);
  assert.throws(() => db.update('t', { age: 1 }, 'id = ?', [999]), perkErr('PERK_ERR_NOT_FOUND'));
  assert.throws(() => db.update('nope', { a: 1 }, 'id = ?', [1]), perkErr('PERK_ERR_SQL'));
  assert.throws(() => db.update('t', { nocol: 1 }, 'id = ?', [1]), perkErr('PERK_ERR_SQL'));
  assert.throws(() => db.update('t', { age: 1 }, 'id = ?', []), perkErr('PERK_ERR_MISUSE'));
  assert.throws(() => db.update('t', {}, 'id = ?', [1]), (e) => e instanceof PerkError);
  assert.throws(() => db.update('t', 'age=1', 'id = ?', [1]), typeErr());
  assert.throws(() => db.update('t', null), typeErr());
  assert.throws(() => db.update('t', [1], 'id = 1'), typeErr());
  assert.throws(() => db.update('t', { age: 1 }, 5), typeErr());
  assert.throws(() => db.update('t', { age: 1 }, 'id = ?', 'x'), typeErr());
  assert.throws(() => db.update(null, { age: 1 }), typeErr());
  assert.throws(() => db.update('t', { 'age=1,name': 1 }, 'id = 1'), perkErr('PERK_ERR_BADNAME'));
  assert.deepStrictEqual(db.select('t').map((r) => r.age), [21, 30, 17]);
  db.close();
});

// ---- delete ----
test('delete: happy path, count, where-less deletes all', () => {
  const db = mem();
  users(db);
  assert.strictEqual(db.delete('t', 'id = ?', [1]), 1);
  assert.strictEqual(db.select('t').length, 2);
  assert.strictEqual(db.delete('t', 'age >= ?', [0]), 2);
  users(db);
  assert.strictEqual(db.delete('t'), 3);
  assert.deepStrictEqual(db.select('t'), []);
  users(db);
  assert.strictEqual(db.delete('t', null, undefined), 3);
  db.close();
});

test('delete: no match -> NOT_FOUND; other errors', () => {
  const db = mem();
  assert.throws(() => db.delete('t'), perkErr('PERK_ERR_NOT_FOUND'));
  users(db);
  assert.throws(() => db.delete('t', 'id = ?', [999]), perkErr('PERK_ERR_NOT_FOUND'));
  assert.throws(() => db.delete('nope', 'id = ?', [1]), perkErr('PERK_ERR_SQL'));
  assert.throws(() => db.delete('t', 'id = ?'), perkErr('PERK_ERR_MISUSE'));
  assert.throws(() => db.delete('t', 'id = 1', [1]), perkErr('PERK_ERR_MISUSE'));
  assert.throws(() => db.delete('t x', 'id = 1'), perkErr('PERK_ERR_BADNAME'));
  assert.throws(() => db.delete(5), typeErr());
  assert.throws(() => db.delete('t', 5), typeErr());
  assert.throws(() => db.delete('t', 'id = ?', {}), typeErr());
  assert.throws(() => db.delete('t', 'a\0b'), typeErr('ERR_INVALID_ARG_VALUE'));
  assert.throws(() => db.delete(), typeErr());
  assert.strictEqual(db.select('t').length, 3);
  db.close();
});

// ---- transactions ----
test('transactions: begin/commit persists, begin/rollback restores', () => {
  const f = tmpFile();
  const db = new Database(f);
  db.createTable('t', [{ name: 'k', type: 'INTEGER' }]);
  db.insert('t', { k: 1 });
  assert.strictEqual(db.begin(), undefined);
  db.insert('t', { k: 2 });
  assert.strictEqual(db.select('t').length, 2);
  assert.strictEqual(db.rollback(), undefined);
  assert.strictEqual(db.select('t').length, 1);
  db.begin();
  db.insert('t', { k: 3 });
  db.delete('t', 'k = ?', [1]);
  db.commit();
  assert.deepStrictEqual(db.select('t'), [{ k: 3 }]);
  const other = new Database(f);
  assert.deepStrictEqual(other.select('t'), [{ k: 3 }]);
  other.close();
  db.close();
});

test('transactions: misuse -> error with code', () => {
  const db = mem();
  assert.throws(() => db.commit(), (e) => e instanceof PerkError && /^PERK_ERR_/.test(e.code));
  assert.throws(() => db.rollback(), (e) => e instanceof PerkError && /^PERK_ERR_/.test(e.code));
  db.begin();
  assert.throws(() => db.begin(), (e) => e instanceof PerkError && /^PERK_ERR_/.test(e.code) && e.errno > 0);
  db.rollback();
  assert.throws(() => db.commit(), (e) => e instanceof PerkError);
  db.begin();
  db.commit();
  db.close();
});

test('transactions: second writer sees BUSY or waits, then ok after commit', () => {
  const f = tmpFile();
  const a = new Database(f);
  const b = new Database(f);
  a.createTable('t', [{ name: 'k' }]);
  a.begin();
  assert.throws(() => b.begin(), (e) => e instanceof PerkError && /^PERK_ERR_(BUSY|SQL|MISUSE)$/.test(e.code));
  a.commit();
  b.begin();
  b.rollback();
  a.close();
  b.close();
});

// ---- errmsg ----
test('errmsg returns a string', () => {
  const db = mem();
  assert.strictEqual(typeof db.errmsg(), 'string');
  try {
    db.select('missing');
  } catch (e) {
    assert.strictEqual(typeof e.message, 'string');
    assert.ok(e.message.length > 0);
  }
  assert.strictEqual(typeof db.errmsg(), 'string');
  db.close();
});

// ---- lifecycle ----
test('close(): idempotent, isOpen reflects state', () => {
  const db = mem();
  assert.strictEqual(db.isOpen, true);
  assert.strictEqual(db.close(), undefined);
  assert.strictEqual(db.isOpen, false);
  db.close();
  db.close();
  assert.strictEqual(db.isOpen, false);
});

test('every method after close throws PERK_ERR_CLOSED', () => {
  const db = mem();
  users(db);
  db.close();
  const calls = {
    errmsg: () => db.errmsg(),
    createTable: () => db.createTable('x', [{ name: 'a' }]),
    insert: () => db.insert('t', { name: 'a' }),
    insertArr: () => db.insert('t', ['name'], ['a']),
    select: () => db.select('t'),
    selectById: () => db.selectById('t', 'id', 1),
    update: () => db.update('t', { age: 1 }, 'id = ?', [1]),
    delete: () => db.delete('t', 'id = ?', [1]),
    begin: () => db.begin(),
    commit: () => db.commit(),
    rollback: () => db.rollback(),
  };
  for (const [name, fn] of Object.entries(calls)) {
    assert.throws(fn, (e) => {
      assert.ok(e instanceof PerkError, name);
      assert.strictEqual(e.code, 'PERK_ERR_CLOSED', name);
      assert.strictEqual(e.errno, constants.PERK_ERR_MISUSE, name);
      return true;
    }, name);
  }
});

test('getter that closes the db mid-call does not crash', () => {
  for (const op of ['insert', 'select', 'update', 'delete', 'selectById', 'createTable', 'insertArr']) {
    const db = mem();
    users(db);
    const evil = {
      get x() {
        db.close();
        return 1;
      },
    };
    const args = {
      insert: () => db.insert('t', { name: 'a', get age() { return evil.x; } }),
      insertArr: () => db.insert('t', ['name', 'age'], ['a', { get length() { return 0; } }]),
      select: () => db.select('t', { where: 'age > ?', get args() { evil.x; return [1]; } }),
      update: () => db.update('t', { get age() { return evil.x; } }, 'id = ?', [1]),
      delete: () => db.delete('t', 'id = ?', { get 0() { return evil.x; }, length: 1, __proto__: Array.prototype }),
      selectById: () => db.selectById('t', 'id', 1, { get columns() { evil.x; return ['name']; } }),
      createTable: () => db.createTable('x', [{ get name() { evil.x; return 'a'; } }]),
    };
    try {
      args[op]();
    } catch (e) {
      assert.ok(e instanceof Error, op);
    }
    if (op !== 'insertArr' && op !== 'delete') {
      assert.strictEqual(db.isOpen, false, `${op} getter should have closed the db`);
    }
    db.close();
  }
});

test('getter closing db: call fails with PERK_ERR_CLOSED (not a crash)', () => {
  const db = mem();
  users(db);
  assert.throws(
    () => db.insert('t', { name: 'a', get age() { db.close(); return 1; } }),
    perkErr('PERK_ERR_CLOSED')
  );
  assert.throws(() => db.select('t'), perkErr('PERK_ERR_CLOSED'));
});

test('independent connections do not interfere', () => {
  const a = mem();
  const b = mem();
  a.insert('t', { name: 'A' });
  assert.strictEqual(b.select('t').length, 0);
  a.close();
  b.insert('t', { name: 'B' });
  assert.strictEqual(b.select('t').length, 1);
  b.close();
});

test('PerkError shape', () => {
  const e = new PerkError('m', 'PERK_ERR_SQL', 2);
  assert.ok(e instanceof Error);
  assert.strictEqual(e.name, 'PerkError');
  assert.strictEqual(e.code, 'PERK_ERR_SQL');
  assert.strictEqual(e.errno, 2);
  assert.strictEqual(e.message, 'm');
});

// ---- GC ----
test('GC: unreferenced file db is closed (write lock released)', async () => {
  const f = tmpFile();
  const setup = new Database(f);
  setup.createTable('t', [{ name: 'k' }]);
  setup.close();

  (function leak() {
    const db = new Database(f);
    db.begin();
    db.insert('t', { k: 1 });
    // db goes out of scope with an open write transaction
  })();

  const second = new Database(f);
  let released = false;
  for (let i = 0; i < 30 && !released; i++) {
    await collect(2);
    try {
      second.begin();
      released = true;
    } catch (e) {
      assert.ok(e instanceof PerkError);
    }
  }
  assert.ok(released, 'second connection could not begin(): the first handle was not closed by GC');
  second.rollback();
  assert.deepStrictEqual(second.select('t'), [], 'uncommitted insert was rolled back by close');
  second.close();
});

test('GC: thousands of dropped :memory: connections stay bounded', async () => {
  const before = process.memoryUsage().rss;
  for (let i = 0; i < 5000; i++) {
    const db = new Database(':memory:');
    db.createTable('t', [{ name: 'a', type: 'TEXT' }]);
    db.insert('t', { a: 'x'.repeat(2000) });
    if (i % 500 === 0) await collect(1);
  }
  await collect(5);
  const growthMb = (process.memoryUsage().rss - before) / (1024 * 1024);
  assert.ok(growthMb < 400, `RSS grew by ${growthMb.toFixed(1)} MB`);
});

test('GC: closed-then-collected handle does not double free', async () => {
  for (let i = 0; i < 200; i++) {
    const db = new Database(':memory:');
    db.close();
  }
  await collect(3);
});

// ---- runner ----
(async () => {
  let failed = 0;
  for (const [name, fn] of tests) {
    try {
      await fn();
      console.log(`  ok   ${name}`);
    } catch (err) {
      failed++;
      console.log(`  FAIL ${name}\n       ${err && err.stack ? err.stack.split('\n').slice(0, 4).join('\n       ') : err}`);
    }
  }
  console.log(`${tests.length - failed}/${tests.length} passed`);
  process.exit(failed === 0 ? 0 : 1);
})();
