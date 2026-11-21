// Original selection driver's JSON scalar and wall-clock helpers, without Python.
import fs from 'node:fs';
const [mode, key] = process.argv.slice(2);
if (mode === 'now') {
  console.log(Date.now());
} else if (mode === 'field') {
  const object = JSON.parse(fs.readFileSync(0, 'utf8'));
  const value = Object.hasOwn(object, key) ? object[key] : undefined;
  console.log(value == null ? '' : typeof value === 'boolean' ? (value ? 'True' : 'False') : String(value));
} else {
  throw Error('usage: model_selection_checks.mjs field KEY | now');
}
