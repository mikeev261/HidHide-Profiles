namespace HidHide.DriverSetup;

// The protected cache of the pinned driver payload used by elevated setup.
// createProtected creates a new file with the protected ACL; validateProtected
// checks the ACL of an existing cache file.
public static class PayloadCache
{
    // A cache file that is missing or does not match its pin (for example after
    // an interrupted copy) is replaced from the verified source through a
    // verified staging file; a correct file is never rewritten.
    public static void Prepare(string source, string target, IReadOnlyList<(string Name, string Hash)> pins,
        Func<string, Stream> createProtected, Action<string> validateProtected)
    {
        bool sourceVerified = false;
        foreach (var (name, hash) in pins)
        {
            string destination = Path.Combine(target, name), staging = destination + ".partial";
            if (File.Exists(staging)) { ProtectedJournal.RejectReparsePath(staging); File.Delete(staging); }
            if (File.Exists(destination))
            {
                ProtectedJournal.RejectReparsePath(destination);
                if (Payload.Hash(destination) == hash) { validateProtected(destination); continue; }
            }
            if (!sourceVerified) { VerifySource(source, pins); sourceVerified = true; }
            using (var input = new FileStream(Path.Combine(source, name), FileMode.Open, FileAccess.Read, FileShare.Read))
            using (var output = createProtected(staging)) { input.CopyTo(output); Flush(output); }
            validateProtected(staging);
            if (Payload.Hash(staging) != hash) throw new InvalidDataException("Pinned driver payload copy mismatch: " + name);
            if (!MoveFileExW(staging, destination, 1 | 8)) // MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
                throw new System.ComponentModel.Win32Exception(System.Runtime.InteropServices.Marshal.GetLastWin32Error(), "Payload cache replacement failed.");
        }
        Verify(target, pins);
    }

    [System.Runtime.InteropServices.DllImport("kernel32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode, SetLastError = true, ExactSpelling = true)]
    static extern bool MoveFileExW(string source, string target, int flags);

    static void VerifySource(string source, IReadOnlyList<(string Name, string Hash)> pins)
    { ProtectedJournal.RejectReparsePath(source); Verify(source, pins); }

    static void Verify(string directory, IReadOnlyList<(string Name, string Hash)> pins)
    {
        foreach (var (name, hash) in pins)
        {
            var path = Path.Combine(directory, name);
            ProtectedJournal.RejectReparsePath(path);
            if (Payload.Hash(path) != hash) throw new InvalidDataException("Pinned driver payload mismatch: " + name);
        }
    }

    static void Flush(Stream output) { if (output is FileStream file) file.Flush(true); else output.Flush(); }
}
