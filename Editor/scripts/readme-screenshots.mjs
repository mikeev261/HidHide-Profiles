// Captures the README screenshots from the development fixture (sample devices
// and profiles; no engine or driver needed). Run after `npm run build`:
//   node scripts/readme-screenshots.mjs
import {_electron as electron,expect} from '@playwright/test';
import fs from 'node:fs/promises';
import path from 'node:path';
const root=path.resolve(import.meta.dirname,'..'),output=path.resolve(root,'../docs/images');
await fs.mkdir(output,{recursive:true});
const env={...process.env};delete env.ELECTRON_RUN_AS_NODE;
const app=await electron.launch({args:[root,'--fixture'],env});
try{
 const page=await app.firstWindow();
 // Fixed content size, placed on the right half of the primary display.
 await app.evaluate(({BrowserWindow,screen})=>{const area=screen.getPrimaryDisplay().workArea,window=BrowserWindow.getAllWindows()[0];
  window.setContentSize(1360,1000);window.setPosition(area.x+Math.round(area.width/2),area.y);});
 await app.evaluate((_electron,value)=>globalThis.__fixtureControl(value),{reset:true,presentation:true});
 const top=()=>page.locator('.editor-scroll').evaluate(element=>element.scrollTop=0);
 const shot=async name=>{await top();await page.waitForTimeout(400);await page.screenshot({path:path.join(output,name)});console.log('wrote docs/images/'+name);};
 const theme=async wanted=>{const toggle=page.getByRole('button',{name:wanted==='dark'?'Switch to dark mode':'Switch to light mode',exact:true});if(await toggle.count())await toggle.click();};
 await expect(page.getByText('Simucube 2 Pro',{exact:true})).toBeVisible({timeout:10000});
 await theme('dark');

 // An application profile: its exact executable plus per-device rules.
 await page.getByRole('button',{name:/Le Mans Ultimate/}).first().click();
 await expect(page.getByRole('heading',{name:'Le Mans Ultimate',exact:true})).toBeVisible();
 const filter=page.getByRole('button',{name:/^Hide non-game controllers/});
 if(await filter.getAttribute('aria-pressed')!=='true')await filter.click();
 await shot('editor-application-profile.png');
 await theme('light');await shot('editor-application-profile-light.png');await theme('dark');

 // A pending edit is visible before it is applied.
 const gamepad=page.getByRole('row').filter({has:page.getByText('Xbox gamepad',{exact:true})});
 await gamepad.getByLabel('Hidden',{exact:true}).check();
 await expect(page.getByText('1 pending change',{exact:true})).toBeVisible();
 await shot('editor-pending-change.png');
 await page.getByRole('button',{name:'Discard',exact:true}).click();
 const keep=page.getByRole('button',{name:'Discard changes',exact:true});if(await keep.count())await keep.click();
 await expect(page.getByText('No pending changes',{exact:true})).toBeVisible();

 // The Global profile used when no application profile is running.
 await page.getByRole('button',{name:'Default',exact:true}).first().click();
 await expect(page.getByRole('heading',{name:'Default',exact:true})).toBeVisible();
 await shot('editor-global-profile.png');

 // Creating a profile from a running application.
 await page.getByRole('button',{name:'New profile',exact:true}).click();
 await page.getByRole('button',{name:'Choose running application',exact:true}).click();
 await expect(page.getByRole('dialog')).toContainText('Endurance Racing');
 await shot('editor-new-profile.png');
}finally{await app.close().catch(()=>{});}
