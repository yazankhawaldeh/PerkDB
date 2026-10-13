'use strict';

// Loads the compiled N-API addon that `npm install` (node-gyp rebuild) builds
// and exports the PerkDB JavaScript API.
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

/**
 * Thrown when a PerkDB C function returns an error (or a method is called
 * after close()).
 * code: the enum name, e.g. 'PERK_ERR_NOT_FOUND', or 'PERK_ERR_CLOSED'
 * errno: the numeric PerkErr value (PERK_ERR_MISUSE for 'PERK_ERR_CLOSED')
 */
class PerkError extends Error {
  /**
   * @param {string} message description, from perk_errmsg()
   * @param {string} code enum name
   * @param {number} errno numeric PerkErr value
   */
  constructor(message, code, errno) {
    super(message);
    this.name = 'PerkError';
    this.code = code;
    this.errno = errno;
  }
}

addon.setErrorClass(PerkError);

module.exports = {
  Database: addon.Database,
  PerkError,
  constants: addon.constants,
  openErrmsg: addon.openErrmsg,
  version: addon.version,
  sqliteVersion: addon.sqliteVersion,
  loaded: addon.loaded,
};
