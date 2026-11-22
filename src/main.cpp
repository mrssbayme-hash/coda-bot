// Coda Bot: Coda learns levels by trial and error.
// His runs never count: the level is put in test mode, completions are blocked, and so is verifying.
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <random>
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
};

static std::unordered_map<std::string, Brain> s_brains;
static bool s_enabled = false;   // Coda is playing
static bool s_fromBot = false;   // the current button press came from Coda
static bool s_fast = true;       // fast-forward through parts he already knows
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

// Try something new near the place he keeps dying. The window grows the longer he is stuck,
// so he eventually rethinks earlier jumps too.
static std::vector<Hold> mutate(std::vector<Hold> const& best, float frontier, int stuck) {
    float window = 45.f + std::min(stuck, 40) * 7.f;
    if (stuck > 25 && chance(0.25f)) window *= 2.f;
    float lo = std::max(0.f, frontier - window);
    std::vector<Hold> out;
    for (auto h : best) {
        if (h.start > frontier + 30.f) continue;               // never reached: forget it
        if (h.start >= lo) {
            if (chance(0.12f)) continue;                       // drop this jump
            if (chance(0.45f)) { float d = rnd(-9.f, 9.f); h.start += d; h.end += d; }
            if (chance(0.30f)) h.end += rnd(-12.f, 12.f);      // hold longer or shorter
        }
        out.push_back(h);
    }
    int adds = chance(0.6f) ? 1 : (chance(0.5f) ? 2 : 0);
    for (int i = 0; i < adds; i++) {
        float s = rnd(lo, frontier + 15.f);
        float len = chance(0.6f) ? rnd(1.f, 6.f) : rnd(10.f, 110.f);   // a tap, or a hold
        out.push_back({s, s + len});
    }
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
        bool sloppy = false;      // this attempt had a deliberate slip
        bool held = false;
        size_t idx = 0;
        std::vector<Hold> play;   // the trial, maybe with a slip in it
    };

    Brain& brain() { return s_brains[levelKey(m_level)]; }

    bool init(GJGameLevel* level, bool dontCreateObjects) {
        if (!PlayLayer::init(level, dontCreateObjects)) return false;
        this->buildHud();
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
        if (m_isPlatformer) say("Coda only plays classic levels.", 3.f);
    }

    void codaStop() {
        auto f = m_fields.self();
        this->release();
        if (f->hud) f->hud->setVisible(false);
        this->restoreIcon();
        CCDirector::get()->getScheduler()->setTimeScale(1.f);
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
        f->play = b.trial.empty() && !b.best.empty() ? b.best : b.trial;
        f->sloppy = false;
        // He's good, not perfect: sometimes he fumbles a jump he already knows.
        if (b.bestX > 300.f && !b.best.empty() && chance(b.beaten ? 0.25f : 0.10f)) {
            std::vector<size_t> known;
            for (size_t i = 0; i < f->play.size(); i++) if (f->play[i].start < b.bestX - 90.f) known.push_back(i);
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
        auto f = m_fields.self();
        auto& b = this->brain();
        this->release();
        if (f->sloppy && x < b.bestX) {
            say(chance(0.5f) ? "...that was on purpose." : "Slipped.");
            b.trial = b.best;     // the slip wasn't a real lesson; try his real plan again
            return;
        }
        if (x > b.bestX + 0.5f) {
            b.best = b.trial.empty() ? f->play : b.trial;
            b.bestX = x;
            b.bestPct = pct;
            if (b.stuck > 6) say("Finally.");
            else if (b.attempts > 1) say("New best: " + std::to_string((int)pct) + "%");
            b.stuck = 0;
            f->smugTime = 1.5f;
        } else {
            b.stuck++;
            if (b.stuck == 15) say("This spike again.");
            if (b.stuck == 40) say("I hate this part.");
        }
        b.trial = mutate(b.best, b.bestX, b.stuck);
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
        if (f->needReset) { f->needReset = false; this->resetLevel(); }
        if (!s_enabled) return;
        this->updateIcon();
        this->updateHud(dt);
        if (m_player1 && !m_isPlatformer) {
            auto& b = this->brain();
            bool known = m_player1->m_position.x < b.bestX - 450.f && !m_player1->m_isDead;
            CCDirector::get()->getScheduler()->setTimeScale(s_fast && known ? 3.f : 1.f);
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
            (b.beaten ? "   Beaten (doesn't count)" : "   Learning...");
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
    int modeOf(PlayerObject* p) {
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
        int mode = this->modeOf(p);
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
            float base = (mode == 0 || mode == 2) ? 1.0f : 0.96f;   // sprites are 128px UHD = 32 units
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
        CCLabelBMFont* speed = nullptr;
    };

    void customSetup() {
        PauseLayer::customSetup();
        auto win = CCDirector::get()->getWinSize();
        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        this->addChild(menu, 50);

        auto face = CCSprite::create("coda-face.png"_spr);
        if (face) face->setScale(1.4f);
        auto btn = CCMenuItemSpriteExtra::create(face ? (CCNode*)face : (CCNode*)CCLabelBMFont::create("Coda", "bigFont.fnt"),
                                                 this, menu_selector(CodaPauseLayer::onCoda));
        btn->setPosition({win.width - 42.f, 52.f});
        menu->addChild(btn);
        m_fields->label = CCLabelBMFont::create("", "bigFont.fnt");
        m_fields->label->setScale(0.3f);
        m_fields->label->setPosition({win.width - 42.f, 22.f});
        this->addChild(m_fields->label, 50);

        auto spd = CCLabelBMFont::create("", "bigFont.fnt");
        spd->setScale(0.3f);
        auto sbtn = CCMenuItemSpriteExtra::create(spd, this, menu_selector(CodaPauseLayer::onSpeed));
        sbtn->setPosition({win.width - 120.f, 22.f});
        menu->addChild(sbtn);
        m_fields->speed = spd;
        this->refresh();
    }

    void refresh() {
        if (m_fields->label) m_fields->label->setString(s_enabled ? "Coda: ON" : "Coda: OFF");
        if (m_fields->speed) m_fields->speed->setString(s_fast ? "Fast training" : "Normal speed");
    }

    void onCoda(CCObject*) {
        s_enabled = !s_enabled;
        if (auto pl = static_cast<CodaPlayLayer*>(PlayLayer::get())) {
            if (s_enabled) { pl->codaStart(); pl->m_fields->needReset = true; }
            else pl->codaStop();
        }
        this->refresh();
    }

    void onSpeed(CCObject*) {
        s_fast = !s_fast;
        this->refresh();
    }
};
