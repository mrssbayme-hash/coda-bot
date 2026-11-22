// Coda Bot: Coda learns levels by trial and error.
// His runs never count: the level is put in test mode, completions are blocked, and so is verifying.
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/EnhancedGameObject.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <random>
#include <deque>
#include <functional>
#include <chrono>
#include <unordered_set>
#include <cstring>
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
    std::vector<float> cps;      // all his checkpoints, oldest first
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
    bool finalExam = false;                      // Perfect: beat it with checkpoints, now prove it from the start
    bool examRun = false;                        // this attempt is the first full run of the final exam
    // Perfect: pathfinding. A decision every few units (hold or let go), each with a checkpoint
    // so he can back up and try the other choice when a branch dies.
    struct Node { float x; bool press; unsigned char tried; bool cp; bool ground; float y; };
    std::vector<CCRect> orbs;    // orb hitboxes: the only places a mid-air click matters for cube/ball/robot/spider
    float sameX = -1e9f;         // where he keeps dying
    int sameCount = 0;
    int flyBackups = 0;          // how many times he's backed up at this same spot while flying
    int routeMode = 0;           // 0 = none, 1 = high route, 2 = low route
    int routeTries = 0;
    float routeY = 0.f, routeToX = 0.f;
    std::vector<Node> nodes;
    bool pfFailed = false;
    int pfDeaths = 0;
    std::vector<Hold> guide;    // a path he found before; tried first when pathfinding again
    bool planDone = false;      // Perfect: the planner already ran on this level
    bool planFailed = false;    // ...and couldn't reach the end, so use pathfinding instead
};
static float const PF_STEP = 12.f;     // units between decisions
static int const PF_WINDOW = 400;      // how many decisions back he can rewind

// ---------------------------------------------------------------- learning modes
struct Mode {
    char const* name;
    bool checkpoints;     // practice from just before the hard part
    float cpMargin;       // how far before his best the checkpoint goes
    int cpPatience;       // deaths in one place before he backs up a checkpoint
    float knownSpeed;     // game speed through parts he already knows
    float frontSpeed;     // game speed near the hard part
    float slip;           // chance of fumbling a known part
    float slipBeaten;
    bool perfect;         // only counts it as beaten after a clean run from the very start
};
static Mode const MODES[] = {
    {"Slow",    false, 0.f,   0,  1.f, 1.f, 0.14f, 0.30f, false},
    {"High",    true,  210.f, 12, 3.f, 1.f, 0.07f, 0.20f, false},
    {"Extreme", true,  120.f, 8,  5.f, 2.f, 0.03f, 0.10f, false},
    {"Perfect", true,  120.f, 8,  5.f, 2.f, 0.f,   0.f,   true},
};
static int const MODE_COUNT = 4;
static int s_mode = 3;              // Perfect (no fumbles)
static bool s_pathOnly = true;      // for now Coda only pathfinds, at normal speed
static std::string s_thought;      // what Coda is thinking right now (shown in a bubble)
static bool s_showThoughts = true;
static bool s_menuOpen = false;
static bool s_analyze = false;       // working out the route behind a loading screen
static bool s_analyzeNext = false;   // start analyzing as soon as the level opens
static bool s_training = false;
static int s_speedIdx = 1;
static float const SPEEDS[] = {1.f, 4.f, 10.f, 20.f};
static bool s_auto = false;        // auto train: turbo until he beats it
static int s_modeBeforeAuto = 1;
static float s_turbo = 20.f;       // adapts to how fast the PC can keep up
static bool s_hitboxes = false;
// planner simulation: while s_sim is on, the game is moving an invisible copy of the player
static bool s_sim = false;
static bool s_ghostDead = false;
static bool s_ghostDone = false;
static PlayerObject* s_simGhost = nullptr;
static std::unordered_set<GameObject*>* s_simActivated = nullptr;
static int const PLAN_BEAM = 14;      // how many candidate runs he keeps at once
static int const PLAN_STEPS = 8;      // physics steps per decision (1/30 s)
static bool s_trail = false;
static int perGen() { return std::max(1, (int)Mod::get()->getSettingValue<int64_t>("attempts-per-generation")); }
static int oldPathCount() { return std::max(0, (int)Mod::get()->getSettingValue<int64_t>("old-paths")); }
// ---------------------------------------------------------------- physics hertz (TPS) bypass
// GD 2.2091 runs physics at 240 steps a second, set by two constants used in GJBaseGameLayer:
//   getModifiedDelta:  timestep = 1/240        (instruction at 0x340671)
//   update:            steps    = delta*60*4.0 (instruction at 0x340900)
// We point those two instructions at our own values (stored in unused padding bytes),
// so the original constants that other code shares stay untouched.
static int s_tps = 240;
static std::vector<Patch*> s_tpsPatches;
static bool s_tpsBroken = false;
static int const TPS_STEPS[] = {240, 60, 120, 144, 165, 360, 480, 1000};

static bool setTps(int tps) {
    if (s_tpsBroken) return false;
    uintptr_t base = geode::base::get();
    auto bytesOf = [](auto v) { ByteVector b(sizeof(v)); std::memcpy(b.data(), &v, sizeof(v)); return b; };
    if (s_tpsPatches.empty()) {
        // only patch the exact build we expect (Steam 2.2091)
        static unsigned char const want1[] = {0xf2, 0x0f, 0x59, 0x3d, 0xc7, 0x49, 0x4f, 0x00};
        static unsigned char const want2[] = {0xf2, 0x0f, 0x59, 0x0d, 0xe8, 0x49, 0x4f, 0x00};
        auto pad1 = reinterpret_cast<unsigned char*>(base + 0x3406c8);
        auto pad2 = reinterpret_cast<unsigned char*>(base + 0x33cf38);
        bool ok = std::memcmp((void*)(base + 0x340671), want1, 8) == 0 && std::memcmp((void*)(base + 0x340900), want2, 8) == 0;
        for (int i = 0; i < 8 && ok; i++) ok = pad1[i] == 0xcc && pad2[i] == 0xcc;
        if (!ok) { s_tpsBroken = true; return false; }
        auto add = [&](uintptr_t rva, ByteVector b) {
            auto r = Mod::get()->patch((void*)(base + rva), b);
            if (r.isOk()) s_tpsPatches.push_back(r.unwrap()); else s_tpsBroken = true;
        };
        add(0x3406c8, bytesOf(1.0 / 240.0));
        add(0x33cf38, bytesOf(4.0));
        add(0x340675, bytesOf((int32_t)(0x3406c8 - 0x340679)));
        add(0x340904, bytesOf((int32_t)(0x33cf38 - 0x340908)));
        if (s_tpsBroken) return false;
    }
    (void)s_tpsPatches[0]->updateBytes(bytesOf(1.0 / (double)tps));
    (void)s_tpsPatches[1]->updateBytes(bytesOf((double)tps / 60.0));
    for (auto p : s_tpsPatches) (void)p->toggle(tps != 240);
    s_tps = tps;
    return true;
}

static void applyFps() {
    if (!Mod::get()->getSettingValue<bool>("fps-bypass")) return;
    double fps = (double)std::max<int64_t>(30, Mod::get()->getSettingValue<int64_t>("fps"));
    CCDirector::get()->setAnimationInterval(1.0 / fps);
}
static Mode const& mode() { return MODES[s_mode]; }
static bool pathfinding(Brain const& b) { return (s_pathOnly || (mode().perfect && b.planFailed)) && !b.finalExam && !b.pfFailed; }
static std::vector<Hold> holdsFrom(std::vector<Brain::Node> const& nodes) {
    std::vector<Hold> out;
    for (size_t i = 0; i < nodes.size(); i++) {
        if (!nodes[i].press) continue;
        float end = i + 1 < nodes.size() ? nodes[i + 1].x : nodes[i].x + PF_STEP;
        if (!out.empty() && std::abs(out.back().end - nodes[i].x) < 0.5f) out.back().end = end;
        else out.push_back({nodes[i].x, end});
    }
    return out;
}

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
static std::vector<Hold> mutate(std::vector<Hold> const& best, float frontier, int stuck, int& sweep, float floor, bool keepTail = false) {
    static float const nudges[] = {2, -2, 4, -4, 6, -6, 9, -9, 13, -13, 18, -18};
    std::vector<Hold> out;
    for (auto h : best) if (keepTail || h.start <= frontier + 30.f) out.push_back(h);   // forget what was never reached (unless it's a known plan)
    float window = 45.f + std::min(stuck, 40) * 7.f;
    if (stuck > 25 && chance(0.25f)) window *= 2.f;
    float lo = std::max(floor + 4.f, frontier - window);

    int last = -1;
    for (int i = 0; i < (int)out.size(); i++) if (out[i].start >= lo && out[i].start < frontier) last = i;
    float r = rnd(0.f, 1.f);
    if (last >= 0 && r < 0.5f) {
        float d = nudges[sweep % 12] * (1.f + (sweep / 12) * 0.5f);
        sweep++;
        s_thought = "Next: jump " + std::to_string((int)std::abs(d)) + (d < 0 ? " units earlier" : " units later") + " before the spike.";
        out[last].start += d;
        if (chance(0.5f)) out[last].end += d; else out[last].end += rnd(-8.f, 8.f);
    } else if (r < 0.75f) {
        s_thought = "Next: an extra click right before where I died.";
        float s = std::max(lo, frontier - rnd(4.f, 45.f));
        float len = chance(0.65f) ? rnd(1.f, 6.f) : rnd(10.f, 90.f);
        out.push_back({s, s + len});
    } else {
        s_thought = "Next: shaking up my last few jumps.";
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
        bool exitPractice = false;
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
        // planner
        struct PNode { int parent; bool press; float x0; CCPoint end; };
        struct BeamE { int node; PlayerObject* g; float clear; bool held; std::unordered_set<GameObject*> act; };
        bool needPlan = false;
        bool planning = false;
        float planShow = 0.f;            // seconds left showing the finished plan
        std::vector<PlayerObject*> pool;
        std::vector<PlayerObject*> freeGhosts;
        std::vector<PNode> arena;
        std::vector<BeamE> beam;
        std::vector<CCRect> hazards;
        std::vector<std::pair<CCPoint, CCPoint>> tries;
        std::vector<CCPoint> planPath;
        std::vector<CCPoint> planClicks;
        int checked = 0;
        float planX = 0.f;
        float planClear = 0.f;
        CCDrawNode* planNode = nullptr;
        CCNode* bubble = nullptr;
        CCLayerColor* bubbleBg = nullptr;
        CCLabelBMFont* bubbleText = nullptr;
        bool tpsUsed = false;
        bool needAnalyze = false;
        CCLayerColor* overlay = nullptr;
        CCLabelBMFont* anaTitle = nullptr;
        CCLabelBMFont* anaInfo = nullptr;
        CCLabelBMFont* anaThought = nullptr;
        CCDrawNode* anaBar = nullptr;
    };

    Brain& brain() { return s_brains[levelKey(m_level)]; }

    bool init(GJGameLevel* level, bool dontCreateObjects) {
        if (!PlayLayer::init(level, dontCreateObjects)) return false;
        this->buildHud();
        applyFps();
        if (s_tps == 240) {
            int saved = (int)Mod::get()->getSavedValue<int64_t>("tps", 240);
            if (saved != 240) setTps(saved);
        }
        if (s_tps != 240) { m_isTestMode = true; m_fields->tpsUsed = true; }
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
        f->bubble = CCNode::create();
        f->bubbleBg = CCLayerColor::create({0, 0, 0, 150}, 10.f, 10.f);
        f->bubbleText = CCLabelBMFont::create("", "chatFont.fnt");
        f->bubbleText->setScale(0.5f);
        f->bubble->addChild(f->bubbleBg);
        f->bubble->addChild(f->bubbleText);
        f->bubble->setVisible(false);
        if (m_objectLayer) m_objectLayer->addChild(f->bubble, 9100);
        if (s_analyzeNext) { s_analyzeNext = false; s_enabled = true; m_fields->needAnalyze = true; }
        if (s_enabled) this->codaStart();
        if (m_fields->tpsUsed) this->say("Physics at " + std::to_string(s_tps) + " Hz: practice only, progress won't save.", 3.f);
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
        if (s_pathOnly) {
            this->removeAllCheckpoints();
            b.cps.clear(); b.cpX = 0.f; b.confirm = false;
            if (!b.finalExam) { b.nodes.clear(); b.pfDeaths = 0; b.sameCount = 0; }
            b.orbs.clear();
            if (m_objects) for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
                if (!obj) continue;
                switch (obj->m_objectType) {
                    case GameObjectType::YellowJumpRing: case GameObjectType::PinkJumpRing: case GameObjectType::GravityRing:
                    case GameObjectType::GreenRing: case GameObjectType::DropRing: case GameObjectType::RedJumpRing:
                    case GameObjectType::CustomRing: case GameObjectType::DashRing: case GameObjectType::GravityDashRing:
                        b.orbs.push_back(obj->getObjectRect()); break;
                    default: break;
                }
            }
            bool practice = !b.finalExam;
            if (m_isPracticeMode != practice) this->togglePracticeMode(practice);
            s_thought = b.finalExam ? "Running my path from the start." : "Pathfinding. Every few units I choose: hold or let go.";
            return;
        }
        this->removeAllCheckpoints();
        b.cpX = 0.f;
        b.cps.clear();
        if (!mode().perfect && b.finalExam) { b.finalExam = false; b.examRun = false; b.noCpBefore = 0.f; }
        if (!mode().perfect) { b.pfFailed = false; b.nodes.clear(); b.planDone = false; b.planFailed = false; }
        bool practice = mode().checkpoints && !b.finalExam;
        if (mode().perfect && !b.planDone && !s_pathOnly) { practice = false; m_fields->needPlan = true; }
        if (mode().perfect && b.planDone && !b.planFailed) practice = false;
        b.confirm = practice && b.bestX > 0.f && !mode().perfect;
        if (pathfinding(b)) { b.nodes.clear(); b.confirm = false; b.pfDeaths = 0; }
        if (m_isPracticeMode != practice) this->togglePracticeMode(practice);
    }

    void codaStop() {
        auto f = m_fields.self();
        if (s_analyze) { s_analyze = false; if (f->overlay) f->overlay->setVisible(false); if (m_objectLayer) m_objectLayer->setVisible(true); }
        this->release();
        if (f->hud) f->hud->setVisible(false);
        this->restoreIcon();
        CCDirector::get()->getScheduler()->setTimeScale(1.f);
        if (f->paths) { f->paths->clear(); f->live->clear(); f->graph->clear(); f->genLabel->setVisible(false); }
        this->refreshDebugDraw();
        if (!m_isPlatformer) {
            this->removeAllCheckpoints();
            this->brain().cpX = 0.f;
            this->brain().cps.clear();
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
        if (f->planning || f->planShow > 0.f) return;
        float x = m_player1->m_position.x;
        auto& b = this->brain();
        if (pathfinding(b) && m_isPracticeMode) { this->pfStep(x); return; }
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

    void pfStep(float x) {
        auto f = m_fields.self();
        auto& b = this->brain();
        // In cube, ball and spider a choice in mid-air does nothing (unless an orb is near),
        // so he only makes decisions while on the ground. Far fewer checkpoints, and no wasted tries.
        int im = this->iconOf(m_player1);
        bool groundMode = im == 0 || im == 2 || im == 6;      // (robot needs mid-air holds, so it decides in the air too)
        bool onGround = m_player1->m_isOnGround;
        bool orbNear = false;
        if (groundMode && !onGround) {
            auto r = m_player1->getObjectRect();
            for (auto& o : b.orbs) if (std::abs(o.getMidX() - r.getMidX()) < 60.f && std::abs(o.getMidY() - r.getMidY()) < 60.f) { orbNear = true; break; }
        }
        bool decide = !groundMode || onGround || orbNear || b.nodes.empty();   // always have a first decision
        if (decide && (b.nodes.empty() || x >= b.nodes.back().x + PF_STEP)) {
            bool prev = !b.nodes.empty() && b.nodes.back().press;
            if (!b.guide.empty()) {                         // follow the path he found before, first
                prev = false;
                for (auto& h : b.guide) if (h.start <= x && x < h.end) { prev = true; break; }
            }
            if (b.routeMode && x < b.routeToX && !groundMode) {
                // Taking a different route: steer toward the route's height by default.
                float y = m_player1->m_position.y;
                bool up = y < b.routeY;
                prev = m_player1->m_isUpsideDown ? !up : up;
            }
            if (b.routeMode && x >= b.routeToX) b.routeMode = 0;
            Brain::Node n{x, prev, (unsigned char)(prev ? 2 : 1), false, onGround || !groundMode, m_player1->m_position.y};
            int before = m_checkpointArray ? (int)m_checkpointArray->count() : -1;
            auto made = this->markCheckpoint();
            int after = m_checkpointArray ? (int)m_checkpointArray->count() : -1;
            n.cp = made != nullptr && after > before;
            if (!n.cp) log::warn("[pf] checkpoint refused at x={:.1f} (array {} -> {})", x, before, after);
            int cps = 0;
            for (auto& o : b.nodes) if (o.cp) cps++;
            if (cps >= PF_WINDOW) {
                this->removeCheckpoint(true);
                for (auto& o : b.nodes) if (o.cp) { o.cp = false; break; }
            }
            b.nodes.push_back(n);
            log::debug("[pf] node #{} x={:.1f} y={:.1f} ground={} press={} gdCps={}", b.nodes.size(), x, n.y, n.ground, n.press,
                       m_checkpointArray ? (int)m_checkpointArray->count() : -1);
        }
        bool want = !b.nodes.empty() && b.nodes.back().press;
        if (want != f->held) {
            s_fromBot = true;
            this->handleButton(want, 1, true);
            s_fromBot = false;
            f->held = want;
            if (want && s_training) f->curClicks.push_back(this->playerPos());
        }
    }

    // A branch died: rewind to the latest decision that still has an untried choice.
    void pfDeath(float x, float pct) {
        auto& b = this->brain();
        log::info("[pf] death x={:.1f} pct={:.1f} nodes={} ourCps={} gdCps={} same={} top=({:.1f}, tried={}, cp={})", x, pct, b.nodes.size(),
                  std::count_if(b.nodes.begin(), b.nodes.end(), [](auto& n) { return n.cp; }),
                  m_checkpointArray ? (int)m_checkpointArray->count() : -1, b.sameCount,
                  b.nodes.empty() ? 0.f : b.nodes.back().x, b.nodes.empty() ? 0 : (int)b.nodes.back().tried, b.nodes.empty() ? false : b.nodes.back().cp);
        this->release();
        b.pfDeaths++;
        if (x > b.bestX + 0.5f) {
            b.bestX = x; b.bestPct = pct; b.pfDeaths = 0; b.flyBackups = 0; b.routeTries = 0;
            this->m_fields->smugTime = 1.f;
        }
        if (std::abs(x - b.sameX) < 30.f) b.sameCount++;
        else { b.sameX = x; b.sameCount = 1; }
        int im = m_player1 ? this->iconOf(m_player1) : 0;
        bool flying = im == 1 || im == 3 || im == 4 || im == 5 || im == 7;   // ship, UFO, wave, robot, swing
        if (b.sameCount >= 10 && flying) {
            // Flying: the mistake usually happened well before the thing he hits (momentum).
            // Back up in steps (ship: 130, then 180, 260, 360). If he's still stuck after all of those,
            // take a different route: go back further and fly a high or low line into the same area.
            b.sameCount = 0;
            static float const SHIP[] = {130.f, 180.f, 260.f, 360.f};
            static float const WAVE[] = {90.f, 120.f, 180.f, 260.f};
            static float const OTHER[] = {110.f, 150.f, 220.f, 320.f};
            float const* steps = im == 1 ? SHIP : im == 4 ? WAVE : OTHER;
            bool newRoute = b.flyBackups >= 4;
            float back = newRoute ? steps[3] + 120.f : steps[b.flyBackups];
            b.flyBackups = newRoute ? 0 : b.flyBackups + 1;
            float target = x - back;
            int i = (int)b.nodes.size() - 1;
            while (i > 0 && b.nodes[i].x > target) i--;
            while (i > 0 && b.nodes[i].tried == 3 && !newRoute) i--;
            if (i >= 0 && i < (int)b.nodes.size() && b.nodes[i].cp) {
                while ((int)b.nodes.size() > i + 1) { bool hc = b.nodes.back().cp; b.nodes.pop_back(); if (hc) this->removeCheckpoint(false); }
                auto& n = b.nodes.back();
                if (newRoute) {
                    b.routeTries++;
                    b.routeMode = (b.routeTries % 2) ? 1 : 2;
                    b.routeY = n.y + (b.routeMode == 1 ? 75.f : -75.f);
                    b.routeToX = x + 60.f;
                    n.tried = 0;                         // a new route: every choice from here is open again
                    bool up = b.routeMode == 1;
                    n.press = m_player1 && m_player1->m_isUpsideDown ? !up : up;
                    n.tried |= n.press ? 2 : 1;
                    s_thought = std::string("Same spot again. Going back ") + std::to_string((int)(x - n.x)) + " units and trying the " +
                                (b.routeMode == 1 ? "high" : "low") + " route.";
                    return;
                }
                if (n.tried != 3) {
                    n.press = !(n.tried & 2);
                    n.tried |= n.press ? 2 : 1;
                    s_thought = "Keep hitting the same thing. Backing up " + std::to_string((int)(x - n.x)) + " units and flying it differently.";
                    return;
                }
            }
        }
        if (b.sameCount >= 10 && !flying) {
            // Died 10 times on the same jump: go back to where he last stood on the ground before it
            // (the takeoff) and rethink from there.
            b.sameCount = 0;
            size_t i = b.nodes.size();
            while (i > 0 && !b.nodes[i - 1].ground) i--;          // skip mid-air choices (orbs)
            if (i > 0) i--;                                       // the takeoff itself
            while (i > 0 && b.nodes[i].tried == 3) i--;           // earlier ground choices if the takeoff is used up
            if (i < b.nodes.size() && b.nodes[i].cp && b.nodes[i].tried != 3) {
                while (b.nodes.size() > i + 1) { bool hc = b.nodes.back().cp; b.nodes.pop_back(); if (hc) this->removeCheckpoint(false); }
                auto& n = b.nodes.back();
                n.press = !(n.tried & 2);
                n.tried |= n.press ? 2 : 1;
                s_thought = "Died 10 times on this jump. Going back to where I took off and trying " + std::string(n.press ? "jumping there." : "waiting there.");
                return;
            }
        }
        while ((s_pathOnly || b.pfDeaths <= 900) && !b.nodes.empty()) {
            auto& n = b.nodes.back();
            if (!n.cp) { b.nodes.pop_back(); continue; }   // no checkpoint here: can't respawn at it
            if (n.tried != 3) {
                n.press = !(n.tried & 2) ? true : false;   // try the choice not tried yet
                n.tried |= n.press ? 2 : 1;
                s_thought = "That branch died. Going back " + std::to_string((int)(x - n.x)) + " units and trying " + (n.press ? "holding." : "letting go.");
                return;                                    // GD respawns at this decision's checkpoint
            }
            bool hadCp = b.nodes.back().cp;
            b.nodes.pop_back();
            if (hadCp) this->removeCheckpoint(false);
        }
        if ((!s_pathOnly && b.pfDeaths > 900) || b.nodes.empty() || !b.nodes.back().cp) {
            // Every choice within reach dies: the real mistake is further back. Learn it the slow way.
            b.pfFailed = true;
            b.best = holdsFrom(b.nodes);
            b.nodes.clear();
            this->removeAllCheckpoints();
            b.cpX = 0.f;
            b.cps.clear();
            b.trial = mutate(b.best, b.bestX, b.stuck, b.sweep, 0.f, true);
            say("Pathfinding hit a dead end. Learning this part by trying instead.", 3.f);
        }
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        if (s_enabled && !m_isPlatformer && pathfinding(this->brain()) && m_isPracticeMode) {
            auto f = m_fields.self();
            log::info("[pf] respawn at x={:.1f} (top node x={:.1f})", m_player1 ? m_player1->m_position.x : -1.f,
                      this->brain().nodes.empty() ? -1.f : this->brain().nodes.back().x);
            s_fromBot = true; this->handleButton(false, 1, true); s_fromBot = false;
            f->held = false;
            f->cur.clear(); f->curClicks.clear();
            if (f->live) f->live->clear();
            for (auto& pt : this->brain().bestPath) { if (pt.x < this->playerPos().x) f->cur.push_back(pt); else break; }
            return;
        }
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
        if (s_sim) { if (player == s_simGhost) s_ghostDead = true; return; }
        if (s_enabled && !m_isPlatformer && player == m_player1 && object != m_anticheatSpike && !player->m_isDead) {
            this->onCodaDeath(player->m_position.x, this->getCurrentPercent());
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void onCodaDeath(float x, float pct) {
        auto& b = this->brain();
        float oldBest = b.bestX;
        if (s_pathOnly && b.finalExam) {
            this->release();
            b.finalExam = false; b.examRun = false; b.beaten = false;
            b.guide = b.best;
            b.bestX = x; b.bestPct = pct;
            m_fields->needApply = true;
            say("Full run went differently. Pathfinding that part again.", 3.f);
            s_thought = "Following my old path, then fixing the spot where it broke.";
            this->trainingBookkeeping(x, pct, false);
            return;
        }
        if (pathfinding(b) && m_isPracticeMode) this->pfDeath(x, pct);
        else this->learnFromDeath(x, pct);
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
            s_thought = "That was a slip, not a real mistake. Same plan again.";
            b.trial = b.best;     // the slip wasn't a real lesson; try his real plan again
            return;
        }
        bool exam = b.examRun;
        b.examRun = false;
        if (exam && x < b.bestX) {
            // Failed the full run here: the plan is right up to this point, so fix it from here.
            b.bestX = x;
            b.bestPct = pct;
            b.stuck = 0;
            b.sweep = 0;
            say("Messed up the full run. Fixing it.");
            s_thought = "The full run went differently here. Fixing just this spot.";
            b.trial = mutate(b.best, b.bestX, b.stuck, b.sweep, 0.f, true);
            return;
        }
        if (b.confirm) {          // the replay that sets his checkpoint; not an experiment
            b.confirm = false;
            if (x > b.bestX + 0.5f) { b.bestX = x; b.bestPct = pct; }
            b.trial = mutate(b.best, b.bestX, b.stuck, b.sweep, b.cpX, b.finalExam);
            return;
        }
        if (x > b.bestX + 0.5f) {
            b.best = f->play;
            b.bestX = x;
            b.bestPct = pct;
            s_thought = "New best. Keeping that and pushing further.";
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
            // Dying over and over in one place: maybe the real mistake is earlier.
            // Back up one checkpoint at a time and re-check the jumps from there.
            if (mode().checkpoints && b.cpX > 0.f && b.stuck % mode().cpPatience == 0) {
                if (b.cps.size() > 1) {
                    this->removeCheckpoint(false);
                    b.cps.pop_back();
                    b.cpX = b.cps.back();
                    b.sweep = 0;
                    std::string t = "Died " + std::to_string(b.stuck) + " times here. Backing up to checkpoint " + std::to_string(b.cps.size()) + " to re-check.";
                    say("Backing up a checkpoint.");
                    b.trial = mutate(b.best, b.bestX, b.stuck, b.sweep, b.cpX, b.finalExam);
                    s_thought = t;
                    return;
                }
                this->removeAllCheckpoints();
                b.cps.clear();
                b.cpX = 0.f;
                b.noCpBefore = b.bestX + 30.f;
                say("Rethinking the whole thing.");
                b.trial = mutate(b.best, b.bestX, b.stuck, b.sweep, b.cpX, b.finalExam);
                s_thought = "No checkpoints left to back up to. Re-checking from the very start.";
                return;
            }
        }
        b.trial = mutate(b.best, b.bestX, b.stuck, b.sweep, b.cpX, b.finalExam);
    }

    // During the confirm replay, drop a checkpoint a bit before his best.
    void maybeCheckpoint() {
        auto& b = this->brain();
        if (!b.confirm || !mode().checkpoints || !m_isPracticeMode || !m_player1 || m_player1->m_isDead) return;
        float x = m_player1->m_position.x;
        float target = b.bestX - mode().cpMargin;
        if (x >= target && target > b.cpX + 60.f) {
            this->markCheckpoint();     // keep the older ones so he can back up later
            b.cps.push_back(x);
            b.cpX = x;
            s_thought = "Checkpoint " + std::to_string(b.cps.size()) + " set. Practicing the hard part from here.";
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
        if (s_enabled && (m_fields->planning || m_fields->planShow > 0.f)) return true;
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

    // Turbo: speed up while the PC keeps up (real frame under ~1/30 s), back off when it lags.
    // ============================================================ analysis
    // Pathfinds and proves the route behind a loading screen at top speed, then plays it for real.
    void startAnalysis() {
        auto f = m_fields.self();
        f->needAnalyze = false;
        if (m_isPlatformer) { say("Coda only plays classic levels.", 3.f); return; }
        s_enabled = true;
        s_analyze = true;
        s_turbo = 20.f;
        this->codaStart();
        auto& b = this->brain();
        b.beaten = false;
        auto win = CCDirector::get()->getWinSize();
        if (!f->overlay) {
            f->overlay = CCLayerColor::create({18, 12, 26, 255});
            this->addChild(f->overlay, 3000);
            auto face = CCSprite::create("coda-face.png"_spr);
            if (face) { face->setScale(2.2f); face->setPosition({win.width / 2, win.height / 2 + 70.f}); f->overlay->addChild(face); }
            f->anaTitle = CCLabelBMFont::create("Coda is analyzing the level...", "bigFont.fnt");
            f->anaTitle->setScale(0.55f);
            f->anaTitle->setPosition({win.width / 2, win.height / 2 + 18.f});
            f->overlay->addChild(f->anaTitle);
            f->anaBar = CCDrawNode::create();
            f->overlay->addChild(f->anaBar);
            f->anaInfo = CCLabelBMFont::create("", "chatFont.fnt");
            f->anaInfo->setScale(0.7f);
            f->anaInfo->setPosition({win.width / 2, win.height / 2 - 34.f});
            f->overlay->addChild(f->anaInfo);
            f->anaThought = CCLabelBMFont::create("", "chatFont.fnt");
            f->anaThought->setScale(0.6f);
            f->anaThought->setOpacity(190);
            f->anaThought->setPosition({win.width / 2, win.height / 2 - 58.f});
            f->overlay->addChild(f->anaThought);
        }
        f->overlay->setVisible(true);
        if (m_objectLayer) m_objectLayer->setVisible(false);    // nothing to draw: faster
    }

    void updateAnalysis() {
        auto f = m_fields.self();
        if (!f->overlay) return;
        auto& b = this->brain();
        auto win = CCDirector::get()->getWinSize();
        float w = 260.f, h = 10.f, x0 = win.width / 2 - w / 2, y0 = win.height / 2 - 12.f;
        float pct = std::clamp(b.bestPct / 100.f, 0.f, 1.f);
        f->anaBar->clear();
        f->anaBar->drawRect(ccp(x0, y0), ccp(x0 + w, y0 + h), ccc4f(1, 1, 1, 0.12f), 1.f, ccc4f(1, 1, 1, 0.8f));
        if (pct > 0) f->anaBar->drawRect(ccp(x0, y0), ccp(x0 + w * pct, y0 + h), ccc4f(0.96f, 0.64f, 0.71f, 1.f), 0.f, ccc4f(0, 0, 0, 0));
        std::string stage = pathfinding(b) ? "Finding the route" : "Checking the full run";
        std::string info = stage + "   " + std::to_string((int)b.bestPct) + "%   " + std::to_string(b.attempts) + " attempts   " +
                           std::to_string((int)s_turbo) + "x speed";
        f->anaInfo->setString(info.c_str());
        f->anaThought->setString(s_thought.c_str());
    }

    void finishAnalysis() {
        auto f = m_fields.self();
        auto& b = this->brain();
        s_analyze = false;
        if (f->overlay) f->overlay->setVisible(false);
        if (m_objectLayer) m_objectLayer->setVisible(true);
        CCDirector::get()->getScheduler()->setTimeScale(1.f);
        say("Figured it out in " + std::to_string(b.attempts) + " attempts. Watch.", 4.f);
        s_thought = "I know the route. Attempt 1.";
        b.attempts = 0;
    }

    float turboSpeed(float cap = 60.f) {
        using clock = std::chrono::steady_clock;
        static auto last = clock::now();
        auto now = clock::now();
        float real = std::chrono::duration<float>(now - last).count();
        last = now;
        if (real > 0.f && real < 0.5f) {
            if (real < 1.f / 40.f) s_turbo = std::min(cap, s_turbo * 1.03f);
            else if (real > 1.f / 25.f) s_turbo = std::max(4.f, s_turbo * 0.9f);
        }
        return s_turbo;
    }

    void finishAuto() {
        auto& b = this->brain();
        s_auto = false;
        s_mode = s_modeBeforeAuto;
        s_speedIdx = 0;
        b.skipUntil = -1;
        say("Auto train done: beaten in " + std::to_string(b.attempts) + " attempts, " + std::to_string(b.gen) + " generations. (Doesn't count.)", 5.f);
    }


    // ============================================================ the planner
    // Reads every hazard hitbox, then simulates an invisible copy of the player with the game's own
    // physics. At every decision (1/30 s) each candidate run branches into "hold" and "let go";
    // runs that hit something are dropped, and he keeps the ones that stay furthest from hazards.
    // The surviving run that reaches the end is the plan.

    CCPoint ghostPos(PlayerObject* g) {
        if (!g->getParent() || !m_objectLayer) return g->getPosition();
        return m_objectLayer->convertToNodeSpace(g->getParent()->convertToWorldSpace(g->getPosition()));
    }

    PlayerObject* acquireGhost() {
        auto f = m_fields.self();
        if (!f->freeGhosts.empty()) { auto g = f->freeGhosts.back(); f->freeGhosts.pop_back(); return g; }
        auto g = PlayerObject::create(1, 1, this, m_objectLayer, true);
        if (!g) return nullptr;
        if (m_player1 && m_player1->getParent()) m_player1->getParent()->addChild(g, -100);
        g->setVisible(false);
        f->pool.push_back(g);
        return g;
    }
    void releaseGhost(PlayerObject* g) { if (g) m_fields->freeGhosts.push_back(g); }

    static void copyState(PlayerObject* src, PlayerObject* dst) {
        dst->copyAttributes(src);
        dst->setPosition(src->getPosition());
        dst->setRotation(src->getRotation());
        dst->m_position = src->m_position;
        dst->m_yVelocity = src->m_yVelocity;
        dst->m_isOnGround = src->m_isOnGround;
        dst->m_isUpsideDown = src->m_isUpsideDown;
        dst->m_isShip = src->m_isShip; dst->m_isBird = src->m_isBird; dst->m_isBall = src->m_isBall;
        dst->m_isDart = src->m_isDart; dst->m_isRobot = src->m_isRobot; dst->m_isSpider = src->m_isSpider;
        dst->m_isSwing = src->m_isSwing;
        dst->m_vehicleSize = src->m_vehicleSize;
        dst->m_playerSpeed = src->m_playerSpeed;
        dst->m_gravityMod = src->m_gravityMod;
        dst->m_isOnSlope = src->m_isOnSlope;
        dst->m_wasOnSlope = src->m_wasOnSlope;
        dst->m_jumpBuffered = src->m_jumpBuffered;
        dst->m_stateJumpBuffered = src->m_stateJumpBuffered;
        dst->m_isDashing = src->m_isDashing;
        dst->m_fallSpeed = src->m_fallSpeed;
        dst->m_isAccelerating = src->m_isAccelerating;
        dst->m_isSideways = src->m_isSideways;
        dst->m_isGoingLeft = src->m_isGoingLeft;
        dst->m_lastGroundedPos = src->m_lastGroundedPos;
        dst->m_platformerXVelocity = src->m_platformerXVelocity;
        dst->m_holdingButtons = src->m_holdingButtons;
        dst->m_isDead = false;
    }

    float clearance(CCRect const& r) {
        auto& hz = m_fields->hazards;
        float best = 1e9f;
        auto it = std::lower_bound(hz.begin(), hz.end(), r.getMinX() - 90.f,
            [](CCRect const& a, float x) { return a.getMinX() < x; });
        for (; it != hz.end() && it->getMinX() < r.getMaxX() + 90.f; ++it) {
            float dx = std::max({0.f, it->getMinX() - r.getMaxX(), r.getMinX() - it->getMaxX()});
            float dy = std::max({0.f, it->getMinY() - r.getMaxY(), r.getMinY() - it->getMaxY()});
            best = std::min(best, std::sqrt(dx * dx + dy * dy));
        }
        return best;
    }

    bool simSegment(PlayerObject* g, bool press, bool& held, std::unordered_set<GameObject*>& act, float& clear) {
        s_sim = true; s_simGhost = g; s_simActivated = &act; s_ghostDead = false; s_ghostDone = false;
        if (press != held) {
            if (press) g->pushButton(PlayerButton::Jump); else g->releaseButton(PlayerButton::Jump);
            held = press;
        }
        for (int i = 0; i < PLAN_STEPS; i++) {
            g->update(0.25f);
            this->checkCollisions(g, 0.25f, false);
            if (s_ghostDead || g->m_isDead) break;
            g->updateRotation(0.25f);
            clear = std::min(clear, this->clearance(g->getObjectRect()));
            if (g->m_position.x >= m_levelLength) { s_ghostDone = true; break; }
        }
        bool alive = !s_ghostDead && !g->m_isDead;
        s_sim = false; s_simGhost = nullptr; s_simActivated = nullptr;
        return alive;
    }

    void startPlanning() {
        auto f = m_fields.self();
        f->needPlan = false;
        if (!m_player1 || !m_objects) return;
        f->hazards.clear();
        for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
            if (obj && obj->m_objectType == GameObjectType::Hazard) f->hazards.push_back(obj->getObjectRect());
        }
        std::sort(f->hazards.begin(), f->hazards.end(), [](CCRect const& a, CCRect const& b) { return a.getMinX() < b.getMinX(); });
        f->arena.clear(); f->beam.clear(); f->tries.clear(); f->planPath.clear(); f->planClicks.clear();
        f->checked = 0; f->planX = 0.f;
        auto g = this->acquireGhost();
        if (!g) { this->planFinished(-1); return; }
        copyState(m_player1, g);
        f->beam.push_back({-1, g, 1e9f, false, {}});
        f->planning = true;
        s_thought = "Reading " + std::to_string(f->hazards.size()) + " hazard hitboxes...";
        if (!f->planNode && m_objectLayer) { f->planNode = CCDrawNode::create(); m_objectLayer->addChild(f->planNode, 9002); }
        this->refreshDebugDraw();
        say("Reading the level...", 1.5f);
    }

    void planChunk() {
        using clock = std::chrono::steady_clock;
        auto f = m_fields.self();
        auto until = clock::now() + std::chrono::milliseconds(12);
        while (f->planning && clock::now() < until) {
            std::vector<Fields::BeamE> kids;
            int finished = -1; float finishedClear = -1.f;
            for (auto& e : f->beam) {
                for (int choice = 0; choice < 2; choice++) {
                    bool press = choice == 1;
                    auto g = this->acquireGhost();
                    if (!g) continue;
                    copyState(e.g, g);
                    Fields::BeamE c{e.node, g, e.clear, e.held, e.act};
                    float x0 = g->m_position.x;
                    CCPoint p0 = this->ghostPos(g);
                    bool alive = this->simSegment(g, press, c.held, c.act, c.clear);
                    CCPoint p1 = this->ghostPos(g);
                    f->checked++;
                    if (f->tries.size() < 6000) f->tries.push_back({p0, p1});
                    if (!alive) { this->releaseGhost(g); continue; }
                    f->arena.push_back({e.node, press, x0, p1});
                    c.node = (int)f->arena.size() - 1;
                    if (s_ghostDone && c.clear > finishedClear) { finished = c.node; finishedClear = c.clear; }
                    kids.push_back(std::move(c));
                }
            }
            for (auto& e : f->beam) this->releaseGhost(e.g);
            f->beam.clear();
            if (finished >= 0) {
                f->planClear = finishedClear;
                for (auto& k : kids) this->releaseGhost(k.g);
                this->planFinished(finished);
                return;
            }
            if (kids.empty()) { this->planFinished(-1); return; }
            // merge runs that ended up in the same spot, keep the safest
            std::sort(kids.begin(), kids.end(), [](auto const& a, auto const& b) { return a.clear > b.clear; });
            std::vector<Fields::BeamE> keep;
            std::unordered_set<long long> seen;
            for (auto& k : kids) {
                auto* g = k.g;
                long long key = ((long long)std::lround(g->m_position.y / 2.f) << 32) ^ ((long long)std::lround(g->m_yVelocity * 4.0) << 12) ^
                                ((long long)this->iconOf(g) << 4) ^ ((long long)g->m_isUpsideDown << 2) ^ ((long long)k.held << 1) ^ (long long)g->m_isOnGround;
                if ((int)keep.size() < PLAN_BEAM && seen.insert(key).second) { f->planX = std::max(f->planX, g->m_position.x); keep.push_back(std::move(k)); }
                else this->releaseGhost(g);
            }
            f->beam = std::move(keep);
            s_thought = "Checking " + std::to_string(f->beam.size() * 2) + " possible moves. Keeping the safest ones.";
        }
    }

    void planFinished(int node) {
        auto f = m_fields.self();
        auto& b = this->brain();
        f->planning = false;
        for (auto& e : f->beam) this->releaseGhost(e.g);
        f->beam.clear();
        b.planDone = true;
        if (node < 0) {
            b.planFailed = true;
            float pct = m_levelLength > 0 ? 100.f * f->planX / m_levelLength : 0.f;
            say("Couldn't plan past " + std::to_string((int)pct) + "%. Switching to pathfinding.", 3.f);
            f->needApply = true;
            return;
        }
        std::vector<int> chain;
        for (int n = node; n >= 0; n = f->arena[n].parent) chain.push_back(n);
        std::reverse(chain.begin(), chain.end());
        std::vector<Hold> plan;
        bool prev = false;
        f->planPath.clear(); f->planClicks.clear();
        f->planPath.push_back(this->playerPos());
        for (size_t i = 0; i < chain.size(); i++) {
            auto& n = f->arena[chain[i]];
            float end = i + 1 < chain.size() ? f->arena[chain[i + 1]].x0 : n.x0 + PF_STEP;
            if (n.press) {
                if (!prev) f->planClicks.push_back(f->planPath.back());
                if (prev && !plan.empty()) plan.back().end = end; else plan.push_back({n.x0, end});
            }
            prev = n.press;
            f->planPath.push_back(n.end);
        }
        b.best = plan; b.trial = plan;
        b.bestPath = f->planPath; b.bestClicks = f->planClicks;
        b.bestX = m_levelLength; b.bestPct = 99.f;
        b.finalExam = true; b.examRun = true; b.noCpBefore = 1e9f;
        int clicks = (int)f->planClicks.size();
        std::string clr = f->planClear > 1e8f ? "no hazards" : std::to_string((int)f->planClear) + " units";
        say("Plan ready: " + std::to_string(clicks) + " clicks, closest call " + clr + ". Attempt 1.", 4.f);
        f->planShow = 3.f;
        s_thought = "This is where I'm going to go.";
        this->drawPlan();
    }

    void drawPlan() {
        auto f = m_fields.self();
        if (!f->planNode) return;
        f->planNode->clear();
        if (!s_enabled || !mode().perfect) return;
        for (auto& [a, b] : f->tries) f->planNode->drawSegment(a, b, 0.5f, ccc4f(0.81f, 0.84f, 1.f, 0.28f));
        if (f->planPath.size() > 1) {
            polyline(f->planNode, f->planPath, 5.f, ccc4f(0.49f, 0.94f, 0.66f, 0.18f));
            polyline(f->planNode, f->planPath, 2.2f, ccc4f(0.96f, 0.64f, 0.71f, 1.f));
        }
        for (auto& c : f->planClicks) { f->planNode->drawDot(c, 3.4f, ccc4f(0.14f, 0.13f, 0.11f, 1.f)); f->planNode->drawDot(c, 2.6f, ccc4f(1.f, 0.88f, 0.29f, 1.f)); }
    }

    void cleanupGhosts() {
        auto f = m_fields.self();
        for (auto g : f->pool) if (g) g->removeFromParent();
        f->pool.clear(); f->freeGhosts.clear(); f->beam.clear();
    }

    void levelComplete() {
        if (s_sim) { s_ghostDone = true; return; }
        auto f = m_fields.self();
        if (!f->used) return PlayLayer::levelComplete();
        // Blocked: no completion, no rewards, no verifying.
        if (s_enabled) {
            auto& b = this->brain();
            if (pathfinding(b)) { f->play = holdsFrom(b.nodes); b.guide = f->play; b.nodes.clear(); }
            b.beaten = true;
            b.bestPct = 100.f;
            b.best = f->play;
            b.bestX = m_player1 ? m_player1->m_position.x : b.bestX;
            b.trial = b.best;
            if (mode().perfect && m_isPracticeMode) {
                // Beat it using checkpoints. Not good enough: now a clean run from the start.
                b.beaten = false;
                b.bestPct = 99.f;
                b.finalExam = true;
                b.examRun = true;
                b.cpX = 0.f;
                b.noCpBefore = 1e9f;
                f->exitPractice = true;
                say("Found a path. Now the whole thing from the start, no checkpoints.", 3.f);
            } else {
                if (s_analyze) { this->finishAnalysis(); }
                else if (mode().perfect) say("Perfect run. From the start, no mistakes. (Doesn't count.)", 4.f);
                if (s_auto) this->finishAuto();
                else if (!mode().perfect) say("Coda beat it. (Doesn't count.)", 3.f);
            }
        } else {
            say("Coda played this level, so it doesn't count. Reopen the level to play for real.", 4.f);
        }
        this->release();
        this->runAction(CCSequence::create(
            CCDelayTime::create(2.5f),
            CCCallFunc::create(this, callfunc_selector(CodaPlayLayer::codaRestart)),
            nullptr));
    }

    void codaRestart() {
        auto f = m_fields.self();
        if (f->exitPractice) {
            f->exitPractice = false;
            this->removeAllCheckpoints();
            this->brain().cps.clear();
            if (m_isPracticeMode) this->togglePracticeMode(false);
        }
        this->resetLevel();
    }

    void onQuit() {
        s_menuOpen = false;
        s_analyze = false;
        this->cleanupGhosts();
        CCDirector::get()->getScheduler()->setTimeScale(1.f);
        PlayLayer::onQuit();
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        auto f = m_fields.self();
        if (f->needApply) { f->needApply = false; if (s_enabled) this->applyMode(); f->needReset = true; }
        if (f->needReset) { f->needReset = false; this->resetLevel(); }
        else if (f->needPlan && s_enabled && !m_isPlatformer) this->startPlanning();
        else if (f->needAnalyze) this->startAnalysis();
        if (s_menuOpen) { CCDirector::get()->getScheduler()->setTimeScale(0.f); if (s_enabled) this->updateBubble(); return; }
        if (!s_enabled) return;
        if (f->planning || f->planShow > 0.f) {
            // the real level waits while he thinks
            CCDirector::get()->getScheduler()->setTimeScale(0.f);
            if (f->planning) {
                this->planChunk();
                float pct = m_levelLength > 0 ? 100.f * f->planX / m_levelLength : 0.f;
                f->info->setString(("Planning... " + std::to_string((int)pct) + "%   " + std::to_string(f->checked) + " moves checked").c_str());
                static int tick = 0;
                if (++tick % 10 == 0) this->drawPlan();
            } else {
                using clock = std::chrono::steady_clock;
                static auto last = clock::now();
                auto now = clock::now();
                float real = std::min(0.1f, std::chrono::duration<float>(now - last).count());
                last = now;
                f->planShow -= real;
                if (f->planShow <= 0.f) {
                    this->cleanupGhosts();
                    this->refreshDebugDraw();
                    CCDirector::get()->getScheduler()->setTimeScale(1.f);
                    f->needReset = true;
                }
            }
            this->updateIcon();
            this->updateBubble();
            return;
        }
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
            if (s_auto) speed = this->turboSpeed();
            if (s_pathOnly) speed = 1.f;
            if (s_analyze) { speed = this->turboSpeed(300.f); this->updateAnalysis(); }
            CCDirector::get()->getScheduler()->setTimeScale(speed);
        }
    }

    void updateBubble() {
        auto f = m_fields.self();
        if (!f->bubble) return;
        bool show = s_enabled && s_showThoughts && !s_thought.empty() && m_player1 && !s_analyze;
        f->bubble->setVisible(show);
        if (!show) return;
        if (std::string(f->bubbleText->getString()) != s_thought) {
            f->bubbleText->setString(s_thought.c_str());
            auto sz = f->bubbleText->getScaledContentSize();
            f->bubbleBg->setContentSize({sz.width + 10.f, sz.height + 6.f});
            f->bubbleBg->setPosition({-sz.width / 2 - 5.f, -sz.height / 2 - 3.f});
        }
        f->bubble->setPosition(this->playerPos() + ccp(0, 46.f));
    }

    void updateHud(float dt) {
        this->updateBubble();
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
            (b.beaten ? "   Beaten (doesn't count)" : "   Learning...") + "   [" + (s_pathOnly ? std::string(pathfinding(b) ? "Pathfinding" : "Full run") : std::string(mode().name) + (pathfinding(b) ? ": pathfinding" : (b.finalExam && !b.beaten ? ": full run" : ""))) + "]" +
            (s_auto ? "   AUTO TRAIN " + std::to_string((int)s_turbo) + "x" : "");
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
    // While the planner simulates, the invisible player must not change anything in the real level.
    void toggleGroupTriggered(int group, bool activate, std::vector<int> const& remapKeys, int triggerID, int controlID) {
        if (s_sim) return;
        GJBaseGameLayer::toggleGroupTriggered(group, activate, remapKeys, triggerID, controlID);
    }
    void spawnGroup(int group, bool ordered, double delay, std::vector<int> const& remapKeys, int triggerID, int controlID) {
        if (s_sim) return;
        GJBaseGameLayer::spawnGroup(group, ordered, delay, remapKeys, triggerID, controlID);
    }
    void pickupItem(EffectGameObject* object) { if (s_sim) return; GJBaseGameLayer::pickupItem(object); }
    void destroyObject(GameObject* object) { if (s_sim) return; GJBaseGameLayer::destroyObject(object); }
    void checkpointActivated(CheckpointGameObject* object) { if (s_sim) return; GJBaseGameLayer::checkpointActivated(object); }
    void activateSFXTrigger(SFXTriggerGameObject* object) { if (s_sim) return; GJBaseGameLayer::activateSFXTrigger(object); }
    void gameEventTriggered(GJGameEvent event, int material, int playerID) { if (s_sim) return; GJBaseGameLayer::gameEventTriggered(event, material, playerID); }
    void lightningFlash(CCPoint from, CCPoint to, ccColor3B color, float lineWidth, float duration, int displacement, bool flash, float opacity) {
        if (s_sim) return;
        GJBaseGameLayer::lightningFlash(from, to, color, lineWidth, duration, displacement, flash, opacity);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        auto pl = PlayLayer::get();
        if (pl && pl == static_cast<GJBaseGameLayer*>(this) && s_menuOpen && !s_fromBot) return;
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

// ---------------------------------------------------------------- simulation guards
class $modify(PlayerObject) {
    void playSpiderDashEffect(CCPoint from, CCPoint to) { if (s_sim && this == s_simGhost) return; PlayerObject::playSpiderDashEffect(from, to); }
    void spawnPortalCircle(ccColor3B color, float startRadius) { if (s_sim && this == s_simGhost) return; PlayerObject::spawnPortalCircle(color, startRadius); }
    void incrementJumps() { if (s_sim && this == s_simGhost) return; PlayerObject::incrementJumps(); }
};

// Orbs, pads and touch triggers remember who used them. The invisible player keeps its own list,
// so planning never "uses up" an orb for the real Coda.
class $modify(EnhancedGameObject) {
    void activatedByPlayer(PlayerObject* player) {
        if (s_sim && player == s_simGhost) { if (s_simActivated) s_simActivated->insert(this); return; }
        EnhancedGameObject::activatedByPlayer(player);
    }
    bool hasBeenActivatedByPlayer(PlayerObject* player) {
        if (s_sim && player == s_simGhost) return s_simActivated && s_simActivated->count(this);
        return EnhancedGameObject::hasBeenActivatedByPlayer(player);
    }
};

// ---------------------------------------------------------------- Coda's menu (~ key)
class CodaMenu : public CCLayerColor {
    struct Row { std::function<std::string()> text; std::function<void()> act; CCLabelBMFont* label = nullptr; };
    std::vector<Row> m_rows;
public:
    static CodaMenu* s_open;

    static CodaPlayLayer* pl() { return static_cast<CodaPlayLayer*>(PlayLayer::get()); }

    static void turnOn() {
        if (s_enabled) return;
        s_enabled = true;
        if (auto p = pl()) p->codaStart();
    }

    static void toggle() {
        if (s_open) { bool wasInLevel = s_open->getParent() != nullptr; s_open->close(); if (wasInLevel || !PlayLayer::get()) return; }
        if (!PlayLayer::get()) return;
        auto m = new CodaMenu();
        if (m->init()) {
            m->autorelease();
            CCDirector::get()->getRunningScene()->addChild(m, 10000);
            m->retain();          // kept alive even if the level closes while it's open
            s_open = m;
            s_menuOpen = true;
            PlatformToolbox::showCursor();
        } else delete m;
    }

    void close() {
        s_open = nullptr;
        s_menuOpen = false;
        CCDirector::get()->getScheduler()->setTimeScale(1.f);
        this->removeFromParent();
        this->release();
    }

    void addRow(std::function<std::string()> text, std::function<void()> act) { m_rows.push_back({text, act}); }

    bool init() override {
        if (!CCLayerColor::initWithColor({0, 0, 0, 140})) return false;
        auto win = CCDirector::get()->getWinSize();
        addRow([] { return std::string("Coda: ") + (s_enabled ? "ON" : "OFF"); }, [] {
            if (s_enabled) { s_enabled = false; if (s_auto) { s_auto = false; s_mode = s_modeBeforeAuto; } if (auto p = pl()) p->codaStop(); }
            else turnOn();
            if (auto p = pl()) { p->redrawPaths(); p->refreshDebugDraw(); }
        });
        addRow([] { return std::string(s_analyze ? "Analyzing..." : "Analyze and beat this level"); }, [] {
            if (auto p = pl(); p && !s_analyze) { p->m_fields->needAnalyze = true; if (s_open) s_open->close(); }
        });
        addRow([] { return std::string("Show his path: ") + (s_training ? "ON" : "OFF"); }, [] {
            s_training = !s_training; if (auto p = pl()) p->redrawPaths();
        });
        if (!s_pathOnly) {
        addRow([] { return std::string("Auto train: ") + (s_auto ? "ON" : "OFF"); }, [] {
            s_auto = !s_auto;
            if (s_auto) {
                s_modeBeforeAuto = s_mode; s_mode = 3; s_training = true; s_turbo = 20.f; turnOn();
                if (auto p = pl()) { if (!p->m_isPlatformer) p->m_fields->needApply = true; p->brain().beaten = false; p->redrawPaths(); }
            } else {
                s_mode = s_modeBeforeAuto;
                if (auto p = pl()) if (s_enabled && !p->m_isPlatformer) p->m_fields->needApply = true;
            }
        });
        addRow([] { return std::string("Learning: ") + mode().name; }, [] {
            s_mode = (s_mode + 1) % MODE_COUNT;
            if (auto p = pl()) if (s_enabled && !p->m_isPlatformer) p->m_fields->needApply = true;
        });
        addRow([] { return std::string("Training: ") + (s_training ? "ON" : "OFF"); }, [] {
            s_training = !s_training; if (s_training) turnOn(); if (auto p = pl()) p->redrawPaths();
        });
        addRow([] { return "Speed: " + std::to_string((int)SPEEDS[s_speedIdx]) + "x"; }, [] { s_speedIdx = (s_speedIdx + 1) % 4; });
        for (int n : {1, 5, 10}) {
            addRow([n] { return "Skip ahead +" + std::to_string(n) + (n == 1 ? " generation" : " generations"); }, [n] {
                s_training = true; turnOn();
                if (auto p = pl()) { auto& b = p->brain(); b.skipUntil = b.gen + n; p->redrawPaths(); }
            });
        }
        }
        addRow([] { return std::string("FPS bypass: ") + (Mod::get()->getSettingValue<bool>("fps-bypass") ? "ON" : "OFF"); }, [] {
            bool on = !Mod::get()->getSettingValue<bool>("fps-bypass");
            (void)Mod::get()->setSettingValue<bool>("fps-bypass", on);
            if (on) { PlatformToolbox::toggleVerticalSync(false); applyFps(); }
            else CCDirector::get()->setAnimationInterval(1.0 / 60.0);
        });
        addRow([] { return "FPS: " + std::to_string((int)Mod::get()->getSettingValue<int64_t>("fps")); }, [] {
            static int64_t const steps[] = {60, 120, 144, 165, 240, 360, 480, 1000};
            int64_t cur = Mod::get()->getSettingValue<int64_t>("fps"), next = steps[0];
            for (auto v : steps) if (v > cur) { next = v; break; }
            (void)Mod::get()->setSettingValue<int64_t>("fps", next);
            applyFps();
        });
        addRow([] {
            if (s_tpsBroken) return std::string("Physics Hz bypass: not supported on this GD version");
            return "Physics Hz (TPS): " + std::to_string(s_tps) + (s_tps == 240 ? " (normal)" : " (practice only)");
        }, [] {
            int next = 240;
            for (size_t i = 0; i < std::size(TPS_STEPS); i++) if (TPS_STEPS[i] == s_tps) { next = TPS_STEPS[(i + 1) % std::size(TPS_STEPS)]; break; }
            if (setTps(next)) {
                (void)Mod::get()->setSavedValue<int64_t>("tps", next);
                if (auto p = pl(); p && next != 240) { p->m_isTestMode = true; p->m_fields->tpsUsed = true; }
            }
        });
        addRow([] { return std::string("Show his thoughts: ") + (s_showThoughts ? "ON" : "OFF"); }, [] { s_showThoughts = !s_showThoughts; });
        addRow([] { return std::string("Hitboxes: ") + (s_hitboxes ? "ON" : "OFF"); }, [] { s_hitboxes = !s_hitboxes; if (auto p = pl()) p->refreshDebugDraw(); });
        addRow([] { return std::string("Hitbox trail: ") + (s_trail ? "ON" : "OFF"); }, [] { s_trail = !s_trail; if (auto p = pl()) p->refreshDebugDraw(); });

        float panelH = 70.f + m_rows.size() * 22.f;
        auto panel = CCLayerColor::create({30, 22, 40, 235}, 300.f, panelH);
        panel->setPosition({win.width / 2 - 150.f, win.height / 2 - panelH / 2});
        this->addChild(panel);
        auto face = CCSprite::create("coda-face.png"_spr);
        if (face) { face->setScale(1.1f); face->setPosition({win.width / 2 - 110.f, win.height / 2 + panelH / 2 - 26.f}); this->addChild(face); }
        auto title = CCLabelBMFont::create("Coda", "bigFont.fnt");
        title->setScale(0.6f);
        title->setAnchorPoint({0.f, 0.5f});
        title->setPosition({win.width / 2 - 84.f, win.height / 2 + panelH / 2 - 26.f});
        this->addChild(title);
        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        this->addChild(menu);
        float y = win.height / 2 + panelH / 2 - 60.f;
        for (size_t i = 0; i < m_rows.size(); i++) {
            auto lbl = CCLabelBMFont::create(m_rows[i].text().c_str(), "bigFont.fnt");
            lbl->setScale(0.38f);
            auto btn = CCMenuItemSpriteExtra::create(lbl, this, menu_selector(CodaMenu::onRow));
            btn->setTag((int)i);
            btn->setPosition({win.width / 2, y - i * 22.f});
            menu->addChild(btn);
            m_rows[i].label = lbl;
        }
        auto hint = CCLabelBMFont::create("~ to close", "chatFont.fnt");
        hint->setScale(0.55f);
        hint->setOpacity(160);
        hint->setPosition({win.width / 2, win.height / 2 - panelH / 2 + 10.f});
        this->addChild(hint);
        return true;
    }

    void onRow(CCObject* sender) {
        int i = static_cast<CCNode*>(sender)->getTag();
        if (i < 0 || i >= (int)m_rows.size()) return;
        m_rows[i].act();
        for (auto& r : m_rows) if (r.label) r.label->setString(r.text().c_str());
    }
};
CodaMenu* CodaMenu::s_open = nullptr;

class $modify(CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double time) {
        if (key == KEY_OEM3 && down && !repeat && PlayLayer::get()) { CodaMenu::toggle(); return true; }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, time);
    }
};

// Pause menu: just one small button that opens the same menu.
class $modify(CodaPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto win = CCDirector::get()->getWinSize();
        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        this->addChild(menu, 50);
        auto face = CCSprite::create("coda-face.png"_spr);
        if (face) face->setScale(1.2f);
        auto btn = CCMenuItemSpriteExtra::create(face ? (CCNode*)face : (CCNode*)CCLabelBMFont::create("Coda", "bigFont.fnt"),
                                                 this, menu_selector(CodaPauseLayer::onCoda));
        btn->setPosition({win.width - 36.f, 46.f});
        menu->addChild(btn);
        auto lbl = CCLabelBMFont::create("Coda (~)", "bigFont.fnt");
        lbl->setScale(0.25f);
        lbl->setPosition({win.width - 36.f, 18.f});
        this->addChild(lbl, 50);
    }
    void onCoda(CCObject*) { CodaMenu::toggle(); }
};

// Level page: "Analyze" next to the play button. Opens the level and goes straight into analysis.
class $modify(CodaLevelInfoLayer, LevelInfoLayer) {
    bool init(GJGameLevel* level, bool challenge) {
        if (!LevelInfoLayer::init(level, challenge)) return false;
        if (level && level->isPlatformer()) return true;
        auto menu = CCMenu::create();
        auto face = CCSprite::create("coda-face.png"_spr);
        CCNode* art = face ? (CCNode*)face : (CCNode*)CCLabelBMFont::create("Coda", "bigFont.fnt");
        if (face) face->setScale(1.6f);
        auto btn = CCMenuItemSpriteExtra::create(art, this, menu_selector(CodaLevelInfoLayer::onCodaAnalyze));
        menu->addChild(btn);
        auto lbl = CCLabelBMFont::create("Analyze", "bigFont.fnt");
        lbl->setScale(0.3f);
        lbl->setPosition({btn->getContentSize().width / 2, -6.f});
        btn->addChild(lbl);
        auto win = CCDirector::get()->getWinSize();
        CCPoint at = m_playBtnMenu ? m_playBtnMenu->getPosition() : ccp(win.width / 2, win.height / 2);
        menu->setPosition(at + ccp(-95.f, 0.f));
        this->addChild(menu, 10);
        return true;
    }
    void onCodaAnalyze(CCObject*) {
        s_analyzeNext = true;
        this->onPlay(nullptr);
    }
};
