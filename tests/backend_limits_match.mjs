// Preserve the original tier-ladder/comment stripping comparison without Python.
import fs from 'node:fs';
import assert from 'node:assert/strict';
const [gateway, test] = process.argv.slice(2);
function tiers(filename) {
  const source = fs.readFileSync(filename, 'utf8').replace(/\/\*[\s\S]*?\*\//g, ' ').replace(/\/\/[^\n]*/g, ' ');
  const body = source.match(/if \(gb <= 0\)([\s\S]*?)\*out_ctx/);
  if (!body) throw Error(`could not find the tier ladder in ${filename}`);
  const cap = source.match(/threads > (\d+)\) threads = (\d+)/);
  return [body[1].match(/\d+(?:\.\d+)?/g) ?? [], cap ? [cap[1], cap[2]] : null];
}
const a = tiers(gateway), b = tiers(test);
assert.deepEqual(a, b, `test_backend_limits_match.sh: the mirrored tier table has drifted (${gateway}, ${test})`);
console.log('test_backend_limits_match.sh: PASS');
