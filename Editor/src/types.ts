export type Version = {revision:number; hash:string};
export type Rule = {identity:string; friendlyName:string; visibility:'hidden'|'visible'};
export type Profile = {schemaVersion:1; id:string; revision:number; name:string; kind:'global'|'application'; enabled:boolean; priority:number; executablePath:string; defaultVisibility:'visible'; deviceRules:Rule[]};
export type CatalogProfile = Profile & {version:Version|null; running?:boolean; missing?:boolean};
export type Settings = {schemaVersion:1; revision:number; selectedGlobalId:string; mode:'automatic'|'useGlobal'; paused:boolean; startWithWindows:boolean; allowedApplications:string[]};
export type HidUsage = {known:boolean;page:number;usage:number};
export type Device = {kind?:import('./device-kind').DeviceKind;hidUsages?:HidUsage[];id:string; name:string; detail:string; connected:boolean; identities:string[]; current:'Visible'|'Hidden'|'Mixed'|'Unknown'};
export type Snapshot = {profiles:CatalogProfile[]; settings:Settings; settingsVersion:Version|null; activeId:string; verified:boolean; status:string; conflict:boolean; launchedId?:string; runningOrder?:string[]; manualMaskId?:string; requestedId?:string; repositoryIssues:(string|{file:string;message:string})[]; deviceError?:string; adoptionAwaitingSave?:boolean; adoptedSettings?:Settings|null; catalogUninitialized?:boolean; devices:Device[]};
export type Reply = {ok:boolean; error?:string; snapshot?:Snapshot; profile?:Profile; saved?:boolean; applied?:boolean; message?:string; settings?:Settings; needsApply?:boolean};
export type Draft = {profile:Profile; settings:Settings; original:Profile|null; originalSettings:Settings; expected:Version|null; expectedSettings:Version|null; deleting:boolean};
export type Picker = 'executable'|'import'|'backup'|'restore'|'export';
export type ApplicationList = {applications:Application[];unavailable:number;truncated:boolean;metadataPartial:boolean};
export type Application = {path:string;name:string;pid:number;created:string;visible:boolean;instances:number};
declare global {
 interface Window { hidHide: {
  listApplications:()=>Promise<ApplicationList>;
  describeApplication:(path:string)=>Promise<Application>;
  validateApplication:(application:Application)=>Promise<Application>;
  cancelApplicationDiscovery:()=>void;
  getTheme:()=>Promise<'light'|'dark'>;
  setTheme:(theme:'light'|'dark')=>Promise<'light'|'dark'>;
  request:(request:Record<string,unknown>)=>Promise<Reply>;
  isVisible:()=>Promise<boolean>;
  onVisibility:(callback:(visible:boolean)=>void)=>()=>void;
  getAppIcon:(path:string)=>Promise<string|null>;
  pick:(kind:Picker)=>Promise<string|null>;
  openFolder:()=>Promise<string>;
  setDirty:(dirty:boolean)=>void;
  close:()=>void;
  onClose:(callback:()=>void)=>()=>void;
 } }
}
