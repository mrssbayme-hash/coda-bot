// Gameplay hacks: noclip, respawn, hitboxes, layout mode, start pos switcher, bypasses, trails.
#include "config.hpp"
#include "shared.hpp"
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/GameManager.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/CCTransitionFade.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <chrono>
#include <cstring>
#include <algorithm>

using namespace geode::prelude;

namespace hacks {
    static Stats s_stats;
    Stats& stats() { return s_stats; }
    double now() {
        static auto t0 = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }

    // ------------------------------------------------------------ FPS
    static double s_normalInterval = 0.0;
    void applyFps() {
        auto d = CCDirector::get();
        if (!d) return;
        if (V::fpsBypass) {
            if (s_normalInterval <= 0.0) s_normalInterval = d->getAnimationInterval();
            PlatformToolbox::toggleVerticalSync(false);
            d->setAnimationInterval(1.0 / (double)std::clamp(V::fps, 30, 10000));
        } else if (s_normalInterval > 0.0) {
            d->setAnimationInterval(s_normalInterval);
            s_normalInterval = 0.0;
        }
    }

    // ------------------------------------------------------------ TPS
    // GD 2.2091 runs physics at 240 steps a second, set by two constants used in GJBaseGameLayer:
    //   getModifiedDelta:  timestep = 1/240        (instruction at 0x340671)
    //   update:            steps    = delta*60*4.0 (instruction at 0x340900)
    // We point those two instructions at our own values (stored in unused padding bytes),
    // so the original constants that other code shares stay untouched.
    static int s_tps = 240;
    static std::vector<Patch*> s_tpsPatches;
    static bool s_tpsBroken = false;
    bool tpsSupported() { return !s_tpsBroken; }
    int currentTps() { return s_tps; }

    static bool setTps(int tps) {
        if (s_tpsBroken) return false;
        uintptr_t base = geode::base::get();
        auto bytesOf = [](auto v) { ByteVector b(sizeof(v)); std::memcpy(b.data(), &v, sizeof(v)); return b; };
        if (s_tpsPatches.empty()) {
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
    void applyTps() {
        int want = V::tpsBypass ? std::clamp(V::tps, 30, 10000) : 240;
        if (want == s_tps) return;
        if (!setTps(want) && want != 240) menu::notify("TPS bypass doesn't work on this version of GD");
    }

    // ------------------------------------------------------------ speedhack
    // The whole game's sound goes through FMOD's master channel group, so changing its pitch
    // speeds up the level song, the practice song and the sound effects together.
    void applySpeedAudio() {
        auto fmod = FMODAudioEngine::sharedEngine();
        if (!fmod || !fmod->m_system) return;
        float pitch = (V::speedhack && V::speedAudio) ? std::clamp(V::speed, .1f, 10.f) : 1.f;
        FMOD::ChannelGroup* master = nullptr;
        if (fmod->m_system->getMasterChannelGroup(&master) == FMOD_OK && master) {
            float cur = 1.f;
            master->getPitch(&cur);
            if (std::abs(cur - pitch) > .0001f) master->setPitch(pitch);
        }
    }

    // ------------------------------------------------------------ hitboxes
    void refreshHitboxes() {
        auto pl = PlayLayer::get();
        if (!pl) return;
        pl->updateDebugDrawSettings();
        if (pl->m_debugDrawNode) {
            if (V::hitboxes) pl->m_debugDrawNode->setVisible(true);
            else pl->m_debugDrawNode->clear();
        }
    }

    // ------------------------------------------------------------ start positions
    static std::vector<StartPosObject*> startPositions(PlayLayer* pl) {
        std::vector<StartPosObject*> out;
        if (!pl || !pl->m_objects) return out;
        for (auto obj : CCArrayExt<GameObject*>(pl->m_objects)) {
            if (obj && obj->m_objectID == 31) out.push_back(static_cast<StartPosObject*>(obj));
        }
        std::sort(out.begin(), out.end(), [](StartPosObject* a, StartPosObject* b) { return a->getPositionX() < b->getPositionX(); });
        return out;
    }
    static void countStartPositions(PlayLayer* pl) {
        auto list = startPositions(pl);
        s_stats.startposCount = (int)list.size();
        s_stats.startposIndex = -1;
        for (int i = 0; i < (int)list.size(); i++) if (list[i] == pl->m_startPosObject) s_stats.startposIndex = i;
    }
    void startposStep(int dir) {
        auto pl = PlayLayer::get();
        if (!pl || !V::startposSwitcher) return;
        auto list = startPositions(pl);
        if (list.empty()) return;
        int n = (int)list.size(), cur = -1;
        for (int i = 0; i < n; i++) if (list[i] == pl->m_startPosObject) cur = i;
        int next = cur + dir;
        if (next < -1) next = n - 1;
        if (next >= n) next = -1;
        StartPosObject* obj = next < 0 ? nullptr : list[next];
        pl->m_isTestMode = obj != nullptr || safe::cheatedThisRun();
        pl->switchToStartPosObject(obj);
        s_stats.startposIndex = next;
        s_stats.startposCount = n;
    }

    // ------------------------------------------------------------ layout mode
    static bool s_needRestart = false;
    void reopenLevel() { if (PlayLayer::get()) s_needRestart = true; }
    static bool isDeco(GameObject* o) {
        if (!o || o->m_isTrigger || o->m_objectID == 31) return false;
        return o->m_objectType == GameObjectType::Decoration;
    }

    // ------------------------------------------------------------ frame advance
    static int s_steps = 0;
    void frameStep() { if (V::frameAdvance) s_steps++; }
}

using namespace hacks;

// ================================================================ the level
static bool s_ourReset = false;

class $modify(HackPlayLayer, PlayLayer) {
    struct Fields {
        CCDrawNode* trail = nullptr;
        std::deque<CCPoint> points;
        bool pendingRespawn = false;
        double deathAt = 0.0;
        float cpTimer = 0.f;
        unsigned frameBase = 0;
        bool hidPlayer = false;
    };

    bool init(GJGameLevel* level, bool dontCreateObjects) {
        if (!PlayLayer::init(level, dontCreateObjects)) return false;
        auto f = m_fields.self();
        f->trail = CCDrawNode::create();
        if (m_objectLayer) m_objectLayer->addChild(f->trail, 8999);
        s_stats.bestFrom = s_stats.bestTo = 0.f;
        s_stats.runFrom = 0.f;
        s_stats.totalFrames = s_stats.deadFrames = 0;
        s_stats.lastDeadFrame = ~0u;
        s_stats.noclipDeaths = 0;
        countStartPositions(this);
        applyFps();
        applyTps();
        applySpeedAudio();
        return true;
    }

    void addObject(GameObject* obj) {
        if (obj) {
            if (V::layoutMode || V::noGlow) obj->m_hasNoGlow = true;
        }
        PlayLayer::addObject(obj);
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (bot::simulating()) return PlayLayer::destroyPlayer(player, object);
        bool mine = player && ((player == m_player1 && V::noclipP1) || (player == m_player2 && V::noclipP2));
        if (V::noclip && mine && object != m_anticheatSpike) {
            unsigned fr = m_gameState.m_currentProgress;
            if (fr != s_stats.lastDeadFrame) {
                if (s_stats.lastDeadFrame == ~0u || fr > s_stats.lastDeadFrame + 1) s_stats.noclipDeaths++;
                s_stats.deadFrames++;
                s_stats.lastDeadFrame = fr;
                if (V::noclipFlash) s_stats.flash = 1.f;
            }
            return;
        }
        bool wasDead = player && player->m_isDead;
        PlayLayer::destroyPlayer(player, object);
        if (player && player->m_isDead && !wasDead && object != m_anticheatSpike) this->onRealDeath();
    }

    void onRealDeath() {
        auto f = m_fields.self();
        float pct = this->getCurrentPercent();
        if (pct - s_stats.runFrom > s_stats.bestTo - s_stats.bestFrom) { s_stats.bestFrom = s_stats.runFrom; s_stats.bestTo = pct; }
        f->pendingRespawn = true;
        f->deathAt = now();
        if (V::codaReacts) {
            auto face = CCSprite::create("coda-face-mad.png"_spr);
            if (face) {
                face->setPosition({38.f, 38.f});
                face->setScale(0.f);
                this->addChild(face, 1100);
                face->runAction(CCSequence::create(
                    CCEaseBackOut::create(CCScaleTo::create(.25f, 1.1f)),
                    CCDelayTime::create(.6f),
                    CCFadeOut::create(.3f),
                    CCCallFunc::create(face, callfunc_selector(CCNode::removeFromParent)),
                    nullptr));
            }
        }
    }

    void delayedResetLevel() {
        if (V::respawnDelayOn && !s_ourReset) return;      // we do it ourselves, on our own timer
        PlayLayer::delayedResetLevel();
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        auto f = m_fields.self();
        f->pendingRespawn = false;
        f->points.clear();
        if (f->trail) f->trail->clear();
        f->cpTimer = 0.f;
        f->frameBase = m_gameState.m_currentProgress;
        s_stats.deadFrames = 0;
        s_stats.totalFrames = 0;
        s_stats.lastDeadFrame = ~0u;
        s_stats.noclipDeaths = 0;
        s_stats.runFrom = this->getCurrentPercent();
        countStartPositions(this);
    }

    void levelComplete() {
        float pct = 100.f;
        if (pct - s_stats.runFrom > s_stats.bestTo - s_stats.bestFrom) { s_stats.bestFrom = s_stats.runFrom; s_stats.bestTo = pct; }
        PlayLayer::levelComplete();
    }

    void updateProgressbar() {
        PlayLayer::updateProgressbar();
        if (V::accuratePercent && !m_isPlatformer && m_percentageLabel) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.*f%%", std::clamp(V::percentDecimals, 1, 4), this->getCurrentPercent());
            m_percentageLabel->setString(buf);
        }
    }

    bool shouldDrawDebugHitboxes() {
        if (V::hitboxes && (!V::hitboxesOnDeath || (m_player1 && m_player1->m_isDead))) return true;
        return PlayLayer::shouldDrawDebugHitboxes();
    }
    bool shouldDrawHitboxTrail() {
        if (V::hitboxes && V::hitboxTrail) return true;
        return PlayLayer::shouldDrawHitboxTrail();
    }
    bool shouldRecordHitboxTrail() {
        if (V::hitboxes && V::hitboxTrail) return true;
        return PlayLayer::shouldRecordHitboxTrail();
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        auto f = m_fields.self();
        // respawn on our own timer
        if (f->pendingRespawn) {
            if (!V::respawnDelayOn) f->pendingRespawn = false;
            else if (now() - f->deathAt >= V::respawnDelay) {
                f->pendingRespawn = false;
                if (m_player1 && m_player1->m_isDead) { s_ourReset = true; this->delayedResetLevel(); s_ourReset = false; }
            }
        }
        // noclip accuracy
        unsigned cur = m_gameState.m_currentProgress;
        s_stats.totalFrames = cur >= f->frameBase ? cur - f->frameBase : 0;
        if (s_stats.flash > 0.f) s_stats.flash = std::max(0.f, s_stats.flash - dt * 3.f);
        // hide player
        if (m_player1) {
            if (V::hidePlayer) { m_player1->setVisible(false); if (m_player2) m_player2->setVisible(false); f->hidPlayer = true; }
            else if (f->hidPlayer) { m_player1->setVisible(true); if (m_player2 && m_gameState.m_isDualMode) m_player2->setVisible(true); f->hidPlayer = false; }
        }
        if (m_attemptLabel) m_attemptLabel->setVisible(!V::hideAttempts);
        if (s_needRestart) {
            s_needRestart = false;
            if (m_objects) for (auto o : CCArrayExt<GameObject*>(m_objects)) {
                if (!isDeco(o)) continue;
                if (V::layoutMode) { o->setVisible(false); if (o->m_glowSprite) o->m_glowSprite->setVisible(false); }
            }
            this->resetLevel();
        }
        if (V::layoutMode) {
            if (m_background) m_background->setColor({40, 125, 255});
            if (m_middleground) m_middleground->setVisible(false);
        }
        if (V::hitboxes && m_debugDrawNode && !m_debugDrawNode->isVisible()) m_debugDrawNode->setVisible(true);
        // auto checkpoints
        if (V::autoCheckpoints && m_isPracticeMode && m_player1 && !m_player1->m_isDead && !m_isPaused) {
            f->cpTimer += dt;
            if (f->cpTimer >= V::autoCpEvery && m_player1->m_isOnGround) { f->cpTimer = 0.f; this->markCheckpoint(); }
        }
        this->drawCodaTrail();
    }

    void drawCodaTrail() {
        auto f = m_fields.self();
        if (!f->trail) return;
        f->trail->clear();
        if (!V::codaTrail || !m_player1 || !m_objectLayer || !m_player1->getParent()) { f->points.clear(); return; }
        if (!m_player1->m_isDead) {
            CCPoint p = m_objectLayer->convertToNodeSpace(m_player1->getParent()->convertToWorldSpace(m_player1->getPosition()));
            if (f->points.empty() || ccpDistance(f->points.back(), p) > .5f) f->points.push_back(p);
        }
        while ((int)f->points.size() > V::trailLength) f->points.pop_front();
        auto& c = V::trailColor;
        size_t n = f->points.size();
        for (size_t i = 1; i < n; i++) {
            if (ccpDistance(f->points[i - 1], f->points[i]) > 150.f) continue;      // teleports
            float k = (float)i / (float)n;
            f->trail->drawSegment(f->points[i - 1], f->points[i], V::trailWidth * (.35f + .65f * k) / 2.f, ccc4f(c.r, c.g, c.b, c.a * k));
        }
    }
};

// ================================================================ player effects
class $modify(HackPlayerObject, PlayerObject) {
    void playDeathEffect() {
        if (V::noDeathEffect && !bot::simulating()) return;
        PlayerObject::playDeathEffect();
    }
    void playSpawnEffect() {
        if (V::noRespawnBlink) return;
        PlayerObject::playSpawnEffect();
    }
};

class $modify(HackHardStreak, HardStreak) {
    struct Fields { bool forced = false; };
    void updateStroke(float dt) {
        if (V::solidWave) { m_isSolid = true; m_fields->forced = true; }
        else if (m_fields->forced) { m_isSolid = false; m_fields->forced = false; }
        HardStreak::updateStroke(dt);
    }
};

// ================================================================ inputs, frame advance, autoclicker
static bool s_acHeld = false;
static int s_acCount = 0;
static unsigned s_acFrame = ~0u;

class $modify(HackBaseLayer, GJBaseGameLayer) {
    // layout mode: every color goes back to GD's defaults
    void updateColor(ccColor3B& color, float fadeTime, int colorID, bool blending, float opacity, ccHSVValue& copyHSV,
                     int colorIDToCopy, bool copyOpacity, EffectGameObject* callerObject, int unk1, int unk2) {
        if (V::layoutMode && PlayLayer::get() == static_cast<GJBaseGameLayer*>(this)) {
            ccColor3B def{255, 255, 255};
            if (colorID == 1000 || colorID == 1013 || colorID == 1014) def = {40, 125, 255};
            else if (colorID == 1001 || colorID == 1009) def = {0, 102, 255};
            color = def;
            blending = false;
            colorIDToCopy = 0;
            opacity = 1.f;
        }
        GJBaseGameLayer::updateColor(color, fadeTime, colorID, blending, opacity, copyHSV, colorIDToCopy, copyOpacity, callerObject, unk1, unk2);
    }

    void update(float dt) {
        if (V::frameAdvance && PlayLayer::get() == static_cast<GJBaseGameLayer*>(this) && !bot::simulating()) {
            if (s_steps <= 0) return;
            s_steps--;
            GJBaseGameLayer::update(1.f / (float)std::max(1, s_tps) + 1e-6f);
            return;
        }
        GJBaseGameLayer::update(dt);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        if (PlayLayer::get() != static_cast<GJBaseGameLayer*>(this) || bot::simulating()) return;
        int p = isPlayer1 ? 0 : 1;
        if (button == 1) s_stats.held[p] = down;
        if (down && button == 1) {
            double t = now();
            s_stats.clicks.push_back(t);
            s_stats.clicksTotal++;
            while (!s_stats.clicks.empty() && t - s_stats.clicks.front() > 1.0) s_stats.clicks.pop_front();
        }
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto pl = PlayLayer::get();
        if (V::jumpHack && pl && pl == static_cast<GJBaseGameLayer*>(this) && !bot::simulating()) {
            // while you hold, the game thinks you're standing on something, so you can jump again
            for (auto p : {m_player1, m_player2}) {
                if (!p || p->m_isDead || (p == m_player2 && !m_gameState.m_isDualMode)) continue;
                if (s_stats.held[p == m_player2 ? 1 : 0]) {
                    p->m_isOnGround = true;
                    p->m_touchingGround = true;
                    p->m_landedSinceFlip = true;
                }
            }
        }
        if (V::autoclicker && pl && pl == static_cast<GJBaseGameLayer*>(this) && !bot::simulating() && !pl->m_player1->m_isDead) {
            unsigned fr = m_gameState.m_currentProgress;
            if (fr != s_acFrame) {
                s_acFrame = fr;
                s_acCount++;
                int limit = s_acHeld ? V::acHold : V::acRelease;
                if (s_acCount >= limit) {
                    s_acCount = 0;
                    s_acHeld = !s_acHeld;
                    bot::fromBot() = true;
                    this->handleButton(s_acHeld, 1, true);
                    if (V::acP2) this->handleButton(s_acHeld, 1, false);
                    bot::fromBot() = false;
                }
            }
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

// ================================================================ layout mode
// Decoration never gets switched on while layout mode is on, so only the gameplay objects are drawn.
class $modify(LayoutObject, GameObject) {
    void activateObject() {
        if (V::layoutMode && PlayLayer::get() && isDeco(this)) {
            if (m_glowSprite) m_glowSprite->setVisible(false);
            this->setVisible(false);
            return;
        }
        GameObject::activateObject();
    }
};

// ================================================================ global bits
class $modify(HackGameManager, GameManager) {
    bool isIconUnlocked(int id, IconType type) {
        if (V::iconHack) return true;
        return GameManager::isIconUnlocked(id, type);
    }
    bool isColorUnlocked(int id, UnlockType type) {
        if (V::iconHack) return true;
        return GameManager::isColorUnlocked(id, type);
    }
};

class $modify(HackScheduler, CCScheduler) {
    void update(float dt) {
        if (V::speedhack) dt *= std::clamp(V::speed, .1f, 10.f);
        applySpeedAudio();      // keep the music in step, even after GD starts a new song
        CCScheduler::update(dt);
    }
};

class $modify(HackFade, CCTransitionFade) {
    static CCTransitionFade* create(float duration, CCScene* scene) {
        return CCTransitionFade::create(V::noTransition ? 0.f : duration, scene);
    }
};

class $modify(HackMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        static bool once = false;
        if (!once) { once = true; applyFps(); applyTps(); applySpeedAudio(); }
        return true;
    }
};
