using System.Diagnostics;
using HidHide.DriverSetup;

// ProtectedJournal.EnsureProtectedDirectory with test trust and creation rules
// (a ".trusted" marker instead of the administrator-only ACL): the handling of
// pre-created paths is independent of elevation.
static class ProtectedDirectoryChecks
{
    static int checks;
    static void Check(bool value, string name) { if (!value) throw new Exception(name); checks++; }

    public static int Run()
    {
        var root = Path.Combine(Path.GetTempPath(), "HidHide-ProtectedDirectory-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            var moved = new List<string>();
            bool Trusted(string path) => (File.GetAttributes(path) & (FileAttributes.Directory | FileAttributes.ReparsePoint)) == FileAttributes.Directory && File.Exists(Path.Combine(path, ".trusted"));
            bool Create(string path)
            {
                if (Directory.Exists(path) || File.Exists(path)) return false;
                Directory.CreateDirectory(path); File.WriteAllText(Path.Combine(path, ".trusted"), ""); return true;
            }
            void Ensure(string path) => ProtectedJournal.EnsureProtectedDirectory(path, Trusted, Create, moved.Add);
            string[] Aside(string path) => Directory.GetFileSystemEntries(root, Path.GetFileName(path) + ".untrusted-*");

            var missing = Path.Combine(root, "missing"); Ensure(missing);
            Check(Trusted(missing) && moved.Count == 0, "a missing directory is created protected");

            var trusted = Path.Combine(root, "trusted"); Create(trusted); File.WriteAllText(Path.Combine(trusted, "journal.xml"), "kept");
            Ensure(trusted);
            Check(File.ReadAllText(Path.Combine(trusted, "journal.xml")) == "kept" && moved.Count == 0, "a trusted directory and its contents are kept");

            // #19: another user pre-created the directory before setup ran.
            var squatted = Path.Combine(root, "squatted"); Directory.CreateDirectory(squatted); File.WriteAllText(Path.Combine(squatted, "planted.txt"), "evidence");
            bool secured = true; try { Ensure(squatted); } catch (UnauthorizedAccessException) { secured = false; }
            Check(secured && Trusted(squatted) && !File.Exists(Path.Combine(squatted, "planted.txt")), "a pre-created untrusted directory is replaced by a protected one");
            Check(Aside(squatted).Length == 1 && File.ReadAllText(Path.Combine(Aside(squatted)[0], "planted.txt")) == "evidence" && moved.Contains(Aside(squatted)[0]), "the untrusted directory is preserved aside as evidence");

            var file = Path.Combine(root, "file"); File.WriteAllText(file, "squat");
            Ensure(file);
            Check(Trusted(file) && Aside(file).Length == 1 && File.ReadAllText(Aside(file)[0]) == "squat", "a file at the directory path is moved aside");

            var target = Path.Combine(root, "elsewhere"); Directory.CreateDirectory(target); File.WriteAllText(Path.Combine(target, "victim.txt"), "untouched");
            var junction = Path.Combine(root, "junction");
            using (var mklink = Process.Start(new ProcessStartInfo("cmd.exe", $"/c mklink /J \"{junction}\" \"{target}\"") { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true })!)
                mklink.WaitForExit();
            Check((File.GetAttributes(junction) & FileAttributes.ReparsePoint) != 0, "test junction created");
            Ensure(junction);
            Check(Trusted(junction) && File.ReadAllText(Path.Combine(target, "victim.txt")) == "untouched" && !File.Exists(Path.Combine(target, ".trusted")), "a junction is moved aside without touching its target");
            return checks;
        }
        finally
        {
            foreach (var entry in Directory.GetFileSystemEntries(root))
                if ((File.GetAttributes(entry) & FileAttributes.ReparsePoint) != 0) Directory.Delete(entry);
            try { Directory.Delete(root, true); } catch (IOException) { }
        }
    }
}
