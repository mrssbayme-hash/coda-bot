// Level Roulette: roll a random level from the AREDL or the Impossible Levels List and go play it.
// Lists are downloaded from aredl.net, impossiblelevels.com and the IPLL's GitHub the first time you roll.
#include "config.hpp"
#include "shared.hpp"
#include <imgui.h>
#include <Geode/utils/web.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <random>
#include <map>
#include <optional>
#include <cfloat>
#include <memory>
#include <algorithm>
#include <cctype>

using namespace geode::prelude;

namespace {
    struct Entry {
        std::string list;
        int pos = 0;
        std::string name;
        int id = 0;
        int fps = 240;
        int len = -1;          // 0 tiny, 1 short, 2 medium, 3 long, 4 XL, -1 unknown
        bool twoP = false;
    };
    struct ListData { std::vector<Entry> levels; bool loading = false, loaded = false; std::string error; };
    std::map<std::string, ListData> s_lists;
    std::optional<Entry> s_current;
    bool s_pendingRoll = false;
    int s_autoplay = 0;
    std::mt19937 s_rng{std::random_device{}()};
    char const* const LISTS[] = {"AREDL", "ILL", "TILL", "PILL", "All"};
    char const* const LENGTHS[] = {"Tiny", "Short", "Medium", "Long", "XL"};

    int lenFromSeconds(double s) { return s < 10 ? 0 : s < 30 ? 1 : s < 60 ? 2 : s < 120 ? 3 : 4; }

    // Roulette run, saved between sessions
    int& stage() { static int v = (int)Mod::get()->getSavedValue<int64_t>("rl-stage", 1); return v; }
    int& need() { static int v = (int)Mod::get()->getSavedValue<int64_t>("rl-need", 1); return v; }
    float& best() { static float v = (float)Mod::get()->getSavedValue<double>("rl-best", 0.0); return v; }
    int& runLevel() { static int v = (int)Mod::get()->getSavedValue<int64_t>("rl-level", 0); return v; }
    std::string& runName() { static std::string v = Mod::get()->getSavedValue<std::string>("rl-name", ""); return v; }
    void saveRun() {
        Mod::get()->setSavedValue<int64_t>("rl-stage", stage());
        Mod::get()->setSavedValue<int64_t>("rl-need", need());
        Mod::get()->setSavedValue<double>("rl-best", best());
        Mod::get()->setSavedValue<int64_t>("rl-level", runLevel());
        Mod::get()->setSavedValue<std::string>("rl-name", runName());
    }

    void roll();

    void parseAredl(matjson::Value const& json, ListData& out) {
        if (!json.isArray()) { out.error = "AREDL sent something unexpected"; return; }
        for (auto const& it : json.asArray().unwrap()) {
            if (!it.isObject()) continue;
            Entry e;
            e.list = "AREDL";
            e.pos = (int)it["position"].asInt().unwrapOr(0);
            e.name = it["name"].asString().unwrapOr("");
            e.id = (int)it["level_id"].asInt().unwrapOr(0);
            e.twoP = it["two_player"].asBool().unwrapOr(false);
            if (it.contains("tags") && it["tags"].isArray()) {
                for (auto const& t : it["tags"].asArray().unwrap()) {
                    auto s = t.asString().unwrapOr("");
                    if (s == "Tiny") e.len = 0; else if (s == "Short") e.len = 1; else if (s == "Medium") e.len = 2;
                    else if (s == "Long") e.len = 3; else if (s == "XL" || s == "XXL" || s == "XXL+") e.len = 4;
                }
            }
            if (e.id > 0 && e.pos > 0 && !e.name.empty()) out.levels.push_back(e);
        }
    }

    void parseIll(matjson::Value const& json, ListData& out) {
        if (!json.isArray()) { out.error = "The ILL sent something unexpected"; return; }
        for (auto const& it : json.asArray().unwrap()) {
            if (!it.isObject() || !it["visible"].asBool().unwrapOr(false)) continue;
            Entry e;
            e.list = "ILL";
            e.pos = (int)it["rank"].asInt().unwrapOr(0);
            e.name = it["name"].asString().unwrapOr("");
            auto idv = it["levelId"];
            e.id = idv.isString() ? geode::utils::numFromString<int>(idv.asString().unwrapOr("0")).unwrapOr(0) : (int)idv.asInt().unwrapOr(0);
            e.fps = (int)it["fps"].asDouble().unwrapOr(240.0);
            if (!it["levelLength"].isNull()) e.len = lenFromSeconds(it["levelLength"].asDouble().unwrapOr(0.0));
            if (e.id > 0 && e.pos > 0 && !e.name.empty()) out.levels.push_back(e);
        }
    }

    // ---- PILL: the Impossible Platformer Levels List (IPLL). It's a website made from files on GitHub:
    // one file with the order, and one small file per level with its ID.
    char const* const PILL_DATA = "https://raw.githubusercontent.com/Romlekt/ImpossiblePlatformerLevelsList/main/data/";
    std::string urlPart(std::string const& s) {
        std::string o; char b[4];
        for (unsigned char c : s) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
            else { std::snprintf(b, sizeof b, "%%%02X", c); o += b; }
        }
        return o;
    }
    void pillDone() {
        auto& d = s_lists["PILL"];
        d.loading = false; d.loaded = true;
        std::sort(d.levels.begin(), d.levels.end(), [](Entry const& a, Entry const& b) { return a.pos < b.pos; });
        if (d.levels.empty()) { d.loaded = false; d.error = "Couldn't read the PILL"; menu::notify(d.error); }
        if (s_pendingRoll) roll();
    }
    void loadPill() {
        auto& d = s_lists["PILL"];
        d.loading = true; d.error.clear(); d.levels.clear();
        async::spawn(web::WebRequest().get(std::string(PILL_DATA) + "_list.json"), [](web::WebResponse res) {
            auto& d = s_lists["PILL"];
            if (!res.ok()) { d.loading = false; d.loaded = false; d.error = "Couldn't reach the PILL (" + std::to_string(res.code()) + ")"; menu::notify(d.error); return; }
            auto j = res.json();
            if (j.isErr() || !j.unwrap().isArray()) { d.loading = false; d.loaded = false; d.error = "Couldn't read the PILL"; menu::notify(d.error); return; }
            std::vector<std::string> names;
            for (auto const& v : j.unwrap().asArray().unwrap()) if (auto n = v.asString(); n.isOk() && !n.unwrap().empty()) names.push_back(n.unwrap());
            if (names.empty()) { d.error = "The PILL list is empty"; d.loading = false; d.loaded = false; menu::notify(d.error); return; }
            auto left = std::make_shared<int>((int)names.size());
            for (size_t i = 0; i < names.size(); i++) {
                int pos = (int)i + 1;
                std::string file = names[i];
                async::spawn(web::WebRequest().get(std::string(PILL_DATA) + urlPart(file) + ".json"), [pos, file, left](web::WebResponse r) {
                    auto& d = s_lists["PILL"];
                    if (r.ok()) if (auto lj = r.json(); lj.isOk() && lj.unwrap().isObject()) {
                        auto const& it = lj.unwrap();
                        Entry e;
                        e.list = "PILL";
                        e.pos = pos;
                        e.name = it["name"].asString().unwrapOr(file);
                        auto idv = it["id"];
                        e.id = idv.isString() ? geode::utils::numFromString<int>(idv.asString().unwrapOr("0")).unwrapOr(0) : (int)idv.asInt().unwrapOr(0);
                        e.twoP = e.name.find("[2P]") != std::string::npos;
                        if (e.id > 0) d.levels.push_back(e);
                    }
                    if (--*left == 0) pillDone();
                });
            }
        });
    }

    void load(std::string const& name) {
        auto& d = s_lists[name];
        if (d.loading || d.loaded) return;
        if (name == "PILL") { loadPill(); return; }
        std::string url;
        if (name == "AREDL") url = "https://api.aredl.net/v2/api/aredl/levels";
        else if (name == "ILL") url = "https://api.impossiblelevels.com/api/Levels";
        else { d.loaded = true; d.error = name + " isn't connected yet"; return; }
        d.loading = true;
        d.error.clear();
        async::spawn(web::WebRequest().get(url), [name](web::WebResponse res) {
            auto& d = s_lists[name];
            d.loading = false;
            d.loaded = true;
            if (!res.ok()) { d.error = "Couldn't reach " + name + " (" + std::to_string(res.code()) + ")"; d.loaded = false; }
            else if (auto j = res.json(); j.isErr()) { d.error = "Couldn't read the " + name + " list"; d.loaded = false; }
            else if (name == "AREDL") parseAredl(j.unwrap(), d);
            else parseIll(j.unwrap(), d);
            if (!d.error.empty()) menu::notify(d.error);
            if (s_pendingRoll) roll();
        });
    }

    std::vector<std::string> chosen() {
        int c = V::rlList;
        if (c == 4) return {"AREDL", "ILL", "PILL"};
        return {LISTS[std::clamp(c, 0, 3)]};
    }

    void roll() {
        s_pendingRoll = false;
        auto names = chosen();
        for (auto& n : names) {
            auto& d = s_lists[n];
            if (!d.loaded) { load(n); if (d.loading) { s_pendingRoll = true; return; } }
        }
        std::vector<Entry const*> pool;
        for (auto& n : names) {
            for (auto& e : s_lists[n].levels) {
                if (e.pos < V::rlFrom || e.pos > V::rlTo) continue;
                if (V::rlLimitFps && e.fps > V::rlMaxFps) continue;
                if (e.len >= 0 && !V::rlLen[e.len]) continue;
                if (V::rlSkip2P && e.twoP) continue;
                if (V::rlSkipBeaten && GameStatsManager::sharedState()->hasCompletedOnlineLevel(e.id)) continue;
                if (s_current && s_current->id == e.id && pool.size() > 0) continue;
                pool.push_back(&e);
            }
        }
        if (pool.empty()) {
            bool missing = true;
            for (auto& n : names) if (!s_lists[n].levels.empty()) missing = false;
            menu::notify(missing ? "That list isn't available yet" : "No levels match these filters");
            return;
        }
        s_current = *pool[std::uniform_int_distribution<size_t>(0, pool.size() - 1)(s_rng)];
        if (V::rlRoulette) { runLevel() = s_current->id; runName() = s_current->name; best() = 0.f; saveRun(); }
    }

    // ---------------------------------------------------------------- opening a level by ID
    struct Fetch : LevelManagerDelegate {
        bool autoplay = false;
        int id = 0;
        void done(CCArray* levels) {
            auto glm = GameLevelManager::sharedState();
            if (glm->m_levelManagerDelegate == this) glm->m_levelManagerDelegate = nullptr;
            if (!levels || levels->count() == 0) { menu::notify("Couldn't find that level on the GD servers"); return; }
            auto level = static_cast<GJGameLevel*>(levels->objectAtIndex(0));
            s_autoplay = autoplay ? id : 0;
            CCDirector::get()->pushScene(CCTransitionFade::create(.4f, LevelInfoLayer::scene(level, false)));
        }
        void loadLevelsFinished(CCArray* levels, char const*, int) override { done(levels); }
        void loadLevelsFinished(CCArray* levels, char const*) override { done(levels); }
        void loadLevelsFailed(char const*, int) override { fail(); }
        void loadLevelsFailed(char const*) override { fail(); }
        void fail() {
            auto glm = GameLevelManager::sharedState();
            if (glm->m_levelManagerDelegate == this) glm->m_levelManagerDelegate = nullptr;
            menu::notify("Couldn't load that level from the GD servers");
        }
    };
    Fetch s_fetch;

    void open(Entry const& e, bool autoplay) {
        if (PlayLayer::get()) { menu::notify("Leave the level you're in first"); return; }
        if (autoplay && V::rlAutoTps && e.fps > 240) {
            V::tpsBypass = true;
            V::tps = std::clamp(e.fps, 30, 10000);
            if (auto f = cfg::find("tps")) cfg::changed(*f);
            if (auto f = cfg::find("tps-bypass")) cfg::changed(*f);
            menu::notify("TPS set to " + std::to_string(V::tps) + " for this level");
        }
        s_fetch.autoplay = autoplay;
        s_fetch.id = e.id;
        auto glm = GameLevelManager::sharedState();
        glm->m_levelManagerDelegate = &s_fetch;
        glm->getOnlineLevels(GJSearchObject::create(SearchType::Search, std::to_string(e.id)));
        menu::notify("Finding " + e.name + "...");
    }
}

// ---------------------------------------------------------------- the window
namespace roulette {
    void drawWindow() {
        auto setChanged = [](char const* key) { if (auto f = cfg::find(key)) cfg::changed(*f); };
        auto accent = ImVec4(V::accent.r, V::accent.g, V::accent.b, 1.f);
        float w = (ImGui::GetContentRegionAvail().x - 4.f * 4) / 5.f;
        for (int i = 0; i < 5; i++) {
            if (i) ImGui::SameLine(0, 4);
            bool on = V::rlList == i;
            if (on) { ImGui::PushStyleColor(ImGuiCol_Button, accent); ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.137f, .125f, .11f, 1.f)); }
            if (ImGui::Button(LISTS[i], ImVec2(w, 0))) { V::rlList = i; setChanged("rl-list"); }
            if (on) ImGui::PopStyleColor(2);
        }
        if (V::rlList == 2)
            ImGui::TextColored(ImVec4(1.f, .65f, .45f, 1.f), "TILL isn't connected yet. Send its website to add it.");
        if (V::rlList == 3) ImGui::TextDisabled("The Impossible Platformer Levels List (platformer levels).");
        // how many levels in the chosen lists
        size_t total = 0; bool loading = false;
        for (auto& n : chosen()) { auto& d = s_lists[n]; total += d.levels.size(); loading |= d.loading; }
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::DragIntRange2("##range", &V::rlFrom, &V::rlTo, 1.f, 1, 5000, "From #%d", "to #%d")) {
            V::rlTo = std::max(V::rlTo, V::rlFrom);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) { setChanged("rl-from"); setChanged("rl-to"); }
        if (loading) ImGui::TextDisabled("Downloading the list...");
        else if (total) ImGui::TextDisabled("%zu levels in this list", total);

        ImGui::SeparatorText("Filters");
        ImGui::TextUnformatted("Length");
        float lw = (ImGui::GetContentRegionAvail().x - 4.f * 4) / 5.f;
        for (int i = 0; i < 5; i++) {
            if (i) ImGui::SameLine(0, 4);
            bool on = V::rlLen[i];
            if (on) { ImGui::PushStyleColor(ImGuiCol_Button, accent); ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.137f, .125f, .11f, 1.f)); }
            if (ImGui::Button(LENGTHS[i], ImVec2(lw, 0))) { V::rlLen[i] = !V::rlLen[i]; setChanged((std::string("rl-len-") + std::to_string(i)).c_str()); }
            if (on) ImGui::PopStyleColor(2);
        }
        if (ImGui::Checkbox("Max FPS/TPS needed", &V::rlLimitFps)) setChanged("rl-limit-fps");
        if (V::rlLimitFps) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputInt("##maxfps", &V::rlMaxFps, 60, 240)) V::rlMaxFps = std::clamp(V::rlMaxFps, 30, 20000);
            if (ImGui::IsItemDeactivatedAfterEdit()) setChanged("rl-max-fps");
            ImGui::TextDisabled("AREDL and PILL levels count as 240. The ILL lists the FPS each level needs.");
        }
        if (ImGui::Checkbox("Skip levels I've beaten", &V::rlSkipBeaten)) setChanged("rl-skip-beaten");
        if (ImGui::Checkbox("Skip 2-player levels", &V::rlSkip2P)) setChanged("rl-skip-2p");
        if (ImGui::Checkbox("Set TPS to what the level needs", &V::rlAutoTps)) setChanged("rl-auto-tps");
        if (ImGui::Checkbox("Roulette mode", &V::rlRoulette)) setChanged("rl-roulette");

        // the rolled level
        ImGui::Separator();
        if (s_current) {
            auto& e = *s_current;
            ImGui::TextColored(accent, "%s #%d", e.list.c_str(), e.pos);
            ImGui::TextUnformatted(e.name.c_str());
            std::string meta = "ID " + std::to_string(e.id);
            if (e.len >= 0) meta += std::string("  -  ") + LENGTHS[e.len];
            if (e.list == "ILL") meta += "  -  needs " + std::to_string(e.fps) + " FPS";
            if (e.twoP) meta += "  -  2 player";
            ImGui::TextDisabled("%s", meta.c_str());
        } else if (s_pendingRoll) ImGui::TextDisabled("Getting the list ready...");
        else ImGui::TextDisabled("Press Roll to get a random level.");

        if (V::rlRoulette) {
            ImGui::Separator();
            if (need() > 100) ImGui::TextColored(ImVec4(.45f, .9f, .55f, 1.f), "Roulette finished! You made it to 100%%.");
            else {
                ImGui::Text("Roulette level %d: get at least %d%%", stage(), need());
                ImGui::TextDisabled("Best on this level: %.0f%%%s", best(), runLevel() ? (" (" + runName() + ")").c_str() : "");
                bool canNext = runLevel() && best() >= (float)need();
                if (!canNext) ImGui::BeginDisabled();
                if (ImGui::Button("Next level", ImVec2(ImGui::GetContentRegionAvail().x / 2 - 2, 0))) {
                    need() = (int)best() + 1; stage()++; runLevel() = 0; runName().clear(); best() = 0.f; saveRun();
                    if (need() <= 100) roll();
                    else menu::notify("You finished the roulette!");
                }
                if (!canNext) ImGui::EndDisabled();
                ImGui::SameLine(0, 4);
                if (ImGui::Button("Give up", ImVec2(-FLT_MIN, 0))) {
                    menu::notify("Roulette over at level " + std::to_string(stage()));
                    stage() = 1; need() = 1; best() = 0.f; runLevel() = 0; runName().clear(); saveRun();
                }
            }
        }

        float bw = (ImGui::GetContentRegionAvail().x - 8.f) / 3.f;
        bool has = s_current.has_value();
        if (!has) ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.18f, .5f, .32f, 1.f));
        if (ImGui::Button("Play it", ImVec2(bw, 0)) && has) open(*s_current, true);
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 4);
        if (ImGui::Button("Level page", ImVec2(bw, 0)) && has) open(*s_current, false);
        if (!has) ImGui::EndDisabled();
        ImGui::SameLine(0, 4);
        bool lockRoll = V::rlRoulette && runLevel() != 0 && need() <= 100;
        if (lockRoll) ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.62f, .18f, .26f, 1.f));
        if (ImGui::Button(has ? "Reroll" : "Roll", ImVec2(-FLT_MIN, 0))) roll();
        ImGui::PopStyleColor();
        if (lockRoll) ImGui::EndDisabled();
        if (lockRoll) ImGui::TextDisabled("In roulette mode you have to play the level you rolled.");
        ImGui::TextDisabled("Lists come from aredl.net, impossiblelevels.com and the IPLL on GitHub.");
    }
}

// ---------------------------------------------------------------- roulette progress and auto play
class $modify(RoulettePlayLayer, PlayLayer) {
    void track(float pct) {
        if (!V::rlRoulette || !m_level || runLevel() == 0) return;
        if (m_level->m_levelID.value() != runLevel() || safe::cheatedThisRun()) return;
        if (pct > best()) { best() = pct; saveRun(); if (best() >= (float)need()) menu::notify("Roulette goal reached! Open the menu for the next level."); }
    }
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        bool was = player && player->m_isDead;
        PlayLayer::destroyPlayer(player, object);
        if (player && player->m_isDead && !was && !bot::simulating() && !m_isPracticeMode) this->track(this->getCurrentPercent());
    }
    void levelComplete() {
        if (!m_isPracticeMode) this->track(100.f);
        PlayLayer::levelComplete();
    }
};

class $modify(RouletteInfoLayer, LevelInfoLayer) {
    bool init(GJGameLevel* level, bool challenge) {
        if (!LevelInfoLayer::init(level, challenge)) return false;
        if (s_autoplay && level && level->m_levelID.value() == s_autoplay && !this->shouldDownloadLevel()) {
            s_autoplay = 0;
            this->runAction(CCSequence::create(CCDelayTime::create(.5f),
                CCCallFunc::create(this, callfunc_selector(RouletteInfoLayer::autoPlay)), nullptr));
        }
        return true;
    }
    void levelDownloadFinished(GJGameLevel* level) {
        LevelInfoLayer::levelDownloadFinished(level);
        if (s_autoplay && m_level && m_level->m_levelID.value() == s_autoplay) {
            s_autoplay = 0;
            this->runAction(CCSequence::create(CCDelayTime::create(.3f),
                CCCallFunc::create(this, callfunc_selector(RouletteInfoLayer::autoPlay)), nullptr));
        }
    }
    void autoPlay() { this->onPlay(nullptr); }
};
