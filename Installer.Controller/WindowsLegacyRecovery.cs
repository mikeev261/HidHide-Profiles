using System.Diagnostics;
using HidHide.DriverSetup;
using HidHide.Installer;

namespace HidHide.Setup;

public sealed partial class WindowsSetupHost
{
    public void SaveRecovery(LegacyRecoveryRecord recovery) => store.SaveRecovery(recovery);
    public void VerifyRecovery(SetupRecord setup)
    {
        lease!.AssertHeld(); store.Verify(setup);
        LegacyFileEvidence.Validate(setup.LegacyFiles!);
        if (setup.Legacy.Any(x => x.Family == ProductContract.UpstreamUpgradeCode)) LegacyServices.Validate(setup.LegacyService!);
        VerifyLegacySurvivors(setup);
        var native = new ProtectedJournal(setup.Id).Load();
        if (native.Status != JournalStatus.Prepared || native.Steps.Count != 0 || native.Reboot || native.Id != setup.Id || native.InitiatingSid != setup.Sid)
            throw new InvalidOperationException("Original native transaction has started; legacy restoration refused.");
        if ((!setup.Before.Healthy || setup.Before.Settings == null) && !(setup.Before.Empty && !setup.Legacy.Any(x => x.Family == ProductContract.UpstreamUpgradeCode)))
            throw new InvalidOperationException("Legacy recovery requires the originally verified healthy driver baseline.");
        VerifyForeignFilters(setup.Before, Inspect());
    }
    public void QuiesceLegacyService(SetupRecord setup) { lease!.AssertHeld(); if (setup.LegacyService != null) LegacyServices.StopVerified(setup.LegacyService); }
    public void RestoreLegacyService(SetupRecord setup) { lease!.AssertHeld(); if (setup.LegacyService != null) LegacyServices.Restore(setup.LegacyService); }
    static void VerifyLegacySurvivors(SetupRecord setup) => LegacyFileEvidence.VerifySurvivingFiles(setup.Legacy.Select(x => new InstalledProduct {
        Family = x.Family, Product = x.Product, Version = Version.Parse(x.Version),
        Operation = x.Family == ProductContract.UpstreamUpgradeCode ? ProductContract.Operation.MigrateUpstream : ProductContract.Operation.MigrateCompanion
    }).ToArray(), setup.LegacyFiles!);
    static void VerifyForeignFilters(DriverState original, DriverState actual)
    {
        if (original.Filters.Length != 3 || actual.Filters.Length != 3) throw new InvalidOperationException("Missing class-filter evidence.");
        for (int i = 0; i < 3; ++i)
            if (!original.Filters[i].Entries.Where(x => !DriverFilters.IsHidHide(x)).SequenceEqual(actual.Filters[i].Entries.Where(x => !DriverFilters.IsHidHide(x))))
                throw new InvalidOperationException("Unrelated class filters changed; legacy recovery refused.");
    }
    bool NativeRecoveryStep(SetupRecord setup, LegacyRecoveryRecord recovery, Operation operation)
    {
        lease!.AssertHeld();
        VerifyLegacySurvivors(setup);
        if (recovery.NativeAttempt == Guid.Empty)
        {
            var state = Inspect(); VerifyForeignFilters(setup.Before, state);
            var id = Guid.NewGuid();
            var native = new TransactionRecord { Id = id, InitiatingSid = setup.Sid, Before = state, BootId = Boot, Operation = operation };
            new ProtectedJournal(id).Save(native);
            recovery.NativeAttempt = id; store.SaveRecovery(recovery);
        }
        var journal = new ProtectedJournal(recovery.NativeAttempt);
        var record = journal.Load();
        if (record.Operation != operation || record.InitiatingSid != setup.Sid) throw new InvalidDataException("Legacy native recovery identity changed.");
        if (record.Status == JournalStatus.RebootRequired && record.BootId == Boot) return false;
        var transaction = new DriverTransaction(Backend, journal, record);
        var result = record.Status == JournalStatus.Prepared ? transaction.Apply(Boot) : record.Status == JournalStatus.RebootRequired ? transaction.ResumeAfterReboot(Boot) : record.Status;
        if (result == JournalStatus.RebootRequired) return false;
        if (result != JournalStatus.Applied && result != JournalStatus.Committed)
            throw new InvalidOperationException("Interrupted native recovery must be inspected; original and recovery journals are retained.");
        // Keep the distinct journal forever; no original native intent is erased.
        record.Status = JournalStatus.Committed; journal.Save(record);
        return true;
    }
    public bool PrepareLegacyDriver(SetupRecord setup, LegacyRecoveryRecord recovery)
    {
        var actual = Inspect(); VerifyForeignFilters(setup.Before, actual);
        if (!setup.Legacy.Any(x => x.Family == ProductContract.UpstreamUpgradeCode))
        {
            if (!ProtectedJournal.StateBytes(actual).SequenceEqual(ProtectedJournal.StateBytes(setup.Before)))
                throw new InvalidOperationException("Companion-only recovery cannot change driver resources.");
            return true;
        }
        bool upstreamPresent = Detect().Any(x => x.Family == ProductContract.UpstreamUpgradeCode);
        if (upstreamPresent) return true; // Full MSI repair first, native repair later.
        if (recovery.NativeStage == 0)
        {
            if (actual.CanInstall) return true;
            if (!actual.Repairable && !actual.CoreHealthy) throw new InvalidOperationException("Unknown partial upstream driver ownership.");
            recovery.NativeRemovalRequired = true; recovery.NativeStage = 1; store.SaveRecovery(recovery);
        }
        if (recovery.NativeStage == 1)
        {
            if (!NativeRecoveryStep(setup, recovery, Operation.Repair)) return false;
            recovery.NativeStage = 2; recovery.NativeAttempt = Guid.Empty; store.SaveRecovery(recovery);
        }
        if (recovery.NativeStage == 2)
        {
            if (!NativeRecoveryStep(setup, recovery, Operation.Uninstall)) return false;
            recovery.NativeStage = 3; recovery.NativeAttempt = Guid.Empty; store.SaveRecovery(recovery);
        }
        if (!Inspect().CanInstall) throw new InvalidOperationException("Verified removal did not leave an installable driver state.");
        return true;
    }
    public int RestoreLegacyPackage(SetupRecord setup, LegacyProduct product, bool registered, int attempt)
    {
        lease!.AssertHeld(); store.Verify(setup);
        VerifyLegacySurvivors(setup);
        LegacyRecovery.VerifyInventory(setup, Detect());
        bool upstream = product.Family == ProductContract.UpstreamUpgradeCode;
        string name = upstream ? "UpstreamRecovery.exe" : product.Version.StartsWith("99.", StringComparison.Ordinal) ? "Companion99Recovery.msi" : "Companion1Recovery.msi";
        string source = Path.Combine(store.Cache, name);
        string log = Path.Combine(store.Cache, "legacy-restore-" + attempt + ".log");
        ProtectedJournal.ValidateDirectory(store.Cache);
        if (File.Exists(log)) throw new InvalidDataException("Legacy restoration attempt log already exists.");
        if (upstream && !registered && !Inspect().CanInstall) throw new InvalidOperationException("Upstream fresh install requires a verified empty driver registration.");
        // Advanced Installer's documented // substitutes its embedded MSI path.
        // All executables, sources, operations and logs are fixed by trusted code.
        string executable = upstream ? source : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "msiexec.exe");
        string arguments = upstream ? "/exenoui " + (registered ? "/fa" : "/i") + " //" : (registered ? "/fa" : "/i") + " \"" + source + "\"";
        arguments += " /qn /norestart REBOOT=ReallySuppress /l*v \"" + log + "\"";
        using var process = Process.Start(new ProcessStartInfo(executable, arguments) { UseShellExecute = false, CreateNoWindow = true })!;
        if (!process.WaitForExit(120000)) throw new TimeoutException("Legacy restoration remains active; restart before recovery. No process was terminated.");
        if (upstream && (process.ExitCode == 0 || process.ExitCode == 3010)) QuiesceLegacyService(setup);
        return process.ExitCode;
    }
    public bool VerifyLegacyDriver(SetupRecord setup, LegacyRecoveryRecord recovery)
    {
        var actual = Inspect(); VerifyForeignFilters(setup.Before, actual);
        if (!setup.Legacy.Any(x => x.Family == ProductContract.UpstreamUpgradeCode)) return ProtectedJournal.StateBytes(actual).SequenceEqual(ProtectedJournal.StateBytes(setup.Before));
        if (recovery.NativeAttempt == Guid.Empty && actual.Healthy) return true;
        if (!actual.Repairable && !actual.CoreHealthy) throw new InvalidOperationException("Original MSI left unknown driver resources.");
        if (!NativeRecoveryStep(setup, recovery, Operation.Repair)) return false;
        return Inspect().Healthy;
    }
    public void RestoreLegacyBaseline(SetupRecord setup)
    {
        LegacyFileEvidence.RestoreShortcutPreferences(Detect(), setup.LegacyFiles!);
        var actual = Inspect(); VerifyForeignFilters(setup.Before, actual);
        if (setup.Before.Empty && ProtectedJournal.StateBytes(actual).SequenceEqual(ProtectedJournal.StateBytes(setup.Before))) return;
        if (!actual.Healthy || actual.Settings == null || setup.Before.Settings == null) throw new InvalidOperationException("Original driver is not ready for baseline restoration.");
        if (actual.Settings.Same(setup.Before.Settings)) return;
        var defaults = DriverTransaction.ExpectedAfterBinding(null);
        var preserved = DriverTransaction.ExpectedAfterBinding(setup.Before.Settings);
        if (!actual.Settings.Same(defaults) && !actual.Settings.Same(preserved)) throw new InvalidOperationException("Settings changed during legacy restoration.");
        Backend.RestoreSettings(actual.Settings, setup.Before.Settings);
    }
    public bool RestoreLegacyFilters(SetupRecord setup, LegacyRecoveryRecord recovery)
    {
        var actual = Inspect(); VerifyForeignFilters(setup.Before, actual);
        if (setup.Before.Empty && ProtectedJournal.StateBytes(actual).SequenceEqual(ProtectedJournal.StateBytes(setup.Before))) return true;
        if (!actual.Healthy || actual.Settings == null || !actual.Settings.Same(setup.Before.Settings!)) throw new InvalidOperationException("Baseline changed before class filter restoration.");
        return LegacyFilterRecovery.Apply(setup.Before.Filters, recovery, Boot, () => Inspect().Filters,
            (index, before, desired) => Backend.SetFilter(index, before, desired), () => store.SaveRecovery(recovery));
    }
    public void VerifyLegacyRestored(SetupRecord setup)
    {
        var products = Detect(); LegacyRecovery.VerifyInventory(setup, products);
        if (products.Count != setup.Legacy.Count) throw new InvalidOperationException("Original legacy product ownership is incomplete.");
        LegacyFileEvidence.VerifyRestored(products, setup.LegacyFiles!);
        var actual = Inspect();
        if (setup.Before.Empty ? !ProtectedJournal.StateBytes(actual).SequenceEqual(ProtectedJournal.StateBytes(setup.Before)) : !actual.Healthy || actual.Settings == null || !actual.Settings.Same(setup.Before.Settings!)) throw new InvalidOperationException("Original legacy baseline did not verify.");
        for (int i = 0; i < 3; i++) if (!actual.Filters[i].Same(setup.Before.Filters[i])) throw new InvalidOperationException("Original class filter ordering was not restored.");
    }
    public void CompleteLegacyRecovery(SetupRecord setup, LegacyRecoveryRecord recovery)
    {
        lease!.CompleteVerifiedLegacyRecovery(() =>
        {
            var durable = store.LoadOrCreateRecovery(Boot);
            if (durable.Phase != LegacyRecoveryPhase.Complete || durable.Transaction != setup.Id) throw new InvalidOperationException("Legacy restoration was not durably completed.");
            VerifyRecovery(setup); VerifyLegacyRestored(setup);
            if (setup.LegacyService != null) LegacyServices.Verify(setup.LegacyService);
        });
    }
}
