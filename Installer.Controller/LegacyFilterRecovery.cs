using HidHide.DriverSetup;
using HidHide.Installer;

namespace HidHide.Setup;

public static class LegacyFilterRecovery
{
    public static bool Apply(FilterState[] desired, LegacyRecoveryRecord recovery, string boot,
        Func<FilterState[]> inspect, Action<int, FilterState, FilterState> write, Action save)
    {
        var actual = inspect();
        if (actual.Length != 3 || desired.Length != 3) throw new InvalidOperationException("Missing filter restoration evidence.");
        for (int i = 0; i < 3; i++)
            if (!actual[i].Entries.Where(x => !DriverFilters.IsHidHide(x)).SequenceEqual(desired[i].Entries.Where(x => !DriverFilters.IsHidHide(x))))
                throw new InvalidOperationException("Unrelated filters changed before restoration.");
        if (recovery.FilterBefore == null)
        { recovery.FilterBefore = actual.Select(x => new FilterState { Exists = x.Exists, Entries = x.Entries.ToArray() }).ToArray(); save(); }
        for (int i = 0; i < recovery.FilterNext; i++) if (!actual[i].Same(desired[i])) throw new InvalidOperationException("Restored class filter changed.");
        while (recovery.FilterNext < 3)
        {
            int i = recovery.FilterNext;
            var before = recovery.FilterBefore[i]; var target = desired[i];
            if (!actual[i].Same(before) && !(recovery.FilterIntent && actual[i].Same(target))) throw new InvalidOperationException("Class filter changed outside recorded restoration.");
            if (!before.Same(target))
            {
                recovery.FilterIntent = true;
                // If a prior write completed before a crash, preserve its boot
                // boundary. A new write records the current boot before mutation.
                if (!actual[i].Same(target)) recovery.FilterBoot = boot;
                save();
                if (!actual[i].Same(target)) write(i, before, target);
                if (!inspect()[i].Same(target)) throw new InvalidOperationException("Class filter restoration read-back failed.");
            }
            recovery.FilterNext++; recovery.FilterIntent = false; save();
        }
        return recovery.FilterBoot.Length == 0 || BootIdentity.Changed(recovery.FilterBoot, boot);
    }
}
