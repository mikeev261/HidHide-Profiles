# Stable restart verification (issue #37)

The issue is valid: a WMI boot timestamp depends on wall-clock interpretation.
Timestamp inequality could authorize forward/rollback continuation after a clock
correction in the same boot, and equality could refuse a later boot. The current
public MSI caller is `Installer/DirectMsiActions.cs`; historical `DirectMsiFlow`
references are obsolete.

The Windows 11 x64 worker now reads the clock-independent `ULONG BootId` from
`KUSER_SHARED_DATA` through read-only `ReadProcessMemory`. The supported shared
user page is at `0x7ffe0000`, with BootId at offset `0x2c4`. The installed Windows
SDK 10.0.28000.0 `km/ntddk.h` defines BootId between AlternativeArchitecture
(offset `0x2c0`) and SystemExpirationDate (offset `0x2c8`), with assertions for
those surrounding offsets. No kernel code, WDK build dependency, driver IOCTL,
elevation, trust change or signed-payload change is introduced.

Primary references:

- [Microsoft KUSER_SHARED_DATA](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/ns-ntddk-kuser_shared_data) documents BootId as the loader's boot sequence.
- [Microsoft shared user page debugger reference](https://learn.microsoft.com/en-us/windows-hardware/drivers/debuggercmds/-kuser) identifies the shared page and address.
- [Microsoft startup-mode reference](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/distinguishing-fast-startup-from-wake-from-hibernation) distinguishes cold initialization from restored kernel/driver memory.

The canonical identity is `winboot-v1:<unsigned decimal sequence>`. Zero and
32-bit wrap are representable. Equality always blocks restart-dependent work;
only two valid stable identities can prove a change. No timestamp, timezone,
uptime estimate or malformed value is a fallback. A provider read failure stops
setup before continuation/commit. A restart-dependent native operation still
requires the existing independently verified completed prefix and driver state.

New driver journals use schema 2. Schema-1 numeric BootId values remain readable and are retained verbatim as
recovery evidence. On first modern continuation they gain a separate protected
RestartAnchor in the current boot, retain their pending status and exclusion,
and perform no native steps. The user must then use **Restart** and retry. A
format mismatch never proves a restart. Anchoring promotes the journal to schema 2 so an old executable rejects the unknown schema instead of ignoring the new protected restart evidence. Applied legacy journals similarly gain
a durable commit restart requirement; they cannot immediately clear maintenance.
Prepared work resumed later and newly initiated rollback record the current boot
before new native work, preventing reuse of stale preparation identity.

The public MSI also persists and checks its own forced-restart requirement when
the native phase returns Applied. The original lifecycle obligation is saved
before worker dispatch, survives Prepared/Applied serialization and parent
interruption, and is anchored by the worker in the boot where work executes.
Missing or malformed saved boot evidence cannot authorize commit. An MSI AFTERREBOOT property alone cannot
commit or clear the exclusion. Application-only upgrades retain their existing
no-forced-restart policy. Historical controller restart comparisons now require
stable identities too; its older numeric setup/legacy recovery checkpoints stop
conservatively for explicit recovery rather than automatically advancing.

Use Windows **Restart**, not a Fast Startup shutdown. Microsoft documents that
Fast Startup and hibernation restore kernel/driver memory. Retained boot identity
is rejected; fixture coverage proves this equality behavior. The primary BootId
reference does not explicitly specify every resume mode's counter behavior, so
this implementation's exact Windows 11 hibernation/Fast Startup observations
remain a native acceptance gate. Fixtures and repeated live same-boot reads do
not establish that power-mode behavior or a real driver reboot/resume lifecycle.

Public MSI preparation, finalization, rollback completion and worker execution
load and validate their transaction inside the maintenance ownership interval.
The production `OwnedJournal` scope acquires before loading, rejects committed
transactions and releases ownership on load/validation failure. MSI releases
ownership before waiting for a child worker, which independently reloads after
acquisition. A queued caller therefore cannot overwrite an advanced native prefix,
restart anchor or legacy migration with its earlier snapshot. Recovery revalidates
the protected marker after waiting for the mutex and never recreates an absent
marker. Initial MSI restart intent is saved before publishing the durable marker.

Validation artifacts are isolated under `artifacts/issue-37-r1`, `artifacts/issue-37-r2`
`artifacts/issue-37-r3` and `artifacts/issue-37-r4`. Driver-free tests
cover the old predicate witness, malformed/current/legacy values, counter wrap,
forward/inverse recovery, serialization/reload, MSI-only commit gating and stale
Prepared work, worker advancement during ownership acquisition, terminal-state
refusal and a queued legacy finalizer retaining the prior restart anchor.
Opt-in `--boot-identity-smoke` performs only repeated provider
reads. No host clock, restart, installation, real journal, profile, startup,
maintenance registration, live IOCTL or trust state is changed by these checks.

Schema-2 serialized records must explicitly carry CommitRebootRequired, including
false for an application-only Upgrade. Deserialization tracks presence separately
from the bool value. Missing/nil members and empty actual-work RestartAnchor fail
closed before continuation, commit or backend mutations. An empty anchor is valid
only for untouched Prepared/no-work completion. Schema 1 may omit these members;
its first migrated checkpoint retains original evidence and demands another
verified boot. New apply/rollback APIs require an explicit current boot identity.
Production-format XML regressions cover omission, empty and nil members for
forward, inverse and commit gates, normal true/false roundtrips and real schema-1
member omission. These checks use isolated journals and fake backends.
