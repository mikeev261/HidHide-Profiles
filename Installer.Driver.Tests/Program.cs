using HidHide.DriverSetup;
using HidHide.Installer;
using Microsoft.Win32;
using System.Runtime.InteropServices;

int checks = 0;
void Check(bool value, string name) { if (!value) throw new Exception(name); checks++; }
void Reject(Action action, string name) { bool threw = false; try { action(); } catch { threw = true; } Check(threw, name); }
TransactionRecord Record(Fake backend, Operation operation) => new() { Id = Guid.NewGuid(), InitiatingSid = "S-1-5-21-1-2-3-1001", Operation = operation, BootId = "1", Before = backend.Inspect() };

// Real Win32 sharing witness using the same opener as Inspect, never the driver.
var sharingPath = Path.Combine(Path.GetTempPath(), "HidHide-sharing-" + Guid.NewGuid() + ".tmp");
File.WriteAllText(sharingPath, "isolated sharing witness");
try
{
    using (var existing = new FileStream(sharingPath, FileMode.Open, FileAccess.ReadWrite, FileShare.ReadWrite))
    {
        using var inspection = ControlHandle.OpenInspection(sharingPath);
        Check(!inspection.IsInvalid, "inspection shares with compatible read/write handle");
        using var restoration = ControlHandle.OpenRestoration(sharingPath);
        int error = Marshal.GetLastWin32Error();
        Check(restoration.IsInvalid && error == 32, "exclusive restoration remains excluded by another handle");
    }
    using (var existing = new FileStream(sharingPath, FileMode.Open, FileAccess.Read, FileShare.None))
    {
        var started = System.Diagnostics.Stopwatch.StartNew();
        bool refused = false;
        try { using var inspection = ControlHandle.OpenInspection(sharingPath); }
        catch (System.ComponentModel.Win32Exception error) { refused = error.NativeErrorCode == 32; }
        Check(refused, "persistent sharing contention fails inspection instead of reporting absence");
        Check(started.ElapsedMilliseconds >= 900 && started.ElapsedMilliseconds < 5000, "inspection contention retries are bounded");
    }
    var transient = new FileStream(sharingPath, FileMode.Open, FileAccess.Read, FileShare.None);
    var release = System.Threading.Tasks.Task.Run(() => { System.Threading.Thread.Sleep(200); transient.Dispose(); });
    try
    {
        using var inspection = ControlHandle.OpenInspection(sharingPath);
        Check(!inspection.IsInvalid, "inspection succeeds after transient exclusive handle closes");
    }
    finally { release.GetAwaiter().GetResult(); transient.Dispose(); }
    using (var restoration = ControlHandle.OpenRestoration(sharingPath))
        Check(!restoration.IsInvalid, "exclusive restoration opens after conflicting handle closes");
    using var missing = ControlHandle.OpenInspection(sharingPath + ".missing");
    int missingError = Marshal.GetLastWin32Error();
    Check(missing.IsInvalid && missingError == 2, "inspection preserves genuine absence");
}
finally { File.Delete(sharingPath); }

var fresh = new Fake(); var journal = new MemoryJournal(); var record = Record(fresh, Operation.Install);
var deleting = Fake.Healthy(); deleting.State.ServicePendingDeletion = true;
Check(!deleting.State.CoreHealthy && !deleting.State.CanInstall, "Pending deletion is observable but never a healthy/fresh driver");
Check(new DriverState { BinaryHash = Payload.SysHash }.CanInstall, "Exact unregistered upstream SYS can be reused");
Check(!new DriverState { BinaryHash = "unknown" }.CanInstall, "Unknown orphan SYS is preserved");
Check(!new DriverState { BinaryHash = Payload.SysHash, ServiceExists = true }.CanInstall, "Registered service is not a fresh installation");
Check(!new DriverState { BinaryHash = Payload.SysHash }.Empty, "Uninstall still requires removal of SYS");
var leftover = new Fake(); leftover.State.BinaryHash = Payload.SysHash;
Check(new DriverTransaction(leftover, new MemoryJournal(), Record(leftover, Operation.Install)).Apply() == JournalStatus.RebootRequired, "Install over exact inert upstream file");
var transaction = new DriverTransaction(fresh, journal, record);
Check(transaction.Apply() == JournalStatus.RebootRequired, "Filter attachment must request reboot");
Check(fresh.State.Healthy, "Fresh state verifies root, binding, service, filters and control");
Check(fresh.Calls.SequenceEqual(new[] { "stage", "create", "bind", "filter0", "filter1", "filter2" }), "Install ordering");
Check(record.Steps.All(x => x.Completed), "Completed operations journaled");
Check(journal.Writes > fresh.Calls.Count * 2, "Intent and completion are separate durable writes");
Reject(() => transaction.Apply(), "No replay after reboot result");
Check(transaction.Rollback() == JournalStatus.RollbackRebootRequired && fresh.Calls.Count == 6, "Rollback direction recorded without crossing reboot boundary");
Check(record.Before.Empty, "Initial snapshot not mutated by execution");

foreach (var operation in new[] { Operation.Repair, Operation.Upgrade }) {
 var backend = Fake.Healthy(); var saved = Record(backend, operation);
 Check(new DriverTransaction(backend, new MemoryJournal(), saved).Apply() == JournalStatus.Applied, "Healthy driver retained");
 Check(backend.Calls.Count == 0, "Repair/upgrade do not reinstall or reset settings");
 Check(!saved.Reboot, "Healthy retained driver does not invent a reboot during repair/upgrade");
}
var repairFilters = Fake.Healthy(); repairFilters.State.Filters[1].Entries = new[] { "VendorA", "VendorB" };
var repairRecord = Record(repairFilters, Operation.Repair);
Check(new DriverTransaction(repairFilters, new MemoryJournal(), repairRecord).Apply() == JournalStatus.RebootRequired, "Missing filter repair requests reboot");
Check(repairFilters.Calls.SequenceEqual(new[] { "filter1" }), "Filter repair does not rebind driver or reset baseline");
Check(repairFilters.State.Settings!.Active, "Filter repair preserves active baseline");
foreach (string missing in new[] { "node", "package", "service", "binary", "control" })
{
    var backend = Fake.Healthy();
    backend.State.Settings = new DriverSettings { Active = true, Inverse = true, Whitelist = SettingsCodec.Encode(new[] { "feeder" }), Blacklist = SettingsCodec.Encode(new[] { "device" }) };
    if (missing == "node") backend.State.Nodes = Array.Empty<Node>();
    if (missing == "package") backend.State.Packages = Array.Empty<string>();
    if (missing == "service") backend.State.ServiceExists = false;
    if (missing == "binary") backend.State.BinaryHash = "";
    if (missing == "control") { backend.State.ControlAvailable = false; backend.State.Nodes[0].Problem = 10; }
    var saved = Record(backend, Operation.Repair); saved.BootId = "1";
    var engine = new DriverTransaction(backend, new MemoryJournal(), saved);
    Check(engine.Apply() == JournalStatus.RebootRequired, "reconstruct missing " + missing);
    Check(!backend.Calls.Any(x => x.StartsWith("remove")), "repair never tears down " + missing);
    Check(backend.Calls.Count(x => x == "stage") == (missing == "package" ? 1 : 0) && backend.Calls.Count(x => x == "create") == (missing == "node" ? 1 : 0), "repair creates only missing resource " + missing);
    Reject(() => engine.Rollback(), "repair reboot cannot be destructively reversed " + missing);
    Check(engine.ResumeAfterReboot("2") == JournalStatus.Applied, "repair verifies retained/new identities after reboot " + missing);
    Check(saved.Before.Settings!.Active && backend.State.Settings!.Same(DriverTransaction.ExpectedAfterBinding(saved.Before.Settings)), "repair preserves baseline and verifies expected INF reset " + missing);
    Reject(() => engine.Rollback(), "completed repair is not rolled back by deleting repaired driver " + missing);
}
{
    var values = new Dictionary<string, (RegistryValueKind? Kind, object? Value)> {
        ["Active"] = (RegistryValueKind.DWord, 1), ["WhitelistedFullImageNames"] = (RegistryValueKind.MultiString, new[] { "feeder" }), ["BlacklistedDeviceInstancePaths"] = (RegistryValueKind.MultiString, new[] { "device" }) };
    DriverSettings ReadStored() => StoredDriverSettings.Read(name => values.TryGetValue(name, out var value) ? value : (null, null));
    var stored = ReadStored();
    Check(stored.Active && !stored.Inverse && SettingsCodec.Decode(stored.Whitelist).Single() == "feeder", "stored baseline uses pinned-driver defaults for absent inverse only");
    values["Active"] = (RegistryValueKind.String, "1"); Reject(() => ReadStored(), "stored flag type rejected");
    values["Active"] = (RegistryValueKind.DWord, 2); Reject(() => ReadStored(), "stored flag value rejected");
    values["Active"] = (RegistryValueKind.DWord, 1); values["WhitelistedFullImageNames"] = (RegistryValueKind.MultiString, new[] { "same", "same" }); Reject(() => ReadStored(), "stored duplicate list rejected");
    values.Remove("BlacklistedDeviceInstancePaths"); Reject(() => ReadStored(), "missing stored baseline list rejected");
}
var uninstall = Fake.Healthy(); var removal = Record(uninstall, Operation.Uninstall);
Check(new DriverTransaction(uninstall, new MemoryJournal(), removal).Apply() == JournalStatus.RebootRequired, "Uninstall filter changes report reboot");
Check(uninstall.Calls.SequenceEqual(new[] { "filter0", "filter1", "filter2", "remove-node", "remove-package" }), "Detach before deletion");
Check(uninstall.State.Empty, "Uninstall resources removed");
Check(removal.Before.Settings!.Active, "Original baseline preserved in journal");
Check(uninstall.State.Filters.All(x => x.Entries.SequenceEqual(new[] { "VendorA", "VendorB" })), "Unrelated filters preserved");

foreach (var failure in new[] { "stage", "create", "bind", "filter0", "filter1", "filter2" }) {
 var backend = new Fake { Fail = failure }; var saved = Record(backend, Operation.Install);
 var engine = new DriverTransaction(backend, new MemoryJournal(), saved);
 Reject(() => engine.Apply(), "Failure propagated: " + failure);
 Check(saved.Status == JournalStatus.RecoveryRequired, "Failure retains recovery state");
 Check(!saved.Steps.Last().Completed, "Unknown outcome recorded as intent only");
 Reject(() => engine.Apply(), "Unknown step never replayed");
 Reject(() => engine.Rollback(), "Unknown ownership never guessed");
}
var detachFailure = Fake.Healthy(); detachFailure.Fail = "filter1";
var detachRecord = Record(detachFailure, Operation.Uninstall);
Reject(() => new DriverTransaction(detachFailure, new MemoryJournal(), detachRecord).Apply(), "Detach failure propagates");
Check(!detachFailure.Calls.Contains("remove-node") && !detachFailure.Calls.Contains("remove-package"), "Detach failure blocks deletion");

foreach (var stop in new[] { "bind", "remove-node", "remove-package" }) {
 var backend = stop == "bind" ? new Fake() : Fake.Healthy(); backend.RebootAt = stop;
 var saved = Record(backend, stop == "bind" ? Operation.Install : Operation.Uninstall);
 Check(new DriverTransaction(backend, new MemoryJournal(), saved).Apply() == JournalStatus.RebootRequired, "BOOL reboot retained: " + stop);
 Check(backend.Calls.Last() == stop, "No action after reboot boundary");
}
var ambiguous = Fake.Healthy(); ambiguous.State.Nodes = new[] { ambiguous.State.Nodes[0], ambiguous.State.Nodes[0] };
Reject(() => new DriverTransaction(ambiguous, new MemoryJournal(), Record(ambiguous, Operation.Repair)).Apply(), "Duplicate nodes rejected");
Check(ambiguous.Calls.Count == 0, "Unknown ownership never mutated");
var changed = new Fake(); var stale = Record(changed, Operation.Install); changed.State.Filters[0].Entries = new[] { "External" };
Reject(() => new DriverTransaction(changed, new MemoryJournal(), stale).Apply(), "Changed snapshot rejected");
Check(changed.Calls.Count == 0, "Stale preparation did not mutate");
var failJournal = new Fake();
Reject(() => new DriverTransaction(failJournal, new MemoryJournal { FailWrite = 2 }, Record(failJournal, Operation.Install)).Apply(), "Intent write failure rejects action");
Check(failJournal.Calls.Count == 0, "No mutation before durable intent");

// Known completed creation can be rolled back in reverse order. Unlike a
// failed/ambiguous native call, ownership and exact filter states are recorded.
var undo = new Fake(); var undoRecord = Record(undo, Operation.Install);
string inf = undo.Stage(), node = undo.CreateNode(); undo.Bind();
undoRecord.Steps.Add(new Step { Kind = StepKind.Stage, Identity = inf, Completed = true });
undoRecord.Steps.Add(new Step { Kind = StepKind.CreateNode, Identity = node, Completed = true });
undoRecord.Steps.Add(new Step { Kind = StepKind.Bind, Completed = true });
undoRecord.Status = JournalStatus.Applying;
Check(new DriverTransaction(undo, new MemoryJournal(), undoRecord).Rollback() == JournalStatus.RolledBack, "Confirmed resources rolled back");
Check(undo.State.Empty && undo.Calls.Skip(undo.Calls.Count - 2).SequenceEqual(new[] { "remove-node", "remove-package" }), "Rollback reverses ownership order");

var invalid = Record(new Fake(), Operation.Install); invalid.Schema = 99;
Reject(() => ProtectedJournal.Validate(invalid), "Unknown journal schema rejected");
invalid.Schema = 1; invalid.PayloadIdentity = new string('0', 64);
Reject(() => ProtectedJournal.Validate(invalid), "Foreign payload journal rejected");
foreach (var text in new[] { "HidHide.inf", "../oem1.inf", "C:\\Windows\\INF\\oem1.inf", "oem1.inf.bak", "oem1.inf\n", "oem*.inf" })
 Reject(() => Payload.PublishedInf(text), "Unsafe package selector rejected");
Check(Payload.PublishedInf("oem34.inf") == "oem34.inf", "Exact package identity accepted");
Reject(() => new WindowsDriverBackend(AppContext.BaseDirectory).Stage(), "Read-only native backend cannot stage");
Reject(() => WorkerProcess.Run("cmd.exe", Guid.NewGuid(), _ => {}), "Arbitrary worker verb rejected");
// Restoration exercises the production compare/write/read-back logic without IOCTL mutations.
DriverSettings CopySettings(DriverSettings value) => new() { Active=value.Active, Inverse=value.Inverse, Whitelist=value.Whitelist.ToArray(), Blacklist=value.Blacklist.ToArray() };
var restoreState = new DriverSettings { Active=true, Whitelist=SettingsCodec.Encode(new[] { @"\Device\app.exe" }), Blacklist=SettingsCodec.Encode(new[] { "HID\\A" }) };
var restoreExpected = CopySettings(restoreState);
var restoreDesired = new DriverSettings { Active=true, Inverse=true, Whitelist=SettingsCodec.Encode(new[] { @"\Device\feeder.exe" }), Blacklist=SettingsCodec.Encode(new[] { "HID\\B" }) };
var writes = new List<SettingsField>();
void WriteSetting(SettingsField field, byte[] bytes) {
 writes.Add(field);
 if(field == SettingsField.Active) restoreState.Active = bytes[0] != 0;
 else if(field == SettingsField.Inverse) restoreState.Inverse = bytes[0] != 0;
 else if(field == SettingsField.Whitelist) restoreState.Whitelist = bytes.ToArray();
 else restoreState.Blacklist = bytes.ToArray();
}
SettingsRestoration.Apply(restoreExpected, restoreDesired, () => CopySettings(restoreState), WriteSetting);
Check(writes.SequenceEqual(new[] { SettingsField.Active, SettingsField.Whitelist, SettingsField.Blacklist, SettingsField.Inverse, SettingsField.Active }), "Baseline restoration disables first and enables last");
Check(restoreState.Same(restoreDesired), "Baseline restoration read-back confirms all fields");
writes.Clear();
Reject(() => SettingsRestoration.Apply(restoreExpected, restoreDesired, () => CopySettings(restoreState), WriteSetting), "Stale restoration rejected");
Check(writes.Count == 0, "Stale baseline never overwrites external state");
SettingsRestoration.Apply(restoreDesired, restoreDesired, () => CopySettings(restoreState), WriteSetting);
Check(writes.Count == 0, "Confirmed baseline restoration is idempotent");
restoreState = CopySettings(restoreExpected);
Reject(() => SettingsRestoration.Apply(restoreExpected, restoreDesired, () => CopySettings(restoreState), (field, bytes) => {
 if(field == SettingsField.Blacklist) throw new IOException("Injected IOCTL failure"); WriteSetting(field, bytes);
}), "Partial baseline restoration failure propagated");
Check(!restoreState.Active && !writes.Contains(SettingsField.Inverse), "Restoration failure stops before later fields and enable");
Reject(() => SettingsRestoration.Apply(restoreExpected, restoreDesired, () => CopySettings(restoreExpected), (_, _) => {}), "False successful writes rejected by read-back");
Check(new DriverSettings { Whitelist=new byte[2], Blacklist=new byte[4] }.Same(new DriverSettings { Whitelist=new byte[4], Blacklist=new byte[2] }), "Empty multistring representations compare semantically");
foreach(var bytes in new[] { new byte[0], new byte[1], new byte[] { 65,0,0,0 }, new byte[] { 0,0,65,0,0,0,0,0 } })
 Reject(() => SettingsCodec.Decode(bytes), "Malformed native list rejected");
Reject(() => SettingsCodec.Encode(new[] { "same", "same" }), "Duplicate READY list rejected");
Check(SettingsCodec.Decode(SettingsCodec.Encode(new[] { "B", "A" })).SequenceEqual(new[] { "A", "B" }), "READY list encoding matches set semantics");

var resumed = new Fake { RebootAt="bind" }; var resumedRecord=Record(resumed, Operation.Install); resumedRecord.BootId="1";
var resumedEngine=new DriverTransaction(resumed, new MemoryJournal(), resumedRecord);
Check(resumedEngine.Apply() == JournalStatus.RebootRequired, "Bind reboot checkpoint");
Reject(() => resumedEngine.ResumeAfterReboot("1"), "Same-boot resume rejected");
resumed.State.Settings!.Active=false;
Check(resumedEngine.ResumeAfterReboot("2") == JournalStatus.RebootRequired, "Post-bind boot attaches filters and requests stack restart");
Check(resumed.Calls.Count(x=>x=="stage")==1 && resumed.Calls.Count(x=>x=="create")==1 && resumed.Calls.Count(x=>x=="bind")==1, "Resume never repeats completed driver steps");
Check(resumedEngine.ResumeAfterReboot("3") == JournalStatus.Applied, "Second boot verifies completed installation");
Reject(() => resumedEngine.ResumeAfterReboot("4"), "Applied transaction cannot replay resume");
var externalResume=Fake.Healthy(); var externalRecord=Record(externalResume, Operation.Repair);
externalRecord.BootId="1"; externalRecord.Status=JournalStatus.RebootRequired; externalRecord.Reboot=true;
externalResume.State.Settings=CopySettings(externalResume.State.Settings!); externalResume.State.Settings.Active=false;
Reject(() => new DriverTransaction(externalResume,new MemoryJournal(),externalRecord).ResumeAfterReboot("2"), "External baseline edit during reboot blocks resume");
var removeResume=Fake.Healthy(); removeResume.RebootAt="remove-node"; var removeRecord=Record(removeResume,Operation.Uninstall); removeRecord.BootId="1";
var removeEngine=new DriverTransaction(removeResume,new MemoryJournal(),removeRecord);
Check(removeEngine.Apply()==JournalStatus.RebootRequired, "Node removal requests reboot");
Check(removeEngine.ResumeAfterReboot("2")==JournalStatus.Applied, "Verified reboot continues package removal");
Check(removeResume.Calls.Count(x=>x=="remove-node")==1, "Removed node never replayed");
var incompleteResume=new Fake { Fail="bind" }; var incompleteRecord=Record(incompleteResume,Operation.Install); incompleteRecord.BootId="1";
var incompleteEngine=new DriverTransaction(incompleteResume,new MemoryJournal(),incompleteRecord);
Reject(()=>incompleteEngine.Apply(), "Native intent failure");
incompleteRecord.Status=JournalStatus.RebootRequired; incompleteRecord.Reboot=true;
Reject(()=>incompleteEngine.ResumeAfterReboot("2"), "Incomplete native intent cannot resume after reboot");
var retainedFile = Fake.Healthy(); retainedFile.KeepBinary = true; retainedFile.RebootAt = "remove-package";
var retainedRecord = Record(retainedFile, Operation.Uninstall); retainedRecord.BootId = "1";
var retainedEngine = new DriverTransaction(retainedFile, new MemoryJournal(), retainedRecord);
Check(retainedEngine.Apply() == JournalStatus.RebootRequired, "Package removal restart retains file for later cleanup");
Check(retainedEngine.ResumeAfterReboot("2") == JournalStatus.Applied && retainedFile.State.Empty, "Post-reboot cleanup removes only remaining owned file");
Check(retainedFile.Calls.Last() == "remove-binary" && retainedRecord.Steps.Last().Completed, "Final file cleanup is journaled");
// A failed MSI may leave the native forward prefix waiting for reboot. Undo
// proceeds only through explicit direction and acknowledged completion writes.
var rolling = new Fake { KeepBinary = true }; var rollingRecord = Record(rolling, Operation.Install);
var rollingEngine = new DriverTransaction(rolling, new MemoryJournal(), rollingRecord);
rollingEngine.Apply();
Check(rollingEngine.Rollback() == JournalStatus.RollbackRebootRequired && rollingRecord.RollbackDirection, "failed installation records explicit rollback direction");
Reject(() => rollingEngine.ResumeAfterReboot("1"), "rollback cannot cross same boot");
Check(rollingEngine.ResumeAfterReboot("2") == JournalStatus.RollbackRebootRequired, "inverse filters require stack restart before node deletion");
Check(!rolling.Calls.Contains("remove-node") && rollingRecord.Steps.Where(x => x.Kind == StepKind.Filter).All(x => x.UndoStarted && x.Undone), "filter undo has durable completion before destructive boundary");
rolling.RebootAt = "remove-node";
Check(rollingEngine.ResumeAfterReboot("3") == JournalStatus.RollbackRebootRequired, "inverse node removal reboot retained");
Check(!rolling.Calls.Contains("remove-package"), "package not removed before node reboot");
rolling.RebootAt = "remove-package";
Check(rollingEngine.ResumeAfterReboot("4") == JournalStatus.RollbackRebootRequired, "inverse package removal reboot retained");
Check(!rolling.Calls.Contains("remove-binary"), "binary retained through package reboot");
Check(rollingEngine.ResumeAfterReboot("5") == JournalStatus.RolledBack && rolling.State.Empty, "final postboot proof permits owned binary cleanup");
Check(rollingRecord.RollbackBinaryStarted && rollingRecord.RollbackBinaryCompleted && rollingRecord.Steps.All(x => x.UndoStarted && x.Undone), "undo and final cleanup evidence retained");
Check(rolling.Calls.Count(x => x == "remove-node") == 1 && rolling.Calls.Count(x => x == "remove-package") == 1, "inverse native operations never replayed");
Reject(() => rollingEngine.ResumeAfterReboot("6"), "completed rollback cannot replay");
foreach (string changedPart in new[] { "filters", "settings", "node", "package" })
{
    var b = new Fake(); var r = Record(b, Operation.Install); var e = new DriverTransaction(b, new MemoryJournal(), r);
    e.Apply(); e.Rollback(); int calls = b.Calls.Count;
    if (changedPart == "filters") b.State.Filters[0].Entries = new[] { "Foreign" };
    if (changedPart == "settings") b.State.Settings!.Active = true;
    if (changedPart == "node") b.State.Nodes[0].Id = @"ROOT\SYSTEM\9999";
    if (changedPart == "package") b.State.Packages[0] = "oem99.inf";
    Reject(() => e.ResumeAfterReboot("2"), "changed rollback prefix rejects " + changedPart);
    Check(b.Calls.Count == calls, "changed prefix performs no undo " + changedPart);
}
var uncertainUndo = new Fake(); var uncertainRecord = Record(uncertainUndo, Operation.Install);
var uncertainEngine = new DriverTransaction(uncertainUndo, new MemoryJournal(), uncertainRecord);
uncertainEngine.Apply(); uncertainEngine.Rollback(); uncertainUndo.Fail = "filter2";
Reject(() => uncertainEngine.ResumeAfterReboot("2"), "failed inverse native call retains ambiguity");
Check(uncertainRecord.Steps.Last().UndoStarted && !uncertainRecord.Steps.Last().Undone && uncertainRecord.Status == JournalStatus.RecoveryRequired, "inverse intent is distinct from completion");
int uncertainCalls = uncertainUndo.Calls.Count;
Reject(() => uncertainEngine.Rollback(), "uncertain undo cannot restart rollback");
Reject(() => uncertainEngine.ResumeAfterReboot("3"), "uncertain undo cannot resume after another reboot");
Check(uncertainUndo.Calls.Count == uncertainCalls, "ambiguous inverse never replayed");
var rollbackRepair = Fake.Healthy(); rollbackRepair.State.Filters[1].Entries = new[] { "VendorA", "VendorB" };
var rollbackRepairRecord = Record(rollbackRepair, Operation.Repair);
var rollbackRepairEngine = new DriverTransaction(rollbackRepair, new MemoryJournal(), rollbackRepairRecord);
rollbackRepairEngine.Apply(); rollbackRepairEngine.Rollback();
Check(rollbackRepairEngine.ResumeAfterReboot("2") == JournalStatus.RollbackRebootRequired, "retained filter repair undo requires restart");
Check(rollbackRepairEngine.ResumeAfterReboot("3") == JournalStatus.RolledBack && rollbackRepair.State.Settings!.Active, "retained driver baseline survives filter rollback");
Check(rollbackRepair.Calls.All(x => x == "filter1"), "filter repair rollback never deletes retained driver");
var historical = Record(new Fake(), Operation.Install); historical.Status = JournalStatus.RollingBack;
Reject(() => new DriverTransaction(new Fake(), new MemoryJournal(), historical).Rollback(), "old interrupted rollback has no inferred direction");
historical.Status = JournalStatus.RollbackRebootRequired; historical.Reboot = true;
Reject(() => ProtectedJournal.Validate(historical), "rollback status requires explicit direction");
// Simulate power loss after the native inverse call but before its completion
// reaches durable storage. Reload the actual last saved record, not the mutated
// in-memory object, then prove it cannot replay the action.
var crashUndo = new Fake(); var crashRecord = Record(crashUndo, Operation.Install); var crashJournal = new SnapshotJournal();
var crashEngine = new DriverTransaction(crashUndo, crashJournal, crashRecord);
crashEngine.Apply(); crashEngine.Rollback(); crashJournal.RejectUndoCompletion = true;
Reject(() => crashEngine.ResumeAfterReboot("2"), "inverse completion write failure propagates");
var crashed = crashJournal.Load();
Check(crashed.Steps.Last().UndoStarted && !crashed.Steps.Last().Undone, "durable journal retains unknown inverse intent");
int crashCalls = crashUndo.Calls.Count;
var crashRecovery = new DriverTransaction(crashUndo, new MemoryJournal(), crashed);
Reject(() => crashRecovery.Rollback(), "reloaded inverse intent cannot restart");
Reject(() => crashRecovery.ResumeAfterReboot("3"), "reloaded inverse intent cannot resume");
Check(crashUndo.Calls.Count == crashCalls, "completion-write crash does not replay native inverse");
Console.WriteLine($"{checks} driver transaction checks passed.");

sealed class MemoryJournal : ITransactionJournal {
 public int Writes; public int FailWrite;
 public void Save(TransactionRecord record) { if (++Writes == FailWrite) throw new IOException("injected journal failure"); ProtectedJournal.Validate(record); }
}
sealed class SnapshotJournal : ITransactionJournal {
 public bool RejectUndoCompletion; byte[] saved = Array.Empty<byte>();
 public void Save(TransactionRecord record) {
  if (RejectUndoCompletion && record.Steps.Any(x => x.Undone)) throw new IOException("injected completion flush failure");
  ProtectedJournal.Validate(record); using var stream = new MemoryStream();
  new System.Runtime.Serialization.DataContractSerializer(typeof(TransactionRecord)).WriteObject(stream, record); saved = stream.ToArray();
 }
 public TransactionRecord Load() { using var stream = new MemoryStream(saved); return (TransactionRecord)new System.Runtime.Serialization.DataContractSerializer(typeof(TransactionRecord)).ReadObject(stream)!; }
}
sealed class Fake : IDriverBackend {
 public bool RepairBind() { var before = State.Settings; var reboot = Bind(); Calls[Calls.Count - 1] = "repair-bind"; State.Nodes[0].Problem = 0; State.Settings = DriverTransaction.ExpectedAfterBinding(before); return reboot; }
 public void RemoveBinary() { Call("remove-binary"); State.BinaryHash=""; }
 public DriverState State = new() { Filters = Enumerable.Range(0,3).Select(_ => new FilterState { Exists = true, Entries = new[] { "VendorA", "VendorB" } }).ToArray() };
 public List<string> Calls = new(); public string Fail = "", RebootAt = ""; public bool KeepBinary;
 void Call(string name) { Calls.Add(name); if (Fail == name) throw new IOException("Injected native failure"); }
 public DriverState Inspect() => new() { Nodes = State.Nodes.Select(x => new Node { Id=x.Id, Inf=x.Inf, Service=x.Service, Problem=x.Problem }).ToArray(), Packages = State.Packages.ToArray(), Filters = State.Filters.Select(x => new FilterState { Exists=x.Exists, Entries=x.Entries.ToArray() }).ToArray(), ServiceExists=State.ServiceExists, ServicePendingDeletion=State.ServicePendingDeletion, BinaryHash=State.BinaryHash, ControlAvailable=State.ControlAvailable, Settings=State.Settings };
 public string Stage() { Call("stage"); State.Packages = new[] { "oem34.inf" }; return "oem34.inf"; }
 public string CreateNode() { Call("create"); State.Nodes = new[] { new Node { Id=@"ROOT\SYSTEM\0004" } }; return State.Nodes[0].Id; }
 public bool Bind() { Call("bind"); State.Nodes[0].Inf="oem34.inf"; State.Nodes[0].Service="HidHide"; State.ServiceExists=true; State.BinaryHash=Payload.SysHash; State.ControlAvailable=true; State.Settings = new DriverSettings { Active = false, Whitelist = new byte[2], Blacklist = new byte[2] }; return RebootAt == "bind"; }
 public void SetFilter(int i, FilterState expected, FilterState desired) { Call("filter"+i); if (!State.Filters[i].Same(expected)) throw new IOException("External filter edit"); State.Filters[i]=new FilterState { Exists=desired.Exists, Entries=desired.Entries.ToArray() }; }
 public bool RemoveNode(string id) { Call("remove-node"); if (State.Filters.Any(x => x.Entries.Any(DriverFilters.IsHidHide))) throw new Exception("Dangling filter"); State.Nodes=Array.Empty<Node>(); State.ControlAvailable=false; return RebootAt == "remove-node"; }
 public bool RemovePackage(string inf) { Call("remove-package"); State.Packages=Array.Empty<string>(); State.ServiceExists=false; State.Settings=null; if (!KeepBinary) State.BinaryHash=""; return RebootAt == "remove-package"; }
 public static Fake Healthy() { var b=new Fake(); b.Stage(); b.CreateNode(); b.Bind(); b.State.Settings!.Active=true; foreach (var filter in b.State.Filters) filter.Entries=new[] { "VendorA", "HidHide", "VendorB" }; b.Calls.Clear(); return b; }
}
