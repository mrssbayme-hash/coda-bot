// Replay bot: records your clicks on each physics step and plays them back.
// With "Position fix" on, it also saves where you were on each click and puts you back there,
// which keeps long macros in sync.
#include "config.hpp"
#include "shared.hpp"
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>

using namespace geode::prelude;

namespace {
    struct Input {
        unsigned frame = 0;
        bool down = false;
        int button = 1;
        bool p2 = false;
        bool hasPos = false;
        float x = 0.f, y = 0.f, rot = 0.f;
        double yv = 0.0;
    };
    replay::State s_state = replay::State::Off;
    std::vector<Input> s_inputs;
    size_t s_idx = 0;
    int s_tps = 240;
    std::string s_name = "my-macro";

    std::filesystem::path dir() {
        auto s = Mod::get()->getSaveDir().string();
        std::filesystem::path p(std::u8string(s.begin(), s.end()));
        return p / "macros";
    }
    std::string clean(std::string n) {
        std::string out;
        for (char c : n) if (std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == ' ') out += c;
        while (!out.empty() && out.back() == ' ') out.pop_back();
        return out.empty() ? "macro" : out;
    }
    bool heldIn(std::vector<Input> const& v, int button, bool p2) {
        for (auto it = v.rbegin(); it != v.rend(); ++it) if (it->button == button && it->p2 == p2) return it->down;
        return false;
    }
}

namespace replay {
    State state() { return s_state; }
    std::string& name() { return s_name; }
    size_t inputCount() { return s_inputs.size(); }
    size_t playIndex() { return s_idx; }
    unsigned lastFrame() { return s_inputs.empty() ? 0 : s_inputs.back().frame; }

    void record() {
        auto pl = PlayLayer::get();
        if (!pl) { menu::notify("Open a level first, then press Record"); return; }
        s_inputs.clear();
        s_idx = 0;
        s_tps = hacks::currentTps();
        s_state = State::Recording;
        if (!pl->m_isPracticeMode) pl->resetLevel();
        menu::notify("Recording. Play the level, then press Stop and Save.");
    }
    void play() {
        auto pl = PlayLayer::get();
        if (!pl) { menu::notify("Open a level first, then press Play"); return; }
        if (s_inputs.empty()) { menu::notify("Nothing to play yet. Record or load a macro first."); return; }
        if (s_tps != hacks::currentTps()) menu::notify("This macro was made at " + std::to_string(s_tps) + " TPS. Set TPS to match or it may not line up.");
        s_state = State::Playing;
        s_idx = 0;
        pl->resetLevel();
    }
    void stop() {
        if (s_state == State::Recording) menu::notify("Stopped. " + std::to_string(s_inputs.size()) + " inputs recorded.");
        s_state = State::Off;
    }
    void clear() { s_inputs.clear(); s_idx = 0; if (s_state == State::Playing) s_state = State::Off; }

    bool save(std::string const& n) {
        std::error_code ec;
        std::filesystem::create_directories(dir(), ec);
        s_name = clean(n);
        std::ofstream out(dir() / (s_name + ".codamacro"));
        if (!out) return false;
        out << "codamacro 1\ntps " << s_tps << "\n";
        for (auto& i : s_inputs) {
            out << i.frame << ' ' << (i.down ? 1 : 0) << ' ' << i.button << ' ' << (i.p2 ? 1 : 0);
            if (i.hasPos) out << ' ' << i.x << ' ' << i.y << ' ' << i.yv << ' ' << i.rot;
            out << '\n';
        }
        return (bool)out;
    }
    bool load(std::string const& n) {
        std::ifstream in(dir() / (clean(n) + ".codamacro"));
        if (!in) return false;
        std::string line;
        std::vector<Input> v;
        int tps = 240;
        while (std::getline(in, line)) {
            if (line.rfind("codamacro", 0) == 0) continue;
            if (line.rfind("tps ", 0) == 0) { try { tps = std::stoi(line.substr(4)); } catch (...) {} continue; }
            std::istringstream ss(line);
            Input i; int d = 0, p = 0;
            if (!(ss >> i.frame >> d >> i.button >> p)) continue;
            i.down = d != 0; i.p2 = p != 0;
            if (ss >> i.x >> i.y >> i.yv >> i.rot) i.hasPos = true;
            v.push_back(i);
        }
        std::stable_sort(v.begin(), v.end(), [](Input const& a, Input const& b) { return a.frame < b.frame; });
        s_inputs = std::move(v);
        s_tps = tps;
        s_idx = 0;
        s_name = clean(n);
        if (s_state == State::Recording) s_state = State::Off;
        return true;
    }
    std::vector<std::string> list() {
        std::vector<std::string> out;
        std::error_code ec;
        for (auto& e : std::filesystem::directory_iterator(dir(), ec)) {
            if (e.path().extension() == ".codamacro") {
                auto s = e.path().stem().u8string();
                out.emplace_back(s.begin(), s.end());
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    }
    std::string status() {
        switch (s_state) {
            case State::Recording: return "Recording: " + std::to_string(s_inputs.size()) + " inputs";
            case State::Playing: return "Playing: " + std::to_string(s_idx) + "/" + std::to_string(s_inputs.size());
            default: return "";
        }
    }
}

// ================================================================ hooks
class $modify(ReplayBaseLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto pl = PlayLayer::get();
        bool here = pl && pl == static_cast<GJBaseGameLayer*>(this) && !bot::simulating();
        if (here && s_state == replay::State::Playing && !bot::fromBot()) return;      // the macro has the controls
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        if (!here || s_state != replay::State::Recording) return;
        Input i;
        i.frame = m_gameState.m_currentProgress;
        i.down = down; i.button = button; i.p2 = !isPlayer1;
        auto p = isPlayer1 ? m_player1 : m_player2;
        if (p) { i.hasPos = true; i.x = p->m_position.x; i.y = p->m_position.y; i.yv = p->m_yVelocity; i.rot = p->getRotation(); }
        s_inputs.push_back(i);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto pl = PlayLayer::get();
        if (pl && pl == static_cast<GJBaseGameLayer*>(this) && !bot::simulating()) fwin::beforeInputs();
        if (s_state == replay::State::Playing && pl && pl == static_cast<GJBaseGameLayer*>(this) && !bot::simulating()) {
            long long cur = (long long)m_gameState.m_currentProgress;
            while (s_idx < s_inputs.size() && (long long)s_inputs[s_idx].frame + V::botOffset <= cur) {
                auto& i = s_inputs[s_idx++];
                auto p = i.p2 ? m_player2 : m_player1;
                if (V::botPosFix && i.hasPos && p && !p->m_isDead) {
                    p->m_position = ccp(i.x, i.y);
                    p->setPosition(ccp(i.x, i.y));
                    p->m_yVelocity = i.yv;
                    p->setRotation(i.rot);
                }
                bot::fromBot() = true;
                this->handleButton(i.down, i.button, !i.p2);
                bot::fromBot() = false;
            }
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

class $modify(ReplayPlayLayer, PlayLayer) {
    void resetLevel() {
        PlayLayer::resetLevel();
        unsigned cur = m_gameState.m_currentProgress;
        if (s_state == replay::State::Recording) {
            // practice: keep what happened before the checkpoint, drop the rest
            s_inputs.erase(std::remove_if(s_inputs.begin(), s_inputs.end(), [cur](Input const& i) { return i.frame >= cur; }), s_inputs.end());
            for (bool p2 : {false, true}) {
                for (int b = 1; b <= 3; b++) {
                    if (heldIn(s_inputs, b, p2)) { Input r; r.frame = cur; r.down = false; r.button = b; r.p2 = p2; s_inputs.push_back(r); }
                }
            }
        } else if (s_state == replay::State::Playing) {
            s_idx = std::lower_bound(s_inputs.begin(), s_inputs.end(), cur, [](Input const& i, unsigned f) { return i.frame < f; }) - s_inputs.begin();
        }
    }

    void levelComplete() {
        if (s_state == replay::State::Recording) {
            s_state = replay::State::Off;
            if (replay::save(s_name)) menu::notify("Level done. Saved as \"" + s_name + "\" (" + std::to_string(s_inputs.size()) + " inputs).");
        } else if (s_state == replay::State::Playing) {
            s_state = replay::State::Off;
            menu::notify("Macro finished the level.");
        }
        PlayLayer::levelComplete();
    }

    void onQuit() {
        if (s_state != replay::State::Off) s_state = replay::State::Off;
        PlayLayer::onQuit();
    }
};
