import {packager} from '@electron/packager';
import {listPackage} from '@electron/asar';
import fs from 'node:fs/promises';
import path from 'node:path';
const root=path.resolve(import.meta.dirname,'..');
const version=(await fs.readFile(path.join(root,'../ProductVersion.props'),'utf8')).match(/<HidHideProductVersion>([^<]+)/)[1];
const outputs=await packager({dir:root,out:path.join(root,'out'),name:'HidHideProfiles',platform:'win32',arch:'x64',overwrite:true,asar:true,appVersion:version.split('.').slice(0,3).join('.'),buildVersion:version,prune:true,ignore:entry=>entry!==''&&!/^\/(?:dist(?:\/|$)|electron$|electron\/(?:main|preload|app-icons|appearance|application-discovery|engine-requests)\.cjs$|package\.json$)/.test(entry),win32metadata:{CompanyName:'HidHide Profiles',FileDescription:'HidHide Profiles editor',ProductName:'HidHide Profiles'}});
let notices='HidHide Profiles\n\n'+await fs.readFile(path.join(root,'../LICENSE'),'utf8');
for(const name of ['react','react-dom','scheduler','lucide-react']){
 const directory=path.join(root,'node_modules',name);
 const metadata=JSON.parse(await fs.readFile(path.join(directory,'package.json'),'utf8'));
 notices+=`\n\n${name} ${metadata.version}\n\n`+await fs.readFile(path.join(directory,'LICENSE'),'utf8');
}
for(const output of outputs)await fs.writeFile(path.join(output,'resources/THIRD_PARTY_NOTICES.txt'),notices);

for(const output of outputs){
 const entries=listPackage(path.join(output,'resources/app.asar')).map(entry=>entry.replaceAll('\\','/'));
 if(entries.some(entry=>!/^\/(?:dist(?:\/|$)|electron$|electron\/(?:main|preload|app-icons|appearance|application-discovery|engine-requests)\.cjs$|package\.json$)/.test(entry)))throw Error('Unexpected content in packaged editor.');
}
