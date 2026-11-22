// JSON scalar and wall-clock helpers for the model manager tests.
import fs from 'node:fs';
const [mode, ...keys] = process.argv.slice(2);
if (mode === 'now') {
  console.log(Date.now());
} else if (mode === 'field') {
  let value = JSON.parse(fs.readFileSync(0, 'utf8'));
  for (const key of keys) value = value != null && Object.hasOwn(value, key) ? value[key] : undefined;
  console.log(value == null ? '' : typeof value === 'boolean' ? (value ? 'True' : 'False') : String(value));
} else {
  throw Error('usage: model_selection_checks.mjs field KEY [NESTED_KEY ...] | now');
}
