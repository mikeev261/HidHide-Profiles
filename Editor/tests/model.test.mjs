import {test} from 'node:test';
import assert from 'node:assert/strict';
import {createRequire} from 'node:module';
import {makeDraft,dirty,visibility,setVisibility,devicesFor,deviceRows,filterDeviceRows,validation,pendingCount,duplicateName,adoptionExpectedSettings} from '../src/model.ts';
const fixture=createRequire(import.meta.url)('../electron/fixture.cjs');
test('draft edits never mutate applied snapshot; grouped mixed identities stay explicit',()=>{
 const s=fixture.control({reset:true});const d=makeDraft(s.profiles[1],s);assert.equal(dirty(d),false);
 const group={...s.devices[0],identities:['wheel','other']};assert.equal(visibility(d.profile,group),'mixed');
 d.profile=setVisibility(d.profile,group,'hidden');assert.equal(dirty(d),true);assert.equal(d.profile.deviceRules.length,3);assert.equal(s.profiles[1].deviceRules.length,2);assert.equal(visibility(d.profile,group),'hidden');
});
test('disconnected rules remain visible and unrelated identities survive edits',()=>{
 const s=fixture.control({reset:true});const d=makeDraft(s.profiles[1],s);d.profile.deviceRules.push({identity:'gone',friendlyName:'Old controller',visibility:'hidden'});
 const rows=devicesFor(d.profile,s.devices);assert.equal(rows.at(-1).connected,false);assert.equal(rows.at(-1).current,'Unknown');
 const p=setVisibility(d.profile,s.devices[0],'visible');assert.equal(p.deviceRules.find(r=>r.identity==='gone').visibility,'hidden');
});
test('selected Global cannot be deleted or disabled without another fallback',()=>{
 const s=fixture.control({reset:true});const d=makeDraft(s.profiles[0],s);d.deleting=true;assert.match(validation(d,s),/another enabled Global/);d.deleting=false;d.profile.enabled=false;assert.match(validation(d,s),/another enabled Global/);
});
test('settings edits and new disabled import are unapplied drafts',()=>{
 const s=fixture.control({reset:true});const d=makeDraft(s.profiles[1],s);d.settings.allowedApplications.push('C:\\Feeder.exe');assert.equal(s.settings.allowedApplications.length,1);assert.equal(dirty(d),true);
 const created=makeDraft({...s.profiles[1],enabled:false},s,true);assert.equal(created.expected,null);assert.equal(dirty(created),true);
});
test('disconnected filtering is presentation-only, preserves pending rules and shows unknown connections',()=>{
 const s=fixture.control({reset:true}),draft=makeDraft(s.profiles[1],s);
 draft.profile.deviceRules.push({identity:'gone',friendlyName:'Remembered pedals',visibility:'hidden'});
 const before=JSON.stringify(draft),rows=deviceRows(draft.profile,draft.original,s.devices);
 const visible=filterDeviceRows(rows,true,true);
 assert.equal(visible.length,s.devices.length);assert.equal(rows.length,s.devices.length+1);
 assert.equal(filterDeviceRows(rows,true,false),rows);assert.equal(filterDeviceRows(rows,false,true),rows);
 assert.equal(JSON.stringify(draft),before);assert.equal(dirty(draft),true);
 for(const row of rows)assert.equal(row.value,visibility(draft.profile,row.device));
});
test('two new explicit Visible remembered rules each count and mark their rows until applied or discarded',()=>{
 const s=fixture.control({reset:true}),draft=makeDraft(s.profiles[1],s);
 const rules=[{identity:'remembered-visible',friendlyName:'Spare wheel',visibility:'visible'},
  {identity:'remembered-visible-2',friendlyName:'Spare pedals',visibility:'visible'}];
 draft.profile.deviceRules.push(...rules);
 assert.equal(dirty(draft),true);assert.equal(pendingCount(draft),2);
 const rows=deviceRows(draft.profile,draft.original,s.devices).filter(row=>rules.some(rule=>rule.identity===row.device.id));
 assert.equal(rows.length,2);
 for(const row of rows){
  assert.equal(row.value,'visible');assert.equal(row.changed,true);
  assert.equal(filterDeviceRows([row],true,true).length,0);
  assert.equal(filterDeviceRows([row],true,false)[0],row);
 }
 const applied=makeDraft(draft.profile,s);
 assert.equal(dirty(applied),false);
 for(const rule of rules)assert.equal(deviceRows(applied.profile,applied.original,s.devices).find(row=>row.device.id===rule.identity).changed,false);
 const discarded=makeDraft(s.profiles[1],s);
 assert.equal(dirty(discarded),false);
 assert.equal(deviceRows(discarded.profile,discarded.original,s.devices).some(row=>rules.some(rule=>rule.identity===row.device.id)),false);
});
test('a visibility reversal restores saved identity casing, while an actual case edit remains pending',()=>{
 const s=fixture.control({reset:true});
 const device={...s.devices[0],id:'HID\\WHEEL',identities:['HID\\WHEEL']};
 s.profiles[1].deviceRules=[{identity:'hid\\wheel',friendlyName:'Saved custom label',visibility:'hidden'}];
 const draft=makeDraft(s.profiles[1],s);
 draft.profile=setVisibility(draft.profile,device,'visible',draft.original);
 draft.profile=setVisibility(draft.profile,device,'hidden',draft.original);
 assert.deepEqual(draft.profile.deviceRules,draft.original.deviceRules);
 assert.equal(dirty(draft),false);
 const row=deviceRows(draft.profile,draft.original,[device]).find(row=>row.device.id===device.id);
 assert.equal(row.value,'hidden');assert.equal(row.changed,false);
 draft.profile.deviceRules[0].identity='HID\\WHEEL';
 assert.equal(dirty(draft),true);assert.equal(pendingCount(draft),1);
 assert.equal(deviceRows(draft.profile,draft.original,[device])[0].changed,true);
});
test('reversing a visibility edit restores the saved rule order and clears the draft',()=>{
 const s=fixture.control({reset:true}),draft=makeDraft(s.profiles[1],s);
 const savedOrder=draft.original.deviceRules.map(rule=>rule.identity);
 draft.profile=setVisibility(draft.profile,s.devices[0],'visible',draft.original);
 draft.profile=setVisibility(draft.profile,s.devices[0],'hidden',draft.original);
 assert.deepEqual(draft.profile.deviceRules.map(rule=>rule.identity),savedOrder);
 assert.equal(dirty(draft),false);
 assert.equal(deviceRows(draft.profile,draft.original,s.devices).some(row=>row.changed),false);
});
test('mixed group row marks an identity rule change even when the group remains mixed',()=>{
 const s=fixture.control({reset:true}),draft=makeDraft(s.profiles[1],s);
 const group={...s.devices[0],identities:['wheel','pedals','other']};
 assert.equal(visibility(draft.profile,group),'mixed');
 draft.profile.deviceRules=draft.profile.deviceRules.map(rule=>rule.identity==='wheel'?{...rule,visibility:'visible'}:rule);
 const row=deviceRows(draft.profile,draft.original,[group]).find(row=>row.device.id===group.id);
 assert.equal(row.value,'mixed');assert.equal(row.changed,true);
});
test('live default Visible returns to no rule after a Hidden then Visible reversal',()=>{
 const s=fixture.control({reset:true}),draft=makeDraft(s.profiles[1],s),device=s.devices.find(device=>device.id==='buttons');
 const before=structuredClone(draft.profile.deviceRules);
 draft.profile=setVisibility(draft.profile,device,'hidden',draft.original);
 assert.equal(dirty(draft),true);assert.equal(pendingCount(draft),1);
 assert.equal(deviceRows(draft.profile,draft.original,s.devices).find(row=>row.device.id===device.id).changed,true);
 draft.profile=setVisibility(draft.profile,device,'visible',draft.original);
 assert.deepEqual(draft.profile.deviceRules,before);
 assert.equal(dirty(draft),false);
 assert.equal(deviceRows(draft.profile,draft.original,s.devices).find(row=>row.device.id===device.id).changed,false);
});
test('saved friendly name and rule position survive a visibility reversal and later Apply',async()=>{
 const s=fixture.control({reset:true});
 s.profiles[1].deviceRules[0].friendlyName='My custom wheel label';
 const draft=makeDraft(s.profiles[1],s),before=structuredClone(draft.original.deviceRules);
 draft.profile=setVisibility(draft.profile,s.devices[0],'visible',draft.original);
 draft.profile=setVisibility(draft.profile,s.devices[0],'hidden',draft.original);
 assert.deepEqual(draft.profile.deviceRules,before);
 assert.equal(dirty(draft),false);
 assert.equal(deviceRows(draft.profile,draft.original,s.devices).find(row=>row.device.id==='wheel').changed,false);
 draft.profile.priority++;
 await fixture.request({command:'apply',profile:draft.profile,expected:draft.expected,settings:draft.settings,expectedSettings:draft.expectedSettings});
 assert.deepEqual(fixture.control({}).profiles[1].deviceRules,before);
});
test('mixed identities restore saved rules and remove only temporary default Visible rules',()=>{
 const s=fixture.control({reset:true}),draft=makeDraft(s.profiles[1],s);
 const group={...s.devices[0],identities:['wheel','buttons','pedals']};
 const before=structuredClone(draft.original.deviceRules);
 draft.profile=setVisibility(draft.profile,group,'hidden',draft.original);
 assert.equal(dirty(draft),true);
 draft.profile=setVisibility(draft.profile,group,'visible',draft.original);
 assert.equal(draft.profile.deviceRules.some(rule=>rule.identity==='buttons'),false);
 draft.profile=setVisibility(draft.profile,group,'hidden',draft.original);
 assert.deepEqual(draft.profile.deviceRules,[...before,{identity:'buttons',friendlyName:group.name,visibility:'hidden'}]);
 draft.profile=setVisibility(draft.profile,group,'visible',draft.original);
 assert.equal(draft.profile.deviceRules.some(rule=>rule.identity==='buttons'),false);
 assert.equal(draft.profile.deviceRules[0].friendlyName,before[0].friendlyName);
});
test('a mixed group keeps saved identity spelling and order while changing visibility',()=>{
 const s=fixture.control({reset:true});
 s.profiles[1].deviceRules=[
  {identity:'hid\\wheel',friendlyName:'Saved wheel',visibility:'hidden'},
  {identity:'unrelated',friendlyName:'Unrelated device',visibility:'hidden'},
  {identity:'HiD\\Pedals',friendlyName:'Saved pedals',visibility:'visible'}
 ];
 const draft=makeDraft(s.profiles[1],s);
 const group={...s.devices[0],identities:['HID\\WHEEL','HID\\PEDALS']};
 const before=structuredClone(draft.original.deviceRules);
 assert.equal(visibility(draft.profile,group),'mixed');
 draft.profile=setVisibility(draft.profile,group,'hidden',draft.original);
 assert.deepEqual(draft.profile.deviceRules.map(rule=>rule.identity),before.map(rule=>rule.identity));
 draft.profile=setVisibility(draft.profile,group,'visible',draft.original);
 assert.deepEqual(draft.profile.deviceRules.map(rule=>rule.identity),before.map(rule=>rule.identity));
 assert.equal(draft.profile.deviceRules[0].friendlyName,'Saved wheel');
 assert.deepEqual(draft.profile.deviceRules[1],before[1]);
 assert.deepEqual(draft.profile.deviceRules[2],before[2]);
});
test('new remembered Visible rules retain identity, names and position through their own toggles',()=>{
 const s=fixture.control({reset:true}),draft=makeDraft(s.profiles[1],s);
 const additions=[{identity:'new-remembered-a',friendlyName:'Spare wheel',visibility:'visible'},{identity:'new-remembered-b',friendlyName:'Spare pedals',visibility:'visible'}];
 draft.profile.deviceRules.push(...additions);
 const device=deviceRows(draft.profile,draft.original,s.devices).find(row=>row.device.id==='new-remembered-a').device;
 const explicit=new Set(additions.map(rule=>rule.identity.toLowerCase()));
 draft.profile=setVisibility(draft.profile,device,'hidden',draft.original,explicit);
 draft.profile=setVisibility(draft.profile,device,'visible',draft.original,explicit);
 assert.deepEqual(draft.profile.deviceRules.slice(-2),additions);
 assert.equal(pendingCount(draft),2);
 assert.equal(deviceRows(draft.profile,draft.original,s.devices).filter(row=>row.device.id.startsWith('new-remembered-')).every(row=>row.changed),true);
});
test('an explicit remembered rule for a live identity survives toggles without preserving another default',()=>{
 const s=fixture.control({reset:true}),draft=makeDraft(s.profiles[1],s);
 const rule={identity:'hid\\buttons',friendlyName:'My button box',visibility:'visible'};
 draft.profile.deviceRules.push(rule);
 const liveButton={...s.devices.find(device=>device.id==='buttons'),id:'HID\\BUTTONS',identities:['HID\\BUTTONS']};
 const group={...liveButton,identities:['HID\\BUTTONS','XBOX']};
 const explicit=new Set(['hid\\buttons']);
 draft.profile=setVisibility(draft.profile,group,'hidden',draft.original,explicit);
 draft.profile=setVisibility(draft.profile,group,'visible',draft.original,explicit);
 assert.deepEqual(draft.profile.deviceRules,[...draft.original.deviceRules,rule]);
 assert.equal(pendingCount(draft),1);
 assert.equal(deviceRows(draft.profile,draft.original,[liveButton]).find(row=>row.device.id===liveButton.id).changed,true);
 assert.equal(deviceRows(draft.profile,draft.original,[{...s.devices.find(device=>device.id==='xbox'),identities:['XBOX']}])[0].changed,false);
});
test('duplicateName truncates at the boundary, preserves short names, and handles surrogate pairs',()=>{
 const s=fixture.control({reset:true});const d=makeDraft(s.profiles[1],s);
 // Validation boundary
 d.profile.name='x'.repeat(256);assert.equal(validation(d,s),'');
 d.profile.name='x'.repeat(257);assert.match(validation(d,s),/256/);
 // Production duplicateName: long name truncated to fit
 const long=duplicateName('y'.repeat(256));
 assert.equal(long.length,256);assert.ok(long.endsWith(' copy'));
 // Short name preserved
 assert.equal(duplicateName('Short Profile'),'Short Profile copy');
 // Exact boundary: 251 chars + ' copy' = 256
 assert.equal(duplicateName('z'.repeat(251)).length,256);
 // Under boundary: 250 chars + ' copy' = 255
 assert.equal(duplicateName('z'.repeat(250)),'z'.repeat(250)+' copy');
 // Unicode: emoji surrogate pairs are not split
 const emoji='\u{1F3AE}'.repeat(126); // 126 emoji = 126 graphemes (252 UTF-16 code units)
 const emojiResult=duplicateName(emoji);
 assert.ok(emojiResult.length<=256);assert.ok(emojiResult.endsWith(' copy'));
 // Verify no broken surrogates: every char before ' copy' should be valid
 const prefix=emojiResult.slice(0,-' copy'.length);
 assert.ok(!/[\uD800-\uDBFF]$/.test(prefix),'must not end with a dangling high surrogate');
});
test('adoption without saved settings is offered only for a missing catalog',()=>{
 const s=fixture.control({reset:true});assert.deepEqual(adoptionExpectedSettings(s),s.settingsVersion);
 assert.deepEqual(adoptionExpectedSettings({...s,catalogUninitialized:true}),s.settingsVersion);
 assert.equal(adoptionExpectedSettings({...s,settingsVersion:null,catalogUninitialized:true}),null);
 assert.equal(adoptionExpectedSettings({...s,settingsVersion:null,catalogUninitialized:false}),undefined);
 assert.equal(adoptionExpectedSettings({...s,settingsVersion:null}),undefined);
});
