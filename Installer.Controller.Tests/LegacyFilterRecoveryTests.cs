using HidHide.Setup;
using HidHide.DriverSetup;

internal static class LegacyFilterRecoveryTests
{
    public static void Run()
    {
        int checks = 0;
        void Check(bool value) { if (!value) throw new Exception("Legacy filter recovery assertion failed."); checks++; }
        void Reject(Action action) { try { action(); } catch (InvalidOperationException) { checks++; return; } throw new Exception("Unsafe filter recovery accepted."); }
        FilterState F(params string[] values) => new() { Exists = true, Entries = values };
        var desired = new[] { F("Other", "HidHide"), F("HidHide"), F("HidHide") };
        var actual = new[] { F("HidHide", "Other"), F("HidHide"), F("HidHide") };
        var record = new LegacyRecoveryRecord(); int writes = 0; bool failAfterWrite = true;
        void Write(int index, FilterState before, FilterState after)
        {
            Check(record.FilterIntent && record.FilterNext == index && record.FilterBoot == "winboot-v1:1" && before.Same(actual[index]));
            actual[index] = after; writes++;
            if (failAfterWrite) throw new InvalidOperationException("Crash after registry write before completion save");
        }
        Reject(() => LegacyFilterRecovery.Apply(desired, record, "winboot-v1:1", () => actual, Write, () => { }));
        Check(record.FilterIntent && record.FilterNext == 0 && writes == 1);
        failAfterWrite = false;
        Check(!LegacyFilterRecovery.Apply(desired, record, "winboot-v1:1", () => actual, Write, () => { }) && writes == 1);
        Check(LegacyFilterRecovery.Apply(desired, record, "winboot-v1:2", () => actual, Write, () => { }) && writes == 1);
        actual[0] = F("Other", "Unexpected", "HidHide");
        Reject(() => LegacyFilterRecovery.Apply(desired, record, "winboot-v1:2", () => actual, Write, () => { }));
        actual = new[] { F("HidHide", "Other"), F("HidHide"), F("HidHide") }; record = new();
        Reject(() => LegacyFilterRecovery.Apply(desired, record, "winboot-v1:1", () => actual, Write, () => throw new InvalidOperationException("Durable save failed")));
        Check(writes == 1 && actual[0].Entries[0] == "HidHide");
        record = new();
        Check(!LegacyFilterRecovery.Apply(desired, record, "winboot-v1:1", () => actual, Write, () => { }));
        Check(writes == 2 && record.FilterNext == 3 && !record.FilterIntent);
        Console.WriteLine(checks + " legacy filter crash/reboot checks passed; no registry mutations.");
    }
}
