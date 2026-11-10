import type {Device,HidUsage} from './types.ts';

export const deviceKinds=['wheel','pedals','keypad','streamdeck','streamdeck-plus','footswitch','camera','capture','audio-interface','headphones','microphone','speaker','keyboard','mouse','joystick','gamepad','handbrake','shifter','throttle','rudder','haptic','monitor','lighting','receiver','barcode','unknown'] as const;
export type DeviceKind=typeof deviceKinds[number];
export type ControllerClass='game'|'non-game'|'unknown';
export type DeviceClassification={kind:DeviceKind;label:string;controller:ControllerClass;reason:string};
type Input=Pick<Device,'name'|'detail'|'kind'|'hidUsages'>;
const labels:Record<DeviceKind,string>={wheel:'Steering wheel',pedals:'Racing pedals',keypad:'Button box / keypad',streamdeck:'Shortcut controller','streamdeck-plus':'Shortcut controller with dials',footswitch:'Shortcut footswitch',camera:'Camera',capture:'Video capture','audio-interface':'USB audio interface',headphones:'Headset / headphones',microphone:'Microphone',speaker:'Speakers',keyboard:'Keyboard',mouse:'Mouse',joystick:'Flight stick / joystick',gamepad:'Gamepad',handbrake:'Handbrake',shifter:'Shifter',throttle:'Flight throttle',rudder:'Rudder pedals',haptic:'Pedal haptics',monitor:'Monitor controls',lighting:'Lighting controls',receiver:'Wireless receiver',barcode:'Barcode reader',unknown:'Unclassified device'};
// Ordered product/family rules, never vendor-wide guesses. Sources and coverage:
// docs/device-classification.md. Product purpose is separate from HID capability.
const rules:readonly [RegExp,DeviceKind,ControllerClass][]=[
 [/\bstream\s*deck\s+pedal\b/,'footswitch','non-game'],
 [/\bstream\s*deck\s*(?:plus\b|\+)/,'streamdeck-plus','non-game'],
 [/\bstream\s*deck\b/,'streamdeck','non-game'],
 [/\b(?:cam\s*link(?:\s+(?:4k|pro))?|elgato\s+(?:game\s+capture\s+)?(?:hd60\s*[sx+]*|4k\s*[xs]|4k60\s*pro))\b/,'capture','non-game'],
 [/\bsound\s*blaster(?:x)?\s+(?:x[34]|g[368]|gc7|play!?\s*[34])\b|\bwave\s+xlr\b/,'audio-interface','non-game'],
 [/\baudeze\b.*\bmaxwell\b|\bmaxwell\s+xbox\s+dongle\b|\bhead(?:phone|set)s?\b|\barctis\b/,'headphones','non-game'],
 [/\b(?:wave\s*[: ]\s*[13]|yeti|microphone)\b/,'microphone','non-game'],
 [/\b(?:usb\s+(?:audio|sound)(?:\s+device)?|audio\s+interface|usb\s+dac)\b/,'audio-interface','non-game'],
 [/\b(?:mystic\s+light|rgb\s+(?:lighting|controller)|lighting\s+controller)\b/,'lighting','non-game'],
 [/\b(?:ultragear\s+monitor|monitor\s+controls?)\b/,'monitor','non-game'],
 [/\b(?:sensei\s+ten|mouse|trackball|touchpad)\b/,'mouse','non-game'],
 [/\bkeychron\s+q1\s+pro\b|\bkeyboard\b/,'keyboard','non-game'],
 [/\b(?:usb|wireless|unifying|bolt)\s+receiver\b|\bkeychron\s+link\b/,'receiver','unknown'],
 [/\b(?:webcam|camera|brio)\b/,'camera','non-game'],
 [/\bspeakers?\b/,'speaker','non-game'],
 [/\b(?:bar\s*code|barcode)\b/,'barcode','non-game'],
 [/\b(?:p2000\s+haptic|simagic\s+(?:p[ -]?)?hpr)\b/,'haptic','unknown'],
 [/\b(?:ace[ -]torq|rudder)\b/,'rudder','game'],
 [/\b(?:mongoos[st]?[ -]?t[ -]?50cm3|flight\s+throttle|throttle)\b/,'throttle','game'],
 [/\bhandbrake\b/,'handbrake','game'],
 [/\bshifter\b/,'shifter','game'],
 // A wheel sold with its pedals is identified by the wheel; separate pedal sets stay pedals.
 [/\b(?:steering\s+|racing\s+)?wheel\b.*\bpedals?\b|\bpedals?\b.*\b(?:steering|racing)\s+wheel\b/,'wheel','game'],
 [/\b(?:pedals?|activepedal)\b|\bheusinkveld\b.*\b(?:sprint|ultimate)\b/,'pedals','game'],
 [/\bgsi\s+gxl\s*v?2\b|\b(?:steering|wheel|simucube)\b/,'wheel','game'],
 [/\b(?:joystick|hotas|flight\s*stick|flight\s*yoke)\b/,'joystick','game'],
 [/\b(?:gamepad|game\s+controller|xbox\s+(?:wireless\s+)?controller|dualshock|dualsense)\b/,'gamepad','game'],
 [/\bbutton\s*box\b/,'keypad','game'],
 [/\b(?:keypad|macro\s*(?:pad|keyboard))\b/,'keypad','non-game'],
];

// Numeric top-level collection usages, never localized Windows descriptions.
// Vendor and unfamiliar usages are not evidence of non-game status.
export function usageController(u:HidUsage):ControllerClass{
 if(!u.known||!Number.isInteger(u.page)||!Number.isInteger(u.usage)||u.page<0||u.usage<0||u.page>65535||u.usage>65535)return 'unknown';
 if(u.page===1&&(u.usage===4||u.usage===5||u.usage===8))return 'game';
 if(u.page===2&&((u.usage>=1&&u.usage<=12)||[0x20,0x21,0x24].includes(u.usage)))return 'game';
 if(u.page===5&&[1,2,3].includes(u.usage))return 'game';
 if(u.page===1&&[1,2,6,7,0x80].includes(u.usage))return 'non-game';
 if(u.page===0x0c&&[1,2,4,5,6].includes(u.usage))return 'non-game';
 if(u.page===0x0b&&[1,2,3,4,5].includes(u.usage))return 'non-game';
 if(u.page===0x0d&&u.usage>=1&&u.usage<=6)return 'non-game';
 if(u.page===0x8c&&[1,2].includes(u.usage))return 'non-game';
 return 'unknown';
}

export function classifyDevice(device:Input):DeviceClassification{
 const name=device.name.normalize('NFKC').toLowerCase();
 const match=rules.find(([pattern])=>pattern.test(name));
 const usages=device.hidUsages??[],classes=usages.map(usageController),gameIndex=classes.indexOf('game');
 let kind:DeviceKind=match?.[1]??'unknown';
 if(kind==='unknown'){
  if(usages.some(u=>u.known&&u.page===1&&u.usage===6))kind='keyboard';
  else if(usages.some(u=>u.known&&u.page===1&&u.usage===2))kind='mouse';
  else if(gameIndex>=0)kind=usages[gameIndex].page===1&&usages[gameIndex].usage===5?'gamepad':'joystick';
  // Legacy native hints supply artwork only: false gamingDevice conflated
  // unavailable metadata with non-game devices before numeric usages were sent.
  else if(device.kind&&deviceKinds.includes(device.kind))kind=device.kind;
 }
 let controller:ControllerClass='unknown',reason='No recognized product or game-input collection. Unknown devices stay in view.';
 if(gameIndex>=0){
  controller='game';const u=usages[gameIndex];
  reason=`Reports a game-input HID collection (0x${u.page.toString(16)} / 0x${u.usage.toString(16)}). A composite device may also include non-game controls.`;
 }else if(classes.length&&classes.every(c=>c==='non-game')){
  controller='non-game';reason='All reported HID collections are recognized non-game controls.';
 }else if(match&&match[2]!=='unknown'){
  controller=match[2];reason=`Inferred from the device name: ${labels[kind].toLowerCase()}.`;
 }
 return {kind,label:labels[kind],controller,reason};
}
export const controllerLabel=(value:ControllerClass)=>value==='game'?'Game-input capable':value==='non-game'?'Not a game controller':'Controller type unknown';
export function deviceKind(device:Input):DeviceKind{return classifyDevice(device).kind;}
