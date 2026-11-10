# Installation layout

Current target: HidHide Profiles, Windows 11 x64. One public per-machine MSI
uses the standard Windows Installer UI and is visible in Installed Apps.
Release readiness and supported paths are tracked in docs/release-readiness.md.

| Location | Owned content |
|---|---|
| %ProgramFiles%\HidHide | HidHideClient.exe, HidHideCLI.exe, app-local Microsoft VC/MFC runtime DLLs |
| %ProgramFiles%\HidHide\Editor | Electron runtime, HidHideProfiles.exe and bundled React editor in resources/app.asar |
| %ProgramFiles%\HidHide\Driver | Verified unchanged INF, SYS, CAT and original LICENSE.rtf |
| Windows driver store / System32\drivers | Native installation of the same verified upstream driver |
| Start menu\HidHide Profiles | One shortcut to the native launcher, which opens the Electron editor |
| %ProgramData%\mikeev261\HidHide\Maintenance | Protected setup cache, journals and recovery evidence (compatibility namespace) |
| %LOCALAPPDATA%\HidHide Profiles\Profiles | Initiating ordinary user's versioned `settings.json` and one JSON file per profile |
| %LOCALAPPDATA%\HidHide Profiles\Profiles.writer.lock | Empty hidden lock file for that repository; created once and left in place, including after uninstall. Safe to delete while the engine is stopped |

Closing the editor exits every Electron process. The ordinary-user native
`HidHideClient.exe` coordinator remains responsible for automatic profile matching,
application, baseline recovery and the tray menu. Tray Exit restores the baseline
and stops monitoring; the baseline is also restored at logoff and shutdown. Maintenance requires the editor to close before handoff;
setup never terminates an editor that may hold an unsaved draft.

The profiles-first catalog is authoritative from a fresh install and has no
`ConfigurationV1` migration dependency. Registry runtime data is limited to
driver-only crash recovery and never contains profile JSON. The initiating user's
owned startup entry is updated only after verification. Never read or write this
LocalAppData repository from an elevated or SYSTEM custom action, and never run the
resident configuration manager as SYSTEM or elevated.

Run the MSI normally and approve Windows Installer elevation. A fresh install,
repair, or uninstall can suspend at the standard Windows Installer reboot
boundary; restart when convenient and Windows Installer resumes its protected
transaction. Keep the MSI and recovery material.
Uninstall first removes the native driver while retaining the registered MSI and
application files. Only continuation after native verification removes the apps
and registration. The ordinary-user helper removes exact owned startup commands
at the confirmed uninstall checkpoint; profile files and startup preference remain.
Do not clear maintenance markers or delete registry entries to resolve failures.

The legacy companion and upstream package must be removed normally before a clean
HidHide Profiles installation. The public MSI never deletes registrations or
guesses at legacy ownership.

## Updating the unified package

Run a newer same-family MSI directly; a manual uninstall is not required. When the
verified pinned driver is already healthy, the upgrade replaces applications and
retains the driver without adding a forced restart. A restart requested by actual
driver maintenance or Windows Installer still takes precedence.

Profile definitions and settings stay at the existing ordinary-user LocalAppData
location across upgrade installs. They are never MSI-owned files or recursive
removal targets. Electron appearance preferences likewise live outside the install
directory. Upgrading does not reset or migrate either store in 2.1.14.
