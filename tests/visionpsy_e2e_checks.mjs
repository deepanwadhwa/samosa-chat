// Port of the existing VisionPsy shell test's JSON/fixture assertions.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
const [mode, ...args] = process.argv.slice(2);
const input = () => JSON.parse(fs.readFileSync(0, 'utf8'));
const commands = () => fs.readFileSync(args[0], 'utf8').split(/\r?\n/).filter(line => line.trim()).map(line => JSON.parse(line));
switch (mode) {
  case 'health': {
    const h = input();
    assert.equal(h.supports_images, false);
    assert.equal(h.supports_image_attachments, true);
    assert.equal(h.vision.auxiliary_runtime_available, true);
    assert.deepEqual(h.ocr, { runtime_available: true, pack_ready: false, ready: false });
    console.log('Health capability separation: PASS');
    break;
  }
  case 'catalog': {
    const d = JSON.parse(fs.readFileSync(args[0], 'utf8'));
    const byId = Object.fromEntries(d.models.map(model => [model.id, model]));
    const vp = byId['visionpsy-nano-460m-mlx-bf16'];
    assert(vp, `VisionPsy missing from catalog: ${Object.keys(byId)}`);
    assert.equal(vp.category, 'vision');
    assert.equal(vp.role, 'auxiliary');
    assert.equal(vp.backend_kind, 'mlx_vision_native');
    assert.equal(vp.active, false);
    assert.equal(vp.routing, 'automatic');
    assert.equal(vp.load_policy, 'on_demand_per_turn');
    assert.equal(vp.artifacts.length, 7);
    console.log('Catalog verification: PASS');
    break;
  }
  case 'id': console.log(input().id ?? ''); break;
  case 'missing-model': {
    const d = input();
    assert.equal(d.error?.code, 'vision_model_required', JSON.stringify(d));
    console.log('Missing VisionPsy error flow: PASS');
    break;
  }
  case 'model-fixture': {
    const fd = fs.openSync(path.join(args[0], 'model.safetensors'), 'w');
    fs.ftruncateSync(fd, 1014772920); fs.closeSync(fd);
    for (const name of ['config.json', 'preprocessor_config.json', 'processor_config.json', 'tokenizer.json', 'tokenizer_config.json', 'chat_template.jinja'])
      fs.writeFileSync(path.join(args[0], name), '{}');
    break;
  }
  case 'choices': { const d = input(); assert('choices' in d, JSON.stringify(d)); break; }
  case 'image-floor': {
    const d = input(); assert('choices' in d, JSON.stringify(d));
    const all = commands();
    for (const name of ['ping', 'inspect', 'quit'])
      assert.equal(all.filter(command => command.command === name).length, 1, JSON.stringify(all));
    assert(all.find(command => command.command === 'inspect').prompt.includes('Copy the lettering exactly and preserve line breaks.'));
    console.log('Image OCR plus visual evidence floor: PASS'); break;
  }
  case 'developer-default': {
    const d = input(); assert.equal(d.enabled, false);
    assert.equal(d.captures_sensitive_data, true);
    assert.equal(d.authentication_tokens_logged, false); break;
  }
  case 'developer-path': {
    const d = input(); assert.equal(d.enabled, true); assert(d.path); console.log(d.path); break;
  }
  case 'developer-trace': {
    const raw = fs.readFileSync(args[0], 'utf8');
    assert(!raw.includes(args[1]), 'UI authentication token leaked into Developer trace');
    const events = raw.split(/\r?\n/).filter(line => line.trim()).map(line => JSON.parse(line));
    const names = events.map(event => event.event);
    const required = new Set([
      'chat_request_received', 'vision_router_request', 'vision_router_response',
      'vision_router_validated_plan', 'vision_resource_budget', 'attachment_selected',
      'visionpsy_process_started', 'visionpsy_request', 'visionpsy_raw_response',
      'visionpsy_complete', 'vision_evidence_observation', 'backend_request',
      'backend_response', 'backend_complete', 'chat_turn_completed',
    ]);
    const missing = [...required].filter(name => !names.includes(name)).sort();
    assert.equal(missing.length, 0, `Developer trace missing events: ${missing}\n${names}`);
    const visual = events.filter(event => event.event === 'visionpsy_raw_response').at(-1);
    assert(visual.fields.payload.includes('A detailed high-resolution diagram'), JSON.stringify(visual));
    const finalRequest = events.filter(event => event.event === 'backend_request').at(-1);
    assert(finalRequest.fields.payload.includes('A detailed high-resolution diagram'), JSON.stringify(finalRequest));
    const turnIds = new Set(events.filter(event => required.has(event.event) && event.turn_id).map(event => event.turn_id));
    assert.equal(turnIds.size, 1, `Expected one correlated turn: ${[...turnIds]}`);
    console.log('Full correlated Developer trace: PASS'); break;
  }
  case 'developer-disabled': assert.equal(input().enabled, false); break;
  case 'developer-cleared': assert(input().removed >= 1); break;
  case 'multipage': {
    const d = input(); assert('choices' in d, JSON.stringify(d));
    const all = commands(), inspects = all.filter(command => command.command === 'inspect');
    assert.equal(all.filter(command => command.command === 'ping').length, 1, JSON.stringify(all));
    assert.equal(inspects.length, 3, JSON.stringify(all));
    assert.deepEqual(inspects.map(command => Number(command.prompt.split(/\s+/).find(word => /^\d+$/.test(word)))), [1, 2, 3]);
    assert.equal(new Set(inspects.map(command => command.max_side_len)).size, 1);
    assert.equal(all.at(-1).command, 'quit');
    console.log('VisionPsy multi-page turn reuse: PASS'); break;
  }
  case 'partial': {
    const all = commands();
    for (const name of ['inspect', 'ping', 'quit'])
      assert.equal(all.filter(command => command.command === name).length, 2, JSON.stringify(all));
    console.log('Labelled OCR-only partial recovery: PASS'); break;
  }
  default: throw new Error(`Unknown VisionPsy test assertion: ${mode}`);
}
