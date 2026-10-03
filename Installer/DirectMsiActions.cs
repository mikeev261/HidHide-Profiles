using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Security.AccessControl;
using System.Security.Principal;
using HidHide.DriverSetup;
using Microsoft.Win32;
using Microsoft.Win32.SafeHandles;
using WixToolset.Dtf.WindowsInstaller;

namespace HidHide.Installer;

// Public-MSI orchestration. Windows Installer owns application rollback and
// reboot continuation; these actions own only the protected driver transaction.
// Profile JSON never enters an elevated action.
public static class DirectMsiActions
{
    const string TransactionProperty = "HIDHIDE_TRANSACTION";
    const string SidProperty = "HIDHIDE_INITIATING_SID";
    const string OperationProperty = "HIDHIDE_OPERATION";
    const string HelperProperty = "HIDHIDE_HELPER_PID";
    const string RecoveryProperty = "HIDHIDE_RECOVERY";
    static readonly SecurityIdentifier Admins = new(WellKnownSidType.BuiltinAdministratorsSid, null);
    static readonly SecurityIdentifier SystemSid = new(WellKnownSidType.LocalSystemSid, null);

    [CustomAction]
    public static ActionResult PrepareOperation(Session session)
    {
        try
        {
            bool remove = session["REMOVE"] == "ALL";
            bool pendingUninstall = false;
            if (TryReadMarker(out _))
            {
                using var machine = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
                using var marker = machine.OpenSubKey(MaintenanceLease.Marker);
                pendingUninstall = marker?.GetValue("Operation") as string == "uninstall";
            }
            var plan = DirectMsiPolicy.RemovalPlan(remove, pendingUninstall);
            // Do this BEFORE costing. ForceReboot executes ProductUnregister
            // even before RemoveFiles during a full removal, destroying its
            // own cached MSI. First prepare the driver as maintenance; after
            // restart the protected uninstall marker requests the real removal.
            session["HIDHIDE_UNINSTALL"] = plan.Uninstall ? "1" : "";
            if (plan.Uninstall)
            {
                session["REMOVE"] = plan.RemoveApplications ? "ALL" : "";
                session["REINSTALL"] = "";
                session["ADDLOCAL"] = "";
            }
            return ActionResult.Success;
        }
        catch (Exception error) { return ReportFailure(session, "prepare the maintenance operation", error); }
    }

    [CustomAction]
    public static ActionResult PrepareUser(Session session)
    {
        Process? helper = null;
        Guid id = Guid.NewGuid();
        try
        {
            string operation = RequestedOperation(session);
            bool recovery = TryReadMarker(out var existing);
            if (recovery) id = existing;
            // UserSID is supplied by Windows Installer, not an admin HKCU guess.
            string sid = session["UserSID"];
            _ = new SecurityIdentifier(sid);

            session[TransactionProperty] = id.ToString("D");
            session[SidProperty] = sid;
            session[OperationProperty] = operation;
            session[RecoveryProperty] = recovery ? "1" : "";
            session[HelperProperty] = "0";

            // Fresh install has no resident coordinator. Upgrade must not
            // depend on a protocol absent from the older installed CLI; its
            // elevated phase requires an exact healthy pinned driver and
            // establishes the protected barrier itself. Recovery already has
            // an administrator-owned durable barrier.
            if (!DirectMsiPolicy.NeedsResidentHelper(operation, recovery)) return ActionResult.Success;

            int.TryParse(session["CLIENTPROCESSID"], out int clientProcessId);
            using var user = new MaintenanceUser(sid, clientProcessId);
            helper = user.StartMaintenance(id, operation == "uninstall");
            WaitForReady(id, helper);
            session[HelperProperty] = helper.Id.ToString(System.Globalization.CultureInfo.InvariantCulture);
            session.Log("HidHide ordinary-user maintenance is ready for protected MSI execution.");
            helper.Dispose(); helper = null;
            return ActionResult.Success;
        }
        catch (Exception error)
        {
            try { SignalRelease(id); } catch { }
            helper?.Dispose();
            return ReportFailure(session, "prepare configuration maintenance", error);
        }
    }

    [CustomAction]
    public static ActionResult ApplyDriver(Session session)
    {
        Guid id = Guid.Empty;
        try
        {
            id = Id(session); string sid = Required(session, SidProperty); string operation = Required(session, OperationProperty);
            int helper = int.Parse(Required(session, HelperProperty), System.Globalization.CultureInfo.InvariantCulture);
            bool recovery = Required(session, RecoveryProperty, allowEmpty: true) == "1";
            using var helperProcess = !recovery && helper > 0 ? Process.GetProcessById(helper) : null;
            if (helperProcess != null) _ = helperProcess.Handle;
            if (recovery)
            {
                bool resume;
                using (var owned = Own(id))
                {
                    var existing = owned.Record;
                    if (!string.Equals(existing.InitiatingSid, sid, StringComparison.Ordinal)) throw new UnauthorizedAccessException("Maintenance belongs to another Windows user.");
                    if (OperationName(existing.Operation) != operation) throw new InvalidDataException("Pending maintenance operation does not match this MSI request.");
                    resume = existing.Status == JournalStatus.RebootRequired || existing.Status == JournalStatus.RollbackRebootRequired;
                    if (!resume && existing.Status != JournalStatus.Prepared && existing.Status != JournalStatus.Applied)
                        throw new InvalidOperationException("Pending driver maintenance requires explicit recovery.");
                } // Child worker needs the same ownership mutex.
                if (resume)
                {
                    int resumed = WorkerProcess.Run("--resume", id, session.Log);
                    if (resumed == 3010) throw new InvalidOperationException("Another restart is required before setup can continue.");
                    if (resumed != 0) throw new InvalidOperationException("Protected driver recovery failed with exit code " + resumed + ".");
                }
            }
            else
            {
                ValidateHelper(helper, sid, operation);
                using var lease = new MaintenanceLease(id, createPreparation: helper == 0);
                PreparePayload();
                var backend = new WindowsDriverBackend(Path.Combine(ProtectedJournal.Root, "payload"), lease.AssertHeld);
                var before = backend.Inspect();
                ValidateStartingState(operation, before);
                var record = new TransactionRecord
                {
                    Id = id,
                    InitiatingSid = sid,
                    Before = before,
                    BootId = BootIdentity.Current(),
                    Operation = operation == "uninstall" ? Operation.Uninstall : before.CanInstall ? Operation.Install : operation == "upgrade" ? Operation.Upgrade : Operation.Repair
                };
                DriverTransaction.PrepareMsiApply(new ProtectedJournal(id), record);
                lease.MarkPending(false, operation);
            }

            bool apply;
            using (var owned = Own(id))
            {
                var current = owned.Record;
                apply = current.Status == JournalStatus.Prepared;
                if (apply)
                {
                    // Durable before dispatch; the worker anchors the work boot.
                    DriverTransaction.PrepareMsiApply(new ProtectedJournal(id), current);
                }
            }
            if (apply)
            {
                int applied = WorkerProcess.Run("--apply", id, session.Log);
                if (applied != 0 && applied != 3010) throw new InvalidOperationException("Driver maintenance failed with exit code " + applied + ".");
            }
            using (var owned = Own(id))
            {
                var current = owned.Record;
                if (current.Status != JournalStatus.Applied && current.Status != JournalStatus.RebootRequired)
                    throw new InvalidOperationException("Driver transaction did not reach a verified forward checkpoint.");
                bool restartRequired = DirectMsiPolicy.RequiresRestart(operation, recovery, current.Reboot);
                owned.Lease.MarkPending(false, operation, restartRequired: restartRequired || current.CommitRebootRequired);
            }
            SignalRelease(id); // Confirmed checkpoint; helper can remove owned startup on uninstall.
            if (helperProcess != null && (!helperProcess.WaitForExit(5000) || helperProcess.ExitCode != 0))
                throw new InvalidOperationException("Configuration maintenance did not finish successfully. Recovery information was retained.");
            session.Log("HidHide protected driver phase completed; Windows Installer owns reboot continuation.");
            return ActionResult.Success;
        }
        catch (Exception error)
        {
            try { if (id != Guid.Empty) SignalRelease(id); } catch { }
            return ReportFailure(session, "complete driver maintenance", error);
        }
    }

    [CustomAction]
    public static ActionResult FinalizeDriver(Session session)
    {
        try
        {
            var id = Id(session); var journal = new ProtectedJournal(id); bool resume;
            using (var snapshot = Own(id)) resume = snapshot.Record.Status == JournalStatus.RebootRequired;
            if (resume)
            {
                int result = WorkerProcess.Run("--resume", id, session.Log);
                if (result != 0) throw new InvalidOperationException("Post-restart driver continuation failed with exit code " + result + ".");
            }
            using var owned = Own(id);
            var record = owned.Record;
            if (record.Status != JournalStatus.Applied) throw new InvalidOperationException("Driver transaction did not reach its verified applied state.");
            var lease = owned.Lease;
            var backend = new WindowsDriverBackend(Path.Combine(ProtectedJournal.Root, "payload"), lease.AssertHeld);
            // The MSI may force a restart even when native APIs returned Applied.
            // AFTERREBOOT/format differences alone never prove that it occurred.
            if (!new DriverTransaction(backend, journal, record).VerifyCommitRestart(BootIdentity.Current()))
                throw new InvalidOperationException("Legacy restart evidence was preserved and anchored. Restart Windows again before continuing setup.");
            var actual = backend.Inspect();
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
                    backend.RestoreSettings(expected, record.Before.Settings);
                }
            }
            record = journal.Load();
            record.Status = JournalStatus.Committed; record.Reboot = false; journal.Save(record);
            lease.Complete();
            session.Log("HidHide driver transaction committed and maintenance exclusion cleared.");
            return ActionResult.Success;
        }
        catch (Exception error)
        {
            return ReportFailure(session, "verify driver maintenance", error);
        }
    }

    internal static ActionResult ReportFailure(Session session, string phase, Exception error)
    {
        string message = "HidHide Profiles could not " + phase + ". " + error.Message;
        session.Log(message);
        using var record = new Record(1);
        record.FormatString = "[1]";
        record[1] = message + " Your profiles and recovery information have been preserved. See the Windows Installer log for details.";
        session.Message(InstallMessage.Error, record);
        return ActionResult.Failure;
    }

    [CustomAction]
    public static ActionResult RollbackDirectMsiDriver(Session session)
    {
        try
        {
            // A new MSI attempt must not undo a protected transaction that
            // began in a previous attempt (including a pre-restart retry).
            if (Required(session, RecoveryProperty, allowEmpty: true) == "1") return ActionResult.Success;
            var id = Id(session); SignalRelease(id);
            if (!MarkerMatches(id)) return ActionResult.Success;
            var journal = new ProtectedJournal(id);
            using (var owned = Own(id))
            {
                var record = owned.Record;
                if (record.Status == JournalStatus.Prepared)
                {
                    if (record.Steps.Count != 0 || record.Reboot)
                        throw new InvalidOperationException("Prepared rollback contains native work evidence.");
                    record.Status = JournalStatus.Committed; journal.Save(record); owned.Lease.Complete();
                    return ActionResult.Success;
                }
            }
            int result = WorkerProcess.Run("--rollback", id, session.Log);
            if (result == 3010)
            {
                session.Log("HidHide rollback requires a restart; protected recovery evidence was retained.");
                return ActionResult.Success;
            }
            if (result != 0) throw new InvalidOperationException("Driver rollback failed with exit code " + result + ".");
            using (var owned = Own(id))
            {
                var record = owned.Record;
                if (record.Status != JournalStatus.RolledBack) throw new InvalidOperationException("Driver rollback did not verify.");
                record.Status = JournalStatus.Committed; record.Reboot = false; journal.Save(record); owned.Lease.Complete();
            }
            return ActionResult.Success;
        }
        catch (Exception error)
        {
            session.Log("HidHide rollback retained recovery evidence: " + error.Message);
            return ActionResult.Success; // MSI rollback must continue; marker remains fail-closed.
        }
    }

    static OwnedJournal<MaintenanceLease> Own(Guid id) =>
        new(() => new MaintenanceLease(id, recovery: true), () => new ProtectedJournal(id).Load());

    static string RequestedOperation(Session session)
    {
        if (session["HIDHIDE_UNINSTALL"] == "1") return "uninstall";
        if (session["REMOVE"] == "ALL" && string.IsNullOrEmpty(session["UPGRADINGPRODUCTCODE"])) return "uninstall";
        if (!string.IsNullOrEmpty(session["WIX_UPGRADE_DETECTED"])) return "upgrade";
        return string.IsNullOrEmpty(session["Installed"]) ? "install" : "repair";
    }
    static string OperationName(Operation operation) => operation switch
    { Operation.Install => "install", Operation.Repair => "repair", Operation.Uninstall => "uninstall", Operation.Upgrade => "upgrade", _ => throw new InvalidDataException("Unknown driver operation.") };
    static void ValidateStartingState(string operation, DriverState state)
    {
        if (state.ServicePendingDeletion) throw new DriverRestartRequiredException();
        if (operation == "install" && !state.CanInstall) throw new InvalidOperationException("A clean installation requires no existing HidHide driver resources.");
        if (operation == "uninstall" && !state.CoreHealthy) throw new InvalidOperationException("Uninstall requires the installed signed driver to be healthy.");
        if (operation is "repair" or "upgrade" && !(state.CoreHealthy || operation == "repair" && state.Repairable))
            throw new InvalidOperationException("The existing driver state is not safe for in-place maintenance.");
    }
    static void WaitForReady(Guid id, Process helper)
    {
        var deadline = DateTime.UtcNow.AddSeconds(20);
        while (DateTime.UtcNow < deadline)
        {
            if (helper.HasExited) throw new InvalidOperationException("The configuration helper exited before confirming readiness (code " + helper.ExitCode + "). Open HidHide Profiles, resolve any pending recovery or ownership message, then retry setup.");
            try
            {
                using var ready = EventWaitHandle.OpenExisting(EventName("Ready", id));
                if (ready.WaitOne(250)) return;
            }
            catch (WaitHandleCannotBeOpenedException) { Thread.Sleep(100); }
        }
        throw new TimeoutException("Configuration maintenance did not become ready.");
    }
    static void ValidateHelper(int pid, string sid, string operation)
    {
        if (!DirectMsiPolicy.NeedsResidentHelper(operation, recovery: false))
        {
            if (pid != 0) throw new InvalidDataException("Install and upgrade cannot use a historical maintenance helper.");
            return;
        }
        if (pid <= 0) throw new InvalidDataException("Existing installation maintenance requires an ordinary-user helper.");
        using var process = Process.GetProcessById(pid);
        if (process.HasExited || !OpenProcessToken(process.Handle, 8, out var token)) throw new UnauthorizedAccessException("Cannot authenticate configuration maintenance.");
        using (token) MaintenanceUser.Validate(token, sid, process.SessionId);
    }
    static void SignalRelease(Guid id)
    {
        try { using var release = EventWaitHandle.OpenExisting(EventName("Release", id)); release.Set(); }
        catch (WaitHandleCannotBeOpenedException) { }
    }
    static string EventName(string role, Guid id) => @"Global\HidHide.Profiles.Msi." + role + "." + id.ToString("D");
    static bool TryReadMarker(out Guid id)
    {
        id = Guid.Empty;
        using var machine = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
        using var marker = machine.OpenSubKey(MaintenanceLease.Marker);
        if (marker == null) return false;
        return Guid.TryParseExact(marker.GetValue("Transaction") as string, "D", out id) && id != Guid.Empty
            ? true : throw new InvalidDataException("The protected HidHide maintenance marker is invalid.");
    }
    static bool MarkerMatches(Guid id)
    {
        try { return TryReadMarker(out var actual) && actual == id; } catch { return false; }
    }
    static Guid Id(Session session)
    {
        string value = Required(session, TransactionProperty);
        return Guid.TryParseExact(value, "D", out var id) && id != Guid.Empty ? id : throw new InvalidDataException("MSI transaction identity is missing or invalid.");
    }
    static string Required(Session session, string name, bool allowEmpty = false)
    {
        string value = session.CustomActionData[name];
        if (!allowEmpty && string.IsNullOrWhiteSpace(value)) throw new InvalidDataException(name + " is missing.");
        return value;
    }
    static FileSecurity FileAcl()
    {
        var acl = new FileSecurity(); acl.SetAccessRuleProtection(true, false); acl.SetOwner(Admins);
        foreach (var sid in new[] { Admins, SystemSid }) acl.AddAccessRule(new FileSystemAccessRule(sid, FileSystemRights.FullControl, AccessControlType.Allow));
        return acl;
    }
    static void PreparePayload()
    {
        ProtectedJournal.CreateRoot();
        string target = Path.Combine(ProtectedJournal.Root, "payload");
        if (Directory.Exists(target))
        {
            ProtectedJournal.ValidateDirectory(target);
            foreach (string name in new[] { "HidHide.inf", "HidHide.sys", "hidhide.cat", "LICENSE.rtf" })
                ProtectedJournal.ValidateAcl(File.GetAccessControl(Path.Combine(target, name)));
            Payload.Verify(target);
            return;
        }
        string source = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "HidHide", "Driver");
        ProtectedJournal.RejectReparsePath(source); Payload.Verify(source);
        if (!Directory.Exists(target))
        {
            var acl = new DirectorySecurity(); acl.SetAccessRuleProtection(true, false); acl.SetOwner(Admins);
            foreach (var sid in new[] { Admins, SystemSid }) acl.AddAccessRule(new FileSystemAccessRule(sid, FileSystemRights.FullControl,
                InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit, PropagationFlags.None, AccessControlType.Allow));
            Directory.CreateDirectory(target, acl);
        }
        ProtectedJournal.ValidateDirectory(target);
        foreach (string name in new[] { "HidHide.inf", "HidHide.sys", "hidhide.cat", "LICENSE.rtf" })
        {
            string destination = Path.Combine(target, name);
            if (!File.Exists(destination))
            {
                using var input = new FileStream(Path.Combine(source, name), FileMode.Open, FileAccess.Read, FileShare.Read);
                using var output = new FileStream(destination, FileMode.CreateNew, FileSystemRights.Write, FileShare.None, 4096, FileOptions.WriteThrough, FileAcl());
                input.CopyTo(output); output.Flush(true);
            }
            ProtectedJournal.ValidateAcl(File.GetAccessControl(destination));
        }
        Payload.Verify(target);
    }

    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool OpenProcessToken(IntPtr process, uint access, out SafeAccessTokenHandle token);
}
