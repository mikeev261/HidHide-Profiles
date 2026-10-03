using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32;
using Microsoft.Win32.SafeHandles;
using HidHide.Installer;

namespace HidHide.DriverSetup;

// Owned Windows API implementation. All mutators require an elevated lease,
// exact pinned payload and state-specific checks. Execution requires a protected
// record supplied by setup; read-only inspection needs no maintenance session.
public sealed class WindowsDriverBackend : IDriverBackend
{
    const string HardwareId = @"root\HidHide";
    static readonly Guid SystemClass = new("4D36E97D-E325-11CE-BFC1-08002BE10318");
    readonly string payloadDirectory;
    readonly Action? requireMutationLease;
    public WindowsDriverBackend(string payloadDirectory, Action? requireMutationLease = null)
    { this.payloadDirectory = Path.GetFullPath(payloadDirectory); this.requireMutationLease = requireMutationLease; }
    void Guard()
    {
        if (requireMutationLease == null) throw new UnauthorizedAccessException("Read-only driver backend.");
        ProtectedJournal.RequireAdministrator(); requireMutationLease(); Payload.Verify(payloadDirectory);
    }
    static Exception Error(string operation) => new Win32Exception(Marshal.GetLastWin32Error(), operation);
    static string InfPath(string name) => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "INF", Payload.PublishedInf(name));
    static FilterState Filter(int index)
    {
        if (index < 0 || index >= 3) throw new ArgumentOutOfRangeException(nameof(index));
        using var machine = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
        using var key = machine.OpenSubKey(@"SYSTEM\CurrentControlSet\Control\Class\" + DriverFilters.Classes[index].ToString("B"));
        return new FilterState { Exists = key?.GetValue("UpperFilters") != null, Entries = DriverFilters.Read(DriverFilters.Classes[index]) };
    }
    static string Property(Devices set, ref DeviceData data, uint property, bool multi = false)
    {
        var bytes = new byte[65536];
        if (!SetupDiGetDeviceRegistryPropertyW(set.Handle, ref data, property, out uint type, bytes, (uint)bytes.Length, out uint needed))
        {
            if (Marshal.GetLastWin32Error() == 13) return ""; // ERROR_INVALID_DATA: property absent.
            throw Error("Read device property");
        }
        if (needed > bytes.Length || (needed & 1) != 0 || type != (multi ? 7u : 1u)) throw new InvalidDataException("Unexpected device property.");
        return Encoding.Unicode.GetString(bytes, 0, (int)needed).TrimEnd('\0');
    }
    static string Instance(Devices set, ref DeviceData data)
    {
        var name = new StringBuilder(512);
        if (!SetupDiGetDeviceInstanceIdW(set.Handle, ref data, name, (uint)name.Capacity, out _)) throw Error("Read device instance identity");
        return name.ToString();
    }
    static Node ReadNode(Devices set, ref DeviceData data)
    {
        var driver = Property(set, ref data, 9); // SPDRP_DRIVER
        string inf = "";
        if (driver != "")
        {
            // Key name is supplied by SetupAPI, never by an elevated command.
            using var key = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Control\Class\" + driver);
            inf = key?.GetValue("InfPath") as string ?? throw new InvalidDataException("Missing device INF identity.");
            Payload.PublishedInf(inf);
            if (File.Exists(InfPath(inf)) && Payload.Hash(InfPath(inf)) != Payload.InfHash) throw new InvalidDataException("Unknown/newer HidHide package; maintenance refused.");
        }
        var status = CM_Get_DevNode_Status(out uint flags, out uint problem, data.DevInst, 0);
        if (status != 0) problem = uint.MaxValue; // Phantom or unavailable device is not healthy.
        return new Node { Id = Instance(set, ref data), Inf = inf, Service = Property(set, ref data, 4), Problem = problem };
    }
    static IEnumerable<DeviceData> Matching(Devices set)
    {
        for (uint index = 0; index < 65536; index++)
        {
            var data = DeviceData.New();
            if (!SetupDiEnumDeviceInfo(set.Handle, index, ref data))
            {
                if (Marshal.GetLastWin32Error() == 259) yield break;
                throw Error("Enumerate devices");
            }
            if (Property(set, ref data, 1, true).Split('\0').Any(x => string.Equals(x, HardwareId, StringComparison.OrdinalIgnoreCase)))
            {
                if (data.ClassGuid != SystemClass || !Instance(set, ref data).StartsWith("ROOT\\", StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException("Unexpected HidHide device identity.");
                yield return data;
            }
        }
        throw new InvalidDataException("Device enumeration exceeds bound.");
    }
    public DriverState Inspect()
    {
        if (!Environment.Is64BitProcess) throw new PlatformNotSupportedException("x64 required.");
        using var set = Devices.All();
        var nodes = new List<Node>();
        foreach (var value in Matching(set)) { var data = value; nodes.Add(ReadNode(set, ref data)); }
        var packages = new List<string>();
        string infRoot = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "INF");
        var files = Directory.GetFiles(infRoot, "oem*.inf");
        if (files.Length > 16384) throw new InvalidDataException("Driver Store inventory exceeds bound.");
        foreach (var path in files)
        {
            // Only exact hardware-ID candidates are hashed. Unknown candidates
            // block maintenance; they are never selected for deletion.
            if (new FileInfo(path).Length > 16 * 1024 * 1024) throw new InvalidDataException("Oversized OEM INF.");
            if (File.ReadAllText(path).IndexOf(HardwareId, StringComparison.OrdinalIgnoreCase) < 0) continue;
            if (Payload.Hash(path) != Payload.InfHash) throw new InvalidDataException("Unrecognized HidHide OEM INF: " + Path.GetFileName(path));
            packages.Add(Payload.PublishedInf(Path.GetFileName(path)));
        }
        using var service = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Services\HidHide");
        if (service != null)
        {
            bool deleting = (int?)service.GetValue("DeleteFlag") == 1;
            var image = service.GetValue("ImagePath", "", RegistryValueOptions.DoNotExpandEnvironmentNames) as string;
            if ((int?)service.GetValue("Type") != 1 || ((int?)service.GetValue("Start") != 3 && !(deleting && (int?)service.GetValue("Start") == 4)) ||
                !string.Equals(image, @"\SystemRoot\System32\drivers\HidHide.sys", StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("Unexpected HidHide service configuration.");
        }
        var binary = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "drivers", "HidHide.sys");
        using var control = ControlHandle.OpenInspection(@"\\.\HidHide");
        return new DriverState {
            Nodes = nodes.OrderBy(x => x.Id, StringComparer.OrdinalIgnoreCase).ToArray(),
            Packages = packages.OrderBy(x => x, StringComparer.OrdinalIgnoreCase).ToArray(),
            Filters = Enumerable.Range(0, 3).Select(Filter).ToArray(), ServiceExists = service != null,
            ServicePendingDeletion = (int?)service?.GetValue("DeleteFlag") == 1,
            BinaryHash = File.Exists(binary) ? Payload.Hash(binary) : "", ControlAvailable = !control.IsInvalid,
            Settings = control.IsInvalid ? service == null || (int?)service.GetValue("DeleteFlag") == 1 ? null : StoredDriverSettings.FromRegistry() : ReadSettings(control)
        };
    }
    static DriverSettings ReadSettings(SafeFileHandle control)
    {
        byte[] Read(uint function, bool list)
        {
            uint code = (32769u << 16) | (1u << 14) | (function << 2);
            uint size = 1;
            if (list && !DeviceIoControl(control, code, IntPtr.Zero, 0, null, 0, out size, IntPtr.Zero)) throw Error("Read settings size");
            if (size > 1024 * 1024 || (list && (size < 2 || size % 2 != 0))) throw new InvalidDataException("Invalid driver settings size.");
            var buffer = new byte[size];
            if (!DeviceIoControl(control, code, IntPtr.Zero, 0, buffer, size, out uint used, IntPtr.Zero)) throw Error("Read baseline settings");
            if (used != size) throw new InvalidDataException("Driver settings changed during read.");
            return buffer;
        }
        return new DriverSettings { Whitelist = Read(2048, true), Blacklist = Read(2050, true), Active = Read(2052, false)[0] != 0, Inverse = Read(2054, false)[0] != 0 };
    }
    public string Stage()
    {
        Guard();
        if (Inspect().Packages.Length != 0) throw new InvalidOperationException("Package appeared before staging.");
        var destination = new StringBuilder(260);
        if (!SetupCopyOEMInfW(Path.Combine(payloadDirectory, "HidHide.inf"), payloadDirectory, 1, 8, destination, (uint)destination.Capacity, out _, IntPtr.Zero))
            throw Error("Stage signed HidHide package"); // Includes concurrent ERROR_FILE_EXISTS: not newly owned.
        var name = Payload.PublishedInf(Path.GetFileName(destination.ToString()));
        if (Payload.Hash(InfPath(name)) != Payload.InfHash) throw new InvalidDataException("Staged INF verification failed.");
        return name;
    }
    public void RestoreSettings(DriverSettings expected, DriverSettings desired)
    {
        Guard();
        if (!Inspect().CoreHealthy) throw new InvalidOperationException("Cannot restore settings to an unverified driver.");
        using var control = ControlHandle.OpenRestoration(@"\\.\HidHide");
        if (control.IsInvalid) throw Error("Acquire exclusive baseline restoration handle");
        SettingsRestoration.Apply(expected, desired, () => ReadSettings(control), (field, bytes) =>
        {
            uint function = field == SettingsField.Whitelist ? 2049u : field == SettingsField.Blacklist ? 2051u : field == SettingsField.Active ? 2053u : 2055u;
            uint code = (32769u << 16) | (1u << 14) | (function << 2);
            if (!WriteDeviceIoControl(control, code, bytes, (uint)bytes.Length, IntPtr.Zero, 0, out _, IntPtr.Zero))
                throw Error("Restore baseline field " + field);
        });
    }
    public string CreateNode()
    {
        Guard(); if (Inspect().Nodes.Length != 0) throw new InvalidOperationException("HidHide root node already exists.");
        var classGuid = SystemClass;
        using var set = new Devices(SetupDiCreateDeviceInfoList(ref classGuid, IntPtr.Zero));
        var data = DeviceData.New();
        if (!SetupDiCreateDeviceInfoW(set.Handle, "HidHide", ref classGuid, "HidHide", IntPtr.Zero, 1, ref data)) throw Error("Create HidHide root node");
        var hardware = Encoding.Unicode.GetBytes(HardwareId + "\0\0");
        if (!SetupDiSetDeviceRegistryPropertyW(set.Handle, ref data, 1, hardware, (uint)hardware.Length)) throw Error("Set root hardware identity");
        if (!SetupDiCallClassInstaller(0x19, set.Handle, ref data)) throw Error("Register HidHide root node");
        return Instance(set, ref data);
    }
    public bool Bind()
        => Bind(false);
    public bool RepairBind()
        => Bind(true);
    bool Bind(bool repair)
    {
        Guard(); var state = Inspect();
        if (state.Nodes.Length != 1 || state.Packages.Length != 1) throw new InvalidOperationException("Ambiguous binding state.");
        if (repair && !state.Repairable) throw new InvalidOperationException("Driver repair ownership changed.");
        if (!UpdateDriverForPlugAndPlayDevicesW(IntPtr.Zero, HardwareId, Path.Combine(payloadDirectory, "HidHide.inf"), repair ? 5u : 4u, out bool reboot))
            throw Error("Bind signed HidHide driver");
        return reboot; // BOOL TRUE is 1, never compare it with > 1.
    }
    public void SetFilter(int index, FilterState expected, FilterState desired)
    {
        Guard(); if (!Filter(index).Same(expected)) throw new InvalidOperationException("Class filters changed externally.");
        DriverFilters.Change(desired.Entries, false);
        if (!DriverFilters.Change(expected.Entries, false).SequenceEqual(DriverFilters.Change(desired.Entries, false)))
            throw new InvalidOperationException("Maintenance cannot change another vendor's filters.");
        if (!desired.Exists && desired.Entries.Length != 0) throw new InvalidDataException("Invalid absent filter value.");
        using var machine = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
        using var key = machine.CreateSubKey(@"SYSTEM\CurrentControlSet\Control\Class\" + DriverFilters.Classes[index].ToString("B"), true);
        if (key == null) throw new InvalidOperationException("Cannot open required device class.");
        if (desired.Exists) key.SetValue("UpperFilters", desired.Entries, RegistryValueKind.MultiString);
        else key.DeleteValue("UpperFilters", false);
        key.Flush();
        if (!Filter(index).Same(desired)) throw new InvalidOperationException("Class-filter read-back failed.");
    }
    public bool RemoveNode(string id)
    {
        Guard(); var state = Inspect();
        if (state.Filters.Any(x => x.Entries.Any(DriverFilters.IsHidHide))) throw new InvalidOperationException("Filters must be detached before node removal.");
        if (state.Nodes.Length != 1 || !string.Equals(state.Nodes[0].Id, id, StringComparison.OrdinalIgnoreCase)) throw new InvalidOperationException("Node ownership changed.");
        using var set = Devices.All();
        foreach (var value in Matching(set))
        {
            var data = value;
            if (!string.Equals(Instance(set, ref data), id, StringComparison.OrdinalIgnoreCase)) continue;
            if (!DiUninstallDevice(IntPtr.Zero, set.Handle, ref data, 0, out bool reboot)) throw Error("Remove owned HidHide node");
            var after = Inspect();
            if (!reboot && after.Nodes.Any(x => string.Equals(x.Id, id, StringComparison.OrdinalIgnoreCase))) throw new InvalidOperationException("Node removal not confirmed.");
            return reboot || after.ServicePendingDeletion;
        }
        throw new InvalidOperationException("Owned root node disappeared.");
    }
    public bool RemovePackage(string publishedInf)
    {
        Guard(); var state = Inspect();
        if (state.Nodes.Length != 0 || state.Filters.Any(x => x.Entries.Any(DriverFilters.IsHidHide))) throw new InvalidOperationException("Node/filter ownership prevents package deletion.");
        if (!state.Packages.Contains(publishedInf, StringComparer.OrdinalIgnoreCase)) throw new InvalidOperationException("Package ownership changed.");
        string path = InfPath(publishedInf);
        if (Payload.Hash(path) != Payload.InfHash) throw new InvalidDataException("Published package identity changed.");
        if (!DiUninstallDriverW(IntPtr.Zero, path, 0, out bool reboot)) throw Error("Remove exact owned HidHide package");
        if (!reboot && File.Exists(path)) throw new InvalidOperationException("Package removal not confirmed.");
        return reboot || Inspect().ServicePendingDeletion;
    }
    public void RemoveBinary()
    {
        Guard(); var state = Inspect();
        if (!state.CanInstall || state.BinaryHash != Payload.SysHash)
            throw new InvalidOperationException("Driver file still has ownership or its identity changed.");
        string path = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "drivers", "HidHide.sys");
        ProtectedJournal.RejectReparsePath(path);
        // One fixed, hash-verified file only, after all registrations are gone.
        OwnedDriverFile.Remove(path);
        if (File.Exists(path)) throw new IOException("Owned driver file removal did not verify.");
    }
    sealed class Devices : IDisposable
    {
        public IntPtr Handle { get; }
        public Devices(IntPtr handle) { if (handle == new IntPtr(-1)) throw Error("Open device information set"); Handle = handle; }
        public static Devices All() => new(SetupDiGetClassDevsW(IntPtr.Zero, null, IntPtr.Zero, 4)); // Includes non-present nodes.
        public void Dispose() { SetupDiDestroyDeviceInfoList(Handle); }
    }
    [StructLayout(LayoutKind.Sequential)] struct DeviceData
    {
        public uint Size; public Guid ClassGuid; public uint DevInst; public UIntPtr Reserved;
        public static DeviceData New() => new() { Size = (uint)Marshal.SizeOf(typeof(DeviceData)) };
    }
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true, ExactSpelling = true)] static extern IntPtr SetupDiGetClassDevsW(IntPtr guid, string? enumerator, IntPtr parent, uint flags);
    [DllImport("setupapi.dll", SetLastError = true)] static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
    [DllImport("setupapi.dll", SetLastError = true)] static extern bool SetupDiEnumDeviceInfo(IntPtr set, uint index, ref DeviceData data);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true, ExactSpelling = true)] static extern bool SetupDiGetDeviceRegistryPropertyW(IntPtr set, ref DeviceData data, uint property, out uint type, byte[] buffer, uint capacity, out uint needed);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true, ExactSpelling = true)] static extern bool SetupDiGetDeviceInstanceIdW(IntPtr set, ref DeviceData data, StringBuilder value, uint capacity, out uint needed);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true, ExactSpelling = true)] static extern bool SetupCopyOEMInfW(string source, string media, uint mediaType, uint flags, StringBuilder destination, uint capacity, out uint needed, IntPtr component);
    [DllImport("setupapi.dll", SetLastError = true)] static extern IntPtr SetupDiCreateDeviceInfoList(ref Guid guid, IntPtr parent);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true, ExactSpelling = true)] static extern bool SetupDiCreateDeviceInfoW(IntPtr set, string name, ref Guid guid, string description, IntPtr parent, uint flags, ref DeviceData data);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true, ExactSpelling = true)] static extern bool SetupDiSetDeviceRegistryPropertyW(IntPtr set, ref DeviceData data, uint property, byte[] bytes, uint size);
    [DllImport("setupapi.dll", SetLastError = true)] static extern bool SetupDiCallClassInstaller(uint function, IntPtr set, ref DeviceData data);
    [DllImport("cfgmgr32.dll")] static extern uint CM_Get_DevNode_Status(out uint status, out uint problem, uint node, uint flags);
    [DllImport("newdev.dll", CharSet = CharSet.Unicode, SetLastError = true, ExactSpelling = true)] static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr parent, string hardwareId, string inf, uint flags, out bool reboot);
    [DllImport("newdev.dll", SetLastError = true)] static extern bool DiUninstallDevice(IntPtr parent, IntPtr set, ref DeviceData data, uint flags, out bool reboot);
    [DllImport("newdev.dll", CharSet = CharSet.Unicode, SetLastError = true, ExactSpelling = true)] static extern bool DiUninstallDriverW(IntPtr parent, string inf, uint flags, out bool reboot);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool DeviceIoControl(SafeFileHandle handle, uint code, IntPtr input, uint inputSize, byte[]? output, uint outputSize, out uint used, IntPtr overlap);
    [DllImport("kernel32.dll", EntryPoint = "DeviceIoControl", SetLastError = true)] static extern bool WriteDeviceIoControl(SafeFileHandle handle, uint code, byte[] input, uint inputSize, IntPtr output, uint outputSize, out uint used, IntPtr overlap);
}
