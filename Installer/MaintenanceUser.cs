using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace HidHide.Installer;

// MSI immediate actions may impersonate an elevated consent token even when
// launched normally from Settings. Select the ordinary MSI client's token and
// authenticate its SID/session; never guess from Explorer or inherit SYSTEM.
internal sealed class MaintenanceUser : IDisposable
{
    readonly SafeAccessTokenHandle token;
    public string Sid { get; }
    public int SessionId { get; }

    public MaintenanceUser(string initiatingSid, int clientProcessId = 0)
    {
        using var identity = WindowsIdentity.GetCurrent();
        Sid = identity.User?.Value ?? throw new UnauthorizedAccessException("Cannot identify the setup user.");
        if (Sid != initiatingSid || identity.IsSystem)
            throw new UnauthorizedAccessException("Setup must be started by the profile owner. Administrator credentials for a different Windows user cannot prepare this user's configuration.");
        var source = identity.AccessToken;
        SafeAccessTokenHandle? clientToken = null;
        try
        {
            if (TokenNumber(source, 20) != 0) // TokenElevation
            {
                if (clientProcessId <= 0) throw new UnauthorizedAccessException("Start setup normally from Windows Settings or Explorer so it can preserve the initiating user's configuration.");
                // The PID selects a candidate, never authorizes it. Validate
                // against the MSI impersonation identity and interactive session.
                // Unlike TokenLinkedToken this is a usable primary token even
                // when Windows exposes the linked token at identification level.
                using var client = OpenProcess(0x1000, false, clientProcessId);
                if (client.IsInvalid || !OpenProcessToken(client.DangerousGetHandle(), 0xA, out clientToken))
                    throw Error("Read initiating MSI client token");
                Validate(clientToken, Sid, TokenNumber(source, 12));
                source = clientToken;
            }
            if (!DuplicateTokenEx(source, 0x02000000, IntPtr.Zero, 2, 1, out token)) throw Error("Duplicate setup user token");
        }
        finally { clientToken?.Dispose(); }
        try
        {
            SessionId = TokenNumber(token, 12);
            Validate(token, Sid, SessionId);
        }
        catch { token.Dispose(); throw; }
    }

    public Process StartMaintenance(Guid id, bool uninstall, out HelperErrorOutput errors)
    {
        if (id == Guid.Empty) throw new ArgumentException("Missing maintenance transaction.");
        string cli = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "HidHide", "HidHideCLI.exe");
        if (!File.Exists(cli)) throw new FileNotFoundException("The installed maintenance helper is missing.", cli);
        return Start(cli, (uninstall ? "--maintenance-msi-uninstall " : "--maintenance-msi ") + id.ToString("D"), out errors);
    }

    internal Process Start(string executable, string arguments)
    {
        var result = Start(executable, arguments, out var errors);
        errors.Dispose();
        return result;
    }

    // Internal seam also used by the isolated token probe. Production supplies
    // only the fixed installed CLI and a generated transaction GUID above.
    // The helper's stderr is a private anonymous pipe: its write end is the
    // only handle the helper inherits, so any reported reason comes from it.
    internal Process Start(string executable, string arguments, out HelperErrorOutput errors)
    {
        IntPtr environment = IntPtr.Zero, attributes = IntPtr.Zero, inherited = IntPtr.Zero;
        bool attributesInitialized = false;
        SafeFileHandle? errorRead = null, errorWrite = null;
        SafeAccessTokenHandle? previous = null;
        ProcessInfo child = default;
        bool resumed = false;
        try
        {
            if (!CreatePipe(out errorRead, out errorWrite, IntPtr.Zero, 65536) || !SetHandleInformation(errorWrite, 1, 1))
                throw Error("Create maintenance diagnostics pipe");
            IntPtr size = IntPtr.Zero;
            InitializeProcThreadAttributeList(IntPtr.Zero, 1, 0, ref size);
            attributes = Marshal.AllocHGlobal(size);
            if (!InitializeProcThreadAttributeList(attributes, 1, 0, ref size)) throw Error("Prepare maintenance handle list");
            attributesInitialized = true;
            inherited = Marshal.AllocHGlobal(IntPtr.Size);
            Marshal.WriteIntPtr(inherited, errorWrite.DangerousGetHandle());
            if (!UpdateProcThreadAttribute(attributes, 0, (IntPtr)0x20002, inherited, (IntPtr)IntPtr.Size, IntPtr.Zero, IntPtr.Zero))
                throw Error("Restrict maintenance handle inheritance"); // PROC_THREAD_ATTRIBUTE_HANDLE_LIST
            if (!CreateEnvironmentBlock(out environment, token, false)) throw Error("Create setup user environment");
            bool impersonating = OpenThreadToken(GetCurrentThread(), 0xC, true, out var saved);
            if (!impersonating && Marshal.GetLastWin32Error() != 1008) { saved.Dispose(); throw Error("Inspect setup impersonation"); }
            if (impersonating) previous = saved; else saved.Dispose();
            if (!RevertToSelf()) throw Error("Enter MSI server process context");
            try
            {
                // STARTF_USESTDHANDLES: only stderr is connected, to the diagnostics
                // pipe; stdin and stdout are null (the MSI helper never uses them).
                var startup = new StartupInfo { Size = Marshal.SizeOf(typeof(StartupInfo)), Desktop = @"winsta0\default",
                    Flags = 0x100, Error = errorWrite.DangerousGetHandle() };
                var extended = new StartupInfoEx { StartupInfo = startup, AttributeList = attributes };
                extended.StartupInfo.Size = Marshal.SizeOf(typeof(StartupInfoEx));
                const uint flags = 0x08000404; // NO_WINDOW | UNICODE_ENVIRONMENT | SUSPENDED
                const uint extendedFlags = flags | 0x00080000; // EXTENDED_STARTUPINFO_PRESENT
                var command = new StringBuilder("\"" + executable + "\" " + arguments);
                bool created = CreateProcessAsUserW(token, executable, command, IntPtr.Zero, IntPtr.Zero, true, extendedFlags,
                    environment, Path.GetDirectoryName(executable), ref extended, out child);
                int error = created ? 0 : Marshal.GetLastWin32Error();
                if (!created && error != 1314)
                {
                    // Diagnostics are best effort: if the host cannot hand the
                    // pipe to the helper, start it exactly as before without one.
                    var plain = new StartupInfo { Size = Marshal.SizeOf(typeof(StartupInfo)), Desktop = @"winsta0\default" };
                    command = new StringBuilder("\"" + executable + "\" " + arguments);
                    created = CreateProcessAsUserW(token, executable, command, IntPtr.Zero, IntPtr.Zero, false, flags,
                        environment, Path.GetDirectoryName(executable), ref plain, out child);
                    if (!created) error = Marshal.GetLastWin32Error();
                }
                if (!created)
                {
                    using var processIdentity = WindowsIdentity.GetCurrent();
                    // Interactive elevated hosts lack service privileges. This
                    // API is safe only in the target session (it ignores the
                    // token's session), with the explicitly selected user token.
                    if (error != 1314 || Process.GetCurrentProcess().SessionId != SessionId)
                        throw new Win32Exception(error, "Start ordinary-user configuration maintenance");
                    command = new StringBuilder("\"" + executable + "\" " + arguments);
                    // The secondary logon service duplicates only the standard
                    // handles named in STARTUPINFO; it never inherits others.
                    bool started = TokenNumber(processIdentity.AccessToken, 20) != 0
                        ? CreateProcessWithTokenW(token, 0, executable, command, flags, environment,
                            Path.GetDirectoryName(executable), ref startup, out child)
                        : processIdentity.User?.Value == Sid && CreateProcessW(executable, command, IntPtr.Zero,
                            IntPtr.Zero, true, extendedFlags, environment, Path.GetDirectoryName(executable), ref extended, out child);
                    if (!started) throw Error("Start ordinary-user configuration maintenance");
                }
                if (!OpenProcessToken(child.Process, 8, out var childToken)) throw Error("Verify maintenance process token");
                using (childToken) Validate(childToken, Sid, SessionId);
                var result = Process.GetProcessById((int)child.ProcessId);
                _ = result.Handle; // Retain exit status even if the helper exits immediately.
                if (ResumeThread(child.Thread) == uint.MaxValue) { result.Dispose(); throw Error("Start maintenance thread"); }
                resumed = true;
                errors = new HelperErrorOutput(errorRead);
                errorRead = null;
                return result;
            }
            finally
            {
                if (previous != null && !SetThreadToken(IntPtr.Zero, previous))
                    throw Error("Restore MSI action impersonation");
            }
        }
        finally
        {
            // Only terminate a child that never ran; never kill profile recovery.
            if (child.Process != IntPtr.Zero) { if (!resumed) TerminateProcess(child.Process, 1); CloseHandle(child.Process); }
            if (child.Thread != IntPtr.Zero) CloseHandle(child.Thread);
            previous?.Dispose();
            if (environment != IntPtr.Zero) DestroyEnvironmentBlock(environment);
            // The helper holds the only remaining write end, so its exit ends the stream.
            errorWrite?.Dispose();
            errorRead?.Dispose();
            if (attributesInitialized) DeleteProcThreadAttributeList(attributes);
            if (attributes != IntPtr.Zero) Marshal.FreeHGlobal(attributes);
            if (inherited != IntPtr.Zero) Marshal.FreeHGlobal(inherited);
        }
    }

    internal static void Validate(SafeAccessTokenHandle value, string sid, int sessionId)
    {
        using var identity = new WindowsIdentity(value.DangerousGetHandle());
        if (identity.User?.Value != sid || identity.IsSystem || TokenNumber(value, 20) != 0 ||
            sessionId <= 0 || TokenNumber(value, 12) != sessionId)
            throw new UnauthorizedAccessException("Configuration maintenance must run as the same ordinary Windows user in their interactive session.");
    }
    internal static int TokenNumber(SafeAccessTokenHandle value, int kind)
    {
        if (!GetTokenInformation(value, kind, out int result, sizeof(int), out _)) throw Error("Inspect setup user token");
        return result;
    }
    static Win32Exception Error(string message) => new(Marshal.GetLastWin32Error(), message);
    public void Dispose() => token.Dispose();

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] struct StartupInfo
    {
        public int Size; public string? Reserved; public string? Desktop; public string? Title;
        public uint X, Y, Width, Height, XChars, YChars, Fill, Flags;
        public ushort Show, ReservedBytes; public IntPtr ReservedData, Input, Output, Error;
    }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] struct StartupInfoEx { public StartupInfo StartupInfo; public IntPtr AttributeList; }
    [StructLayout(LayoutKind.Sequential)] struct ProcessInfo { public IntPtr Process, Thread; public uint ProcessId, ThreadId; }
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool GetTokenInformation(SafeAccessTokenHandle token, int kind, out int value, int size, out int needed);
    [DllImport("kernel32.dll", SetLastError = true)] static extern SafeProcessHandle OpenProcess(uint access, bool inherit, int pid);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool DuplicateTokenEx(SafeAccessTokenHandle token, uint access, IntPtr attributes, int level, int type, out SafeAccessTokenHandle result);
    [DllImport("advapi32.dll", SetLastError = true)] internal static extern bool OpenProcessToken(IntPtr process, uint access, out SafeAccessTokenHandle token);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool OpenThreadToken(IntPtr thread, uint access, bool self, out SafeAccessTokenHandle token);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool RevertToSelf();
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool SetThreadToken(IntPtr thread, SafeAccessTokenHandle token);
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentThread();
    [DllImport("userenv.dll", SetLastError = true)] static extern bool CreateEnvironmentBlock(out IntPtr environment, SafeAccessTokenHandle token, bool inherit);
    [DllImport("userenv.dll")] static extern bool DestroyEnvironmentBlock(IntPtr environment);
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern bool CreateProcessAsUserW(SafeAccessTokenHandle token, string app, StringBuilder command, IntPtr processAttributes, IntPtr threadAttributes, bool inherit, uint flags, IntPtr environment, string? directory, ref StartupInfoEx startup, out ProcessInfo process);
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern bool CreateProcessAsUserW(SafeAccessTokenHandle token, string app, StringBuilder command, IntPtr processAttributes, IntPtr threadAttributes, bool inherit, uint flags, IntPtr environment, string? directory, ref StartupInfo startup, out ProcessInfo process);
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern bool CreateProcessWithTokenW(SafeAccessTokenHandle token, uint logon, string app, StringBuilder command, uint flags, IntPtr environment, string? directory, ref StartupInfo startup, out ProcessInfo process);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern bool CreateProcessW(string app, StringBuilder command, IntPtr processAttributes, IntPtr threadAttributes, bool inherit, uint flags, IntPtr environment, string? directory, ref StartupInfoEx startup, out ProcessInfo process);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool CreatePipe(out SafeFileHandle read, out SafeFileHandle write, IntPtr attributes, uint size);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool SetHandleInformation(SafeFileHandle handle, uint mask, uint flags);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool InitializeProcThreadAttributeList(IntPtr list, int count, int flags, ref IntPtr size);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool UpdateProcThreadAttribute(IntPtr list, uint flags, IntPtr attribute, IntPtr value, IntPtr size, IntPtr previous, IntPtr returnSize);
    [DllImport("kernel32.dll")] static extern void DeleteProcThreadAttributeList(IntPtr list);
    [DllImport("kernel32.dll", SetLastError = true)] static extern uint ResumeThread(IntPtr thread);
    [DllImport("kernel32.dll")] static extern bool TerminateProcess(IntPtr process, uint code);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
}

// Bounded capture of the helper's stderr. The CLI writes why the resident
// coordinator refused maintenance there; the text is display-only data from
// the ordinary-user helper and never selects any elevated behaviour.
// The pipe is only ever polled on the caller's thread: the helper stays alive
// with its write end open after readiness, and the .NET Framework custom action
// AppDomain cannot unload while any thread is blocked in a pipe read.
internal sealed class HelperErrorOutput : IDisposable
{
    const int Limit = 4096, ReasonLimit = 512, ReadsPerDrain = 64;
    readonly SafeFileHandle pipe;
    readonly byte[] buffer = new byte[Limit], chunk = new byte[4096];
    int length;

    internal HelperErrorOutput(SafeFileHandle pipe) => this.pipe = pipe;

    // Reads only bytes already in the pipe, so it never blocks. Keeps draining
    // past the limit so a verbose helper never blocks on a full pipe. Returns
    // false once the helper's write end is closed and nothing remains.
    public bool Drain()
    {
        if (pipe.IsClosed) return false;
        for (int reads = 0; reads < ReadsPerDrain; reads++)
        {
            if (!PeekNamedPipe(pipe, IntPtr.Zero, 0, IntPtr.Zero, out uint available, IntPtr.Zero)) return false;
            if (available == 0) return true;
            if (!ReadFile(pipe, chunk, Math.Min(available, (uint)chunk.Length), out uint read, IntPtr.Zero) || read == 0) return false;
            int copy = Math.Min((int)read, Limit - length);
            Array.Copy(chunk, 0, buffer, length, copy);
            length += copy;
        }
        return true;
    }

    // Waits briefly for the (exited) helper's stream to end, then returns
    // whatever arrived as a single plain line.
    public string Reason(int milliseconds)
    {
        var clock = Stopwatch.StartNew();
        while (Drain() && clock.ElapsedMilliseconds < milliseconds) Thread.Sleep(10);
        return Sanitize(Encoding.UTF8.GetString(buffer, 0, length));
    }

    // Later helper writes fail harmlessly; the CLI only reports on failure.
    public void Dispose() => pipe.Dispose();

    [DllImport("kernel32.dll", SetLastError = true)] static extern bool PeekNamedPipe(SafeFileHandle pipe, IntPtr buffer, uint size, IntPtr read, out uint available, IntPtr left);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool ReadFile(SafeFileHandle file, byte[] buffer, uint size, out uint read, IntPtr overlapped);

    // One line of printable text; brackets and braces are neutralized so the
    // text cannot act as Windows Installer formatting in the error record.
    internal static string Sanitize(string text)
    {
        var result = new StringBuilder();
        bool separator = false;
        foreach (char c in text)
        {
            var category = char.GetUnicodeCategory(c);
            if (char.IsWhiteSpace(c) || char.IsControl(c) || category == System.Globalization.UnicodeCategory.Format || char.IsSurrogate(c))
            {
                separator = result.Length > 0;
                continue;
            }
            if (result.Length >= ReasonLimit) break;
            if (separator) { result.Append(' '); separator = false; }
            result.Append(c == '[' || c == '{' ? '(' : c == ']' || c == '}' ? ')' : c);
        }
        return result.ToString();
    }
}

internal static class HelperReadiness
{
    public static void Wait(Process helper, HelperErrorOutput errors, Func<bool> ready, TimeSpan timeout)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline)
        {
            errors.Drain();
            if (helper.HasExited)
            {
                string reason = errors.Reason(1000).TrimEnd('.', ' ');
                throw new InvalidOperationException("The configuration helper exited before confirming readiness (code " + helper.ExitCode + ")"
                    + (reason.Length > 0 ? ": " + reason : "") + ". Close the HidHide Profiles editor and any application launched from it, then retry setup.");
            }
            if (ready()) return;
        }
        throw new TimeoutException("Configuration maintenance did not become ready.");
    }
}
