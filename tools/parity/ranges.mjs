// Dump the browser rig's parameter tables (web/js/graph/types.js) as JSON: node ranges.mjs RIG_DIR
import path from 'node:path';
import { pathToFileURL } from 'node:url';
const { TYPES } = await import(pathToFileURL(path.join(process.argv[2], 'web/js/graph/types.js')).href);
const out = {};
for (const [type, t] of Object.entries(TYPES)) if (typeof t.params === 'object') out[type] = t.params;
console.log(JSON.stringify(out));
