// Editor request pipeline to the profile engine: command validation, size
// bound, serialized exchange, one engine start on direct launch and editor
// registration. The exchange and process hooks are injected for tests.
// Protocol::MaxBytes in Shared/Configuration.h: the native bridge drops a
// larger request and exits, so refuse it here with a clear message.
const REQUEST_LIMIT=1024*1024;
function createEngineRequests({commands,exchange,fixtureRequest,canStartEngine,startEngine,pid,isDirty,onSnapshot,onStart,onFinish,delay=ms=>new Promise(resolve=>setTimeout(resolve,ms))}){
 let registered=false,engineStartAttempted=false,engineInstance=null,queue=Promise.resolve();
 function request(value){
  const next=queue.then(async()=>{
   if(!value||typeof value!=='object'||!commands.has(value.command))throw Error('Invalid editor command.');
   if(Buffer.byteLength(JSON.stringify(value))>=REQUEST_LIMIT)throw Error('This request exceeds the profile engine limit of 1 MiB.');
   onStart();try{
    if(fixtureRequest)return await fixtureRequest(value);
    let result=await exchange(value);
    // Direct editor launches can create the resident native owner once. Native
    // ownership admission still decides whether this ordinary-user process runs.
    if(value.command==='snapshot'&&!engineStartAttempted&&canStartEngine()){
     engineStartAttempted=true;
     if(!result.ok&&startEngine()){
      for(let attempt=0;attempt<4&&!result.ok;attempt++){await delay(300);result=await exchange(value);}
     }
    }
    if(result.snapshot){
     onSnapshot(result.snapshot);
     // A restarted resident engine has no registration for this editor.
     if(result.snapshot.engineInstance&&result.snapshot.engineInstance!==engineInstance){engineInstance=result.snapshot.engineInstance;registered=false;}
    }
    if(!result.ok){registered=false;return result;}
    if(!registered){const registration=await exchange({command:'editor-state',pid,dirty:isDirty()});if(!registration.ok)throw Error(registration.error);registered=true;}
    return result;
   }catch(error){registered=false;throw error;}finally{onFinish();}
  });queue=next.catch(()=>{});return next;
 }
 return {request,unregister(){registered=false;}};
}
module.exports={createEngineRequests,REQUEST_LIMIT};
