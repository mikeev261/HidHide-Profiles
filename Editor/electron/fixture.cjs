// Explicit development-only acceptance fixture. Never loaded by packaged builds.
const clone=value=>JSON.parse(JSON.stringify(value));
const ids=['00000000-0000-4000-8000-000000000001','00000000-0000-4000-8000-000000000002','00000000-0000-4000-8000-000000000003'];
let counter=10,fail='',unknown=false,disconnected=false,snapshotDelay=0,snapshotRequests=0,activeSnapshots=0,peakSnapshots=0;
const profile=(id,name,kind,executablePath='')=>({schemaVersion:1,id,revision:1,name,kind,enabled:true,priority:0,executablePath,defaultVisibility:'visible',deviceRules:[],version:{revision:1,hash:id}});
const devices=[['wheel','Simucube 2 Pro','Wheel base'],['pedals','Heusinkveld Ultimate+','Pedals'],['buttons','Button box','Controller'],['xbox','Xbox controller','Gamepad']].map(([id,name,detail])=>({id,name,detail,connected:true,identities:[id],current:id==='wheel'||id==='pedals'?'Hidden':'Visible'}));
const initial={profiles:[profile(ids[0],'Default','global'),profile(ids[1],'Le Mans Ultimate','application','C:\\Games\\Le Mans Ultimate.exe'),profile(ids[2],'F1 25','application','C:\\Games\\F1 25.exe')],settings:{schemaVersion:1,revision:1,selectedGlobalId:ids[0],mode:'automatic',paused:false,startWithWindows:true,allowedApplications:['C:\\Program Files\\SimHub\\SimHub.exe']},settingsVersion:{revision:1,hash:'settings1'},activeId:ids[1],verified:true,status:'Running application profile applied and verified',conflict:false,repositoryIssues:[],devices};
initial.profiles[1].deviceRules=[{identity:'wheel',friendlyName:'Simucube 2 Pro',visibility:'hidden'},{identity:'pedals',friendlyName:'Heusinkveld Ultimate+',visibility:'hidden'}];
let state=clone(initial);
function snapshot(){const result=clone(state);if(unknown){result.verified=false;result.status='Driver observation unavailable';result.devices.forEach(d=>d.current='Unknown');}return result;}
function cas(expected,actual){if(!expected||expected.hash!==actual.hash)throw Error('Saved files changed. Discard to reload before applying.');}
exports.request=async request=>{
 if(disconnected)throw Error('Fixture engine disconnected.');
 const q=clone(request);
 if(fail&&q.command!=='snapshot'){const message=fail;fail='';throw Error(message);}
 switch(q.command){
 case 'snapshot':{snapshotRequests++;activeSnapshots++;peakSnapshots=Math.max(peakSnapshots,activeSnapshots);try{if(snapshotDelay)await new Promise(resolve=>setTimeout(resolve,snapshotDelay));return {ok:true,snapshot:snapshot()};}finally{activeSnapshots--;}}
 case 'editor-state':return {ok:true};
 case 'launch':{
  cas(q.expectedSettings,state.settingsVersion);
  const target=state.profiles.find(p=>p.id===q.id);
  if(!target||target.kind!=='application'||!target.enabled)throw Error('Choose a saved enabled application profile.');
  cas(q.expected,target.version);
  if(state.settings.paused||state.settings.allowedApplications.some(p=>p.toLowerCase()===target.executablePath.toLowerCase()))throw Error('Profile launch is blocked by current settings.');
  state.activeId=target.id;state.launchedId=target.id;
  return {ok:true,message:'Fixture simulated a verified direct launch.'};
 }
 case 'new':case 'import':if(q.command==='new'&&discoveryOptions.createDelay)await new Promise(resolve=>setTimeout(resolve,discoveryOptions.createDelay));return {ok:true,profile:{...profile('00000000-0000-4000-8000-'+String(++counter).padStart(12,'0'),q.name||'Imported profile',q.kind||'application',q.executable||'C:\\Games\\Imported.exe'),enabled:q.command!=='import',version:undefined}};
 case 'apply':case 'settings':case 'delete':{
  cas(q.expectedSettings,state.settingsVersion);
  if(q.command!=='settings'){
   const id=q.profile?.id||q.id,index=state.profiles.findIndex(p=>p.id===id);
   if(index>=0)cas(q.expected,state.profiles[index].version);else if(q.expected)throw Error('Profile no longer exists.');
   if(q.command==='delete')state.profiles.splice(index,1);
   else{const p={...q.profile,revision:q.profile.revision+1,version:{revision:q.profile.revision+1,hash:'profile'+(++counter)}};if(index>=0)state.profiles[index]=p;else state.profiles.push(p);}
  }
  state.settings={...q.settings,revision:state.settings.revision+1};state.settingsVersion={revision:state.settings.revision,hash:'settings'+(++counter)};
  state.activeId=state.settings.mode==='useGlobal'?state.settings.selectedGlobalId:ids[1];
  const p=state.profiles.find(p=>p.id===state.activeId);state.devices.forEach(d=>d.current=state.settings.paused?'Visible':p?.deviceRules.find(r=>r.identity===d.id)?.visibility==='hidden'?'Hidden':'Visible');
  return {ok:true,saved:true,applied:true,message:'Saved and applied.'};
 }
 case 'adopt':{
  // Only the missing-catalog form is modelled: null expected settings seed the catalog.
  if(q.expectedSettings!==null||!state.catalogUninitialized)throw Error('Unsupported fixture adoption.');
  const allowed=state.observedAllowed;state=clone(initial);state.settings.allowedApplications=allowed;
  return {ok:true,saved:true,applied:true,message:'Profile catalog saved and applied.'};
 }
 case 'retry':unknown=false;return {ok:true,saved:false,applied:true,message:'Activation verified.'};
 case 'backup':case 'export':return {ok:true};
 case 'restore':state=clone(initial);return {ok:true,saved:true,applied:true};
 default:throw Error('Unsupported fixture request: '+q.command);
 }
};
// A picker can stay open while saved settings change outside this editor.
const pickOptions={delay:0,externalAllowed:''};
exports.pick=async kind=>{
 const result=({executable:discoveryOptions.path,import:'C:\\Fixture\\profile.json',backup:'C:\\Fixture\\backup',restore:'C:\\Fixture\\backup',export:'C:\\Fixture\\export.json'})[kind]||null;
 const {delay,externalAllowed}=pickOptions;
 if(delay){
  await new Promise(resolve=>setTimeout(resolve,delay/2));
  if(externalAllowed){state.settings={...state.settings,allowedApplications:[...state.settings.allowedApplications,externalAllowed],revision:state.settings.revision+1};state.settingsVersion={revision:state.settings.revision,hash:'external'+(++counter)};}
  await new Promise(resolve=>setTimeout(resolve,delay/2));
 }
 return result;
};
exports.control=options=>{if(options.discovery)Object.assign(discoveryOptions,options.discovery);if(options.pick)Object.assign(pickOptions,options.pick);if(options.crash)state.devices[0].name=null;if(options.uninitializedConflict){const view=profile('00000000-0000-0000-0000-000000000001','Unsaved recovery view','global');view.version=undefined;state={...clone(initial),profiles:[view],settings:{...clone(initial).settings,selectedGlobalId:view.id,allowedApplications:[]},settingsVersion:null,activeId:view.id,verified:false,conflict:true,catalogUninitialized:true,observedAllowed:['C:\\Program Files\\Feeder\\Feeder.exe'],status:'Driver recovery evidence requires review',repositoryIssues:[{file:'C:\\Fixture\\Profiles',message:'Profile catalog is not initialized; activation is blocked and existing evidence was preserved'}]};}if(options.reset){state=clone(initial);unknown=false;disconnected=false;fail='';Object.assign(pickOptions,{delay:0,externalAllowed:''});}if('snapshotDelay'in options)snapshotDelay=options.snapshotDelay;if('deviceError'in options)state.deviceError=options.deviceError;if(options.presentation){state.profiles[1].executablePath=options.executable||state.profiles[1].executablePath;const names=['Simucube 2 Pro','Heusinkveld Ultimate+ pedals','Elgato Stream Deck XL','Logitech BRIO webcam','Arctis headphones','Microphone','Keyboard','Mouse','Xbox gamepad','Unidentified USB interface','Sound Blaster X3','Elgato Cam Link 4K','Elgato Stream Deck Plus','Elgato Stream Deck Pedal','GSI GXL v2 - simOS','Audeze Maxwell XBOX Dongle','Keychron Q1 Pro','MSI MYSTIC LIGHT','LG UltraGear Monitor','SIMAGIC P2000 Haptic','Unknown Shifter','VPC ACE-Torq Rudder','L-VPC MongoosT-50CM3','Logitech USB Receiver','HID-compliant bar code badge reader','Keychron Link'];state.devices=names.map((name,i)=>({id:i===0?'wheel':i===1?'pedals':'presentation'+i,name,detail:'',connected:true,identities:[i===0?'wheel':i===1?'pedals':'presentation'+i],current:i<2?'Hidden':'Visible'}));state.devices.find(d=>d.name==='Keychron Link').hidUsages=[{known:true,page:1,usage:4},{known:true,page:12,usage:1}];state.devices.find(d=>d.name==='SIMAGIC P2000 Haptic').hidUsages=[{known:true,page:1,usage:4}];state.devices.push({id:'remembered',name:'Old steering wheel',detail:'',connected:false,identities:['remembered'],current:'Hidden'});state.profiles[1].deviceRules.push({identity:'remembered',friendlyName:'Old steering wheel',visibility:'hidden'});}if('fail'in options)fail=options.fail;if('unknown'in options)unknown=options.unknown;if('disconnected'in options)disconnected=options.disconnected;if(options.external){state.settingsVersion.hash='external'+(++counter);state.settings.revision++;}if(options.long){state.profiles[1].name='Le Mans Ultimate — endurance championship 日本語 '+('very long name '.repeat(8));state.devices[0].name='Simucube wheel — '+('long device name '.repeat(8));}if(options.many)state.devices=Array.from({length:80},(_,i)=>({...devices[i%4],id:'fixture'+i,identities:['fixture'+i],name:devices[i%4].name+' '+i}));return {...snapshot(),fixtureDiagnostics:{snapshotRequests,activeSnapshots,peakSnapshots}};};

const discoveryOptions={delay:0,createDelay:0,fail:'',path:'C:\\Games\\New game.exe'};
const runningApps=[{path:'C:\\Games\\Endurance\\game.exe',name:'Endurance Racing',pid:42,created:'1234',visible:true,instances:2},{path:'D:\\Tools\\game.exe',name:'Background Game',pid:43,created:'1235',visible:false,instances:1}];
exports.discover=async(command,selection)=>{
 const options={...discoveryOptions};if(options.delay)await new Promise(r=>setTimeout(r,options.delay));if(options.fail)throw Error(options.fail);
 if(command==='list')return {applications:clone(runningApps),unavailable:2,truncated:false,metadataPartial:false};
 const item=runningApps.find(a=>a.path===selection.path);
 return {application:item?clone(item):{path:selection.path,name:selection.path.split(/[\\/]/).pop().replace(/\.exe$/i,''),pid:0,created:'0',visible:false,instances:1}};
};
