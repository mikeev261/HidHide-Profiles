using HidHide.Setup;
using HidHide.DriverSetup;
using HidHide.Installer;

internal static class LegacyRecoveryTests
{
    static int count;
    static void Check(bool condition, string message) { if (!condition) throw new Exception(message); count++; }
    static void Reject(Action action, string message) { try { action(); } catch (InvalidOperationException) { count++; return; } throw new Exception(message); }
    static SetupRecord Setup() => new() { Id = Guid.NewGuid(), Version = "2.1.0.0", Phase = SetupPhase.RecoveryRequired, LegacyFiles = new(), Legacy = new() {
        new LegacyProduct { Family = ProductContract.CompanionUpgradeCode, Product = Guid.Parse("B7E9D4A2-6F31-4E88-9C0D-1A2B4C4D5E70"), Version = "1.0.0.0", RemovalIntent = true },
        new LegacyProduct { Family = ProductContract.UpstreamUpgradeCode, Product = ProductContract.UpstreamProductCode, Version = "1.5.230", RemovalIntent = true }
    }};
    public static void Run()
    {
        var setup = Setup(); var record = new LegacyRecoveryRecord { Transaction = setup.Id, Boot = "winboot-v1:1" }; var host = new Fake(setup, record); var recovery = new LegacyRecovery(host, setup, record);
        Check(!recovery.Advance() && host.Calls.Count == 0, "same boot cannot restore or declare legacy worker dead");
        host.Boot = "winboot-v1:2"; Check(!recovery.Advance() && host.Calls.SequenceEqual(new[] { "prepare", "upstream" }), "restores upstream before companion and requests reboot");
        Check(!recovery.Advance() && host.Calls.Count == 2, "same boot after package cannot continue");
        host.Boot = "winboot-v1:3"; Check(!recovery.Advance() && host.Calls.Last() == "companion", "next boot restores companion");
        host.Boot = "winboot-v1:4"; Check(recovery.Advance() && host.Calls.Skip(3).SequenceEqual(new[] { "driver", "baseline", "verify", "verify", "verify", "clear" }), "complete only after driver baseline and file proof");
        Check(setup.Legacy.All(x => x.RemovalIntent && !x.Removed) && setup.Phase == SetupPhase.RecoveryRequired, "original uncertain removal journal stays unchanged");
        Check(recovery.Advance() && host.Calls.Last() == "clear", "completed recovery re-verifies before marker cleanup");
        foreach (var phase in new[] { SetupPhase.MsiPending, SetupPhase.MsiApplied, SetupPhase.Restoring, SetupPhase.Complete })
        {
            setup = Setup(); setup.Phase = phase; record = new() { Transaction = setup.Id, Boot = "winboot-v1:1" }; host = new(setup, record) { Boot = "winboot-v1:2" };
            Reject(() => new LegacyRecovery(host, setup, record).Advance(), "MSI-owned phase accepted for legacy restore");
            Check(host.Calls.Count == 0, "MSI-owned phase produced no legacy mutations");
        }
        setup = Setup(); setup.LegacyFiles = null; record = new() { Transaction = setup.Id, Boot = "winboot-v1:1" }; host = new(setup, record) { Boot = "winboot-v1:2" };
        Reject(() => new LegacyRecovery(host, setup, record).Advance(), "old journal without evidence accepted");
        setup = Setup(); record = new() { Transaction = setup.Id, Boot = "winboot-v1:1" }; host = new(setup, record) { Boot = "winboot-v1:2", FailPackage = true }; recovery = new(host, setup, record);
        Reject(() => recovery.Advance(), "failed package accepted");
        Check(record.PackageAttemptBoot == "winboot-v1:2" && record.Attempts == 1 && record.PackageIndex == 0, "failed attempt remains durably unknown");
        Check(!recovery.Advance() && host.Calls.Count == 2, "unknown package is never retried on same boot");
        host.Boot = "winboot-v1:3"; host.FailPackage = false; Check(!recovery.Advance() && record.Attempts == 2, "explicit new-boot retry can repair exact registered ownership");
        setup = Setup(); record = new() { Transaction = setup.Id, Boot = "winboot-v1:1" }; host = new(setup, record) { Boot = "winboot-v1:2", Foreign = true };
        Reject(() => new LegacyRecovery(host, setup, record).Advance(), "foreign owner accepted");
        Check(host.Calls.Count == 0, "foreign product prevented any action");
        setup = Setup(); record = new() { Transaction = setup.Id, Boot = "winboot-v1:1" }; host = new(setup, record) { Boot = "winboot-v1:2", FailSave = true };
        Reject(() => new LegacyRecovery(host, setup, record).Advance(), "failed durable intent accepted");
        Check(host.Calls.Count == 0, "journal failure prevented mutation");
        Console.WriteLine(count + " legacy recovery engine checks passed; no machine mutations.");
    }
    sealed class Fake : ILegacyRecoveryHost
    {
        readonly SetupRecord setup; readonly LegacyRecoveryRecord record;
        readonly List<InstalledProduct> products = new();
        public readonly List<string> Calls = new();
        public string Boot { get; set; } = "winboot-v1:1";
        public bool FailPackage, Foreign, FailSave;
        public Fake(SetupRecord setup, LegacyRecoveryRecord record) { this.setup = setup; this.record = record; }
        public void SaveRecovery(LegacyRecoveryRecord recovery) { if (FailSave) throw new InvalidOperationException("write failure"); }
        public void VerifyRecovery(SetupRecord value) { }
        public void QuiesceLegacyService(SetupRecord value) { }
        public void RestoreLegacyService(SetupRecord value) { }
        public IReadOnlyList<InstalledProduct> Detect() => Foreign ? new[] { new InstalledProduct { Product = Guid.NewGuid() } } : products;
        public DriverState Inspect() => new();
        public bool PrepareLegacyDriver(SetupRecord value, LegacyRecoveryRecord recovery) { Calls.Add("prepare"); return true; }
        public int RestoreLegacyPackage(SetupRecord value, LegacyProduct product, bool registered, int attempt)
        {
            Check(record.PackageAttemptBoot == Boot && record.Attempts == attempt, "package intent exists before operation");
            Calls.Add(product.Family == ProductContract.UpstreamUpgradeCode ? "upstream" : "companion");
            if (FailPackage) return 1603;
            products.Add(new InstalledProduct { Family = product.Family, Product = product.Product, Version = Version.Parse(product.Version), Operation = product.Family == ProductContract.UpstreamUpgradeCode ? ProductContract.Operation.MigrateUpstream : ProductContract.Operation.MigrateCompanion }); return 0;
        }
        public bool VerifyLegacyDriver(SetupRecord value, LegacyRecoveryRecord recovery) { Calls.Add("driver"); return true; }
        public void RestoreLegacyBaseline(SetupRecord value) => Calls.Add("baseline");
        public bool RestoreLegacyFilters(SetupRecord value, LegacyRecoveryRecord recovery) => true;
        public void VerifyLegacyRestored(SetupRecord value) => Calls.Add("verify");
        public void CompleteLegacyRecovery(SetupRecord value, LegacyRecoveryRecord recovery) { Check(recovery.Phase == LegacyRecoveryPhase.Complete, "clear only after durable completion"); Calls.Add("clear"); }
    }
}
