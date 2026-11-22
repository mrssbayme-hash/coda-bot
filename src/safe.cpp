// Coda Safe: once a cheat is used in a run, that run can't count.
// The level goes into test mode (no saved progress) and completing it is blocked,
// so you can't verify, complete or get stars with hacks. Restarting with no cheats on makes runs count again.
#include "config.hpp"
#include "shared.hpp"
#include <Geode/modify/PlayLayer.hpp>

using namespace geode::prelude;

static bool s_cheated = false;
namespace safe { bool cheatedThisRun() { return s_cheated; } }

class $modify(SafePlayLayer, PlayLayer) {
    struct Fields { bool normalTestMode = false; };

    bool init(GJGameLevel* level, bool dontCreateObjects) {
        s_cheated = false;
        if (!PlayLayer::init(level, dontCreateObjects)) return false;
        m_fields->normalTestMode = m_isTestMode;
        this->check();
        return true;
    }

    void check() {
        if (!cfg::cheatsInUse().empty()) { s_cheated = true; m_isTestMode = true; }
    }

    void postUpdate(float dt) {
        this->check();
        PlayLayer::postUpdate(dt);
    }

    void resetLevel() {
        if (cfg::cheatsInUse().empty() && !bot::enabled()) {
            s_cheated = false;
            m_isTestMode = m_fields->normalTestMode || m_startPosObject != nullptr;
        }
        PlayLayer::resetLevel();
        this->check();
    }

    void levelComplete() {
        // Coda's own pathfinder handles its runs itself
        if (bot::usedHere()) return PlayLayer::levelComplete();
        this->check();
        if (!s_cheated) return PlayLayer::levelComplete();
        menu::notify("Coda Safe: this run used hacks, so it doesn't count.");
        this->runAction(CCSequence::create(
            CCDelayTime::create(1.2f),
            CCCallFunc::create(this, callfunc_selector(SafePlayLayer::restartAfterBlock)),
            nullptr));
    }

    void restartAfterBlock() { this->resetLevel(); }
};
