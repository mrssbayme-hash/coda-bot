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
    // ---------- frame window counting
    // While counting, we keep a copy of player 1 from the start of each of the last few physics steps.
    // For every click and release, an invisible player replays the macro from one of those copies with
    // that one input moved earlier or later, one step at a time, until it dies. The steps that still
    // survive are the input's frame window.
    struct Snap { unsigned tick = ~0u; PlayerObject* g = nullptr; };
    bool s_count = false;
    PlayLayer* s_countLayer = nullptr;
    std::vector<Snap> s_hist;
    PlayerObject* s_test = nullptr;
    int const MAX_FRAMES = 12;           // search up to 12 frames (240 Hz) each way

    bool isJump(Input const& i) { return !i.p2 && i.button == 1; }
    void dropGhosts() { s_hist.clear(); s_test = nullptr; s_countLayer = nullptr; }
    bool ensureGhosts(PlayLayer* pl, int ticksBack) {
        if (s_countLayer != pl) dropGhosts();
        s_countLayer = pl;
        if (!s_test) s_test = sim::makeGhost();
        while ((int)s_hist.size() < ticksBack + 2) {
            auto g = sim::makeGhost();
            if (!g) return false;
            s_hist.push_back({~0u, g});
        }
        return s_test != nullptr;
    }
    Snap* snapAt(unsigned tick) {
        for (auto& sn : s_hist) if (sn.tick == tick) return &sn;
        return nullptr;
    }
    void takeSnapshot(PlayLayer* pl, unsigned tick) {
        if (s_hist.empty() || !pl->m_player1) return;
        auto& sn = s_hist[tick % s_hist.size()];
        sim::copy(pl->m_player1, sn.g);
        sn.tick = tick;
    }

    // 1 = survives, 0 = dies, -1 = can't test this timing
    int tryTiming(PlayLayer* pl, size_t j, int shift, unsigned prevT, unsigned nextT, int tps) {
        auto& in = s_inputs[j];
        long long t = (long long)in.frame + shift;
        if (t <= (long long)prevT || t >= (long long)nextT) return -1;
        unsigned start = std::min<unsigned>(in.frame, (unsigned)t);
        Snap* sn = snapAt(start);
        if (!sn) return -1;
        unsigned horizon = std::max<unsigned>((unsigned)t, nextT == ~0u ? in.frame : nextT) + (unsigned)std::max(8, tps / 8);
        horizon = std::min<unsigned>(horizon, in.frame + (unsigned)tps);
        sim::copy(sn->g, s_test);
        std::unordered_set<GameObject*> act;
        float dt = 60.f / (float)tps;
        size_t k = std::lower_bound(s_inputs.begin(), s_inputs.end(), start, [](Input const& a, unsigned f) { return a.frame < f; }) - s_inputs.begin();
        for (unsigned tick = start; tick <= horizon; tick++) {
            // press/release whatever happens on this step (with input j moved)
            if (j < s_inputs.size() && (unsigned)t == tick) sim::button(s_test, in.down);
            for (; k < s_inputs.size() && s_inputs[k].frame <= tick; k++) {
                if (k == j || !isJump(s_inputs[k])) continue;
                if (s_inputs[k].frame == tick) sim::button(s_test, s_inputs[k].down);
            }
            bool done = false;
            if (!sim::tick(s_test, act, dt, done)) return 0;
            if (done) return 1;
        }
        return 1;
    }

    fwc::Window measure(PlayLayer* pl, size_t j, int tps) {
        auto& in = s_inputs[j];
        fwc::Window w;
        w.tick = in.frame; w.down = in.down;
        w.pct = pl->getCurrentPercent();
        unsigned prevT = 0, nextT = ~0u;
        for (size_t a = j; a-- > 0;) if (isJump(s_inputs[a])) { prevT = s_inputs[a].frame; break; }
        for (size_t a = j + 1; a < s_inputs.size(); a++) if (isJump(s_inputs[a])) { nextT = s_inputs[a].frame; break; }
        int maxSteps = std::max(1, MAX_FRAMES * tps / 240);
        if (tryTiming(pl, j, 0, prevT, nextT, tps) != 1) { w.unknown = true; return w; }
        for (int d = 1; d <= maxSteps; d++) {
            int r = tryTiming(pl, j, -d, prevT, nextT, tps);
            if (r == 1) w.early = d; else { w.cappedEarly = r < 0; break; }
            if (d == maxSteps) w.cappedEarly = true;
        }
        for (int d = 1; d <= maxSteps; d++) {
            int r = tryTiming(pl, j, d, prevT, nextT, tps);
            if (r == 1) w.late = d; else { w.cappedLate = r < 0; break; }
            if (d == maxSteps) w.cappedLate = true;
        }
        w.frames = (float)(w.early + w.late + 1) * 240.f / (float)tps;
        return w;
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
        if (s_count) { s_count = false; fwc::finish(false); }
        s_state = State::Off;
    }
    void countWindows() {
        auto pl = PlayLayer::get();
        if (!pl) { menu::notify("Open the level first, then press Count"); return; }
        if (pl->m_isPlatformer) { menu::notify("The frame window counter only works in classic levels"); return; }
        if (s_inputs.empty()) { menu::notify("Record or load a macro of the level first"); return; }
        if (s_tps != hacks::currentTps()) { menu::notify("Set TPS to " + std::to_string(s_tps) + " (what the macro was made at) first"); return; }
        if (V::botOffset != 0) { menu::notify("Set the bot's Frame offset to 0 before counting"); return; }
        if (s_state == State::Recording) s_state = State::Off;
        if (!ensureGhosts(pl, std::max(1, MAX_FRAMES * s_tps / 240))) { menu::notify("Couldn't start the counter"); return; }
        for (auto& sn : s_hist) sn.tick = ~0u;
        s_count = true;
        fwc::begin(s_tps);
        s_state = State::Playing;
        s_idx = 0;
        pl->resetLevel();
        menu::notify("Counting frame windows. Coda is replaying your macro.");
    }
    void clear() {
        s_inputs.clear(); s_idx = 0;
        if (s_count) { s_count = false; fwc::cancel(); }
        if (s_state == State::Playing) s_state = State::Off;
    }

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
        if (p) { i.hasPos = true; i.x = p->getPositionX(); i.y = p->getPositionY(); i.yv = p->m_yVelocity; i.rot = p->getRotation(); }   // node position: m_position is only refreshed once per frame
        s_inputs.push_back(i);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto pl = PlayLayer::get();
        if (s_state == replay::State::Playing && pl && pl == static_cast<GJBaseGameLayer*>(this) && !bot::simulating()) {
            long long cur = (long long)m_gameState.m_currentProgress;
            if (s_count && m_player1 && !m_player1->m_isDead) takeSnapshot(pl, (unsigned)cur);
            while (s_idx < s_inputs.size() && (long long)s_inputs[s_idx].frame + V::botOffset <= cur) {
                if (s_count && isJump(s_inputs[s_idx]) && s_inputs[s_idx].frame == (unsigned)cur && m_player1 && !m_player1->m_isDead)
                    fwc::add(measure(pl, s_idx, s_tps));
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
    bool init(GJGameLevel* level, bool dontCreateObjects) {
        dropGhosts();
        if (s_count) { s_count = false; fwc::cancel(); }
        return PlayLayer::init(level, dontCreateObjects);
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        bool wasDead = player && player->m_isDead;
        PlayLayer::destroyPlayer(player, object);
        if (s_count && player == m_player1 && player->m_isDead && !wasDead && !bot::simulating()) {
            s_count = false;
            fwc::finish(false);
            menu::notify("The macro died, so counting stopped here. Results so far are in the Coda Bot window.");
        }
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        for (auto& sn : s_hist) sn.tick = ~0u;
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
            if (s_count) { s_count = false; fwc::finish(true); }
            else menu::notify("Macro finished the level.");
        }
        PlayLayer::levelComplete();
    }

    void onQuit() {
        if (s_state != replay::State::Off) s_state = replay::State::Off;
        if (s_count) { s_count = false; fwc::finish(false); }
        dropGhosts();
        PlayLayer::onQuit();
    }
};
