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
const installedBackend=new Map([['--installed-ornith','ornith'],['--installed-maple','maple'],
  ['--installed-bonsai','bonsai']]).get(process.argv[2]);
const installed=installedRelease||installedFixture||Boolean(installedBackend);
const pinnedBackend=new Map([['--actual-ornith','ornith'],['--actual-maple','maple'],
  ['--actual-bonsai','bonsai']]).get(process.argv[2]);
const nativeBackend=installedBackend??pinnedBackend;
const actual=process.argv[2]==='--actual'||installedRelease||Boolean(nativeBackend);
const selectedBackend=nativeBackend??'qwen';
const nativeEnv=`SAMOSA_TEST_REAL_${selectedBackend.toUpperCase()}`;
const nativeEngine=pinnedBackend?fs.realpathSync(process.env[`${nativeEnv}_ENGINE`]):fixture;
const nativeModel=nativeBackend?fs.realpathSync(process.env[`${nativeEnv}_MODEL`]):null;
const nativeAliases={ornith:'ornith-1.0-9b',bonsai:'bonsai-27b-1bit',maple:'deepgrove-maple-preview'};
const nativeArtifacts=nativeBackend?JSON.parse(fs.readFileSync('assets/models.json','utf8')).models
  .find(model=>model.id===nativeBackend).artifacts:[];
const nativeArtifactPath=artifact=>nativeBackend==='ornith'?nativeModel:path.join(nativeModel,artifact.name);
const bonsaiTextOnly=installedBackend==='bonsai'&&process.env.SAMOSA_TEST_BONSAI_TEXT_ONLY==='1';
const realModel=process.env.SAMOSA_TEST_REAL_QWEN_MODEL??'/private/tmp/samosa-life07h-real.2wwUok/model';
const visualModel=installedRelease&&process.env.SAMOSA_TEST_REAL_MOLMO2_MODEL;
const visualEngine=installedRelease&&process.env.SAMOSA_TEST_REAL_MOLMO2_ENGINE;
const visionpsyModel=(installedRelease||installedBackend==='maple')&&process.env.SAMOSA_TEST_REAL_VISIONPSY_MODEL;
const visionpsyEngine=visionpsyModel&&process.env.SAMOSA_TEST_REAL_VISIONPSY_ENGINE;
const model=installed?path.join(test,'models','qwen'):actual?realModel:path.join(test,'model');
const release=installedRelease||installedBackend?fs.realpathSync(process.argv[3]):path.join(test,'releases','1a2b3c4d5e6a7b8c');
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
  if(installedBackend) {
    for(const name of ['SAMOSA_BONSAI_SERVER','SAMOSA_BONSAI_MODEL','SAMOSA_BONSAI_MMPROJ',
      'SAMOSA_ORNITH_MODEL','SAMOSA_MAPLE_ENGINE','SAMOSA_MAPLE_MODEL','SAMOSA_LLAMA_CTX','SAMOSA_LLAMA_THREADS'])
      delete env[name];
  } else if(pinnedBackend) {
    env.SAMOSA_LLAMA_CTX=pinnedBackend==='ornith'?'1024':'2048';
    env.SAMOSA_LLAMA_THREADS='2';
    env.SAMOSA_MODELS_CATALOG=path.join(root,'assets','models.json');
    if(pinnedBackend==='maple') Object.assign(env,{SAMOSA_MAPLE_ENGINE:nativeEngine,SAMOSA_MAPLE_MODEL:nativeModel});
    if(pinnedBackend==='ornith') Object.assign(env,{SAMOSA_BONSAI_SERVER:nativeEngine,SAMOSA_ORNITH_MODEL:nativeModel});
    if(pinnedBackend==='bonsai') {
      const [weights,projector]=nativeArtifacts.map(nativeArtifactPath);
      Object.assign(env,{SAMOSA_BONSAI_SERVER:nativeEngine,SAMOSA_BONSAI_MODEL:weights,SAMOSA_BONSAI_MMPROJ:projector});
    }
  }
  if(visualModel||visualEngine) {
    assert.ok(visualModel,'Set the native Molmo2 model path');
    env.SAMOSA_MOLMO2_MODEL=visualModel;
    delete env.SAMOSA_MOLMO2_ENGINE;
    if(visualEngine) env.SAMOSA_MOLMO2_ENGINE=visualEngine;
    env.SAMOSA_CONTEXT_TOKENS='2048';
  }
  if(visionpsyModel||visionpsyEngine) {
    assert.ok(visionpsyModel,'Set the native VisionPsy model path');
    delete env.SAMOSA_VISIONPSY_MODEL;
    delete env.SAMOSA_VISIONPSY_ENGINE;
    if(visionpsyEngine) env.SAMOSA_VISIONPSY_ENGINE=visionpsyEngine;
    if(installedRelease) env.SAMOSA_CONTEXT_TOKENS='2048';
  }
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

async function uploadLogo() {
  const upload=await fetch(base+'/v1/attachments',{
    method:'POST',
    headers:{'X-Samosa-Token':token,'Content-Type':'image/png','X-Filename':'logo.png'},
    body:fs.readFileSync('assets/samosa-chat.png'),
    signal:AbortSignal.timeout(20000)
  });
  assert.equal(upload.status,201);
  const attachment=await upload.json();
  assert.match(attachment.id,/^[a-f0-9]{64}$/);
  return attachment.id;
}

async function qualifySpecialist(previousWorker, molmo) {
  const health=(await request('/healthz')).body;
  assert.equal(health.vision.molmo2_model_ready,molmo);
  const trace=await request('/v1/developer/trace','PUT',{enabled:true});
  assert.equal(trace.code,200,JSON.stringify(trace.body));
  const chat={
    messages:[{role:'user',content:'Describe the subject and main colors shown in this image.'}],
    attachment_ids:[await uploadLogo()],analysis_depth:'fast',max_tokens:installedRelease?12:512,
    thinking:false,stream:false
  };
  const result=await request('/v1/chat/completions','POST',chat,660000);
  assert.equal(result.code,200,JSON.stringify(result.body));
  await ready(selectedBackend);
  const resumed=Number(fs.readFileSync(marker,'utf8'));
  if(molmo) {
    assert.ok(resumed>1&&resumed!==previousWorker);
    assert.throws(()=>process.kill(previousWorker,0),{code:'ESRCH'});
  } else assert.equal(resumed,previousWorker);
  assert.equal((await request('/v1/backends')).body.active,selectedBackend);

  const events=fs.readFileSync(trace.body.path,'utf8').trim().split('\n').map(JSON.parse);
  const observations=events.filter(({event})=>event==='vision_evidence_observation');
  if(molmo) assert.ok(observations.some(({fields})=>fields?.component==='molmo2_4b'&&fields.bytes>0));
  else {
    const attempts=events.filter(({event})=>event==='visionpsy_complete');
    assert.equal(attempts.length,2);
    assert.equal(attempts[0].fields.observation_chars,0);
    assert.ok(attempts[1].fields.observation_chars>0);
    assert.ok(attempts[1].fields.max_side_len<attempts[0].fields.max_side_len);
    assert.equal(observations.length,1);
    assert.equal(observations[0].fields.component,'visionpsy_nano_460m');
    assert.ok(observations[0].fields.bytes>0);
  }
  const started=molmo?'molmo2_ready':'visionpsy_process_started';
  const workers=events.filter(({event})=>event===started).map(({fields})=>fields.pid);
  assert.equal(workers.length,molmo?1:2);
  for(const pid of workers) {
    assert.ok(pid>1);
    assert.throws(()=>process.kill(pid,0),{code:'ESRCH'});
  }
  if (!(molmo ? visualEngine : visionpsyEngine)) {
    const profile=molmo?'molmo2':'visionpsy';
    const state=path.join(test,profile+'-activation','activation.sqlite3');
    const activation=()=>execFileSync('/usr/bin/sqlite3',[state,
      "select revision,json_extract(config,'$.bundle_config.profile') from current"],
      {encoding:'utf8'}).trim();
    const row=activation();
    assert.equal(row,workers.length+'|default-'+profile);
    console.log('PASS installed '+profile+' activated verified release and model pins');
    if (!molmo) {
      const input=path.join(test,'models','visionpsy','processor_config.json');
      const original=fs.readFileSync(input);
      const changed=Buffer.from(original);
      changed[0]^=1;
      const traceStart=fs.statSync(trace.body.path).size;
      try {
        fs.writeFileSync(input,changed);
        await request('/v1/chat/completions','POST',chat,120000);
        const retry=fs.readFileSync(trace.body.path).subarray(traceStart).toString().trim().split('\n').map(JSON.parse);
        assert.ok(retry.some(({event,fields})=>event==='visionpsy_start_failed'&&fields?.code==='artifact_checksum_mismatch'));
        assert.ok(!retry.some(({event})=>event==='visionpsy_process_started'));
        assert.equal(activation(),row);
      } finally {
        fs.writeFileSync(input,original);
      }
      console.log('PASS installed VisionPsy refused a changed model before launch and preserved last good');
    }
  }
  assert.ok(result.body.choices[0].message.content.trim(),JSON.stringify(result.body));
  console.log(molmo?'PASS real Molmo2 image evidence, specialist cleanup and fresh Qwen qualification':
    'PASS real VisionPsy recovered an empty observation, reaped both workers and retained '+selectedBackend);
}

async function checkSingleChatModel() {
  const available=await request('/v1/backends');
  assert.equal(available.code,200);
  assert.deepEqual(available.body.backends.map(backend=>backend.id),['qwen']);
  const catalog=await request('/v1/models/catalog');
  assert.equal(catalog.code,200);
  assert.deepEqual(catalog.body.models.filter(model=>model.family==='chat').map(model=>model.id),['qwen']);
  for (const backend of ['maple','ornith','bonsai','molmo2-4b-mlx-q4-v1']) {
    const result=await request('/v1/backends/select','POST',{backend});
    assert.equal(result.code,400,JSON.stringify(result.body));
    assert.equal(result.body.error.code,'invalid_backend');
  }
  const setup=await request('/v1/setup/status');
  assert.equal(setup.code,200);
  assert.equal(setup.body.selected_model_id,'qwen',JSON.stringify(setup.body));
  assert.equal(setup.body.next_step,'chat');
  console.log('PASS installed release uses Qwen for an existing profile and refuses retired chat models');
}

async function checkBonsaiProjector() {
  assert.equal((await request('/healthz')).body.supports_images,true);
  const result=await request('/v1/chat/completions','POST',{
    model:nativeAliases.bonsai,messages:[{role:'user',content:'Name the main colors in the image.'}],
    attachment_ids:[await uploadLogo()],analysis_depth:'fast',max_tokens:32,thinking:'off',
    chat_template_kwargs:{enable_thinking:false},temperature:0,stream:false
  },240000);
  assert.equal(result.code,200,JSON.stringify(result.body));
  assert.ok(result.body.choices[0].message.content.trim());
  assert.match(fs.readFileSync(path.join(test,'backend.log'),'utf8'),/image processed in \d+ ms/);
  console.log('PASS real Bonsai image request used the pinned projector');
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
  if(installedBackend){
    for(const artifact of nativeArtifacts) {
      if(bonsaiTextOnly&&artifact.role==='vision_projector') continue;
      const destination=path.join(test,artifact.install_path);
      fs.mkdirSync(path.dirname(destination),{recursive:true});
      execFileSync('cp',['-c','-p',nativeArtifactPath(artifact),destination]);
    }
    if(visionpsyModel) execFileSync('cp',['-c','-pR',visionpsyModel,path.join(test,'models','visionpsy')]);
  } else if(installedRelease){
    fs.mkdirSync(model,{recursive:true});
    for(const name of names)execFileSync('cp',['-c','-p',path.join(realModel,name),model]);
    if(visionpsyModel) execFileSync('cp',['-c','-pR',visionpsyModel,path.join(test,'models','visionpsy')]);
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
  } else if(pinnedBackend){
    const modelPins=nativeArtifacts.map(artifact=>[artifact.sha256,artifact.bytes,nativeArtifactPath(artifact)].join('\t'));
    const library=pinnedBackend==='maple'?record(path.join(path.dirname(nativeEngine),'mlx.metallib')):'';
    fs.writeFileSync(pinsFile,record(nativeEngine)+library+modelPins.join('\n')+'\n',{mode:0o600});
  } else if(!installed){
    const assets=[engine,...names.map(name=>path.join(model,name))];
    if(actual) assets.push('/opt/homebrew/opt/libomp/lib/libomp.dylib');
    else assets.push(goodGguf,badGguf);
    const recorded=actual?new Map(fs.readFileSync(path.join(module,'docs/qualification/life07h-qwen-sources/actual-artifacts.sha256'),'utf8').trim().split('\n').map(line=>[line.slice(66),line.slice(0,64)])):new Map();
    fs.writeFileSync(pinsFile,assets.map(asset=>[recorded.get(asset)??quote(asset),fs.statSync(asset).size,asset].join('\t')).join('\n')+'\n',{mode:0o600});
  }
  if(installed) fs.symlinkSync(release,path.join(test,'current'),'dir');
  if(installedBackend==='bonsai') {
    const launcher=path.join(test,'current','bin','samosa');
    const report=execFileSync('env',['-u','SAMOSA_RELEASE_DIR','-u','SAMOSA_BONSAI_SERVER',
      '-u','SAMOSA_BONSAI_MODEL',launcher,'doctor'],{encoding:'utf8',env:{...process.env,SAMOSA_HOME:test}});
    assert.match(report,/^bonsai:\s+ok \(27B 1-bit\)$/m);
  }
  fs.writeFileSync(path.join(test,'model-backend'),(installedRelease?'ornith':selectedBackend)+'\n');
  if(installedRelease) fs.writeFileSync(path.join(test,'profile.json'),JSON.stringify({
    name:'Native fixture',onboarding:{welcome_completed_at:'2026-11-22T00:00:00Z',selected_model_id:'ornith',selected_model_version:'legacy'}
  }));
  startGateway();
  await controlPlane();
  await ready(selectedBackend);
  const first=Number(fs.readFileSync(marker,'utf8'));
  assert(first>1);
  assert.ok(fs.existsSync(path.join(test,'run','primary-activation','activation.sqlite3')));
  if(installedRelease) await checkSingleChatModel();
  if(visualModel||visionpsyModel) await qualifySpecialist(first,Boolean(visualModel));
  if(nativeBackend){
    if(installedBackend){
      const command=execFileSync('ps',['-p',String(first),'-o','command='],{encoding:'utf8'}).trim();
      const runtime=installedBackend==='maple'?'samosa-maple':'samosa-ornith-llama-server';
      assert.ok(command.startsWith(path.join(release,'bin',runtime)+' '));
      const catalog=await request('/v1/models/catalog');
      assert.equal(catalog.code,200,JSON.stringify(catalog.body));
      const entry=catalog.body.models.find(model=>model.id===installedBackend);
      assert.equal(entry.install_state,'ready');
      if(bonsaiTextOnly) {
        assert.deepEqual(entry.capabilities,['text']);
        assert.equal((await request('/healthz')).body.supports_images,false);
      }
    }
    const maxTokens={ornith:4,bonsai:32,maple:64}[selectedBackend];
    const result=await request('/v1/chat/completions','POST',{model:nativeAliases[selectedBackend],
      messages:[{role:'user',content:'Say hello.'}],max_tokens:maxTokens,thinking:'off',
      chat_template_kwargs:{enable_thinking:false},temperature:0,stream:false},120000);
    assert.equal(result.code,200,JSON.stringify(result.body));
    assert.ok(result.body.choices[0].message.content.trim());
    if(nativeBackend==='bonsai'&&!bonsaiTextOnly) await checkBonsaiProjector();
  } else if(installedRelease){
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
  console.log(installedBackend?`PASS installed ${installedBackend} runtime, automatic pins, catalog readiness, public answer and cleanup${bonsaiTextOnly?' (text-only)':''}`:
    pinnedBackend?`PASS real ${pinnedBackend} qualification, public answer and cleanup`:
    installedRelease?'PASS installed launcher qualified real Qwen and reaped its worker':installedFixture?
    'PASS installed Qwen pins, legacy selection, Qwen reselection and refusal of changed or missing release metadata':actual?
    'PASS Gateway completed real Qwen qualification and reaped its primary worker':
    'PASS Gateway qualification, Bonsai selection, failed generation rollback, stale input refusal and cleanup');
} catch(err) {
  await stop();
  if(fs.existsSync(log)) process.stderr.write(fs.readFileSync(log,'utf8'));
  const backendLog=path.join(test,'backend.log');
  if(nativeBackend&&fs.existsSync(backendLog)) process.stderr.write(fs.readFileSync(backendLog,'utf8').slice(-12000));
  throw err;
} finally {
  fs.rmSync(test,{recursive:true,force:true});
}
