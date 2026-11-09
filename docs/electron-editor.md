# Electron profile editor

For device-open ordering, the editor can launch a saved application profile's
exact executable through the ordinary-user coordinator. The process remains
suspended until the complete profile is applied and read back, then the coordinator
returns to Automatic, clears any manual override, and tracks the process as an
ordinary activation. Newer applications can supersede its mask. An existing target, an Allowed
apps exemption, stale saved version, paused hiding, maintenance, or unknown driver
state blocks launch. This action does not cover launcher handoffs or extra arguments;
automatic discovery of externally started games remains best effort. See
`testing/app-profile-activation.md` for the outstanding signed-driver procedure.
Run `node tests/launch-order.mjs` from `Editor` for the isolated real-child and
Electron-to-native launch ordering regression. Run `node tests/recent-mask.mjs`
for runtime selection, worker history, override, readback, and dirty-draft coverage.

Candidate 2.1.12 implements the approved charcoal/red design in React and CSS,
with Electron as an independent editor process. The native coordinator continues
to own matching, profile persistence, enforcement, baseline recovery and its tray.
Closing the editor exits Electron instead of minimizing to a tray. Tray Exit is
the separate action that restores the baseline and stops the native coordinator.

## Interaction contract

- Profile rules and global settings are drafts until Apply. Observed driver state
  is shown separately, and becomes Unknown when the engine is unavailable.
- Navigation and closing protect unsaved work with Apply, Discard or Keep editing.
  Repository revision/hash checks prevent an old draft overwriting newer files.
- Import and duplication produce unsaved copies. Allowed apps remain global.
  Recovery adoption stages changed allowed applications for explicit review.
  When no profile file exists yet (for example stale recovery evidence on first
  start), adoption instead creates the catalog from the adopted Allowed apps;
  an incomplete or corrupt catalog still requires repair or restore first.
- Normal desktop layouts retain the profile rail and save bar. Narrow or short
  effective viewports reflow to one scroll surface with a Profiles toggle. Zoom
  does not hide actions; dialogs scroll and trap focus with Escape to return.
- Background-only native startup does not display legacy configuration windows.
  Setup handoff requires the editor to close, preserving unsaved work.

## Boundary and packaging

The Windows 11 x64 public MSI includes `Editor/HidHideProfiles.exe`, the Electron
runtime and `resources/app.asar`. The Start menu still launches the native
`HidHideClient.exe`. Dependencies and Electron are pinned in the npm lockfile.
The renderer loads only local bundled resources via `hidhide://editor`, has a
restrictive CSP, sandboxing/context isolation and no Node integration. Navigation,
new windows and permission requests are denied. The preload exposes a narrow
command bridge, native file pickers and guarded close.

JSON requests use a fixed sibling native executable. Its ordinary-token and
same-user/session checks authenticate the editor; the native application service
validates the domain mutations. A registered live process handle lets maintenance
detect editor ownership without trusting a reused PID. No web renderer or MSI
custom action directly writes profile JSON or performs driver mutations.

## Validation and evidence

Run `npm run build`, `npm test`, and `npm run test:ui` in `Editor`. The UI test
launches the actual Electron renderer with an explicit development-only fixture.
It covers staged vs observed state, CAS-aware navigation, inactive profile saves,
failed saves, dialog failures, disconnect/reconnect, live device details, long
Unicode names, many devices, search, five sizes, repeated maximize/restore and
combined minimum-window/125–200% zoom. Action bounding boxes and segmented-label
widths are asserted; zoom screenshots use native capture to avoid CDP cropping.

`node tests/native-integration.mjs` additionally starts an isolated native host,
uses the actual native JSON bridge and repository service, edits through Electron,
closes every Electron PID, switches automatic profiles without the editor, reopens
and verifies saved/observed policy, then restores the fixture baseline. The host
uses a separate TEMP catalog and authenticated test pipe, fake device/process
sources and fake enforcement. Packaged builds cannot enable the Electron fixtures.

Evidence lives in `artifacts/electron-ui-validation` and
`artifacts/editor-native-validation`. Native acceptance includes nine service
groups. These are executable UI/native integration tests, not proof of physical
driver behavior or a completed install/reboot/uninstall cycle. The existing live
installation is not changed by these tests. Unified Release x64 Ci additionally
builds the MSI and verifies its tables and unchanged signed driver payload.

## Performance and artwork (2.1.13)

The editor keeps one ordinary-user native bridge for its lifetime. Native pipe
servers wait on overlapped I/O events instead of a periodic wakeup. The bridge
exits with the editor; the existing native enforcement owner continues independently.
Snapshots remain authenticated and revalidate the repository. A loaded file's
version is reused within that load, not cached across external file changes.

Renderer polling is single-flight and visibility-aware, with an unchanged-state
backoff from 500 ms to 3 seconds. Focusing or reopening the window refreshes it.
Native visibility events take precedence over a delayed initial visibility query.
Executable icons load only for visible items outside the backend request queue;
failed loads retry, successful icons expire, and extraction concurrency/cache sizes
are bounded. Device category icons preserve the actual device/rule identity.
The disconnected-device filter is local presentation state and never deletes rules.

Additional checks: `node tests/polling.mjs` exercises responses slower than the old
poll interval, hidden-window quiescence, resume and the initial visibility race.
`node tests/presentation.mjs` exercises icon categories, actual local EXE artwork,
filter persistence, saved/draft rule preservation and unknown device connection state.
Set `HIDHIDE_TEST_ICON_EXE` to an existing local executable to verify its shell icon.
See `docs/performance-review.md` for measured results and production limitations.

## Appearance (2.1.14)

The header offers Light mode / Dark mode independently of backend availability or
unsaved profile edits. Both palettes use vivid red accents with neutral surfaces.
Every custom dialog/control/status color is tokenized; native Electron appearance
and the window background use the same choice. Preference saves are atomic and
serialized in `editor-appearance.json` under Electron's ordinary-user data folder.
Reload and startup read the saved choice before mounting the editor. Closing waits
for an in-flight preference save. A failed save retains the previous theme and
shows an error; profile JSON is never involved. The default remains dark.

`node tests/themes.mjs` covers both themes, text contrast, dialogs, compact windows
at 100-200 percent zoom, pending-draft preservation, reload persistence, write
failure recovery and changing themes while the backend is unavailable.
`npm test` also verifies preference storage across separate store instances.

## Retired native UI fixtures

The former MFC Profiles page, theme renderer, private dialog resources and
`Test-ProfilesPresentation.ps1` / `Measure-ProfilesPerformance.ps1` are removed.
Unified `Ci` no longer constructs or shows that retired editor. The resident
engine still uses a hidden MFC window for native notifications and tray commands.

The native restart harness now calls the same `Editor::Service` used by Electron:

| Former native test responsibility | Current coverage |
| --- | --- |
| Apply, abrupt process loss, reload, detached drafts | Cross-process service acceptance and independent JSON parsing |
| Invalid/missing settings, interrupted transactions, initial unknown driver state | Blocked service snapshots, no driver mutation, unchanged repository evidence |
| Backup recovery, save versus activation failure, retry | Service restore and retry assertions |
| Driver adoption, repository races, Allowed apps drafts | Service CAS/adoption/discard/apply assertions |
| Automatic selection, startup integration failures, observed unknown/conflict | Injected coordinator process source and current service snapshots |
| Device notification burst | Production device worker with a hidden engine window and fake device source |
| Old control layout, MSAA/tab order, theme/DPI, dirty prompts and old-window performance modes | Retired with the old page; current Electron `acceptance.mjs`, `themes.mjs`, `presentation.mjs`, `polling.mjs` and `native-integration.mjs` cover the supported editor |

Run editor UI checks explicitly from `Editor` after building native Release x64:
`npm run test:ui`, `node tests/native-integration.mjs`, `node tests/themes.mjs`,
`node tests/presentation.mjs`, and `node tests/polling.mjs`. These tests may display
the current Electron editor. Fake driver/service coverage does not establish
physical-device or installed-driver acceptance.
