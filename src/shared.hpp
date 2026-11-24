// What the different parts of the mod need from each other.
#pragma once
#include <Geode/Geode.hpp>
#include <deque>
#include <string>

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

// Frame window counter (bot.cpp): how many frames each click could have been and still lived
namespace fwin {
    struct Result {
        unsigned frame = 0;
        bool down = true;
        int early = 0, late = 0;          // frames earlier / later that still work
        bool capEarly = false, capLate = false;   // hit the checking limit (could be even more)
        bool unknown = false;             // the replay didn't match what happened (moving parts, triggers)
        bool died = false;                // you died right after this click
        float pct = 0.f;
        int tps = 240, K = 8;
        double shownAt = 0.0;
        int window() const { return early + late + 1; }
    };
    void beforeInputs();            // copy where you are before this step's clicks
    std::deque<Result>& results();
    int waiting();
    void clear();
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
// Coda Share (share.cpp): .gmd export and import, like GDShare
namespace share { void drawWindow(); }

namespace roulette {
    void drawWindow();
}
