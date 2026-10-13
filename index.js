'use strict';

// Loads the compiled N-API addon that `npm install` (node-gyp rebuild) builds.
const path = require('path');

const addonPath = path.join(__dirname, 'build', 'Release', 'perkdb.node');

let addon;
try {
  addon = require(addonPath);
} catch (err) {
  if (err && err.code === 'MODULE_NOT_FOUND') {
    throw new Error(
      `perkdb: native addon not found at ${addonPath}. ` +
        'Build it with `npm install` (or `npm run build`); this needs Node >= 18.17, ' +
        'a C/C++ toolchain and Python for node-gyp. Note that `make clean` deletes build/.',
      { cause: err }
    );
  }
  throw err;
}

module.exports = addon;
