# Device icons and controller classification

The Electron view separates product type (the icon) from game-input capability.
Classification is presentation-only: it never changes HID path expansion, device
rules, grouping, the exported driver protocol, or the verified driver payload.

## General classifier

The existing native `HidP_GetCaps` call supplies numeric top-level UsagePage/Usage
plus an explicit availability bit. The editor receives these for every collection
in a group. No additional device probes, network requests or polling are added.

1. Any recognized game-input collection makes the group game-capable, even if it
   also contains keyboard/media/vendor controls. This deliberately keeps Keychron
   Link and SIMAGIC P2000 Haptic visible on this host: both report joystick input.
2. Ordered product/category aliases choose artwork and provide an explicitly
   name-inferred controller classification when descriptor evidence is unavailable
   or does not establish game capability. Model rules precede generic words:
   Audeze Maxwell XBOX is a headset, Stream Deck Pedal a shortcut footswitch,
   Sound Blaster X3 an audio interface, and Cam Link 4K a video capture device.
   A wheel sold with its pedals ("Racing Wheel and Pedals") is a steering wheel;
   a separate pedal set remains pedals.
3. All-known non-game collections establish non-game status; vendor, unavailable,
   malformed and unrecognized collections otherwise remain unknown. Multi-axis
   CAD controllers and arbitrary Game Controls page usages are not assumed to be
   game controllers. Generic receiver names remain ambiguous.

Numeric game usages: Generic Desktop joystick/gamepad (01:04/05), Simulation
Controls application collections (02:01-0C,20,21,24), Game Controls application
collections (05:01-03). Legacy native icon hints can supply artwork only; the old
boolean gamingDevice cannot distinguish missing metadata from non-game status.

The view toggle hides only non-game rows, retains unknown rows, combines with the
existing disconnected filter, and persists separately from profiles. Hidden rows'
unsaved and saved rules remain intact. Device details explain the classification;
name-inferred classifications may be imperfect, and Show all devices resets both
filters. This is not a bulk action to hide devices from Windows or games.

## Local product coverage and primary references

| Product / family | Icon category | Name-based capability | Primary source |
|---|---|---|---|
| Creative Sound Blaster X3/X4, G6, GC7, Play!4 | USB audio interface with volume knob | Non-game | [Creative X3](https://us.creative.com/p/sound-blaster/sound-blaster-x3) |
| Elgato Cam Link / Cam Link 4K; HD60 X / 4K X | Video-capture dongle | Non-game | [Cam Link 4K specifications](https://help.elgato.com/hc/en-us/articles/360027963272-Cam-Link-4K-Technical-Specifications) |
| Stream Deck MK.2 / XL; Plus; Pedal | Key grid; grid/dials; footswitch | Non-game | [Stream Deck](https://www.elgato.com/us/en/p/stream-deck), [Pedal](https://www.elgato.com/us/en/p/stream-deck-pedal) |
| Audeze Maxwell XBOX Dongle; Arctis | Headset | Non-game | [Audeze Maxwell](https://www.audeze.com/products/maxwell) |
| GSI GXL v2 | Steering wheel | Game | [GSI setup](https://gomezsimindustries.com/pages/gxl-v2-setup-guide) |
| Simucube 2 Pro; Heusinkveld Sprint / Handbrake | Wheel; pedals; handbrake | Game | Existing local names and joystick/gamepad descriptors |
| VIRPIL MongoosT-50CM3; ACE-Torq | Twin throttle; rudder pedals | Game | [Throttle](https://virpil-controls.eu/vpc-mongoost-50cm3-throttle-b-stock.html), [Rudder](https://virpil-controls.eu/vpc-ace-torq-rudder-pedals.html) |
| SIMAGIC P2000 Haptic | Haptic actuator | Unknown; local HID says game | [SIMAGIC HPR](https://simagic.com/products/p-hpr) |
| Unknown Shifter | H-pattern shifter | Game | Local name plus gamepad descriptor |
| Keychron Q1 Pro | Keyboard | Non-game | [Keychron](https://www.keychron.com/products/keychron-q1-pro-qmk-via-wireless-custom-mechanical-keyboard) |
| Keychron Link; generic Logitech receivers | Receiver | Unknown; Keychron local HID says game | [Logitech receivers](https://www.logitech.com/en-us/discover/a/setup-unifying-receiver) does not establish the exact local model |
| SteelSeries Sensei Ten | Mouse | Non-game | [SteelSeries](https://steelseries.com/en-au/gaming-mice/sensei-ten) |
| LG UltraGear Monitor | Monitor | Non-game | [LG monitors](https://www.lg.com/us/business/ultragear-gaming-monitors) |
| MSI MYSTIC LIGHT | Lighting | Non-game | [MSI](https://us.msi.com/Landing/mystic-light-motherboard/) |
| Generic USB Audio / USB Sound Device | Audio interface | Non-game | Descriptive local product name |
| Generic Virtual HID / unnamed remembered HID interfaces | Neutral USB | Descriptor-dependent / unknown | No unsupported model guess |

All artwork is local vector geometry or the existing Lucide set. No third-party
product photos, trademarks, remote icon service, or unverified VID/PID catalogue
is bundled. Vendor IDs alone do not determine the category.

HID references: [Microsoft HIDP_CAPS](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/hidpi/ns-hidpi-_hidp_caps),
[Top-level collections](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/top-level-collections),
[USB HID Usage Tables](https://usb.org/sites/default/files/hut1_21_0.pdf).

Three Sol researchers received only their scoped instructions, relevant source
paths and product names. Local serial numbers and full instance paths were not
sent to external research sources. Raw local inventory is retained in ignored
artifacts; committed tests use product names and synthetic identities.

## Validation evidence

Read-only enumeration with the rebuilt CLI found 41 local groups: 10 game-capable,
16 non-game and 15 unknown. Every named product above received its intended icon;
unknown remembered interfaces and generic receiver capabilities remain unguessed.
`artifacts/device-classification-local-results.json` records per-name outcomes.

The native/Electron fixture passed six integration groups, including exact numeric
metadata transfer and an unavailable collection, profile persistence, monitoring
with the editor closed and complete frontend shutdown. Five measured editor starts
were 246-274 ms on this host; these are isolated fixture timings, not a physical
device startup benchmark. Electron unit tests cover local names, unfamiliar HID
collections, name precedence, mixed/unknown groups and filter rule preservation.
The presentation suite covers persistent combined filters and dark/light icon
rendering; general acceptance covers resize, zoom, keyboard/dialog and draft flows.
Final unified CI output is in `artifacts/device-classification-ci-final.log`.

The UI review corrections retain stable accessible toggle names and distinguish
device form factor from game-input capability. No physical visibility policy or
installed application was changed; install/upgrade acceptance is separate.
