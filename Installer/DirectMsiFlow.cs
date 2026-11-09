using HidHide.DriverSetup;

namespace HidHide.Installer;

// Machine resources used by the public-MSI driver orchestration. Production
// adapts Windows Installer, the protected journal, the maintenance marker and
// the elevated worker; contract tests substitute deterministic fakes.
public interface IDirectMsiHost
{
    // Reacquires the durable recovery lease of an existing transaction, then
    // loads and validates its journal inside that ownership interval. The
    // elevated worker needs the same ownership, so dispose before running it.
    // A committed transaction is refused unless allowCommitted is set.
    IDirectMsiLease Own(Guid id, bool allowCommitted = false);
    void Save(TransactionRecord record);
    int RunWorker(string verb, Guid id);
    bool MarkerMatches(Guid id);
    string CurrentBoot();
    // True when no user helper is involved or it exited successfully in time.
    bool WaitForHelper(int milliseconds);
    void SignalRelease(Guid id);
    void Log(string message);
}

public interface IDirectMsiLease : IDisposable
{
    // The journal as loaded when ownership was acquired.
    TransactionRecord Record { get; }
    // The journal as it is now, still inside this ownership interval.
    TransactionRecord Reload();
    void MarkPending(string operation, bool restartRequired);
    DriverState Inspect();
    // False when legacy restart evidence was anchored and another restart is needed.
    bool VerifyCommitRestart(TransactionRecord record, string currentBootId);
    void RestoreSettings(DriverSettings expected, DriverSettings desired);
    void Complete();
}

// Session-free orchestration of the protected driver transaction for the
// ApplyDriver, FinalizeDriver and rollback custom actions.
public sealed class DirectMsiFlow
{
    readonly IDirectMsiHost host;
    public DirectMsiFlow(IDirectMsiHost host) { this.host = host; }

    sealed class HostJournal : ITransactionJournal
    {
        readonly IDirectMsiHost host;
        public HostJournal(IDirectMsiHost host) { this.host = host; }
        public void Save(TransactionRecord record) => host.Save(record);
    }

    public static string OperationName(Operation operation) => operation switch
    { Operation.Install => "install", Operation.Repair => "repair", Operation.Uninstall => "uninstall", Operation.Upgrade => "upgrade", _ => throw new InvalidDataException("Unknown driver operation.") };

    // Recovery rerun: continue only this user's pending transaction.
    public void ResumePending(Guid id, string sid, string operation)
    {
        using (var owned = host.Own(id, allowCommitted: true))
        {
            var existing = owned.Record;
            if (!string.Equals(existing.InitiatingSid, sid, StringComparison.Ordinal)) throw new UnauthorizedAccessException("Maintenance belongs to another Windows user.");
            if (OperationName(existing.Operation) != operation) throw new InvalidDataException("Pending maintenance operation does not match this MSI request.");
            if (existing.Status != JournalStatus.RebootRequired && existing.Status != JournalStatus.RollbackRebootRequired)
            {
                if (existing.Status != JournalStatus.Prepared && existing.Status != JournalStatus.Applied) CloseTerminal(owned);
                return;
            }
        } // Child worker needs the same ownership mutex.
        int resumed = host.RunWorker("--resume", id);
        if (resumed == 3010) throw new InvalidOperationException("Another restart is required before setup can continue.");
        if (resumed != 0) throw new InvalidOperationException("Protected driver recovery failed with exit code " + resumed + ".");
        using (var owned = host.Own(id, allowCommitted: true))
            if (owned.Record.Status != JournalStatus.Prepared && owned.Record.Status != JournalStatus.Applied) CloseTerminal(owned);
    }

    // A previous attempt that ended rolled back, or whose resources are
    // observed exactly as before, is terminal. Close it without any native
    // change and fail this attempt so the next setup starts a fresh transaction.
    void CloseTerminal(IDirectMsiLease owned)
    {
        var record = owned.Record;
        bool terminal = record.Status == JournalStatus.RolledBack ||
            record.Status is JournalStatus.Committed or JournalStatus.RecoveryRequired or JournalStatus.Applying or JournalStatus.RollingBack &&
            DriverTransaction.Unchanged(record, owned.Inspect());
        if (!terminal) throw new InvalidOperationException("Pending driver maintenance requires explicit recovery.");
        // Completion requires every recorded step to be acknowledged; a step
        // that never was had no effect on a state proven unchanged.
        if (record.Status != JournalStatus.RolledBack) DriverTransaction.DiscardUnappliedSteps(record);
        record.Status = JournalStatus.Committed; record.Reboot = false; host.Save(record);
        owned.Complete();
        host.Log("HidHide closed a previous driver transaction that made no remaining driver changes.");
        throw new InvalidOperationException("A previous interrupted setup was rolled back and closed without driver changes; run setup again.");
    }

    // Runs the prepared transaction and records whether Windows must restart.
    public void Apply(Guid id, string operation, bool recovery)
    {
        bool apply;
        using (var owned = host.Own(id))
        {
            apply = owned.Record.Status == JournalStatus.Prepared;
            // Durable before dispatch; the worker anchors the work boot.
            if (apply) DriverTransaction.PrepareMsiApply(new HostJournal(host), owned.Record);
        }
        if (apply)
        {
            int applied = host.RunWorker("--apply", id);
            if (applied != 0 && applied != 3010) throw new InvalidOperationException("Driver maintenance failed with exit code " + applied + ".");
        }
        using (var owned = host.Own(id))
        {
            var current = owned.Record;
            if (current.Status != JournalStatus.Applied && current.Status != JournalStatus.RebootRequired)
                throw new InvalidOperationException("Driver transaction did not reach a verified forward checkpoint.");
            bool restartRequired = DirectMsiPolicy.RequiresRestart(operation, recovery, current.Reboot);
            owned.MarkPending(operation, restartRequired || current.CommitRebootRequired);
        }
        host.SignalRelease(id); // Confirmed checkpoint; helper can remove owned startup on uninstall.
        // After this checkpoint the helper only removes owned startup entries;
        // the driver change is complete and cannot be undone by MSI rollback.
        if (!host.WaitForHelper(30000))
            host.Log("HidHide configuration helper did not finish its post-maintenance cleanup; the driver phase is complete. A stale startup entry may remain.");
    }

    public static void ValidateStartingState(string operation, DriverState state)
    {
        if (state.ServicePendingDeletion) throw new DriverRestartRequiredException();
        if (operation == "install" && !state.CanInstall) throw new InvalidOperationException("A clean installation requires no existing HidHide driver resources.");
        if (operation == "uninstall" && !state.Removable) throw new InvalidOperationException("Uninstall requires the installed signed driver.");
        if (operation is "repair" or "upgrade" && !(state.CoreHealthy || operation == "repair" && state.Repairable))
            throw new InvalidOperationException("The existing driver state is not safe for in-place maintenance.");
    }

    // Returns true when Windows must restart at the end of setup.
    public bool Finalize(Guid id)
    {
        string boot = host.CurrentBoot();
        bool restartAtEnd, resume, resumed = false;
        using (var snapshot = host.Own(id))
        {
            // Without an intervening restart (upgrades never force one) a
            // continuation cannot run; only a verified filter-only change commits.
            restartAtEnd = snapshot.Record.Status == JournalStatus.RebootRequired && DriverTransaction.SameBootAsWork(snapshot.Record, boot);
            resume = snapshot.Record.Status == JournalStatus.RebootRequired && !restartAtEnd;
        } // Child worker needs the same ownership mutex.
        if (resume)
        {
            int result = host.RunWorker("--resume", id);
            // The continuation may itself attach filters, which take effect only
            // after another restart; that restart is requested at the end.
            if (result == 3010) restartAtEnd = resumed = true;
            else if (result != 0) throw new InvalidOperationException("Post-restart driver continuation failed with exit code " + result + ".");
        }
        using var owned = host.Own(id);
        var record = owned.Record;
        if (record.Status != JournalStatus.Applied && !restartAtEnd) throw new InvalidOperationException("Driver transaction did not reach its verified applied state.");
        DriverState actual;
        if (restartAtEnd)
        {
            actual = owned.Inspect();
            if (!(resumed ? DriverTransaction.CanCommitAfterResume(record, actual, boot) : DriverTransaction.CanCommitBeforeRestart(record, actual)))
                throw new InvalidOperationException("Driver maintenance requires a restart before setup can finish.");
        }
        else
        {
            // The MSI may force a restart even when native APIs returned Applied.
            // AFTERREBOOT/format differences alone never prove that it occurred.
            if (!owned.VerifyCommitRestart(record, boot))
                throw new InvalidOperationException("Legacy restart evidence was preserved and anchored. Restart Windows again before continuing setup.");
            actual = owned.Inspect();
        }
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
                owned.RestoreSettings(expected, record.Before.Settings);
            }
        }
        record = owned.Reload();
        record.Status = JournalStatus.Committed; record.Reboot = false; host.Save(record);
        owned.Complete();
        return restartAtEnd;
    }

    // Forward-attempt rollback only. The caller never rolls back a transaction
    // that began in a previous MSI attempt (including a pre-restart retry).
    public void Rollback(Guid id)
    {
        host.SignalRelease(id);
        if (!host.MarkerMatches(id)) return;
        using (var owned = host.Own(id))
        {
            var record = owned.Record;
            if (record.Status == JournalStatus.Prepared)
            {
                if (record.Steps.Count != 0 || record.Reboot)
                    throw new InvalidOperationException("Prepared rollback contains native work evidence.");
                record.Status = JournalStatus.Committed; host.Save(record); owned.Complete();
                return;
            }
        } // Child worker needs the same ownership mutex.
        int result = host.RunWorker("--rollback", id);
        if (result == 3010)
        {
            host.Log("HidHide rollback requires a restart; protected recovery evidence was retained.");
            return;
        }
        if (result != 0) throw new InvalidOperationException("Driver rollback failed with exit code " + result + ".");
        using (var owned = host.Own(id))
        {
            var record = owned.Record;
            if (record.Status != JournalStatus.RolledBack) throw new InvalidOperationException("Driver rollback did not verify.");
            record.Status = JournalStatus.Committed; record.Reboot = false; host.Save(record); owned.Complete();
        }
    }
}
