// MFO item catalog — the analysis the DLL cannot do at runtime.
//
// Runtime SKSE code only sees an item's live form; guessing its category from
// MGEF archetypes is fragile the moment an overhaul (Requiem/CACO) reworks how
// potions are built. Here, with Mutagen, we read the ACTUAL records — every
// effect, flag, slot, enchantment — and emit a flat FormKey->category catalog
// the DLL loads and trusts. Adding a new loot category = add a section here and
// a lookup in the DLL.
//
// Output schema (mfo_items.json), each entry = { plugin, id } + category fields:
//   potions[] : restores = health|stamina|magicka  (optional)
//               cures    = poison|disease|both      (optional; cure potions)
//               a potion row appears if it restores a resource OR cures -- some
//               do both, some only cure (no restore ActorValue at all).
//   ammo[]    : kind     = arrow|bolt
//   jewelry[] : (amulets + rings)
//   soulgems[]: (every SLGM record — the DLL treats membership as the category)
//   exclude[] : why      = quest|unique|script   (never auto-loot these)
//               quest/unique stand alone; "script" only fires when a scripted
//               item ALSO carries another special signal (quest, unique
//               enchant, non-playable flag, or template link) — a benign VMAD
//               on normal gear no longer excludes it.
//   All rows carry name/value for auditability; the DLL ignores them.
// The DLL resolves each with LookupForm<T>(id, plugin) (MAO's mao_tiers scheme).

using System.Text.Json;
using System.Text.Json.Serialization;
using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Cache;
using Mutagen.Bethesda.Plugins.Order;
using Mutagen.Bethesda.Skyrim;

static class Catalog
{
    // One catalog row. id is the local FormID as 0xRRRRRR; plugin is the master
    // file it lives in — exactly what RE::TESDataHandler::LookupForm(id, plugin)
    // wants at runtime.
    sealed record Entry(string plugin, string id,
                        string? restores = null, string? kind = null, string? why = null,
                        string? name = null, uint? value = null, string? cures = null);

    static string Id(FormKey fk) => $"0x{fk.ID:X6}";
    // Canonical LOWERCASE plugin name. Mutagen can stringify the SAME master with
    // different casing across records (seen on the cc* files under a case-sensitive
    // FS: "ccBGSSSE037-Curios.esl" vs "ccbgssse037-curios.esl"), which the engine's
    // case-insensitive LookupForm tolerates but which leaves the catalog with
    // duplicate-cased plugin strings. Lowercasing makes the output deterministic;
    // LookupForm resolves it identically (proven in the field: mixed-case rows all
    // resolved, 0 unresolved).
    static string Plugin(FormKey fk) => fk.ModKey.FileName.ToString().ToLowerInvariant();

    public static int Write(
        ILoadOrderGetter<IModListingGetter<ISkyrimModGetter>> lo, ILinkCache cache, string outPath)
    {
        var potions  = new List<Entry>();
        var ammo     = new List<Entry>();
        var jewelry  = new List<Entry>();
        var soulgems = new List<Entry>();
        var exclude  = new List<Entry>();
        var excluded = new HashSet<FormKey>();

        // ── EXCLUSION signals gathered first ────────────────────────────────
        // (a) Enchantment uniqueness: an object-effect used by ONE item is an
        // artifact/unique; used by many is a generic disenchantable enchant.
        var enchUse = new Dictionary<FormKey, int>();
        void Tally(IFormLinkGetter<IObjectEffectGetter> ench)
        {
            if (!ench.IsNull) enchUse[ench.FormKey] = enchUse.GetValueOrDefault(ench.FormKey) + 1;
        }
        foreach (var w in lo.PriorityOrder.Weapon().WinningOverrides()) Tally(w.ObjectEffect);
        foreach (var a in lo.PriorityOrder.Armor().WinningOverrides())  Tally(a.ObjectEffect);

        // (b) Quest items: base objects a quest alias force-creates/points at.
        var questItems = new HashSet<FormKey>();
        foreach (var q in lo.PriorityOrder.Quest().WinningOverrides())
            foreach (var al in q.Aliases)
                if (al.CreateReferenceToObject is { } cro && !cro.Object.IsNull)
                    questItems.Add(cro.Object.FormKey);

        // Shared exclusion test for any enchantable gear (weapon/armor).
        // A script alone is NOT enough — plenty of normal gear carries a benign
        // VMAD. "script" only fires when the script is corroborated by another
        // special signal (`special` = non-playable flag or template link; quest
        // and unique-enchant hits are labelled by their own branches first).
        void MaybeExclude(FormKey fk, bool scripted, bool special,
                          IFormLinkGetter<IObjectEffectGetter> ench,
                          string? name, uint value)
        {
            string? why = null;
            if (questItems.Contains(fk))                                           why = "quest";
            else if (!ench.IsNull && enchUse.GetValueOrDefault(ench.FormKey) <= 1) why = "unique";
            else if (scripted && special)                                          why = "script";
            if (why != null && excluded.Add(fk))
                exclude.Add(new Entry(Plugin(fk), Id(fk), why: why, name: name, value: value));
        }

        static bool Scripted(IHaveVirtualMachineAdapterGetter? o)
            => o?.VirtualMachineAdapter is { } vm && vm.Scripts.Count > 0;

        // ── POTIONS ─────────────────────────────────────────────────────────
        foreach (var p in lo.PriorityOrder.Ingestible().WinningOverrides())
        {
            if (p.Flags.HasFlag(Ingestible.Flag.FoodItem)) continue;   // food, not a potion
            var restores = ClassifyRestore(p, cache);
            var cures    = ClassifyCure(p, cache);
            if (restores != null || cures != null)
                potions.Add(new Entry(Plugin(p.FormKey), Id(p.FormKey), restores: restores,
                                      cures: cures, name: p.Name?.String, value: p.Value));
        }

        // ── AMMO ────────────────────────────────────────────────────────────
        foreach (var am in lo.PriorityOrder.Ammunition().WinningOverrides())
        {
            if (am.Flags.HasFlag(Ammunition.Flag.NonPlayable)) continue;
            // NonBolt set => arrow; clear => crossbow bolt (matches TESAmmo::IsBolt).
            var kind = am.Flags.HasFlag(Ammunition.Flag.NonBolt) ? "arrow" : "bolt";
            ammo.Add(new Entry(Plugin(am.FormKey), Id(am.FormKey), kind: kind,
                               name: am.Name?.String, value: am.Value));
        }

        // ── SOUL GEMS ───────────────────────────────────────────────────────
        // Membership IS the category: the DLL only asks "is this FormID a soul
        // gem?" (its As<TESSoulGem>() fallback covers uncatalogued mod gems).
        // Quest gems (Azura's Star et al.) are alias-flagged and land in
        // exclude[] via the shared quest-item signal below.
        foreach (var sg in lo.PriorityOrder.SoulGem().WinningOverrides())
        {
            if (questItems.Contains(sg.FormKey))
            {
                if (excluded.Add(sg.FormKey))
                    exclude.Add(new Entry(Plugin(sg.FormKey), Id(sg.FormKey), why: "quest",
                                          name: sg.Name?.String, value: sg.Value));
                continue;
            }
            soulgems.Add(new Entry(Plugin(sg.FormKey), Id(sg.FormKey),
                                   name: sg.Name?.String, value: sg.Value));
        }

        // ── ARMOR: jewellery classification + exclusions ────────────────────
        foreach (var a in lo.PriorityOrder.Armor().WinningOverrides())
        {
            bool special = a.MajorFlags.HasFlag(Armor.MajorFlag.NonPlayable)
                        || (a.BodyTemplate?.Flags.HasFlag(BodyTemplate.Flag.NonPlayable) ?? false)
                        || !a.TemplateArmor.IsNull;
            MaybeExclude(a.FormKey, Scripted(a), special, a.ObjectEffect,
                         a.Name?.String, a.Value);
            if (IsJewelry(a) && !excluded.Contains(a.FormKey))
                jewelry.Add(new Entry(Plugin(a.FormKey), Id(a.FormKey),
                                      name: a.Name?.String, value: a.Value));
        }

        // ── WEAPONS: exclusions only (looted as "equipment") ────────────────
        foreach (var w in lo.PriorityOrder.Weapon().WinningOverrides())
        {
            // NON-PLAYABLE = creature/automaton gear (a Dwarven Sphere's built-in
            // crossbow, a dragon's bite, etc.) with no humanoid model. Loot it onto
            // a follower and it is INVISIBLE (they still FIRE it) -- field-caught: MFO
            // looted a "Dwarven Sphere Crossbow" off an automaton corpse onto two
            // followers. Exclude on its OWN signal, not only when scripted.
            if (w.MajorFlags.HasFlag(Weapon.MajorFlag.NonPlayable))
            {
                if (excluded.Add(w.FormKey))
                    exclude.Add(new Entry(Plugin(w.FormKey), Id(w.FormKey), why: "nonplayable",
                                          name: w.Name?.String, value: w.BasicStats?.Value ?? 0));
                continue;
            }
            MaybeExclude(w.FormKey, Scripted(w), /*special:*/ !w.Template.IsNull, w.ObjectEffect,
                         w.Name?.String, w.BasicStats?.Value ?? 0);
        }

        // ── emit ────────────────────────────────────────────────────────────
        var model = new
        {
            schema = 1,
            potions,
            ammo,
            jewelry,
            soulgems,
            exclude,
        };
        // Omit null category fields (WhenWritingNull): a potion row that only
        // restores no longer carries "cures": null, and vice versa. The DLL reads
        // every field with a defaulted lookup, so an ABSENT key is safe -- but a
        // present JSON null would throw its string conversion. This also keeps the
        // catalog small and readable.
        var json = JsonSerializer.Serialize(model, new JsonSerializerOptions
        {
            WriteIndented = true,
            DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        });
        File.WriteAllText(outPath, json);

        Console.WriteLine($"[MFO] catalog written: {potions.Count} potions, {ammo.Count} ammo, "
                        + $"{jewelry.Count} jewellery, {soulgems.Count} soul gems, "
                        + $"{exclude.Count} excluded -> {outPath}");
        return 0;
    }

    // A potion RESTORES resource X if it carries a beneficial, NON-recovering
    // effect on Health/Stamina/Magicka. Beneficial (not Detrimental, not Hostile)
    // rules out poisons; NOT-Recover rules out FORTIFY (a temporary buff whose
    // value is taken back when it ends) — leaving true restores, instant or over
    // time. The dominant (largest-magnitude) such effect names the potion.
    // A potion CURES if any of its effects is a CurePoison or CureDisease
    // archetype -- read from the magic effect record, so a modded cure potion
    // (LoreRim/CACO) classifies the same as a vanilla one. These archetypes
    // carry no restore ActorValue, so ClassifyRestore misses them entirely; the
    // "cure poison / disease" gambit needs them recognised on their own.
    static string? ClassifyCure(IIngestibleGetter p, ILinkCache cache)
    {
        bool poison = false, disease = false;
        foreach (var e in p.Effects)
        {
            if (!e.BaseEffect.TryResolve(cache, out var m)) continue;
            switch (m.Archetype.Type)
            {
                case MagicEffectArchetype.TypeEnum.CurePoison:  poison  = true; break;
                case MagicEffectArchetype.TypeEnum.CureDisease: disease = true; break;
            }
        }
        return (poison, disease) switch
        {
            (true,  true)  => "both",
            (true,  false) => "poison",
            (false, true)  => "disease",
            _              => null,
        };
    }

    static string? ClassifyRestore(IIngestibleGetter p, ILinkCache cache)
    {
        string? best = null;
        float bestMag = -1f;
        foreach (var e in p.Effects)
        {
            if (!e.BaseEffect.TryResolve(cache, out var m)) continue;
            if (m.Flags.HasFlag(MagicEffect.Flag.Detrimental) ||
                m.Flags.HasFlag(MagicEffect.Flag.Hostile) ||
                m.Flags.HasFlag(MagicEffect.Flag.Recover)) continue;
            var res = m.Archetype.ActorValue switch
            {
                ActorValue.Health  => "health",
                ActorValue.Stamina => "stamina",
                ActorValue.Magicka => "magicka",
                _ => null,
            };
            if (res == null) continue;
            var mag = e.Data?.Magnitude ?? 0f;
            if (mag > bestMag) { bestMag = mag; best = res; }
        }
        return best;
    }

    // Jewellery = worn on the amulet or ring biped slot, and NOTHING else. Some
    // mods also flag a body/head/etc. armour piece with a jewellery bit (field
    // audit caught "Vampire Lord Armor", "Xivkyn Armor", "Knight of Zenithar
    // Helmet" landing in jewellery); that is armour borrowing the flag, not
    // jewellery, so an item that ALSO occupies a real armour slot is rejected. A
    // true amulet/ring occupies ONLY its jewellery slot. (Circlets are head armour.)
    static bool IsJewelry(IArmorGetter a)
    {
        var f = a.BodyTemplate?.FirstPersonFlags ?? default;
        if (!f.HasFlag(BipedObjectFlag.Amulet) && !f.HasFlag(BipedObjectFlag.Ring))
            return false;
        const BipedObjectFlag armourSlots =
            BipedObjectFlag.Head | BipedObjectFlag.Hair | BipedObjectFlag.Body |
            BipedObjectFlag.Hands | BipedObjectFlag.Forearms | BipedObjectFlag.Feet |
            BipedObjectFlag.Calves | BipedObjectFlag.Shield;
        return (f & armourSlots) == 0;
    }
}
