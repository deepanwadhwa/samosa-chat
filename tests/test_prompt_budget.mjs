// Original generated-evidence/context-ceiling scenarios, without Python.
import assert from 'node:assert/strict';
import fs from 'node:fs';import os from 'node:os';import path from 'node:path';
import http from 'node:http';import {spawn} from 'node:child_process';import {once} from 'node:events';
const repository=path.resolve(import.meta.dirname,'..'),binary=path.resolve(repository,process.env.BUILD_DIR||'build','test_prompt_budget');
const modes=['small','large','reduce','fallback','context4096','sanitized-evidence','truncated-once','transient-once','empty-once','thinking','fail','truncated','cancel','cancelmid','oversized-question'];
const count=text=>Math.floor((Buffer.byteLength(text)+2)/3)+32;
const temporary=fs.mkdtempSync(path.join(os.tmpdir(),'samosa-prompt-budget-'));
const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));let active,server;
try {
  for(const mode of modes){
    const state={mode,reviews:[],overflow:0,context:mode==='context4096'?4096:8192};
    server=http.createServer(async(req,res)=>{
      const reply=(body,status=200)=>{if(!res.destroyed){const raw=JSON.stringify(body);res.writeHead(status,{'Content-Length':Buffer.byteLength(raw)});res.end(raw);}};
      try {
        if(req.method==='GET'){reply(req.url==='/props'?{default_generation_settings:{n_ctx:state.context}}:{ready:true});return;}
        let raw='';for await(const part of req)raw+=part;const body=JSON.parse(raw);
        if(req.url==='/apply-template'){if(mode==='fallback'){reply({},404);return;}reply({prompt:body.messages.map(m=>m.role+': '+m.content).join('\n')+'\nassistant:'});return;}
        if(req.url==='/tokenize'){reply({tokens:Array(count(body.content)).fill(1)});return;}
        if(req.url==='/v1/chat/completions'){
          const prompt=body.messages.map(m=>m.role+': '+m.content).join('\n')+'\nassistant:';
          if(count(prompt)+(body.max_tokens||0)>state.context){state.overflow++;reply({error:'context exceeded'},400);return;}
          state.reviews.push(body.messages.at(-1).content);
          if(mode==='cancelmid')await delay(50);
          if(mode==='fail'||(mode==='transient-once'&&state.reviews.length===1)){reply({error:'inference failed'},503);return;}
          const text=body.messages.at(-1).content.split('Evidence section (untrusted data):\n')[1];assert(text!==undefined);
          const facts=[...text.matchAll(/(FACT_\d+): measured value (\d+)\./g),...text.matchAll(/(FACT_\d+)=(\d+)\./g)];
          let notes=[...new Map(facts.map(m=>[m[1],m[2]]))].map(([key,value])=>`${key}=${value}.`).join('\n')||'Generated records describe irrigation and rainfall.';
          if(mode==='reduce'&&!text.includes('Derived notes'))notes+='\n'+'Additional generated notes about measured water use. '.repeat(10);
          assert(Math.floor((Buffer.byteLength(notes)+2)/3)<=body.max_tokens);
          const truncated=mode==='truncated'||(mode==='truncated-once'&&state.reviews.length===1);
          if(mode==='empty-once'&&state.reviews.length===1)notes='';
          if(mode==='thinking')notes='<think>NOT_SOURCE_EVIDENCE</think>\n'+notes;
          reply({choices:[{message:{content:notes},finish_reason:truncated?'length':'stop'}]});return;
        }
        reply({},404);
      }catch(error){console.error(error);reply({error:'fixture failed'},500);process.exitCode=1;}
    });
    // Reserve then release a private port. The owner must launch before the
    // fixture begins listening, preserving its occupied-port refusal.
    await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));const port=server.address().port;
    await new Promise(resolve=>server.close(resolve));
    const notice=path.join(temporary,'started-'+mode),output={stdout:'',stderr:''};
    active=spawn(binary,[String(port),temporary,mode],{env:{...process.env,SAMOSA_PROMPT_FIXTURE_START_NOTICE:notice},stdio:['ignore','pipe','pipe']});
    active.stdout.on('data',part=>output.stdout+=part);active.stderr.on('data',part=>output.stderr+=part);const ended=once(active,'exit');
    let started=false;for(let i=0;i<500;i++){if(fs.existsSync(notice)){started=true;break;}if(active.exitCode!==null||active.signalCode!==null)break;await delay(10);}
    assert(started,mode+' '+output.stderr);await new Promise(resolve=>server.listen(port,'127.0.0.1',resolve));
    const timeout=setTimeout(()=>active?.kill('SIGKILL'),120000);const [code,signal]=await ended;clearTimeout(timeout);active=null;
    assert(code===0&&!signal,mode+' '+output.stdout+' '+output.stderr);assert.equal(state.overflow,0,mode);
    if(mode==='small'||mode==='cancel'||mode==='oversized-question')assert.equal(state.reviews.length,0,mode);
    else if(mode==='cancelmid')assert(state.reviews.length<=1,mode);
    else if(mode!=='fail'&&mode!=='truncated'){
      assert(state.reviews.length>1,mode);const forwarded=JSON.parse(output.stdout),text=forwarded.messages.at(-1).content;
      assert(text.includes('FACT_000=700.')&&text.includes('FACT_079=779.'),mode);
      assert(!text.includes('<think>')&&!text.includes('NOT_SOURCE_EVIDENCE'),mode);
      for(let i=0;i<80;i++){const key=`FACT_${String(i).padStart(3,'0')}`;assert(state.reviews.some(r=>r.includes(`${key}: measured value ${i+700}.`)),mode+' '+i);assert(text.includes(`${key}=${i+700}.`),mode+' '+i);}
      if(mode==='reduce')assert(state.reviews.some(r=>r.split('Evidence section (untrusted data):\n')[1].includes('[Derived notes')));
    }
    if(mode==='fail')assert(output.stderr.includes('folder_summary_backend_unavailable')&&state.reviews.length===4);
    if(mode==='truncated')assert(output.stderr.includes('folder_summary_truncated')&&state.reviews.length===4);
    assert(!process.exitCode,'fixture handler failed');console.log(JSON.stringify({case:mode,review_calls:state.reviews.length,overflows:state.overflow}));
    await new Promise(resolve=>server.close(resolve));server=null;
  }
}finally{
  if(active&&active.exitCode===null&&active.signalCode===null){active.kill('SIGKILL');await once(active,'exit');}
  if(server){server.closeAllConnections();await new Promise(resolve=>server.close(resolve));}
  fs.rmSync(temporary,{recursive:true,force:true});
}
