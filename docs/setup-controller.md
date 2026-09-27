# Historical unified Burn controller

This controller is retained as recovery-design reference and is not shipped by
the current public MSI.

The build now produces a WiX 5.0.2 Burn EXE with one private MSI. This is an
unsigned development artifact, not a lifecycle-validated release. Kernel source,
exported IOCTLs and Microsoft-signed driver bytes remain unchanged.

## Implemented path

`Installer.Bootstrapper` is a .NET Framework 4.8 out-of-process WiX 5 BA. Its main
thread must allow the native bootstrapper API to initialize COM; declaring it
STA caused an actual launch failure found and fixed by the bundle smoke test.
Windows 11 supplies the framework. The BA runs as the ordinary configuration user.

Before elevation, the packaged CLI restores/confirms the same-user baseline and
retains exclusion. A random transaction pipe authenticates both process IDs;
the elevated controller independently reads the initiating peer's token SID. The pipe permits that account and Administrators, allowing alternate administrator credentials while retaining the initiating user's ownership. The bounded READY parser matches the C++ wire format,
including exact UTF-16 code units. No pipe request accepts executable paths,
registry paths, arbitrary commands or an authoritative caller-supplied SID.

The controller retains the live event before handoff, acquires the coordinator
lease, and compares actual driver settings with the confirmed snapshot. It copies
embedded, build-pinned applications, driver files, MSI and available recovery
sources into an administrator/SYSTEM-only, per-transaction cache. Existing files,
ACLs, owners, hashes and reparse paths are checked. The setup journal uses bounded
serialization, protected ACLs and flushed atomic replacement; interrupted writes
are refused. Neither the snapshot nor raw settings are sent to ordinary logs.

Exact recognized legacy products are removed companion-first, outside the MSI
transaction. Complete pinned recovery media is required before the first removal.
The native baseline is compared again before every removal. Intent is durable
before Windows Installer is started, exit codes and registration are checked,
and replacement refuses legacy resource residue. A current unified MSI normalizes
an install request to repair so Burn cannot silently skip its driver transaction.

Before Burn executes the private MSI, the controller releases the coordinator
mutex while retaining the event and durable marker. Each worker reacquires the
lease. The controller records MSI completion, verifies final registration,
driver/control/filter state, restores only an expected unchanged baseline/default
state, and verifies application/runtime bytes. Only then is the driver record
committed and the matching marker removed. An interrupted restore is not replayed.

An ordinary-user completion step updates/removes only a recognized HidHide Run
command. Missing or unrelated commands remain untouched. Pause/preferences and
other users' hives are not modified. An enabled manager is restarted without
elevation only after successful completion and release of all barriers.

## Reboot and failure semantics

Setup never initiates reboot. A 3010 result records a checkpoint and boot identity;
rerunning the **same exact setup artifact** reads the protected transaction.
The same boot cannot advance it. Completed driver prefixes can continue after
re-detection, including an additional filter-stack reboot. Unknown native intents,
unknown MSI outcomes, interrupted legacy uninstall, changed settings, unrecognized
ownership and unsupported damaged resources stop for explicit recovery.
Recognized missing owned resources use the bounded [repair path](unified-driver-repair.md).

The elevated controller has an independent three-minute deadline for native work
and a 31-minute MSI-wait deadline. Burn also supervises the process and bounded
pipe operations. Timeout is an unknown OS outcome: Windows Installer/native work
may have outlived its caller. Journals, recovery media and durable exclusion remain;
termination never proves cancellation and never clears the marker.

Recovery media is retained for explicit repair, but a user-facing legacy restore
workflow and arbitrary interrupted-MSI reconciliation are not implemented. Do not
delete the marker or rerun native commands to guess an outcome.

## Build and verification

```powershell
dotnet run --project Installer.Controller.Tests -c Release
.\build\BuildUnifiedSetup.ps1 -DriverPayload .\artifacts\unified-driver-verified `
  -Out .\artifacts\new-unified-build `
  -UpstreamRecovery .\artifacts\unified-evidence\HidHide_1.5.230_x64.exe
```

Use a new output directory. Optional `Companion1Recovery`/`Companion99Recovery`
inputs must be recognized full MSIs with embedded media; their hashes are embedded
in the controller. If a detected legacy product lacks recovery media, setup refuses
removal. The exact pinned upstream EXE is used only as recovery media, not a chained
legacy installer or a helper. Build acquisition remains responsible for verifying
the original Microsoft catalog and payload membership.

`StageAppRuntime.ps1` stages four signed AMD64 Microsoft runtime DLLs.
All four are app-local MSI files; the three needed by the CLI accompany the BA.
Dependency inspection covers normal and delay imports. This follows Microsoft's
[app-local deployment guidance](https://learn.microsoft.com/en-us/cpp/windows/deployment-in-visual-cpp?view=msvc-170).
They come from the installed toolset's default redist directory
(`VC\Auxiliary\Build\Microsoft.VCRedistVersion.default.txt`, overridable with
`-RedistVersion`), which must not be older than the default compiler toolset.
Each file must carry a valid Microsoft Authenticode signature and be an AMD64 PE;
the redist version, file versions and SHA256 hashes are recorded under `vcRuntime`
in `release-manifest.json` for release-toolchain review. A clean offline machine has not yet exercised this deployment.

The bundle supports `/quiet --inspect-only` for a nonmutating smoke test. It loads
the actual WiX BA, detects packages and runs the extracted controller's `--inspect`;
it never starts maintenance preparation, elevation, planning or application.

Historical development evidence (superseded by the current [progress](unified-package-progress.md)):

- 74 controller checks cover READY parsing, framing, production policy/transaction
  code, ordered migration, persistence failures, changed baseline, reboot/resume,
  unknown outcomes and exclusion retention. These simulate Windows mutations.
- 104 driver transaction/settings checks pass (see driver-lifecycle.md).
- The final actual Burn smoke exits 0. Its earlier COM-apartment failure is retained
  as diagnostic evidence, not counted as a passing check.
- The actual MSI contains ten files, including the four app-local runtimes, and
  passes 29 structure checks. Payload extraction/hash verification is read-only.
- `artifacts/unified-burn-v4/HidHide.Setup.exe` is the current unsigned preview;
  logs are `artifacts/unified-evidence/controller-tests.log`, `burn-build-v4.log`,
  `bootstrapper-build-final.log` and `burn-inspect-only-final.log`.

## Current verification and remaining gates

The 2.0 host lifecycle passed install, reboot continuation, repair, normal
uninstall and reinstall, including GUI maintenance handoff and normal exit.
Those results are recorded in [native testing](native-lifecycle-testing.md).
The current 2.1 implementation adds compatible related-bundle upgrade handling,
missing-resource repair, alternate-administrator ownership, and visible setup
progress with cooperative cancellation. These additions require native matrix
verification against the final artifact; see [release readiness](release-readiness.md).

Cancellation before MSI planning independently verifies the untouched native
journal and installed ownership, retains completed legacy-removal records, and
allows the same setup to resume without repeating removals. Unknown MSI outcomes
and interrupted legacy removal remain explicit recovery cases. Never interpret
retained recovery media as proof of a completed rollback.

Unified CI and optional signing are implemented; the public artifact is explicitly
unsigned. Certificate signing has not been tested end to end. Clean source/build
provenance and final VM acceptance remain required before public release.
