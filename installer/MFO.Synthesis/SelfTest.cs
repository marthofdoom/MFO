// Local self-test: resolve an MO2 modlist's load order off-VFS (Linux, no game),
// run Catalog.Write, and print category counts + a sample. Dev-only — the shipped
// path is the Synthesis pipeline in Program.cs. Mirrors MEO.Installer's manual
// MO2 resolution: filename -> file (any copy is byte-identical for a given plugin
// name), ORDER taken from the profile's loadorder.txt, ACTIVE set from plugins.txt.
//
//   dotnet run -c Release -- selftest <mo2Root> <profile> <outJson>

using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Order;
using Mutagen.Bethesda.Skyrim;

static class SelfTest
{
    public static int Run(string[] args)
    {
        if (args.Length < 4)
        {
            Console.Error.WriteLine("usage: selftest <mo2Root> <profile> <outJson>");
            return 2;
        }
        string mo2Root = args[1], profile = args[2], outJson = args[3];

        string profDir = Path.Combine(mo2Root, "profiles", profile);
        var order  = File.ReadAllLines(Path.Combine(profDir, "loadorder.txt"))
                         .Select(l => l.Trim()).Where(l => l.Length > 0 && !l.StartsWith('#')).ToList();
        var active = new HashSet<string>(
            File.ReadAllLines(Path.Combine(profDir, "plugins.txt"))
                .Select(l => l.Trim()).Where(l => l.Length > 0 && !l.StartsWith('#'))
                .Select(l => l.StartsWith('*') ? l[1..] : l),
            StringComparer.OrdinalIgnoreCase);

        // filename -> full path, scanning Stock Game/Data + every mods/* + overwrite.
        var files = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        void Scan(string dir)
        {
            if (!Directory.Exists(dir)) return;
            foreach (var f in Directory.EnumerateFiles(dir))
            {
                var ext = Path.GetExtension(f).ToLowerInvariant();
                if (ext is ".esm" or ".esp" or ".esl")
                    files.TryAdd(Path.GetFileName(f), f);
            }
        }
        Scan(Path.Combine(mo2Root, "Stock Game", "Data"));
        var modsDir = Path.Combine(mo2Root, "mods");
        if (Directory.Exists(modsDir))
            foreach (var m in Directory.EnumerateDirectories(modsDir)) Scan(m);
        Scan(Path.Combine(mo2Root, "overwrite"));

        var listings = new List<IModListingGetter<ISkyrimModGetter>>();
        int missing = 0;
        foreach (var name in order)
        {
            if (!active.Contains(name)) continue;
            if (!files.TryGetValue(name, out var full)) { missing++; continue; }
            var mod = SkyrimMod.CreateFromBinaryOverlay(
                new ModPath(ModKey.FromNameAndExtension(name), full), SkyrimRelease.SkyrimSE);
            listings.Add(new ModListing<ISkyrimModGetter>(mod, enabled: true));
        }
        Console.WriteLine($"[selftest] {listings.Count} plugins resolved, {missing} missing");

        var lo = new LoadOrder<IModListingGetter<ISkyrimModGetter>>(listings);
        var cache = lo.ToImmutableLinkCache();
        return Catalog.Write(lo, cache, outJson);
    }
}
