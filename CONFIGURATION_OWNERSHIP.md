# Configuration ownership decision

Scope: Windows 11 x64 companion, using the separately installed signed HidHide
driver. This decision implements execution package 02 and the minimal baseline
and recovery behavior needed to make bounded transactions safe.

## Ownership and command flow

The resident GUI owns one machine-wide named mutex for its lifetime. Another
Windows session cannot start a competing companion coordinator. A CLI acquires
that same mutex for each direct transaction when no coordinator owns it; otherwise
it uses the coordinator's local named pipe. Failure to connect never falls back
to writing the driver behind an active coordinator.

The coordinator owns the baseline, last confirmed effective configuration, current
profile contributions, suspension flag, and conflict status. The Devices tab and
CLI edit the baseline directly. Profile device selections are unioned into a copy
of the baseline; removing a profile never subtracts its devices from baseline
intent. The checkbox displays effective hiding so the user can explicitly turn off
a temporary override even if the saved baseline was already disabled.

Pause restores the baseline; resume waits for a fresh process scan. Explicit
`--cloak-off` or unchecking hiding persists suspension until the tray's resume
action, including across manager restarts. Profile definitions and pause preferences
remain per-user. Read-only CLI commands no longer change the whitelist implicitly.
Whitelist edits are explicit, and normal/inverse driver semantics are preserved.

## Driver transactions and external clients

No proxy retains a driver handle. Each transaction acquires the exclusive control
handle, reads fresh state, compares it with the caller's expected state, writes
changes, reads back, then closes the handle. Contention retries are bounded to
three 25 ms delays. Disabling occurs before other IOCTL changes; enabling occurs
last. IOCTLs are not atomic. Cached confirmed state advances only after read-back.

Unexpected state during an override or pending recovery latches a conflict. The
manager stops reconciliation, rejects CLI writes, and retains recovery data. The
tray offers an explicit action to accept actual driver settings as the new baseline
and remain paused. This action does not write the old baseline over external edits.
When a trustworthy record of what HidHide Profiles applied exists (in-process, or a
parseable same-user recovery record even if it no longer matches the driver), only
what changed outside HidHide Profiles is accepted: each driver flag, and each
blacklist or whitelist entry, takes the observed value where it differs from both
states HidHide Profiles recorded for its last attempt (what it held before, and what
it requested; a failed commit can leave either, or part of each) and otherwise keeps
the original baseline. Profile hiding therefore
never becomes the baseline, and the recovery record is replaced, not cleared, while
the driver still holds it. Without such a record (legacy, unreadable or foreign
evidence) the observed settings become the baseline as a whole and the prior
restoration claim is given up. Ordinary external edits when no override
or recovery is pending are adopted as baseline. An exit requesting restoration
stays resident and reports an error if restoration cannot be confirmed.

The official utility can acquire the driver between transactions. It edits effective
driver state, not companion baseline intent; edits during an override therefore
cause a conflict. The companion cannot revoke existing application device handles
or guarantee that automatic detection beats a game's first device open.

## Local protocol and permissions

The pipe accepts versioned Read, compare-and-commit and payload-free PrepareMaintenance messages. Maintenance preparation is described in [maintenance-session.md](docs/maintenance-session.md).
It carries no executable commands, arbitrary file/registry paths, or process-launch
requests. Both peers verify the same Windows user SID; the pipe DACL permits that
user, rejects remote clients, and uses a first-instance claim. The global mutex
permits authenticated users to synchronize ownership, but does not grant them
access to another user's pipe or profile definitions.

Other local users and named objects:

- The per-user profile repository is serialized by an exclusive `.writer.lock`
  file inside that user's own repository directory, not by a predictable
  `Global\` name that another user could pre-create or deny.
- The MSI handshake events (`Global\HidHide.Profiles.Msi.Ready/Release.<transaction>`)
  may be signalled only by the initiating user, SYSTEM and Administrators.
- Elevated setup moves aside any `C:\ProgramData\mikeev261\HidHide\Maintenance`
  path component that is not a trusted protected directory (another user may
  create ProgramData children), keeps it as evidence, and recreates it protected.
- The machine-wide coordination objects (owner and admission mutexes, the
  maintenance barrier event and the configuration pipe) are shared by design:
  another user's engine or setup may legitimately hold them, which blocks this
  user until it releases them, and a user can hold them the same way on purpose.
  That lasts only while their process runs (the objects cannot outlive it), and
  the pipe client still refuses a server owned by another user. Removing this
  residual denial of service would need a privileged broker service.

Messages are limited to 1 MiB, 4096 entries per collection, and 32767 UTF-16 code
units per string. Parsing rejects truncated data, invalid flags, duplicate entries,
embedded nulls, and trailing bytes. Client I/O has a five-second timeout; server
connections expire after four seconds. A timeout can occur after a write was applied:
the user must refresh before retrying; mutation requests are never blindly replayed.
The server pumps on the existing GUI thread without blocking pipe operations.
Moving slow driver/storage work off that thread remains package 05 work.

## Storage and recovery

Profile saves replace one versioned REG_BINARY value (`ConfigurationV1`) under the
existing per-user AppProfiles key. Legacy values are read until the first save,
then retained but superseded. This avoids delete-all/rewrite data loss; older
companion builds do not understand the new value and must not edit this store.

Before an override transition, a single versioned recovery value records the user
SID, baseline, prior confirmed state, and intended effective state. The value is
flushed before driver writes. Startup restores only if actual state matches a
recorded state; otherwise it preserves the record and reports conflict. Legacy,
malformed, oversized, or unknown-version journals are retained for explicit
resolution. A partial IOCTL outcome that matches neither recorded state is a
conflict, not a claim of successful restoration. Recovery records are cleared only
after confirmed restoration or explicit acceptance of current settings.

An abandoned mutex permits a new coordinator to start. Recovery uses only that
user's journal and verifies its SID. Cross-user takeover does not load another
user's journal; inherited effective settings may require manual resolution. Actual
cross-session/cross-user handoff and power-interruption durability are not validated
by the local same-session tests; package 04 still needs exhaustive fault injection.

## Verification on 2026-09-07

- Windows 11 x64, build 26200 (25H2); non-elevated runtime token.
- Installed driver 1.4.181.0 with a valid Authenticode signature.
- x64 GUI/CLI rebuilt; 19 tests pass, including protocol truncation/limits,
  baseline overlap, suspension policy, mutex exclusion, and real pipe reconnects.
- Resident manager plus CLI read and explicit-disable commands succeeded.
- Independent read-only driver client acquired the handle while manager resident;
  CLI returned nonzero with a contention message while that handle was held, then
  succeeded after release. A second manager refused ownership.
- Live nonexistent-device probe: baseline A, profile A+B, add baseline C, disable:
  actual state restored A+C and remained disabled across manager restart.
- Live external-edit probe: edits preserved, writes rejected, conflict latched,
  journal retained. After returning our test edit to the recorded expected state,
  terminating the owner and restarting without the profile process restored A.
- Test process, driver/profile settings, pause preference, and autostart changes
  were cleaned up and restored. No physical device was selected by these probes.

Local logs: `artifacts/package02-tests.log`, `artifacts/package02-contention.txt`,
`artifacts/package02-live.log`, and `artifacts/package02-conflict.log`. These logs
are evidence of the tested scenarios, not a completed release/integration matrix.

## Integrated feature-branch verification

Merged `feature/app-profiles` at `7681826` and retained its shared device selector,
explicit displayed-value checks and rollback, device refresh retries, and exact
process-path matching with unresolved-path status. The coordinator remains the
single production transaction owner. The retained ConfigurationSession tests
exercise the earlier standalone session implementation; they do not substitute
for testing the coordinator's Windows IPC and live driver paths.

Release x64 GUI, CLI, and Tests rebuilt successfully; all 74 tests passed.
Live tests against the installed signed driver confirmed baseline A plus profile
A+B, adding C, persistent disable across restart, external-edit conflict retention,
rejected conflicting writes, and crash/restart restoration. The conflict harness
was adjusted to allow the CLI's expected nonzero stderr result in Windows
PowerShell. Both probes completed and restored the original driver/profile state,
pause preference, and autostart entry. No physical device IDs were used.

Logs: `artifacts/pr20-merge-build.log`, `artifacts/pr20-merge-live.log`, and
`artifacts/pr20-merge-recovery.log`. Interactive MFC failure injection and the
broader release/integration limits above remain unverified.
