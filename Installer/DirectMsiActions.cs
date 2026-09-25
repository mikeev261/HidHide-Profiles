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
            var flow = new DirectMsiFlow(new MachineHost(session, helperProcess));
            if (recovery) flow.ResumePending(id, sid, operation);
            else
            {
                ValidateHelper(helper, sid, operation);
                using var lease = new MaintenanceLease(id, createPreparation: helper == 0);
                PreparePayload(session.Log);
                var backend = new WindowsDriverBackend(Path.Combine(ProtectedJournal.Root, "payload"), lease.AssertHeld);
                var before = backend.Inspect();
                DirectMsiFlow.ValidateStartingState(operation, before);
                var record = new TransactionRecord
                {
                    Id = id,
                    InitiatingSid = sid,
                    Before = before,
                    BootId = BootIdentity.Current(),
                    Operation = operation == "uninstall" ? Operation.Uninstall : before.CanInstall ? Operation.Install : operation == "upgrade" ? Operation.Upgrade : Operation.Repair
                };
                new ProtectedJournal(id).Save(record);
                lease.MarkPending(false, operation);
            }

            flow.Apply(id, operation, recovery);
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
            if (new DirectMsiFlow(new MachineHost(session)).Finalize(Id(session)))
            {
                session.SetMode(InstallRunMode.RebootAtEnd, true);
                session.Log("HidHide restored driver filters that take effect after the restart requested at the end of setup.");
            }
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
            new DirectMsiFlow(new MachineHost(session)).Rollback(Id(session));
            return ActionResult.Success;
        }
        catch (Exception error)
        {
            session.Log("HidHide rollback retained recovery evidence: " + error.Message);
            return ActionResult.Success; // MSI rollback must continue; marker remains fail-closed.
        }
    }

    static string RequestedOperation(Session session)
    {
        if (session["HIDHIDE_UNINSTALL"] == "1") return "uninstall";
        if (session["REMOVE"] == "ALL" && string.IsNullOrEmpty(session["UPGRADINGPRODUCTCODE"])) return "uninstall";
        if (!string.IsNullOrEmpty(session["WIX_UPGRADE_DETECTED"])) return "upgrade";
        return string.IsNullOrEmpty(session["Installed"]) ? "install" : "repair";
    }
    // Production resources for DirectMsiFlow: the protected journal, durable
    // marker, elevated worker and the installed driver behind the lease.
    sealed class MachineHost : IDirectMsiHost
    {
        readonly Session session;
        readonly Process? helper;
        public MachineHost(Session session, Process? helper = null) { this.session = session; this.helper = helper; }
        public bool WaitForHelper(int milliseconds) => helper == null || helper.WaitForExit(milliseconds) && helper.ExitCode == 0;
        public TransactionRecord Load(Guid id) => new ProtectedJournal(id).Load();
        public void Save(TransactionRecord record) => new ProtectedJournal(record.Id).Save(record);
        public int RunWorker(string verb, Guid id) => WorkerProcess.Run(verb, id, session.Log);
        public IDirectMsiLease OpenLease(Guid id) => new MachineLease(id);
        public bool MarkerMatches(Guid id) => DirectMsiActions.MarkerMatches(id);
        public string CurrentBoot() => BootIdentity.Current();
        public void SignalRelease(Guid id) => DirectMsiActions.SignalRelease(id);
        public void Log(string message) => session.Log(message);
    }
    sealed class MachineLease : IDirectMsiLease
    {
        readonly MaintenanceLease lease;
        readonly WindowsDriverBackend backend;
        public MachineLease(Guid id)
        {
            lease = new MaintenanceLease(id, recovery: true);
            backend = new WindowsDriverBackend(Path.Combine(ProtectedJournal.Root, "payload"), lease.AssertHeld);
        }
        public void MarkPending(string operation, bool restartRequired) => lease.MarkPending(false, operation, restartRequired: restartRequired);
        public DriverState Inspect() => backend.Inspect();
        public void RestoreSettings(DriverSettings expected, DriverSettings desired) => backend.RestoreSettings(expected, desired);
        public void Complete() => lease.Complete();
        public void Dispose() => lease.Dispose();
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
    static void PreparePayload(Action<string> log)
    {
        ProtectedJournal.CreateRoot(log);
        string target = Path.Combine(ProtectedJournal.Root, "payload");
        string source = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "HidHide", "Driver");
        if (!Directory.Exists(target))
        {
            var acl = new DirectorySecurity(); acl.SetAccessRuleProtection(true, false); acl.SetOwner(Admins);
            foreach (var sid in new[] { Admins, SystemSid }) acl.AddAccessRule(new FileSystemAccessRule(sid, FileSystemRights.FullControl,
                InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit, PropagationFlags.None, AccessControlType.Allow));
            Directory.CreateDirectory(target, acl);
        }
        ProtectedJournal.ValidateDirectory(target);
        PayloadCache.Prepare(source, target, Payload.Files,
            path => new FileStream(path, FileMode.CreateNew, FileSystemRights.Write, FileShare.None, 4096, FileOptions.WriteThrough, FileAcl()),
            path => ProtectedJournal.ValidateAcl(File.GetAccessControl(path)));
        Payload.Verify(target);
    }

    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool OpenProcessToken(IntPtr process, uint access, out SafeAccessTokenHandle token);
}
