using System.Runtime.Serialization;
using HidHide.DriverSetup;
using HidHide.Installer;

namespace HidHide.Setup;

public enum LegacyRecoveryPhase { AwaitingBoot, DriverPreparation, Packages, DriverVerification, Baseline, Complete, Filters, Services }
[DataContract]
public sealed class LegacyRecoveryRecord
{
    [DataMember] public int Schema { get; set; } = 1;
    [DataMember] public Guid Transaction { get; set; }
    [DataMember] public LegacyRecoveryPhase Phase { get; set; }
    [DataMember] public string Boot { get; set; } = "";
    [DataMember] public int PackageIndex { get; set; }
    [DataMember] public string PackageAttemptBoot { get; set; } = "";
    [DataMember] public int Attempts { get; set; }
    [DataMember] public Guid NativeAttempt { get; set; }
    [DataMember] public int NativeStage { get; set; }
    [DataMember] public bool NativeRemovalRequired { get; set; }
    [DataMember] public FilterState[]? FilterBefore { get; set; }
    [DataMember] public int FilterNext { get; set; }
    [DataMember] public bool FilterIntent { get; set; }
    [DataMember] public string FilterBoot { get; set; } = "";
    [DataMember] public bool ServiceQuiesceIntent { get; set; }
}
public interface ILegacyRecoveryHost
{
    string Boot { get; }
    void SaveRecovery(LegacyRecoveryRecord recovery);
    void VerifyRecovery(SetupRecord setup);
    void QuiesceLegacyService(SetupRecord setup);
    void RestoreLegacyService(SetupRecord setup);
    IReadOnlyList<InstalledProduct> Detect();
    DriverState Inspect();
    bool PrepareLegacyDriver(SetupRecord setup, LegacyRecoveryRecord recovery);
    int RestoreLegacyPackage(SetupRecord setup, LegacyProduct product, bool registered, int attempt);
    bool VerifyLegacyDriver(SetupRecord setup, LegacyRecoveryRecord recovery);
    void RestoreLegacyBaseline(SetupRecord setup);
    bool RestoreLegacyFilters(SetupRecord setup, LegacyRecoveryRecord recovery);
    void VerifyLegacyRestored(SetupRecord setup);
    void CompleteLegacyRecovery(SetupRecord setup, LegacyRecoveryRecord recovery);
}

// Explicit restoration abandons a migration only after original ownership and
// settings are verified. It never reclassifies an uncertain removal as success.
public sealed class LegacyRecovery
{
    readonly ILegacyRecoveryHost host;
    readonly SetupRecord setup;
    readonly LegacyRecoveryRecord record;
    public LegacyRecovery(ILegacyRecoveryHost host, SetupRecord setup, LegacyRecoveryRecord record)
    { this.host = host; this.setup = setup; this.record = record; Validate(record, setup.Id); }
    public static void Validate(LegacyRecoveryRecord r, Guid id)
    {
        if (r.Schema != 1 || r.Transaction != id || id == Guid.Empty || !Enum.IsDefined(typeof(LegacyRecoveryPhase), r.Phase) ||
            string.IsNullOrEmpty(r.Boot) || r.Boot.Length > 128 || r.PackageIndex < 0 || r.PackageIndex > 2 || r.Attempts < 0 || r.Attempts > 32 ||
            r.PackageAttemptBoot == null || r.PackageAttemptBoot.Length > 128 || r.NativeStage < 0 || r.NativeStage > 3 ||
            r.FilterNext < 0 || r.FilterNext > 3 || r.FilterBefore != null && r.FilterBefore.Length != 3 || r.FilterBoot == null || r.FilterBoot.Length > 128)
            throw new InvalidDataException("Invalid legacy recovery journal.");
        if (r.FilterBefore == null && (r.FilterNext != 0 || r.FilterIntent || r.FilterBoot.Length != 0) || r.FilterIntent && r.FilterBoot.Length == 0)
            throw new InvalidDataException("Missing filter restoration intent evidence.");
    }
    public static void VerifyInventory(SetupRecord setup, IReadOnlyList<InstalledProduct> actual)
    {
        if (actual.Count > setup.Legacy.Count || actual.GroupBy(x => x.Family).Any(x => x.Count() != 1) || actual.Any(x =>
            !setup.Legacy.Any(y => y.Product == x.Product && y.Family == x.Family && Version.Parse(y.Version) == x.Version) ||
            x.Operation is not (ProductContract.Operation.MigrateCompanion or ProductContract.Operation.MigrateUpstream)))
            throw new InvalidOperationException("Legacy recovery ownership changed; preserve all resources.");
    }
    void Save() => host.SaveRecovery(record);
    void Phase(LegacyRecoveryPhase phase) { record.Phase = phase; Save(); }
    public bool Advance()
    {
        if (setup.Legacy.Count == 0 || setup.LegacyFiles == null || setup.BeforeMsiFiles != null || setup.MsiFailureReported ||
            setup.Phase is SetupPhase.MsiPending or SetupPhase.MsiApplied or SetupPhase.Restoring or SetupPhase.Complete)
            throw new InvalidOperationException("This journal has no proven pre-MSI legacy restoration path.");
        host.VerifyRecovery(setup);
        VerifyInventory(setup, host.Detect());
        if (record.Phase == LegacyRecoveryPhase.Complete)
        { host.VerifyLegacyRestored(setup); host.CompleteLegacyRecovery(setup, record); return true; }
        if (record.Phase == LegacyRecoveryPhase.AwaitingBoot)
        {
            // Even old journals lacking per-removal boot proof get a new durable
            // boot boundary. No original worker can survive this boundary.
            if (!BootIdentity.Changed(record.Boot, host.Boot)) return false;
            Phase(LegacyRecoveryPhase.DriverPreparation);
        }
        if (record.Phase == LegacyRecoveryPhase.Packages && (!BootIdentity.Changed(record.Boot, host.Boot) || record.PackageAttemptBoot.Length != 0 && !BootIdentity.Changed(record.PackageAttemptBoot, host.Boot))) return false;
        if (record.Phase != LegacyRecoveryPhase.Services)
        {
            record.ServiceQuiesceIntent = true; Save();
            host.QuiesceLegacyService(setup);
        }
        if (record.Phase == LegacyRecoveryPhase.DriverPreparation)
        {
            if (!host.PrepareLegacyDriver(setup, record)) return false;
            Phase(LegacyRecoveryPhase.Packages);
        }
        var products = setup.Legacy.OrderBy(x => x.Family == ProductContract.UpstreamUpgradeCode ? 0 : 1).ToArray();
        if (record.Phase == LegacyRecoveryPhase.Packages)
        {
            if (!BootIdentity.Changed(record.Boot, host.Boot)) return false;
            while (record.PackageIndex < products.Length)
            {
                if (record.PackageAttemptBoot.Length != 0 && !BootIdentity.Changed(record.PackageAttemptBoot, host.Boot)) return false;
                VerifyInventory(setup, host.Detect());
                var product = products[record.PackageIndex];
                bool registered = host.Detect().Any(x => x.Product == product.Product);
                if (record.Attempts >= 32) throw new InvalidOperationException("Legacy restoration attempt limit reached; retained history requires inspection.");
                record.Attempts++; record.PackageAttemptBoot = host.Boot; Save();
                int result = host.RestoreLegacyPackage(setup, product, registered, record.Attempts);
                if (result != 0 && result != 3010) throw new InvalidOperationException("Legacy installer did not complete; recovery and exclusion are retained.");
                VerifyInventory(setup, host.Detect());
                if (!host.Detect().Any(x => x.Product == product.Product)) throw new InvalidOperationException("Original product registration was not restored.");
                record.PackageIndex++; record.PackageAttemptBoot = "";
                // Reboot after every package execution, including exit zero:
                // custom-action children and delayed driver work must be gone.
                record.Boot = host.Boot; Save(); return false;
            }
            if (!BootIdentity.Changed(record.Boot, host.Boot)) return false;
            Phase(LegacyRecoveryPhase.DriverVerification);
        }
        if (record.Phase == LegacyRecoveryPhase.DriverVerification)
        {
            if (!host.VerifyLegacyDriver(setup, record)) return false;
            Phase(LegacyRecoveryPhase.Baseline);
        }
        if (record.Phase == LegacyRecoveryPhase.Baseline)
        {
            host.RestoreLegacyBaseline(setup);
            Phase(LegacyRecoveryPhase.Filters);
        }
        if (record.Phase == LegacyRecoveryPhase.Filters)
        {
            if (!host.RestoreLegacyFilters(setup, record)) return false;
            host.VerifyLegacyRestored(setup);
            Phase(LegacyRecoveryPhase.Services);
        }
        if (record.Phase == LegacyRecoveryPhase.Services)
        {
            host.VerifyLegacyRestored(setup);
            host.RestoreLegacyService(setup);
            host.VerifyLegacyRestored(setup);
            Phase(LegacyRecoveryPhase.Complete);
            host.CompleteLegacyRecovery(setup, record);
        }
        return true;
    }
}
