// Coda Bot: Coda learns levels by trial and error.
// His runs never count: the level is put in test mode, completions are blocked, and so is verifying.
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <random>
#include <deque>
#include <functional>
#include <algorithm>

using namespace geode::prelude;

// ---------------------------------------------------------------- the brain
// What Coda knows about a level: when to hold the button, as ranges of the player's X position.
struct Hold { float start; float end; };

struct Brain {
    std::vector<Hold> best;      // the inputs from his furthest run
    std::vector<Hold> trial;     // what he is trying this attempt
    float bestX = 0.f;           // how far the best run got
    float bestPct = 0.f;
    int attempts = 0;
    int stuck = 0;               // attempts since his last new best
    bool beaten = false;
    bool confirm = false;        // replay the best run once to set a checkpoint before experimenting
    float cpX = 0.f;             // where his current checkpoint is (0 = none)
    float noCpBefore = 0.f;      // after giving up on a checkpoint, don't set another until he gets past here
    int sweep = 0;               // which nudge to try next on the jump before the spike
    // training mode
    int gen = 0;                 // finished generations
    float genBestX = 0.f, genBestPct = 0.f;
    std::vector<CCPoint> genBestPath;
    std::vector<float> genHistory;               // best % of each generation (for the graph)
    std::deque<std::vector<CCPoint>> oldPaths;   // best path of the last few generations
    std::vector<CCPoint> bestPath;               // the path of his best run
    std::vector<CCPoint> bestClicks;
    std::deque<CCPoint> deaths;
    int skipUntil = -1;                          // skip ahead until this generation is done
};

// ---------------------------------------------------------------- learning modes
struct Mode {
    char const* name;
    bool checkpoints;     // practice from just before the hard part
    float cpMargin;       // how far before his best the checkpoint goes
    int cpPatience;       // attempts stuck before he gives up on the checkpoint
    float knownSpeed;     // game speed through parts he already knows
    float frontSpeed;     // game speed near the hard part
    float slip;           // chance of fumbling a known part
    float slipBeaten;
};
static Mode const MODES[] = {
    {"Slow",    false, 0.f,   0,  1.f, 1.f, 0.14f, 0.30f},
    {"High",    true,  210.f, 35, 3.f, 1.f, 0.07f, 0.20f},
    {"Extreme", true,  120.f, 25, 5.f, 2.f, 0.03f, 0.10f},
};
static int s_mode = 1;
static bool s_training = false;
static int s_speedIdx = 1;
static float const SPEEDS[] = {1.f, 4.f, 10.f, 20.f};
static bool s_hitboxes = false;
static bool s_trail = false;
static int perGen() { return std::max(1, (int)Mod::get()->getSettingValue<int64_t>("attempts-per-generation")); }
static int oldPathCount() { return std::max(0, (int)Mod::get()->getSettingValue<int64_t>("old-paths")); }
static void applyFps() {
    if (!Mod::get()->getSettingValue<bool>("fps-bypass")) return;
    double fps = (double)std::max<int64_t>(30, Mod::get()->getSettingValue<int64_t>("fps"));
    CCDirector::get()->setAnimationInterval(1.0 / fps);
}
static Mode const& mode() { return MODES[s_mode]; }

static std::unordered_map<std::string, Brain> s_brains;
static bool s_enabled = false;   // Coda is playing
static bool s_fromBot = false;   // the current button press came from Coda
static std::mt19937 s_rng{std::random_device{}()};

static float rnd(float a, float b) { return std::uniform_real_distribution<float>(a, b)(s_rng); }
static bool chance(float p) { return rnd(0.f, 1.f) < p; }

static std::string levelKey(GJGameLevel* level) {
    int id = level->m_levelID.value();
    if (id > 0) return std::to_string(id);
    return "local:" + std::string(level->m_levelName);
}

static void tidy(std::vector<Hold>& v) {
    std::sort(v.begin(), v.end(), [](Hold const& a, Hold const& b) { return a.start < b.start; });
    std::vector<Hold> out;
    for (auto h : v) {
        if (h.end <= h.start) h.end = h.start + 2.f;
        if (!out.empty() && h.start <= out.back().end + 1.f) out.back().end = std::max(out.back().end, h.end);
        else out.push_back(h);
    }
    v = std::move(out);
}

// Try something new near the place he keeps dying.
//  - most of the time: nudge the last jump before the spike, sweeping earlier/later in growing steps
//  - sometimes: add a jump right before the spike
//  - otherwise: shake up everything in a window that grows the longer he's stuck
// Nothing before `floor` (his checkpoint) can change, since he'd never replay it.
static std::vector<Hold> mutate(std::vector<Hold> const& best, float frontier, int stuck, int& sweep, float floor) {
    static float const nudges[] = {2, -2, 4, -4, 6, -6, 9, -9, 13, -13, 18, -18};
    std::vector<Hold> out;
    for (auto h : best) if (h.start <= frontier + 30.f) out.push_back(h);   // forget what was never reached
    float window = 45.f + std::min(stuck, 40) * 7.f;
    if (stuck > 25 && chance(0.25f)) window *= 2.f;
    float lo = std::max(floor + 4.f, frontier - window);

    int last = -1;
    for (int i = 0; i < (int)out.size(); i++) if (out[i].start >= lo && out[i].start < frontier) last = i;
    float r = rnd(0.f, 1.f);
    if (last >= 0 && r < 0.5f) {
        float d = nudges[sweep % 12] * (1.f + (sweep / 12) * 0.5f);
        sweep++;
        out[last].start += d;
        if (chance(0.5f)) out[last].end += d; else out[last].end += rnd(-8.f, 8.f);
    } else if (r < 0.75f) {
        float s = std::max(lo, frontier - rnd(4.f, 45.f));
        float len = chance(0.65f) ? rnd(1.f, 6.f) : rnd(10.f, 90.f);
        out.push_back({s, s + len});
    } else {
        std::vector<Hold> keep;
        for (auto h : out) {
            if (h.start >= lo) {
                if (chance(0.12f)) continue;
                if (chance(0.45f)) { float d = rnd(-9.f, 9.f); h.start += d; h.end += d; }
                if (chance(0.30f)) h.end += rnd(-12.f, 12.f);
            }
            keep.push_back(h);
        }
        out = keep;
        if (chance(0.6f)) { float s = rnd(lo, frontier + 15.f); out.push_back({s, s + (chance(0.6f) ? rnd(1.f, 6.f) : rnd(10.f, 110.f))}); }
    }
    for (auto& h : out) if (h.start < floor + 4.f && h.end > floor + 4.f && h.start >= lo - 20.f) h.start = floor + 4.f;
    tidy(out);
    return out;
}

// ---------------------------------------------------------------- the level
class $modify(CodaPlayLayer, PlayLayer) {
    struct Fields {
        CCNode* hud = nullptr;
        CCDrawNode* bar = nullptr;
        CCSprite* face = nullptr;
        CCLabelBMFont* pct = nullptr;
        CCLabelBMFont* info = nullptr;
        CCLabelBMFont* msg = nullptr;
        CCSprite* icon = nullptr;
        int iconMode = -1;
        int faceMood = -1;
        float smugTime = 0.f;
        float msgTime = 0.f;
        bool used = false;        // Coda played at some point while this level was open
        bool needReset = false;
        bool needApply = false;
        bool sloppy = false;      // this attempt had a deliberate slip
        bool held = false;
        size_t idx = 0;
        std::vector<Hold> play;   // the trial, maybe with a slip in it
        // training mode drawing
        CCDrawNode* paths = nullptr;     // best path, old generations, clicks, deaths
        CCDrawNode* live = nullptr;      // the attempt in progress
        std::vector<CCPoint> cur;
        std::vector<CCPoint> curClicks;
        CCDrawNode* graph = nullptr;
        CCLabelBMFont* genLabel = nullptr;
        float fpsTimer = 0.f;
    };

    Brain& brain() { return s_brains[levelKey(m_level)]; }

    bool init(GJGameLevel* level, bool dontCreateObjects) {
        if (!PlayLayer::init(level, dontCreateObjects)) return false;
        this->buildHud();
        applyFps();
        auto f = m_fields.self();
        f->paths = CCDrawNode::create();
        f->live = CCDrawNode::create();
        if (m_objectLayer) { m_objectLayer->addChild(f->paths, 9000); m_objectLayer->addChild(f->live, 9001); }
        f->graph = CCDrawNode::create();
        f->hud->addChild(f->graph);
        f->genLabel = CCLabelBMFont::create("", "chatFont.fnt");
        f->genLabel->setScale(0.6f);
        f->genLabel->setAnchorPoint({0.f, 0.5f});
        f->genLabel->setPosition({12.f, 46.f});
        f->hud->addChild(f->genLabel);
        if (s_enabled) this->codaStart();
        return true;
    }

    void buildHud() {
        auto f = m_fields.self();
        auto win = CCDirector::get()->getWinSize();
        f->hud = CCNode::create();
        f->hud->setZOrder(1000);
        this->addChild(f->hud);
        f->bar = CCDrawNode::create();
        f->hud->addChild(f->bar);
        f->pct = CCLabelBMFont::create("0%", "bigFont.fnt");
        f->pct->setScale(0.45f);
        f->pct->setAnchorPoint({0.f, 0.5f});
        f->pct->setPosition({win.width / 2 + 128.f, win.height - 42.f});
        f->hud->addChild(f->pct);
        auto name = CCLabelBMFont::create("CODA", "bigFont.fnt");
        name->setScale(0.35f);
        name->setAnchorPoint({1.f, 0.5f});
        name->setPosition({win.width / 2 - 128.f, win.height - 42.f});
        f->hud->addChild(name);
        f->info = CCLabelBMFont::create("", "chatFont.fnt");
        f->info->setScale(0.6f);
        f->info->setPosition({win.width / 2, win.height - 60.f});
        f->hud->addChild(f->info);
        f->msg = CCLabelBMFont::create("", "goldFont.fnt");
        f->msg->setScale(0.6f);
        f->msg->setPosition({win.width / 2, win.height - 84.f});
        f->hud->addChild(f->msg);
        f->face = CCSprite::create("coda-face.png"_spr);
        if (f->face) { f->face->setScale(0.85f); f->hud->addChild(f->face, 2); }
        f->hud->setVisible(false);
    }

    void say(std::string const& text, float secs = 2.f) {
        auto f = m_fields.self();
        if (!f->msg) return;
        f->msg->setString(text.c_str());
        f->msgTime = secs;
    }

    // Coda takes over: nothing from here on can count for this level.
    void codaStart() {
        auto f = m_fields.self();
        f->used = true;
        m_isTestMode = true;
        if (f->hud) f->hud->setVisible(true);
        if (m_isPlatformer) { say("Coda only plays classic levels.", 3.f); return; }
        f->needApply = true;      // switch practice mode on the next frame, not in the middle of setup
    }

    // Slow plays every attempt from the start; High and Extreme practice from checkpoints.
    void applyMode() {
        auto& b = this->brain();
        this->removeAllCheckpoints();
        b.cpX = 0.f;
        b.confirm = mode().checkpoints && b.bestX > 0.f;
        if (m_isPracticeMode != mode().checkpoints) this->togglePracticeMode(mode().checkpoints);
    }

    void codaStop() {
        auto f = m_fields.self();
        this->release();
        if (f->hud) f->hud->setVisible(false);
        this->restoreIcon();
        CCDirector::get()->getScheduler()->setTimeScale(1.f);
        if (f->paths) { f->paths->clear(); f->live->clear(); f->graph->clear(); f->genLabel->setVisible(false); }
        this->refreshDebugDraw();
        if (!m_isPlatformer) {
            this->removeAllCheckpoints();
            this->brain().cpX = 0.f;
            if (m_isPracticeMode) this->togglePracticeMode(false);
        }
    }

    void release() {
        auto f = m_fields.self();
        if (f->held && m_player1) {
            s_fromBot = true;
            this->handleButton(false, 1, true);
            s_fromBot = false;
        }
        f->held = false;
    }

    CCPoint playerPos() {
        auto p = m_player1;
        if (!p || !p->getParent() || !m_objectLayer) return {0, 0};
        return m_objectLayer->convertToNodeSpace(p->getParent()->convertToWorldSpace(p->getPosition()));
    }

    // called every physics step, before the game moves the player
    void codaStep() {
        auto f = m_fields.self();
        if (!s_enabled || m_isPlatformer || !m_player1 || m_player1->m_isDead) return;
        float x = m_player1->m_position.x;
        auto& p = f->play;
        while (f->idx < p.size() && p[f->idx].end <= x) f->idx++;
        bool want = f->idx < p.size() && p[f->idx].start <= x && x < p[f->idx].end;
        if (want != f->held) {
            s_fromBot = true;
            this->handleButton(want, 1, true);
            s_fromBot = false;
            f->held = want;
            if (want && s_training) f->curClicks.push_back(this->playerPos());
        }
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        auto f = m_fields.self();
        f->idx = 0;
        f->held = false;
        if (!s_enabled || m_isPlatformer) return;
        auto& b = this->brain();
        b.attempts++;
        f->cur.clear(); f->curClicks.clear();
        if (f->live) f->live->clear();
        if (b.cpX > 0.f) for (auto& pt : b.bestPath) { if (pt.x < this->playerPos().x) f->cur.push_back(pt); else break; }
        f->play = (b.confirm || b.trial.empty()) ? b.best : b.trial;
        f->sloppy = false;
        // He's good, not perfect: sometimes he fumbles a jump he already knows.
        if (!b.confirm && b.bestX > 300.f && !b.best.empty() && chance(b.beaten ? mode().slipBeaten : mode().slip)) {
            std::vector<size_t> known;
            for (size_t i = 0; i < f->play.size(); i++) if (f->play[i].start < b.bestX - 90.f && f->play[i].start > b.cpX + 10.f) known.push_back(i);
            if (!known.empty()) {
                auto& h = f->play[known[(size_t)rnd(0.f, (float)known.size() - 0.001f)]];
                float d = (chance(0.5f) ? 1.f : -1.f) * rnd(4.f, 10.f);
                h.start += d; h.end += d;
                f->sloppy = true;
            }
        }
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (s_enabled && !m_isPlatformer && player == m_player1 && object != m_anticheatSpike && !player->m_isDead) {
            this->onCodaDeath(player->m_position.x, this->getCurrentPercent());
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void onCodaDeath(float x, float pct) {
        auto& b = this->brain();
        float oldBest = b.bestX;
        this->learnFromDeath(x, pct);
        this->trainingBookkeeping(x, pct, b.bestX > oldBest + 0.5f);
    }

    void trainingBookkeeping(float x, float pct, bool newBest) {
        auto f = m_fields.self();
        auto& b = this->brain();
        auto here = this->playerPos();
        f->cur.push_back(here);
        b.deaths.push_back(here);
        while (b.deaths.size() > 80) b.deaths.pop_front();
        if (newBest) { b.bestPath = f->cur; b.bestClicks = f->curClicks; }
        if (x > b.genBestX) { b.genBestX = x; b.genBestPct = pct; b.genBestPath = f->cur; }
        if (b.attempts % perGen() == 0) {               // a generation is done
            b.gen++;
            b.genHistory.push_back(std::max(b.genBestPct, 0.f));
            if (!b.genBestPath.empty()) b.oldPaths.push_back(b.genBestPath);
            while ((int)b.oldPaths.size() > oldPathCount()) b.oldPaths.pop_front();
            b.genBestX = 0.f; b.genBestPct = 0.f; b.genBestPath.clear();
            if (b.skipUntil >= 0 && b.gen >= b.skipUntil) {
                b.skipUntil = -1;
                s_speedIdx = 0;
                say("Done skipping. Watch this.", 3.f);
            }
        }
        this->redrawPaths();
    }

    void learnFromDeath(float x, float pct) {
        auto f = m_fields.self();
        auto& b = this->brain();
        this->release();
        if (f->sloppy && x < b.bestX) {
            say(chance(0.5f) ? "...that was on purpose." : "Slipped.");
            b.trial = b.best;     // the slip wasn't a real lesson; try his real plan again
            return;
        }
        if (b.confirm) {          // the replay that sets his checkpoint; not an experiment
            b.confirm = false;
            if (x > b.bestX + 0.5f) { b.bestX = x; b.bestPct = pct; }
            b.trial = mutate(b.best, b.bestX, b.stuck, b.sweep, b.cpX);
            return;
        }
        if (x > b.bestX + 0.5f) {
            b.best = f->play;
            b.bestX = x;
            b.bestPct = pct;
            if (b.stuck > 6) say("Finally.");
            else if (b.attempts > 1) say("New best: " + std::to_string((int)pct) + "%");
            b.stuck = 0;
            b.sweep = 0;
            f->smugTime = 1.5f;
            if (mode().checkpoints && b.bestX - mode().cpMargin > b.cpX + 60.f && b.bestX > b.noCpBefore) b.confirm = true;
        } else {
            b.stuck++;
            if (b.stuck == 15) say("This spike again.");
            if (b.stuck == 40) say("I hate this part.");
            // Stuck too long from this checkpoint: maybe the mistake is earlier. Go back to full runs.
            if (mode().checkpoints && b.cpX > 0.f && b.stuck % mode().cpPatience == 0) {
                this->removeAllCheckpoints();
                b.cpX = 0.f;
                b.noCpBefore = b.bestX + 30.f;
                say("Rethinking the whole thing.");
            }
        }
        b.trial = mutate(b.best, b.bestX, b.stuck, b.sweep, b.cpX);
    }

    // During the confirm replay, drop a checkpoint a bit before his best.
    void maybeCheckpoint() {
        auto& b = this->brain();
        if (!b.confirm || !mode().checkpoints || !m_isPracticeMode || !m_player1 || m_player1->m_isDead) return;
        float x = m_player1->m_position.x;
        float target = b.bestX - mode().cpMargin;
        if (x >= target && target > b.cpX + 60.f) {
            this->removeAllCheckpoints();
            this->markCheckpoint();
            b.cpX = x;
        }
    }

    static void polyline(CCDrawNode* n, std::vector<CCPoint> const& pts, float r, ccColor4F c, bool dashed = false) {
        for (size_t i = 1; i < pts.size(); i++) {
            if (dashed && (i / 3) % 2) continue;
            n->drawSegment(pts[i - 1], pts[i], r, c);
        }
    }
    static void xmark(CCDrawNode* n, CCPoint p, ccColor4F c) {
        n->drawSegment(p + ccp(-5, -5), p + ccp(5, 5), 1.4f, c);
        n->drawSegment(p + ccp(-5, 5), p + ccp(5, -5), 1.4f, c);
    }

    void redrawPaths() {
        auto f = m_fields.self();
        if (!f->paths) return;
        f->paths->clear();
        f->paths->setVisible(s_enabled && s_training);
        f->live->setVisible(s_enabled && s_training);
        if (!s_enabled || !s_training) return;
        auto& b = this->brain();
        int n = (int)b.oldPaths.size();
        for (int i = 0; i < n; i++) {                   // older generations: faded, blue to purple
            float t = n > 1 ? (float)i / (n - 1) : 1.f;
            polyline(f->paths, b.oldPaths[i], 1.2f, ccc4f(0.49f + 0.22f * t, 0.78f - 0.17f * t, 1.f, 0.25f + 0.3f * t));
        }
        for (auto& d : b.deaths) xmark(f->paths, d, ccc4f(1.f, 0.42f, 0.42f, 0.75f));
        polyline(f->paths, b.bestPath, 2.2f, ccc4f(0.96f, 0.64f, 0.71f, 1.f));
        for (auto& c : b.bestClicks) { f->paths->drawDot(c, 3.2f, ccc4f(0.14f, 0.13f, 0.11f, 1.f)); f->paths->drawDot(c, 2.4f, ccc4f(1.f, 0.88f, 0.29f, 1.f)); }
    }

    void updateTraining(float dt) {
        auto f = m_fields.self();
        auto& b = this->brain();
        if (f->live && s_training && m_player1 && !m_player1->m_isDead) {
            auto here = this->playerPos();
            if (f->cur.empty() || ccpDistance(f->cur.back(), here) > 4.f) {
                if (!f->cur.empty() && (f->cur.size() / 3) % 2 == 0) f->live->drawSegment(f->cur.back(), here, 1.1f, ccc4f(1, 1, 1, 0.75f));
                f->cur.push_back(here);
            }
        }
        // generation panel (bottom left of the screen)
        f->graph->clear();
        f->genLabel->setVisible(s_training);
        if (!s_training) return;
        int inGen = b.attempts % perGen();
        std::string g = "Generation " + std::to_string(b.gen + 1) + "   Attempt " + std::to_string(inGen == 0 ? perGen() : inGen) + "/" +
            std::to_string(perGen()) + "   Best this gen " + std::to_string((int)b.genBestPct) + "%   Speed " +
            std::to_string((int)SPEEDS[s_speedIdx]) + "x" + (b.skipUntil >= 0 ? "   Skipping to gen " + std::to_string(b.skipUntil + 1) + "..." : "");
        f->genLabel->setString(g.c_str());
        float gx = 12.f, gy = 14.f, gw = 160.f, gh = 22.f;
        f->graph->drawRect(ccp(gx - 4, gy - 4), ccp(gx + gw + 4, gy + gh + 4), ccc4f(0, 0, 0, 0.35f), 0.f, ccc4f(0, 0, 0, 0));
        auto& hist = b.genHistory;
        for (size_t i = 1; i < hist.size(); i++) {
            float x1 = gx + gw * (i - 1) / std::max<size_t>(1, hist.size() - 1), x2 = gx + gw * i / std::max<size_t>(1, hist.size() - 1);
            f->graph->drawSegment(ccp(x1, gy + gh * hist[i - 1] / 100.f), ccp(x2, gy + gh * hist[i] / 100.f), 1.f, ccc4f(0.96f, 0.64f, 0.71f, 1.f));
        }
    }

    // GD's own hitbox drawing, switched on while Coda plays
    bool shouldDrawDebugHitboxes() {
        if (s_enabled && s_hitboxes) return true;
        return PlayLayer::shouldDrawDebugHitboxes();
    }
    bool shouldDrawHitboxTrail() {
        if (s_enabled && s_trail) return true;
        return PlayLayer::shouldDrawHitboxTrail();
    }
    bool shouldRecordHitboxTrail() {
        if (s_enabled && s_trail) return true;
        return PlayLayer::shouldRecordHitboxTrail();
    }
    void refreshDebugDraw() {
        this->updateDebugDrawSettings();
        if (m_debugDrawNode) m_debugDrawNode->setVisible(s_enabled && (s_hitboxes || s_trail) ? true : m_debugDrawNode->isVisible());
        if (!(s_enabled && (s_hitboxes || s_trail)) && m_debugDrawNode && !PlayLayer::shouldDrawDebugHitboxes()) m_debugDrawNode->clear();
    }

    void levelComplete() {
        auto f = m_fields.self();
        if (!f->used) return PlayLayer::levelComplete();
        // Blocked: no completion, no rewards, no verifying.
        if (s_enabled) {
            auto& b = this->brain();
            b.beaten = true;
            b.bestPct = 100.f;
            b.best = f->play;
            b.bestX = m_player1 ? m_player1->m_position.x : b.bestX;
            b.trial = b.best;
            say("Coda beat it. (Doesn't count.)", 3.f);
        } else {
            say("Coda played this level, so it doesn't count. Reopen the level to play for real.", 4.f);
        }
        this->release();
        this->runAction(CCSequence::create(
            CCDelayTime::create(2.5f),
            CCCallFunc::create(this, callfunc_selector(CodaPlayLayer::codaRestart)),
            nullptr));
    }

    void codaRestart() { this->resetLevel(); }

    void onQuit() {
        CCDirector::get()->getScheduler()->setTimeScale(1.f);
        PlayLayer::onQuit();
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        auto f = m_fields.self();
        if (f->needApply) { f->needApply = false; if (s_enabled) this->applyMode(); f->needReset = true; }
        if (f->needReset) { f->needReset = false; this->resetLevel(); }
        if (!s_enabled) return;
        this->updateIcon();
        this->updateHud(dt);
        f->fpsTimer -= dt;
        if (f->fpsTimer <= 0.f) { f->fpsTimer = 2.f; applyFps(); }
        if (m_player1 && !m_isPlatformer) {
            this->maybeCheckpoint();
            this->updateTraining(dt);
            auto& b = this->brain();
            bool known = m_player1->m_position.x < b.bestX - 300.f && !m_player1->m_isDead;
            float speed = known ? mode().knownSpeed : mode().frontSpeed;
            if (s_training) speed = b.skipUntil >= 0 ? SPEEDS[3] : SPEEDS[s_speedIdx];
            CCDirector::get()->getScheduler()->setTimeScale(speed);
        }
    }

    void updateHud(float dt) {
        auto f = m_fields.self();
        if (!f->bar) return;
        auto& b = this->brain();
        auto win = CCDirector::get()->getWinSize();
        float w = 240.f, h = 9.f, x0 = win.width / 2 - w / 2, y0 = win.height - 42.f - h / 2;
        float now = std::clamp(this->getCurrentPercent() / 100.f, 0.f, 1.f);
        float best = std::clamp(b.bestPct / 100.f, 0.f, 1.f);
        f->bar->clear();
        f->bar->drawRect(ccp(x0, y0), ccp(x0 + w, y0 + h), ccc4f(1, 1, 1, 0.15f), 1.f, ccc4f(1, 1, 1, 0.9f));
        if (best > 0) f->bar->drawRect(ccp(x0, y0), ccp(x0 + w * best, y0 + h), ccc4f(0.96f, 0.64f, 0.71f, 0.4f), 0.f, ccc4f(0, 0, 0, 0));
        if (now > 0) f->bar->drawRect(ccp(x0, y0), ccp(x0 + w * now, y0 + h), ccc4f(0.96f, 0.64f, 0.71f, 1.f), 0.f, ccc4f(0, 0, 0, 0));
        if (best > 0) f->bar->drawRect(ccp(x0 + w * best - 1, y0 - 4), ccp(x0 + w * best + 1, y0 + h + 4), ccc4f(1, 1, 1, 1), 0.f, ccc4f(0, 0, 0, 0));
        if (f->face) f->face->setPosition({x0 + w * now, y0 + h / 2});
        f->pct->setString((std::to_string((int)(now * 100)) + "%").c_str());
        std::string info = "Best " + std::to_string((int)b.bestPct) + "%   Attempt " + std::to_string(b.attempts) +
            (b.beaten ? "   Beaten (doesn't count)" : "   Learning...") + "   [" + mode().name + "]";
        f->info->setString(info.c_str());

        f->smugTime -= dt;
        int mood = f->smugTime > 0 ? 2 : (b.stuck >= 10 ? 1 : 0);
        if (mood != f->faceMood && f->face) {
            f->faceMood = mood;
            char const* names[] = {"coda-face.png"_spr, "coda-face-mad.png"_spr, "coda-face-smug.png"_spr};
            if (auto tex = CCTextureCache::sharedTextureCache()->addImage(names[mood], false)) {
                f->face->setTexture(tex);
                f->face->setTextureRect({0, 0, tex->getContentSize().width, tex->getContentSize().height});
            }
        }
        if (f->msgTime > 0) { f->msgTime -= dt; if (f->msgTime <= 0) f->msg->setString(""); }
    }

    // ------------------------------------------------ his icons (only while he plays)
    int iconOf(PlayerObject* p) {
        if (p->m_isShip) return 1;
        if (p->m_isBall) return 2;
        if (p->m_isBird) return 3;
        if (p->m_isDart) return 4;
        if (p->m_isRobot) return 5;
        if (p->m_isSpider) return 6;
        if (p->m_isSwing) return 7;
        return 0;
    }

    void updateIcon() {
        auto f = m_fields.self();
        auto p = m_player1;
        if (!p || !p->m_mainLayer) return;
        int mode = this->iconOf(p);
        if (mode != f->iconMode || !f->icon) {
            if (f->icon) f->icon->removeFromParent();
            char const* names[] = {"coda-cube.png"_spr, "coda-ship.png"_spr, "coda-ball.png"_spr, "coda-ufo.png"_spr,
                                   "coda-wave.png"_spr, "coda-robot.png"_spr, "coda-spider.png"_spr, "coda-swing.png"_spr};
            f->icon = CCSprite::create(names[mode]);
            f->iconMode = mode;
            if (f->icon) p->addChild(f->icon, 10);
        }
        p->m_mainLayer->setVisible(false);
        if (p->m_robotSprite) p->m_robotSprite->setVisible(false);
        if (p->m_spiderSprite) p->m_spiderSprite->setVisible(false);
        if (f->icon) {
            float base = (mode == 0 || mode == 2) ? 0.8f : 0.77f;    // sprites are 128px UHD = 32 units
            f->icon->setPosition(p->m_mainLayer->getPosition());
            f->icon->setScaleX(base * p->m_vehicleSize * (p->m_mainLayer->getScaleX() < 0 ? -1.f : 1.f));
            f->icon->setScaleY(base * p->m_vehicleSize * (p->m_mainLayer->getScaleY() < 0 ? -1.f : 1.f));
            f->icon->setRotation(p->m_mainLayer->getRotation());
            f->icon->setVisible(!p->m_isDead && !p->m_isHidden);
        }
    }

    void restoreIcon() {
        auto f = m_fields.self();
        if (f->icon) { f->icon->removeFromParent(); f->icon = nullptr; f->iconMode = -1; }
        auto p = m_player1;
        if (!p || !p->m_mainLayer) return;
        p->m_mainLayer->setVisible(true);
        if (p->m_robotSprite) p->m_robotSprite->setVisible(p->m_isRobot);
        if (p->m_spiderSprite) p->m_spiderSprite->setVisible(p->m_isSpider);
    }
};

// ---------------------------------------------------------------- input
class $modify(GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto pl = PlayLayer::get();
        if (pl && pl == static_cast<GJBaseGameLayer*>(this) && s_enabled && !pl->m_isPlatformer && !s_fromBot) {
            return;   // Coda has the controls
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto pl = PlayLayer::get();
        if (pl && pl == static_cast<GJBaseGameLayer*>(this)) static_cast<CodaPlayLayer*>(pl)->codaStep();
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

// ---------------------------------------------------------------- pause menu buttons
class $modify(CodaPauseLayer, PauseLayer) {
    struct Fields {
        CCLabelBMFont* label = nullptr;
        std::vector<std::pair<CCLabelBMFont*, std::function<std::string()>>> texts;
    };

    CCLabelBMFont* addText(CCMenu* menu, CCPoint pos, SEL_MenuHandler cb, std::function<std::string()> text) {
        auto lbl = CCLabelBMFont::create(text().c_str(), "bigFont.fnt");
        lbl->setScale(0.3f);
        auto btn = CCMenuItemSpriteExtra::create(lbl, this, cb);
        btn->setPosition(pos);
        menu->addChild(btn);
        m_fields->texts.push_back({lbl, text});
        return lbl;
    }

    void customSetup() {
        PauseLayer::customSetup();
        auto win = CCDirector::get()->getWinSize();
        auto bg = CCLayerColor::create({0, 0, 0, 110}, 250.f, 118.f);
        bg->setPosition({win.width - 258.f, 6.f});
        this->addChild(bg, 49);
        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        this->addChild(menu, 50);

        auto face = CCSprite::create("coda-face.png"_spr);
        if (face) face->setScale(1.4f);
        auto btn = CCMenuItemSpriteExtra::create(face ? (CCNode*)face : (CCNode*)CCLabelBMFont::create("Coda", "bigFont.fnt"),
                                                 this, menu_selector(CodaPauseLayer::onCoda));
        btn->setPosition({win.width - 42.f, 60.f});
        menu->addChild(btn);
        m_fields->label = CCLabelBMFont::create("", "bigFont.fnt");
        m_fields->label->setScale(0.3f);
        m_fields->label->setPosition({win.width - 42.f, 26.f});
        this->addChild(m_fields->label, 50);

        float cx = win.width - 165.f;
        addText(menu, {cx, 112.f}, menu_selector(CodaPauseLayer::onMode), [] { return std::string("Learning: ") + mode().name; });
        addText(menu, {cx, 94.f}, menu_selector(CodaPauseLayer::onTraining), [] { return std::string("Training: ") + (s_training ? "ON" : "OFF"); });
        addText(menu, {cx, 76.f}, menu_selector(CodaPauseLayer::onSpeed), [] { return "Speed: " + std::to_string((int)SPEEDS[s_speedIdx]) + "x"; });
        addText(menu, {cx - 52.f, 58.f}, menu_selector(CodaPauseLayer::onSkip1), [] { return std::string("+1 gen"); });
        addText(menu, {cx + 4.f, 58.f}, menu_selector(CodaPauseLayer::onSkip5), [] { return std::string("+5"); });
        addText(menu, {cx + 44.f, 58.f}, menu_selector(CodaPauseLayer::onSkip10), [] { return std::string("+10"); });
        addText(menu, {cx, 40.f}, menu_selector(CodaPauseLayer::onHitboxes), [] { return std::string("Hitboxes: ") + (s_hitboxes ? "ON" : "OFF"); });
        addText(menu, {cx, 22.f}, menu_selector(CodaPauseLayer::onTrail), [] { return std::string("Trail: ") + (s_trail ? "ON" : "OFF"); });
        this->refresh();
    }

    void refresh() {
        if (m_fields->label) m_fields->label->setString(s_enabled ? "Coda: ON" : "Coda: OFF");
        for (auto& [lbl, text] : m_fields->texts) lbl->setString(text().c_str());
    }

    CodaPlayLayer* pl() { return static_cast<CodaPlayLayer*>(PlayLayer::get()); }

    void turnOn() {
        if (s_enabled) return;
        s_enabled = true;
        if (auto p = pl()) p->codaStart();
    }

    void onCoda(CCObject*) {
        if (s_enabled) {
            s_enabled = false;
            if (auto p = pl()) p->codaStop();
        } else this->turnOn();
        if (auto p = pl()) { p->redrawPaths(); p->refreshDebugDraw(); }
        this->refresh();
    }

    void onMode(CCObject*) {
        s_mode = (s_mode + 1) % 3;
        if (auto p = pl()) if (s_enabled && !p->m_isPlatformer) p->m_fields->needApply = true;
        this->refresh();
    }

    void onTraining(CCObject*) {
        s_training = !s_training;
        if (s_training) this->turnOn();
        if (auto p = pl()) p->redrawPaths();
        this->refresh();
    }

    void onSpeed(CCObject*) {
        s_speedIdx = (s_speedIdx + 1) % 4;
        this->refresh();
    }

    void skip(int n) {
        s_training = true;
        this->turnOn();
        if (auto p = pl()) {
            auto& b = p->brain();
            b.skipUntil = b.gen + n;
            p->redrawPaths();
        }
        this->refresh();
    }
    void onSkip1(CCObject*) { this->skip(1); }
    void onSkip5(CCObject*) { this->skip(5); }
    void onSkip10(CCObject*) { this->skip(10); }

    void onHitboxes(CCObject*) {
        s_hitboxes = !s_hitboxes;
        if (auto p = pl()) p->refreshDebugDraw();
        this->refresh();
    }
    void onTrail(CCObject*) {
        s_trail = !s_trail;
        if (auto p = pl()) p->refreshDebugDraw();
        this->refresh();
    }
};
