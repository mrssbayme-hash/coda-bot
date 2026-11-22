// Every setting in the Coda menu lives here, so the menu, search, keybinds and saving
// can all work from one list.
#pragma once
#include <Geode/Geode.hpp>
#include <string>
#include <vector>
#include <functional>

struct Col { float r, g, b, a; };

namespace cfg {
    enum class T { Bool, Int, Float, Color, Choice, Action };

    struct Field {
        std::string key, label, win, tip, parent;
        T type = T::Bool;
        void* ptr = nullptr;
        float mn = 0.f, mx = 1.f;
        const char* fmt = "%.2f";
        std::vector<std::string> choices;
        bool cheat = false;          // on = the run can't count (Coda Safe)
        bool typed = false;          // int/float you type instead of drag
        int bind = 0;                // cocos key code, 0 = none
        int defBind = 0;
        std::string def;             // default value, as text
        std::function<void()> onChange;
        std::function<bool()> cheatIf;   // custom "is this cheating right now"
    };

    std::vector<Field>& all();
    Field* find(std::string const& key);
    std::string text(Field const& f);
    void setText(Field& f, std::string const& v);
    void changed(Field& f);          // run its callback and save
    void load();
    void save();
    void resetAll();
    void init();
    // names of cheats in use right now (empty = clean)
    std::vector<std::string> cheatsInUse();
    extern Field* waitingBind;       // the menu is waiting for a key for this field
    std::string keyName(int key);
}

// ------------------------------------------------------------------ the values
namespace V {
    // Player
    inline bool noclip = false, noclipP1 = true, noclipP2 = true, noclipFlash = false;
    inline Col noclipFlashColor{1.f, .15f, .2f, .35f};
    inline bool jumpHack = false;
    inline bool noDeathEffect = false, respawnDelayOn = false, noRespawnBlink = false, hidePlayer = false;
    inline int respawnDelayMs = 500;
    inline bool noclipLimit = false;
    inline float noclipLimitPct = 90.f;
    inline bool clickSounds = false, clickRelease = true;
    inline int clickStyle = 0;
    inline float clickVolume = .7f;
    inline bool hitboxes = false, hitboxesOnDeath = false, hitboxTrail = false;
    inline bool hitboxAccurate = true, hitboxHideRotated = true, hitboxFill = false;
    inline int hitboxTrailLen = 240;
    inline float hitboxFillAlpha = .3f, hitboxWidth = 1.f;
    inline Col hbSolid{0.f, .247f, 1.f, 1.f}, hbDanger{1.f, 0.f, 0.f, 1.f}, hbPlayer{1.f, 1.f, 0.f, 1.f},
        hbInner{1.f, 0.f, .5f, 1.f}, hbOther{0.f, 1.f, 0.f, 1.f};
    inline bool solidWave = false;
    inline bool trajectory = false;
    inline int trajLen = 120;
    inline bool codaTrail = false;
    inline Col trailColor{.965f, .635f, .71f, .9f};
    inline float trailWidth = 4.f;
    inline int trailLength = 120;
    inline bool autoCheckpoints = false;
    inline float autoCpEvery = 2.f;
    // Level
    inline bool layoutMode = false, noGlow = false, accuratePercent = false, startposSwitcher = true;
    inline int percentDecimals = 2;
    inline bool hideAttempts = false, noTransition = false;
    // Bypass
    inline bool fpsBypass = false, tpsBypass = false, speedhack = false, speedAudio = true, iconHack = false;
    inline int fps = 240, tps = 240;
    inline float speed = 1.f;
    // Labels
    inline bool labels = true, lblCodaSafe = true, lblFps = true, lblTps = false, lblCps = false, lblNoclip = true,
        lblNoclipDeaths = false, lblAttempts = false, lblSession = false, lblClock = false, lblBestRun = false,
        lblStartpos = true, lblMacro = true, lblInputs = false;
    inline int lblCorner = 0;
    inline float lblScale = 1.f, lblOpacity = .85f;
    inline Col lblColor{1.f, 1.f, 1.f, 1.f};
    inline bool lblShadow = true;
    // Coda Bot
    inline bool botPosFix = true;
    inline int botOffset = 0;
    inline bool frameAdvance = false;
    inline bool autoclicker = false, acP2 = false;
    inline int acHold = 2, acRelease = 2;
    inline bool pathfinder = false, showThoughts = true, showPath = false, analyzeButton = false;
    // Level Roulette
    inline int rlList = 0, rlFrom = 1, rlTo = 150, rlMaxFps = 240;
    inline bool rlLimitFps = false, rlSkipBeaten = true, rlSkip2P = false, rlAutoTps = false, rlRoulette = false;
    inline bool rlLen[5] = {true, true, true, true, true};
    // Menu
    inline Col accent{.965f, .635f, .71f, 1.f};
    inline Col bg{.11f, .086f, .133f, .94f};
    inline Col textCol{.953f, .925f, .851f, 1.f};
    inline float menuScale = 1.f, rounding = 9.f;
    inline bool menuAnim = true, dimGame = false, codaReacts = true;
}
