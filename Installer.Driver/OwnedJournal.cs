namespace HidHide.DriverSetup;

// A snapshot is usable only inside the ownership interval that loaded it.
// Acquiring may wait while the preceding owner advances or completes recovery.
public sealed class OwnedJournal<TLease> : IDisposable where TLease : IDisposable
{
    public TLease Lease { get; }
    public TransactionRecord Record { get; }
    // allowCommitted is only for a recovery run that must close a transaction
    // whose marker outlived its commit; nothing else may act on a committed one.
    public OwnedJournal(Func<TLease> acquire, Func<TransactionRecord> load, bool allowCommitted = false)
    {
        Lease = acquire();
        try
        {
            Record = load();
            ProtectedJournal.Validate(Record);
            if (Record.Status == JournalStatus.Committed && !allowCommitted)
                throw new InvalidOperationException("The driver transaction has already committed.");
        }
        catch { Lease.Dispose(); throw; }
    }
    public void Dispose() => Lease.Dispose();
}
