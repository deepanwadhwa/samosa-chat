// Runs the compiled Gateway with the extracted native lifecycle owner.
import assert from 'node:assert/strict';
import {spawn,execFileSync} from 'node:child_process';
import {once} from 'node:events';
import fs from 'node:fs';
import net from 'node:net';
import path from 'node:path';
import os from 'node:os';

const root=process.cwd();
const module=path.join(root,'components/samosa-inference');
const gateway=path.resolve(process.env.SAMOSA_COMPILED_GATEWAY ?? 'build/samosa-gateway');
const fixture=path.join(module,'build/test-primary-host');
const test=fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(),'primary-gateway-')));
const actual=process.argv[2]==='--actual';
const model=actual?'/private/tmp/samosa-life07h-real.2wwUok/model':path.join(test,'model');
const engine=actual?path.join(path.dirname(model),'qwen36b'):fixture;
const pinsFile=path.join(test,'primary.tsv');
const names=['manifest.json','config.json','experts.bin','resident.safetensors','tokenizer_qwen36.json','generation_config.json'];
const tokenizer=path.join(model,names[4]);
const goodGguf=path.join(test,'bonsai.gguf');
const badGguf=path.join(test,'ornith.gguf');
const port=await freePort();
const backendPort=await freePort();
const log=path.join(test,'gateway.log');
const marker=path.join(test,'run','backend.pid');
const quote=asset=>execFileSync('shasum',['-a','256',asset],{encoding:'utf8'}).slice(0,64);
let child, finished;

async function freePort() {
  const server=net.createServer();
  server.listen(0,'127.0.0.1');
  await once(server,'listening');
  const port=server.address().port;
  await new Promise(resolve=>server.close(resolve));
  return port;
}

async function poll(action,timeout,message) {
  const end=Date.now()+timeout;
  while(Date.now()<end) {
    if(child?.exitCode!=null || child?.signalCode) throw Error('Gateway exited early');
    const value=await action().catch(()=>null);
    if(value) return value;
    await new Promise(resolve=>setTimeout(resolve,80));
  }
  throw Error(message);
}

const base='http://127.0.0.1:'+port;
let token;
async function request(route,method='GET',data) {
  const headers={'Content-Type':'application/json'};
  if(token) headers['X-Samosa-Token']=token;
  const response=await fetch(base+route,{method,headers,body:JSON.stringify(data),signal:AbortSignal.timeout(3000)});
  return {code:response.status,body:await response.json()};
}
async function ready(backend) {
  return poll(async()=>{
    const {body}=await request('/healthz');
    return body.ready&&body.backend===backend?body:null;
  },actual?180000:15000,backend+' readiness');
}
async function select(backend,state) {
  const accepted=await request('/v1/backends/select','POST',{backend});
  assert.equal(accepted.code,202,JSON.stringify(accepted.body));
  assert.ok(accepted.body.job_id);
  const result=await poll(async()=>{
    const {body}=await request(accepted.body.status_url);
    return ['selected','failed'].includes(body.state)?body:null;
  },15000,backend+' terminal selection');
  assert.equal(result.state,state,JSON.stringify(result));
}
async function stop() {
  if(child && child.exitCode===null&&!child.signalCode) child.kill('SIGTERM');
  if(finished) await finished;
}
process.on('SIGTERM',()=>{void stop().then(()=>process.exit(143));});

try {
  if(!actual){
    fs.mkdirSync(model);
    for(const name of names)fs.writeFileSync(path.join(model,name),'fixture');
    fs.writeFileSync(goodGguf,'good gguf');
    fs.writeFileSync(badGguf,'bad gguf');
  }
  const assets=[engine,...names.map(name=>path.join(model,name))];
  if(actual)assets.push('/opt/homebrew/opt/libomp/lib/libomp.dylib');
  else assets.push(goodGguf,badGguf);
  const recorded=actual?new Map(fs.readFileSync(path.join(module,'docs/qualification/life07h-qwen-sources/actual-artifacts.sha256'),'utf8').trim().split('\n').map(line=>[line.slice(66),line.slice(0,64)])):new Map();
  fs.writeFileSync(pinsFile,assets.map(asset=>[recorded.get(asset)??quote(asset),fs.statSync(asset).size,asset].join('\t')).join('\n')+'\n',{mode:0o600});
  fs.writeFileSync(path.join(test,'model-backend'),'qwen\n');
  const fd=fs.openSync(log,'w');
  child=spawn(gateway,[],{env:{...process.env,SAMOSA_HOME:test,SAMOSA_BIND:'127.0.0.1',
    SAMOSA_PORT:String(port),SAMOSA_BACKEND_PORT:String(backendPort),SAMOSA_PRIMARY_PINS:pinsFile,
    SAMOSA_QWEN_ENGINE:engine,SAMOSA_QWEN_MODEL:model,SAMOSA_TOKENIZER:tokenizer,
    SAMOSA_BONSAI_SERVER:fixture,SAMOSA_BONSAI_MODEL:goodGguf,SAMOSA_ORNITH_MODEL:badGguf,
    SAMOSA_BONSAI_MMPROJ:path.join(test,'missing-mmproj'),SAMOSA_CONTEXT_TOKENS:'512',OMP_NUM_THREADS:'2',
    SAMOSA_PRIMARY_GATEWAY_FIXTURE:actual?'':'1'},stdio:['ignore',fd,fd]});
  fs.closeSync(fd);
  finished=once(child,'exit');
  await poll(async()=>((await request('/healthz')).code===200),5000,'control plane startup');
  token=fs.readFileSync(path.join(test,'run','ui-token'),'utf8').trim();
  await ready('qwen');
  const first=Number(fs.readFileSync(marker,'utf8'));
  assert(first>1);
  assert.ok(fs.existsSync(path.join(test,'run','primary-activation','activation.sqlite3')));
  if(!actual){
    await select('bonsai','selected');
    await ready('bonsai');
    const second=Number(fs.readFileSync(marker,'utf8'));
    assert(second>1&&second!==first);
    await select('ornith','failed');
    await ready('bonsai');
    assert.equal(fs.readFileSync(path.join(test,'model-backend'),'utf8').trim(),'bonsai');
    assert.match(fs.readFileSync(log,'utf8'),/ornith activation: provider_qualification_failed/);
    fs.chmodSync(goodGguf,0o600);
    const stale=await request('/healthz');
    assert.equal(stale.body.ready,false);
  }
  const worker=Number(fs.readFileSync(marker,'utf8'));
  await stop();
  assert.equal(fs.existsSync(marker),false);
  assert.throws(()=>process.kill(worker,0),{code:'ESRCH'});
  console.log(actual?'PASS Gateway completed real Qwen qualification and reaped its primary worker':'PASS Gateway qualification, Bonsai selection, failed generation rollback, stale input refusal and cleanup');
} catch(err) {
  await stop();
  if(fs.existsSync(log)) process.stderr.write(fs.readFileSync(log,'utf8'));
  throw err;
} finally {
  fs.rmSync(test,{recursive:true,force:true});
}
