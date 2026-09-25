using HidHide.DriverSetup;

namespace HidHide.Installer;

// Machine resources used by the public-MSI driver orchestration. Production
// adapts Windows Installer, the protected journal, the maintenance marker and
// the elevated worker; contract tests substitute deterministic fakes.
public interface IDirectMsiHost
{
    TransactionRecord Load(Guid id);
    void Save(TransactionRecord record);
    int RunWorker(string verb, Guid id);
    // Reopens the durable marker of an existing transaction (recovery lease).
    IDirectMsiLease OpenLease(Guid id);
    bool MarkerMatches(Guid id);
    void SignalRelease(Guid id);
    void Log(string message);
}

public interface IDirectMsiLease : IDisposable
{
    void MarkPending(string operation, bool restartRequired);
    DriverState Inspect();
    void RestoreSettings(DriverSettings expected, DriverSettings desired);
    void Complete();
}

// Session-free orchestration of the protected driver transaction for the
// ApplyDriver, FinalizeDriver and rollback custom actions.
public sealed class DirectMsiFlow
{
    readonly IDirectMsiHost host;
    public DirectMsiFlow(IDirectMsiHost host) { this.host = host; }

    public static string OperationName(Operation operation) => operation switch
    { Operation.Install => "install", Operation.Repair => "repair", Operation.Uninstall => "uninstall", Operation.Upgrade => "upgrade", _ => throw new InvalidDataException("Unknown driver operation.") };

    // Recovery rerun: continue only this user's pending transaction.
    public void ResumePending(Guid id, string sid, string operation)
    {
        var existing = host.Load(id);
        if (!string.Equals(existing.InitiatingSid, sid, StringComparison.Ordinal)) throw new UnauthorizedAccessException("Maintenance belongs to another Windows user.");
        if (OperationName(existing.Operation) != operation) throw new InvalidDataException("Pending maintenance operation does not match this MSI request.");
        if (existing.Status == JournalStatus.RebootRequired || existing.Status == JournalStatus.RollbackRebootRequired)
        {
            int resumed = host.RunWorker("--resume", id);
            if (resumed == 3010) throw new InvalidOperationException("Another restart is required before setup can continue.");
            if (resumed != 0) throw new InvalidOperationException("Protected driver recovery failed with exit code " + resumed + ".");
        }
        else if (existing.Status != JournalStatus.Prepared && existing.Status != JournalStatus.Applied)
            throw new InvalidOperationException("Pending driver maintenance requires explicit recovery.");
    }

    // Runs the prepared transaction and records whether Windows must restart.
    public void Apply(Guid id, string operation, bool recovery)
    {
        var current = host.Load(id);
        if (current.Status == JournalStatus.Prepared)
        {
            int applied = host.RunWorker("--apply", id);
            if (applied != 0 && applied != 3010) throw new InvalidOperationException("Driver maintenance failed with exit code " + applied + ".");
        }
        current = host.Load(id);
        using var lease = host.OpenLease(id);
        lease.MarkPending(operation, DirectMsiPolicy.RequiresRestart(operation, recovery, current.Reboot));
    }

    public void Finalize(Guid id)
    {
        var record = host.Load(id);
        if (record.Status == JournalStatus.RebootRequired)
        {
            int result = host.RunWorker("--resume", id);
            if (result != 0) throw new InvalidOperationException("Post-restart driver continuation failed with exit code " + result + ".");
            record = host.Load(id);
        }
        if (record.Status != JournalStatus.Applied) throw new InvalidOperationException("Driver transaction did not reach its verified applied state.");
        using var lease = host.OpenLease(id);
        var actual = lease.Inspect();
        if (record.Operation == Operation.Uninstall)
        {
            if (!actual.Empty) throw new InvalidOperationException("Driver resources remain after uninstall.");
        }
        else
        {
            if (!actual.Healthy || actual.Settings == null) throw new InvalidOperationException("Installed driver did not verify as healthy.");
            if (record.Operation == Operation.Repair && !record.Before.CoreHealthy && record.Before.Settings != null)
            {
                var expected = DriverTransaction.ExpectedAfterBinding(record.Before.Settings);
                lease.RestoreSettings(expected, record.Before.Settings);
            }
        }
        record = host.Load(id);
        record.Status = JournalStatus.Committed; record.Reboot = false; host.Save(record);
        lease.Complete();
    }

    // Forward-attempt rollback only. The caller never rolls back a transaction
    // that began in a previous MSI attempt (including a pre-restart retry).
    public void Rollback(Guid id)
    {
        host.SignalRelease(id);
        if (!host.MarkerMatches(id)) return;
        var record = host.Load(id);
        if (record.Status == JournalStatus.Prepared)
        {
            using var lease = host.OpenLease(id);
            record.Status = JournalStatus.Committed; host.Save(record); lease.Complete();
            return;
        }
        int result = host.RunWorker("--rollback", id);
        if (result == 3010)
        {
            host.Log("HidHide rollback requires a restart; protected recovery evidence was retained.");
            return;
        }
        if (result != 0) throw new InvalidOperationException("Driver rollback failed with exit code " + result + ".");
        record = host.Load(id);
        if (record.Status != JournalStatus.RolledBack) throw new InvalidOperationException("Driver rollback did not verify.");
        using (var lease = host.OpenLease(id))
        { record.Status = JournalStatus.Committed; record.Reboot = false; host.Save(record); lease.Complete(); }
    }
}
