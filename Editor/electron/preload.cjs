const {contextBridge,ipcRenderer}=require('electron');
// The page asks about unsaved work before closing. If it registered no close
// handler (for example it crashed while rendering), close instead of hanging.
let closeHandlers=0;
ipcRenderer.on('editor:request-close',()=>{if(!closeHandlers)ipcRenderer.send('editor:close');});
contextBridge.exposeInMainWorld('hidHide',Object.freeze({
 listApplications:()=>ipcRenderer.invoke('editor:applications-list'),
 describeApplication:path=>ipcRenderer.invoke('editor:application-describe',path),
 validateApplication:application=>ipcRenderer.invoke('editor:application-validate',application),
 cancelApplicationDiscovery:()=>ipcRenderer.send('editor:applications-cancel'),
 getTheme:()=>ipcRenderer.invoke('editor:theme-current'),
 setTheme:theme=>ipcRenderer.invoke('editor:theme',theme),
 request:request=>ipcRenderer.invoke('editor:request',request),
 isVisible:()=>ipcRenderer.invoke('editor:visibility'),
 onVisibility:callback=>{const listener=(_event,visible)=>callback(visible===true);ipcRenderer.on('editor:visibility',listener);return()=>ipcRenderer.removeListener('editor:visibility',listener);},
 getAppIcon:path=>ipcRenderer.invoke('editor:icon',path),
 pick:kind=>ipcRenderer.invoke('editor:pick',kind),
 openFolder:()=>ipcRenderer.invoke('editor:folder'),
 setDirty:dirty=>ipcRenderer.send('editor:dirty',dirty===true),
 close:()=>ipcRenderer.send('editor:close'),
 onClose:callback=>{const listener=()=>callback();closeHandlers++;ipcRenderer.on('editor:request-close',listener);return()=>{closeHandlers--;ipcRenderer.removeListener('editor:request-close',listener);};}
}));
