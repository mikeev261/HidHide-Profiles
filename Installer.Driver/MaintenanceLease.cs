using System.Runtime.InteropServices;
using System.Security.AccessControl;
using System.Security.Principal;
using Microsoft.Win32;
using Microsoft.Win32.SafeHandles;

namespace HidHide.DriverSetup;

// Durable marker is intentionally readable by ordinary configuration clients.
// Only this elevated worker writes it. It is exclusion, never authorization.
public sealed class MaintenanceLease : IDisposable
{
    public const string Marker = @"SOFTWARE\mikeev261\HidHide\Maintenance";
    readonly SafeWaitHandle barrier;
    readonly EventWaitHandle? recoveryBarrier;
    readonly Mutex owner;
    bool owned;
    readonly Guid transaction;
    readonly bool recovery;
    public MaintenanceLease(Guid transaction, bool recovery = false, bool createPreparation = false)
    {
        ProtectedJournal.RequireAdministrator(); this.transaction = transaction; this.recovery = recovery;
        if (recovery || createPreparation)
        {
            var eventSecurity = new EventWaitHandleSecurity(); eventSecurity.SetAccessRuleProtection(true, false);
            eventSecurity.AddAccessRule(new EventWaitHandleAccessRule(new SecurityIdentifier(WellKnownSidType.AuthenticatedUserSid, null), EventWaitHandleRights.Synchronize, AccessControlType.Allow));
            foreach (var sid in new[] { WellKnownSidType.BuiltinAdministratorsSid, WellKnownSidType.LocalSystemSid })
                eventSecurity.AddAccessRule(new EventWaitHandleAccessRule(new SecurityIdentifier(sid, null), EventWaitHandleRights.FullControl, AccessControlType.Allow));
            recoveryBarrier = new EventWaitHandle(false, EventResetMode.ManualReset, @"Global\HidHide.AppProfiles.Maintenance.v1", out bool created, eventSecurity);
            if (createPreparation && !created)
            {
                recoveryBarrier.Dispose();
                throw new InvalidOperationException("Another ordinary-user maintenance preparation is active.");
            }
            barrier = recoveryBarrier.SafeWaitHandle;
        }
        else barrier = OpenEventW(0x100000, false, @"Global\HidHide.AppProfiles.Maintenance.v1");
        if (barrier.IsInvalid) { barrier.Dispose(); throw new InvalidOperationException("Ordinary-user maintenance preparation is not active."); }
        var security = new MutexSecurity();
        security.SetAccessRuleProtection(true, false);
        security.AddAccessRule(new MutexAccessRule(new SecurityIdentifier(WellKnownSidType.AuthenticatedUserSid, null), MutexRights.Synchronize | MutexRights.Modify, AccessControlType.Allow));
        foreach (var sid in new[] { WellKnownSidType.BuiltinAdministratorsSid, WellKnownSidType.LocalSystemSid })
            security.AddAccessRule(new MutexAccessRule(new SecurityIdentifier(sid, null), MutexRights.FullControl, AccessControlType.Allow));
        Mutex? acquired = null;
        try
        {
            acquired = new Mutex(false, @"Global\HidHide.AppProfiles.Coordinator.v1", out _, security);
            try { owned = acquired.WaitOne(5000); } catch (AbandonedMutexException) { owned = true; }
            if (!owned) throw new InvalidOperationException("Maintenance ownership handoff timed out.");
            // Validate after waiting: the preceding owner may have committed
            // and cleared its marker, or a different transaction may own it.
            if (recovery)
            {
                ValidateMarker(transaction);
                _ = new ProtectedJournal(transaction).Load();
            }
            else
            {
                using var machine = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
                using var existing = machine.OpenSubKey(Marker);
                if (existing != null) throw new InvalidOperationException("Another protected maintenance transaction is active.");
            }
            owner = acquired;
        }
        catch { if (owned) { acquired!.ReleaseMutex(); owned = false; } acquired?.Dispose(); barrier.Dispose(); recoveryBarrier?.Dispose(); throw; }
    }
    public void MarkPending(bool rollback, string? operation = null, bool canRestoreLegacy = false, bool restartRequired = false)
    {
        AssertHeld();
        // A recovery lease must never recreate a marker cleared by completion.
        if (recovery) ValidateMarker(transaction);
        if (operation != null && operation is not ("install" or "repair" or "uninstall" or "upgrade"))
            throw new InvalidDataException("Invalid maintenance operation metadata.");
        using var machine = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
        // Forward MSI workers and recovery may reuse only this transaction's marker.
        using (var existing = machine.OpenSubKey(Marker, operation != null))
            if (existing != null)
            {
                ValidateMarker(transaction);
                if (operation != null) WriteDisplayMetadata(existing, operation, canRestoreLegacy, restartRequired);
                return;
            }
        var acl = new RegistrySecurity(); acl.SetAccessRuleProtection(true, false);
        var admins = new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null);
        acl.SetOwner(admins);
        foreach (var sid in new[] { admins, new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null) })
            acl.AddAccessRule(new RegistryAccessRule(sid, RegistryRights.FullControl, InheritanceFlags.ContainerInherit, PropagationFlags.None, AccessControlType.Allow));
        acl.AddAccessRule(new RegistryAccessRule(new SecurityIdentifier(WellKnownSidType.AuthenticatedUserSid, null), RegistryRights.ReadKey, InheritanceFlags.ContainerInherit, PropagationFlags.None, AccessControlType.Allow));
        using var key = machine.CreateSubKey(Marker, RegistryKeyPermissionCheck.ReadWriteSubTree, acl);
        key.SetValue("Transaction", transaction.ToString("D"), RegistryValueKind.String);
        if (operation != null) WriteDisplayMetadata(key, operation, canRestoreLegacy, restartRequired);
        key.Flush();
    }
    static void WriteDisplayMetadata(RegistryKey key, string operation, bool canRestoreLegacy, bool restartRequired)
    {
        // These values are ordinary-user-readable display hints only. The
        // protected setup journal remains the sole authorization source.
        key.SetValue("Operation", operation, RegistryValueKind.String);
        key.SetValue("CanRestoreLegacy", canRestoreLegacy ? 1 : 0, RegistryValueKind.DWord);
        key.SetValue("RestartRequired", restartRequired ? 1 : 0, RegistryValueKind.DWord);
        key.Flush();
    }
    public void AssertHeld()
    { if (!owned || barrier.IsInvalid || barrier.IsClosed) throw new InvalidOperationException("Maintenance lease lost."); }
    public static void ValidateMarker(Guid transaction)
    {
        using var machine = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
        using var key = machine.OpenSubKey(Marker) ?? throw new InvalidOperationException("Missing durable maintenance marker.");
        var acl = key.GetAccessControl();
        var admins = new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null);
        var system = new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null);
        var users = new SecurityIdentifier(WellKnownSidType.AuthenticatedUserSid, null);
        var owner = acl.GetOwner(typeof(SecurityIdentifier));
        var rules = acl.GetAccessRules(true, true, typeof(SecurityIdentifier)).Cast<RegistryAccessRule>().ToArray();
        if ((!owner.Equals(admins) && !owner.Equals(system)) || !acl.AreAccessRulesProtected || rules.Length != 3 ||
            rules.Any(x => x.AccessControlType != AccessControlType.Allow ||
                (x.IdentityReference.Equals(users) ? x.RegistryRights != RegistryRights.ReadKey :
                 (!x.IdentityReference.Equals(admins) && !x.IdentityReference.Equals(system)) || x.RegistryRights != RegistryRights.FullControl)) ||
            !rules.Any(x => x.IdentityReference.Equals(admins)) || !rules.Any(x => x.IdentityReference.Equals(system)) || !rules.Any(x => x.IdentityReference.Equals(users)) ||
            key.GetValueKind("Transaction") != RegistryValueKind.String || !string.Equals(key.GetValue("Transaction") as string, transaction.ToString("D"), StringComparison.Ordinal))
            throw new UnauthorizedAccessException("Maintenance marker identity or ACL is invalid.");
    }
    public void Complete()
    {
        AssertHeld(); ValidateMarker(transaction);
        var record = new ProtectedJournal(transaction).Load();
        if (record.Status != JournalStatus.Committed || record.Reboot || record.Steps.Any(x => !x.Completed))
            throw new InvalidOperationException("Driver transaction is not durably committed.");
        ClearVerifiedMarker();
    }
    // The elevated legacy controller owns an independently persisted restoration
    // record. Its callback verifies that record plus original products/files and
    // settings while this lease remains held. Preserve the original native log.
    public void CompleteVerifiedLegacyRecovery(Action verifyRestored)
    {
        AssertHeld(); ValidateMarker(transaction);
        if (verifyRestored == null) throw new ArgumentNullException(nameof(verifyRestored));
        verifyRestored();
        ClearVerifiedMarker();
    }
    void ClearVerifiedMarker()
    {
        using var machine = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
        // Delete only the known key, never descendants or a caller-supplied path.
        machine.DeleteSubKey(Marker, true);
        using var parent = machine.OpenSubKey(@"SOFTWARE\mikeev261\HidHide", true);
        parent?.Flush();
    }
    public void Dispose()
    {
        if (owned) { owner.ReleaseMutex(); owned = false; }
        owner.Dispose(); barrier.Dispose(); recoveryBarrier?.Dispose();
        // Never clear durable exclusion on worker exit, timeout or MSI rollback.
        // The final recovery controller must verify MSI/settings/reboot completion.
    }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, ExactSpelling = true)] static extern SafeWaitHandle OpenEventW(uint access, bool inherit, string name);
}
