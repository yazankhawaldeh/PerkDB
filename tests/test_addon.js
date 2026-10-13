'use strict';
// Node smoke test for the N-API addon. Uses only Node built-ins.
// Run with: node tests/test_addon.js  (after `npm install` / `npm run build`)

const assert = require('node:assert');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const root = path.resolve(__dirname, '..');
const results = [];

function check(name, fn) {
  try {
    fn();
    results.push([name, null]);
    console.log(`  ok   ${name}`);
  } catch (err) {
    results.push([name, err]);
    console.log(`  FAIL ${name}\n       ${err && err.message}`);
  }
}

const addon = require(root);
const pkg = JSON.parse(fs.readFileSync(path.join(root, 'package.json'), 'utf8'));

check('loaded === true', () => assert.strictEqual(addon.loaded, true));

check('version() is semver-like and matches package.json', () => {
  const v = addon.version();
  assert.strictEqual(typeof v, 'string');
  assert.match(v, /^\d+\.\d+\.\d+/);
  assert.strictEqual(v, pkg.version);
});

check('sqliteVersion() looks like 3.x.y', () => {
  const v = addon.sqliteVersion();
  assert.strictEqual(typeof v, 'string');
  assert.match(v, /^3\.\d+\.\d+/);
});

check('unexpected arguments do not crash', () => {
  const weird = [undefined, null, 0, -1, NaN, 'x'.repeat(100000), {}, [], () => {}, Symbol('s'), 10n];
  for (const a of weird) {
    for (const fn of [addon.version, addon.sqliteVersion]) {
      try {
        fn(a, a, a);
      } catch (e) {
        assert.ok(e instanceof Error, 'only regular errors may be thrown');
      }
    }
  }
  assert.strictEqual(addon.version(1, 2, 3), pkg.version);
});

check('repeated calls are stable', () => {
  for (let i = 0; i < 10000; i++) {
    addon.version();
    addon.sqliteVersion();
  }
});

check('missing addon gives a clear error', () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'perkdb-noaddon-'));
  const copy = path.join(dir, 'index.js');
  try {
    fs.copyFileSync(path.join(root, 'index.js'), copy);
    assert.ok(!fs.existsSync(path.join(dir, 'build')));
    assert.throws(
      () => require(copy),
      (err) => {
        assert.ok(err instanceof Error);
        assert.match(err.message, /npm install/);
        assert.match(err.message, /npm run build/);
        return true;
      }
    );
  } finally {
    delete require.cache[copy];
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

const failed = results.filter(([, e]) => e);
if (failed.length) {
  console.log(`FAIL: ${failed.length}/${results.length} addon checks failed`);
  process.exit(1);
}
console.log(`PASS: ${results.length}/${results.length} addon checks passed`);
