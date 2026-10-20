// logistics/Pricing.cpp -- the barter price model + its per-(follower, vendor) session cache. See Pricing.h.
#include "PCH.h"
#include "logistics/Pricing.h"
#include "logistics/TradePerks.h"
#include "Config.h"
#include "MainThread.h"
#include <chrono>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace MFO::Logistics::Pricing {

    namespace {
        using Clock = std::chrono::steady_clock;

        // Refresh window. Speech changes are caught EXACTLY (the cached Speech is compared on every Resolve);
        // a PERK change has no cheap signal here, so the window bounds it. Perks change on a level-up or a
        // board allocation (minutes apart), a trade cadence is >= 8 s per follower, and the vendor-side
        // conditions are fixed per pair, so 60 s keeps a new perk at most one minute late. An UNLOADED follower
        // cannot refresh (R0-4) and keeps his last value for the session.
        constexpr auto kRefreshWindow = std::chrono::seconds(60);

        struct Entry { Quote q; Clock::time_point at{}; };

        std::mutex                                  g_mx;
        std::unordered_map<std::uint64_t, Entry>    g_cache;     // (follower, vendorKey) -> last main-thread quote
        std::unordered_set<std::uint64_t>           g_pending;   // a refresh is already Posted for this pair
        std::unordered_set<std::uint64_t>           g_missLogged;// the no-cache line was written for this pair
        std::atomic<bool>                           g_gmstWarned{ false };

        std::uint64_t Key(RE::FormID a_f, RE::FormID a_v) { return (static_cast<std::uint64_t>(a_f) << 32) | a_v; }

        struct Gmst { float barterMax, barterMin, barterBuyMin, barterSellMax, minBuyMult, maxSellMult; };

        // Every constant is a GMST, read by name at use (LoreRim / Requiem may override any of them).
        bool ReadGmst(Gmst& out) {
            auto* gsc = RE::GameSettingCollection::GetSingleton();
            if (!gsc) return false;
            auto get = [&](const char* a_name, float& a_out) {
                auto* st = gsc->GetSetting(a_name);
                if (!st) {
                    if (!g_gmstWarned.exchange(true))
                        spdlog::error("[econ] price model OFF: GMST {} unreadable -- buy and sell use the base value "
                                      "until it reads", a_name);
                    return false;
                }
                a_out = st->GetFloat();
                return true;
            };
            return get("fBarterMax", out.barterMax) && get("fBarterMin", out.barterMin) &&
                   get("fBarterBuyMin", out.barterBuyMin) && get("fBarterSellMax", out.barterSellMax) &&
                   get("fMinBuyMult", out.minBuyMult) && get("fMaxSellMult", out.maxSellMult);
        }

        float ReadSpeech(RE::Actor* a_f) {
            auto* avo = a_f ? a_f->AsActorValueOwner() : nullptr;
            return avo ? std::clamp(avo->GetActorValue(RE::ActorValue::kSpeech), 0.0f, 100.0f) : 0.0f;
        }

        // The pure model. a_buyPerk / a_sellPerk: the entry-point results, or negative = not evaluated
        // (the seed stays).
        Quote Model(const Gmst& g, float a_speech, const float* a_buyPerk, const float* a_sellPerk) {
            Quote q;
            q.speech = a_speech;
            std::string clamps;
            float buy = g.barterMax - (g.barterMax - g.barterMin) * a_speech * 0.01f;
            if (buy < g.barterBuyMin) { buy = g.barterBuyMin; clamps += "buy>=fBarterBuyMin "; }
            float sell = (buy > 0.0f) ? 1.0f / buy : g.barterSellMax;
            if (sell > g.barterSellMax) { sell = g.barterSellMax; clamps += "sell<=fBarterSellMax "; }
            q.buySeed = buy; q.sellSeed = sell;
            if (a_buyPerk)  buy  = *a_buyPerk;
            if (a_sellPerk) sell = *a_sellPerk;
            q.buyPerk = buy; q.sellPerk = sell;
            const float buyFloor = std::max(g.minBuyMult, 1.0f);
            if (buy < buyFloor) { buy = buyFloor; clamps += "buy>=max(fMinBuyMult,1.0) "; }
            const float sellCap = std::min(g.maxSellMult, 1.0f);
            if (sell > sellCap) { sell = sellCap; clamps += "sell<=min(fMaxSellMult,1.0) "; }
            q.buy = buy; q.sell = sell;
            q.note = clamps;
            return q;
        }

        // MAIN THREAD. The perk-evaluated quote for one pair, stored in the cache.
        void RefreshOnMain(RE::ActorHandle a_fh, RE::ActorHandle a_vh, RE::FormID a_fid, RE::FormID a_vkey) {
            const auto key = Key(a_fid, a_vkey);
            auto fp = a_fh.get();
            auto vp = a_vh.get();
            auto* f = fp.get();
            auto* v = vp.get();
            Gmst g{};
            // the handle may now resolve to another actor (a reused slot): never cache under the captured id then
            if (!f || f->GetFormID() != a_fid || !ReadGmst(g)) { std::scoped_lock lk(g_mx); g_pending.erase(key); return; }
            const float speech = ReadSpeech(f);
            std::optional<float> bp, sp;
            std::string why;
            if (!v) {
                why = "perks NOT evaluated: no vendor actor loaded (the entry points need the Target)";
            } else {
                const Gmst& gg = g;
                // seeds are the Speech factors (R0-2), not 1.0
                const Quote seedQ = Model(gg, speech, nullptr, nullptr);
                bp = TradePerks::EntryPointValue(RE::BGSEntryPoint::ENTRY_POINT::kModBuyPrices, f, v, seedQ.buySeed);
                sp = TradePerks::EntryPointValue(RE::BGSEntryPoint::ENTRY_POINT::kModSellPrices, f, v, seedQ.sellSeed);
                if (!bp || !sp) why = "perks NOT evaluated: the entry-point query refused (see the [trade] line)";
            }
            const float b = bp ? *bp : 0.0f, s = sp ? *sp : 0.0f;
            Quote q = Model(g, speech, (bp && sp) ? &b : nullptr, (bp && sp) ? &s : nullptr);
            q.perksEvaluated = bp && sp;
            q.source = "fresh";
            if (!why.empty()) q.note = why + (q.note.empty() ? "" : "; " + q.note);
            {
                std::scoped_lock lk(g_mx);
                g_cache[key] = Entry{ q, Clock::now() };
                g_pending.erase(key);
                g_missLogged.erase(key);
            }
            spdlog::info("[econ] {:08X} @ {:08X}: price refreshed on main -- {}", a_fid, a_vkey, Describe(q));
        }
    }

    Quote Resolve(RE::ActorHandle a_follower, RE::ActorHandle a_vendor, RE::FormID a_vendorKey) {
        Quote off;
        if (!Config::g_speechPricing.load()) { off.source = "off"; off.note = "bSpeechPricing = 0"; return off; }
        Gmst g{};
        if (!ReadGmst(g)) { off.modelOn = false; off.source = "gmst-unreadable"; return off; }

        auto fp = a_follower.get();
        auto* f = fp.get();
        const RE::FormID fid = f ? f->GetFormID() : 0u;
        const float speechNow = f ? ReadSpeech(f) : -1.0f;
        const auto key = Key(fid, a_vendorKey);
        const auto now = Clock::now();

        bool wantRefresh = false;
        bool pendingNow = false;   // a refresh for this pair is queued (by this call or an earlier one)
        Quote out;
        bool haveEntry = false;
        {
            std::scoped_lock lk(g_mx);
            auto it = g_cache.find(key);
            if (it != g_cache.end()) {
                haveEntry = true;
                out = it->second.q;
                const bool speechMoved = speechNow >= 0.0f && std::fabs(speechNow - it->second.q.speech) >= 0.5f;
                const bool aged = now - it->second.at >= kRefreshWindow;
                if (speechMoved || aged) { wantRefresh = true; out.source = "cache-stale"; }
                else out.source = "cache";
            } else {
                wantRefresh = true;
            }
            if (wantRefresh && f && f->Is3DLoaded() && MainThread::IsInstalled()) {
                pendingNow = true;
                if (!g_pending.insert(key).second) wantRefresh = false;   // already queued; post below otherwise
            } else {
                wantRefresh = false;
            }
        }
        if (wantRefresh)
            MainThread::Post([fh = a_follower, vh = a_vendor, fid, vk = a_vendorKey] { RefreshOnMain(fh, vh, fid, vk); });

        if (haveEntry) return out;

        // No cached perk value: the base Speech-only factor, said once per pair until a value is cached.
        Quote q = Model(g, speechNow >= 0.0f ? speechNow : 0.0f, nullptr, nullptr);
        q.source = "speech-only";
        q.refreshPending = pendingNow;
        q.note = std::string(f ? (f->Is3DLoaded() ? "no cached value yet (main-thread refresh queued)"
                                                  : "follower not loaded and no cached value")
                               : "no follower") + (q.note.empty() ? "" : "; " + q.note);
        bool first;
        { std::scoped_lock lk(g_mx); first = g_missLogged.insert(key).second; }
        if (first)
            spdlog::info("[econ] {:08X} @ {:08X}: no cached perk-adjusted price -- base Speech-only factor used -- {}",
                         fid, a_vendorKey, Describe(q));
        return q;
    }

    std::int32_t Apply(std::int32_t a_base, float a_mult) {
        if (a_base <= 0) return 0;
        return static_cast<std::int32_t>(std::lround(static_cast<double>(a_base) * static_cast<double>(a_mult)));
    }

    std::string Describe(const Quote& q) {
        return std::format("source={} speech={:.0f} | buy: factor {:.3f} -> perk-adjusted {:.3f} -> final {:.3f} | "
                           "sell: factor {:.3f} -> perk-adjusted {:.3f} -> final {:.3f} | perks {} | clamps [{}] | "
                           "rounding std::lround per unit, base x multiplier",
                           q.source, q.speech, q.buySeed, q.buyPerk, q.buy, q.sellSeed, q.sellPerk, q.sell,
                           q.perksEvaluated ? "evaluated" : "NOT evaluated", q.note);
    }

    void Clear() {
        std::scoped_lock lk(g_mx);
        g_cache.clear();
        g_pending.clear();
        g_missLogged.clear();
    }
}
