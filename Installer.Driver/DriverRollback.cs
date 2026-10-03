namespace HidHide.DriverSetup;

public sealed partial class DriverTransaction
{
    void ValidateRollbackOwnership()
    {
        if (record.Steps.Any(x => !x.Completed || x.UndoStarted && !x.Undone) ||
            record.RollbackBinaryStarted && !record.RollbackBinaryCompleted)
            throw new InvalidOperationException("Unknown native outcome; rollback intent is never replayed.");
        if (Reconstructing && record.Steps.Any(x => x.Kind == StepKind.Bind))
            throw new InvalidOperationException("Reconstructed existing driver requires baseline recovery, not destructive rollback.");
        if (record.Steps.Any(x => x.Kind == StepKind.RemoveNode || x.Kind == StepKind.RemovePackage || x.Kind == StepKind.RemoveBinary))
            throw new InvalidOperationException("Removed existing resources require explicit restoration.");
        foreach (var kind in new[] { StepKind.Stage, StepKind.CreateNode, StepKind.Bind })
            if (record.Steps.Count(x => x.Kind == kind) > 1)
                throw new InvalidDataException("Ambiguous rollback ownership.");
        if (record.Operation != Operation.Install && record.Steps.Any(x => x.Kind != StepKind.Filter))
            throw new InvalidOperationException("Only retained-driver filter changes can be reversed.");
        bool undone = false;
        foreach (var step in record.Steps)
        {
            if (step.Undone) undone = true;
            else if (undone) throw new InvalidDataException("Rollback completion is not a reverse prefix.");
        }
    }

    // Prove the remaining resources independently after each acknowledged reboot.
    // In particular, a completed removal may still be pending until that boot.
    void VerifyRollbackPrefix()
    {
        var state = backend.Inspect(); ValidateState(state);
        for (int index = 0; index < 3; index++)
        {
            var edits = record.Steps.Where(x => x.Kind == StepKind.Filter && x.FilterIndex == index).ToArray();
            if (edits.Length > 1 || !state.Filters[index].Same(edits.SingleOrDefault() is Step edit && !edit.Undone ? edit.AfterFilter! : record.Before.Filters[index]))
                throw new InvalidOperationException("Filters changed across rollback; preserve external state.");
        }
        if (record.Operation != Operation.Install)
        {
            var expected = new DriverState { Nodes = record.Before.Nodes, Packages = record.Before.Packages,
                Filters = state.Filters, ServiceExists = record.Before.ServiceExists, ServicePendingDeletion = record.Before.ServicePendingDeletion,
                BinaryHash = record.Before.BinaryHash, ControlAvailable = record.Before.ControlAvailable, Settings = record.Before.Settings };
            if (!ProtectedJournal.StateBytes(state).SequenceEqual(ProtectedJournal.StateBytes(expected)))
                throw new InvalidOperationException("Retained driver or baseline changed during rollback.");
            return;
        }
        if (!record.Before.CanInstall) throw new InvalidDataException("Fresh rollback has an owned prior driver.");
        var stage = record.Steps.SingleOrDefault(x => x.Kind == StepKind.Stage);
        var create = record.Steps.SingleOrDefault(x => x.Kind == StepKind.CreateNode);
        bool hasPackage = stage != null && !stage.Undone, hasNode = create != null && !create.Undone;
        if (state.Packages.Length != (hasPackage ? 1 : 0) || hasPackage && !string.Equals(state.Packages[0], stage!.Identity, StringComparison.OrdinalIgnoreCase) ||
            state.Nodes.Length != (hasNode ? 1 : 0) || hasNode && !string.Equals(state.Nodes[0].Id, create!.Identity, StringComparison.OrdinalIgnoreCase) || state.ServicePendingDeletion)
            throw new InvalidOperationException("Rollback resource removal/ownership did not verify after reboot.");
        bool bound = record.Steps.Any(x => x.Kind == StepKind.Bind);
        if (!bound && (state.BinaryHash != record.Before.BinaryHash || state.ServiceExists || state.ControlAvailable || state.Settings != null))
            throw new InvalidOperationException("Unbound resource prefix gained unexpected driver ownership.");
        if (hasNode && bound)
        {
            if (!state.CoreHealthy || state.Settings == null || !state.Settings.Same(ExpectedAfterBinding(null)))
                throw new InvalidOperationException("New driver or baseline changed before rollback.");
        }
        else if (hasNode)
        {
            if (state.Nodes[0].Inf != "" || state.Nodes[0].Service != "" || state.ServiceExists || state.ControlAvailable || state.Settings != null || state.BinaryHash != record.Before.BinaryHash)
                throw new InvalidOperationException("Unbound node changed before rollback.");
        }
        else if (state.ControlAvailable || !hasPackage && state.ServiceExists ||
            state.Settings != null && (!bound || !state.Settings.Same(ExpectedAfterBinding(null))) ||
            state.ServiceExists && state.Settings == null)
            throw new InvalidOperationException("Removed driver still has unexpected ownership or baseline.");
    }

    public JournalStatus Rollback(string currentBootId)
    {
        ProtectedJournal.Validate(record);
        if (record.RollbackDirection || record.Status != JournalStatus.Applied && record.Status != JournalStatus.RecoveryRequired &&
            record.Status != JournalStatus.Applying && record.Status != JournalStatus.RebootRequired)
            throw new InvalidOperationException("Cannot begin rollback from this transaction state.");
        ValidateRollbackOwnership();
        if (record.Steps.Any(x => x.Undone || x.UndoStarted)) throw new InvalidOperationException("Historical rollback direction is unknown.");
        if (record.Reboot)
        {
            if (!BootIdentity.Valid(record.BootId)) throw new InvalidOperationException("Rollback reboot has no recorded boot identity.");
            record.RollbackDirection = true; record.Status = JournalStatus.RollbackRebootRequired;
            journal.Save(record); return record.Status;
        }
        VerifyRollbackPrefix();
        AnchorNewWork(currentBootId);
        record.RollbackDirection = true;
        return ContinueRollback();
    }

    JournalStatus ResumeRollbackAfterReboot(string currentBootId)
    {
        if (!record.RollbackDirection || !record.Reboot || !BootIdentity.Valid(record.BootId) ||
            !BootIdentity.Stable(currentBootId))
            throw new InvalidOperationException("Explicit rollback direction and a new boot are required.");
        ValidateRollbackOwnership(); VerifyRollbackPrefix();
        if (!VerifyRestart(currentBootId)) return record.Status;
        record.RestartAnchor = currentBootId; record.Reboot = false;
        return ContinueRollback();
    }

    JournalStatus ContinueRollback()
    {
        record.Status = JournalStatus.RollingBack; journal.Save(record);
        bool changedFilters = false;
        try
        {
            foreach (var step in record.Steps.AsEnumerable().Reverse().Where(x => !x.Undone))
            {
                // Detach device stacks on a new boot before any owned-node removal.
                if (changedFilters && step.Kind != StepKind.Filter) return RollbackRestart();
                step.UndoStarted = true; journal.Save(record);
                if (step.Kind == StepKind.Filter)
                { backend.SetFilter(step.FilterIndex, step.AfterFilter!, step.BeforeFilter!); changedFilters = true; }
                else if (step.Kind == StepKind.CreateNode)
                    record.Reboot = backend.RemoveNode(step.Identity);
                else if (step.Kind == StepKind.Stage)
                    record.Reboot = backend.RemovePackage(step.Identity);
                step.Undone = true; journal.Save(record);
                if (record.Reboot) return RollbackRestart();
            }
            if (changedFilters) return RollbackRestart();
            var state = backend.Inspect();
            if (record.Before.BinaryHash == "" && record.Steps.Any(x => x.Kind == StepKind.Bind) && state.CanInstall && state.BinaryHash == Payload.SysHash)
            {
                record.RollbackBinaryStarted = true; journal.Save(record);
                backend.RemoveBinary();
                record.RollbackBinaryCompleted = true; journal.Save(record);
            }
            if (!ProtectedJournal.StateBytes(backend.Inspect()).SequenceEqual(ProtectedJournal.StateBytes(record.Before)))
                throw new InvalidOperationException("Rollback did not restore the observed resource state.");
            record.Status = JournalStatus.RolledBack; record.Failure = ""; journal.Save(record); return record.Status;
        }
        catch (Exception error)
        {
            record.Status = JournalStatus.RecoveryRequired; record.Failure = error.GetType().Name;
            journal.Save(record); throw;
        }
    }
    JournalStatus RollbackRestart()
    {
        if (!BootIdentity.Valid(record.BootId)) throw new InvalidOperationException("Rollback restart requires a recorded boot identity.");
        record.Reboot = true; record.Status = JournalStatus.RollbackRebootRequired;
        journal.Save(record); return record.Status;
    }
}
