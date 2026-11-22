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
const installedRelease=process.argv[2]==='--installed-release';
const installedFixture=process.argv[2]==='--installed';
const installed=installedRelease||installedFixture;
const actual=process.argv[2]==='--actual'||installedRelease;
const realModel=process.env.SAMOSA_TEST_REAL_QWEN_MODEL??'/private/tmp/samosa-life07h-real.2wwUok/model';
const model=installed?path.join(test,'models','qwen'):actual?realModel:path.join(test,'model');
const release=installedRelease?fs.realpathSync(process.argv[3]):path.join(test,'releases','1a2b3c4d5e6a7b8c');
const engine=installed?path.join(release,'bin','qwen36b'):actual?path.join(path.dirname(model),'qwen36b'):fixture;
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
async function request(route,method='GET',data,timeout=3000) {
  const headers={'Content-Type':'application/json'};
  if(token) headers['X-Samosa-Token']=token;
  const response=await fetch(base+route,{method,headers,body:JSON.stringify(data),signal:AbortSignal.timeout(timeout)});
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
function startGateway() {
  const env={...process.env,SAMOSA_HOME:test,SAMOSA_BIND:'127.0.0.1',
    SAMOSA_PORT:String(port),SAMOSA_BACKEND_PORT:String(backendPort),
    SAMOSA_BONSAI_SERVER:fixture,SAMOSA_BONSAI_MODEL:goodGguf,SAMOSA_ORNITH_MODEL:badGguf,
    SAMOSA_BONSAI_MMPROJ:path.join(test,'missing-mmproj'),SAMOSA_CONTEXT_TOKENS:'512',OMP_NUM_THREADS:'2'};
  if(!actual) env.SAMOSA_PRIMARY_GATEWAY_FIXTURE='1';
  if(installed) {
    delete env.SAMOSA_RELEASE_DIR;
    delete env.SAMOSA_PRIMARY_PINS;
  } else {
    Object.assign(env,{SAMOSA_PRIMARY_PINS:pinsFile,SAMOSA_QWEN_ENGINE:engine,
      SAMOSA_QWEN_MODEL:model,SAMOSA_TOKENIZER:tokenizer});
  }
  const fd=fs.openSync(log,'a');
  child=installed?spawn(path.join(test,'current','bin','samosa'),['serve','--foreground'],{env,stdio:['ignore',fd,fd]}):
    spawn(gateway,[],{env,stdio:['ignore',fd,fd]});
  fs.closeSync(fd);
  finished=once(child,'exit');
}

async function controlPlane() {
  await poll(async()=>((await request('/healthz')).code===200),5000,'control plane startup');
  token=fs.readFileSync(path.join(test,'run','ui-token'),'utf8').trim();
}

function record(asset,name=asset) {
  return [quote(asset),fs.statSync(asset).size,name].join('\t')+'\n';
}

async function stop() {
  if(child && child.exitCode===null&&!child.signalCode) child.kill('SIGTERM');
  if(finished) await finished;
}
process.on('SIGTERM',()=>{void stop().then(()=>process.exit(143));});

try {
  if(installedRelease){
    fs.mkdirSync(model,{recursive:true});
    for(const name of names)execFileSync('cp',['-c','-p',path.join(realModel,name),model]);
  } else if(!actual){
    fs.mkdirSync(model,{recursive:true});
    for(const name of names)fs.writeFileSync(path.join(model,name),'fixture');
    fs.writeFileSync(goodGguf,'good gguf');
    fs.writeFileSync(badGguf,'bad gguf');
  }
  const catalogPath=path.join(release,'models.json');
  const receipt=path.join(release,'primary-qwen-pins.tsv');
  if(installedFixture){
    fs.mkdirSync(path.join(release,'bin'),{recursive:true});
    for(const [src,name] of [[fixture,'qwen36b'],[gateway,'samosa-gateway'],['dist/samosa','samosa']]){
      fs.copyFileSync(src,path.join(release,'bin',name));
      fs.chmodSync(path.join(release,'bin',name),0o755);
    }
    const catalog=JSON.parse(fs.readFileSync('assets/models.json','utf8'));
    const qwen=catalog.models.find(model=>model.id==='qwen');
    for(const artifact of qwen.artifacts) {
      const asset=path.join(model,artifact.name);
      artifact.bytes=fs.statSync(asset).size;
      artifact.sha256=quote(asset);
    }
    fs.writeFileSync(catalogPath,JSON.stringify(catalog),{mode:0o600});
    fs.writeFileSync(receipt,record(catalogPath,'models.json')+record(engine,'bin/qwen36b'),{mode:0o600});
  } else if(!installedRelease){
    const assets=[engine,...names.map(name=>path.join(model,name))];
    if(actual) assets.push('/opt/homebrew/opt/libomp/lib/libomp.dylib');
    else assets.push(goodGguf,badGguf);
    const recorded=actual?new Map(fs.readFileSync(path.join(module,'docs/qualification/life07h-qwen-sources/actual-artifacts.sha256'),'utf8').trim().split('\n').map(line=>[line.slice(66),line.slice(0,64)])):new Map();
    fs.writeFileSync(pinsFile,assets.map(asset=>[recorded.get(asset)??quote(asset),fs.statSync(asset).size,asset].join('\t')).join('\n')+'\n',{mode:0o600});
  }
  if(installed) fs.symlinkSync(release,path.join(test,'current'),'dir');
  fs.writeFileSync(path.join(test,'model-backend'),'qwen\n');
  startGateway();
  await controlPlane();
  await ready('qwen');
  const first=Number(fs.readFileSync(marker,'utf8'));
  assert(first>1);
  assert.ok(fs.existsSync(path.join(test,'run','primary-activation','activation.sqlite3')));
  if(installedRelease){
    const result=await request('/v1/chat/completions','POST',{messages:[{role:'user',content:'Say hello.'}],
      max_tokens:4,thinking:false,stream:false},120000);
    assert.equal(result.code,200,JSON.stringify(result.body));
    assert.ok(result.body.choices[0].message.content.trim());
  } else if(installedFixture){
    await select('bonsai','selected');
    await ready('bonsai');
    assert.throws(()=>process.kill(first,0),{code:'ESRCH'});
    await select('qwen','selected');
    await ready('qwen');
  } else if(!actual){
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
  if(installedFixture){
    const catalog=fs.readFileSync(catalogPath);
    for(const failure of ['catalog','receipt']){
      if(failure==='catalog')fs.appendFileSync(catalogPath,'\n');
      else {
        fs.writeFileSync(catalogPath,catalog);
        fs.unlinkSync(receipt);
      }
      const logStart=fs.statSync(log).size;
      startGateway();
      await controlPlane();
      assert.equal((await request('/healthz')).body.ready,false);
      assert.equal(fs.existsSync(marker),false);
      assert.match(fs.readFileSync(log).subarray(logStart).toString(),/primary qwen activation: invalid or missing artifact pins/);
      await stop();
    }
  }
  console.log(installedRelease?'PASS installed launcher qualified real Qwen and reaped its worker':installedFixture?
    'PASS installed Qwen pins, legacy selection, Qwen reselection and refusal of changed or missing release metadata':actual?
    'PASS Gateway completed real Qwen qualification and reaped its primary worker':
    'PASS Gateway qualification, Bonsai selection, failed generation rollback, stale input refusal and cleanup');
} catch(err) {
  await stop();
  if(fs.existsSync(log)) process.stderr.write(fs.readFileSync(log,'utf8'));
  throw err;
} finally {
  fs.rmSync(test,{recursive:true,force:true});
}
