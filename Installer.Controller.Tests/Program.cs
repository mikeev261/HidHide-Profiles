using HidHide.Setup;
using HidHide.DriverSetup;
using HidHide.Installer;

static class Tests
{
    static int checks;
    const string Sid = "S-1-5-21-1-2-3-1000";
    static void Check(bool value, string name) { if (!value) throw new Exception(name); checks++; }
    static void Reject(Action action, string name) { try { action(); } catch (Exception) { checks++; return; } throw new Exception("Accepted " + name); }
    static SetupRecord Record() => new() { Id = Guid.NewGuid(), Sid = Sid, Version = "2.0.0.0", Boot = "winboot-v1:1", Before = new DriverState() };
    static LegacyProduct Upstream() => new() { Product = ProductContract.UpstreamProductCode, Family = ProductContract.UpstreamUpgradeCode, Version = "1.5.230" };
    static LegacyProduct Companion() => new() { Product = new Guid("B7E9D4A2-6F31-4E88-9C0D-1A2B6C4D5E70"), Family = ProductContract.CompanionUpgradeCode, Version = "99.0.0.0" };
    static string Ready(bool driverPresent = true, bool baselineAvailable = true)
    {
        var data = new List<byte>(); void N(uint n) => data.AddRange(BitConverter.GetBytes(n));
        void S(string s) { N((uint)s.Length); foreach (char c in s) N(c); }
        N(2); S(Sid); N(driverPresent ? 1u : 0u); N(baselineAvailable ? 1u : 0u); N(1); N(0); N(1); S("synthetic-device"); N(1); S("C:\\feeder.exe"); N(1); S("C:\\game.exe"); N(1); S("synthetic-device");
        return "READY " + BitConverter.ToString(data.ToArray()).Replace("-", "");
    }
    public static int Main()
    {
        SetupPipeSecurityTests.Run();
        PublicDiagnosticTests.Run();
        LegacyFileEvidenceTests.Run();
        LegacyServiceTests.Run();
        LegacyRecoveryTests.Run();
        LegacyFilterRecoveryTests.Run();
        {
            var r = Record(); var h = new Fake(r); var tx = new SetupTransaction(h, r);
            Reject(() => tx.CancelBeforeApply(), "cancel before MSI authorization exists");
            tx.Advance();
            Check(tx.CancelBeforeApply() == SetupPhase.LegacyRemoved && h.Events.Contains("not-started") && !h.Events.Contains("clear"), "verified nonexecution cancellation is retryable and keeps exclusion");
            Check(tx.Advance() == SetupPhase.MsiPending && h.Removed.Count == 0, "cancelled fresh setup can authorize MSI again");
            h.Failure = "not-started";
            Reject(() => tx.CancelBeforeApply(), "unproven native nonexecution rejected");
            Check(r.Phase == SetupPhase.MsiPending, "failed cancellation proof cannot rewrite phase");
        }
        {
            var r = Record(); r.Legacy.Add(Companion()); r.Legacy.Add(Upstream());
            var h = new Fake(r); var tx = new SetupTransaction(h, r); tx.Advance();
            var original = ProtectedJournal.StateBytes(r.Before);
            tx.CancelBeforeApply();
            Check(r.Legacy.All(x => x.Removed && x.RemovalIntent) && h.Removed.Count == 2 && ProtectedJournal.StateBytes(r.Before).SequenceEqual(original), "cancel preserves completed legacy removals and original baseline");
            tx.Advance(); Check(h.Removed.Count == 2, "resume after cancellation never repeats legacy uninstall");
        }
        {
            var r = Record();
            var driver = new TransactionRecord { Id = r.Id, InitiatingSid = r.Sid, Before = r.Before, BootId = "winboot-v1:1" };
            SetupTransaction.VerifyUntouchedDriver(r, driver, r.Before, "winboot-v1:1");
            Check(true, "prepared empty native journal proves nonexecution with matching observed state");
            foreach (JournalStatus state in Enum.GetValues(typeof(JournalStatus)))
            {
                if (state == JournalStatus.Prepared) continue;
                driver.Status = state;
                Reject(() => SetupTransaction.VerifyUntouchedDriver(r, driver, r.Before, "winboot-v1:1"), "cancel rejects native state " + state);
            }
            driver.Status = JournalStatus.Prepared; driver.Steps.Add(new Step());
            Reject(() => SetupTransaction.VerifyUntouchedDriver(r, driver, r.Before, "winboot-v1:1"), "intent-only native step prevents safe cancel");
            driver.Steps.Clear();
            Reject(() => SetupTransaction.VerifyUntouchedDriver(r, driver, new DriverState { ServiceExists = true }, "winboot-v1:1"), "changed native resources prevent safe cancel");
            Reject(() => SetupTransaction.VerifyUntouchedDriver(r, driver, r.Before, "winboot-v1:2"), "boot change prevents unobserved safe cancel");
            driver.Id = Guid.NewGuid();
            Reject(() => SetupTransaction.VerifyUntouchedDriver(r, driver, r.Before, "winboot-v1:1"), "unrelated native journal cannot authorize retry");
        }
        Check(UpgradeProtocol.CompatibleRelatedBundle(UpgradeProtocol.Tag, true, false), "cached compatible bundle");
        Check(!UpgradeProtocol.CompatibleRelatedBundle("", true, false), "preview bundle rejected");
        Check(!UpgradeProtocol.CompatibleRelatedBundle(UpgradeProtocol.Tag, false, false), "per-user bundle rejected");
        Check(!UpgradeProtocol.CompatibleRelatedBundle(UpgradeProtocol.Tag, true, true), "uncached bundle rejected");
        Check(UpgradeProtocol.CanFinalizeRegistration(1, true), "only empty owned package can finalize");
        Check(!UpgradeProtocol.CanFinalizeRegistration(1, false) && !UpgradeProtocol.CanFinalizeRegistration(0, true) && !UpgradeProtocol.CanFinalizeRegistration(2, true), "registration cleanup rejects package ownership ambiguity");
        Check(UpgradeProtocol.OlderVersion("2.1.0.0", "2.2.0.0"), "older compatible bundle allowed");
        Check(!UpgradeProtocol.OlderVersion("2.1.0.0", "2.1.0") && !UpgradeProtocol.OlderVersion("2.1.0.1", "2.1.0.0"), "same MSI version rebuilt bundle rejected");
        Check(!UpgradeProtocol.OlderVersion("2.2.0.0", "2.1.0.0") && !UpgradeProtocol.OlderVersion("bad", "2.1.0.0"), "newer or malformed related bundle rejected");
        {
            var r = Record();
            r.Operation = Operation.Uninstall;
            r.PackageProduct = ProductContract.UnifiedProductCode(Version.Parse(r.Version));
            r.BeforeMsiProduct = r.PackageProduct;
            r.PackageHash = new string('A', 64);
            foreach (SetupPhase phase in new[] { SetupPhase.MsiApplied, SetupPhase.Restoring, SetupPhase.Complete })
            {
                r.Phase = phase;
                Check(ControllerStore.CanFinalizeUninstallWithJournalPackage(r), "later setup can finish journal-bound post-MSI uninstall phase " + phase);
            }
            r.Phase = SetupPhase.WaitingForReboot; r.ResumePhase = SetupPhase.MsiApplied;
            Check(ControllerStore.CanFinalizeUninstallWithJournalPackage(r), "later setup can finish journal-bound uninstall after required reboot");
            foreach (SetupPhase phase in new[] { SetupPhase.Prepared, SetupPhase.RemovingLegacy, SetupPhase.LegacyRemoved, SetupPhase.MsiPending, SetupPhase.RecoveryRequired })
            {
                r.Phase = phase;
                Check(!ControllerStore.CanFinalizeUninstallWithJournalPackage(r), "package mismatch rejected before completed MSI removal: " + phase);
            }
            r.Phase = SetupPhase.WaitingForReboot; r.ResumePhase = SetupPhase.RecoveryRequired;
            Check(!ControllerStore.CanFinalizeUninstallWithJournalPackage(r), "package mismatch rejected for ambiguous rollback reboot");
            r.Phase = SetupPhase.MsiApplied; r.ResumePhase = SetupPhase.MsiApplied;
            r.Operation = Operation.Install;
            Check(!ControllerStore.CanFinalizeUninstallWithJournalPackage(r), "package mismatch rejected for install recovery");
            r.Operation = Operation.Uninstall; r.Legacy.Add(Upstream());
            Check(!ControllerStore.CanFinalizeUninstallWithJournalPackage(r), "package mismatch rejected for legacy migration recovery");
            r.Legacy.Clear(); r.MsiFailureReported = true;
            Check(!ControllerStore.CanFinalizeUninstallWithJournalPackage(r), "package mismatch rejected after MSI failure");
            r.MsiFailureReported = false; r.BeforeMsiProduct = Guid.NewGuid();
            Check(!ControllerStore.CanFinalizeUninstallWithJournalPackage(r), "package mismatch rejected without exact removed product evidence");
            r.BeforeMsiProduct = r.PackageProduct; r.PackageHash = "bad";
            Check(!ControllerStore.CanFinalizeUninstallWithJournalPackage(r), "package mismatch rejected without valid journal package digest");
        }
        string ready = Ready(); var parsed = ReadySnapshot.Parse(ready, Sid);
        var storedReady = ReadySnapshot.Parse(Ready(false, true), Sid);
        Check(!storedReady.DriverPresent && storedReady.BaselineAvailable && storedReady.Active, "ordinary-user stored baseline distinct from live driver");
        Reject(() => ReadySnapshot.Parse(Ready(true, false), Sid), "live driver without confirmed baseline");
        Reject(() => ReadySnapshot.Parse(Ready(false, false), Sid), "unconfirmed stored settings rejected");
        Check(parsed.Active && !parsed.Inverse && parsed.DriverPresent && parsed.Profiles.Count == 1 && parsed.Blacklist.Single() == "synthetic-device", "real READY format");
        Reject(() => ReadySnapshot.Parse(ready, "S-1-5-18"), "cross-user snapshot");
        Reject(() => ReadySnapshot.Parse(ready + "00000000", Sid), "trailing snapshot");
        Reject(() => ReadySnapshot.Parse("READY 03000000" + ready.Substring(14), Sid), "unknown version");
        bool allTruncated = true;
        for (int i = 6; i < ready.Length; i += 2) { try { ReadySnapshot.Parse(ready.Substring(0, i), Sid); allTruncated = false; } catch (InvalidDataException) { } }
        Check(allTruncated, "every truncated READY rejected");
        Reject(() => ReadySnapshot.Parse(ready.Substring(0, ready.Length - 1) + "Z", Sid), "invalid hex");
        Check(SettingsCodec.Decode(ReadySnapshot.MultiString(parsed.Blacklist)).SequenceEqual(parsed.Blacklist), "baseline codec compatibility");
        Check(SettingsCodec.Decode(ReadySnapshot.MultiString(Array.Empty<string>())).Length == 0, "empty baseline codec compatibility");
        Check(SettingsCodec.Decode(ReadySnapshot.MultiString(new[] { "x\uD800" })).Single() == "x\uD800", "UTF16 code units preserved exactly");
        using (var bytes = new MemoryStream()) { var wire = new SessionWire(bytes); wire.Write("handoff"); bytes.Position = 0; Check(wire.Read() == "handoff", "framed controller pipe"); }
        foreach (string code in new[] { "error:restart-required", "error:controller-failed" })
        {
            using var bytes = new MemoryStream(); var wire = new SessionWire(bytes); wire.Write(code); bytes.Position = 0;
            string message = "";
            try { wire.Read(); } catch (InvalidOperationException error) { message = error.Message; }
            Check(message.Length > 0 && (code != "error:restart-required" || message.Contains("Restart Windows")), "controller error survives pipe handoff");
        }
        using (var bytes = new MemoryStream(BitConverter.GetBytes(-1))) Reject(() => new SessionWire(bytes).Read(), "negative frame");
        using (var bytes = new MemoryStream())
        {
            var wire = new SessionWire(bytes); wire.Write("error:controller:cache:UnauthorizedAccessException:80070005"); bytes.Position = 0;
            string message = ""; try { wire.Read(); } catch (InvalidOperationException error) { message = error.Message; }
            Check(message.Contains("cache") && message.Contains("80070005"), "safe phase and failure code reach bootstrapper");
        }
        using (var bytes = new MemoryStream(BitConverter.GetBytes(ReadySnapshot.Limit * 2 + 7))) Reject(() => new SessionWire(bytes).Read(), "oversized frame");
        using (var bytes = new MemoryStream(new byte[] { 2, 0, 0, 0, 65 })) Reject(() => new SessionWire(bytes).Read(), "truncated frame");
        var installed = new[] { new InstalledProduct { Family = ProductContract.MsiUpgradeCode, Product = Guid.NewGuid(), Version = new Version(2,0,0), Operation = ProductContract.Operation.Repair } };
        Check(SetupPolicy.Select(Operation.Install, installed, new DriverState()) == Operation.Repair, "default install repairs existing current MSI");
        Check(SetupPolicy.Select(Operation.Uninstall, installed, new DriverState()) == Operation.Uninstall, "uninstall remains uninstall");
        Reject(() => SetupPolicy.Select(Operation.Uninstall, Array.Empty<InstalledProduct>(), new DriverState()), "unowned uninstall");
        Reject(() => SetupPolicy.Select(Operation.Install, Array.Empty<InstalledProduct>(), new DriverState { ServiceExists = true }), "orphaned resources");
        installed[0].Operation = ProductContract.Operation.Upgrade;
        Reject(() => SetupPolicy.Select(Operation.Install, installed, new DriverState()), "earlier unified upgrade gate");
        installed[0].Version = new Version(2, 1, 0);
        var healthy = new DriverState { Nodes = new[] { new Node { Inf = "oem1.inf", Service = "HidHide" } }, Packages = new[] { "oem1.inf" }, ServiceExists = true, ControlAvailable = true, BinaryHash = Payload.SysHash,
            Filters = Enumerable.Range(0, 3).Select(_ => new FilterState { Exists = true, Entries = new[] { "HidHide" } }).ToArray() };
        Check(SetupPolicy.Select(Operation.Install, installed, healthy) == Operation.Upgrade, "compatible unified selects upgrade");
        Reject(() => SetupPolicy.Select(Operation.Install, installed, new DriverState()), "upgrade retains only healthy driver");
        Reject(() => SetupPolicy.Select(Operation.Uninstall, installed, healthy), "new setup cannot uninstall older product");
        {
            var r = Record(); r.Version = "2.2.0.0"; r.Operation = Operation.Upgrade;
            Reject(() => SetupTransaction.Validate(r), "upgrade without prior recovery package");
            r.PriorUnified = new PriorUnifiedProduct { Product = ProductContract.UnifiedProductCode(new Version(2, 1, 0)), Version = "2.1.0.0", SourceTransaction = Guid.NewGuid(), PackageHash = new string('A', 64) };
            SetupTransaction.Validate(r);
            var h = new Fake(r); var tx = new SetupTransaction(h, r);
            Check(tx.Advance() == SetupPhase.MsiPending && h.Removed.Count == 0, "upgrade authorizes MSI without legacy removal");
            Check(tx.MsiCompleted(1603) == SetupPhase.RecoveryRequired && !h.Events.Contains("clear"), "failed upgrade retains recovery metadata and exclusion");
            r.PriorUnified.Product = Guid.NewGuid();
            Reject(() => SetupTransaction.Validate(r), "prior product identity changed");
            r.PriorUnified.Product = ProductContract.UnifiedProductCode(new Version(2, 1, 0)); r.PriorUnified.PackageHash = "bad";
            Reject(() => SetupTransaction.Validate(r), "prior cache digest corrupt");
        }
        foreach (Operation op in new[] { Operation.Install, Operation.Repair, Operation.Uninstall })
        {
            var r = Record(); r.Operation = op; var h = new Fake(r); var tx = new SetupTransaction(h, r);
            Check(tx.Advance() == SetupPhase.MsiPending, op + " preflight authorizes MSI");
            Check(h.Events.IndexOf("cache") < h.Events.IndexOf("prepare"), "cache before authorization");
            Check(tx.MsiCompleted(0) == SetupPhase.Complete && h.Events.Last() == "clear", op + " verified completion");
            Check(h.Events.IndexOf("restore") < h.Events.IndexOf("verify"), "restore before verification");
        }
        {
            var r = Record(); r.Legacy.Add(Upstream()); r.Legacy.Add(Companion()); var h = new Fake(r); var tx = new SetupTransaction(h, r);
            Check(tx.Advance() == SetupPhase.MsiPending, "both legacy products migrate");
            Check(h.Removed.SequenceEqual(new[] { Companion().Product, Upstream().Product }), "companion removed before upstream");
            Check(h.Events.Last() == "save:MsiPending", "MSI not executed inside legacy removal");
        }
        foreach (string failure in new[] { "cache", "save:RemovingLegacy", "remove", "prepare", "restore", "verify" })
        {
            var r = Record(); r.Legacy.Add(Upstream()); var h = new Fake(r) { Failure = failure }; var tx = new SetupTransaction(h, r);
            Reject(() => { tx.Advance(); tx.MsiCompleted(0); }, "failure at " + failure);
            Check(!h.Events.Contains("clear"), "failure retains exclusion: " + failure);
            if (failure == "cache" || failure == "save:RemovingLegacy") Check(h.Removed.Count == 0, "failure before removal: " + failure);
        }
        {
            var r = Record(); r.Legacy.Add(Upstream()); var h = new Fake(r) { RemoveCode = 1603 }; var tx = new SetupTransaction(h, r);
            Reject(() => tx.Advance(), "legacy exit code"); Check(r.Phase == SetupPhase.RecoveryRequired && !h.Events.Contains("prepare"), "failed legacy never advances MSI");
        }
        {
            var r = Record(); r.Legacy.Add(Upstream()); var h = new Fake(r) { Changed = true };
            Reject(() => new SetupTransaction(h, r).Advance(), "changed native baseline"); Check(h.Removed.Count == 0, "changed baseline preserves legacy");
        }
        {
            var r = Record(); r.Legacy.Add(Upstream()); var h = new Fake(r) { UnknownProduct = true };
            Reject(() => new SetupTransaction(h, r).Advance(), "unknown product"); Check(h.Removed.Count == 0, "unknown identity preserves all products");
        }
        {
            var r = Record(); r.Legacy.Add(Upstream()); var h = new Fake(r) { RemoveCode = 3010 }; var tx = new SetupTransaction(h, r);
            Check(tx.Advance() == SetupPhase.WaitingForReboot, "legacy reboot stops MSI");
            Check(tx.Advance() == SetupPhase.WaitingForReboot && h.Removed.Count == 1, "same boot does not advance");
            h.Boot = "winboot-v1:2"; Check(tx.Advance() == SetupPhase.MsiPending && h.Removed.Count == 1, "changed boot does not replay removal");
        }
        {
            var r = Record(); var h = new Fake(r); var tx = new SetupTransaction(h, r); tx.Advance();
            Check(tx.MsiCompleted(3010) == SetupPhase.WaitingForReboot, "MSI reboot suspends finalization");
            h.Boot = "winboot-v1:2"; h.MoreReboot = true; Check(tx.Advance() == SetupPhase.WaitingForReboot, "second driver reboot supported");
            h.Boot = "winboot-v1:3"; h.MoreReboot = false; Check(tx.Advance() == SetupPhase.Complete, "multiple reboot finalization");
        }
        {
            var r = Record(); var h = new Fake(r); var tx = new SetupTransaction(h, r); tx.Advance();
            Check(tx.MsiCompleted(1603) == SetupPhase.RecoveryRequired && !h.Events.Contains("clear"), "MSI failure retains journal");
        }
        foreach (SetupPhase phase in new[] { SetupPhase.MsiPending, SetupPhase.Restoring, SetupPhase.RecoveryRequired })
        {
            var r = Record(); r.Phase = phase; var h = new Fake(r); Reject(() => new SetupTransaction(h, r).Advance(), "unknown outcome " + phase); Check(!h.Events.Contains("clear"), "no guessed recovery " + phase);
        }
        {
            var r = Record(); var h = new Fake(r); var tx = new SetupTransaction(h, r); tx.Advance();
            tx.MsiCompleted(1603);
            Check(r.MsiFailureReported, "terminal MSI failure recorded durably");
            Check(tx.Advance() == SetupPhase.MsiPending && r.MsiRetryCount == 1 && !r.MsiFailureReported, "verified rollback permits a new Apply, not success");
            Check(h.Events.Contains("rollback-proof") && !h.Events.Contains("clear"), "retry verifies archive and retains exclusion");
        }
        {
            var r = Record(); var h = new Fake(r); var tx = new SetupTransaction(h, r); tx.Advance(); tx.MsiCompleted(1603);
            h.RollbackReboot = true;
            Check(tx.Advance() == SetupPhase.WaitingForReboot && r.ResumePhase == SetupPhase.RecoveryRequired, "failed Apply rollback has distinct reboot continuation");
            Check(r.MsiFailureReported && r.MsiRetryCount == 0 && !h.Events.Contains("clear"), "rollback restart preserves terminal failure and baseline without retrying MSI");
            int proofs = h.Events.Count(x => x == "rollback-proof");
            Check(tx.Advance() == SetupPhase.WaitingForReboot && proofs == h.Events.Count(x => x == "rollback-proof"), "same boot never resumes inverse native calls");
            h.Boot = "winboot-v1:2";
            Check(tx.Advance() == SetupPhase.WaitingForReboot && r.MsiFailureReported && r.MsiRetryCount == 0, "multiple native inverse reboots do not authorize Apply");
            h.Boot = "winboot-v1:3"; h.RollbackReboot = false;
            Check(tx.Advance() == SetupPhase.MsiPending && r.MsiRetryCount == 1 && !r.MsiFailureReported, "complete rollback after reboot authorizes only a new MSI attempt");
            Check(!h.Events.Contains("clear"), "rollback recovery does not clear maintenance before new Apply finishes");
        }
        {
            var r = Record(); r.Operation = Operation.Repair;
            r.Before = new DriverState { Nodes = new[] { new Node { Id = @"ROOT\SYSTEM\0001", Inf = "oem1.inf", Service = "HidHide" } }, Packages = new[] { "oem1.inf" }, ServiceExists = true, ControlAvailable = true, BinaryHash = Payload.SysHash,
                Filters = Enumerable.Range(0, 3).Select(_ => new FilterState { Exists = true, Entries = new[] { "HidHide" } }).ToArray(),
                Settings = new DriverSettings { Active = false, Whitelist = SettingsCodec.Encode(new[] { "feeder" }), Blacklist = SettingsCodec.Encode(new[] { "original-baseline" }) } };
            var h = new Fake(r); var tx = new SetupTransaction(h, r); tx.Advance(); tx.CancelBeforeApply();
            r.BeforeMsiFiles = new[] { "HidHideCLI.exe", "HidHideClient.exe", "mfc140u.dll", "msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll", "Driver/HidHide.inf", "Driver/HidHide.sys", "Driver/hidhide.cat", "Driver/LICENSE.rtf", "shortcut/legacy-unified", "shortcut/current" }.ToDictionary(x => x, _ => "absent");
            var previous = new TransactionRecord { Id = r.Id, InitiatingSid = r.Sid, Operation = Operation.Repair, Before = r.Before };
            WindowsSetupHost.VerifyNativeBeforeReplacingPreparation(r, previous, r.Before);
            Check(r.MsiRetryCount == 0 && r.Phase == SetupPhase.LegacyRemoved, "cancelled preparation uses baseline proof without failed-MSI retry count");
            var external = new DriverState { Nodes = r.Before.Nodes, Packages = r.Before.Packages, ServiceExists = true, ControlAvailable = true, BinaryHash = Payload.SysHash, Filters = r.Before.Filters,
                Settings = new DriverSettings { Active = true, Whitelist = r.Before.Settings.Whitelist, Blacklist = SettingsCodec.Encode(new[] { "later-external-baseline" }) } };
            Reject(() => WindowsSetupHost.VerifyNativeBeforeReplacingPreparation(r, previous, external), "pre-Apply cancellation resume rejects later baseline edits before journal replacement");
            Reject(() => WindowsSetupHost.VerifyNativeBeforeReplacingPreparation(r, null, r.Before), "pre-Apply cancellation resume cannot replace missing native evidence");
            Check(previous.Status == JournalStatus.Prepared && previous.Steps.Count == 0 && !previous.Before.Settings!.Active && SettingsCodec.Decode(previous.Before.Settings.Blacklist).Single() == "original-baseline", "refused cancellation resume preserves original native evidence");
        }
        {
            var r = Record(); var h = new Fake(r); var tx = new SetupTransaction(h, r); tx.Advance(); tx.MsiCompleted(1603);
            h.Failure = "rollback-proof";
            Reject(() => tx.Advance(), "unverified rollback cannot retry");
            Check(r.Phase == SetupPhase.RecoveryRequired && r.MsiRetryCount == 0 && !h.Events.Contains("clear"), "failed rollback proof preserves original failure and attempt");
        }
        {
            var r = Record(); r.Before.Filters = Enumerable.Range(0, 3).Select(_ => new FilterState()).ToArray();
            var native = new TransactionRecord { Id = r.Id, InitiatingSid = r.Sid, Operation = Operation.Install, Before = r.Before };
            MsiRollbackRecovery.VerifyNative(r, native, r.Before);
            Check(true, "untouched native attempt permits verified retry");
            native.BootId = "winboot-v1:1"; native.RestartAnchor = "winboot-v1:1"; // completed work carries actual-work evidence
            native.Status = JournalStatus.RolledBack; native.Steps.Add(new Step { Kind = StepKind.Bind, Completed = true, Undone = true });
            MsiRollbackRecovery.VerifyNative(r, native, r.Before);
            Check(true, "completed native rollback permits verified retry");
            foreach (JournalStatus status in new[] { JournalStatus.Applying, JournalStatus.Applied, JournalStatus.RollingBack, JournalStatus.RecoveryRequired, JournalStatus.RebootRequired, JournalStatus.Committed })
            {
                native.Status = status;
                Reject(() => MsiRollbackRecovery.VerifyNative(r, native, r.Before), "uncertain or applied native outcome blocks retry: " + status);
            }
            native.Status = JournalStatus.RolledBack; native.Reboot = true;
            Reject(() => MsiRollbackRecovery.VerifyNative(r, native, r.Before), "pending reboot cannot be declared rolled back");
            native.Reboot = false; native.Steps[0].Undone = false;
            Reject(() => MsiRollbackRecovery.VerifyNative(r, native, r.Before), "incomplete undo blocks retry");
            native.Steps[0].Undone = true;
            Reject(() => MsiRollbackRecovery.VerifyNative(r, native, new DriverState { ServiceExists = true }), "changed native state blocks retry");
            var original = new Dictionary<string, string> { ["file"] = new string('A', 64) };
            Check(!MsiRollbackRecovery.Same(original, new Dictionary<string, string> { ["file"] = new string('B', 64) }) && !MsiRollbackRecovery.Same(original, new Dictionary<string, string>()), "changed or missing application evidence blocks retry");
            var payloadNames = new[] { "HidHideCLI.exe", "HidHideClient.exe", "mfc140u.dll", "msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll", "Driver/HidHide.inf", "Driver/HidHide.sys", "Driver/hidhide.cat", "Driver/LICENSE.rtf" };
            var legacyEvidence = payloadNames.Concat(new[] { "shortcut" }).ToDictionary(x => x, _ => "absent");
            var currentEvidence = payloadNames.Concat(new[] { "shortcut/legacy-unified", "shortcut/current" }).ToDictionary(x => x, _ => "absent");
            MsiRollbackRecovery.ValidateFiles(legacyEvidence); MsiRollbackRecovery.ValidateFiles(currentEvidence);
            Check(MsiRollbackRecovery.Same(legacyEvidence, currentEvidence), "2.1.3 shortcut evidence maps to the exact legacy-unified path");
            currentEvidence["shortcut/current"] = new string('A', 64);
            Check(!MsiRollbackRecovery.Same(legacyEvidence, currentEvidence), "legacy evidence cannot prove an unrecorded new-name shortcut was restored");
            currentEvidence["shortcut/current"] = "absent"; currentEvidence["shortcut/legacy-unified"] = new string('A', 64);
            Check(!MsiRollbackRecovery.Same(legacyEvidence, currentEvidence), "legacy-unified shortcut changes block rollback proof");
        }
        {
            var r = Record(); r.Legacy.Add(Upstream()); r.Legacy[0].RemovalIntent = true; var h = new Fake(r);
            Reject(() => new SetupTransaction(h, r).Advance(), "interrupted legacy intent"); Check(h.Removed.Count == 0, "no destructive replay");
        }
        foreach (Action<SetupRecord> corrupt in new Action<SetupRecord>[] { r => r.Schema = 2, r => r.Id = Guid.Empty, r => r.Sid = "bad", r => r.Version = "99", r => r.Phase = (SetupPhase)99, r => { r.Legacy.Add(Upstream()); r.Legacy.Add(Upstream()); } })
        { var r = Record(); corrupt(r); Reject(() => SetupTransaction.Validate(r), "invalid journal"); }
        Console.WriteLine(checks + " setup controller checks passed; no machine mutations."); return 0;
    }
    sealed class Fake : ISetupHost
    {
        readonly SetupRecord record;
        public List<string> Events = new(); public List<Guid> Removed = new();
        public string Boot { get; set; } = "winboot-v1:1";
        public string Failure = ""; public int RemoveCode; public bool Changed, UnknownProduct, MoreReboot;
        public Fake(SetupRecord r) { record = r; }
        void Event(string value) { Events.Add(value); if (Failure == value) throw new IOException("Injected failure"); }
        public void Save(SetupRecord r) => Event("save:" + r.Phase);
        public void VerifyCache(SetupRecord r) => Event("cache");
        public IReadOnlyList<InstalledProduct> Detect()
        {
            var list = record.Legacy.Where(x => !Removed.Contains(x.Product)).Select(x => new InstalledProduct { Product = x.Product, Family = x.Family, Version = Version.Parse(x.Version), Operation = ProductContract.Select(x.Family, x.Product, Version.Parse(x.Version), Version.Parse(record.Version)) }).ToList();
            if (record.PriorUnified != null) list.Add(new InstalledProduct { Product = record.PriorUnified.Product, Family = ProductContract.MsiUpgradeCode, Version = Version.Parse(record.PriorUnified.Version), Operation = ProductContract.Operation.Upgrade });
            if (UnknownProduct) list.Add(new InstalledProduct { Product = Guid.NewGuid(), Operation = ProductContract.Operation.RejectUnknown }); return list;
        }
        public DriverState Inspect() => Changed ? new DriverState { ServiceExists = true } : record.Before;
        public int RemoveLegacy(Guid product) { Event("remove"); if (RemoveCode == 0 || RemoveCode == 3010) Removed.Add(product); return RemoveCode; }
        public void PrepareDriver(SetupRecord r) => Event("prepare");
        public void VerifyMsiNotStarted(SetupRecord r) => Event("not-started");
        public bool RollbackReboot; public bool VerifyMsiRollbackAndArchive(SetupRecord r) { Event("rollback-proof"); return !RollbackReboot; }
        public bool DriverNeedsReboot(SetupRecord r) => false;
        public bool ResumeDriver(SetupRecord r) { Event("resume"); return !MoreReboot; }
        public void RestoreBaseline(SetupRecord r) => Event("restore");
        public void VerifyFinal(SetupRecord r) => Event("verify");
        public void ClearMarker(SetupRecord r) => Event("clear");
    }
}
