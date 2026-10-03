using System.Runtime.Serialization;
using HidHide.Installer;

namespace HidHide.DriverSetup;

public enum Operation { Install, Repair, Uninstall, Upgrade }
public enum StepKind { Stage, CreateNode, Bind, Filter, RemoveNode, RemovePackage, RemoveBinary }
public enum JournalStatus { Prepared, Applying, Applied, RebootRequired, RollingBack, RolledBack, RecoveryRequired, Committed, RollbackRebootRequired }
[DataContract]
public sealed class Step
{
    [DataMember] public StepKind Kind { get; set; }
    [DataMember] public int FilterIndex { get; set; } = -1;
    [DataMember] public FilterState? BeforeFilter { get; set; }
    [DataMember] public FilterState? AfterFilter { get; set; }
    [DataMember] public string Identity { get; set; } = "";
    [DataMember] public bool Completed { get; set; }
    [DataMember] public bool Undone { get; set; }
    [DataMember] public bool UndoStarted { get; set; }
}
[DataContract]
public sealed class TransactionRecord
{
    [DataMember] public int Schema { get; set; } = 2;
    [DataMember] public Guid Id { get; set; }
    [DataMember] public string InitiatingSid { get; set; } = "";
    [DataMember] public string PayloadIdentity { get; set; } = Payload.InfHash;
    [DataMember] public Operation Operation { get; set; }
    [DataMember] public JournalStatus Status { get; set; } = JournalStatus.Prepared;
    [DataMember] public DriverState Before { get; set; } = new();
    [DataMember] public List<Step> Steps { get; set; } = new();
    [DataMember] public bool Reboot { get; set; }
    [DataMember] public string Failure { get; set; } = "";
    [DataMember] public string BootId { get; set; } = "";
    // Legacy timestamps remain evidence. This separate anchor is populated on
    // first recovery and requires another verified boot before continuation.
    [DataMember] public string RestartAnchor { get; set; } = "";
    bool commitRebootRequired;
    bool commitRebootEvidencePresent = true;
    [DataMember] public bool CommitRebootRequired
    {
        get => commitRebootRequired;
        set { commitRebootRequired = value; commitRebootEvidencePresent = true; }
    }
    internal bool HasCommitRebootEvidence => commitRebootEvidencePresent;
    // Only called after validation, when schema-1 evidence is conservatively
    // migrated or a Prepared transaction starts work in a verified current boot.
    internal void AnchorRestartEvidence(string anchor)
    { Schema = 2; RestartAnchor = anchor; commitRebootEvidencePresent = true; }
    // DataContractSerializer bypasses constructors and field initializers.
    // An omitted bool must not be indistinguishable from an explicit false.
    [OnDeserializing]
    void BeginRead(StreamingContext context) { commitRebootEvidencePresent = false; }
    [DataMember] public bool RollbackDirection { get; set; }
    [DataMember] public bool RollbackBinaryStarted { get; set; }
    [DataMember] public bool RollbackBinaryCompleted { get; set; }
}
public interface ITransactionJournal { void Save(TransactionRecord record); }

// The production backend requires its own elevated maintenance lease. This
// engine has no implicit retry: intent is durable before each native call, and
// interrupted/ambiguous operations remain recovery-required.
public sealed partial class DriverTransaction
{
    readonly IDriverBackend backend;
    readonly ITransactionJournal journal;
    readonly TransactionRecord record;
    public DriverTransaction(IDriverBackend backend, ITransactionJournal journal, TransactionRecord record)
    { this.backend = backend; this.journal = journal; this.record = record; }
    bool Reconstructing => record.Operation == Operation.Repair && !record.Before.CoreHealthy;
    void AnchorNewWork(string currentBootId)
    {
        ProtectedJournal.Validate(record);
        if (!BootIdentity.Stable(currentBootId)) throw new InvalidOperationException("A stable boot identity is required.");
        if (string.IsNullOrEmpty(record.BootId)) record.BootId = currentBootId;
        record.AnchorRestartEvidence(currentBootId); journal.Save(record);
    }
    bool VerifyRestart(string currentBootId)
    {
        if (!BootIdentity.Stable(currentBootId)) throw new InvalidOperationException("A stable current boot identity is required.");
        string anchor = string.IsNullOrEmpty(record.RestartAnchor) ? record.BootId : record.RestartAnchor;
        if (BootIdentity.Legacy(anchor))
        {
            record.AnchorRestartEvidence(currentBootId); journal.Save(record);
            return false;
        }
        if (!BootIdentity.Changed(anchor, currentBootId))
            throw new InvalidOperationException("Restart Windows before continuing protected maintenance.");
        return true;
    }
    // Persist the MSI lifecycle obligation before dispatch. Recovery is a new
    // caller attempt, not evidence that the original restart already occurred.
    public void PrepareMsiApply() => PrepareMsiApply(journal, record);
    public static void PrepareMsiApply(ITransactionJournal journal, TransactionRecord record)
    {
        ProtectedJournal.Validate(record);
        if (record.Status != JournalStatus.Prepared || record.Steps.Count != 0)
            throw new InvalidOperationException("MSI preparation requires new driver work.");
        record.RestartAnchor ??= ""; // Legacy Prepared XML may omit the new member.
        record.Schema = 2;
        record.CommitRebootRequired |= record.Operation != Operation.Upgrade;
        journal.Save(record);
    }
    public bool VerifyCommitRestart(string currentBootId)
    {
        ProtectedJournal.Validate(record);
        if (record.Status != JournalStatus.Applied) throw new InvalidOperationException("Commit requires verified applied driver state.");
        if (!BootIdentity.Valid(record.BootId)) throw new InvalidDataException("Commit requires recorded boot evidence.");
        if (!BootIdentity.Stable(currentBootId)) throw new InvalidOperationException("A stable current boot identity is required.");
        if (BootIdentity.Legacy(record.BootId) && string.IsNullOrEmpty(record.RestartAnchor))
            record.CommitRebootRequired = true;
        return !record.CommitRebootRequired || VerifyRestart(currentBootId);
    }
    public static DriverSettings ExpectedAfterBinding(DriverSettings? before) => new()
    {
        Active = false, Inverse = before?.Inverse ?? false,
        Whitelist = before?.Whitelist.ToArray() ?? SettingsCodec.Encode(Array.Empty<string>()),
        Blacklist = before?.Blacklist.ToArray() ?? SettingsCodec.Encode(Array.Empty<string>())
    };

    static void ValidateState(DriverState state)
    {
        if (state.Filters.Length != 3 || state.Nodes.Length > 1 || state.Packages.Length > 1)
            throw new InvalidOperationException("Ambiguous driver ownership; preserve existing resources.");
        foreach (var filter in state.Filters) DriverFilters.Change(filter.Entries, false);
        foreach (var inf in state.Packages) Payload.PublishedInf(inf);
        if (state.BinaryHash != "" && state.BinaryHash != Payload.SysHash)
            throw new InvalidOperationException("Unrecognized HidHide binary; replacement refused.");
    }
    void Run(Step step, Action action)
    {
        record.Steps.Add(step); journal.Save(record);
        action();
        step.Completed = true; journal.Save(record);
        if (record.Reboot) throw new RebootBoundary();
    }
    void Filters(bool attach)
    {
        for (int index = 0; index < 3; index++)
        {
            var before = backend.Inspect().Filters[index];
            var values = DriverFilters.Change(before.Entries, attach);
            var after = new FilterState { Exists = before.Exists || attach, Entries = values };
            if (before.Same(after)) continue;
            var step = new Step { Kind = StepKind.Filter, FilterIndex = index, BeforeFilter = before, AfterFilter = after };
            Run(step, () => backend.SetFilter(index, before, after));
            // Class-filter changes take effect for newly started device stacks.
            // Accumulate reboot after all filter writes so detachment is verified
            // for all classes before any device/package removal is considered.
        }
    }
    public JournalStatus Apply(string currentBootId)
    {
        ProtectedJournal.Validate(record);
        if (record.Status != JournalStatus.Prepared || record.Steps.Count != 0)
            throw new InvalidOperationException("Transaction is not new; explicit recovery/re-detection required.");
        var state = backend.Inspect(); ValidateState(state);
        if (!ProtectedJournal.StateBytes(state).SequenceEqual(ProtectedJournal.StateBytes(record.Before)))
            throw new InvalidOperationException("Driver state changed after preparation.");
        if (record.Operation == Operation.Install && !state.CanInstall)
            throw new InvalidOperationException("Fresh installation requires empty driver ownership; migrate legacy first.");
        if (record.Operation != Operation.Install && !(Reconstructing ? state.Repairable : state.CoreHealthy))
            throw new InvalidOperationException("Existing resources are not healthy and proven; explicit recovery required.");
        AnchorNewWork(currentBootId);
        record.Status = JournalStatus.Applying; journal.Save(record);
        try
        {
            if (record.Operation == Operation.Install || Reconstructing)
            {
                if (state.Packages.Length == 0)
                {
                    var stage = new Step { Kind = StepKind.Stage };
                    Run(stage, () => { stage.Identity = backend.Stage(); Payload.PublishedInf(stage.Identity); });
                }
                if (state.Nodes.Length == 0)
                {
                    var node = new Step { Kind = StepKind.CreateNode };
                    Run(node, () => node.Identity = backend.CreateNode());
                }
                Run(new Step { Kind = StepKind.Bind }, () =>
                {
                    if (Reconstructing)
                    {
                        var current = backend.Inspect();
                        if ((current.Settings == null) != (record.Before.Settings == null) || current.Settings != null && !current.Settings.Same(record.Before.Settings!))
                            throw new InvalidOperationException("Stored baseline changed before driver repair binding.");
                    }
                    record.Reboot = Reconstructing ? backend.RepairBind() : backend.Bind();
                    // Repair may replace a loaded binary or revive a device.
                    // Verify it on a new boot before restoring Active.
                    if (Reconstructing) record.Reboot = true;
                });
                Filters(true);
                var final = backend.Inspect();
                if (!final.Healthy) throw new InvalidOperationException("Driver installation did not verify; recovery required.");
                // Filter attachment needs device-stack restart, even when the
                // control interface is already available. Do not claim readiness.
                record.Reboot = record.Steps.Any(x => x.Kind == StepKind.Filter);
            }
            else if (record.Operation == Operation.Uninstall)
            {
                Filters(false);
                if (backend.Inspect().Filters.Any(x => x.Entries.Any(DriverFilters.IsHidHide)))
                    throw new InvalidOperationException("Filters remain attached; driver deletion refused.");
                foreach (var node in state.Nodes)
                    Run(new Step { Kind = StepKind.RemoveNode, Identity = node.Id }, () => record.Reboot = backend.RemoveNode(node.Id));
                foreach (var inf in state.Packages)
                    Run(new Step { Kind = StepKind.RemovePackage, Identity = inf }, () => record.Reboot = backend.RemovePackage(inf));
                if (backend.Inspect().BinaryHash != "") Run(new Step { Kind = StepKind.RemoveBinary }, backend.RemoveBinary);
                if (!backend.Inspect().Empty) throw new InvalidOperationException("Driver resources remain; uninstall is incomplete.");
                record.Reboot = record.Steps.Any(x => x.Kind == StepKind.Filter);
            }
            else
            {
                // Repair missing filter registration without rebinding a healthy
                // device, which would reset the INF's Active setting.
                Filters(true);
                if (!backend.Inspect().Healthy) throw new InvalidOperationException("Filter repair did not verify.");
                record.Reboot = record.Steps.Any(x => x.Kind == StepKind.Filter);
            }
            record.Status = record.Reboot ? JournalStatus.RebootRequired : JournalStatus.Applied;
            journal.Save(record); return record.Status;
        }
        catch (RebootBoundary)
        { record.Status = JournalStatus.RebootRequired; journal.Save(record); return record.Status; }
        catch (Exception error)
        {
            record.Status = JournalStatus.RecoveryRequired;
            record.Failure = error.GetType().Name; journal.Save(record);
            throw;
        }
    }
    public JournalStatus ResumeAfterReboot(string currentBootId)
    {
        ProtectedJournal.Validate(record);
        if (record.Status == JournalStatus.RollbackRebootRequired) return ResumeRollbackAfterReboot(currentBootId);
        if (record.Status != JournalStatus.RebootRequired || !record.Reboot ||
            record.RollbackDirection ||
            !BootIdentity.Valid(record.BootId) || !BootIdentity.Stable(currentBootId) ||
            record.Steps.Any(x => !x.Completed || x.Undone))
            throw new InvalidOperationException("A verified new boot and completed driver-step prefix are required for resume.");
        if (!VerifyRestart(currentBootId)) return record.Status;
        var state = backend.Inspect(); ValidateState(state);
        foreach (int index in Enumerable.Range(0, 3))
        {
            var edits = record.Steps.Where(x => x.Kind == StepKind.Filter && x.FilterIndex == index).ToArray();
            if (edits.Length > 1 || !state.Filters[index].Same(edits.LastOrDefault()?.AfterFilter ?? record.Before.Filters[index]))
                throw new InvalidOperationException("Class filters changed during restart; automatic resume refused.");
        }
        if (record.Operation == Operation.Install || Reconstructing)
        {
            var stage = record.Steps.Where(x => x.Kind == StepKind.Stage).ToArray();
            var create = record.Steps.Where(x => x.Kind == StepKind.CreateNode).ToArray();
            string expectedNode = record.Before.Nodes.Length == 1 ? record.Before.Nodes[0].Id : create.SingleOrDefault()?.Identity ?? "";
            string expectedInf = record.Before.Packages.Length == 1 ? record.Before.Packages[0] : stage.SingleOrDefault()?.Identity ?? "";
            if (stage.Length != (record.Before.Packages.Length == 0 ? 1 : 0) || create.Length != (record.Before.Nodes.Length == 0 ? 1 : 0) || record.Steps.Count(x => x.Kind == StepKind.Bind) != 1 ||
                !state.CoreHealthy || !string.Equals(state.Nodes[0].Id, expectedNode, StringComparison.OrdinalIgnoreCase) ||
                !string.Equals(state.Packages[0], expectedInf, StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("Installed driver did not become healthy after reboot.");
            var defaults = ExpectedAfterBinding(Reconstructing ? record.Before.Settings : null);
            if (state.Settings == null || !state.Settings.Same(defaults))
                throw new InvalidOperationException("Fresh driver settings changed before baseline restoration.");
        }
        else if (record.Operation == Operation.Uninstall)
        {
            if (state.Filters.Any(x => x.Entries.Any(DriverFilters.IsHidHide)) || state.Nodes.Length != 0 ||
                record.Steps.Count(x => x.Kind == StepKind.RemoveNode) != record.Before.Nodes.Length)
                throw new InvalidOperationException("Device removal did not complete after reboot.");
            var packageRemoved = record.Steps.Any(x => x.Kind == StepKind.RemovePackage);
            if (packageRemoved ? !state.CanInstall : !state.Packages.SequenceEqual(record.Before.Packages, StringComparer.OrdinalIgnoreCase))
                throw new InvalidOperationException("Package ownership changed during reboot.");
        }
        else if (!state.CoreHealthy || !state.Nodes.Select(x => x.Id).SequenceEqual(record.Before.Nodes.Select(x => x.Id), StringComparer.OrdinalIgnoreCase) ||
            !state.Packages.SequenceEqual(record.Before.Packages, StringComparer.OrdinalIgnoreCase) || state.Settings == null ||
            record.Before.Settings == null || !state.Settings.Same(record.Before.Settings))
            throw new InvalidOperationException("Retained driver or baseline changed during reboot.");

        int previousSteps = record.Steps.Count;
        record.Reboot = false; record.RestartAnchor = currentBootId; record.CommitRebootRequired = false; record.Status = JournalStatus.Applying; journal.Save(record);
        try
        {
            if (record.Operation == Operation.Uninstall)
            {
                if (!record.Steps.Any(x => x.Kind == StepKind.RemovePackage))
                    foreach (var inf in record.Before.Packages)
                        Run(new Step { Kind = StepKind.RemovePackage, Identity = inf }, () => record.Reboot = backend.RemovePackage(inf));
                if (backend.Inspect().BinaryHash != "") Run(new Step { Kind = StepKind.RemoveBinary }, backend.RemoveBinary);
                if (!backend.Inspect().Empty) throw new InvalidOperationException("Driver resources remain after reboot continuation.");
            }
            else
            {
                Filters(true);
                if (!backend.Inspect().Healthy) throw new InvalidOperationException("Driver verification failed after reboot continuation.");
                record.Reboot = record.Steps.Skip(previousSteps).Any(x => x.Kind == StepKind.Filter);
            }
            record.Status = record.Reboot ? JournalStatus.RebootRequired : JournalStatus.Applied;
            journal.Save(record); return record.Status;
        }
        catch (RebootBoundary) { record.Status = JournalStatus.RebootRequired; journal.Save(record); return record.Status; }
        catch (Exception error) { record.Status = JournalStatus.RecoveryRequired; record.Failure = error.GetType().Name; journal.Save(record); throw; }
    }

    sealed class RebootBoundary : Exception { }
}
