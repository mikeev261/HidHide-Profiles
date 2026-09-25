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
        return checks;
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
    public bool MarkerRestart, FailBeforeWorker;
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
            // ForceReboot: NOT HIDHIDE_RECOVERY AND NOT WIX_UPGRADE_DETECTED.
            if (!recovery && operation != "upgrade") { Reboot(); rebooted = true; }
            flow.Finalize(id);
            return new MsiRun(true, "");
        }
        catch (Exception error)
        {
            Logs.Add(error.Message);
            // Rollback scripts do not survive ForceReboot; recovery runs skip it.
            if (!rebooted && !recovery)
                try { flow.Rollback(id); } catch (Exception rollback) { Logs.Add("rollback: " + rollback.Message); }
            return new MsiRun(false, error.Message);
        }
    }

    public const string Sid = "S-1-5-21-1-2-3-1001";

    // Mirrors the fresh-attempt branch of DirectMsiActions.ApplyDriver.
    void Prepare(Guid id, string operation)
    {
        var before = Backend.Inspect();
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
        public void RestoreSettings(DriverSettings expected, DriverSettings desired)
        {
            if (!host.Backend.State.Settings!.Same(expected)) throw new InvalidOperationException("Settings changed before baseline restoration.");
            host.Backend.State.Settings = desired;
        }
        public void Complete() { host.Marker = null; host.MarkerRestart = false; }
        public void Dispose() { }
    }
}
