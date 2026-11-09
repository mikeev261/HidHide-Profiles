using HidHide.DriverSetup;

// PayloadCache with test pins and unprotected files: the repair logic is
// independent of the pinned upstream bytes and of administrator ACLs.
static class PayloadCacheChecks
{
    static int checks;
    static void Check(bool value, string name) { if (!value) throw new Exception(name); checks++; }

    public static int Run()
    {
        var root = Path.Combine(Path.GetTempPath(), "HidHide-PayloadCache-" + Guid.NewGuid().ToString("D"));
        try
        {
            string source = Path.Combine(root, "source"), target = Path.Combine(root, "payload");
            Directory.CreateDirectory(source); Directory.CreateDirectory(target);
            var contents = new Dictionary<string, string> { ["HidHide.inf"] = "inf bytes", ["HidHide.sys"] = new string('s', 70000), ["hidhide.cat"] = "catalog" };
            foreach (var file in contents) File.WriteAllText(Path.Combine(source, file.Key), file.Value);
            var pins = contents.Keys.Select(name => (name, Payload.Hash(Path.Combine(source, name)))).ToArray();
            var validated = new List<string>();
            void Prepare() => PayloadCache.Prepare(source, target, pins,
                path => new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None), path => validated.Add(Path.GetFileName(path)));
            bool Matches() => pins.All(pin => File.Exists(Path.Combine(target, pin.Item1)) && Payload.Hash(Path.Combine(target, pin.Item1)) == pin.Item2);

            Prepare();
            Check(Matches() && Directory.GetFiles(target).Length == pins.Length, "fresh cache receives exactly the pinned files");

            // #13: a copy interrupted by power loss or a killed setup leaves a
            // truncated file (and possibly a staging file) in the cache.
            var sys = Path.Combine(target, "HidHide.sys");
            using (var truncate = new FileStream(sys, FileMode.Open, FileAccess.Write)) truncate.SetLength(4096);
            File.WriteAllText(sys + ".partial", "stale staging");
            bool repaired = true; try { Prepare(); } catch (Exception) { repaired = false; }
            Check(repaired && Matches(), "a partially copied payload file is repaired from the verified source");
            Check(Directory.GetFiles(target).Length == pins.Length, "repair leaves no staging files");

            // A correct cache never needs the source.
            Directory.Move(source, source + ".moved");
            validated.Clear(); Prepare();
            Check(Matches() && validated.Count == pins.Length, "a correct cache is validated without reading the source");
            Directory.Move(source + ".moved", source);

            // A corrupt source is never copied over the cache.
            File.WriteAllText(Path.Combine(source, "HidHide.inf"), "tampered");
            File.Delete(Path.Combine(target, "hidhide.cat"));
            bool refused = false; try { Prepare(); } catch (InvalidDataException) { refused = true; }
            Check(refused && !File.Exists(Path.Combine(target, "hidhide.cat")) && Payload.Hash(Path.Combine(target, "HidHide.inf")) == pins[0].Item2, "a mismatched source is refused");
            return checks;
        }
        finally { try { Directory.Delete(root, true); } catch (IOException) { } }
    }
}
