// Opt-in browser acceptance for the generated, complete 20-file Files fixture.
// Supply SAMOSA_PLAYWRIGHT_ROOT for an isolated Playwright installation.
import {createRequire} from 'node:module';
const require=createRequire(import.meta.url);
const {chromium}=require(process.env.SAMOSA_PLAYWRIGHT_ROOT ? process.env.SAMOSA_PLAYWRIGHT_ROOT+'/node_modules/playwright' : 'playwright');
import fs from 'node:fs';
import assert from 'node:assert/strict';
import {assertMultipartAnswer} from './assert_memory_browser.mjs';
const out=process.env.SAMOSA_TEST_EVIDENCE || '/tmp/samosa-memory-browser-'+Date.now();fs.mkdirSync(out,{recursive:true});
const browser=await chromium.launch({executablePath:process.env.SAMOSA_CHROME || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',headless:true});
const page=await browser.newPage({viewport:{width:1440,height:1000}});const evidence=[];
function record(row){evidence.push(row);fs.writeFileSync(out+'/results.json',JSON.stringify(evidence,null,2));console.log(JSON.stringify(row));}
page.on('pageerror',e=>record({error:e.message}));
try{
await page.goto(process.env.SAMOSA_TEST_URL || 'http://127.0.0.1:8642');await page.waitForTimeout(1200);
if(await page.locator('#webConsentNo').isVisible())await page.locator('#webConsentNo').click();
await page.locator('#prompt').focus();
const focus=await page.locator('#prompt').evaluate(el=>({outline:getComputedStyle(el).outlineStyle,shadow:getComputedStyle(el).boxShadow}));
record({stage:'composer_focus',...focus});assert.equal(focus.outline,'none');
await page.screenshot({path:out+'/composer-focus.png',fullPage:true});
await page.locator('#navChutni').click();
const card=page.locator('.chutni-card').filter({has:page.locator('.chutni-name',{hasText:/^Files$/})});
assert.equal(await card.count(),1);assert.equal(await card.getByRole('button',{name:/Ask (?:about this folder|another question)/}).count(),1);
assert(!/unspecified|safety limit|not fully indexed/.test(await card.innerText()));
await page.screenshot({path:out+'/folder-memory.png',fullPage:true});
await card.getByRole('button',{name:/Ask (?:about this folder|another question)/}).click();
if(!process.env.SAMOSA_BROWSER_UI_ONLY){
const question='tell me what is in the Person X file? How many pages in that file? are there files related to person X?';
const started=Date.now();await page.locator('#prompt').fill(question);await page.locator('#send').click();
await page.waitForFunction(()=>!document.body.classList.contains('generating'),null,{timeout:900000});
const node=page.locator('.message.assistant').last();const answer=await node.locator('.response').innerText();const error=await node.locator('.error-note').innerText();
record({question,answer,error,seconds:(Date.now()-started)/1000});
// Allow the independent five-second health poll to catch up with stream closure.
await page.waitForFunction(()=>!document.querySelector('#modelStatus').textContent.includes('is generating'),null,{timeout:15000});
await page.screenshot({path:out+'/multipart-answer.png',fullPage:true});
assertMultipartAnswer({answer,error});
}
assert(!evidence.some(row=>row.error),'Browser reported an error');
record({stage:'passed',ui_only:!!process.env.SAMOSA_BROWSER_UI_ONLY});
}catch(e){record({stage:'failed',error:e.stack});process.exitCode=1;}finally{await browser.close();}
