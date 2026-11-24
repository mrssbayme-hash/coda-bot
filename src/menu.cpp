// The Coda menu: Mega Hack style windows drawn with ImGui, plus labels, toasts and keybinds.
#include "config.hpp"
#include "shared.hpp"
#include "fonts.hpp"
#include <imgui-cocos.hpp>
#include <imgui.h>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <map>
#include <ctime>
#include <cfloat>
#include <cstring>

using namespace geode::prelude;

namespace {
    bool s_open = false;
    float s_anim = 0.f;
    char s_search[64] = {};
    bool s_resetLayout = false;
    bool s_confirmReset = false;
    char s_macroName[64] = "my-macro";
    std::map<std::string, bool> s_expanded;
    std::map<std::string, std::vector<cfg::Field*>> s_kids;
    struct Toast { std::string text; double at; };
    std::deque<Toast> s_toasts;
    ImFont* s_body = nullptr;
    ImFont* s_title = nullptr;

    char const* const WINDOWS[] = {"Player", "Level", "Bypass", "Labels", "Coda Bot", "Frames", "Roulette", "Keybinds", "Coda"};

    ImU32 col(Col const& c, float alphaMul = 1.f) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a * alphaMul)); }
    ImVec4 v4(Col const& c, float a = -1.f) { return ImVec4(c.r, c.g, c.b, a < 0.f ? c.a : a); }
    ImVec4 mix(Col const& a, Col const& b, float k, float alpha = 1.f) {
        return ImVec4(a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k, a.b + (b.b - a.b) * k, alpha);
    }

    bool matches(cfg::Field const& f) {
        if (!s_search[0]) return true;
        std::string a = f.label, b = s_search;
        for (auto& c : a) c = (char)std::tolower((unsigned char)c);
        for (auto& c : b) c = (char)std::tolower((unsigned char)c);
        return a.find(b) != std::string::npos;
    }

    void buildKids() {
        s_kids.clear();
        for (auto& f : cfg::all()) if (!f.parent.empty()) s_kids[f.parent].push_back(&f);
    }

    // ------------------------------------------------------------ theme
    void applyStyle() {
        auto& st = ImGui::GetStyle();
        st.WindowRounding = V::rounding;
        st.ChildRounding = V::rounding;
        st.FrameRounding = V::rounding * .6f;
        st.PopupRounding = V::rounding * .6f;
        st.GrabRounding = V::rounding * .6f;
        st.WindowBorderSize = 2.f;
        st.FrameBorderSize = 0.f;
        st.WindowPadding = ImVec2(10, 8);
        st.ItemSpacing = ImVec2(6, 4);
        st.WindowTitleAlign = ImVec2(0.f, .5f);
        st.FontScaleMain = V::menuScale;
        auto& c = st.Colors;
        Col dark{0.137f, 0.125f, 0.11f, 1.f};
        c[ImGuiCol_Text] = v4(V::textCol);
        c[ImGuiCol_TextDisabled] = v4(V::textCol, .5f);
        c[ImGuiCol_WindowBg] = v4(V::bg);
        c[ImGuiCol_PopupBg] = v4(V::bg, 1.f);
        c[ImGuiCol_Border] = ImVec4(0.137f, 0.125f, 0.11f, 1.f);
        c[ImGuiCol_TitleBg] = v4(V::accent, 1.f);
        c[ImGuiCol_TitleBgActive] = v4(V::accent, 1.f);
        c[ImGuiCol_TitleBgCollapsed] = v4(V::accent, .8f);
        c[ImGuiCol_FrameBg] = mix(V::bg, V::textCol, .08f, 1.f);
        c[ImGuiCol_FrameBgHovered] = mix(V::bg, V::accent, .25f, 1.f);
        c[ImGuiCol_FrameBgActive] = mix(V::bg, V::accent, .4f, 1.f);
        c[ImGuiCol_SliderGrab] = v4(V::accent, 1.f);
        c[ImGuiCol_SliderGrabActive] = mix(V::accent, Col{1, 1, 1, 1}, .3f);
        c[ImGuiCol_CheckMark] = v4(V::accent, 1.f);
        c[ImGuiCol_Button] = mix(V::bg, V::textCol, .12f, 1.f);
        c[ImGuiCol_ButtonHovered] = mix(V::bg, V::accent, .45f, 1.f);
        c[ImGuiCol_ButtonActive] = v4(V::accent, 1.f);
        c[ImGuiCol_Header] = mix(V::bg, V::accent, .3f, 1.f);
        c[ImGuiCol_HeaderHovered] = mix(V::bg, V::accent, .45f, 1.f);
        c[ImGuiCol_HeaderActive] = v4(V::accent, 1.f);
        c[ImGuiCol_Separator] = mix(V::bg, V::textCol, .2f, 1.f);
        c[ImGuiCol_ScrollbarGrab] = mix(V::bg, V::accent, .5f, 1.f);
        c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_NavCursor] = v4(V::accent, 1.f);
        (void)dark;
    }

    // ------------------------------------------------------------ widgets
    void bindPopup(cfg::Field& f) {
        if (ImGui::BeginPopup("bind")) {
            ImGui::TextDisabled("Keybind for %s", f.label.c_str());
            bool waiting = cfg::waitingBind == &f;
            std::string label = waiting ? "Press a key... (Esc clears)" : cfg::keyName(f.bind);
            if (ImGui::Button(label.c_str(), ImVec2(200, 0))) cfg::waitingBind = &f;
            if (f.bind && ImGui::Button("Clear", ImVec2(200, 0))) { f.bind = 0; cfg::save(); }
            ImGui::EndPopup();
        } else if (cfg::waitingBind == &f) cfg::waitingBind = nullptr;
    }

    void drawField(cfg::Field& f, bool searching);

    void drawToggle(cfg::Field& f, bool searching) {
        bool& v = *(bool*)f.ptr;
        auto& kids = s_kids[f.key];
        bool hasKids = !kids.empty() && !searching;
        float h = ImGui::GetFrameHeight();
        float w = ImGui::GetContentRegionAvail().x - (hasKids ? h + 2.f : 0.f);
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("row", ImVec2(std::max(10.f, w), h));
        bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) { v = !v; cfg::changed(f); }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("bind");
        if (hovered && !f.tip.empty()) ImGui::SetTooltip("%s\nRight-click to set a keybind.", f.tip.c_str());
        else if (hovered) ImGui::SetTooltip("Right-click to set a keybind.");
        auto dl = ImGui::GetWindowDrawList();
        if (hovered) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(ImGuiCol_FrameBgHovered, .5f), 4.f);
        float fs = ImGui::GetFontSize();
        dl->AddText(ImVec2(p.x + 4.f, p.y + (h - fs) / 2.f), ImGui::GetColorU32(ImGuiCol_Text), f.label.c_str());
        // switch
        float sw = h * 1.7f, sh = h * .62f;
        ImVec2 a(p.x + w - sw - 4.f, p.y + (h - sh) / 2.f), b(a.x + sw, a.y + sh);
        dl->AddRectFilled(a, b, v ? col(V::accent, 1.f) : ImGui::GetColorU32(ImGuiCol_FrameBg), sh / 2.f);
        float r = sh / 2.f - 2.f;
        ImVec2 knob(v ? b.x - r - 2.f : a.x + r + 2.f, a.y + sh / 2.f);
        dl->AddCircleFilled(knob, r, v ? IM_COL32(35, 32, 28, 255) : ImGui::GetColorU32(ImGuiCol_TextDisabled));
        if (f.bind) {
            auto k = "[" + cfg::keyName(f.bind) + "]";
            auto ks = ImGui::CalcTextSize(k.c_str());
            dl->AddText(ImVec2(a.x - ks.x - 6.f, p.y + (h - fs) / 2.f), ImGui::GetColorU32(ImGuiCol_TextDisabled), k.c_str());
        }
        bindPopup(f);
        if (hasKids) {
            ImGui::SameLine(0.f, 2.f);
            bool& open = s_expanded[f.key];
            if (ImGui::ArrowButton("more", open ? ImGuiDir_Down : ImGuiDir_Right)) open = !open;
            if (open) {
                ImGui::Indent(10.f);
                for (auto k : kids) drawField(*k, false);
                ImGui::Unindent(10.f);
            }
        }
    }

    void drawField(cfg::Field& f, bool searching) {
        ImGui::PushID(f.key.c_str());
        switch (f.type) {
            case cfg::T::Bool: drawToggle(f, searching); break;
            case cfg::T::Int: {
                int& v = *(int*)f.ptr;
                if (f.typed) {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(f.label.c_str());
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    bool hit = ImGui::InputInt("##v", &v, 1, 60);
                    v = std::clamp(v, (int)f.mn, (int)f.mx);
                    if (ImGui::IsItemDeactivatedAfterEdit() || (hit && !ImGui::IsItemActive())) cfg::changed(f);
                } else {
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    std::string fmt = f.label + ": %d";
                    if (ImGui::SliderInt("##v", &v, (int)f.mn, (int)f.mx, fmt.c_str())) { if (f.onChange) f.onChange(); }
                    if (ImGui::IsItemDeactivatedAfterEdit()) cfg::changed(f);
                }
                break;
            }
            case cfg::T::Float: {
                if (f.typed) {
                    // a box you type in (with - and + buttons), kept between the field's limits
                    float& v = *(float*)f.ptr;
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(f.label.c_str());
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    bool hit = ImGui::InputFloat("##v", &v, .01f, .1f, "%.2f");
                    bool typing = ImGui::IsItemActive();
                    if (!typing) v = std::clamp(v, f.mn, f.mx);
                    if (hit && f.onChange && v >= f.mn && v <= f.mx) f.onChange();
                    if (ImGui::IsItemDeactivatedAfterEdit() || (hit && !typing)) { v = std::clamp(v, f.mn, f.mx); if (f.onChange) f.onChange(); cfg::changed(f); }
                    if (ImGui::IsItemHovered() && !f.tip.empty()) ImGui::SetTooltip("%s", f.tip.c_str());
                    break;
                }
                ImGui::SetNextItemWidth(-FLT_MIN);
                std::string fmt = f.label + ": " + f.fmt;
                if (ImGui::SliderFloat("##v", (float*)f.ptr, f.mn, f.mx, fmt.c_str())) { if (f.onChange) f.onChange(); }
                if (ImGui::IsItemDeactivatedAfterEdit()) cfg::changed(f);
                break;
            }
            case cfg::T::Color: {
                if (ImGui::ColorEdit4("##v", (float*)f.ptr, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) cfg::changed(f);
                ImGui::SameLine();
                ImGui::TextUnformatted(f.label.c_str());
                break;
            }
            case cfg::T::Choice: {
                int& v = *(int*)f.ptr;
                ImGui::SetNextItemWidth(-FLT_MIN);
                std::string preview = f.label + ": " + f.choices[std::clamp(v, 0, (int)f.choices.size() - 1)];
                if (ImGui::BeginCombo("##v", preview.c_str())) {
                    for (int i = 0; i < (int)f.choices.size(); i++)
                        if (ImGui::Selectable(f.choices[i].c_str(), v == i)) { v = i; cfg::changed(f); }
                    ImGui::EndCombo();
                }
                break;
            }
            case cfg::T::Action: break;
        }
        ImGui::PopID();
    }

    void drawFields(char const* win) {
        bool searching = s_search[0] != 0;
        for (auto& f : cfg::all()) {
            if (f.win != win) continue;
            if (searching) { if (matches(f)) drawField(f, true); }
            else if (f.parent.empty()) drawField(f, false);
        }
    }

    bool anyMatch(char const* win) {
        if (!s_search[0]) return true;
        for (auto& f : cfg::all()) if (f.win == win && matches(f)) return true;
        return false;
    }

    // ------------------------------------------------------------ special window parts
    void botControls() {
        auto st = replay::state();
        float w = (ImGui::GetContentRegionAvail().x - 8.f) / 3.f;
        ImGui::TextDisabled("REPLAY");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##name", "Macro name", s_macroName, sizeof s_macroName);
        ImGui::PushStyleColor(ImGuiCol_Button, st == replay::State::Recording ? ImVec4(.85f, .2f, .3f, 1.f) : ImVec4(.62f, .18f, .26f, 1.f));
        if (ImGui::Button(st == replay::State::Recording ? "Recording" : "Record", ImVec2(w, 0))) replay::record();
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 4);
        ImGui::PushStyleColor(ImGuiCol_Button, st == replay::State::Playing ? ImVec4(.2f, .7f, .4f, 1.f) : ImVec4(.18f, .5f, .32f, 1.f));
        if (ImGui::Button(st == replay::State::Playing ? "Playing" : "Play", ImVec2(w, 0))) replay::play();
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 4);
        if (ImGui::Button("Stop", ImVec2(w, 0))) replay::stop();
        if (ImGui::Button("Save", ImVec2(w, 0))) {
            if (replay::inputCount() == 0) menu::notify("Nothing recorded yet");
            else if (replay::save(s_macroName)) menu::notify("Saved \"" + replay::name() + "\"");
            else menu::notify("Couldn't save the macro");
        }
        ImGui::SameLine(0, 4);
        if (ImGui::Button("Load", ImVec2(w, 0))) ImGui::OpenPopup("load");
        ImGui::SameLine(0, 4);
        if (ImGui::Button("Clear", ImVec2(w, 0))) { replay::clear(); menu::notify("Macro cleared"); }
        if (ImGui::BeginPopup("load")) {
            auto names = replay::list();
            if (names.empty()) ImGui::TextDisabled("No saved macros yet");
            for (auto& n : names) {
                if (ImGui::Selectable(n.c_str())) {
                    if (replay::load(n)) {
                        std::snprintf(s_macroName, sizeof s_macroName, "%s", n.c_str());
                        menu::notify("Loaded \"" + n + "\" (" + std::to_string(replay::inputCount()) + " inputs)");
                    } else menu::notify("Couldn't load that macro");
                }
            }
            ImGui::EndPopup();
        }
        ImGui::TextDisabled("%zu inputs%s", replay::inputCount(), st == replay::State::Playing ? "" : "");
        ImGui::Separator();
    }

    // ------------------------------------------------------------ frame windows
    // how hard a click is, from how long its window lasts (so it means the same at any TPS)
    struct Grade { char const* name; ImVec4 color; };
    Grade grade(fwin::Result const& r) {
        double ms = r.window() * 1000.0 / std::max(1, r.tps);
        if (r.window() <= 1) return {"Frame perfect", ImVec4(1.f, .3f, .35f, 1.f)};
        if (ms <= 9.0) return {"Very hard", ImVec4(1.f, .5f, .3f, 1.f)};
        if (ms <= 13.0) return {"Hard", ImVec4(1.f, .72f, .3f, 1.f)};
        if (ms <= 22.0) return {"Tight", ImVec4(1.f, .9f, .4f, 1.f)};
        if (ms <= 40.0) return {"Okay", ImVec4(.6f, .9f, .5f, 1.f)};
        return {"Easy", ImVec4(.45f, .85f, 1.f, 1.f)};
    }
    std::string windowText(fwin::Result const& r) {
        if (r.died) return "you died right after";
        if (r.unknown) return "couldn't tell";
        char b[96];
        bool more = r.capEarly || r.capLate;
        std::snprintf(b, sizeof b, "%s%d frame%s (%.1f ms)", more ? "at least " : "", r.window(), r.window() == 1 ? "" : "s", r.window() * 1000.0 / std::max(1, r.tps));
        return b;
    }

    void framesWindow() {
        auto& res = fwin::results();
        int n = 0, fp = 0, unk = 0, died = 0;
        fwin::Result const* hardest = nullptr;
        for (auto& r : res) {
            if (r.died) { died++; continue; }
            if (r.unknown) { unk++; continue; }
            n++; if (r.window() <= 1) fp++;
            if (!hardest || r.window() < hardest->window()) hardest = &r;
        }
        ImGui::Separator();
        ImGui::PushTextWrapPos(0.f);
        if (!V::fwOn) ImGui::TextDisabled("Turn it on, then play. Each click gets a window: how many frames you could have clicked and still lived.");
        else if (res.empty()) ImGui::TextDisabled("Play, or play a macro from Coda Bot. Clicks show up here a moment after you make them.");
        ImGui::PopTextWrapPos();
        if (n) {
            ImGui::Text("%d clicks counted, %d frame perfect", n, fp);
            if (hardest) {
                auto g = grade(*hardest);
                ImGui::Text("Hardest:"); ImGui::SameLine();
                ImGui::TextColored(g.color, "%s at %.1f%%", windowText(*hardest).c_str(), hardest->pct);
            }
        }
        if (unk) ImGui::TextDisabled("%d couldn't be checked (moving parts or triggers)", unk);
        if (fwin::waiting()) ImGui::TextDisabled("Working on %d more...", fwin::waiting());
        float w = (ImGui::GetContentRegionAvail().x - 4.f) / 2.f;
        if (ImGui::Button("Clear list", ImVec2(w, 0))) fwin::clear();
        ImGui::SameLine(0, 4);
        if (ImGui::Button("Count my macro", ImVec2(w, 0))) {
            if (replay::inputCount() == 0) menu::notify("Load or record a macro in Coda Bot first");
            else { if (!V::fwOn) { V::fwOn = true; if (auto f = cfg::find("fw")) cfg::changed(*f); } fwin::clear(); replay::play(); }
        }
        if (res.empty()) return;
        ImGui::BeginChild("fwlist", ImVec2(0, 190.f * V::menuScale), ImGuiChildFlags_Borders);
        for (auto it = res.rbegin(); it != res.rend(); ++it) {
            auto& r = *it;
            ImGui::TextDisabled("%5.1f%%", r.pct); ImGui::SameLine();
            ImGui::TextUnformatted(r.down ? "click  " : "release"); ImGui::SameLine();
            if (r.died || r.unknown) { ImGui::TextDisabled("%s", windowText(r).c_str()); continue; }
            auto g = grade(r);
            ImGui::TextColored(g.color, "%d%s f", r.window(), (r.capEarly || r.capLate) ? "+" : "");
            ImGui::SameLine();
            ImGui::TextColored(g.color, "%s", g.name);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\n%d%s frame%s earlier and %d%s later would still work.\nPhysics frame %u.",
                    windowText(r).c_str(), r.early, r.capEarly ? "+" : "", r.early == 1 ? "" : "s", r.late, r.capLate ? "+" : "", r.frame);
        }
        ImGui::EndChild();
    }

    // the last click's window at the bottom of the screen
    void drawFrameWidget() {
        if (!V::fwOn || !V::fwScreen || !PlayLayer::get()) return;
        auto& res = fwin::results();
        if (res.empty()) return;
        auto& r = res.back();
        double age = hacks::now() - r.shownAt;
        float a = (float)std::clamp(4.0 - age, 0.0, 1.0);
        if (a <= 0.f) return;
        auto& io = ImGui::GetIO();
        auto dl = ImGui::GetBackgroundDrawList();
        ImFont* font = s_body ? s_body : ImGui::GetFont();
        float size = 17.f * V::lblScale;
        auto g = grade(r);
        std::string top = (r.down ? "Click: " : "Release: ") + windowText(r);
        std::string word = (r.died || r.unknown) ? "" : g.name;
        float cell = 11.f * V::lblScale, gap = 2.f;
        int K = r.K, cells = 2 * K + 1;
        float barW = cells * (cell + gap) - gap;
        ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.f, top.c_str());
        float boxW = std::max(barW, ts.x) + 24.f, boxH = size * 2.4f + cell + 14.f;
        float y0 = io.DisplaySize.y - boxH - (V::lblInputs ? 50.f * V::lblScale : 12.f);
        ImVec2 p0(io.DisplaySize.x / 2 - boxW / 2, y0), p1(p0.x + boxW, y0 + boxH);
        dl->AddRectFilled(p0, p1, IM_COL32(20, 16, 24, (int)(190 * a * V::lblOpacity)), 10.f);
        ImU32 gc = ImGui::ColorConvertFloat4ToU32(ImVec4(g.color.x, g.color.y, g.color.z, a));
        dl->AddRect(p0, p1, (r.died || r.unknown) ? IM_COL32(255, 255, 255, (int)(90 * a)) : gc, 10.f, 0, 2.f);
        dl->AddText(font, size, ImVec2(io.DisplaySize.x / 2 - ts.x / 2, p0.y + 6.f), IM_COL32(243, 236, 217, (int)(255 * a)), top.c_str());
        if (!word.empty()) {
            ImVec2 ws = font->CalcTextSizeA(size * .9f, FLT_MAX, 0.f, word.c_str());
            dl->AddText(font, size * .9f, ImVec2(io.DisplaySize.x / 2 - ws.x / 2, p0.y + 6.f + size * 1.15f), gc, word.c_str());
        }
        // the bar: each square is a frame; filled ones would still work; the outlined one is when you clicked
        float bx = io.DisplaySize.x / 2 - barW / 2, by = p1.y - cell - 8.f;
        for (int i = 0; i < cells; i++) {
            int k = i - K;
            bool ok = !r.died && !r.unknown && k >= -r.early && k <= r.late;
            ImVec2 c0(bx + i * (cell + gap), by), c1(c0.x + cell, c0.y + cell);
            dl->AddRectFilled(c0, c1, ok ? gc : IM_COL32(255, 255, 255, (int)(40 * a)), 2.f);
            if (k == 0) dl->AddRect(ImVec2(c0.x - 2, c0.y - 2), ImVec2(c1.x + 2, c1.y + 2), IM_COL32(255, 255, 255, (int)(230 * a)), 3.f, 0, 1.5f);
        }
    }

    void codaStatus() {
        auto cheats = cfg::cheatsInUse();
        if (bot::enabled()) cheats.push_back("Coda's pathfinder");
        if (cheats.empty()) {
            ImGui::TextColored(ImVec4(.45f, .9f, .55f, 1.f), "Coda Safe: your runs count");
        } else {
            ImGui::TextColored(ImVec4(1.f, .45f, .5f, 1.f), "Coda Safe: runs won't count");
            std::string list;
            for (size_t i = 0; i < cheats.size(); i++) list += (i ? ", " : "") + cheats[i];
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextDisabled("%s", list.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::Separator();
    }

    void codaExtras() {
        ImGui::Separator();
        if (ImGui::Button("Reset window layout", ImVec2(-FLT_MIN, 0))) s_resetLayout = true;
        if (!s_confirmReset) { if (ImGui::Button("Reset all settings", ImVec2(-FLT_MIN, 0))) s_confirmReset = true; }
        else {
            ImGui::TextColored(ImVec4(1.f, .5f, .55f, 1.f), "Reset everything?");
            if (ImGui::Button("Yes, reset", ImVec2(ImGui::GetContentRegionAvail().x / 2 - 2, 0))) { cfg::resetAll(); s_confirmReset = false; menu::notify("Settings reset"); }
            ImGui::SameLine(0, 4);
            if (ImGui::Button("Cancel", ImVec2(-FLT_MIN, 0))) s_confirmReset = false;
        }
    }

    void keyButton(cfg::Field& f) {
        ImGui::PushID(f.key.c_str());
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(f.label.c_str());
        ImGui::SameLine();
        float bw = 92.f * V::menuScale;
        float rem = ImGui::GetContentRegionAvail().x;
        if (rem > bw) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + rem - bw);
        bool waiting = cfg::waitingBind == &f;
        if (waiting) ImGui::PushStyleColor(ImGuiCol_Button, v4(V::accent, 1.f));
        if (ImGui::Button(waiting ? "Press a key" : cfg::keyName(f.bind).c_str(), ImVec2(bw, 0))) cfg::waitingBind = waiting ? nullptr : &f;
        if (waiting) ImGui::PopStyleColor();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { f.bind = 0; cfg::save(); }
        ImGui::PopID();
    }

    void keybindsWindow() {
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("Click a button, then press the key you want. Right-click a button to clear it. Esc clears too.");
        ImGui::PopTextWrapPos();
        bool searching = s_search[0] != 0;
        if (!searching) ImGui::SeparatorText("Menu and actions");
        for (auto& f : cfg::all()) if (f.type == cfg::T::Action && matches(f)) keyButton(f);
        for (char const* w : {"Player", "Level", "Bypass", "Coda Bot", "Frames", "Labels"}) {
            bool header = false;
            for (auto& f : cfg::all()) {
                if (f.win != w || f.type != cfg::T::Bool || !matches(f)) continue;
                if (!header && !searching) { ImGui::SeparatorText(w); header = true; }
                if (!f.parent.empty() && !searching) ImGui::Indent(10.f);
                keyButton(f);
                if (!f.parent.empty() && !searching) ImGui::Unindent(10.f);
            }
        }
    }

    // ------------------------------------------------------------ the menu
    void drawMenu() {
        auto& io = ImGui::GetIO();
        float scale = V::menuScale;
        float winW = 250.f * scale;
        bool closing = !s_open;
        // search bar
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x / 2.f, 14.f), ImGuiCond_Always, ImVec2(.5f, 0.f));
        ImGui::SetNextWindowSize(ImVec2(360.f * scale, 0.f));
        ImGuiWindowFlags barFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_AlwaysAutoResize | (closing ? ImGuiWindowFlags_NoInputs : 0);
        if (ImGui::Begin("##coda-search", nullptr, barFlags)) {
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##s", "Search every setting...", s_search, sizeof s_search);
        }
        ImGui::End();

        float x = 16.f, y = 70.f * scale, rowH = 0.f;
        for (int i = 0; i < (int)(sizeof WINDOWS / sizeof *WINDOWS); i++) {
            char const* name = WINDOWS[i];
            if (x + winW > io.DisplaySize.x - 8.f && i > 0) { x = 16.f; y += 340.f * scale; }
            ImVec2 def(x, y);
            x += winW + 12.f;
            if (!anyMatch(name) && std::strcmp(name, "Coda") != 0 && std::strcmp(name, "Coda Bot") != 0 && std::strcmp(name, "Keybinds") != 0 && std::strcmp(name, "Roulette") != 0 && std::strcmp(name, "Frames") != 0) continue;
            ImGui::SetNextWindowPos(def, s_resetLayout ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSizeConstraints(ImVec2(winW, 0.f), ImVec2(winW, io.DisplaySize.y - 20.f));
            bool kbWin = std::strcmp(name, "Keybinds") == 0;
            if (std::strcmp(name, "Roulette") == 0) ImGui::SetNextWindowSizeConstraints(ImVec2(winW * 1.4f, 0.f), ImVec2(winW * 1.4f, io.DisplaySize.y - 20.f));
            if (kbWin) ImGui::SetNextWindowSizeConstraints(ImVec2(winW * 1.15f, 120.f), ImVec2(winW * 1.15f, io.DisplaySize.y * .7f));
            ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | (closing ? ImGuiWindowFlags_NoInputs : 0);
            if (s_title) ImGui::PushFont(s_title, 0.f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.137f, .125f, .11f, 1.f));
            bool open = ImGui::Begin((std::string(name) + "##coda").c_str(), nullptr, flags);
            ImGui::PopStyleColor();
            if (s_title) ImGui::PopFont();
            if (open) {
                if (std::strcmp(name, "Coda Bot") == 0 && !s_search[0]) botControls();
                if (std::strcmp(name, "Coda") == 0 && !s_search[0]) codaStatus();
                if (std::strcmp(name, "Keybinds") == 0) keybindsWindow();
                else if (std::strcmp(name, "Roulette") == 0) roulette::drawWindow();
                else drawFields(name);
                if (std::strcmp(name, "Frames") == 0 && !s_search[0]) framesWindow();
                if (std::strcmp(name, "Bypass") == 0 && !hacks::tpsSupported())
                    ImGui::TextColored(ImVec4(1.f, .6f, .4f, 1.f), "TPS bypass needs Steam GD 2.2091.");
                if (std::strcmp(name, "Coda") == 0 && !s_search[0]) codaExtras();
            }
            ImGui::End();
            (void)rowH;
        }
        s_resetLayout = false;
    }

    // ------------------------------------------------------------ labels
    void drawLabels() {
        auto pl = PlayLayer::get();
        if (!pl || !V::labels) return;
        auto& io = ImGui::GetIO();
        auto& s = hacks::stats();
        std::vector<std::pair<std::string, ImU32>> lines;
        ImU32 base = col(V::lblColor, V::lblOpacity);
        ImU32 red = ImGui::ColorConvertFloat4ToU32(ImVec4(1.f, .4f, .45f, V::lblOpacity));
        char b[128];
        if (V::lblCodaSafe && (safe::cheatedThisRun() || bot::enabled())) lines.push_back({"Coda Safe: this run won't count", red});
        if (V::lblFps) { std::snprintf(b, sizeof b, "%d FPS", (int)(io.Framerate + .5f)); lines.push_back({b, base}); }
        if (V::lblTps) { std::snprintf(b, sizeof b, "%d TPS", hacks::currentTps()); lines.push_back({b, base}); }
        if (V::lblCps) {
            double t = hacks::now();
            while (!s.clicks.empty() && t - s.clicks.front() > 1.0) s.clicks.pop_front();
            std::snprintf(b, sizeof b, "%d CPS (%d total)", (int)s.clicks.size(), s.clicksTotal); lines.push_back({b, base});
        }
        if (V::noclip && V::lblNoclip) {
            double acc = s.totalFrames ? 100.0 * (1.0 - (double)s.deadFrames / (double)s.totalFrames) : 100.0;
            std::snprintf(b, sizeof b, "Noclip: %.2f%%", std::max(0.0, acc));
            lines.push_back({b, s.flash > 0.f ? red : base});
        }
        if (V::noclip && V::lblNoclipDeaths) { std::snprintf(b, sizeof b, "Noclip deaths: %d", s.noclipDeaths); lines.push_back({b, base}); }
        if (V::lblAttempts) { std::snprintf(b, sizeof b, "Attempt %d", pl->m_attempts); lines.push_back({b, base}); }
        if (V::lblSession) {
            int t = (int)hacks::now();
            std::snprintf(b, sizeof b, "Session %d:%02d:%02d", t / 3600, t / 60 % 60, t % 60); lines.push_back({b, base});
        }
        if (V::lblClock) {
            std::time_t t = std::time(nullptr); std::tm tm{};
#ifdef GEODE_IS_WINDOWS
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            int h = tm.tm_hour % 12; if (h == 0) h = 12;
            std::snprintf(b, sizeof b, "%d:%02d %s", h, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM"); lines.push_back({b, base});
        }
        if (V::lblBestRun && s.bestTo > 0.f) { std::snprintf(b, sizeof b, "Best run: %.0f%% to %.0f%%", s.bestFrom, s.bestTo); lines.push_back({b, base}); }
        if (V::lblStartpos && V::startposSwitcher && s.startposCount > 0) {
            if (s.startposIndex < 0) std::snprintf(b, sizeof b, "Start pos: start (%d)", s.startposCount);
            else std::snprintf(b, sizeof b, "Start pos: %d/%d", s.startposIndex + 1, s.startposCount);
            lines.push_back({b, base});
        }
        if (V::lblMacro) {
            auto st = replay::status();
            if (!st.empty()) lines.push_back({st, replay::state() == replay::State::Recording ? red : base});
            if (bot::enabled()) lines.push_back({"Coda is playing", col(V::accent, V::lblOpacity)});
            if (V::frameAdvance) lines.push_back({"Frame advance (" + cfg::keyName(cfg::find("frame-step") ? cfg::find("frame-step")->bind : 0) + " to step)", base});
        }
        auto dl = ImGui::GetBackgroundDrawList();
        ImFont* font = s_body ? s_body : ImGui::GetFont();
        float size = 17.f * V::lblScale;
        float lh = size * 1.18f, m = 8.f;
        bool right = V::lblCorner == 1 || V::lblCorner == 3, bottom = V::lblCorner >= 2;
        float y = bottom ? io.DisplaySize.y - m - lh * lines.size() : m;
        if (!bottom && !right && PlayLayer::get()) y += 0.f;
        for (auto& [text, c] : lines) {
            ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.f, text.c_str());
            float x = right ? io.DisplaySize.x - m - ts.x : m;
            if (V::lblShadow) dl->AddText(font, size, ImVec2(x + 1.5f, y + 1.5f), IM_COL32(0, 0, 0, (int)(170 * V::lblOpacity)), text.c_str());
            dl->AddText(font, size, ImVec2(x, y), c, text.c_str());
            y += lh;
        }
        if (V::lblInputs) {
            float bs = 26.f * V::lblScale;
            ImVec2 o(io.DisplaySize.x / 2.f - bs - 4.f, io.DisplaySize.y - bs - 12.f);
            for (int p = 0; p < 2; p++) {
                ImVec2 a(o.x + p * (bs + 8.f), o.y), z(a.x + bs, a.y + bs);
                dl->AddRectFilled(a, z, s.held[p] ? col(V::accent, V::lblOpacity) : IM_COL32(20, 16, 24, (int)(150 * V::lblOpacity)), 6.f);
                dl->AddRect(a, z, IM_COL32(255, 255, 255, (int)(200 * V::lblOpacity)), 6.f, 0, 2.f);
                char const* t = p ? "P2" : "P1";
                ImVec2 ts = font->CalcTextSizeA(size * .7f, FLT_MAX, 0.f, t);
                dl->AddText(font, size * .7f, ImVec2(a.x + (bs - ts.x) / 2, a.y + (bs - ts.y) / 2), IM_COL32(255, 255, 255, 230), t);
            }
        }
        if (s.flash > 0.f && V::noclip && V::noclipFlash) {
            auto c = V::noclipFlashColor;
            dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, col(c, s.flash));
        }
    }

    void drawToasts() {
        double t = hacks::now();
        while (!s_toasts.empty() && t - s_toasts.front().at > 3.0) s_toasts.pop_front();
        auto& io = ImGui::GetIO();
        auto dl = ImGui::GetForegroundDrawList();
        ImFont* font = s_body ? s_body : ImGui::GetFont();
        float size = 17.f, y = io.DisplaySize.y * .16f;
        for (auto& to : s_toasts) {
            float age = (float)(t - to.at), a = std::clamp(std::min(age * 6.f, (3.f - age) * 3.f), 0.f, 1.f);
            ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.f, to.text.c_str());
            ImVec2 p0(io.DisplaySize.x / 2 - ts.x / 2 - 14, y), p1(io.DisplaySize.x / 2 + ts.x / 2 + 14, y + ts.y + 12);
            dl->AddRectFilled(p0, p1, IM_COL32(28, 22, 34, (int)(235 * a)), 10.f);
            dl->AddRect(p0, p1, col(V::accent, a), 10.f, 0, 2.f);
            dl->AddText(font, size, ImVec2(p0.x + 14, p0.y + 6), IM_COL32(243, 236, 217, (int)(255 * a)), to.text.c_str());
            y += ts.y + 18;
        }
    }

    void drawAll() {
        applyStyle();
        auto& io = ImGui::GetIO();
        float target = s_open ? 1.f : 0.f;
        float speed = V::menuAnim ? io.DeltaTime * 8.f : 1.f;
        s_anim = s_anim < target ? std::min(target, s_anim + speed) : std::max(target, s_anim - speed);
        drawLabels();
        drawFrameWidget();
        if (s_anim > .001f) {
            if (V::dimGame) ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(10, 8, 20, (int)(110 * s_anim)));
            ImGui::GetStyle().Alpha = s_anim;
            if (s_body) ImGui::PushFont(s_body, 0.f);
            drawMenu();
            if (s_body) ImGui::PopFont();
            ImGui::GetStyle().Alpha = 1.f;
        }
        drawToasts();
    }
}

namespace menu {
    bool isOpen() { return s_open; }
    void toggle() {
        s_open = !s_open;
        if (s_open) PlatformToolbox::showCursor();
        else {
            cfg::waitingBind = nullptr;
            if (auto pl = PlayLayer::get(); pl && !pl->m_isPaused) PlatformToolbox::hideCursor();
        }
    }
    void notify(std::string const& text) {
        s_toasts.push_back({text, hacks::now()});
        while (s_toasts.size() > 4) s_toasts.pop_front();
    }
}

$on_mod(Loaded) {
    cfg::init();
    buildKids();
    ImGuiCocos::get().setup([] {
        ImGui::StyleColorsDark();
        auto& io = ImGui::GetIO();
        ImFontConfig fc;
        fc.FontDataOwnedByAtlas = false;
        s_body = io.Fonts->AddFontFromMemoryTTF((void*)inter_ttf, (int)inter_ttf_size, 16.f, &fc);
        ImFontConfig fc2;
        fc2.FontDataOwnedByAtlas = false;
        s_title = io.Fonts->AddFontFromMemoryTTF((void*)fredoka_ttf, (int)fredoka_ttf_size, 19.f, &fc2);
        if (s_body) io.FontDefault = s_body;
    }).draw([] {
        drawAll();
    });
    ImGuiCocos::get().setVisible(true);
}

// ------------------------------------------------------------ keys
class $modify(MenuKeys, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double time) {
        if (down && !repeat) {
            if (cfg::waitingBind) {
                cfg::waitingBind->bind = key == KEY_Escape ? 0 : (int)key;
                menu::notify(cfg::waitingBind->label + ": " + cfg::keyName(cfg::waitingBind->bind));
                cfg::waitingBind = nullptr;
                cfg::save();
                return true;
            }
            bool typing = ImGuiCocos::get().isInitialized() && ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput;
            if (!typing) {
                bool used = false;
                for (auto& f : cfg::all()) {
                    if (!f.bind || f.bind != (int)key) continue;
                    used = true;
                    if (f.type == cfg::T::Action) { if (f.onChange) f.onChange(); }
                    else if (f.type == cfg::T::Bool) {
                        bool& v = *(bool*)f.ptr;
                        v = !v;
                        cfg::changed(f);
                        menu::notify(f.label + (v ? ": on" : ": off"));
                    }
                }
                if (used) return true;
            }
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, time);
    }
};
