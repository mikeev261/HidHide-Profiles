using System.Globalization;
using System.Runtime.InteropServices;

namespace HidHide.DriverSetup;

public static class BootIdentity
{
    const string Prefix = "winboot-v1:";
    // Windows 11 x64's read-only user shared page. ntddk.h KUSER_SHARED_DATA
    // places BootId between AlternativeArchitecture (0x2c0) and
    // SystemExpirationDate (0x2c8). The OS loader increments it per boot attempt.
    // This reads no wall clock and needs neither elevation nor a driver/WDK.
    public static string Current()
    {
        if (Environment.OSVersion.Platform != PlatformID.Win32NT || IntPtr.Size != 8)
            throw new PlatformNotSupportedException("Boot verification requires Windows x64.");
        var address = new IntPtr(0x7ffe02c4);
        if (!ReadProcessMemory(GetCurrentProcess(), address, out uint sequence, new IntPtr(4), out var read) || read.ToInt64() != 4)
            throw new InvalidOperationException("Cannot read the Windows boot sequence.");
        return FromSequence(sequence);
    }
    public static string FromSequence(uint sequence) => Prefix + sequence.ToString(CultureInfo.InvariantCulture);
    public static bool Stable(string? value) => value != null && value.StartsWith(Prefix, StringComparison.Ordinal) &&
        uint.TryParse(value.Substring(Prefix.Length), NumberStyles.None, CultureInfo.InvariantCulture, out uint sequence) && FromSequence(sequence) == value;
    public static bool Legacy(string? value) => value != null && long.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out long ticks) &&
        ticks > 0 && ticks <= DateTime.MaxValue.Ticks && ticks.ToString(CultureInfo.InvariantCulture) == value;
    public static bool Valid(string? value) => Stable(value) || Legacy(value);
    public static bool Changed(string? previous, string? current) => Stable(previous) && Stable(current) && previous != current;
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)]
    static extern bool ReadProcessMemory(IntPtr process, IntPtr address, out uint value, IntPtr count, out IntPtr read);
}
