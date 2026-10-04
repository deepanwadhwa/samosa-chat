import {createRequire} from 'node:module';import fs from 'node:fs';import assert from 'node:assert/strict';
const require=createRequire(import.meta.url),{chromium}=require('/private/tmp/fw6-browser/node_modules/playwright');
const out='/private/tmp/samosa-type-search-browser-final';
const b=await chromium.launch({executablePath:'/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',headless:true});
const p=await b.newPage({viewport:{width:1440,height:1000}});let errors=[];p.on('pageerror',e=>errors.push(e.message));
try{
 await p.goto('http://127.0.0.1:18642');await p.locator('#navJobs').click();await p.getByText('Recent work',{exact:true}).click();
 const row=p.locator('.job-history-row').filter({hasText:'/Users/deepanwadhwa/Documents/Samosa Walkthrough/Document Type Search v4'}).filter({hasText:'find all documents that look like I539 in this folder.'}).first();
 await row.getByRole('button',{name:'View saved run'}).click();await p.locator('.job-decision-results').waitFor();
 await p.getByText('Recent work',{exact:true}).click();await p.getByLabel('Filter files').selectOption('all');
 await p.locator('.job-decision-results').evaluate(el=>el.scrollIntoView({block:'start'}));
 await p.screenshot({path:out+'/checked-files-table-visible.png',fullPage:true});
 await p.setViewportSize({width:390,height:844});await p.locator('.job-decision-results').evaluate(el=>el.scrollIntoView({block:'start'}));
 let rect=await p.locator('.job-decision-results table').boundingBox();assert(rect.x>=0 && rect.x+rect.width<=390);
 const cells=await p.locator('.job-decision-results table thead th').evaluateAll(cs=>cs.map(c=>({text:c.textContent,width:c.getBoundingClientRect().width})));assert(cells.every(c=>c.width>=35));
 await p.screenshot({path:out+'/narrow-table-visible.png',fullPage:true});assert.equal(errors.length,0);
 fs.writeFileSync(out+'/visual-results.json',JSON.stringify({passed:true,viewport:390,table:rect,cells,errors},null,2));
}catch(e){console.log(e.stack);process.exitCode=1;}finally{await b.close();}
