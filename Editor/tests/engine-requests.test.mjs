import {test} from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {createRequire} from 'node:module';
const {createEngineRequests,REQUEST_LIMIT}=createRequire(import.meta.url)('../electron/engine-requests.cjs');

// A fake engine: snapshots report the resident engine instance, every request is recorded.
function fakeEngine(){
 const engine={instance:'engine-1',sent:[]};
 engine.exchange=async request=>{engine.sent.push(request);return request.command==='snapshot'?{ok:true,snapshot:{engineInstance:engine.instance}}:{ok:true};};
 engine.requests=createEngineRequests({commands:new Set(['snapshot','apply']),exchange:engine.exchange,canStartEngine:()=>false,startEngine:()=>false,
  pid:4242,isDirty:()=>false,onSnapshot:()=>{},onStart:()=>{},onFinish:()=>{}});
 engine.registrations=()=>engine.sent.filter(r=>r.command==='editor-state').length;
 return engine;
}

test('the editor registers once per engine instance and again after the engine restarts (#20)',async()=>{
 const engine=fakeEngine();
 await engine.requests.request({command:'snapshot'});await engine.requests.request({command:'snapshot'});
 assert.equal(engine.registrations(),1);
 engine.instance='engine-2'; // The resident engine restarted; its registration state is empty.
 await engine.requests.request({command:'snapshot'});
 assert.equal(engine.registrations(),2);
 assert.deepEqual(engine.sent.at(-1),{command:'editor-state',pid:4242,dirty:false});
});

test('requests over the native bridge limit are refused before they reach the engine (#29)',async()=>{
 const configuration=fs.readFileSync(new URL('../../Shared/Configuration.h',import.meta.url),'utf8');
 const [,a,b]=configuration.match(/constexpr std::size_t MaxBytes = (\d+) \* (\d+);/);
 assert.equal(REQUEST_LIMIT,Number(a)*Number(b),'editor request limit matches Protocol::MaxBytes');
 const engine=fakeEngine();
 const oversized={command:'apply',padding:'x'.repeat(REQUEST_LIMIT)};
 await assert.rejects(engine.requests.request(oversized),/limit/);
 assert.equal(engine.sent.length,0);
 await engine.requests.request({command:'snapshot'});
 assert.equal(engine.registrations(),1,'the bridge stays usable after a refused request');
});
