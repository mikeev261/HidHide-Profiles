using System.Text;

namespace HidHide.DriverSetup;

public static class SettingsCodec
{
    public static string[] Decode(byte[] bytes)
    {
        if (bytes == null || bytes.Length < 2 || bytes.Length > 1024 * 1024 || bytes.Length % 2 != 0)
            throw new InvalidDataException("Invalid driver list size.");
        var chars = new char[bytes.Length / 2];
        for (int i = 0; i < chars.Length; i++) chars[i] = (char)(bytes[i * 2] | bytes[i * 2 + 1] << 8);
        if (chars[chars.Length - 1] != '\0' || (chars.Length > 1 && chars[chars.Length - 2] != '\0'))
            throw new InvalidDataException("Driver list is not terminated.");
        var result = new List<string>(); int start = 0;
        while (start < chars.Length && chars[start] != '\0')
        {
            int end = Array.IndexOf(chars, '\0', start);
            if (end < 0 || end - start > 32767 || result.Count == 4096) throw new InvalidDataException("Driver list exceeds contract.");
            result.Add(new string(chars, start, end - start)); start = end + 1;
        }
        if (chars.Skip(start).Any(x => x != '\0') || result.Distinct(StringComparer.Ordinal).Count() != result.Count)
            throw new InvalidDataException("Malformed or duplicate driver list entry.");
        return result.OrderBy(x => x, StringComparer.Ordinal).ToArray();
    }
    public static byte[] Encode(IEnumerable<string> entries)
    {
        var values = entries.ToArray();
        if (values.Length > 4096 || values.Any(x => string.IsNullOrEmpty(x) || x.Length > 32767 || x.Contains('\0')) || values.Distinct(StringComparer.Ordinal).Count() != values.Length)
            throw new InvalidDataException("Invalid settings list.");
        var text = string.Join("\0", values.OrderBy(x => x, StringComparer.Ordinal)) + "\0\0";
        if (text.Length > 512 * 1024) throw new InvalidDataException("Settings list exceeds contract.");
        var bytes = new byte[text.Length * 2];
        for (int i = 0; i < text.Length; i++) { bytes[2 * i] = (byte)text[i]; bytes[2 * i + 1] = (byte)(text[i] >> 8); }
        return bytes;
    }
    public static void Validate(DriverSettings settings)
    {
        if (settings == null) throw new InvalidDataException("Missing driver settings.");
        Decode(settings.Whitelist); Decode(settings.Blacklist);
    }
}

public enum SettingsField { Active, Whitelist, Blacklist, Inverse }
public static class SettingsRestoration
{
    // The production caller retains one exclusive driver handle for this entire
    // operation. An IOCTL error leaves the durable restoration intent incomplete;
    // the caller must never blindly replay it over an unknown partial state.
    public static void Apply(DriverSettings expected, DriverSettings desired, Func<DriverSettings> read, Action<SettingsField, byte[]> write)
    {
        SettingsCodec.Validate(expected); SettingsCodec.Validate(desired);
        var actual = read();
        // Already restored (for example by an earlier attempt that failed before
        // it could commit): nothing to write, and retrying must not fail.
        if (actual.Same(desired)) return;
        if (!actual.Same(expected)) throw new InvalidOperationException("Settings changed before baseline restoration.");
        if (actual.Active) write(SettingsField.Active, new byte[] { 0 });
        if (!SettingsCodec.Decode(actual.Whitelist).SequenceEqual(SettingsCodec.Decode(desired.Whitelist))) write(SettingsField.Whitelist, desired.Whitelist);
        if (!SettingsCodec.Decode(actual.Blacklist).SequenceEqual(SettingsCodec.Decode(desired.Blacklist))) write(SettingsField.Blacklist, desired.Blacklist);
        if (actual.Inverse != desired.Inverse) write(SettingsField.Inverse, new byte[] { desired.Inverse ? (byte)1 : (byte)0 });
        if (desired.Active) write(SettingsField.Active, new byte[] { 1 });
        if (!read().Same(desired)) throw new InvalidOperationException("Driver did not confirm baseline restoration.");
    }
}
