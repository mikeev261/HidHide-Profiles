using System.Runtime.Serialization;
using HidHide.DriverSetup;
using HidHide.Installer;

// End-to-end checks of the public-MSI driver orchestration (DirectMsiFlow)
// with an in-memory journal and marker, and a worker that runs the real
// DriverTransaction against the fake backend exactly as Program.Main does.
static class DirectMsiFlowChecks
{
    static int checks;
    static void Check(bool value, string name) { if (!value) throw new Exception(name); checks++; }

    public static int Run()
    {
        FreshInstallCommitsAfterRestart();
        UninstallRemovesDriverAfterRestart();
        HealthyUpgradeCommitsWithoutRestart();
        PreparedTransactionClosesOnForwardRollback();
        RolledBackRecoveryClosesAndNextSetupStartsFresh();
        CommittedMarkerClosesOnlyWhenStateIsUnchanged();
        UnchangedFailedFirstStepClosesInSameAttempt();
        UnchangedFailedStepFoundByRecoveryRunCloses();
        ChangedFailedTransactionStillRequiresExplicitRecovery();
        UpgradeRestoringMissingFilterCommitsAndRestartsAtEnd();
        RepairRestoringMissingFilterStillResumesAfterForcedRestart();
        FinalizeRetryAfterRestoredSettingsCommits();
        DisabledDriverCanBeUninstalled();
        ReconstructingRepairWithMissingFilterFinishesInOneAttempt();
        SlowHelperAfterCompletedUninstallDoesNotFailSetup();
        return checks;
    }

    // #16: a disabled device (problem 22) has no control device, but it is still
    // exactly the owned signed driver and must be removable.
    static void DisabledDriverCanBeUninstalled()
    {
        var backend = Fake.Healthy(); backend.State.Nodes[0].Problem = 22; backend.State.ControlAvailable = false;
        var host = new FlowHost(backend);
        var run = host.Msi("uninstall");
        Check(run.Succeeded && host.Marker == null && backend.State.Empty, "disabled driver uninstall: " + run.Failure);
    }

    // #17: repairing a driver whose binary and one filter are missing needs a
    // restart after binding; the filter then added must not fail setup.
    static void ReconstructingRepairWithMissingFilterFinishesInOneAttempt()
    {
        var backend = MissingFilter(1); backend.State.BinaryHash = "";
        backend.State.Settings = new DriverSettings { Active = true, Inverse = false, Whitelist = SettingsCodec.Encode(new[] { "feeder" }), Blacklist = SettingsCodec.Encode(new[] { "device" }) };
        var baseline = backend.State.Settings;
        var host = new FlowHost(backend);
        var run = host.Msi("repair");
        Check(run.Succeeded && host.Marker == null && host.Last.Status == JournalStatus.Committed, "reconstructing repair with a missing filter: " + run.Failure);
        Check(host.RestartAtEnd && backend.State.Healthy && backend.State.Settings!.Same(baseline), "repair restores the baseline and asks for the filter restart at the end");
    }

    // #18: the helper's cleanup after release is not part of the driver
    // transaction; a slow or failed helper must not fail a completed uninstall.
    static void SlowHelperAfterCompletedUninstallDoesNotFailSetup()
    {
        var host = new FlowHost(Fake.Healthy()) { HelperSucceeds = false };
        var run = host.Msi("uninstall");
        Check(run.Succeeded && host.Marker == null && host.Backend.State.Empty, "completed uninstall with a slow helper: " + run.Failure);
        Check(host.Logs.Any(x => x.Contains("helper")), "a slow helper is logged");
    }

    // #12: FinalizeDriver restored the repaired driver's settings but failed
    // before committing; the recovery rerun must finish instead of refusing
    // the already restored settings forever.
    static void FinalizeRetryAfterRestoredSettingsCommits()
    {
        var backend = Fake.Healthy(); backend.State.BinaryHash = "";
        backend.State.Settings = new DriverSettings { Active = true, Inverse = true, Whitelist = SettingsCodec.Encode(new[] { "feeder" }), Blacklist = SettingsCodec.Encode(new[] { "device" }) };
        var baseline = backend.State.Settings;
        var host = new FlowHost(backend) { FailCommitOnce = true };
        var first = host.Msi("repair");
        Check(!first.Succeeded && host.Marker != null && host.Last.Status == JournalStatus.Applied && backend.State.Settings!.Same(baseline), "settings were restored before the commit failed");
        var retry = host.Msi("repair");
        Check(retry.Succeeded && host.Marker == null && host.Last.Status == JournalStatus.Committed, "finalize retry after restored settings commits: " + retry.Failure);
        Check(backend.State.Healthy && backend.State.Settings!.Same(baseline), "retry keeps the restored baseline");
    }

    static Fake MissingFilter(int index)
    {
        var backend = Fake.Healthy(); backend.State.Filters[index].Entries = new[] { "VendorA", "VendorB" }; return backend;
    }

    // #2: upgrades never force a mid-setup restart, so a verified filter-only
    // change on the retained driver commits and asks for a restart at the end.
    static void UpgradeRestoringMissingFilterCommitsAndRestartsAtEnd()
    {
        var host = new FlowHost(MissingFilter(1));
        var run = host.Msi("upgrade");
        Check(run.Succeeded, "upgrade restoring a missing filter succeeds: " + run.Failure);
        Check(host.RestartAtEnd && host.Boot == "1" && host.Marker == null && host.Last.Status == JournalStatus.Committed, "upgrade commits and requests a restart at the end");
        Check(host.Backend.State.Healthy && host.Backend.State.Settings!.Active && host.Backend.Calls.SequenceEqual(new[] { "filter1" }), "upgrade only restores the missing filter and keeps the baseline");

        var changed = new FlowHost(MissingFilter(1)) { AfterApply = backend => backend.State.Filters[0].Entries = new[] { "VendorA", "HidHide", "VendorB", "External" } };
        var refused = changed.Msi("upgrade");
        Check(!refused.Succeeded && !changed.RestartAtEnd && changed.Last.Status != JournalStatus.Committed, "filters changed after apply are not committed before restart");
    }

    static void RepairRestoringMissingFilterStillResumesAfterForcedRestart()
    {
        var host = new FlowHost(MissingFilter(2));
        var run = host.Msi("repair");
        Check(run.Succeeded && !host.RestartAtEnd && host.Boot == "2" && host.Workers.SequenceEqual(new[] { "--apply", "--resume" }), "repair keeps the forced-restart continuation");
    }

    const string RunAgain = "run setup again";

    // #1: a restart-requiring rollback is resumed by a recovery rerun and
    // reaches RolledBack; that must be terminal, not a permanent block.
    static void RolledBackRecoveryClosesAndNextSetupStartsFresh()
    {
        var host = new FlowHost(new Fake()) { KillNextWorkerAfterRun = true };
        var first = host.Msi("install");
        Check(!first.Succeeded && host.Marker != null && host.Last.Status == JournalStatus.RollbackRebootRequired, "killed worker rollback waits for restart");
        host.Reboot();
        var second = host.Msi("install");
        Check(!second.Succeeded && host.Last.Status == JournalStatus.RollbackRebootRequired, "filter undo needs a second restart");
        host.Reboot();
        var third = host.Msi("install");
        Check(!third.Succeeded && third.Failure.Contains(RunAgain), "rolled-back recovery fails cleanly: " + third.Failure);
        Check(host.Marker == null && host.Last.Status == JournalStatus.Committed && host.Backend.State.CanInstall, "rolled-back recovery closes the transaction");
        var fourth = host.Msi("install");
        Check(fourth.Succeeded && host.Marker == null && host.Backend.State.Healthy, "next setup starts a fresh transaction");
    }

    // #1: Committed with a leftover marker (crash before the marker was cleared).
    static void CommittedMarkerClosesOnlyWhenStateIsUnchanged()
    {
        var host = new FlowHost(new Fake()) { FailBeforeWorker = true };
        host.Msi("install");
        var id = host.Last.Id; host.Marker = id; host.FailBeforeWorker = false; // crash before lease.Complete()
        var closed = host.Msi("install");
        Check(!closed.Succeeded && closed.Failure.Contains(RunAgain) && host.Marker == null, "unchanged committed transaction is closed: " + closed.Failure);
        Check(host.Msi("install").Succeeded, "setup after closing a committed marker succeeds");

        var installed = new FlowHost(new Fake());
        installed.Msi("install");
        installed.Marker = installed.Last.Id; int calls = installed.Backend.Calls.Count;
        var refused = installed.Msi("install");
        Check(!refused.Succeeded && !refused.Failure.Contains(RunAgain) && installed.Marker != null, "committed transaction with changed state is not closed");
        Check(installed.Backend.Calls.Count == calls && installed.Backend.State.Healthy, "refusal makes no native changes");
    }

    // #3: a failed step that changed nothing is closed by the MSI rollback of
    // the same attempt, for install and for uninstall.
    static void UnchangedFailedFirstStepClosesInSameAttempt()
    {
        var install = new FlowHost(new Fake()); install.Backend.Fail = "stage";
        var failed = install.Msi("install");
        Check(!failed.Succeeded && install.Marker == null && install.Last.Status == JournalStatus.Committed, "unchanged failed install is closed by rollback");
        install.Backend.Fail = "";
        Check(install.Msi("install").Succeeded && install.Backend.State.Healthy, "install can be retried after an unchanged failure");

        var uninstall = new FlowHost(Fake.Healthy()); uninstall.Backend.Fail = "filter0";
        var refused = uninstall.Msi("uninstall");
        Check(!refused.Succeeded && uninstall.Marker == null && uninstall.Backend.State.Healthy, "unchanged failed uninstall is closed by rollback");
        uninstall.Backend.Fail = "";
        Check(uninstall.Msi("uninstall").Succeeded && uninstall.Backend.State.Empty, "uninstall can be retried after an unchanged failure");
    }

    // #3: the same unchanged failure, found by a later recovery rerun (the
    // forward rollback never ran, for example because setup was terminated).
    static void UnchangedFailedStepFoundByRecoveryRunCloses()
    {
        var host = new FlowHost(new Fake()) { SkipRollback = true }; host.Backend.Fail = "stage";
        host.Msi("install");
        Check(host.Marker != null && host.Last.Status == JournalStatus.RecoveryRequired, "terminated setup leaves recovery evidence");
        host.SkipRollback = false; host.Backend.Fail = "";
        var closed = host.Msi("install");
        Check(!closed.Succeeded && closed.Failure.Contains(RunAgain) && host.Marker == null, "recovery run closes an unchanged failed transaction: " + closed.Failure);
        Check(host.Msi("install").Succeeded, "setup after an unchanged recovery close succeeds");
    }

    // Safety: an unknown outcome that did change resources is never guessed.
    static void ChangedFailedTransactionStillRequiresExplicitRecovery()
    {
        var host = new FlowHost(new Fake()); host.Backend.Fail = "bind";
        var failed = host.Msi("install");
        Check(!failed.Succeeded && host.Marker != null && host.Last.Status == JournalStatus.RecoveryRequired, "changed failed install keeps recovery evidence");
        int calls = host.Backend.Calls.Count;
        var rerun = host.Msi("install");
        Check(!rerun.Succeeded && !rerun.Failure.Contains(RunAgain) && host.Marker != null, "changed failed install is not closed by a rerun");
        Check(host.Backend.Calls.Count == calls, "no native changes while explicit recovery is required");
    }

    static void FreshInstallCommitsAfterRestart()
    {
        var host = new FlowHost(new Fake());
        var run = host.Msi("install");
        Check(run.Succeeded && host.Marker == null && host.Boot == "2", "fresh install commits after its forced restart");
        Check(host.Backend.State.Healthy && host.Last.Status == JournalStatus.Committed, "fresh install leaves a healthy committed driver");
    }

    static void UninstallRemovesDriverAfterRestart()
    {
        var host = new FlowHost(Fake.Healthy());
        var run = host.Msi("uninstall");
        Check(run.Succeeded && host.Marker == null && host.Backend.State.Empty, "uninstall removes all driver resources");
    }

    static void HealthyUpgradeCommitsWithoutRestart()
    {
        var host = new FlowHost(Fake.Healthy());
        var run = host.Msi("upgrade");
        Check(run.Succeeded && host.Marker == null && host.Boot == "1", "healthy upgrade needs no restart");
    }

    static void PreparedTransactionClosesOnForwardRollback()
    {
        var host = new FlowHost(new Fake()) { FailBeforeWorker = true };
        var run = host.Msi("install");
        Check(!run.Succeeded && host.Marker == null && host.Last.Status == JournalStatus.Committed, "untouched prepared transaction is closed by MSI rollback");
    }
}

// One MSI attempt as sequenced by UnifiedPreview: PrepareUser, ApplyDriver
// (+ its rollback action), ForceReboot, FinalizeDriver.
sealed class MsiRun
{
    public MsiRun(bool succeeded, string failure) { Succeeded = succeeded; Failure = failure; }
    public bool Succeeded { get; }
    public string Failure { get; }
}

sealed class FlowHost : IDirectMsiHost
{
    public FlowHost(Fake backend) { Backend = backend; }
    public Fake Backend;
    public string Boot = "1";
    public Guid? Marker;
    public string MarkerOperation = "";
    public bool MarkerRestart, RestartAtEnd, FailBeforeWorker, SkipRollback, FailCommitOnce;
    public Action<Fake>? AfterApply;
    // Applied to the next worker run: the process is killed after the engine
    // returns, so MSI sees a timeout while the journal holds the real outcome.
    public bool KillNextWorkerAfterRun;
    public readonly List<string> Workers = new(), Logs = new();
    readonly Dictionary<Guid, byte[]> journals = new();
    Guid last;
    public TransactionRecord Last => Load(last);

    public void Reboot() => Boot = (long.Parse(Boot) + 1).ToString();

    public MsiRun Msi(string operation)
    {
        var flow = new DirectMsiFlow(this);
        bool recovery = Marker != null;
        Guid id = recovery ? Marker!.Value : Guid.NewGuid();
        bool rebooted = false;
        try
        {
            if (recovery) flow.ResumePending(id, Sid, operation);
            else Prepare(id, operation);
            if (FailBeforeWorker) throw new InvalidOperationException("Injected failure before the driver worker ran.");
            flow.Apply(id, operation, recovery);
            AfterApply?.Invoke(Backend);
            // ForceReboot: NOT HIDHIDE_RECOVERY AND NOT WIX_UPGRADE_DETECTED.
            if (!recovery && operation != "upgrade") { Reboot(); rebooted = true; }
            RestartAtEnd = flow.Finalize(id);
            return new MsiRun(true, "");
        }
        catch (Exception error)
        {
            Logs.Add(error.Message);
            // Rollback scripts do not survive ForceReboot; recovery runs skip it.
            if (!rebooted && !recovery && !SkipRollback)
                try { flow.Rollback(id); } catch (Exception rollback) { Logs.Add("rollback: " + rollback.Message); }
            return new MsiRun(false, error.Message);
        }
    }

    public const string Sid = "S-1-5-21-1-2-3-1001";

    // Mirrors the fresh-attempt branch of DirectMsiActions.ApplyDriver.
    void Prepare(Guid id, string operation)
    {
        var before = Backend.Inspect(); DirectMsiFlow.ValidateStartingState(operation, before);
        var record = new TransactionRecord
        {
            Id = id, InitiatingSid = Sid, Before = before, BootId = Boot,
            Operation = operation == "uninstall" ? Operation.Uninstall : before.CanInstall ? Operation.Install : operation == "upgrade" ? Operation.Upgrade : Operation.Repair
        };
        Save(record); Marker = id; MarkerOperation = operation; last = id;
    }

    public TransactionRecord Load(Guid id)
    {
        if (!journals.TryGetValue(id, out var bytes)) throw new FileNotFoundException("Missing maintenance journal.");
        using var stream = new MemoryStream(bytes);
        var record = (TransactionRecord)new DataContractSerializer(typeof(TransactionRecord)).ReadObject(stream)!;
        ProtectedJournal.Validate(record); return record;
    }
    public void Save(TransactionRecord record)
    {
        ProtectedJournal.Validate(record);
        if (FailCommitOnce && record.Status == JournalStatus.Committed) { FailCommitOnce = false; throw new IOException("Injected journal write failure."); }
        using var stream = new MemoryStream();
        new DataContractSerializer(typeof(TransactionRecord)).WriteObject(stream, record);
        journals[record.Id] = stream.ToArray();
    }

    // Mirrors Installer.Driver Program.Main for a setup-authorized verb.
    public int RunWorker(string verb, Guid id)
    {
        Workers.Add(verb);
        int code;
        try
        {
            if (Marker != id) throw new InvalidOperationException("Missing durable maintenance marker.");
            var record = Load(id);
            var transaction = new DriverTransaction(Backend, new Journal(this), record);
            if (verb == "--apply" && string.IsNullOrEmpty(record.BootId)) { record.BootId = Boot; Save(record); }
            var outcome = verb == "--apply" ? transaction.Apply() : verb == "--resume" ? transaction.ResumeAfterReboot(Boot) : transaction.Rollback();
            code = outcome == JournalStatus.RebootRequired || outcome == JournalStatus.RollbackRebootRequired ? 3010 : 0;
        }
        catch (Exception error) { Logs.Add(verb + ": " + error.Message); code = 1; }
        if (KillNextWorkerAfterRun) { KillNextWorkerAfterRun = false; throw new TimeoutException("Driver worker timed out; outcome is unknown and recovery is required."); }
        return code;
    }

    public IDirectMsiLease OpenLease(Guid id)
    {
        if (Marker != id) throw new InvalidOperationException("Missing durable maintenance marker.");
        _ = Load(id); return new Lease(this);
    }
    public bool MarkerMatches(Guid id) => Marker == id;
    public void SignalRelease(Guid id) { }
    public void Log(string message) => Logs.Add(message);
    public string CurrentBoot() => Boot;
    public bool HelperSucceeds = true;
    public bool WaitForHelper(int milliseconds) => HelperSucceeds;

    sealed class Journal : ITransactionJournal
    {
        readonly FlowHost host;
        public Journal(FlowHost host) { this.host = host; }
        public void Save(TransactionRecord record) => host.Save(record);
    }

    sealed class Lease : IDirectMsiLease
    {
        readonly FlowHost host;
        public Lease(FlowHost host) { this.host = host; }
        public void MarkPending(string operation, bool restartRequired) { host.MarkerOperation = operation; host.MarkerRestart = restartRequired; }
        public DriverState Inspect() => host.Backend.Inspect();
        // The production restoration logic over the fake driver's settings.
        public void RestoreSettings(DriverSettings expected, DriverSettings desired)
        {
            var settings = host.Backend.State.Settings!;
            SettingsRestoration.Apply(expected, desired,
                () => new DriverSettings { Active = settings.Active, Inverse = settings.Inverse, Whitelist = settings.Whitelist.ToArray(), Blacklist = settings.Blacklist.ToArray() },
                (field, bytes) =>
                {
                    if (field == SettingsField.Active) settings.Active = bytes[0] != 0;
                    else if (field == SettingsField.Inverse) settings.Inverse = bytes[0] != 0;
                    else if (field == SettingsField.Whitelist) settings.Whitelist = bytes.ToArray();
                    else settings.Blacklist = bytes.ToArray();
                });
        }
        public void Complete() { host.Marker = null; host.MarkerRestart = false; }
        public void Dispose() { }
    }
}
