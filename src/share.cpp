// Coda Share: GDShare-style .gmd files. Export the level you're on to a .gmd file, and import .gmd
// files as levels in My Levels (the editor). Files live in one folder you can open from the menu.
#include <Geode/Geode.hpp>
#include "config.hpp"
#include "shared.hpp"
#include <imgui.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <regex>
#include <map>
#include <vector>
#include <algorithm>

using namespace geode::prelude;

namespace {
    std::filesystem::path folder() {
        auto s = Mod::get()->getConfigDir().string();
        std::filesystem::path p(std::u8string(s.begin(), s.end()));
        p /= "levels";
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        return p;
    }

    std::string esc(std::string const& s) {
        std::string o; o.reserve(s.size());
        for (char c : s) {
            if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;";
            else if (c == '"') o += "&quot;"; else if (c == '\'') o += "&apos;"; else o += c;
        }
        return o;
    }
    std::string unesc(std::string s) {
        static std::pair<char const*, char const*> const R[] = {{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}};
        for (auto& [a, b] : R) { size_t p = 0; while ((p = s.find(a, p)) != std::string::npos) { s.replace(p, std::strlen(a), b); p += std::strlen(b); } }
        return s;
    }

    std::string safeName(std::string n) {
        for (auto& c : n) if (std::string("\\/:*?\"<>|").find(c) != std::string::npos || (unsigned char)c < 32) c = '_';
        while (!n.empty() && (n.back() == ' ' || n.back() == '.')) n.pop_back();
        return n.empty() ? "level" : n;
    }

    // the level you're looking at: playing, in the editor, or on its page
    GJGameLevel* currentLevel() {
        if (auto pl = PlayLayer::get()) return pl->m_level;
        if (auto ed = LevelEditorLayer::get()) return ed->m_level;
        if (auto sc = CCDirector::get()->getRunningScene()) {
            if (auto l = sc->getChildByType<LevelInfoLayer>(0)) return l->m_level;
            if (auto l = sc->getChildByType<EditLevelLayer>(0)) return l->m_level;
        }
        return nullptr;
    }

    std::string lastMsg;
    std::vector<std::filesystem::path> files;
    double lastScan = -10.0;

    void scan() {
        files.clear();
        std::error_code ec;
        for (auto& e : std::filesystem::directory_iterator(folder(), ec)) {
            auto ext = e.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (e.is_regular_file() && ext == ".gmd") files.push_back(e.path());
        }
        std::sort(files.begin(), files.end(), [](auto& a, auto& b) { return a.filename().string() < b.filename().string(); });
    }

    bool exportLevel(GJGameLevel* lv) {
        std::string data = lv->m_levelString;
        if (data.empty()) { menu::notify("This level isn't downloaded yet. Open it (or play it) first."); return false; }
        std::ostringstream o;
        o << "<?xml version=\"1.0\"?><plist version=\"1.0\" gjver=\"2.0\"><dict>";
        auto str = [&](char const* k, std::string const& v) { o << "<k>" << k << "</k><s>" << esc(v) << "</s>"; };
        auto num = [&](char const* k, long long v) { o << "<k>" << k << "</k><i>" << v << "</i>"; };
        num("kCEK", 4);
        str("k2", std::string(lv->m_levelName));
        str("k4", data);
        if (!std::string(lv->m_levelDesc).empty()) str("k3", std::string(lv->m_levelDesc));
        if (!std::string(lv->m_creatorName).empty()) str("k5", std::string(lv->m_creatorName));
        num("k8", lv->m_audioTrack);
        if (lv->m_songID > 0) num("k45", lv->m_songID);
        if (!std::string(lv->m_songIDs).empty()) str("k104", std::string(lv->m_songIDs));
        if (!std::string(lv->m_sfxIDs).empty()) str("k105", std::string(lv->m_sfxIDs));
        if (lv->m_twoPlayerMode) o << "<k>k43</k><t/>";
        o << "<k>k13</k><t/><k>k21</k><i>2</i><k>k50</k><i>45</i>";
        o << "</dict></plist>";
        auto path = folder() / (safeName(lv->m_levelName) + ".gmd");
        std::ofstream f(path, std::ios::binary);
        if (!f) { menu::notify("Couldn't write the file"); return false; }
        f << o.str();
        f.close();
        lastMsg = "Saved " + path.filename().string();
        menu::notify("Exported " + std::string(lv->m_levelName) + " as a .gmd");
        lastScan = -10.0;
        return true;
    }

    bool importFile(std::filesystem::path const& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) { menu::notify("Couldn't open " + path.filename().string()); return false; }
        std::stringstream ss; ss << f.rdbuf();
        std::string s = ss.str();
        // read the top-level keys: <k>key</k> followed by <s>, <i>, <r> or <t/>
        std::map<std::string, std::string> kv;
        size_t p = 0;
        while ((p = s.find("<k>", p)) != std::string::npos) {
            size_t e = s.find("</k>", p);
            if (e == std::string::npos) break;
            std::string key = s.substr(p + 3, e - p - 3);
            size_t v = e + 4;
            if (s.compare(v, 4, "<t/>") == 0) { kv[key] = "1"; p = v + 4; continue; }
            if (s.compare(v, 3, "<s>") == 0 || s.compare(v, 3, "<i>") == 0 || s.compare(v, 3, "<r>") == 0) {
                char tag = s[v + 1];
                std::string close = std::string("</") + tag + ">";
                size_t ve = s.find(close, v + 3);
                if (ve == std::string::npos) break;
                if (!kv.count(key)) kv[key] = unesc(s.substr(v + 3, ve - v - 3));
                p = ve + close.size(); continue;
            }
            p = v;
        }
        if (kv["k4"].empty()) { menu::notify(path.filename().string() + " has no level in it"); return false; }
        auto lv = GJGameLevel::create();
        lv->m_levelName = kv.count("k2") ? kv["k2"] : path.stem().string();
        lv->m_levelString = kv["k4"];
        lv->m_levelDesc = kv["k3"];
        lv->m_creatorName = kv.count("k5") ? kv["k5"] : std::string(GameManager::get()->m_playerName);
        lv->m_audioTrack = numFromString<int>(kv["k8"]).unwrapOr(0);
        lv->m_songID = numFromString<int>(kv["k45"]).unwrapOr(0);
        lv->m_songIDs = kv["k104"];
        lv->m_sfxIDs = kv["k105"];
        lv->m_twoPlayerMode = kv.count("k43") > 0;
        lv->m_levelType = GJLevelType::Editor;
        lv->m_isEditable = true;
        LocalLevelManager::get()->m_localLevels->insertObject(lv, 0);
        lastMsg = "Imported " + std::string(lv->m_levelName) + " into My Levels";
        menu::notify(lastMsg);
        return true;
    }
}

namespace {
    // a level string you copied (starts with H4sI) becomes a new level in My Levels
    void importString(std::string data) {
        data.erase(std::remove_if(data.begin(), data.end(), [](char c) { return c == '\n' || c == '\r' || c == ' ' || c == '\t' || c == '"'; }), data.end());
        if (data.rfind("H4sI", 0) != 0 && data.rfind("kS38", 0) != 0 && data.rfind("kA", 0) != 0) {
            menu::notify("The clipboard doesn't have a level string in it"); return;
        }
        auto lv = GJGameLevel::create();
        lv->m_levelName = "Pasted level";
        lv->m_levelString = data;
        lv->m_creatorName = std::string(GameManager::get()->m_playerName);
        lv->m_levelType = GJLevelType::Editor;
        lv->m_isEditable = true;
        LocalLevelManager::get()->m_localLevels->insertObject(lv, 0);
        lastMsg = "Added \"Pasted level\" to My Levels";
        menu::notify(lastMsg);
    }
}

namespace share {
    void drawWindow() {
        auto lv = currentLevel();
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("Like GDShare: save levels as .gmd files, and add .gmd files to My Levels.");
        ImGui::PopTextWrapPos();
        ImGui::SeparatorText("Export");
        if (lv) {
            ImGui::TextUnformatted(std::string(lv->m_levelName).c_str());
            if (!std::string(lv->m_creatorName).empty()) { ImGui::SameLine(); ImGui::TextDisabled("by %s", std::string(lv->m_creatorName).c_str()); }
            float hw = (ImGui::GetContentRegionAvail().x - 4.f) / 2.f;
            if (ImGui::Button("Export as .gmd", ImVec2(hw, 0))) exportLevel(lv);
            ImGui::SameLine(0, 4);
            if (ImGui::Button("Copy level string", ImVec2(-FLT_MIN, 0))) {
                std::string data = lv->m_levelString;
                if (data.empty()) menu::notify("This level isn't downloaded yet. Open it (or play it) first.");
                else { (void)utils::clipboard::write(data); lastMsg = "Copied the level string (" + std::to_string(data.size()) + " characters)"; menu::notify("Level string copied"); }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("The long code that holds every object in the level (zipped, then base64).\nPaste it into Import from clipboard, or another level tool.");
        } else {
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextDisabled("Open a level's page, play it, or open it in the editor to export it.");
            ImGui::PopTextWrapPos();
        }
        ImGui::SeparatorText("Import");
        if (ImGui::Button("Import level string from clipboard", ImVec2(-FLT_MIN, 0))) importString(utils::clipboard::read());
        if (hacks::now() - lastScan > 2.0) { scan(); lastScan = hacks::now(); }
        if (files.empty()) {
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextDisabled("Put .gmd files in the levels folder, then they show up here.");
            ImGui::PopTextWrapPos();
        } else {
            ImGui::BeginChild("gmdfiles", ImVec2(0, std::min(220.f, 30.f * files.size() + 10.f) * V::menuScale), ImGuiChildFlags_Borders);
            for (size_t i = 0; i < files.size(); i++) {
                ImGui::PushID((int)i);
                float bw = 70.f * V::menuScale;
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(files[i].stem().string().c_str());
                ImGui::SameLine();
                ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - bw);
                if (ImGui::Button("Import", ImVec2(bw, 0))) importFile(files[i]);
                ImGui::PopID();
            }
            ImGui::EndChild();
            ImGui::TextDisabled("Imported levels go to the top of My Levels in Create.");
        }
        if (ImGui::Button("Open levels folder", ImVec2(-FLT_MIN, 0))) { auto u = folder().u8string(); utils::file::openFolder(std::string(u.begin(), u.end())); }
        if (!lastMsg.empty()) ImGui::TextColored(ImVec4(.45f, .9f, .55f, 1.f), "%s", lastMsg.c_str());
    }
}
