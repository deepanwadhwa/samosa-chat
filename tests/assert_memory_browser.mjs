// Shared acceptance assertions for live and preserved generated-fixture answers.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {pathToFileURL} from 'node:url';

export function assertMultipartAnswer({answer,error}) {
  assert(!error);
  assert(!/<\/?think>|I should answer/.test(answer));
  assert(/Person X records\.pdf[^\n]{0,200}\b(?:40|forty)\s*pages?/i.test(answer),
    'Missing 40-page reader total for the records PDF');
  assert(/Person X scan\.pdf[^\n]{0,200}\b(?:1|one)\s*page\b/i.test(answer),
    'Missing one-page reader total for the scanned PDF');
  assert(/12|twelve/i.test(answer));
  assert(/multiple|several|more than one|not a single|not one|ambiguous|two PDFs|2 PDFs|PDF files\s*\(2\)/i.test(answer),
    'Ambiguous reference was not explained');
  assert(!/other files.*all.*Person Y/i.test(answer));
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const rows = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
  const answer = rows.find(row => row.answer !== undefined);
  assert(answer, 'No recorded browser answer');
  assertMultipartAnswer(answer);
  console.log(JSON.stringify({stage:'answer_assertions_passed',source:process.argv[2],
    seconds:answer.seconds,original_run_status:rows.at(-1).stage}));
}
