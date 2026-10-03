using System.Runtime.InteropServices;
using System.Runtime.Serialization;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Xml;

namespace HidHide.DriverSetup;

public sealed class ProtectedJournal : ITransactionJournal
{
    public static readonly string Root = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "mikeev261", "HidHide", "Maintenance");
    const int Limit = 1024 * 1024;
    readonly Guid id;
    static readonly SecurityIdentifier Admins = new(WellKnownSidType.BuiltinAdministratorsSid, null);
    static readonly SecurityIdentifier System = new(WellKnownSidType.LocalSystemSid, null);
    public ProtectedJournal(Guid id)
    {
        if (id == Guid.Empty) throw new InvalidDataException("Empty transaction ID.");
        RequireAdministrator(); this.id = id;
        ValidateDirectory(Root);
    }
    public static void RequireAdministrator()
    {
        using var user = WindowsIdentity.GetCurrent();
        if (!new WindowsPrincipal(user).IsInRole(WindowsBuiltInRole.Administrator))
            throw new UnauthorizedAccessException("Driver maintenance requires an elevated administrator token.");
        if (!Environment.Is64BitProcess) throw new PlatformNotSupportedException("Driver maintenance requires a native x64 process.");
    }
    // Called only by the future elevated setup controller, never from MSI public
    // properties or an ordinary-user READY record. No caller-supplied paths.
    public static void CreateRoot()
    {
        RequireAdministrator();
        string current = Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData);
        RejectReparsePath(current);
        foreach (var part in new[] { "mikeev261", "HidHide", "Maintenance" })
        {
            current = Path.Combine(current, part);
            if (!Directory.Exists(current))
            {
                var acl = new DirectorySecurity(); acl.SetAccessRuleProtection(true, false); acl.SetOwner(Admins);
                foreach (var sid in new[] { Admins, System })
                    acl.AddAccessRule(new FileSystemAccessRule(sid, FileSystemRights.FullControl,
                        InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit, PropagationFlags.None, AccessControlType.Allow));
                Directory.CreateDirectory(current, acl);
            }
            ValidateDirectory(current);
        }
    }
    public static void RejectReparsePath(string path)
    {
        var current = Path.GetFullPath(path);
        while (!string.IsNullOrEmpty(current))
        {
            if ((File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0)
                throw new UnauthorizedAccessException("Reparse points are not allowed in maintenance paths.");
            current = Path.GetDirectoryName(current);
        }
    }
    public static void ValidateAcl(FileSystemSecurity acl)
    {
        var owner = (SecurityIdentifier)acl.GetOwner(typeof(SecurityIdentifier));
        if (!owner.Equals(Admins) && !owner.Equals(System)) throw new UnauthorizedAccessException("Unexpected maintenance owner.");
        if (!acl.AreAccessRulesProtected) throw new UnauthorizedAccessException("Maintenance ACL must be protected.");
        var rules = acl.GetAccessRules(true, true, typeof(SecurityIdentifier)).Cast<FileSystemAccessRule>().ToArray();
        if (rules.Length != 2 || rules.Any(x => x.AccessControlType != AccessControlType.Allow ||
            (!x.IdentityReference.Equals(Admins) && !x.IdentityReference.Equals(System)) || x.FileSystemRights != FileSystemRights.FullControl) ||
            !rules.Any(x => x.IdentityReference.Equals(Admins)) || !rules.Any(x => x.IdentityReference.Equals(System)))
            throw new UnauthorizedAccessException("Unexpected maintenance ACL; access refused.");
    }
    public static void ValidateDirectory(string path)
    { RejectReparsePath(path); ValidateAcl(Directory.GetAccessControl(path)); }
    string PathFor(string suffix) => Path.Combine(Root, id.ToString("D") + suffix);
    public static byte[] StateBytes(DriverState state) => Serialize(state);
    static byte[] Serialize<T>(T value)
    {
        using var bytes = new MemoryStream();
        new DataContractSerializer(typeof(T)).WriteObject(bytes, value);
        if (bytes.Length > Limit) throw new InvalidDataException("Oversized maintenance journal.");
        return bytes.ToArray();
    }
    public static void Validate(TransactionRecord record)
    {
        if (record.Schema is not (1 or 2) || record.Id == Guid.Empty || record.PayloadIdentity != Payload.InfHash ||
            !Enum.IsDefined(typeof(Operation), record.Operation) || !Enum.IsDefined(typeof(JournalStatus), record.Status) ||
            record.Steps == null || record.Steps.Count > 32 || record.Failure == null || record.Failure.Length > 256)
            throw new InvalidDataException("Unsupported maintenance journal.");
        _ = new SecurityIdentifier(record.InitiatingSid);
        if (record.Status == JournalStatus.RollbackRebootRequired && (!record.RollbackDirection || !record.Reboot) ||
            record.RollbackBinaryCompleted && !record.RollbackBinaryStarted ||
            record.RollbackBinaryStarted && !record.RollbackDirection ||
            record.Steps.Any(x => x != null && x.UndoStarted && (!x.Completed || !record.RollbackDirection)))
            throw new InvalidDataException("Invalid rollback direction or undo evidence.");
        if (record.Before == null || record.Before.Nodes == null || record.Before.Packages == null || record.Before.Filters == null ||
            record.Before.Nodes.Length > 1 || record.Before.Packages.Length > 1 || record.Before.Filters.Length != 3)
            throw new InvalidDataException("Invalid maintenance resource snapshot.");
        foreach (var inf in record.Before.Packages) Payload.PublishedInf(inf);
        foreach (var node in record.Before.Nodes)
        {
            if (node == null || node.Id == null || node.Id.Length > 512 || node.Id.IndexOf('\0') >= 0 ||
                !node.Id.StartsWith("ROOT\\", StringComparison.OrdinalIgnoreCase) || node.Inf == null || node.Service == null ||
                (node.Service != "" && !string.Equals(node.Service, "HidHide", StringComparison.OrdinalIgnoreCase)))
                throw new InvalidDataException("Invalid journal device identity.");
            if (node.Inf != "") Payload.PublishedInf(node.Inf);
        }
        // Empty identity is an initialization allowance, never completed work
        // or a restart checkpoint that may clear protected exclusion.
        if (record.Schema == 2 && string.IsNullOrEmpty(record.BootId) &&
            (record.Steps.Count != 0 || record.Status is not (JournalStatus.Prepared or JournalStatus.Committed)))
            throw new InvalidDataException("Missing recorded boot evidence for driver work.");
        // Schema 1 predates these members; schema 2 must explicitly persist them.
        // Preparation identity is never proof of the boot that performed work.
        if (record.Schema == 2 && (!record.HasCommitRebootEvidence || record.RestartAnchor == null ||
            string.IsNullOrEmpty(record.RestartAnchor) &&
            (record.Steps.Count != 0 || record.Status is not (JournalStatus.Prepared or JournalStatus.Committed))))
            throw new InvalidDataException("Missing schema-2 restart obligation or actual-work anchor.");
        if (!string.IsNullOrEmpty(record.BootId) && !BootIdentity.Valid(record.BootId)) throw new InvalidDataException("Invalid boot identity.");
        if (!string.IsNullOrEmpty(record.RestartAnchor) && !BootIdentity.Stable(record.RestartAnchor)) throw new InvalidDataException("Invalid restart anchor.");
        if (record.Schema == 1 && (!string.IsNullOrEmpty(record.RestartAnchor) || record.CommitRebootRequired || BootIdentity.Stable(record.BootId)))
            throw new InvalidDataException("Stable restart evidence requires journal schema 2.");
        if (record.Before.ControlAvailable && record.Before.Settings == null) throw new InvalidDataException("Missing confirmed driver settings backup.");
        if (record.Before.Settings != null)
            SettingsCodec.Validate(record.Before.Settings);
        foreach (var step in record.Steps)
        {
            if (step == null || !Enum.IsDefined(typeof(StepKind), step.Kind) || step.Identity == null || step.Identity.Length > 256)
                throw new InvalidDataException("Invalid maintenance step.");
            if (step.Kind == StepKind.Filter && (step.FilterIndex < 0 || step.FilterIndex > 2 || step.BeforeFilter == null || step.AfterFilter == null))
                throw new InvalidDataException("Invalid filter journal step.");
            if (step.Undone && !step.Completed) throw new InvalidDataException("Uncompleted step cannot be recorded as undone.");
            if (step.Completed && (step.Kind == StepKind.Stage || step.Kind == StepKind.RemovePackage)) Payload.PublishedInf(step.Identity);
        }
        foreach (var filter in record.Before.Filters.Concat(record.Steps.Where(x => x.Kind == StepKind.Filter).SelectMany(x => new[] { x.BeforeFilter!, x.AfterFilter! })))
        {
            if (filter == null || filter.Entries == null || (!filter.Exists && filter.Entries.Length != 0)) throw new InvalidDataException("Invalid filter snapshot.");
            HidHide.Installer.DriverFilters.Change(filter.Entries, false);
        }
    }
    public void Save(TransactionRecord record)
    {
        Validate(record); if (record.Id != id) throw new InvalidDataException("Transaction identity mismatch.");
        ValidateDirectory(Root);
        var target = PathFor(".xml"); var temp = PathFor(".pending");
        if (File.Exists(target)) { RejectReparsePath(target); ValidateAcl(File.GetAccessControl(target)); }
        // An existing pending file means a previous write was interrupted. Do
        // not truncate it or overwrite evidence of an uncertain transaction.
        var security = new FileSecurity(); security.SetAccessRuleProtection(true, false); security.SetOwner(Admins);
        foreach (var sid in new[] { Admins, System }) security.AddAccessRule(new FileSystemAccessRule(sid, FileSystemRights.FullControl, AccessControlType.Allow));
        var bytes = Serialize(record);
        using (var output = new FileStream(temp, FileMode.CreateNew, FileSystemRights.Write, FileShare.None, 4096, FileOptions.WriteThrough, security))
        { output.Write(bytes, 0, bytes.Length); output.Flush(true); }
        if (!MoveFileExW(temp, target, 1 | 8)) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "Journal atomic replacement failed.");
    }
    public TransactionRecord Load()
    {
        ValidateDirectory(Root); var path = PathFor(".xml");
        if (File.Exists(PathFor(".pending"))) throw new InvalidDataException("Interrupted journal replacement; explicit recovery required.");
        RejectReparsePath(path); ValidateAcl(File.GetAccessControl(path));
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        if (stream.Length > Limit) throw new InvalidDataException("Oversized maintenance journal.");
        using var xml = XmlReader.Create(stream, new XmlReaderSettings { DtdProcessing = DtdProcessing.Prohibit, XmlResolver = null, MaxCharactersInDocument = Limit });
        var record = (TransactionRecord)new DataContractSerializer(typeof(TransactionRecord)).ReadObject(xml)!;
        Validate(record); if (record.Id != id) throw new InvalidDataException("Transaction identity mismatch.");
        return record;
    }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, ExactSpelling = true)]
    static extern bool MoveFileExW(string source, string target, int flags);
}
