using System.Runtime.Serialization;
using System.Security.Cryptography;
using HidHide.Installer;

namespace HidHide.DriverSetup;

[DataContract]
public sealed class DriverSettings
{
    [DataMember] public byte[] Whitelist { get; set; } = Array.Empty<byte>();
    [DataMember] public byte[] Blacklist { get; set; } = Array.Empty<byte>();
    [DataMember] public bool Active { get; set; }
    [DataMember] public bool Inverse { get; set; }
    public bool Same(DriverSettings other) => other != null && Active == other.Active && Inverse == other.Inverse &&
        SettingsCodec.Decode(Whitelist).SequenceEqual(SettingsCodec.Decode(other.Whitelist)) &&
        SettingsCodec.Decode(Blacklist).SequenceEqual(SettingsCodec.Decode(other.Blacklist));
}
[DataContract]
public sealed class FilterState
{
    [DataMember] public bool Exists { get; set; }
    [DataMember] public string[] Entries { get; set; } = Array.Empty<string>();
    public bool Same(FilterState other) => Exists == other.Exists && Entries.SequenceEqual(other.Entries);
}
[DataContract]
public sealed class Node
{
    [DataMember] public string Id { get; set; } = "";
    [DataMember] public string Inf { get; set; } = "";
    [DataMember] public string Service { get; set; } = "";
    [DataMember] public uint Problem { get; set; }
}
[DataContract]
public sealed class DriverState
{
    [DataMember] public Node[] Nodes { get; set; } = Array.Empty<Node>();
    [DataMember] public string[] Packages { get; set; } = Array.Empty<string>();
    [DataMember] public FilterState[] Filters { get; set; } = Array.Empty<FilterState>();
    [DataMember] public bool ServiceExists { get; set; }
    [DataMember] public bool ServicePendingDeletion { get; set; }
    [DataMember] public string BinaryHash { get; set; } = "";
    [DataMember] public bool ControlAvailable { get; set; }
    [DataMember] public DriverSettings? Settings { get; set; }
    public bool Empty => Nodes.Length == 0 && Packages.Length == 0 && !ServiceExists && BinaryHash == "" && !Filters.Any(x => x.Entries.Any(DriverFilters.IsHidHide));
    // Upstream uninstall can leave the exact signed SYS with no remaining
    // registration. SetupAPI may reuse that identical file; unknown bytes block.
    public bool CanInstall => Nodes.Length == 0 && Packages.Length == 0 && !ServiceExists && !ControlAvailable &&
        (BinaryHash == "" || BinaryHash == Payload.SysHash) && !Filters.Any(x => x.Entries.Any(DriverFilters.IsHidHide));
    public bool CoreHealthy => Nodes.Length == 1 && Nodes[0].Problem == 0 && Packages.Length == 1 &&
        string.Equals(Nodes[0].Inf, Packages[0], StringComparison.OrdinalIgnoreCase) &&
        string.Equals(Nodes[0].Service, "HidHide", StringComparison.OrdinalIgnoreCase) &&
        ServiceExists && !ServicePendingDeletion && BinaryHash == Payload.SysHash && ControlAvailable;
    public bool Healthy => CoreHealthy && Filters.All(x => x.Entries.Any(DriverFilters.IsHidHide));
    // Exactly the owned signed driver, possibly with its device disabled
    // (CM_PROB_DISABLED, so no control device): uninstall may remove it.
    public bool Removable => CoreHealthy || Nodes.Length == 1 && Nodes[0].Problem == 22 && Packages.Length == 1 &&
        string.Equals(Nodes[0].Inf, Packages[0], StringComparison.OrdinalIgnoreCase) &&
        string.Equals(Nodes[0].Service, "HidHide", StringComparison.OrdinalIgnoreCase) &&
        ServiceExists && !ServicePendingDeletion && BinaryHash == Payload.SysHash;
    // Inspection independently verifies every surviving INF and service path.
    // Missing resources can be reconstructed additively; unknown bytes cannot.
    public bool Repairable => !ServicePendingDeletion && Filters.Length == 3 && Nodes.Length <= 1 && Packages.Length <= 1 &&
        (BinaryHash == "" || BinaryHash == Payload.SysHash) && (!ServiceExists || Settings != null) &&
        Nodes.All(x => (x.Service == "" || string.Equals(x.Service, "HidHide", StringComparison.OrdinalIgnoreCase)) &&
            (x.Inf == "" || Packages.Length == 0 || string.Equals(x.Inf, Packages[0], StringComparison.OrdinalIgnoreCase)));
}

public static class Payload
{
    public const string InfHash = "38BF0541D798991A893E5DDAC37430D097361726A00CD714C287DA7A774722DF";
    public const string SysHash = "E3BB8B6A4E4FF90290B34E710EDBEF88CF71CEC6BA0CADCB8E1685D5DD65DDB2";
    public const string CatHash = "0E4209963524163B3B75302C60F7B8A965FC349ADA5752B74452AD59D417E877";
    public const string LicenseHash = "62356AD799305776DDD2FDF7B178EC9755E17ED980DA560E3887B883465825E0";
    public static string Hash(string path)
    {
        using var input = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        if (input.Length > 16 * 1024 * 1024) throw new InvalidDataException("Oversized driver payload.");
        using var sha = SHA256.Create();
        return BitConverter.ToString(sha.ComputeHash(input)).Replace("-", "");
    }
    public static readonly (string Name, string Hash)[] Files = { ("HidHide.inf", InfHash), ("HidHide.sys", SysHash), ("hidhide.cat", CatHash), ("LICENSE.rtf", LicenseHash) };
    public static void Verify(string directory)
    {
        foreach (var entry in Files)
        {
            var path = Path.Combine(directory, entry.Item1);
            ProtectedJournal.RejectReparsePath(path);
            if (Hash(path) != entry.Item2) throw new InvalidDataException("Pinned driver payload mismatch: " + entry.Item1);
        }
        // Build acquisition verifies catalog trust and every member. At runtime
        // these exact bytes are rechecked and Windows SetupAPI enforces signing.
    }
    public static string PublishedInf(string name)
    {
        if (!System.Text.RegularExpressions.Regex.IsMatch(name, @"\Aoem[0-9]{1,8}\.inf\z", System.Text.RegularExpressions.RegexOptions.IgnoreCase))
            throw new InvalidDataException("Not an exact published OEM INF identity.");
        return name;
    }
}

public interface IDriverBackend
{
    DriverState Inspect();
    string Stage();
    string CreateNode();
    bool Bind();
    bool RepairBind();
    void SetFilter(int index, FilterState expected, FilterState desired);
    bool RemoveNode(string id);
    bool RemovePackage(string publishedInf);
    void RemoveBinary();
}
