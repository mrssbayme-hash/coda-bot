// Frame Window Counter: replays your macro and measures how many physics steps of room every click
// and release has, like NaN GD's and Antic's counters. replay.cpp does the measuring; this file keeps
// the results and draws them (on screen while counting, and in the Coda Bot window).
#include "config.hpp"
#include "shared.hpp"
#include <imgui.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>

using namespace geode::prelude;

namespace {
    std::vector<fwc::Window> s_res;
    bool s_counting = false;
    bool s_done = false, s_completed = false;
    int s_tps = 240;
    double s_lastAt = -10.0;            // when the last input was measured (for the pop-up number)

    struct Bucket { char const* label; float lo, hi; ImU32 color; };
    // NaN-style rows, tightest at the bottom. Window sizes are in 240 Hz frames.
    Bucket const BUCKETS[] = {
        {"9-12", 8.5f, 12.5f, IM_COL32(178, 120, 255, 255)},
        {"6-8", 5.5f, 8.5f, IM_COL32(110, 170, 255, 255)},
        {"5", 4.5f, 5.5f, IM_COL32(80, 220, 235, 255)},
        {"4", 3.5f, 4.5f, IM_COL32(110, 230, 120, 255)},
        {"3", 2.5f, 3.5f, IM_COL32(250, 225, 80, 255)},
        {"2", 1.5f, 2.5f, IM_COL32(255, 150, 60, 255)},
        {"1", 0.f, 1.5f, IM_COL32(255, 70, 85, 255)},
    };
    int const NB = (int)(sizeof BUCKETS / sizeof *BUCKETS);

    int bucketOf(fwc::Window const& w) {
        if (w.unknown) return -1;
        for (int i = 0; i < NB; i++) if (w.frames >= BUCKETS[i].lo && w.frames < BUCKETS[i].hi) return i;
        return -1;   // more than 12 frames: easy, not counted
    }
    ImU32 colorOf(fwc::Window const& w) {
        int b = bucketOf(w);
        return b < 0 ? IM_COL32(220, 220, 220, 255) : BUCKETS[b].color;
    }
    std::string sizeText(fwc::Window const& w) {
        if (w.unknown) return "?";
        char b[32];
        bool atLeast = w.cappedEarly || w.cappedLate;
        if (w.frames >= 12.5f) std::snprintf(b, sizeof b, "12+");
        else std::snprintf(b, sizeof b, "%s%g", atLeast ? ">=" : "", std::round(w.frames * 10.f) / 10.f);
        return b;
    }
    std::array<int, 7> counts() {
        std::array<int, 7> c{};
        for (auto& w : s_res) { int b = bucketOf(w); if (b >= 0) c[b]++; }
        return c;
    }
    ImVec2 toScreen(CCPoint world) {
        auto& io = ImGui::GetIO();
        auto ws = CCDirector::get()->getWinSize();
        return ImVec2(world.x / ws.width * io.DisplaySize.x, io.DisplaySize.y - world.y / ws.height * io.DisplaySize.y);
    }
    std::filesystem::path dir() {
        auto s = Mod::get()->getSaveDir().string();
        std::filesystem::path p(std::u8string(s.begin(), s.end()));
        return p / "macros";
    }
}

namespace fwc {
    bool counting() { return s_counting; }
    int tps() { return s_tps; }
    std::vector<Window> const& results() { return s_res; }

    void begin(int tps) { s_res.clear(); s_counting = true; s_done = false; s_completed = false; s_tps = tps; s_lastAt = -10.0; }
    void add(Window const& w) { s_res.push_back(w); s_lastAt = hacks::now(); }
    void finish(bool completed) {
        if (!s_counting) return;
        s_counting = false; s_done = true; s_completed = completed;
        auto c = counts();
        int tight = c[NB - 1] + c[NB - 2];
        menu::notify((completed ? "Counted the whole level: " : "Counted up to here: ") + std::to_string(s_res.size()) +
                     " inputs, " + std::to_string(tight) + " of them 1-2 frames.");
    }
    void cancel() { s_counting = false; s_done = false; s_res.clear(); }

    bool saveReport(std::string const& name) {
        std::error_code ec;
        std::filesystem::create_directories(dir(), ec);
        std::ofstream out(dir() / (name + "-windows.txt"));
        if (!out) return false;
        auto c = counts();
        out << "Coda Frame Window Counter\nMacro: " << name << "\nPhysics: " << s_tps << " TPS (windows in 240 Hz frames)\n";
        out << (s_completed ? "Counted the whole level\n" : "Counted part of the level\n") << "\n";
        for (int i = 0; i < NB; i++) out << BUCKETS[i].label << " frames: " << c[i] << "\n";
        int unknown = 0; for (auto& w : s_res) unknown += w.unknown;
        out << "Couldn't measure: " << unknown << "\n\n%\ttype\twindow\tms\tearly\tlate\n";
        for (auto& w : s_res) {
            out << std::fixed;
            out.precision(2); out << w.pct << "%\t" << (w.down ? "click" : "release") << "\t" << sizeText(w) << "\t";
            if (w.unknown) out << "-\t-\t-\n";
            else { out.precision(2); out << w.frames * 1000.f / 240.f << "\t-" << w.early << "\t+" << w.late << "\n"; }
        }
        return (bool)out;
    }

    // ------------------------------------------------------------ on screen
    void drawOverlay() {
        auto pl = PlayLayer::get();
        if (!pl || (!s_counting && !s_done) || !V::fwcOverlay) return;
        auto& io = ImGui::GetIO();
        auto dl = ImGui::GetBackgroundDrawList();
        ImFont* font = ImGui::GetFont();
        float sc = V::fwcScale * io.DisplaySize.y / 720.f;
        float size = 30.f * sc, lh = size * 1.08f;
        auto c = counts();
        float x = 14.f * sc, y = io.DisplaySize.y * .12f;
        auto text = [&](float tx, float ty, float sz, ImU32 col, std::string const& s) {
            dl->AddText(font, sz, ImVec2(tx + 2.f * sc, ty + 2.f * sc), IM_COL32(0, 0, 0, 190), s.c_str());
            dl->AddText(font, sz, ImVec2(tx, ty), col, s.c_str());
        };
        for (int i = 0; i < NB; i++) {
            text(x, y, size, BUCKETS[i].color, std::string(BUCKETS[i].label) + ": " + std::to_string(c[i]));
            y += lh;
        }
        // the last input's numbers
        if (!s_res.empty()) {
            auto& w = s_res.back();
            y += lh * .3f;
            if (w.unknown) text(x, y, size * .7f, IM_COL32(210, 210, 210, 255), "last: couldn't measure");
            else {
                char b[64];
                std::snprintf(b, sizeof b, "%.2fms", w.frames * 1000.f / 240.f);
                text(x, y, size * .8f, IM_COL32(230, 230, 230, 255), b); y += lh * .8f;
                std::snprintf(b, sizeof b, "%dHz", (int)std::ceil(240.f / std::max(w.frames, .01f)));
                text(x, y, size * .8f, IM_COL32(230, 230, 230, 255), b); y += lh * .8f;
                std::snprintf(b, sizeof b, "-%d / +%d", w.early, w.late);
                text(x, y, size * .6f, IM_COL32(190, 190, 190, 255), b);
            }
        }
        // title
        char const* title = s_counting ? "CODA COUNTER" : (s_completed ? "CODA COUNTER - DONE" : "CODA COUNTER - STOPPED");
        ImVec2 ts = font->CalcTextSizeA(size * .6f, FLT_MAX, 0.f, title);
        text(io.DisplaySize.x / 2 - ts.x / 2, 8.f * sc, size * .6f, IM_COL32(246, 162, 181, 255), title);
        // the number popping up by the icon
        double age = hacks::now() - s_lastAt;
        if (!s_res.empty() && age < .7 && pl->m_player1 && pl->m_player1->getParent()) {
            auto& w = s_res.back();
            auto p = toScreen(pl->m_player1->getParent()->convertToWorldSpace(pl->m_player1->getPosition()));
            float a = std::clamp((float)(1.0 - age / .7), 0.f, 1.f);
            std::string s = sizeText(w);
            float sz = size * 1.1f;
            ImVec2 st = font->CalcTextSizeA(sz, FLT_MAX, 0.f, s.c_str());
            ImU32 col = colorOf(w);
            ImU32 colA = (col & 0x00FFFFFF) | ((ImU32)(255 * a) << 24);
            float px = p.x - st.x / 2, py = p.y - 70.f * sc - (float)age * 40.f * sc;
            dl->AddText(font, sz, ImVec2(px + 2, py + 2), IM_COL32(0, 0, 0, (int)(190 * a)), s.c_str());
            dl->AddText(font, sz, ImVec2(px, py), colA, s.c_str());
            dl->AddCircle(p, 26.f * sc, colA, 0, 3.f * sc);
        }
    }

    // ------------------------------------------------------------ in the Coda Bot window
    void drawPanel() {
        ImGui::TextDisabled("FRAME WINDOW COUNTER");
        ImGui::PushTextWrapPos(0.f);
        if (!s_counting && !s_done) ImGui::TextDisabled("Record a macro that beats the level (or load one), then press Count. Coda replays it and measures every click and release.");
        ImGui::PopTextWrapPos();
        if (s_counting) {
            if (ImGui::Button("Stop counting", ImVec2(-FLT_MIN, 0))) replay::stop();
            ImGui::Text("Counting... %zu inputs so far", s_res.size());
        } else if (ImGui::Button("Count frame windows", ImVec2(-FLT_MIN, 0))) replay::countWindows();
        if (s_res.empty()) { ImGui::Separator(); return; }
        auto c = counts();
        int unknown = 0, easy = 0;
        for (auto& w : s_res) { if (w.unknown) unknown++; else if (bucketOf(w) < 0) easy++; }
        if (ImGui::BeginTable("fwc", 2, ImGuiTableFlags_SizingStretchSame)) {
            for (int i = NB - 1; i >= 0; i--) {
                ImGui::TableNextColumn();
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(BUCKETS[i].color), "%s fr: %d", BUCKETS[i].label, c[i]);
            }
            ImGui::TableNextColumn();
            ImGui::TextDisabled("12+ fr: %d", easy);
            ImGui::EndTable();
        }
        if (unknown) ImGui::TextDisabled("Couldn't measure %d (moving objects or triggers)", unknown);
        // tightest inputs
        std::vector<fwc::Window const*> v;
        for (auto& w : s_res) if (!w.unknown) v.push_back(&w);
        std::sort(v.begin(), v.end(), [](auto a, auto b) { return a->frames < b->frames; });
        if (!v.empty()) {
            ImGui::TextDisabled("Tightest:");
            for (size_t i = 0; i < std::min<size_t>(5, v.size()); i++) {
                auto& w = *v[i];
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(colorOf(w)), "%s fr", sizeText(w).c_str());
                ImGui::SameLine();
                ImGui::Text("%s at %.2f%% (%.1fms)", w.down ? "click" : "release", w.pct, w.frames * 1000.f / 240.f);
            }
        }
        if (!s_counting && ImGui::Button("Save report", ImVec2(-FLT_MIN, 0))) {
            if (saveReport(replay::name())) menu::notify("Saved \"" + replay::name() + "-windows.txt\" in the macros folder");
            else menu::notify("Couldn't save the report");
        }
        ImGui::Separator();
    }
}
