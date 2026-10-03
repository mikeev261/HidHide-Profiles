using System.Runtime.InteropServices;
using System.ComponentModel;
using System.Threading;
using Microsoft.Win32.SafeHandles;

namespace HidHide.DriverSetup;

internal static class ControlHandle
{
    // Sharing permits compatible clients; it cannot override an exclusive
    // device or an existing incompatible handle. Restoration remains exclusive.
    internal static SafeFileHandle OpenInspection(string path)
    {
        // The immutable driver can itself be exclusive. Sharing flags only
        // permit compatible handles; transient contention must never become
        // an absent/damaged-driver snapshot eligible for repair.
        const int attempts = 11;
        for (int attempt = 0; ; attempt++)
        {
            var handle = Open(path, 3);
            if (!handle.IsInvalid) return handle;
            int error = Marshal.GetLastWin32Error();
            // Only an absent interface supports the existing repair fallback.
            if (error == 2 || error == 3) return handle;
            handle.Dispose();
            if ((error != 5 && error != 32) || attempt + 1 == attempts)
                throw new Win32Exception(error, "Cannot inspect HidHide control interface; close conflicting utilities and retry maintenance.");
            Thread.Sleep(100);
        }
    }
    internal static SafeFileHandle OpenRestoration(string path)
        => Open(path, 0);
    static SafeFileHandle Open(string path, uint share)
        => CreateFileW(path, 0x80000000, share, IntPtr.Zero, 3, 0, IntPtr.Zero);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, ExactSpelling = true)]
    static extern SafeFileHandle CreateFileW(string path, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);
}
