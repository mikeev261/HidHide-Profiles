## Issue #37 ownership and interruption repair — 2026-10-03

The MSI restart obligation is now durable before worker dispatch, including
recovered Prepared repair, and retains the actual work boot anchor after an
interrupted parent. Commit rejects absent saved boot evidence. Application-only
upgrades retain the no-forced-restart policy. Serialized driver-free fixtures
cover both dispatch interruption boundaries. MSI and worker journal reads and
updates now occur under maintenance ownership; queued callers reload the current
prefix and revalidate the marker after acquisition. Ownership is released before
child-worker waits. Driver-free interleavings cover advanced native work, terminal
refusal and legacy anchor retention. Final unified Ci evidence is kept
under `artifacts/issue-37-r4`. Schema-2 XML now requires explicit restart
obligation and actual-work anchor evidence; omission, empty and nil-member
regressions fail closed while legacy XML remains conservatively readable. Real Restart/Fast Startup/hibernation and native
lifecycle acceptance remain unverified. No installed machine state was changed.

# Unified package progress

## Stable restart verification — issue #37

Restart-dependent native continuation and public-MSI commit now require a
clock-independent Windows boot sequence. Legacy timestamp journals retain their
evidence and require an anchored additional restart before continuation. See
[stable restart verification](stable-restart-verification.md) for contract,
migration, driver-free validation and outstanding power-mode/native acceptance.
This source work does not establish installed lifecycle or public release readiness.

## Most recent application mask — 2026-09-22

Candidate 2.1.17 selects the newest activated still-running saved application
profile, falling back through earlier surviving activations to the selected Global.
Runtime manual overrides are authenticated and version-checked; process lifetime,
exact paths, saved eligibility, and complete scans control selection. Edits and
additional instances do not promote applications. Use Global and Pause keep the
activation history current while clearing the pin. Restart reconstructs history.
Verified process handles retain exit times across scans, preserving activation
order and manual pins when another instance starts before its predecessor exits.
Exit evidence is retained through incomplete scans and consumed only with a
complete result. Launch prerequisites and the identity of the successfully created
suspended process are checked before saving Automatic mode, including pending
review of an accepted external baseline. Failed creation preserves settings and
the current mask; later failures restore the prior mode after confirmed abort.
An uncertain child retains the existing conservative policy hold.
The editor distinguishes running profiles, requested masks, verified active masks,
and manual overrides without replacing unsaved drafts.

Successful direct launches retain suspended creation, policy application and fresh
readback before resume, then participate in normal activation order. Launch returns
to durable Automatic mode and clears the pin. Multiple launched processes are
supported; uncertain failures retain conservative protection. Owned live processes
still block setup maintenance. Kernel, signed payload, IOCTL, baseline, ownership,
and installer action boundaries are unchanged.

Validation is isolated native/editor fixtures with simulated enforcement, pending
final build evidence. Installed-driver, physical-input, and owner UI acceptance are
separate and are not claimed by this checkpoint.


## Application selection during profile creation — 2026-09-20

New application profiles choose an EXE before naming, through Browse or a searchable
running-application list with explicit refresh, instance grouping, full paths and
unavailable-process diagnostics. A localized metadata/directory/filename suggestion
remains editable; manual names and detached drafts survive asynchronous discovery.
Only Apply persists the existing profile fields. Global creation is unchanged.

Discovery uses an ordinary-user read-only native helper that exits before coordinator
ownership/driver startup. PID, process lifetime and path are revalidated; discovery
and metadata do not enter the configuration queue or automatic matching scan.
See [application-picker.md](application-picker.md) for bounds and regression fixtures.
No host install, live profile change, or physical-controller acceptance is claimed.

## Direct application launch ordering — 2026-09-20

The editor now offers **Launch with profile** for a saved enabled application
profile. The ordinary-user coordinator creates that exact executable suspended,
applies and reads back the complete policy, then resumes it and holds the profile
by an owned process handle until that process exits. It rejects an existing target,
an Allowed-app target, stale profile/settings versions, paused hiding, maintenance,
and unresolved driver or repository state. Automatic discovery of games started
elsewhere remains best effort. The isolated Windows child-process fixture covers
execution ordering and failure cleanup; physical signed-driver early-open, feeder,
and retained-handle acceptance remains outstanding. No installed driver or profile
was changed by this development work.

## Device categories and controller view filter — 2026-09-19

Candidate 2.1.16 adds distinct audio-interface, video-capture, Stream Deck/dial,
footswitch, shifter, throttle, rudder, haptic, monitor, lighting, receiver and
barcode artwork. Three scoped Sol researchers verified product categories and HID
classification semantics. See docs/device-classification.md for sources, local
coverage and the conservative fallback hierarchy.

Numeric top-level HID usages are carried from existing enumeration into the
editor snapshot. Device type and game-input capability are separate; recognized
game-input collections take precedence over name-inferred capability. Unknown
collections do not silently become non-game devices. A persistent one-click
Hide non-game controllers view filter retains unknowns, composes with Hide
disconnected and preserves all saved and pending profile rules. Device details
explain the classification. The signed driver, grouping/path expansion and
application-only upgrade behavior are unchanged.

The installer license first-paint change in 2.1.15 was accepted by the user before
this work and remains included. The installed application and driver are not
modified by development checks. Validation evidence for this candidate is kept
in artifacts/device-classification-ci.log and artifacts/performance-ui-validation.

## Installer license initial rendering — 2026-09-19

Candidate 2.1.15 replaces the setup license's word-processor RTF with simple
paragraph/font formatting, addressing the documented Windows Installer first-page
blank-until-scroll issue with WixUI_Minimal. The displayed text now matches the
canonical MIT license, including both upstream copyright notices. MSI checks parse
the embedded license control and compare its text to LICENSE. The driver payload
and application-only upgrade/profile-preservation behavior are unchanged.
The x64 Release `Ci` target passed: 162 native tests, 546 managed checks,
245 MSI checks, 7 source-evidence checks and 8 editor tests. The embedded RTF
successfully parses and matches the full canonical license, including Unicode.
Running the new MSI checks against the previous 2.1.14 release fails at the
complex-RTF regression guard, as expected.
An opening-dialog visual check reached WelcomeEulaDlg, but Computer Use returned
a black capture and `GetCursorPos failed: Access is denied (0x80070005)`;
first-paint visual acceptance remains pending. The inspection process was closed
before installation. No install/upgrade lifecycle was executed.

The upstream WiX guidance describes this symptom and the simple-RTF workaround:
https://docs.firegiant.com/wix3/wixui/wixui_customizations/#specifying-a-license-file

## Light/dark appearance and upgrade preservation — 2026-09-19

Candidate 2.1.14 uses vivid red accents with neutral grey/black dark surfaces and
adds a complete light palette. The always-available theme button saves immediately
in the ordinary user's Electron preferences, separately from profile edits, and
restores the selected appearance on launch. Dialogs, inputs, errors, warnings,
selection, focus and native Electron appearance follow the selected theme.

The user reports that the 2.1.13 upgrade completed without manually uninstalling
the older version or restarting Windows. This is host feedback, not a complete
upgrade matrix. The existing application-only upgrade path remains: retain a
healthy pinned driver, skip the forced restart boundary, and preserve any restart
actually requested by native maintenance. Profile JSON remains under the same
per-user LocalAppData root, outside MSI component/removal ownership. Regression
checks now evaluate the emitted MSI upgrade conditions and enforce that separation.
The exact 2.1.14 install/upgrade lifecycle has not been run on the host.

## Editor performance and device presentation — 2026-09-19

Candidate 2.1.13 removes the native pipe polling delay, retains one authenticated
editor bridge per window session, and reuses repository versions within each load.
The window starts alongside native initialization. Renderer snapshots run one at a
time, back off while unchanged, and stop while the native window is hidden. A slow
snapshot can no longer be invalidated indefinitely by the next polling interval.

Application profiles read their actual executable icon asynchronously with bounded
caching. Device icons use product metadata and conservative category matching;
unrecognized devices retain a neutral icon. Hide disconnected is a persistent,
one-click view preference that preserves all saved rules and pending changes.

The reusable Sol specialist is `.codex/agents/hidhide-performance.toml`. Performance
findings, measurements and validation boundaries are in `docs/performance-review.md`.
No installed application or physical driver policy was changed during this work.

## Electron editor reconstruction — 2026-09-19

Candidate 2.1.12 replaces the production MFC profile page with the approved
charcoal/red Electron + React editor. The native coordinator continues monitoring
when all Electron processes have exited. Profile definitions, baseline recovery,
ordinary-user ownership and signed driver payload remain under existing native
services. Editor mutations use authenticated IPC and repository version/hash checks.

UI/UX review identified and repaired draft navigation, recovery staging, status
accuracy, dialog error visibility, control contrast and compact/zoom layout issues.
Evidence and remaining host-validation boundaries are in docs/electron-editor.md.


## Settings uninstall repair — 2026-09-18

Version 2.1.11 fixes the 2.1.10 Settings-removal failure: Windows Installer supplied
an elevated consent token and the preparation action rejected it before
InstallInitialize. Preparation now launches a verified ordinary same-user/session
helper using the authenticated MSI client token, with visible MSI failure reporting.
Uninstall also retains applications and product registration through its native
restart checkpoint, then requests full removal on continuation. This avoids the
old ForceReboot sequence unregistering the product and losing its cached package.
Exact owned startup commands are removed in the ordinary user's context; profiles
and the stored startup preference are preserved.

The user's old 2.1.10 installation was manually removed after their restart. Native
inspection verifies no HidHide node, package, service, SYS, control interface or
filter references. Windows Installer completed the old suspended app transaction;
no product, app directory, shortcut, startup command or continuation remains. Both
profile JSON hashes match their pre-removal backups. Recovery evidence is retained
in `artifacts/uninstall-recovery-20260918`.

Release x64 unified Ci passes: 159 native tests, 532 managed checks, 83 MSI
structure checks and 7 source-evidence checks. Seven token checks also pass under
an elevated parent. A disposable driver-free MSI reproduces the elevated consent
path and proves retained-file/product restart suspension, successful continuation
and error callbacks. Its product, files, marker and stale test continuation were
removed. Evidence: `artifacts/installer-fix-validation/README.md`.

The rebuilt unsigned 2.1.11 MSI includes the existing dark UI and unchanged verified
Microsoft-signed driver. It has not been installed on this host. A complete native
install/reboot/uninstall cycle for this exact artifact remains unperformed; the
driver-free probe is not that acceptance test. Old 2.1.10 in-place upgrade is not
validated because its cached MSI retains the old reboot defect. No publication,
merge or automatic restart occurred.

## Public MSI checkpoint — 2026-09-17

The custom Burn bootstrapper is retired from the release path. Version 2.1.8 is
built as one directly launchable, visible per-machine MSI using WiX's standard
welcome/license, progress, completion, repair and remove dialogs. The former
private-MSI launch condition and `ARPSYSTEMCOMPONENT` hiding are gone.

An immediate ordinary-user action performs profile-coordinator handoff for
repair/uninstall; elevated deferred actions accept only bounded identity fields,
derive fixed protected paths, verify the pinned driver payload, and own the native
transaction. Fresh install has no legacy-package dependency. Windows Installer's
`ForceReboot` suspends fresh install, repair and uninstall before commit, and the
post-reboot action verifies/finishes the driver transaction before
`InstallFinalize`. The package requires interactive MSI UI, so Windows never
restarts without its standard prompt.

Structural CI now asserts standard dialog presence, public visibility, privilege
separation, action ordering and reboot continuation. This checkpoint still needs
a real clean install/reboot/launch/profile-save/uninstall test on the exact MSI.

Current scope: practical, generally working unsigned release tested on the user's local Windows 11 x64 PC. The user explicitly cancelled VM testing and deferred the exhaustive legacy, interrupted-install and power-loss matrix. No automatic publishing or merge. Driver signatures, Secure Boot, IOCTL contracts and user settings remain unchanged.

## Current checkpoint — 2026-09-08 evening

The old local 2.0 preview completed normal uninstall/reboot/resume, exit 0. Strict empty-state verification found no product, node, service, driver package, SYS or HidHide filter references. Evidence: artifacts/local-validation-20260908/host-empty-after-resume.json.

The 2.1.0.0 candidate from clean source 59c03f2 completed install/reboot/resume with exit 0. Transaction 58e29028-e172-4b06-97ba-2a4ab30e39ae is Complete/Committed, with no maintenance marker. One visible Burn entry, private MSI and healthy exact Microsoft-signed driver were verified with Secure Boot enabled.

Authoritative handoff: artifacts/local-validation-20260908/RESUME-AFTER-REBOOT.md. It records the exact candidate path/hash, current marker, actual-user SID, settings backups and startup preference restoration. Use the same candidate for continuation; preserve all journals.

The user's enabled startup preference has been restored to the verified installed app at Program Files/HidHide, replacing the backed-up obsolete test-build command. Codex autostart remains configured. The user's registry settings export immediately after installation matched the pre-test export.

Same-artifact repair completed with exit 0 and no restart, including shutdown and background restart of the running coordinator. Transaction 6bd40111-a0c6-47b5-817f-41ccb9749802 is Complete/Committed. A temporary process profile applied exactly one joystick interface in the real driver's effective state and restored the exact baseline after process exit; original profiles were preserved. The user exercised tray Exit, the process ended, baseline was verified, and no HidHide crash event was found. Evidence is under artifacts/local-validation-20260908.

Reopening the background app exposed a real usability defect: the existing window opened, but the second process also displayed an ownership warning. Commit 85bce35 fixes acknowledged activation within the same user/session and preserves the warning when no local owner responds. Version 2.1.1.0 passed clean unified Ci (95 native tests, managed suites) and 45 MSI checks. Its normal local upgrade and repair both returned 0 without reboot. Installed reopening now opens the existing window and the second process exits 0 with no popup. About correctly distinguishes product 2.1.1.0 from driver 1.4.181.0.

The repaired final build also passed a real effective-state profile activation/restoration check with the original profiles preserved. One repair attempt was refused before mutation while About was open; closing the dialog and retrying succeeded. Close child dialogs before maintenance. Both attempt logs are retained.

Final setup: artifacts/local-release-validation-85bce35/artifacts/local-test-candidate/HidHide_2.1.1_x64.exe. SHA256: 3D2FEB6F1C6B31FEC54FC013819820BFBA77880E04984C1A5FE8F5E276B481AC. Clean source: 85bce353ce5c9100e9b154aeed30c1610a3360fe. All three recovery media are included. This exact artifact is now installed; use it for maintenance.

## Verified and remaining

Clean unified Ci and full unsigned packaging passed for this candidate, including native/managed suites and45MSI checks. Owned code is unsigned; the pinned upstream driver remains Microsoft-signed. The earlier VM candidate passed clean offline install/reboot/resume and repair; this is separate evidence, not a substitute for the current local candidate.

Practical local installation, repair, reopening and automatic profile state changes have been validated. Controller enumeration and effective profile settings were exercised locally; actual game input remains a user check. Standalone removal and normal tray exit of the final candidate were not repeated (normal removal passed on 2.0 and normal tray exit on 2.1). The broader failure matrix remains untested. No further restart or setup action is currently required.

The broader release matrix remains documented in docs/release-readiness.md as deferred coverage. Historical host/VM results are in docs/native-lifecycle-testing.md and docs/release-vm-testing.md; implementation history is in docs/unified-package-history.md. No further VM work is requested.

## Source checkpoint — 2026-09-13

Live restart evidence disproved the 2.1.3 persistence claim. The installed client
matched the reviewed 2.1.3 bytes, the ordinary-user `ConfigurationV1` blob remained
valid and contained the F1 profile plus two device paths, but the resident
coordinator served an empty device set. The AppProfiles key was written about two
seconds after process start, while no runtime key remained afterward. This proves
that startup reconciliation still reached the profile writer; a surviving or
missing recovery journal alone does not explain the failure.

Version 2.1.4 separates the profile catalog transaction from driver transitions.
Startup, scan, recovery, pause, and exit now commit a `DriverConfiguration` value
that cannot contain profiles. Only an explicit profile mutation can write
`ConfigurationV1`, after an expected/live conflict check and read-back. Coordinator
reads refresh the durable catalog independently. Focused regression coverage
asserts that an automatic startup transition exposes only driver fields to its
commit callback and retains both device associations.

The same 2.1.3 install also logged a post-MSI bootstrapper crash when disposal
invoked a form whose STA loop had already ended. Disposal is now idempotent and
skips cross-thread invocation after close, with a completed-window regression.
The displayed product and publisher name is now **HidHide Profiles**. Historical
maintenance registry/directories, bundle/MSI upgrade identities, driver/service
identity, and legacy companion paths remain unchanged for recovery compatibility.
This source checkpoint is not installed or live-validated.

## Profiles-first source checkpoint — 2026-09-13

The client now opens directly into the native Profiles workspace. A fresh ordinary-
user JSON repository owns complete Global/application policies and global Allowed
apps; the CLI exposes no profile mutation verbs. Apply uses revision-plus-SHA CAS and
recoverable multi-file replacement. The single `HidHideClient.exe` process hosts
both UI and coordinator, so terminating that process destroys both roles.

Automatic discovery uses one process snapshot per active scan, exact verified paths,
PID-plus-creation-time caching, and changed-only UI notification. Scanning sleeps
indefinitely when Automatic matching is unnecessary. Driver recovery remains a
driver-only journal, and requested state is not presented as observed without a
fresh readback. The nine-mode isolated Release performance matrix passed its
ten-minute CPU and memory budgets with deterministic no-op enforcement. Installed-
driver, physical-input and VM lifecycle acceptance remain outstanding.

## Recovery UX checkpoint — 2026-09-13

Version 2.1.5 fixes a host-reproduced post-reboot uninstall dead end. The 2.1.4
private MSI was removed successfully and its native driver removal requested a
restart. After reboot, a later candidate correctly found the protected transaction
but incorrectly compared its current application/MSI cache index to the earlier
transaction's cache. That rejected continuation with
`failure/maintenance/InvalidDataException/0x80131501`.

A later setup may now finish a prior uninstall with a different package only when
the journal proves the MSI removal already completed and the transaction is in a
post-MSI uninstall phase. It verifies the prior MSI against the digest stored in
that journal and separately verifies the unchanged signed-driver payload. Install,
upgrade, legacy migration, pre-MSI, failed-MSI and ambiguous recovery phases remain
bound to their originating package. The recovery window exposes one normal
continuation action by default; it labels a known pending uninstall as **Finish
uninstall**, and offers legacy restoration only when protected metadata says a
legacy migration actually exists. Existing recovery evidence is not deleted.

## Windows setup UX and restart checkpoint — 2026-09-13

Live 2.1.5 evidence showed a successful MSI followed by controller exit `3010`.
The protected maintenance marker correctly remained while the user launched the
app before restarting, but the custom completion screen did not communicate the
required next action clearly enough.

Version 2.1.6 uses a conventional welcome, progress, and completion layout with
explicit Install, Repair, and Uninstall actions. Restart-required completion now
offers **Restart now** and **Restart later**, explains that setup must be run again
after sign-in, and records explicit restart consent before requesting a Windows
restart. Durable display metadata distinguishes a restart-required transaction;
the application reports actionable restart-and-resume guidance while retaining
the same fail-closed configuration guard.

The Release CI covers the new window states and restart choice without restarting
or otherwise mutating the test host. This source checkpoint still requires live
upgrade and reboot/resume validation.

## Profiles client startup repair — 2026-09-14

Live 2.1.5 launch evidence and a first-chance debugger trace identified the
startup failure before the later `0xc0000409` report. The production enforcement
adapter left its maintenance admission and barrier names empty, while every
isolated adapter test supplied explicit test names. Initial profile reconciliation
therefore called `OpenEventW` with an empty name and let the resulting fail-closed
maintenance exception escape the MFC initialization path.

Version 2.1.7 routes production construction through the same transport
normalization used by tested adapters and gives the transport safe production
defaults. Maintenance-inspection failures now remain fail-closed as a visible
profile status instead of terminating the application, and a future top-level
standard exception reports its actual diagnostic text. The production-adapter
acceptance phase checks the default names and injects a maintenance-inspection
failure through the real native window. A controlled host launch confirmed that
the rebuilt client remained alive past initialization against the installed
driver; normal tray exit and installed-package behavior remain user acceptance
gates for the exact 2.1.7 artifact.
