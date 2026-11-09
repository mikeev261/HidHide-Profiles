import type {Profile, CatalogProfile, Snapshot, Draft, Device, Settings, Rule, Version} from './types.ts';
import {classifyDevice} from './device-kind.ts';
export const clone = <T,>(value:T):T => structuredClone(value);
export function duplicateName(name:string,maxLen=256):string{const suffix=' copy';if(name.length+suffix.length<=maxLen)return name+suffix;const target=maxLen-suffix.length;const chars=Array.from(name);let len=0,cut=0;for(const ch of chars){if(len+ch.length>target)break;len+=ch.length;cut++;}return chars.slice(0,cut).join('')+suffix;}
export function profileData(p:Profile|CatalogProfile):Profile {
 return {schemaVersion:1,id:p.id,revision:p.revision,name:p.name,kind:p.kind,enabled:p.enabled,priority:p.priority,executablePath:p.executablePath,defaultVisibility:'visible',deviceRules:clone(p.deviceRules)};
}
export function makeDraft(p:CatalogProfile|Profile,s:Snapshot,isNew=false):Draft {
 const profile=profileData(p);
 return {profile,settings:clone(s.settings),original:isNew?null:clone(profile),originalSettings:clone(s.settings),expected:isNew?null:('version' in p?p.version:null),expectedSettings:s.settingsVersion,deleting:false};
}
// Adoption compares the saved settings it replaces. Only a missing catalog (no
// profile files at all) is adopted without them; the engine then creates it.
export function adoptionExpectedSettings(s:Snapshot):Version|null|undefined{return s.settingsVersion??(s.catalogUninitialized?null:undefined);}
export const same=(a:unknown,b:unknown)=>JSON.stringify(a)===JSON.stringify(b);
export const dirty=(d:Draft|null)=>!!d&&(d.deleting||!same(d.profile,d.original)||!same(d.settings,d.originalSettings));
const sameRule=(a:Rule|undefined,b:Rule|undefined)=>a?.identity===b?.identity&&a?.friendlyName===b?.friendlyName&&a?.visibility===b?.visibility;
export function visibility(p:Profile,device:Device,index?:Map<string,Rule['visibility']>):'hidden'|'visible'|'mixed' {
 const values=device.identities.map(id=>(index?index.get(id.toLowerCase()):p.deviceRules.find(r=>r.identity.toLowerCase()===id.toLowerCase())?.visibility)??'visible');
 return values.every(v=>v==='hidden')?'hidden':values.every(v=>v==='visible')?'visible':'mixed';
}
export function deviceRows(profile:Profile,original:Profile|null,devices:Device[]){
 const rules=new Map(profile.deviceRules.map(rule=>[rule.identity.toLowerCase(),rule]));
 const savedRules=original?new Map(original.deviceRules.map(rule=>[rule.identity.toLowerCase(),rule])):null;
 const index=new Map<string,Rule['visibility']>([...rules].map(([id,rule])=>[id,rule.visibility]));
 return devicesFor(profile,devices).map(device=>{
  const value=visibility(profile,device,index);
  const changed=!savedRules||device.identities.some(identity=>{
   const id=identity.toLowerCase(),rule=rules.get(id),saved=savedRules.get(id);
   return !sameRule(rule,saved);
  });
  return {device,value,changed};
 });
}
export function filterDeviceRows<T extends {device:Device}>(rows:T[],hideDisconnected:boolean,connectionKnown:boolean,hideNonControllers=false):T[]{
 if(!hideNonControllers&&!(hideDisconnected&&connectionKnown))return rows;
 return rows.filter(row=>(!hideDisconnected||!connectionKnown||row.device.connected)&&(!hideNonControllers||classifyDevice(row.device).controller!=='non-game'));
}
export function setVisibility(p:Profile,device:Device,value:'hidden'|'visible',original?:Profile|null,explicitDraftRules:ReadonlySet<string>=new Set()):Profile {
 const ids=new Map(device.identities.map(identity=>[identity.toLowerCase(),identity]));
 const saved=new Map(original?.deviceRules.map(rule=>[rule.identity.toLowerCase(),rule])??[]);
 const seen=new Set<string>();
 const rules=p.deviceRules.flatMap(rule=>{
  const id=rule.identity.toLowerCase(),identity=ids.get(id);
  if(identity===undefined)return [rule];
  if(seen.has(id))return [];
  seen.add(id);
  const savedRule=saved.get(id);
  if(original&&value==='visible'&&!savedRule&&!explicitDraftRules.has(id))return [];
  if(savedRule?.visibility===value)return [{...savedRule}];
  return [{...rule,visibility:value}];
 });
 for(const [id,identity] of ids)if(!seen.has(id)){
  const savedRule=saved.get(id);
  if(original&&value==='visible'&&!savedRule&&!explicitDraftRules.has(id))continue;
  rules.push(savedRule?.visibility===value?{...savedRule}:{identity:savedRule?.identity??identity,friendlyName:savedRule?.friendlyName??device.name,visibility:value});
 }
 return {...p,deviceRules:rules};
}
export function devicesFor(profile:Profile,devices:Device[]):Device[] {
 const known=new Set(devices.flatMap(d=>d.identities.map(id=>id.toLowerCase())));
 const remembered=profile.deviceRules.filter(r=>!known.has(r.identity.toLowerCase())).map(r=>({id:r.identity,name:r.friendlyName||r.identity,detail:'Remembered device',connected:false,identities:[r.identity],current:'Unknown' as const}));
 return [...devices,...remembered];
}
export function validation(d:Draft,s:Snapshot):string {
 if(!d.profile.name.trim())return 'Enter a profile name.';
 if(d.profile.name.length>256)return 'Use a profile name of 256 characters or fewer.';
 if(!Number.isInteger(d.profile.priority)||Math.abs(d.profile.priority)>100000)return 'Priority must be a whole number from −100000 to 100000.';
 if(d.profile.kind==='application'&&!/^[a-z]:\\.+\.exe$/i.test(d.profile.executablePath))return 'Choose a local application executable (.exe).';
 const selected=s.profiles.find(p=>p.id===d.settings.selectedGlobalId);
 const selectedDraft=d.profile.id===d.settings.selectedGlobalId?d.profile:selected;
 if(!selectedDraft||selectedDraft.kind!=='global'||!selectedDraft.enabled||d.deleting&&d.profile.id===d.settings.selectedGlobalId)return 'Choose another enabled Global fallback before disabling or deleting this profile.';
 if(s.repositoryIssues.length)return 'Resolve the profile file problems before saving.';
 return '';
}
export function pendingCount(d:Draft):number {
 if(!d.original||d.deleting)return 1;
 let count=0; const a=d.original;
 for(const key of ['name','enabled','priority','executablePath'] as const)if(d.profile[key]!==a[key])count++;
 const rules=new Map(a.deviceRules.map(r=>[r.identity.toLowerCase(),r]));
 const updated=new Map(d.profile.deviceRules.map(r=>[r.identity.toLowerCase(),r]));
 for(const id of new Set([...rules.keys(),...updated.keys()]))if(!sameRule(rules.get(id),updated.get(id)))count++;
 for(const key of Object.keys(d.settings) as (keyof Settings)[])if(!same(d.settings[key],d.originalSettings[key]))count++;
 return Math.max(1,count);
}
