import {test} from 'node:test';
import assert from 'node:assert/strict';
import {classifyDevice,usageController} from '../src/device-kind.ts';
import {deviceRows,filterDeviceRows,makeDraft,setVisibility,dirty} from '../src/model.ts';
import {createRequire} from 'node:module';
const fixture=createRequire(import.meta.url)('../electron/fixture.cjs');
const info=(name,hidUsages=[],kind='unknown')=>({name,detail:'',kind,hidUsages});
const usage=(page,usage,known=true)=>({page,usage,known});

test('local named products receive distinct, accurate categories including Xbox headset and game capture',()=>{
 const products=[
 ['Creative Technology Ltd Sound Blaster X3','audio-interface','non-game'],
 ['Elgato Cam Link 4K','capture','non-game'],
 ['Elgato Stream Deck MK.2','streamdeck','non-game'],
 ['Elgato Stream Deck XL','streamdeck','non-game'],
 ['Elgato Stream Deck Plus','streamdeck-plus','non-game'],
 ['Audeze LLC Maxwell XBOX Dongle','headphones','non-game'],
 ['Gomez Sim Industries, LLC GSI GXL v2 - simOS','wheel','game'],
 ['Granite devices Simucube 2 Pro','wheel','game'],
 ['Heusinkveld Engineering HE SIM HANDBRAKE','handbrake','game'],
 ['Heusinkveld Sim Pedals Sprint','pedals','game'],
 ['Keychron Q1 Pro','keyboard','non-game'],
 ['LG Electronics Inc. HID I2C UltraGear Monitor','monitor','non-game'],
 ['MSI MYSTIC LIGHT','lighting','non-game'],
 ['SIMAGIC P2000 Haptic','haptic','unknown'],
 ['SteelSeries Sensei Ten','mouse','non-game'],
 ['Unknown Shifter','shifter','game'],
 ['VIRPIL Controls 20260218 L-VPC MongoosT-50CM3','throttle','game'],
 ['VIRPIL Controls 20260218 VPC ACE-Torq Rudder','rudder','game'],
 ['Generic USB Audio','audio-interface','non-game'],
 ['USB Sound Device','audio-interface','non-game'],
 ['Logitech USB Receiver','receiver','unknown'],
 ['Logitech Wireless Receiver','receiver','unknown'],
 ['Keychron Link','receiver','unknown'],
 ['HID-compliant bar code badge reader','barcode','non-game'],
 ['HID-compliant vendor-defined device','unknown','unknown'],
 ];
 for(const [name,kind,controller] of products){const c=classifyDevice(info(name));assert.equal(c.kind,kind,name);assert.equal(c.controller,controller,name);}
 assert.equal(classifyDevice(info('Audeze Maxwell XBOX Dongle',[],'gamepad')).kind,'headphones');
});

test('numeric HID collections support unfamiliar hardware, composites and inaccessible metadata',()=>{
 for(const [page,id] of [[1,4],[1,5],[1,8],[2,2],[2,0x21],[5,1]])assert.equal(classifyDevice(info('New USB device',[usage(page,id)])).controller,'game');
 for(const [page,id] of [[1,2],[1,6],[12,1],[11,5],[13,2]])assert.equal(usageController(usage(page,id)),'non-game');
 for(const u of [usage(1,5,false),usage(0xff00,1),usage(5,0x32),usage(-1,5),usage(1,NaN)])assert.equal(usageController(u),'unknown');
 assert.equal(classifyDevice(info('Generic device',[usage(12,1),usage(0xff00,1)])).controller,'unknown');
 assert.equal(classifyDevice(info('Generic device',[usage(12,1),usage(1,5,false)])).controller,'unknown');
 assert.equal(classifyDevice(info('Generic device',[usage(12,1),usage(1,0x80)])).controller,'non-game');
 assert.equal(classifyDevice(info('Keychron Link',[usage(12,1),usage(1,4),usage(0xff60,61)])).controller,'game');
 assert.equal(classifyDevice(info('SIMAGIC P2000 Haptic',[usage(1,4)])).controller,'game');
 assert.equal(classifyDevice(info('Unknown USB device',[],'gamepad')).controller,'unknown');
});

test('numeric collection evidence takes precedence over game-like product names',()=>{
 const keyboardOnly=classifyDevice(info('Generic USB pedals',[usage(1,6)]));
 assert.equal(keyboardOnly.kind,'pedals');
 assert.equal(keyboardOnly.controller,'non-game');
 assert.match(keyboardOnly.reason,/All reported HID collections/);

 const mixedKnownNonGame=classifyDevice(info('Unknown Shifter',[usage(1,6),usage(12,1)]));
 assert.equal(mixedKnownNonGame.kind,'shifter');
 assert.equal(mixedKnownNonGame.controller,'non-game');

 const mixedGame=classifyDevice(info('Generic USB pedals',[usage(1,6),usage(1,4)]));
 assert.equal(mixedGame.kind,'pedals');
 assert.equal(mixedGame.controller,'game');
 assert.match(mixedGame.reason,/Reports a game-input HID collection/);

 for(const unknownUsage of [usage(0xff00,1),usage(1,6,false),usage(1,9)]){
  const ambiguous=classifyDevice(info('Generic USB device',[usage(1,6),unknownUsage]));
  assert.equal(ambiguous.controller,'unknown',JSON.stringify(unknownUsage));
 }
});

test('name precedence avoids peripheral traps and supports offline hints without guessing vendors',()=>{
 for(const name of ['Elgato Game Capture HD60 X','Stream Deck Pedal','Stream Deck +','Sound BlasterX G6','Gaming mouse wheel','Xbox gaming headset'])assert.equal(classifyDevice(info(name)).controller,'non-game',name);
 for(const name of ['Creative device','Elgato device','Valve device','Steam Deck','Generic controller','Guitar accessory'])assert.equal(classifyDevice(info(name)).controller,'unknown',name);
 assert.equal(classifyDevice(info('Remembered steering wheel')).controller,'game');
 assert.match(classifyDevice(info('Remembered steering wheel')).reason,/Inferred/);
});

test('combined view filters keep unknowns and preserve hidden, pending and offline rules',()=>{
 const snapshot=fixture.control({reset:true}),draft=makeDraft(snapshot.profiles[1],snapshot);
 const extra=(id,name,connected=true)=>({id,name,connected,detail:'',identities:[id],current:'Visible'});
 const audio=extra('audio','Sound Blaster X3'),unknown=extra('unknown','Unidentified USB interface'),offline=extra('offline','Cam Link 4K',false);
 draft.profile=setVisibility(draft.profile,audio,'hidden');draft.profile=setVisibility(draft.profile,offline,'hidden');
 const rows=deviceRows(draft.profile,draft.original,[...snapshot.devices,audio,unknown,offline]),before=JSON.stringify(draft);
 const filtered=filterDeviceRows(rows,false,true,true);
 assert(filtered.some(r=>r.device.id==='unknown'));assert(!filtered.some(r=>r.device.id==='audio'||r.device.id==='offline'));
 assert.equal(filterDeviceRows(rows,true,true,true).length,filtered.length);
 assert.equal(filterDeviceRows(rows,true,false,false).length,rows.length);
 assert.equal(filterDeviceRows(rows,false,true,false),rows);
 assert.equal(JSON.stringify(draft),before);assert.equal(dirty(draft),true);
 assert.equal(draft.profile.deviceRules.find(r=>r.identity==='audio').visibility,'hidden');
});

test('wheel and pedal combination devices are steering wheels, separate pedals stay pedals (#33)',()=>{
 for(const name of ['Thrustmaster T150 Racing Wheel and Pedals','Logitech Driving Force Wheel & Pedals','Hori Racing Wheel Apex with pedals','Pedals + Steering Wheel Set'])
  assert.equal(classifyDevice(info(name)).kind,'wheel',name);
 for(const name of ['Heusinkveld Sim Pedals Sprint','Fanatec ClubSport Pedals V3','Generic USB pedals'])
  assert.equal(classifyDevice(info(name)).kind,'pedals',name);
 assert.equal(classifyDevice(info('Gaming mouse wheel')).kind,'mouse');
});
