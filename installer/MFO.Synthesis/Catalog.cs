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
//   potions[] : restores = health|stamina|magicka
//   ammo[]    : kind     = arrow|bolt
//   jewelry[] : (amulets + rings)
//   exclude[] : why      = quest|unique|script   (never auto-loot these)
//               quest/unique stand alone; "script" only fires when a scripted
//               item ALSO carries another special signal (quest, unique
//               enchant, non-playable flag, or template link) — a benign VMAD
//               on normal gear no longer excludes it.
//   All rows carry name/value for auditability; the DLL ignores them.
// The DLL resolves each with LookupForm<T>(id, plugin) (MAO's mao_tiers scheme).

using System.Text.Json;
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
                        string? name = null, uint? value = null);

    static string Id(FormKey fk) => $"0x{fk.ID:X6}";
    static string Plugin(FormKey fk) => fk.ModKey.FileName;

    public static int Write(
        ILoadOrderGetter<IModListingGetter<ISkyrimModGetter>> lo, ILinkCache cache, string outPath)
    {
        var potions = new List<Entry>();
        var ammo    = new List<Entry>();
        var jewelry = new List<Entry>();
        var exclude = new List<Entry>();
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
            if (restores != null)
                potions.Add(new Entry(Plugin(p.FormKey), Id(p.FormKey), restores: restores,
                                      name: p.Name?.String, value: p.Value));
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
            bool special = w.MajorFlags.HasFlag(Weapon.MajorFlag.NonPlayable)
                        || !w.Template.IsNull;
            MaybeExclude(w.FormKey, Scripted(w), special, w.ObjectEffect,
                         w.Name?.String, w.BasicStats?.Value ?? 0);
        }

        // ── emit ────────────────────────────────────────────────────────────
        var model = new
        {
            schema = 1,
            potions,
            ammo,
            jewelry,
            exclude,
        };
        var json = JsonSerializer.Serialize(model, new JsonSerializerOptions { WriteIndented = true });
        File.WriteAllText(outPath, json);

        Console.WriteLine($"[MFO] catalog written: {potions.Count} potions, {ammo.Count} ammo, "
                        + $"{jewelry.Count} jewellery, {exclude.Count} excluded -> {outPath}");
        return 0;
    }

    // A potion RESTORES resource X if it carries a beneficial, NON-recovering
    // effect on Health/Stamina/Magicka. Beneficial (not Detrimental, not Hostile)
    // rules out poisons; NOT-Recover rules out FORTIFY (a temporary buff whose
    // value is taken back when it ends) — leaving true restores, instant or over
    // time. The dominant (largest-magnitude) such effect names the potion.
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

    // Jewellery = worn on the amulet or ring biped slot. (Circlets are head
    // armour, not jewellery.) Reads the winning armour's body template.
    static bool IsJewelry(IArmorGetter a)
    {
        var f = a.BodyTemplate?.FirstPersonFlags ?? default;
        return f.HasFlag(BipedObjectFlag.Amulet) || f.HasFlag(BipedObjectFlag.Ring);
    }
}
