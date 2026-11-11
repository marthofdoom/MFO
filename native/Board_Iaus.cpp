#include "PCH.h"

// Board_Iaus.cpp -- the Gambits tab's IAUS "Thinking" panel (IAUS card I7). A READ-ONLY view of a follower's last
// combat decision from the IAUS source: what he is doing, the top candidates with their scores, and each one's
// consideration breakdown. RENDER THREAD, inside DrawFieldKit's Gambits tab.
//
// THREADING: the IAUS minds are worker-owned and are never read here. The worker publishes an immutable Readout copy
// per follower (scheduler/IausSource.cpp -> Iaus::Engine::Publish) and this reads it by value through
// Iaus::Engine::ReadReadout, which holds its own leaf mutex for the copy only. No lock is held across ImGui. The panel
// draws plain text (no focusable widget), so gamepad nav is exactly what it was without it.

#include <imgui.h>

#include "Board_internal.h"
#include "iaus/Engine.h"

namespace MFO::Board {

    namespace {
        // A readout older than this is a fight that ended (the worker republishes every IAUS service lap).
        constexpr double kStaleSec = 3.0;

        // FormatWhy writes "<label> = <score> | <term> | <term> ...". The label and score are drawn on their own line,
        // so the breakdown is everything after the first " | ".
        std::string_view Breakdown(const std::string& a_why) {
            const auto p = a_why.find(" | ");
            return p == std::string::npos ? std::string_view{} : std::string_view(a_why).substr(p + 3);
        }
    }

    void DrawIausThinking(RE::FormID a_follower, const MenuSkin& a_skin) {
        ImGui::PushStyleColor(ImGuiCol_Text, a_skin.accent);
        ImGui::TextUnformatted(Str::Get(Str::K::Gb_IausThinkTitle));
        ImGui::PopStyleColor();
        ImGui::TextDisabled("%s", Str::Get(Str::K::Gb_IausThinkNote));

        Iaus::Readout r;
        double age = 0.0;
        if (!Iaus::Engine::ReadReadout(a_follower, r, &age)) {
            ImGui::TextDisabled("%s", Str::Get(Str::K::Gb_IausThinkNone));
            ImGui::Separator();
            return;
        }
        if (age > kStaleSec)
            ImGui::TextDisabled("%s", Str::Fmt(Str::K::Gb_IausThinkStale, { static_cast<int>(age) }).c_str());

        switch (r.verdict) {
        case Iaus::Verdict::Act:
            ImGui::TextWrapped("%s", Str::Fmt(Str::K::Gb_IausThinkDoing, { r.pick }).c_str());
            break;
        case Iaus::Verdict::Hold:
            ImGui::TextWrapped("%s", Str::Fmt(Str::K::Gb_IausThinkHold, { r.pick }).c_str());
            break;
        default:
            ImGui::TextWrapped("%s", Str::Fmt(Str::K::Gb_IausThinkIdle, { Str::Arg::F(r.actFloor, 2) }).c_str());
            break;
        }
        ImGui::TextDisabled("%s", Str::Fmt(Str::K::Gb_IausThinkRank,
                                           { static_cast<int>(r.rapportRank), Str::Arg::F(r.runningSec, 0) }).c_str());

        for (std::uint8_t i = 0; i < r.topN; ++i) {
            const auto& row = r.top[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TextWrapped("%u. %.3f  %s", static_cast<unsigned>(i) + 1u, static_cast<double>(row.score), row.label.c_str());
            const std::string_view why = Breakdown(row.why);
            if (!why.empty()) {
                ImGui::Indent();
                ImGui::PushStyleColor(ImGuiCol_Text, a_skin.dim);
                ImGui::TextWrapped("%.*s", static_cast<int>(why.size()), why.data());
                ImGui::PopStyleColor();
                ImGui::Unindent();
            }
            ImGui::PopID();
        }
        ImGui::Separator();
    }

}
