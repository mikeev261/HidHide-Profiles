using HidHide.DriverSetup;
using HidHide.Installer;
using Microsoft.Win32;
using System.Runtime.InteropServices;

int checks = 0;
void Check(bool value, string name) { if (!value) throw new Exception(name); checks++; }
void Reject(Action action, string name) { bool threw = false; try { action(); } catch { threw = true; } Check(threw, name); }
void RejectGuard<T>(Action action, string message, string name) where T : Exception
{ bool matched = false; try { action(); } catch (T ex) { matched = ex.GetType() == typeof(T) && ex.Message == message; } Check(matched, name); }
byte[] RecordBytes(TransactionRecord record) { using var bytes = new MemoryStream(); new System.Runtime.Serialization.DataContractSerializer(typeof(TransactionRecord)).WriteObject(bytes, record); return bytes.ToArray(); }
void RejectUnchanged<T>(Action action, string message, string name, Fake backend, SnapshotJournal journal, TransactionRecord record) where T : Exception
{
    var state = ProtectedJournal.StateBytes(backend.Inspect()); var evidence = RecordBytes(record); var durable = journal.Bytes;
    int calls = backend.Calls.Count, writes = journal.Writes;
    RejectGuard<T>(action, message, name);
    Check(backend.Calls.Count == calls && journal.Writes == writes && ProtectedJournal.StateBytes(backend.Inspect()).SequenceEqual(state) &&
        RecordBytes(record).SequenceEqual(evidence) && journal.Bytes.SequenceEqual(durable), name + " preserves complete backend and durable evidence");
}
TransactionRecord Record(Fake backend, Operation operation) => new() { Id = Guid.NewGuid(), InitiatingSid = "S-1-5-21-1-2-3-1001", Operation = operation, BootId = "winboot-v1:1", Before = backend.Inspect() };

// Read-only provider smoke is opt-in and never opens the driver or journals.
if (args.Contains("--boot-identity-smoke"))
{
    string boot = BootIdentity.Current();
    Check(BootIdentity.Stable(boot), "production provider returns canonical stable identity");
    for (int i = 0; i < 100; i++) Check(BootIdentity.Current() == boot, "same live boot has stable identity");
    Console.WriteLine("Read-only current boot identity: " + boot); return;
}

// Witness the original predicate's false-positive and false-negative. These
// are synthetic clock corrections; the host clock is never changed.
string savedTimestamp = "638900000000000000", correctedTimestamp = "638899999000000000";
Check(BootIdentity.Legacy(savedTimestamp) && BootIdentity.Legacy(correctedTimestamp) && savedTimestamp != correctedTimestamp,
    "old timestamp inequality falsely permits same-boot clock correction");
Check(!BootIdentity.Changed("winboot-v1:1", "winboot-v1:1"), "clock/timezone changes and hibernation with retained kernel identity cannot advance");
Check(BootIdentity.Changed("winboot-v1:1", "winboot-v1:2"), "new kernel identity advances even with identical synthetic wall time");
Check(BootIdentity.Changed("winboot-v1:4294967295", "winboot-v1:0"), "uint wrap remains a changed boot");
foreach (string invalidBoot in new[] { "", "winboot-v1:01", "winboot-v1:-1", "winboot-v1:4294967296", "winboot-v2:1", "1 ", "01" })
{
    Check(!BootIdentity.Valid(invalidBoot), "malformed identity rejected: " + invalidBoot);
    Check(!BootIdentity.Changed("winboot-v1:1", invalidBoot), "malformed provider cannot prove restart");
}
foreach (bool rollback in new[] { false, true })
{
    var b = new Fake(); var r = Record(b, Operation.Install); var j = new SnapshotJournal();
    var t = new DriverTransaction(b, j, r); t.Apply("winboot-v1:1"); if (rollback) t.Rollback("winboot-v1:1");
    r.Schema = 1; r.BootId = savedTimestamp; r.RestartAnchor = ""; j.Save(r);
    byte[] steps = ProtectedJournal.StateBytes(b.Inspect()); int nativeCalls = b.Calls.Count;
    Check(t.ResumeAfterReboot("winboot-v1:9") == (rollback ? JournalStatus.RollbackRebootRequired : JournalStatus.RebootRequired), "legacy checkpoint anchors without claiming reboot");
    Check(r.Schema == 2 && r.BootId == savedTimestamp && r.RestartAnchor == "winboot-v1:9" && r.Reboot && b.Calls.Count == nativeCalls,
        "legacy evidence and exclusion retained without native work");
    r = j.Load(); t = new DriverTransaction(b, j, r);
    Reject(() => t.ResumeAfterReboot("winboot-v1:9"), "reloaded migration refuses same boot");
    Reject(() => t.ResumeAfterReboot(correctedTimestamp), "clock-corrected legacy provider refuses advancement");
    Check(b.Calls.Count == nativeCalls && ProtectedJournal.StateBytes(b.Inspect()).SequenceEqual(steps), "rejected reboot does not mutate backend");
    Check(t.ResumeAfterReboot("winboot-v1:10") != JournalStatus.RecoveryRequired, "next stable boot can continue existing prefix");
}
{
    var b = Fake.Healthy(); var r = Record(b, Operation.Repair); var j = new SnapshotJournal(); var t = new DriverTransaction(b, j, r);
    t.Apply("winboot-v1:1"); r.CommitRebootRequired = true; j.Save(r);
    Reject(() => t.VerifyCommitRestart("winboot-v1:1"), "MSI-only forced restart cannot commit in same boot");
    Reject(() => t.VerifyCommitRestart(correctedTimestamp), "legacy clock correction cannot authorize MSI commit");
    Check(t.VerifyCommitRestart("winboot-v1:2"), "MSI-only forced restart verifies changed stable boot");
    r.Schema = 1; r.CommitRebootRequired = false; r.BootId = savedTimestamp; r.RestartAnchor = ""; j.Save(r);
    Check(!t.VerifyCommitRestart("winboot-v1:5"), "legacy Applied commit anchors and demands another restart");
    r = j.Load(); t = new DriverTransaction(b, j, r);
    Check(r.BootId == savedTimestamp && r.CommitRebootRequired, "legacy commit recovery preserves original timestamp and durable restart requirement");
    Reject(() => t.VerifyCommitRestart("winboot-v1:5"), "legacy commit retry refuses same anchor");
    Check(t.VerifyCommitRestart("winboot-v1:6") && b.Calls.Count == 0, "legacy commit gate authorizes only next stable boot without native writes");
}
{
    var b = new Fake { RebootAt = "bind" }; var r = Record(b, Operation.Install); var j = new MemoryJournal(); var t = new DriverTransaction(b, j, r);
    t.Apply("winboot-v1:7");
    Reject(() => t.ResumeAfterReboot("winboot-v1:7"), "recovered Prepared apply cannot reuse an older preparation boot as restart proof");
    Check(t.ResumeAfterReboot("winboot-v1:8") == JournalStatus.RebootRequired, "recovered Prepared work continues on actual next boot");
}
{
    var b = Fake.Healthy(); b.State.Filters[0].Entries = new[] { "VendorA", "VendorB" };
    var r = Record(b, Operation.Repair); var j = new SnapshotJournal(); var t = new DriverTransaction(b, j, r);
    t.Apply("winboot-v1:1"); t.ResumeAfterReboot("winboot-v1:2"); t.Rollback("winboot-v1:7");
    Reject(() => t.ResumeAfterReboot("winboot-v1:7"), "new rollback restart cannot reuse earlier forward boot");
    Check(t.ResumeAfterReboot("winboot-v1:8") == JournalStatus.RolledBack, "rollback uses actual new-work boot anchor");
}
foreach (JournalStatus rejectedStatus in new[] { JournalStatus.Applying, JournalStatus.RecoveryRequired, JournalStatus.Committed })
{
    var b = new Fake(); var r = Record(b, Operation.Install); r.Status = rejectedStatus; r.RestartAnchor = "winboot-v1:1";
    ProtectedJournal.Validate(r); var j = new SnapshotJournal(); j.Save(r); var t = new DriverTransaction(b, j, r);
    RejectUnchanged<InvalidOperationException>(() => t.Apply("winboot-v1:99"), "Transaction is not new; explicit recovery/re-detection required.",
        "non-new Apply status guard: " + rejectedStatus, b, j, r);
}
{
    var r = Record(new Fake(), Operation.Install); r.Schema = 1;
    Reject(() => ProtectedJournal.Validate(r), "schema1 cannot carry stable identities ignored by old readers");
    r.BootId = savedTimestamp; ProtectedJournal.Validate(r);
    r.RestartAnchor = "winboot-v1:1";
    Reject(() => ProtectedJournal.Validate(r), "schema1 cannot carry a restart anchor ignored by old readers");
    r.RestartAnchor = ""; r.CommitRebootRequired = true;
    Reject(() => ProtectedJournal.Validate(r), "schema1 cannot carry commit restart requirement ignored by old readers");
}

// Persist original MSI intent, interrupt before dispatch, reload on another boot,
// then interrupt after Applied: neither interruption may erase the obligation.
foreach (var op in new[] { Operation.Repair, Operation.Upgrade })
{
    var b = Fake.Healthy(); var r = Record(b, op); var j = new SnapshotJournal();
    new DriverTransaction(b, j, r).PrepareMsiApply();
    r = j.Load();
    Check(r.Status == JournalStatus.Prepared && r.CommitRebootRequired == (op != Operation.Upgrade), "pre-dispatch serialized MSI obligation: " + op);
    var t = new DriverTransaction(b, j, r);
    t.PrepareMsiApply(); // recovered Prepared preserves the original obligation
    Check(t.Apply("winboot-v1:7") == JournalStatus.Applied, "healthy worker reaches Applied: " + op);
    r = j.Load(); t = new DriverTransaction(b, j, r);
    Check(r.RestartAnchor == "winboot-v1:7" && b.Calls.Count == 0, "actual work boot anchored without native writes: " + op);
    if (op == Operation.Repair)
        Reject(() => t.VerifyCommitRestart("winboot-v1:7"), "interrupted parent cannot bypass same-boot repair commit");
    else
        Check(t.VerifyCommitRestart("winboot-v1:7"), "application-only upgrade retains no forced restart");
    Check(t.VerifyCommitRestart("winboot-v1:8"), "serialized Applied obligation accepts next actual boot: " + op);
}
foreach (string missing in new[] { "", "winboot-v1:01", "garbage" })
{
    var b = Fake.Healthy(); var r = Record(b, Operation.Upgrade); var j = new SnapshotJournal(); var t = new DriverTransaction(b, j, r);
    t.Apply("winboot-v1:1"); r.BootId = missing;
    RejectUnchanged<InvalidDataException>(() => t.VerifyCommitRestart("winboot-v1:7"), missing == "" ? "Missing recorded boot evidence for driver work." : "Invalid boot identity.",
        "missing/malformed saved evidence cannot commit: " + missing, b, j, r);
}
{
    var b = Fake.Healthy(); var r = Record(b, Operation.Repair); r.BootId = "";
    ProtectedJournal.Validate(r); var j = new SnapshotJournal(); var t = new DriverTransaction(b, j, r);
    t.PrepareMsiApply(); t.Apply("winboot-v1:7");
    Check(j.Load().BootId == "winboot-v1:7", "legitimate empty Prepared initialization obtains actual work evidence");
}

// Exercise the production XML format, not initialized in-memory defaults.
TransactionRecord Serialized(TransactionRecord source, string member = "", string edit = "")
{
    var serializer = new System.Runtime.Serialization.DataContractSerializer(typeof(TransactionRecord));
    using var bytes = new MemoryStream(); serializer.WriteObject(bytes, source);
    var xml = new System.Xml.XmlDocument(); xml.LoadXml(System.Text.Encoding.UTF8.GetString(bytes.ToArray()));
    foreach (string name in member.Split(',').Where(x => x != ""))
    {
        var element = (System.Xml.XmlElement)xml.DocumentElement!.ChildNodes.Cast<System.Xml.XmlNode>().Single(x => x.LocalName == name);
        if (edit == "omit") element.ParentNode!.RemoveChild(element);
        else { element.InnerText = ""; if (edit == "nil") element.SetAttribute("nil", "http://www.w3.org/2001/XMLSchema-instance", "true"); }
    }
    using var reader = new System.Xml.XmlNodeReader(xml);
    return (TransactionRecord)serializer.ReadObject(reader)!;
}
foreach (string gate in new[] { "forward", "inverse", "commit", "upgrade" })
{
    var b = gate is "commit" or "upgrade" ? Fake.Healthy() : new Fake { RebootAt = "bind" };
    var r = Record(b, gate == "upgrade" ? Operation.Upgrade : gate == "commit" ? Operation.Repair : Operation.Install);
    var j = new SnapshotJournal(); var t = new DriverTransaction(b, j, r); t.PrepareMsiApply(); t.Apply("winboot-v1:7");
    if (gate == "inverse") t.Rollback("winboot-v1:7");
    var normal = Serialized(r); ProtectedJournal.Validate(normal);
    Check(normal.RestartAnchor == "winboot-v1:7" && normal.CommitRebootRequired == (gate != "upgrade"), "serialized true/false evidence preserved: " + gate);
    foreach (string member in new[] { "CommitRebootRequired", "RestartAnchor" })
    foreach (string edit in new[] { "omit", "empty", "nil" })
    {
        int calls = b.Calls.Count;
        Reject(() => {
            var broken = Serialized(r, member, edit); ProtectedJournal.Validate(broken);
            var engine = new DriverTransaction(b, j, broken);
            if (gate is "commit" or "upgrade") engine.VerifyCommitRestart("winboot-v1:7");
            else engine.ResumeAfterReboot("winboot-v1:7");
        }, "serialized incomplete evidence refuses " + gate + "/" + member + "/" + edit);
        Check(b.Calls.Count == calls && j.Load().Status == r.Status && j.Load().RestartAnchor == "winboot-v1:7",
            "incomplete evidence retains native prefix and durable checkpoint");
    }
}
{
    var r = Record(new Fake(), Operation.Install); var untouched = Serialized(r); ProtectedJournal.Validate(untouched);
    Check(untouched.Status == JournalStatus.Prepared && untouched.RestartAnchor == "" && !untouched.CommitRebootRequired, "explicit no-work schema2 preparation roundtrips");
    r.Schema = 1; r.BootId = savedTimestamp;
    var oldXml = Serialized(r, "CommitRebootRequired,RestartAnchor", "omit");
    ProtectedJournal.Validate(oldXml);
    Check(oldXml.Schema == 1 && oldXml.BootId == savedTimestamp, "real schema1 XML omissions remain readable");
    var preparedJournal = new SnapshotJournal(); var preparedBackend = new Fake(); var preparedEngine = new DriverTransaction(preparedBackend, preparedJournal, oldXml);
    preparedEngine.PrepareMsiApply(); preparedEngine.Apply("winboot-v1:7");
    Check(preparedJournal.Load().RestartAnchor == "winboot-v1:7" && preparedJournal.Load().CommitRebootRequired, "legacy omitted Prepared evidence gains current-work anchor and durable obligation");
    var b = Fake.Healthy(); var applied = Record(b, Operation.Repair); applied.Schema = 1; applied.BootId = savedTimestamp; applied.Status = JournalStatus.Applied;
    var loaded = Serialized(applied, "CommitRebootRequired,RestartAnchor", "omit"); var j = new SnapshotJournal();
    Check(!new DriverTransaction(b, j, loaded).VerifyCommitRestart("winboot-v1:7"), "omitted schema1 members migrate with another restart");
    var migrated = j.Load(); ProtectedJournal.Validate(migrated);
    Reject(() => new DriverTransaction(b, j, migrated).VerifyCommitRestart("winboot-v1:7"), "serialized legacy migration refuses same boot");
    Check(new DriverTransaction(b, j, migrated).VerifyCommitRestart("winboot-v1:8") && b.Calls.Count == 0, "serialized legacy migration permits next boot without native work");
}

// Acquisition deliberately lets a preceding owner advance the serialized
// production journal before handing ownership to the queued caller.
{
    var b = new Fake(); var r = Record(b, Operation.Install); var j = new SnapshotJournal(); j.Save(r);
    var queuedSnapshot = j.Load(); bool held = false; int releases = 0;
    using (var owned = new OwnedJournal<FixtureLease>(
        () => {
            var worker = new DriverTransaction(b, j, j.Load());
            worker.PrepareMsiApply(); worker.Apply("winboot-v1:7");
            held = true; return new FixtureLease(() => { held = false; releases++; });
        }, () => { Check(held, "journal reload occurs after maintenance acquisition"); return j.Load(); }))
    {
        Check(queuedSnapshot.Status == JournalStatus.Prepared && owned.Record.Status == JournalStatus.RebootRequired,
            "queued MSI parent observes worker advancement rather than stale Prepared");
        Reject(() => DriverTransaction.PrepareMsiApply(j, owned.Record), "advanced prefix cannot be overwritten by MSI preparation");
        Check(held, "rejected preparation retains ownership until scope exit");
    }
    var durable = j.Load();
    Check(!held && releases == 1, "ownership released before child dispatch");
    Check(durable.Status == JournalStatus.RebootRequired && durable.Steps.Count == 6 && durable.Steps.All(x => x.Completed) &&
        durable.Reboot && durable.RestartAnchor == "winboot-v1:7" && durable.CommitRebootRequired && b.Inspect().Healthy,
        "interleaving preserves completed native prefix and both restart obligations");
}
foreach (string phase in new[] { "worker", "finalization", "rollback" })
{
    var b = Fake.Healthy(); var r = Record(b, Operation.Repair); var j = new SnapshotJournal();
    new DriverTransaction(b, j, r).Apply("winboot-v1:7"); j.Save(r);
    bool held = false; int releases = 0; int reads = 0;
    Reject(() => {
        using var owned = new OwnedJournal<FixtureLease>(
            () => {
                var previous = j.Load(); previous.Status = JournalStatus.Committed; previous.Reboot = false; j.Save(previous);
                held = true; return new FixtureLease(() => { held = false; releases++; });
            }, () => { reads++; Check(held, "terminal reload under ownership: " + phase); return j.Load(); });

    }, "waiting caller refuses terminal transaction: " + phase);
    Check(!held && releases == 1 && reads == 1 && j.Load().Status == JournalStatus.Committed && b.Calls.Count == 0,
        "terminal refusal releases ownership without native/journal mutation: " + phase);
}
{
    var b = Fake.Healthy(); var r = Record(b, Operation.Repair); var j = new SnapshotJournal();
    new DriverTransaction(b, j, r).Apply("winboot-v1:1"); r.Schema = 1; r.BootId = savedTimestamp; r.RestartAnchor = ""; j.Save(r);
    using var owned = new OwnedJournal<FixtureLease>(
        () => {
            var previous = new DriverTransaction(b, j, j.Load());
            Check(!previous.VerifyCommitRestart("winboot-v1:5"), "preceding owner establishes legacy commit anchor");
            return new FixtureLease(() => { });
        }, j.Load);
    Reject(() => new DriverTransaction(b, j, owned.Record).VerifyCommitRestart("winboot-v1:5"),
        "queued finalizer preserves preceding legacy restart anchor");
    Check(j.Load().RestartAnchor == "winboot-v1:5" && j.Load().CommitRebootRequired && b.Calls.Count == 0,
        "queued finalizer does not erase or move legacy evidence");
}

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
Check(new DriverTransaction(leftover, new MemoryJournal(), Record(leftover, Operation.Install)).Apply("winboot-v1:1") == JournalStatus.RebootRequired, "Install over exact inert upstream file");
var transaction = new DriverTransaction(fresh, journal, record);
Check(transaction.Apply("winboot-v1:1") == JournalStatus.RebootRequired, "Filter attachment must request reboot");
Check(fresh.State.Healthy, "Fresh state verifies root, binding, service, filters and control");
Check(fresh.Calls.SequenceEqual(new[] { "stage", "create", "bind", "filter0", "filter1", "filter2" }), "Install ordering");
Check(record.Steps.All(x => x.Completed), "Completed operations journaled");
Check(journal.Writes > fresh.Calls.Count * 2, "Intent and completion are separate durable writes");
Reject(() => transaction.Apply("winboot-v1:1"), "No replay after reboot result");
Check(transaction.Rollback("winboot-v1:1") == JournalStatus.RollbackRebootRequired && fresh.Calls.Count == 6, "Rollback direction recorded without crossing reboot boundary");
Check(record.Before.Empty, "Initial snapshot not mutated by execution");

foreach (var operation in new[] { Operation.Repair, Operation.Upgrade }) {
 var backend = Fake.Healthy(); var saved = Record(backend, operation);
 Check(new DriverTransaction(backend, new MemoryJournal(), saved).Apply("winboot-v1:1") == JournalStatus.Applied, "Healthy driver retained");
 Check(backend.Calls.Count == 0, "Repair/upgrade do not reinstall or reset settings");
 Check(!saved.Reboot, "Healthy retained driver does not invent a reboot during repair/upgrade");
}
var repairFilters = Fake.Healthy(); repairFilters.State.Filters[1].Entries = new[] { "VendorA", "VendorB" };
var repairRecord = Record(repairFilters, Operation.Repair);
Check(new DriverTransaction(repairFilters, new MemoryJournal(), repairRecord).Apply("winboot-v1:1") == JournalStatus.RebootRequired, "Missing filter repair requests reboot");
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
    var saved = Record(backend, Operation.Repair); saved.BootId = "winboot-v1:1";
    var engine = new DriverTransaction(backend, new MemoryJournal(), saved);
    Check(engine.Apply("winboot-v1:1") == JournalStatus.RebootRequired, "reconstruct missing " + missing);
    Check(!backend.Calls.Any(x => x.StartsWith("remove")), "repair never tears down " + missing);
    Check(backend.Calls.Count(x => x == "stage") == (missing == "package" ? 1 : 0) && backend.Calls.Count(x => x == "create") == (missing == "node" ? 1 : 0), "repair creates only missing resource " + missing);
    Reject(() => engine.Rollback("winboot-v1:1"), "repair reboot cannot be destructively reversed " + missing);
    Check(engine.ResumeAfterReboot("winboot-v1:2") == JournalStatus.Applied, "repair verifies retained/new identities after reboot " + missing);
    Check(saved.Before.Settings!.Active && backend.State.Settings!.Same(DriverTransaction.ExpectedAfterBinding(saved.Before.Settings)), "repair preserves baseline and verifies expected INF reset " + missing);
    Reject(() => engine.Rollback("winboot-v1:1"), "completed repair is not rolled back by deleting repaired driver " + missing);
}
{
    var values = new Dictionary<string, (RegistryValueKind? Kind, object? Value)> {
        ["Active"] = (RegistryValueKind.DWord, 1), ["WhitelistedFullImageNames"] = (RegistryValueKind.MultiString, new[] { "feeder" }), ["BlacklistedDeviceInstancePaths"] = (RegistryValueKind.MultiString, new[] { "device" }) };
    DriverSettings ReadStored() => StoredDriverSettings.Read(name => values.TryGetValue(name, out var value) ? value : (null, null));
    var stored = ReadStored();
    Check(stored.Active && !stored.Inverse && SettingsCodec.Decode(stored.Whitelist).Single() == "feeder", "stored baseline uses pinned-driver defaults for absent inverse only");
    values["Active"] = (RegistryValueKind.String, "winboot-v1:1"); Reject(() => ReadStored(), "stored flag type rejected");
    values["Active"] = (RegistryValueKind.DWord, 2); Reject(() => ReadStored(), "stored flag value rejected");
    values["Active"] = (RegistryValueKind.DWord, 1); values["WhitelistedFullImageNames"] = (RegistryValueKind.MultiString, new[] { "same", "same" }); Reject(() => ReadStored(), "stored duplicate list rejected");
    values["WhitelistedFullImageNames"] = (RegistryValueKind.MultiString, new[] { "feeder" });
    values.Remove("BlacklistedDeviceInstancePaths"); RejectGuard<InvalidDataException>(() => ReadStored(), "Invalid stored driver list.", "missing stored baseline list rejected");
}
var uninstall = Fake.Healthy(); var removal = Record(uninstall, Operation.Uninstall);
Check(new DriverTransaction(uninstall, new MemoryJournal(), removal).Apply("winboot-v1:1") == JournalStatus.RebootRequired, "Uninstall filter changes report reboot");
Check(uninstall.Calls.SequenceEqual(new[] { "filter0", "filter1", "filter2", "remove-node", "remove-package" }), "Detach before deletion");
Check(uninstall.State.Empty, "Uninstall resources removed");
Check(removal.Before.Settings!.Active, "Original baseline preserved in journal");
Check(uninstall.State.Filters.All(x => x.Entries.SequenceEqual(new[] { "VendorA", "VendorB" })), "Unrelated filters preserved");

foreach (var failure in new[] { "stage", "create", "bind", "filter0", "filter1", "filter2" }) {
 var backend = new Fake { Fail = failure }; var saved = Record(backend, Operation.Install);
 var engine = new DriverTransaction(backend, new MemoryJournal(), saved);
 Reject(() => engine.Apply("winboot-v1:1"), "Failure propagated: " + failure);
 Check(saved.Status == JournalStatus.RecoveryRequired, "Failure retains recovery state");
 Check(!saved.Steps.Last().Completed, "Unknown outcome recorded as intent only");
 Reject(() => engine.Apply("winboot-v1:1"), "Unknown step never replayed");
 Reject(() => engine.Rollback("winboot-v1:1"), "Unknown ownership never guessed");
}
var detachFailure = Fake.Healthy(); detachFailure.Fail = "filter1";
var detachRecord = Record(detachFailure, Operation.Uninstall);
Reject(() => new DriverTransaction(detachFailure, new MemoryJournal(), detachRecord).Apply("winboot-v1:1"), "Detach failure propagates");
Check(!detachFailure.Calls.Contains("remove-node") && !detachFailure.Calls.Contains("remove-package"), "Detach failure blocks deletion");

foreach (var stop in new[] { "bind", "remove-node", "remove-package" }) {
 var backend = stop == "bind" ? new Fake() : Fake.Healthy(); backend.RebootAt = stop;
 var saved = Record(backend, stop == "bind" ? Operation.Install : Operation.Uninstall);
 Check(new DriverTransaction(backend, new MemoryJournal(), saved).Apply("winboot-v1:1") == JournalStatus.RebootRequired, "BOOL reboot retained: " + stop);
 Check(backend.Calls.Last() == stop, "No action after reboot boundary");
}
var ambiguous = Fake.Healthy(); ambiguous.State.Nodes = new[] { ambiguous.State.Nodes[0], ambiguous.State.Nodes[0] };
Reject(() => new DriverTransaction(ambiguous, new MemoryJournal(), Record(ambiguous, Operation.Repair)).Apply("winboot-v1:1"), "Duplicate nodes rejected");
Check(ambiguous.Calls.Count == 0, "Unknown ownership never mutated");
var changed = new Fake(); var stale = Record(changed, Operation.Install); changed.State.Filters[0].Entries = new[] { "External" };
Reject(() => new DriverTransaction(changed, new MemoryJournal(), stale).Apply("winboot-v1:1"), "Changed snapshot rejected");
Check(changed.Calls.Count == 0, "Stale preparation did not mutate");
var failJournal = new Fake();
var intentRecord = Record(failJournal, Operation.Install); var intentJournal = new SnapshotJournal { RejectStageIntent = true };
var intentState = ProtectedJournal.StateBytes(failJournal.Inspect());
RejectGuard<IOException>(() => new DriverTransaction(failJournal, intentJournal, intentRecord).Apply("winboot-v1:1"), "injected Stage intent flush failure", "Stage intent write failure rejects action");
var intentCheckpoint = intentJournal.Load();
Check(intentCheckpoint.Status == JournalStatus.RecoveryRequired && intentCheckpoint.Steps.Count == 1 && intentCheckpoint.Steps[0].Kind == StepKind.Stage &&
    !intentCheckpoint.Steps[0].Completed && intentCheckpoint.RestartAnchor == "winboot-v1:1" && intentJournal.StageIntentFailures == 1,
    "failed Stage intent leaves explicit incomplete recovery checkpoint");
Check(failJournal.Calls.Count == 0 && ProtectedJournal.StateBytes(failJournal.Inspect()).SequenceEqual(intentState), "No mutation before durable Stage intent");
var statusBackend = new Fake(); var statusRecord = Record(statusBackend, Operation.Install); var statusJournal = new MemoryJournal { FailWrite = 2 };
RejectGuard<IOException>(() => new DriverTransaction(statusBackend, statusJournal, statusRecord).Apply("winboot-v1:1"), "injected journal failure", "Applying status persistence interruption propagates");
Check(statusRecord.Status == JournalStatus.Applying && statusRecord.Steps.Count == 0 && statusJournal.Writes == 2 && statusBackend.Calls.Count == 0,
    "status persistence interruption occurs before native intent");

// Known completed creation can be rolled back in reverse order. Unlike a
// failed/ambiguous native call, ownership and exact filter states are recorded.
var undo = new Fake(); var undoRecord = Record(undo, Operation.Install);
string inf = undo.Stage(), node = undo.CreateNode(); undo.Bind();
undoRecord.Steps.Add(new Step { Kind = StepKind.Stage, Identity = inf, Completed = true });
undoRecord.Steps.Add(new Step { Kind = StepKind.CreateNode, Identity = node, Completed = true });
undoRecord.Steps.Add(new Step { Kind = StepKind.Bind, Completed = true });
undoRecord.Status = JournalStatus.Applying; undoRecord.RestartAnchor = "winboot-v1:1";
Check(new DriverTransaction(undo, new MemoryJournal(), undoRecord).Rollback("winboot-v1:1") == JournalStatus.RolledBack, "Confirmed resources rolled back");
Check(undo.State.Empty && undo.Calls.Skip(undo.Calls.Count - 2).SequenceEqual(new[] { "remove-node", "remove-package" }), "Rollback reverses ownership order");

var invalid = Record(new Fake(), Operation.Install); invalid.Schema = 99;
Reject(() => ProtectedJournal.Validate(invalid), "Unknown journal schema rejected");
invalid.Schema = 2; invalid.PayloadIdentity = new string('0', 64);
RejectGuard<InvalidDataException>(() => ProtectedJournal.Validate(invalid), "Unsupported maintenance journal.", "Foreign payload journal rejected");
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

var resumed = new Fake { RebootAt="bind" }; var resumedRecord=Record(resumed, Operation.Install); resumedRecord.BootId="winboot-v1:1";
var resumedEngine=new DriverTransaction(resumed, new MemoryJournal(), resumedRecord);
Check(resumedEngine.Apply("winboot-v1:1") == JournalStatus.RebootRequired, "Bind reboot checkpoint");
Reject(() => resumedEngine.ResumeAfterReboot("winboot-v1:1"), "Same-boot resume rejected");
resumed.State.Settings!.Active=false;
Check(resumedEngine.ResumeAfterReboot("winboot-v1:2") == JournalStatus.RebootRequired, "Post-bind boot attaches filters and requests stack restart");
Check(resumed.Calls.Count(x=>x=="stage")==1 && resumed.Calls.Count(x=>x=="create")==1 && resumed.Calls.Count(x=>x=="bind")==1, "Resume never repeats completed driver steps");
Check(resumedEngine.ResumeAfterReboot("winboot-v1:3") == JournalStatus.Applied, "Second boot verifies completed installation");
Reject(() => resumedEngine.ResumeAfterReboot("winboot-v1:4"), "Applied transaction cannot replay resume");
var externalResume=Fake.Healthy(); externalResume.State.Filters[1].Entries = new[] { "VendorA", "VendorB" };
var externalRecord=Record(externalResume, Operation.Repair); var externalJournal=new MemoryJournal();
var externalEngine=new DriverTransaction(externalResume,externalJournal,externalRecord);
Check(externalEngine.Apply("winboot-v1:1") == JournalStatus.RebootRequired && externalRecord.RestartAnchor == "winboot-v1:1", "Baseline-change witness has production-generated restart evidence");
externalResume.State.Settings=CopySettings(externalResume.State.Settings!); externalResume.State.Settings.Active=false;
var externalState=ProtectedJournal.StateBytes(externalResume.Inspect()); int externalCalls=externalResume.Calls.Count, externalWrites=externalJournal.Writes;
bool baselineRejected=false;
try { externalEngine.ResumeAfterReboot("winboot-v1:2"); }
catch (InvalidOperationException ex) { baselineRejected=ex.Message == "Retained driver or baseline changed during reboot."; }
Check(baselineRejected, "External baseline edit during reboot reaches baseline-change guard");
Check(externalResume.Calls.Count == externalCalls && externalJournal.Writes == externalWrites && ProtectedJournal.StateBytes(externalResume.Inspect()).SequenceEqual(externalState), "Baseline-change refusal preserves backend settings and journal without mutation");
var removeResume=Fake.Healthy(); removeResume.RebootAt="remove-node"; var removeRecord=Record(removeResume,Operation.Uninstall); removeRecord.BootId="winboot-v1:1";
var removeEngine=new DriverTransaction(removeResume,new MemoryJournal(),removeRecord);
Check(removeEngine.Apply("winboot-v1:1")==JournalStatus.RebootRequired, "Node removal requests reboot");
Check(removeEngine.ResumeAfterReboot("winboot-v1:2")==JournalStatus.Applied, "Verified reboot continues package removal");
Check(removeResume.Calls.Count(x=>x=="remove-node")==1, "Removed node never replayed");
var incompleteResume=new Fake { Fail="bind" }; var incompleteRecord=Record(incompleteResume,Operation.Install); incompleteRecord.BootId="winboot-v1:1";
var incompleteEngine=new DriverTransaction(incompleteResume,new MemoryJournal(),incompleteRecord);
Reject(()=>incompleteEngine.Apply("winboot-v1:1"), "Native intent failure");
incompleteRecord.Status=JournalStatus.RebootRequired; incompleteRecord.Reboot=true;
Reject(()=>incompleteEngine.ResumeAfterReboot("winboot-v1:2"), "Incomplete native intent cannot resume after reboot");
var retainedFile = Fake.Healthy(); retainedFile.KeepBinary = true; retainedFile.RebootAt = "remove-package";
var retainedRecord = Record(retainedFile, Operation.Uninstall); retainedRecord.BootId = "winboot-v1:1";
var retainedEngine = new DriverTransaction(retainedFile, new MemoryJournal(), retainedRecord);
Check(retainedEngine.Apply("winboot-v1:1") == JournalStatus.RebootRequired, "Package removal restart retains file for later cleanup");
Check(retainedEngine.ResumeAfterReboot("winboot-v1:2") == JournalStatus.Applied && retainedFile.State.Empty, "Post-reboot cleanup removes only remaining owned file");
Check(retainedFile.Calls.Last() == "remove-binary" && retainedRecord.Steps.Last().Completed, "Final file cleanup is journaled");
// A failed MSI may leave the native forward prefix waiting for reboot. Undo
// proceeds only through explicit direction and acknowledged completion writes.
var rolling = new Fake { KeepBinary = true }; var rollingRecord = Record(rolling, Operation.Install);
var rollingEngine = new DriverTransaction(rolling, new MemoryJournal(), rollingRecord);
rollingEngine.Apply("winboot-v1:1");
Check(rollingEngine.Rollback("winboot-v1:1") == JournalStatus.RollbackRebootRequired && rollingRecord.RollbackDirection, "failed installation records explicit rollback direction");
Reject(() => rollingEngine.ResumeAfterReboot("winboot-v1:1"), "rollback cannot cross same boot");
Check(rollingEngine.ResumeAfterReboot("winboot-v1:2") == JournalStatus.RollbackRebootRequired, "inverse filters require stack restart before node deletion");
Check(!rolling.Calls.Contains("remove-node") && rollingRecord.Steps.Where(x => x.Kind == StepKind.Filter).All(x => x.UndoStarted && x.Undone), "filter undo has durable completion before destructive boundary");
rolling.RebootAt = "remove-node";
Check(rollingEngine.ResumeAfterReboot("winboot-v1:3") == JournalStatus.RollbackRebootRequired, "inverse node removal reboot retained");
Check(!rolling.Calls.Contains("remove-package"), "package not removed before node reboot");
rolling.RebootAt = "remove-package";
Check(rollingEngine.ResumeAfterReboot("winboot-v1:4") == JournalStatus.RollbackRebootRequired, "inverse package removal reboot retained");
Check(!rolling.Calls.Contains("remove-binary"), "binary retained through package reboot");
Check(rollingEngine.ResumeAfterReboot("winboot-v1:5") == JournalStatus.RolledBack && rolling.State.Empty, "final postboot proof permits owned binary cleanup");
Check(rollingRecord.RollbackBinaryStarted && rollingRecord.RollbackBinaryCompleted && rollingRecord.Steps.All(x => x.UndoStarted && x.Undone), "undo and final cleanup evidence retained");
Check(rolling.Calls.Count(x => x == "remove-node") == 1 && rolling.Calls.Count(x => x == "remove-package") == 1, "inverse native operations never replayed");
Reject(() => rollingEngine.ResumeAfterReboot("winboot-v1:6"), "completed rollback cannot replay");
foreach (string changedPart in new[] { "filters", "settings", "node", "package" })
{
    var b = new Fake(); var r = Record(b, Operation.Install); var e = new DriverTransaction(b, new MemoryJournal(), r);
    e.Apply("winboot-v1:1"); e.Rollback("winboot-v1:1"); int calls = b.Calls.Count;
    if (changedPart == "filters") b.State.Filters[0].Entries = new[] { "Foreign" };
    if (changedPart == "settings") b.State.Settings!.Active = true;
    if (changedPart == "node") b.State.Nodes[0].Id = @"ROOT\SYSTEM\9999";
    if (changedPart == "package") b.State.Packages[0] = "oem99.inf";
    Reject(() => e.ResumeAfterReboot("winboot-v1:2"), "changed rollback prefix rejects " + changedPart);
    Check(b.Calls.Count == calls, "changed prefix performs no undo " + changedPart);
}
var uncertainUndo = new Fake(); var uncertainRecord = Record(uncertainUndo, Operation.Install);
var uncertainEngine = new DriverTransaction(uncertainUndo, new MemoryJournal(), uncertainRecord);
uncertainEngine.Apply("winboot-v1:1"); uncertainEngine.Rollback("winboot-v1:1"); uncertainUndo.Fail = "filter2";
Reject(() => uncertainEngine.ResumeAfterReboot("winboot-v1:2"), "failed inverse native call retains ambiguity");
Check(uncertainRecord.Steps.Last().UndoStarted && !uncertainRecord.Steps.Last().Undone && uncertainRecord.Status == JournalStatus.RecoveryRequired, "inverse intent is distinct from completion");
int uncertainCalls = uncertainUndo.Calls.Count;
Reject(() => uncertainEngine.Rollback("winboot-v1:1"), "uncertain undo cannot restart rollback");
Reject(() => uncertainEngine.ResumeAfterReboot("winboot-v1:3"), "uncertain undo cannot resume after another reboot");
Check(uncertainUndo.Calls.Count == uncertainCalls, "ambiguous inverse never replayed");
var rollbackRepair = Fake.Healthy(); rollbackRepair.State.Filters[1].Entries = new[] { "VendorA", "VendorB" };
var rollbackRepairRecord = Record(rollbackRepair, Operation.Repair);
var rollbackRepairEngine = new DriverTransaction(rollbackRepair, new MemoryJournal(), rollbackRepairRecord);
rollbackRepairEngine.Apply("winboot-v1:1"); rollbackRepairEngine.Rollback("winboot-v1:1");
Check(rollbackRepairEngine.ResumeAfterReboot("winboot-v1:2") == JournalStatus.RollbackRebootRequired, "retained filter repair undo requires restart");
Check(rollbackRepairEngine.ResumeAfterReboot("winboot-v1:3") == JournalStatus.RolledBack && rollbackRepair.State.Settings!.Active, "retained driver baseline survives filter rollback");
Check(rollbackRepair.Calls.All(x => x == "filter1"), "filter repair rollback never deletes retained driver");
var historicalBackend = new Fake(); var historical = Record(historicalBackend, Operation.Install); historical.Status = JournalStatus.RollingBack; historical.RestartAnchor = "winboot-v1:1";
ProtectedJournal.Validate(historical); var historicalJournal = new SnapshotJournal(); historicalJournal.Save(historical);
RejectUnchanged<InvalidOperationException>(() => new DriverTransaction(historicalBackend, historicalJournal, historical).Rollback("winboot-v1:1"),
    "Cannot begin rollback from this transaction state.", "old interrupted rollback has no inferred direction", historicalBackend, historicalJournal, historical);
historical.Status = JournalStatus.RollbackRebootRequired; historical.Reboot = true;
Reject(() => ProtectedJournal.Validate(historical), "rollback status requires explicit direction");
// Simulate power loss after the native inverse call but before its completion
// reaches durable storage. Reload the actual last saved record, not the mutated
// in-memory object, then prove it cannot replay the action.
var crashUndo = new Fake(); var crashRecord = Record(crashUndo, Operation.Install); var crashJournal = new SnapshotJournal();
var crashEngine = new DriverTransaction(crashUndo, crashJournal, crashRecord);
crashEngine.Apply("winboot-v1:1"); crashEngine.Rollback("winboot-v1:1"); crashJournal.RejectUndoCompletion = true;
Reject(() => crashEngine.ResumeAfterReboot("winboot-v1:2"), "inverse completion write failure propagates");
var crashed = crashJournal.Load();
Check(crashed.Steps.Last().UndoStarted && !crashed.Steps.Last().Undone, "durable journal retains unknown inverse intent");
int crashCalls = crashUndo.Calls.Count;
var crashRecovery = new DriverTransaction(crashUndo, new MemoryJournal(), crashed);
Reject(() => crashRecovery.Rollback("winboot-v1:1"), "reloaded inverse intent cannot restart");
Reject(() => crashRecovery.ResumeAfterReboot("winboot-v1:3"), "reloaded inverse intent cannot resume");
Check(crashUndo.Calls.Count == crashCalls, "completion-write crash does not replay native inverse");
Console.WriteLine($"{checks} driver transaction checks passed.");

sealed class MemoryJournal : ITransactionJournal {
 public int Writes; public int FailWrite;
 public void Save(TransactionRecord record) { if (++Writes == FailWrite) throw new IOException("injected journal failure"); ProtectedJournal.Validate(record); }
}
sealed class FixtureLease : IDisposable {
 readonly Action release;
 public FixtureLease(Action release) { this.release = release; }
 public void Dispose() => release();
}
sealed class SnapshotJournal : ITransactionJournal {
 public bool RejectUndoCompletion, RejectStageIntent; public int Writes, StageIntentFailures; byte[] saved = Array.Empty<byte>();
 public byte[] Bytes => saved.ToArray();
 public void Save(TransactionRecord record) {
  Writes++;
  if (RejectStageIntent && StageIntentFailures == 0 && record.Status == JournalStatus.Applying && record.Steps.Count == 1 && record.Steps[0].Kind == StepKind.Stage && !record.Steps[0].Completed)
  { StageIntentFailures++; throw new IOException("injected Stage intent flush failure"); }
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
