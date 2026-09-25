// Electron acceptance for editor state bugs (audit #21, #22, #23) against the
// isolated fixture engine. Requires an interactive desktop; never touches the driver.
import {_electron as electron,expect} from '@playwright/test';
import path from 'node:path';
const root=path.resolve(import.meta.dirname,'..');
const env={...process.env};delete env.ELECTRON_RUN_AS_NODE;
const passed=[];
// Apply and report the engine's rejection, if any, instead of a bare timeout.
async function apply(page){
 await page.getByRole('button',{name:'Apply changes',exact:true}).click();
 const done=page.getByText('No pending changes',{exact:true}),failure=page.getByRole('alert');
 await expect(done.or(failure).first()).toBeVisible();
 if(await failure.count())throw Error('Apply was rejected: '+(await failure.first().innerText()).replace(/\s+/g,' '));
}

const only=process.env.ONLY;
async function session(name,body){
 if(only&&!name.includes(only))return;
 const app=await electron.launch({args:[root,'--fixture'],env,timeout:30000});
 let closed=false;app.on('close',()=>{closed=true;});
 try{
  const page=await app.firstWindow();
  // Optional: keep the test window in the right half of the screen (HIDHIDE_TEST_WINDOW=right).
  if(process.env.HIDHIDE_TEST_WINDOW==='right')await app.evaluate(({BrowserWindow,screen})=>{
   const area=screen.getPrimaryDisplay().workArea,half=Math.floor(area.width/2);
   BrowserWindow.getAllWindows()[0].setBounds({x:area.x+half,y:area.y,width:area.width-half,height:area.height});
  });
  const control=options=>app.evaluate((_electron,value)=>globalThis.__fixtureControl(value),options);
  await control({reset:true});
  await expect(page.getByRole('heading',{name:'Le Mans Ultimate',exact:true})).toBeVisible();
  await body({app,page,control,isClosed:()=>closed});
  passed.push(name);
 }finally{
  // A window that refuses to close must never be left on the desktop.
  if(!closed)await Promise.race([app.close().catch(()=>{}),new Promise(resolve=>setTimeout(resolve,5000))]);
  if(!closed)app.process().kill();
 }
}

// #22: a file picker stays open while saved Allowed apps change outside this
// editor. Adding the picked app must build on the refreshed draft, not undo it.
await session('picked Allowed app keeps an Allowed-app change saved while the picker was open',async({page,control})=>{
 await page.getByRole('button',{name:'Manage allowed apps',exact:true}).click();
 await control({pick:{delay:8000,externalAllowed:'C:\\External\\Feeder.exe'}});
 await page.getByRole('button',{name:'Add application',exact:true}).click();
 await expect(page.getByRole('dialog').getByText('C:\\Games\\New game.exe',{exact:true})).toBeVisible({timeout:15000});
 await page.getByRole('button',{name:'Done',exact:true}).click();
 await apply(page);
 const saved=(await control({})).settings.allowedApplications;
 if(!saved.includes('C:\\External\\Feeder.exe')||!saved.includes('C:\\Games\\New game.exe'))throw Error('Allowed apps after Apply: '+JSON.stringify(saved));
});

// #23: an imported draft must be based on the snapshot current when it is
// created, so Apply is not rejected for a change made while the picker was open.
await session('imported draft applies after settings changed while its picker was open',async({page,control})=>{
 await control({pick:{delay:8000,externalAllowed:'C:\\External\\Feeder.exe'}});
 await page.getByRole('button',{name:'Import profiles',exact:true}).click();
 await expect(page.getByText('Imported as a disabled copy',{exact:false})).toBeVisible({timeout:15000});
 await apply(page);
 const state=await control({});
 if(!state.profiles.some(p=>p.name==='Imported profile'))throw Error('Imported profile was not saved.');
 if(!state.settings.allowedApplications.includes('C:\\External\\Feeder.exe'))throw Error('Import undid an external Allowed-app change.');
});

// #21: if the page crashes while rendering, the window must still close.
await session('editor window closes after the page crashed while rendering',async({app,page,control,isClosed})=>{
 const errors=[];page.on('pageerror',e=>errors.push(e.message));
 await control({crash:true});
 const boundary=page.getByText('The editor stopped unexpectedly',{exact:true});
 await expect.poll(async()=>errors.length+await boundary.count(),{timeout:15000}).toBeGreaterThan(0);
 await app.evaluate(({BrowserWindow})=>BrowserWindow.getAllWindows()[0].close());
 await expect.poll(isClosed,{timeout:10000}).toBe(true);
});

console.log(`${passed.length} editor regression groups passed.`);
