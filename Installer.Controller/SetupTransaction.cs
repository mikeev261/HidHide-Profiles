using System.Runtime.Serialization;
using HidHide.DriverSetup;
using HidHide.Installer;

namespace HidHide.Setup;

public enum SetupPhase { Prepared, RemovingLegacy, LegacyRemoved, MsiPending, MsiApplied, WaitingForReboot, Restoring, Complete, RecoveryRequired }
[DataContract]
public sealed class LegacyProduct
{
    [DataMember] public Guid Family { get; set; }
    [DataMember] public Guid Product { get; set; }
    [DataMember] public string Version { get; set; } = "";
    [DataMember] public bool RemovalIntent { get; set; }
    [DataMember] public bool Removed { get; set; }
    [DataMember] public string RemovalBoot { get; set; } = "";
}
[DataContract]
public sealed class SetupRecord
{
    [DataMember] public int Schema { get; set; } = 1;
    [DataMember] public Guid Id { get; set; }
    [DataMember] public string Sid { get; set; } = "";
    [DataMember] public string Version { get; set; } = "";
    [DataMember] public Operation Operation { get; set; }
    [DataMember] public SetupPhase Phase { get; set; }
    [DataMember] public SetupPhase ResumePhase { get; set; }
    [DataMember] public string Boot { get; set; } = "";
    [DataMember] public DriverState Before { get; set; } = new();
    [DataMember] public List<LegacyProduct> Legacy { get; set; } = new();
    [DataMember] public bool RestoreIntent { get; set; }
    [DataMember] public bool Restored { get; set; }
    [DataMember] public Guid PackageProduct { get; set; }
    [DataMember] public string PackageHash { get; set; } = "";
    [DataMember] public PriorUnifiedProduct? PriorUnified { get; set; }
    [DataMember] public Dictionary<string, string>? BeforeMsiFiles { get; set; }
    [DataMember] public Dictionary<string, string>? BeforeMsiBundles { get; set; }
    [DataMember] public bool MsiFailureReported { get; set; }
    [DataMember] public int MsiRetryCount { get; set; }
    [DataMember] public Guid BeforeMsiProduct { get; set; }
    [DataMember] public Dictionary<string, string>? LegacyFiles { get; set; }
    [DataMember] public LegacyServiceEvidence? LegacyService { get; set; }
}
[DataContract]
public sealed class PriorUnifiedProduct
{
    [DataMember] public Guid Product { get; set; }
    [DataMember] public string Version { get; set; } = "";
    [DataMember] public string PackageHash { get; set; } = "";
    [DataMember] public Guid SourceTransaction { get; set; }
}
public interface ISetupHost
{
    string Boot { get; }
    void Save(SetupRecord record);
    void VerifyCache(SetupRecord record);
    IReadOnlyList<InstalledProduct> Detect();
    DriverState Inspect();
    int RemoveLegacy(Guid product);
    void PrepareDriver(SetupRecord record);
    void VerifyMsiNotStarted(SetupRecord record);
    bool VerifyMsiRollbackAndArchive(SetupRecord record);
    bool DriverNeedsReboot(SetupRecord record);
    bool ResumeDriver(SetupRecord record);
    void RestoreBaseline(SetupRecord record);
    void VerifyFinal(SetupRecord record);
    void ClearMarker(SetupRecord record);
}

// This engine is shared by the elevated controller and fault-injection tests.
// Burn owns only the private MSI. Legacy removals happen before its transaction.
public sealed class SetupTransaction
{
    readonly ISetupHost host;
    readonly SetupRecord record;
    public SetupTransaction(ISetupHost host, SetupRecord record) { this.host = host; this.record = record; Validate(record); }
    public static void Validate(SetupRecord r)
    {
        if (r.Schema != 1 || r.Id == Guid.Empty || !Enum.IsDefined(typeof(SetupPhase), r.Phase) || !Enum.IsDefined(typeof(SetupPhase), r.ResumePhase) || !Enum.IsDefined(typeof(Operation), r.Operation) || r.Legacy == null || r.Legacy.Count > 2 || r.Before == null)
            throw new InvalidDataException("Unsupported setup journal.");
        _ = ProductContract.ParseVersion(r.Version); _ = new System.Security.Principal.SecurityIdentifier(r.Sid);
        if (r.MsiRetryCount < 0 || r.MsiRetryCount > 32) throw new InvalidDataException("Invalid retry count.");
        if (r.BeforeMsiFiles != null) MsiRollbackRecovery.ValidateFiles(r.BeforeMsiFiles);
        if (r.LegacyFiles != null) LegacyFileEvidence.Validate(r.LegacyFiles);
        if (r.LegacyService != null) LegacyServices.Validate(r.LegacyService);
        if (r.BeforeMsiBundles != null && (r.BeforeMsiBundles.Count > 8 || r.BeforeMsiBundles.Any(x => x.Key.Length > 64 || x.Value.Length > 128))) throw new InvalidDataException("Invalid bundle recovery evidence.");
        if (!string.IsNullOrEmpty(r.PackageHash) && (!ValidHash(r.PackageHash) || r.PackageProduct != ProductContract.UnifiedProductCode(System.Version.Parse(r.Version)))) throw new InvalidDataException("Invalid package identity in journal.");
        if (r.Operation == Operation.Upgrade)
        {
            var prior = r.PriorUnified ?? throw new InvalidDataException("Upgrade requires the prior unified recovery source.");
            var priorVersion = ProductContract.ParseVersion(prior.Version);
            if (prior.Product != ProductContract.UnifiedProductCode(priorVersion) || !UpgradeProtocol.OlderVersion(prior.Version, r.Version) || !ValidHash(prior.PackageHash) || prior.SourceTransaction == Guid.Empty || prior.SourceTransaction == r.Id)
                throw new InvalidDataException("Invalid prior unified identity or recovery source.");
        }
        else if (r.PriorUnified != null) throw new InvalidDataException("Unexpected prior unified recovery source.");
        if (r.Boot.Length == 0 || r.Boot.Length > 128 || r.Legacy.GroupBy(x => x.Family).Any(x => x.Count() > 1)) throw new InvalidDataException("Ambiguous setup journal.");
        foreach (var p in r.Legacy)
            if (ProductContract.Select(p.Family, p.Product, System.Version.Parse(p.Version), System.Version.Parse(r.Version)) is not (ProductContract.Operation.MigrateCompanion or ProductContract.Operation.MigrateUpstream) || p.Removed && !p.RemovalIntent || p.RemovalBoot != null && p.RemovalBoot.Length > 128)
                throw new InvalidDataException("Unrecognized migration record.");
    }
    static bool ValidHash(string hash) => hash != null && System.Text.RegularExpressions.Regex.IsMatch(hash, "\\A[A-F0-9]{64}\\z");
    void Phase(SetupPhase phase) { record.Phase = phase; host.Save(record); }
    void Reboot(SetupPhase resume) { record.Boot = host.Boot; record.ResumePhase = resume; Phase(SetupPhase.WaitingForReboot); }
    public SetupPhase Advance()
    {
        host.VerifyCache(record);
        if (record.Phase == SetupPhase.WaitingForReboot && record.ResumePhase == SetupPhase.RecoveryRequired)
        {
            if (!record.MsiFailureReported) throw new InvalidOperationException("Rollback reboot has no terminal MSI failure.");
            if (!BootIdentity.Changed(record.Boot, host.Boot)) return record.Phase;
            Phase(SetupPhase.RecoveryRequired);
        }
        if (record.Phase == SetupPhase.RecoveryRequired && record.MsiFailureReported)
        {
            if (record.MsiRetryCount >= 32) throw new InvalidOperationException("Automatic MSI retry limit reached; inspect recovery history.");
            if (!host.VerifyMsiRollbackAndArchive(record)) { Reboot(SetupPhase.RecoveryRequired); return record.Phase; }
            record.MsiRetryCount++; record.MsiFailureReported = false;
            Phase(SetupPhase.LegacyRemoved);
        }
        if (record.Phase == SetupPhase.RecoveryRequired || record.Phase == SetupPhase.Restoring && !record.Restored)
            throw new InvalidOperationException("Interrupted operation requires explicit verified recovery; no blind replay.");
        if (record.Phase == SetupPhase.Complete) { host.VerifyFinal(record); host.ClearMarker(record); return record.Phase; }
        if (record.Phase == SetupPhase.WaitingForReboot)
        {
            if (!BootIdentity.Changed(record.Boot, host.Boot)) return record.Phase;
            if (record.ResumePhase == SetupPhase.MsiApplied && !host.ResumeDriver(record)) { Reboot(SetupPhase.MsiApplied); return record.Phase; }
            Phase(record.ResumePhase);
        }
        if (record.Phase is SetupPhase.Prepared or SetupPhase.RemovingLegacy)
        {
            // Check the entire inventory before removing even the first product.
            var inventory = host.Detect();
            if (record.Operation == Operation.Upgrade && (inventory.Count != 1 || inventory[0].Product != record.PriorUnified!.Product || !SameVersion(inventory[0].Version, System.Version.Parse(record.PriorUnified.Version))))
                throw new InvalidOperationException("Prior unified product changed before MSI authorization.");
            if (inventory.Any(x => x.Operation is ProductContract.Operation.RejectUnknown or ProductContract.Operation.RejectDowngrade) ||
                inventory.Any(x => x.Family != ProductContract.MsiUpgradeCode && !record.Legacy.Any(y => y.Product == x.Product && y.Version == x.Version.ToString())))
                throw new InvalidOperationException("Installed product ownership changed; migration refused.");
            foreach (var legacy in record.Legacy.OrderBy(x => x.Family == ProductContract.CompanionUpgradeCode ? 0 : 1))
            {
                bool present = host.Detect().Any(x => x.Product == legacy.Product);
                if (legacy.Removed) { if (present) throw new InvalidOperationException("Removed legacy product reappeared."); continue; }
                if (legacy.RemovalIntent)
                {
                    // An interrupted uninstaller can remove registration before all
                    // cleanup completes. Absence alone never proves its completion.
                    Phase(SetupPhase.RecoveryRequired); throw new InvalidOperationException("Unknown legacy removal outcome.");
                }
                if (!present) throw new InvalidOperationException("Legacy ownership changed before removal.");
                if (!ProtectedJournal.StateBytes(host.Inspect()).SequenceEqual(ProtectedJournal.StateBytes(record.Before)))
                    throw new InvalidOperationException("Driver baseline changed before legacy removal; preserve the existing installation.");
                legacy.RemovalBoot = host.Boot; legacy.RemovalIntent = true; Phase(SetupPhase.RemovingLegacy);
                int code = host.RemoveLegacy(legacy.Product);
                if (code != 0 && code != 3010) { Phase(SetupPhase.RecoveryRequired); throw new InvalidOperationException("Legacy removal failed; recovery files retained."); }
                if (host.Detect().Any(x => x.Product == legacy.Product)) { Phase(SetupPhase.RecoveryRequired); throw new InvalidOperationException("Legacy product remains registered."); }
                legacy.Removed = true; host.Save(record);
                if (code == 3010) { Reboot(SetupPhase.RemovingLegacy); return record.Phase; }
            }
            Phase(SetupPhase.LegacyRemoved);
        }
        if (record.Phase == SetupPhase.LegacyRemoved)
        {
            host.PrepareDriver(record); Phase(SetupPhase.MsiPending); return record.Phase;
        }
        if (record.Phase == SetupPhase.MsiPending)
            throw new InvalidOperationException("MSI outcome is not recorded; inspect the existing transaction before recovery.");
        if (record.Phase == SetupPhase.MsiApplied)
        {
            if (host.DriverNeedsReboot(record)) { Reboot(SetupPhase.MsiApplied); return record.Phase; }
            record.RestoreIntent = true; Phase(SetupPhase.Restoring);
            host.RestoreBaseline(record); record.Restored = true; host.Save(record);
        }
        if (record.Phase == SetupPhase.Restoring && record.Restored)
        {
            host.VerifyFinal(record); Phase(SetupPhase.Complete); host.ClearMarker(record);
        }
        return record.Phase;
    }
    static bool SameVersion(System.Version first, System.Version second) => first.Major == second.Major && first.Minor == second.Minor && first.Build == second.Build;
    public SetupPhase CancelBeforeApply()
    {
        if (record.Phase != SetupPhase.MsiPending || record.Legacy.Any(x => !x.Removed)) throw new InvalidOperationException("No completed preparation with pending MSI authorization to cancel.");
        host.VerifyCache(record);
        host.VerifyMsiNotStarted(record);
        // Keep completed legacy-removal records and exclusion. Resume will verify
        // resources again without replaying those removals or assuming MSI rollback.
        Phase(SetupPhase.LegacyRemoved);
        return record.Phase;
    }
    public static void VerifyUntouchedDriver(SetupRecord setup, TransactionRecord driver, DriverState actual, string boot)
    {
        var operation = setup.Operation == Operation.Uninstall ? Operation.Uninstall : driver.Before.CanInstall ? Operation.Install : setup.Operation == Operation.Upgrade ? Operation.Upgrade : Operation.Repair;
        if (driver.Id != setup.Id || driver.InitiatingSid != setup.Sid || driver.PayloadIdentity != Payload.InfHash || driver.Operation != operation ||
            driver.Status != JournalStatus.Prepared || driver.Steps.Count != 0 || driver.Reboot || driver.Failure.Length != 0 || driver.BootId != boot ||
            !ProtectedJournal.StateBytes(actual).SequenceEqual(ProtectedJournal.StateBytes(driver.Before)))
            throw new InvalidOperationException("Native execution cannot be excluded; explicit recovery is required.");
    }
    public SetupPhase MsiCompleted(int exitCode)
    {
        if (record.Phase != SetupPhase.MsiPending) throw new InvalidOperationException("MSI was not authorized for this phase.");
        if (exitCode != 0 && exitCode != 3010) { record.MsiFailureReported = true; Phase(SetupPhase.RecoveryRequired); return record.Phase; }
        if (exitCode == 3010) { Reboot(SetupPhase.MsiApplied); return record.Phase; }
        Phase(SetupPhase.MsiApplied); return Advance();
    }
}
