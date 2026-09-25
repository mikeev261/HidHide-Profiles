const {app,BrowserWindow,ipcMain,protocol,net,dialog,shell,session,nativeTheme}=require('electron');
const {spawn}=require('node:child_process');
const path=require('node:path');
const {pathToFileURL}=require('node:url');
const fs=require('node:fs');
const {createAppIconLoader}=require('./app-icons.cjs');
const {createAppearanceStore}=require('./appearance.cjs');
const {createEngineRequests}=require('./engine-requests.cjs');
const MAX=4*1024*1024;
const origin='hidhide://editor';
protocol.registerSchemesAsPrivileged([{scheme:'hidhide',privileges:{standard:true,secure:true,supportFetchAPI:true}}]);
app.setName('HidHide Profiles');
let window,dirty=false,closing=false,inFlight=0,closeWhenIdle=false,repositoryPath=null;
const loadAppIcon=createAppIconLoader(app);
const fixture=!app.isPackaged&&process.argv.includes('--fixture');
const engineLaunching=process.argv.includes('--engine-launching');
const fixtureIndex=process.argv.indexOf('--fixture-native');
const fixtureNative=!app.isPackaged&&fixtureIndex>=0?process.argv[fixtureIndex+1]:null;
if(fixture)globalThis.__fixtureControl=require('./fixture.cjs').control;
if(fixture||fixtureNative)app.setPath('userData',path.join(app.getPath('temp'),'HidHide-Editor-Fixture-'+process.pid));
const native=app.isPackaged?path.resolve(path.dirname(process.execPath),'..','HidHideClient.exe'):path.resolve(__dirname,'../../bin/Release/x64/HidHideClient.exe');
const discovery=require('./application-discovery.cjs').createApplicationDiscovery(native);
const fixtureDiscoveryNative=fixture&&process.argv.includes('--fixture-discovery-native');
const discover=(command,selection)=>fixture&&!fixtureDiscoveryNative?require('./fixture.cjs').discover(command,selection):discovery.run(command,selection);
const commands=new Set(['snapshot','apply','settings','delete','retry','mask','automatic','launch','backup','restore','import','export','new','adopt','abandon-adoption']);
function finishOperation(){inFlight--;if(!inFlight&&closeWhenIdle){closeWhenIdle=false;closing=true;window.close();}}
function validSender(event){if(!window||event.sender!==window.webContents||event.senderFrame!==window.webContents.mainFrame||!event.senderFrame.url.startsWith(origin+'/'))throw Error('Untrusted editor request.');}
let bridge=null;
if(fixtureNative)globalThis.__nativeBridgePid=()=>bridge?.child.pid??null;
function stopBridge(){if(bridge){bridge.child.stdin.end();bridge.child.kill();bridge=null;}}
function exchange(request){
 return new Promise((resolve,reject)=>{
  if(!bridge){
   const child=spawn(native,fixtureNative?['--editor-fixture-session',fixtureNative]:['--editor-session'],{windowsHide:true,stdio:['pipe','pipe','pipe']});
   const state={child,pending:null,buffer:Buffer.alloc(0),error:''};bridge=state;
   const fail=cause=>{if(bridge!==state)return;bridge=null;requests.unregister();if(state.pending){clearTimeout(state.pending.timer);state.pending.reject(cause);state.pending=null;}child.kill();};
   child.on('error',fail);child.stdin.on('error',fail);
   child.stderr.on('data',data=>{if(state.error.length<8192)state.error+=data.toString('utf8');});
   child.stdout.on('data',data=>{
    state.buffer=Buffer.concat([state.buffer,data]);
    if(state.buffer.length>MAX+1){fail(Error('Engine response exceeded its limit.'));return;}
    const end=state.buffer.indexOf(10);if(end<0)return;
    const line=state.buffer.subarray(0,end);state.buffer=state.buffer.subarray(end+1);
    const pending=state.pending;state.pending=null;
    if(!pending){fail(Error('Unexpected profile engine response.'));return;}
    clearTimeout(pending.timer);
    try{pending.resolve(JSON.parse(line.toString('utf8').replace(/^\uFEFF/,'')));}
    catch(cause){pending.reject(cause);fail(cause);}
   });
   child.on('close',code=>fail(Error(state.error.trim()||`Profile engine is unavailable (${code}).`)));
  }
  const state=bridge;
  if(state.pending){reject(Error('Profile engine request overlap.'));return;}
  const timer=setTimeout(()=>{if(bridge===state){state.pending=null;stopBridge();requests.unregister();}reject(Error('The engine did not respond. The outcome may be unknown; reconnect before retrying.'));},15000);
  state.pending={resolve,reject,timer};
  state.child.stdin.write(JSON.stringify(request)+'\n');
 });
}
const requests=createEngineRequests({commands,exchange,fixtureRequest:fixture?require('./fixture.cjs').request:null,
 canStartEngine:()=>!fixtureNative&&!engineLaunching,startEngine,pid:process.pid,isDirty:()=>dirty,
 onSnapshot:snapshot=>{if(snapshot.repositoryPath)repositoryPath=snapshot.repositoryPath;},onStart:()=>{inFlight++;},onFinish:finishOperation});
const request=value=>requests.request(value);
const pickers={
 executable:{title:'Choose application',filters:[{name:'Windows application',extensions:['exe']}],properties:['openFile']},
 import:{title:'Import profile as a disabled copy',filters:[{name:'Profile JSON',extensions:['json']}],properties:['openFile']},
 backup:{title:'Choose a parent folder for the new backup',properties:['openDirectory','createDirectory']},
 restore:{title:'Choose a HidHide Profiles backup folder',properties:['openDirectory']}
};
if(!app.requestSingleInstanceLock()){app.quit();}else{
 app.on('second-instance',()=>{if(window){if(window.isMinimized())window.restore();window.show();window.focus();}});
 app.whenReady().then(async()=>{
  const appearance=createAppearanceStore(app.getPath('userData'));
  nativeTheme.themeSource=await appearance.load();
  protocol.handle('hidhide',request=>{
   const url=new URL(request.url);if(url.host!=='editor'||url.search)return new Response('',{status:403});
   const root=path.resolve(__dirname,'../dist');let file=path.resolve(root,'.'+decodeURIComponent(url.pathname==='/'?'/index.html':url.pathname));
   if(!file.startsWith(root+path.sep))return new Response('',{status:403});return net.fetch(pathToFileURL(file).href);
  });
  session.defaultSession.setPermissionRequestHandler((_contents,_permission,callback)=>callback(false));
  session.defaultSession.setPermissionCheckHandler(()=>false);
  window=new BrowserWindow({width:1320,height:900,minWidth:600,minHeight:560,backgroundColor:appearance.get()==='light'?'#f5f5f5':'#121212',title:'HidHide Profiles',autoHideMenuBar:true,show:false,webPreferences:{preload:path.join(__dirname,'preload.cjs'),contextIsolation:true,nodeIntegration:false,sandbox:true,webSecurity:true,devTools:!app.isPackaged}});
  window.removeMenu();window.webContents.setWindowOpenHandler(()=>({action:'deny'}));
  const announceVisibility=()=>window.webContents.send('editor:visibility',window.isVisible()&&!window.isMinimized());
  window.on('show',announceVisibility);window.on('hide',announceVisibility);
  window.on('minimize',announceVisibility);window.on('restore',announceVisibility);
  window.webContents.on('will-navigate',e=>e.preventDefault());window.webContents.on('will-attach-webview',e=>e.preventDefault());
  window.on('close',e=>{if(!closing){e.preventDefault();window.webContents.send('editor:request-close');}});
  window.webContents.on('render-process-gone',()=>{dirty=false;closing=true;window.destroy();});
  ipcMain.handle('editor:theme-current',event=>{validSender(event);return appearance.get();});
  ipcMain.handle('editor:theme',async(event,value)=>{validSender(event);inFlight++;try{const theme=await appearance.set(value);nativeTheme.themeSource=theme;window.setBackgroundColor(theme==='light'?'#f5f5f5':'#121212');return theme;}finally{finishOperation();}});
  ipcMain.handle('editor:request',(event,value)=>{validSender(event);return request(value).catch(error=>({ok:false,error:error.message}));});
  ipcMain.handle('editor:visibility',event=>{validSender(event);return window.isVisible()&&!window.isMinimized();});
  ipcMain.handle('editor:applications-list',async event=>{validSender(event);return await discover('list');});
  ipcMain.handle('editor:application-describe',async(event,filePath)=>{validSender(event);return (await discover('describe',{path:filePath})).application;});
  ipcMain.handle('editor:application-validate',async(event,selection)=>{validSender(event);return (await discover(selection?.pid===0?'validate-file':'validate',selection)).application;});
  ipcMain.on('editor:applications-cancel',event=>{validSender(event);discovery.cancel();});
  ipcMain.handle('editor:icon',(event,filePath)=>{validSender(event);return loadAppIcon(filePath);});
  ipcMain.handle('editor:pick',async(event,kind)=>{
   validSender(event);if(fixture)return require('./fixture.cjs').pick(kind);
   if(kind==='export'){const result=await dialog.showSaveDialog(window,{title:'Export saved profile',defaultPath:'profile.json',filters:[{name:'Profile JSON',extensions:['json']}]});return result.canceled?null:result.filePath;}
   if(!Object.hasOwn(pickers,kind))throw Error('Unknown file dialog.');
   const result=await dialog.showOpenDialog(window,pickers[kind]);if(result.canceled)return null;
   return kind==='backup'?path.join(result.filePaths[0],'HidHide-Backup-'+new Date().toISOString().replace(/[:.]/g,'-')):result.filePaths[0];
  });
  ipcMain.handle('editor:folder',event=>{validSender(event);return fixture?'':repositoryPath?shell.openPath(repositoryPath):'Reconnect to the profile engine before opening its profiles folder.';});
  ipcMain.on('editor:dirty',(event,value)=>{validSender(event);dirty=value===true;});
  ipcMain.on('editor:close',event=>{validSender(event);if(inFlight){closeWhenIdle=true;return;}closing=true;window.close();});
  await window.loadURL(origin+'/index.html');window.show();
 });
}
function startEngine(){if(!fs.existsSync(native))return false;const child=spawn(native,['--background'],{detached:true,windowsHide:true,stdio:'ignore'});child.on('error',()=>{});child.unref();return true;}
app.on('window-all-closed',()=>app.quit());
app.on('before-quit',()=>{discovery.cancel();stopBridge();});
