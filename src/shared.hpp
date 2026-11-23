// What the different parts of the mod need from each other.
#pragma once
#include <Geode/Geode.hpp>
#include <deque>
#include <string>
#include <vector>
#include <unordered_set>

namespace menu {
    bool isOpen();
    void toggle();
    void notify(std::string const& text);     // small pop-up message at the top of the screen
}

// Coda's old pathfinder (bot.cpp)
namespace bot {
    bool enabled();
    void setEnabled(bool on);
    bool usedHere();          // Coda played in the open level
    bool simulating();        // the planner or the trajectory is moving an invisible player right now
    bool menuBlocksInput();
    bool& fromBot();          // true while a button press comes from a bot, not your hands
}

// Replay bot (replay.cpp)
namespace replay {
    enum class State { Off, Recording, Playing };
    State state();
    void record();
    void play();
    void stop();
    void clear();
    bool save(std::string const& name);
    bool load(std::string const& name);
    std::vector<std::string> list();
    std::string& name();
    size_t inputCount();
    size_t playIndex();
    unsigned lastFrame();
    std::string status();
    void countWindows();      // play the macro and measure every input's frame window
}

// Invisible-player physics, shared from bot.cpp
namespace sim {
    PlayerObject* makeGhost();                       // a hidden player you own (lives until the level closes)
    void copy(PlayerObject* from, PlayerObject* to);
    void button(PlayerObject* g, bool down);
    bool tick(PlayerObject* g, std::unordered_set<GameObject*>& act, float dt, bool& reachedEnd);   // false = died
}

// Frame Window Counter (fwc.cpp)
namespace fwc {
    struct Window {
        unsigned tick = 0;
        bool down = true;        // click or release
        float pct = 0.f;         // where in the level
        int early = 0, late = 0; // how many steps earlier / later still work
        float frames = 0.f;      // window size in 240 Hz frames
        bool cappedEarly = false, cappedLate = false;   // hit another input or the search limit
        bool unknown = false;    // the simulation couldn't reproduce this input
    };
    bool counting();
    void begin(int tps);
    void add(Window const& w);
    void finish(bool completed);
    void cancel();
    std::vector<Window> const& results();
    int tps();
    bool saveReport(std::string const& macroName);
    void drawOverlay();          // the NaN-style counter on screen
    void drawPanel();            // results inside the Coda Bot window
}

// Bypasses and gameplay hacks (hacks.cpp)
namespace hacks {
    bool tpsSupported();
    void applyFps();
    void applyTps();
    void applySpeedAudio();
    void refreshHitboxes();
    void startposStep(int dir);
    void frameStep();
    void reopenLevel();
    int currentTps();
    // live numbers for the labels
    struct Stats {
        unsigned totalFrames = 0, deadFrames = 0, lastDeadFrame = ~0u;
        int noclipDeaths = 0;
        float flash = 0.f;
        std::deque<double> clicks;      // times of recent presses
        int clicksTotal = 0;
        bool held[2] = {false, false};
        float bestFrom = 0.f, bestTo = 0.f, runFrom = 0.f;
        int startposIndex = -1, startposCount = 0;
    };
    Stats& stats();
    double now();
}

// Coda Safe (safe.cpp)
namespace safe {
    bool cheatedThisRun();
}

// Level Roulette (roulette.cpp)
namespace roulette {
    void drawWindow();
}
