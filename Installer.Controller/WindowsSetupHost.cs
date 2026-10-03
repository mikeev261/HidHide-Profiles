using System.Diagnostics;
using HidHide.DriverSetup;
using HidHide.Installer;

namespace HidHide.Setup;

public sealed partial class WindowsSetupHost : ISetupHost, ILegacyRecoveryHost, IDisposable
{
    readonly ControllerStore store;
    readonly Version version;
    MaintenanceLease? lease;
    public WindowsSetupHost(Guid id, Version version, bool recovery) { store = new ControllerStore(id); this.version = version; lease = new MaintenanceLease(id, recovery); }
    public void ReleaseForMsi() { lease!.Dispose(); lease = null; }
    public void Reacquire(Guid id) { if (lease != null) throw new InvalidOperationException("Already owns maintenance."); lease = new MaintenanceLease(id, true); }
    WindowsDriverBackend Backend => new(Path.Combine(ProtectedJournal.Root, "payload"), () => lease!.AssertHeld());
    public string Boot => BootIdentity.Current();
    public void Save(SetupRecord record) => store.Save(record);
    public void VerifyCache(SetupRecord record) => store.Verify(record);
    public IReadOnlyList<InstalledProduct> Detect() => InstalledProducts.Detect(version);
    public DriverState Inspect() => Backend.Inspect();
    public int RemoveLegacy(Guid product)
    {
        lease!.AssertHeld();
        // ProductCode-only, fixed system executable, outside any MSI custom action.
        var info = new ProcessStartInfo(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "msiexec.exe"), "/x " + product.ToString("B") + " /qn /norestart REBOOT=ReallySuppress") { UseShellExecute = false, CreateNoWindow = true };
        using var process = Process.Start(info)!;
        if (!process.WaitForExit(120000)) throw new TimeoutException("Legacy Windows Installer operation is still running; retain exclusion and inspect before recovery.");
        return process.ExitCode;
    }
    public void PrepareDriver(SetupRecord record)
    {
        bool initialSnapshot = record.BeforeMsiFiles == null;
        var files = MsiRollbackRecovery.CaptureFiles();
        var bundles = MsiRollbackRecovery.CaptureBundles();
        if (record.BeforeMsiFiles != null && !MsiRollbackRecovery.Same(record.BeforeMsiFiles, files) || record.BeforeMsiBundles != null && !MsiRollbackRecovery.Same(record.BeforeMsiBundles, bundles))
            throw new InvalidOperationException("MSI-owned files or bundle registration changed after preparation.");
        var before = Inspect();
        var previous = !initialSnapshot || record.MsiRetryCount > 0 ? new ProtectedJournal(record.Id).Load() : null;
        VerifyNativeBeforeReplacingPreparation(record, previous, before);
        record.BeforeMsiFiles = files; record.BeforeMsiBundles = bundles;
        var products = Detect();
        if (products.Count > 1) throw new InvalidOperationException("Ambiguous product ownership before MSI preparation.");
        var existingProduct = products.Count == 0 ? Guid.Empty : products[0].Product;
        if (!initialSnapshot && existingProduct != record.BeforeMsiProduct) throw new InvalidOperationException("MSI registration changed after preparation.");
        record.BeforeMsiProduct = existingProduct;
        if (record.Operation == Operation.Uninstall && before.Empty && products.Count == 0)
        {
            var completed = new ProtectedJournal(record.Id).Load();
            if (completed.Operation != Operation.Uninstall || completed.Status != JournalStatus.Applied || completed.Reboot || completed.Steps.Any(x => !x.Completed || x.Undone))
                throw new InvalidOperationException("Completed native removal evidence is required before bundle-only cleanup.");
            return;
        }
        if (products.Any(x => x.Family != ProductContract.MsiUpgradeCode)) throw new InvalidOperationException("Legacy registrations remain; replacement refused.");
        if (record.Operation == Operation.Upgrade && (record.PriorUnified == null || products.Count != 1 || products[0].Product != record.PriorUnified.Product || !before.Healthy))
            throw new InvalidOperationException("Prior unified ownership or healthy driver changed before upgrade.");
        if (record.Legacy.Any(x => x.Family == ProductContract.UpstreamUpgradeCode) && !before.CanInstall)
            throw new InvalidOperationException("Legacy uninstaller left driver resources; explicit recovery is required.");
        bool ownedRepair = record.Operation == Operation.Repair && products.Count == 1 && products[0].Family == ProductContract.MsiUpgradeCode && products[0].Operation == ProductContract.Operation.Repair;
        if (record.Operation == Operation.Uninstall ? !before.CoreHealthy : !(before.CanInstall || before.CoreHealthy || ownedRepair && before.Repairable))
            throw new InvalidOperationException("Unproven or damaged driver resources require explicit recovery.");
        var driver = new TransactionRecord { Id = record.Id, InitiatingSid = record.Sid, Before = before, BootId = Boot,
            Operation = record.Operation == Operation.Uninstall ? Operation.Uninstall : before.CanInstall ? Operation.Install : record.Operation == Operation.Upgrade ? Operation.Upgrade : Operation.Repair };
        new ProtectedJournal(record.Id).Save(driver);
    }
    public static void VerifyNativeBeforeReplacingPreparation(SetupRecord record, TransactionRecord? previous, DriverState actual)
    {
        // A pre-Apply cancellation is a reused preparation too, even though it
        // did not increment the failed-MSI retry counter. Its original baseline
        // must never be replaced with a later historical client's changed state.
        if (record.BeforeMsiFiles == null && record.MsiRetryCount == 0) return;
        if (previous == null) throw new InvalidOperationException("Previous native preparation is missing.");
        MsiRollbackRecovery.VerifyNative(record, previous, actual);
    }
    public bool VerifyMsiRollbackAndArchive(SetupRecord record)
    {
        lease!.AssertHeld();
        if (record.Phase != SetupPhase.RecoveryRequired || !record.MsiFailureReported || record.BeforeMsiFiles == null || record.BeforeMsiBundles == null || record.Legacy.Any(x => !x.Removed))
            throw new InvalidOperationException("A reported terminal MSI failure with complete starting evidence is required for retry.");
        MsiRollbackRecovery.ValidateFiles(record.BeforeMsiFiles);
        var products = Detect();
        Guid expected = record.BeforeMsiProduct;
        if (expected == Guid.Empty ? products.Count != 0 : products.Count != 1 || products[0].Family != ProductContract.MsiUpgradeCode || products[0].Product != expected || products[0].Operation != (record.Operation == Operation.Upgrade ? ProductContract.Operation.Upgrade : ProductContract.Operation.Repair))
            throw new InvalidOperationException("Windows Installer ownership was not restored after failure.");
        if (!MsiRollbackRecovery.Same(record.BeforeMsiFiles, MsiRollbackRecovery.CaptureFiles()) || !MsiRollbackRecovery.Same(record.BeforeMsiBundles, MsiRollbackRecovery.CaptureBundles()))
            throw new InvalidOperationException("Application files or prior bundle registration were not restored after failure.");
        var journal = new ProtectedJournal(record.Id); var driver = journal.Load();
        MsiRollbackRecovery.VerifyIdentity(record, driver);
        if (driver.Status == JournalStatus.RebootRequired && !driver.RollbackDirection)
        {
            // A terminal failed Apply plus restored MSI/Burn state explicitly
            // authorizes conversion; old forward records are never inferred undo.
            new DriverTransaction(Backend, journal, driver).Rollback(BootIdentity.Current());
            return false;
        }
        if (driver.Status == JournalStatus.RollbackRebootRequired)
        {
            if (driver.BootId == Boot) return false;
            var outcome = new DriverTransaction(Backend, journal, driver).ResumeAfterReboot(Boot);
            if (outcome == JournalStatus.RollbackRebootRequired) return false;
            if (outcome != JournalStatus.RolledBack) throw new InvalidOperationException("Native rollback remains incomplete.");
        }
        MsiRollbackRecovery.VerifyNative(record, driver, Inspect());
        store.ArchiveDriverRetry(record, driver);
        return true;
    }
    public bool DriverNeedsReboot(SetupRecord record)
    {
        var driver = new ProtectedJournal(record.Id).Load();
        if (driver.Status != JournalStatus.Applied && driver.Status != JournalStatus.RebootRequired)
            throw new InvalidOperationException("MSI driver transaction did not complete.");
        return driver.Status == JournalStatus.RebootRequired;
    }
    public void VerifyMsiNotStarted(SetupRecord record)
    {
        lease!.AssertHeld();
        var driver = new ProtectedJournal(record.Id).Load();
        SetupTransaction.VerifyUntouchedDriver(record, driver, Inspect(), Boot);
        var products = Detect();
        bool unchanged = record.Operation == Operation.Install ? products.Count == 0 :
            products.Count == 1 && products[0].Family == ProductContract.MsiUpgradeCode &&
            products[0].Product == (record.Operation == Operation.Upgrade ? record.PriorUnified!.Product : record.PackageProduct);
        if (!unchanged) throw new InvalidOperationException("MSI ownership changed before cancellation; recovery retained.");
    }
    public bool ResumeDriver(SetupRecord record)
    {
        var journal = new ProtectedJournal(record.Id); var driver = journal.Load();
        if (driver.Status == JournalStatus.RebootRequired)
        {
            var outcome = new DriverTransaction(Backend, journal, driver).ResumeAfterReboot(Boot);
            if (outcome == JournalStatus.RebootRequired) return false;
            if (outcome != JournalStatus.Applied) throw new InvalidOperationException("Driver recovery requires explicit inspection.");
        }
        else if (driver.Status != JournalStatus.Applied) throw new InvalidOperationException("Driver transaction outcome is uncertain.");
        return true;
    }
    public void RestoreBaseline(SetupRecord record)
    {
        if (record.Operation == Operation.Uninstall) return;
        var driver = new ProtectedJournal(record.Id).Load();
        var actual = Inspect();
        if (!actual.Healthy || actual.Settings == null) throw new InvalidOperationException("Driver control is not ready for restoration.");
        var expected = driver.Operation == Operation.Install || driver.Operation == Operation.Repair && !driver.Before.CoreHealthy ? DriverTransaction.ExpectedAfterBinding(driver.Operation == Operation.Repair ? driver.Before.Settings : null) : driver.Before.Settings!;
        var desired = record.Before.Settings ?? expected;
        // Historical clients can ignore exclusion. Never bless arbitrary fresh
        // state as an expected value after reboot; only known driver defaults or
        // the recorded healthy baseline may be replaced.
        if (!actual.Settings.Same(expected)) throw new InvalidOperationException("Driver settings changed during setup; baseline restoration refused.");
        Backend.RestoreSettings(expected, desired);
    }
    public void VerifyFinal(SetupRecord record)
    {
        var products = Detect(); var state = Inspect();
        if (record.Operation == Operation.Uninstall)
        {
            if (products.Count != 0 || !state.Empty) throw new InvalidOperationException("Uninstall remains incomplete.");
        }
        else
        {
            if (products.Count != 1 || products[0].Family != ProductContract.MsiUpgradeCode || products[0].Operation != ProductContract.Operation.Repair || !state.Healthy)
                throw new InvalidOperationException("Unified registration/driver verification failed.");
            if (record.Before.Settings != null && (state.Settings == null || !state.Settings.Same(record.Before.Settings))) throw new InvalidOperationException("Baseline read-back failed.");
            store.VerifyInstalledApplications();
            foreach (string name in new[] { "HidHideCLI.exe", "HidHideClient.exe" })
            {
                string path = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "HidHide", name);
                ProtectedJournal.RejectReparsePath(path);
                if (ProductContract.ParseVersion(FileVersionInfo.GetVersionInfo(path).FileVersion ?? "") != version)
                    throw new InvalidOperationException("Installed application version did not verify.");
            }
        }
    }
    public void ClearMarker(SetupRecord record)
    {
        var journal = new ProtectedJournal(record.Id); var driver = journal.Load();
        if (driver.Status != JournalStatus.Applied && driver.Status != JournalStatus.Committed) throw new InvalidOperationException("Driver transaction remains incomplete.");
        driver.Status = JournalStatus.Committed; driver.Reboot = false; journal.Save(driver); lease!.Complete();
    }
    public void MarkPending(SetupRecord record) => lease!.MarkPending(false, record.Operation.ToString().ToLowerInvariant(),
        record.Legacy.Any(x => x.RemovalIntent || x.Removed), record.Phase == SetupPhase.WaitingForReboot);
    public void Dispose() => lease?.Dispose();
}
