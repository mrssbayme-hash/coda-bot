#include "config.hpp"
#include "shared.hpp"
#include <sstream>

using namespace geode::prelude;

namespace cfg {
    Field* waitingBind = nullptr;
    static std::vector<Field> s_fields;
    std::vector<Field>& all() { return s_fields; }

    Field* find(std::string const& key) {
        for (auto& f : s_fields) if (f.key == key) return &f;
        return nullptr;
    }

    static std::string colText(Col const& c) {
        char b[96]; std::snprintf(b, sizeof b, "%.4f,%.4f,%.4f,%.4f", c.r, c.g, c.b, c.a); return b;
    }

    std::string text(Field const& f) {
        switch (f.type) {
            case T::Bool: return *(bool*)f.ptr ? "1" : "0";
            case T::Int: case T::Choice: return std::to_string(*(int*)f.ptr);
            case T::Float: { char b[32]; std::snprintf(b, sizeof b, "%.5f", *(float*)f.ptr); return b; }
            case T::Color: return colText(*(Col*)f.ptr);
            case T::Action: return "";
        }
        return "";
    }

    void setText(Field& f, std::string const& v) {
        try {
            switch (f.type) {
                case T::Bool: *(bool*)f.ptr = v == "1"; break;
                case T::Int: case T::Choice: {
                    int x = std::stoi(v);
                    if (f.type == T::Choice) x = std::clamp(x, 0, (int)f.choices.size() - 1);
                    else x = std::clamp(x, (int)f.mn, (int)f.mx);
                    *(int*)f.ptr = x; break;
                }
                case T::Float: *(float*)f.ptr = std::clamp(std::stof(v), f.mn, f.mx); break;
                case T::Color: {
                    Col c{}; if (std::sscanf(v.c_str(), "%f,%f,%f,%f", &c.r, &c.g, &c.b, &c.a) == 4) *(Col*)f.ptr = c; break;
                }
                case T::Action: break;
            }
        } catch (...) {}
    }

    void save() {
        std::string out;
        for (auto& f : s_fields) {
            if (f.type != T::Action) out += f.key + "=" + text(f) + "\n";
            if (f.bind != f.defBind) out += "bind." + f.key + "=" + std::to_string(f.bind) + "\n";
        }
        Mod::get()->setSavedValue<std::string>("menu-config", out);
    }

    void load() {
        auto data = Mod::get()->getSavedValue<std::string>("menu-config", "");
        std::istringstream in(data);
        std::string line;
        while (std::getline(in, line)) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            auto k = line.substr(0, eq), v = line.substr(eq + 1);
            if (k.rfind("bind.", 0) == 0) {
                if (auto f = find(k.substr(5))) { try { f->bind = std::stoi(v); } catch (...) {} }
            } else if (auto f = find(k)) setText(*f, v);
        }
    }

    void changed(Field& f) {
        if (f.onChange) f.onChange();
        save();
    }

    void resetAll() {
        for (auto& f : s_fields) {
            if (f.type != T::Action) setText(f, f.def);
            f.bind = f.defBind;
            if (f.onChange) f.onChange();
        }
        save();
    }

    std::vector<std::string> cheatsInUse() {
        std::vector<std::string> out;
        for (auto& f : s_fields) {
            if (!f.cheat) continue;
            bool on = f.cheatIf ? f.cheatIf() : (f.type == T::Bool && *(bool*)f.ptr);
            if (on) out.push_back(f.label);
        }
        if (replay::state() == replay::State::Playing) out.push_back("Replay playback");
        return out;
    }

    std::string keyName(int key) {
        if (key == 0) return "None";
        if (key == (int)KEY_OEM3) return "~";
        if (key >= (int)KEY_A && key <= (int)KEY_Z) return std::string(1, (char)('A' + key - (int)KEY_A));
        if (key >= (int)KEY_Zero && key <= (int)KEY_Nine) return std::string(1, (char)('0' + key - (int)KEY_Zero));
        if (key >= (int)KEY_F1 && key <= (int)KEY_F12) return "F" + std::to_string(key - (int)KEY_F1 + 1);
        switch ((enumKeyCodes)key) {
            case KEY_Tab: return "Tab"; case KEY_Space: return "Space"; case KEY_Shift: return "Shift";
            case KEY_Control: return "Ctrl"; case KEY_Alt: return "Alt"; case KEY_Insert: return "Insert";
            case KEY_Delete: return "Delete"; case KEY_Home: return "Home"; case KEY_End: return "End";
            case KEY_PageUp: return "Page Up"; case KEY_PageDown: return "Page Down";
            case KEY_Left: return "Left"; case KEY_Right: return "Right"; case KEY_Up: return "Up"; case KEY_Down: return "Down";
            case KEY_Backspace: return "Backspace"; case KEY_Enter: return "Enter";
            default: break;
        }
        return "Key " + std::to_string(key);
    }

    // ---------------------------------------------------------------- the list
    static Field& add(Field f) {
        f.def = f.type == T::Action ? "" : text(f);
        f.defBind = f.bind;
        s_fields.push_back(std::move(f));
        return s_fields.back();
    }
    static Field B(char const* key, char const* label, char const* win, bool* p, char const* tip = "", char const* parent = "") {
        Field f; f.key = key; f.label = label; f.win = win; f.tip = tip; f.parent = parent; f.type = T::Bool; f.ptr = p; return f;
    }
    static Field I(char const* key, char const* label, char const* win, int* p, int mn, int mx, char const* parent = "", bool typed = false) {
        Field f; f.key = key; f.label = label; f.win = win; f.parent = parent; f.type = T::Int; f.ptr = p; f.mn = (float)mn; f.mx = (float)mx; f.typed = typed; return f;
    }
    static Field F(char const* key, char const* label, char const* win, float* p, float mn, float mx, char const* fmt, char const* parent = "") {
        Field f; f.key = key; f.label = label; f.win = win; f.parent = parent; f.type = T::Float; f.ptr = p; f.mn = mn; f.mx = mx; f.fmt = fmt; return f;
    }
    static Field C(char const* key, char const* label, char const* win, Col* p, char const* parent = "") {
        Field f; f.key = key; f.label = label; f.win = win; f.parent = parent; f.type = T::Color; f.ptr = p; return f;
    }
    static Field Ch(char const* key, char const* label, char const* win, int* p, std::vector<std::string> ch, char const* parent = "") {
        Field f; f.key = key; f.label = label; f.win = win; f.parent = parent; f.type = T::Choice; f.ptr = p; f.choices = std::move(ch); return f;
    }
    static Field A(char const* key, char const* label, char const* win, int bind, std::function<void()> act) {
        Field f; f.key = key; f.label = label; f.win = win; f.type = T::Action; f.bind = bind; f.onChange = std::move(act); return f;
    }
    static Field cheat(Field f, std::function<bool()> when = nullptr) { f.cheat = true; f.cheatIf = std::move(when); return f; }
    static Field on(Field f, std::function<void()> cb) { f.onChange = std::move(cb); return f; }

    void init() {
        if (!s_fields.empty()) return;
        s_fields.reserve(160);
        using namespace V;
        // Player
        add(cheat(B("noclip", "Noclip", "Player", &noclip, "You can't die. Deaths are counted for noclip accuracy.")));
        add(B("noclip-p1", "Player 1", "Player", &noclipP1, "", "noclip"));
        add(B("noclip-p2", "Player 2", "Player", &noclipP2, "", "noclip"));
        add(B("noclip-flash", "Red flash on death", "Player", &noclipFlash, "", "noclip"));
        add(C("noclip-flash-color", "Flash color", "Player", &noclipFlashColor, "noclip"));
        add(B("noclip-limit", "Death limit", "Player", &noclipLimit, "Restarts the attempt when noclip accuracy drops below the limit. Uses your respawn delay.", "noclip"));
        add(F("noclip-limit-pct", "Lowest accuracy", "Player", &noclipLimitPct, 50.f, 100.f, "%.1f%%", "noclip-limit"));
        add(cheat(B("jump-hack", "Jump hack", "Player", &jumpHack, "Jump in mid-air. Hold to keep jumping.")));
        add(B("no-death-effect", "No death effect", "Player", &noDeathEffect));
        add(B("respawn-delay", "Respawn delay", "Player", &respawnDelayOn, "Change how long it takes to load the next attempt, in milliseconds. 0 skips the wait."));
        add(I("respawn-delay-ms", "Milliseconds", "Player", &respawnDelayMs, 0, 5000, "respawn-delay", true));
        add(B("no-respawn-blink", "No respawn blink", "Player", &noRespawnBlink));
        add(B("hide-player", "Hide player", "Player", &hidePlayer));
        add(B("click-sounds", "Click sounds", "Player", &clickSounds, "Plays a click every time you (or the bot) press."));
        add(Ch("click-style", "Sound", "Player", &clickStyle, {"Mouse", "Soft", "Coda"}, "click-sounds"));
        add(F("click-volume", "Volume", "Player", &clickVolume, 0.f, 1.f, "%.2f", "click-sounds"));
        add(B("click-release", "Release sound too", "Player", &clickRelease, "", "click-sounds"));
        add(on(cheat(B("hitboxes", "Show hitboxes", "Player", &hitboxes, "Doesn't count as a cheat in practice mode."),
            [] { auto pl = PlayLayer::get(); return V::hitboxes && !(pl && pl->m_isPracticeMode); }), [] { hacks::refreshHitboxes(); }));
        add(on(B("hitboxes-death", "Only on death", "Player", &hitboxesOnDeath, "", "hitboxes"), [] { hacks::refreshHitboxes(); }));
        add(B("hitbox-accurate", "Accurate player hitbox", "Player", &hitboxAccurate, "Draws your real hitbox (yellow) and the small inner one that solid blocks use (pink).", "hitboxes"));
        add(B("hitbox-hide-rotated", "Hide rotated hitbox", "Player", &hitboxHideRotated, "GD's own player box spins with your icon. This hides it.", "hitboxes"));
        add(B("hitbox-trail", "Hitbox trail", "Player", &hitboxTrail, "Leaves your hitbox behind on every physics step.", "hitboxes"));
        add(I("hitbox-trail-len", "Trail length (steps)", "Player", &hitboxTrailLen, 10, 2000, "hitboxes"));
        add(F("hitbox-width", "Border width", "Player", &hitboxWidth, .2f, 4.f, "%.1fx", "hitboxes"));
        add(B("hitbox-fill", "Fill hitboxes", "Player", &hitboxFill, "", "hitboxes"));
        add(F("hitbox-fill-alpha", "Fill opacity", "Player", &hitboxFillAlpha, 0.f, 1.f, "%.2f", "hitboxes"));
        add(C("hb-solid", "Solid color", "Player", &hbSolid, "hitboxes"));
        add(C("hb-danger", "Danger color", "Player", &hbDanger, "hitboxes"));
        add(C("hb-player", "Player color", "Player", &hbPlayer, "hitboxes"));
        add(C("hb-inner", "Inner player color", "Player", &hbInner, "hitboxes"));
        add(C("hb-other", "Other color", "Player", &hbOther, "hitboxes"));
        add(B("solid-wave", "Solid wave trail", "Player", &solidWave));
        add(cheat(B("trajectory", "Show trajectory", "Player", &trajectory, "Shows where you'd go if you hold (pink) or let go (white).")));
        add(I("trajectory-len", "Length (steps)", "Player", &trajLen, 30, 480, "trajectory"));
        add(B("coda-trail", "Coda trail", "Player", &codaTrail, "A soft trail behind your icon."));
        add(C("coda-trail-color", "Color", "Player", &trailColor, "coda-trail"));
        add(F("coda-trail-width", "Width", "Player", &trailWidth, 1.f, 12.f, "%.1f", "coda-trail"));
        add(I("coda-trail-len", "Length (frames)", "Player", &trailLength, 10, 600, "coda-trail"));
        add(B("auto-checkpoints", "Auto checkpoints", "Player", &autoCheckpoints, "In practice mode, places checkpoints for you while you're on the ground."));
        add(F("auto-cp-every", "Every", "Player", &autoCpEvery, .5f, 10.f, "%.1f s", "auto-checkpoints"));
        // Level
        add(on(cheat(B("layout-mode", "Layout mode", "Level", &layoutMode, "Shows just the layout: decoration is removed and colors go back to default. Restarts the level when you change it.")),
            [] { hacks::reopenLevel(); }));
        add(B("no-glow", "No glow", "Level", &noGlow, "Reopen the level to apply."));
        add(B("accurate-percent", "Accurate percent", "Level", &accuratePercent));
        add(I("percent-decimals", "Decimals", "Level", &percentDecimals, 1, 4, "accurate-percent"));
        add(B("startpos-switcher", "Start pos switcher", "Level", &startposSwitcher, "Switch between start positions with the keys set under Coda > Keybinds (Q and E)."));
        add(B("hide-attempts", "Hide attempt counter", "Level", &hideAttempts));
        add(B("no-transition", "No transitions", "Level", &noTransition, "Menus switch instantly instead of fading."));
        // Bypass
        add(on(B("fps-bypass", "FPS bypass", "Bypass", &fpsBypass, "Turn VSync off in GD's settings for this to work."), [] { hacks::applyFps(); }));
        add(on(I("fps", "FPS", "Bypass", &fps, 30, 10000, "fps-bypass", true), [] { hacks::applyFps(); }));
        add(on(cheat(B("tps-bypass", "TPS bypass", "Bypass", &tpsBypass, "Physics steps per second. 240 is normal. Anything else can't count."),
            [] { return V::tpsBypass && V::tps != 240; }), [] { hacks::applyTps(); }));
        add(on(I("tps", "TPS", "Bypass", &tps, 30, 10000, "tps-bypass", true), [] { hacks::applyTps(); }));
        add(on(cheat(B("speedhack", "Speedhack", "Bypass", &speedhack, "Speeds the whole game up or down."),
            [] { return V::speedhack && std::abs(V::speed - 1.f) > .001f; }), [] { hacks::applySpeedAudio(); }));
        add(on(F("speed", "Speed", "Bypass", &speed, .1f, 10.f, "%.2fx", "speedhack"), [] { hacks::applySpeedAudio(); }));
        add(on(B("speed-audio", "Music follows speed", "Bypass", &speedAudio, "", "speedhack"), [] { hacks::applySpeedAudio(); }));
        add(B("icon-hack", "Unlock all icons", "Bypass", &iconHack, "Every icon and color can be picked in the icon kit."));
        // Labels
        add(B("labels", "Show labels", "Labels", &labels));
        add(B("lbl-safe", "Coda Safe status", "Labels", &lblCodaSafe, "Only shows when the run can't count."));
        add(B("lbl-fps", "FPS", "Labels", &lblFps));
        add(B("lbl-tps", "TPS", "Labels", &lblTps));
        add(B("lbl-cps", "Clicks per second", "Labels", &lblCps));
        add(B("lbl-noclip", "Noclip accuracy", "Labels", &lblNoclip, "Shows while noclip is on."));
        add(B("lbl-noclip-deaths", "Noclip deaths", "Labels", &lblNoclipDeaths));
        add(B("lbl-attempts", "Attempts", "Labels", &lblAttempts));
        add(B("lbl-session", "Session time", "Labels", &lblSession));
        add(B("lbl-clock", "Clock", "Labels", &lblClock));
        add(B("lbl-best", "Best run", "Labels", &lblBestRun, "Your best run this session, like 34% to 78%."));
        add(B("lbl-startpos", "Start pos", "Labels", &lblStartpos));
        add(B("lbl-macro", "Bot status", "Labels", &lblMacro));
        add(B("lbl-inputs", "Show clicks", "Labels", &lblInputs, "Two boxes that light up while P1 and P2 are holding."));
        add(Ch("lbl-corner", "Corner", "Labels", &lblCorner, {"Top left", "Top right", "Bottom left", "Bottom right"}));
        add(F("lbl-scale", "Size", "Labels", &lblScale, .5f, 2.5f, "%.2f"));
        add(F("lbl-opacity", "Opacity", "Labels", &lblOpacity, .1f, 1.f, "%.2f"));
        add(C("lbl-color", "Color", "Labels", &lblColor));
        add(B("lbl-shadow", "Shadow", "Labels", &lblShadow));
        // Coda Bot
        add(B("bot-pos-fix", "Position fix", "Coda Bot", &botPosFix, "Saves where you were on every click and puts you back there on playback. Much more accurate."));
        add(I("bot-offset", "Frame offset", "Coda Bot", &botOffset, -3, 3));
        add(cheat(B("frame-advance", "Frame advance", "Coda Bot", &frameAdvance, "Freezes the game. Press the step key (F) to move one physics step.")));
        add(cheat(B("autoclicker", "Autoclicker", "Coda Bot", &autoclicker)));
        add(I("ac-hold", "Hold (steps)", "Coda Bot", &acHold, 1, 120, "autoclicker"));
        add(I("ac-release", "Release (steps)", "Coda Bot", &acRelease, 1, 120, "autoclicker"));
        add(B("ac-p2", "Player 2 too", "Coda Bot", &acP2, "", "autoclicker"));
        add(on(cheat(B("pathfinder", "Coda plays (pathfinder)", "Coda Bot", &pathfinder, "Coda finds a path through the level himself.")),
            [] { bot::setEnabled(V::pathfinder); }));
        add(B("show-thoughts", "Show his thoughts", "Coda Bot", &showThoughts, "", "pathfinder"));
        add(B("show-path", "Show his path", "Coda Bot", &showPath, "", "pathfinder"));
        add(B("analyze-button", "Analyze button on levels", "Coda Bot", &analyzeButton, "A Coda button on level pages that works out a route, then plays it."));
        // Level Roulette (drawn by its own window)
        add(I("rl-list", "List", "Roulette", &rlList, 0, 4));
        add(I("rl-from", "From", "Roulette", &rlFrom, 1, 5000));
        add(I("rl-to", "To", "Roulette", &rlTo, 1, 5000));
        add(B("rl-limit-fps", "Max FPS/TPS needed", "Roulette", &rlLimitFps));
        add(I("rl-max-fps", "Max FPS/TPS", "Roulette", &rlMaxFps, 30, 20000));
        add(B("rl-skip-beaten", "Skip levels I've beaten", "Roulette", &rlSkipBeaten));
        add(B("rl-skip-2p", "Skip 2-player levels", "Roulette", &rlSkip2P));
        add(B("rl-auto-tps", "Set TPS to what the level needs", "Roulette", &rlAutoTps));
        add(B("rl-roulette", "Roulette mode", "Roulette", &rlRoulette));
        add(B("rl-len-0", "Tiny", "Roulette", &rlLen[0]));
        add(B("rl-len-1", "Short", "Roulette", &rlLen[1]));
        add(B("rl-len-2", "Medium", "Roulette", &rlLen[2]));
        add(B("rl-len-3", "Long", "Roulette", &rlLen[3]));
        add(B("rl-len-4", "XL", "Roulette", &rlLen[4]));
        // Menu
        add(C("accent", "Accent color", "Coda", &accent));
        add(C("bg", "Window color", "Coda", &bg));
        add(C("text", "Text color", "Coda", &textCol));
        add(F("menu-scale", "Menu size", "Coda", &menuScale, .7f, 1.8f, "%.2f"));
        add(F("rounding", "Roundness", "Coda", &rounding, 0.f, 14.f, "%.0f"));
        add(B("menu-anim", "Fade in and out", "Coda", &menuAnim));
        add(B("dim-game", "Dim the game while open", "Coda", &dimGame));
        add(B("coda-reacts", "Coda reacts when you die", "Coda", &codaReacts));
        // Keybind-only actions
        add(A("menu-key", "Open the menu", "Keys", (int)KEY_OEM3, [] { menu::toggle(); }));
        add(A("startpos-prev", "Previous start pos", "Keys", (int)KEY_Q, [] { hacks::startposStep(-1); }));
        add(A("startpos-next", "Next start pos", "Keys", (int)KEY_E, [] { hacks::startposStep(1); }));
        add(A("frame-step", "Step one frame", "Keys", (int)KEY_F, [] { hacks::frameStep(); }));
        add(A("bot-record", "Start or stop recording", "Keys", 0, [] {
            if (replay::state() == replay::State::Recording) replay::stop(); else replay::record(); }));
        add(A("bot-play", "Start or stop playback", "Keys", 0, [] {
            if (replay::state() == replay::State::Playing) replay::stop(); else replay::play(); }));
        load();
        // Coda never starts playing by himself: his pathfinder is off every time the game opens
        pathfinder = false;
    }
}
