// MFO.Synthesis — marth's Follower Overhaul load-order patcher, as a Synthesis
// patch. Vehicle copied from MAO.Synthesis/MEO.Synthesis (same pinned Mutagen).
//
// It does NOT edit records. It SCANS the winning load order and writes the item
// catalog the DLL loads:  Data/SKSE/Plugins/MFO/mfo_items.json — potion restore
// types, ammo class, jewellery, and the "never loot" exclusion list. Categorizing
// from real record data (Mutagen sees every effect/flag) is Requiem-proof, unlike
// the DLL's runtime archetype guess. Run once, and again after any load-order
// change (Catalog.Write is pure analysis; re-running is safe and idempotent).

using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Skyrim;
using Mutagen.Bethesda.Synthesis;

// Dev-only off-VFS run (no game/Synthesis) — see SelfTest.cs.
if (args.Length >= 1 && args[0] == "selftest") return SelfTest.Run(args);

return await SynthesisPipeline.Instance
    .AddPatch<ISkyrimMod, ISkyrimModGetter>(RunPatch)
    .SetTypicalOpen(GameRelease.SkyrimSE, "MFO - Patch.esp")
    .Run(args);

static void RunPatch(IPatcherState<ISkyrimMod, ISkyrimModGetter> state)
{
    // GUARD (MEO's hard-learned trap): Synthesis names a group's output plugin
    // after the GROUP. A group named "MFO" therefore outputs MFO.esp -- which
    // OVERWRITES the real MFO.esp (deleting MFO's forms + startup quest), while
    // the run still reports success. Refuse rather than silently kill the mod.
    if (state.PatchMod.ModKey.FileName.String.Equals("MFO.esp", StringComparison.OrdinalIgnoreCase))
        throw new Exception(
            "Your Synthesis group is named 'MFO', so its output plugin is MFO.esp and would " +
            "REPLACE the real MFO.esp (deleting MFO's forms and startup quest). Rename the " +
            "Synthesis group to anything else (e.g. 'Synthesis'), reinstall MFO.esp, and re-run.");

    // Pure JSON emitter: the output plugin stays empty. Flag it ESL so a stray
    // "MFO - Patch.esp" costs no load-order slot (mirrors MAO's IsSmallMaster).
    state.PatchMod.IsSmallMaster = true;

    var dir = Path.Combine(state.DataFolderPath, "SKSE", "Plugins", "MFO");
    Directory.CreateDirectory(dir);
    var outPath = Path.Combine(dir, "mfo_items.json");

    var rc = Catalog.Write(state.LoadOrder, state.LinkCache, outPath);
    if (rc != 0)
        throw new Exception($"MFO catalog write failed (rc={rc}) — see log above");
}
