// Coach v1.0 - adaptive trainer for Geometry Dash (Geode v6)
//
// - Death heatmap per level (100 buckets of 1%), saved between sessions
// - Auto-detects your hardest sections
// - Training: auto checkpoint before the section, loops it in practice mode,
//   starts slow and speeds up as you pass it, moves on when mastered
// - Stats, in-game heat strip HUD, death markers, tuning and options

#include <Geode/Geode.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

using namespace geode::prelude;

namespace coach {

// ====================== datos ======================

struct LevelData {
    std::array<int, 100> heat{};
    int completions = 0;
    int best = 0;
    int trainPasses = 0;
    int trainTries = 0;
    int mastered = 0;
    std::vector<std::pair<int, int>> marks;

    int totalDeaths() const {
        int s = 0;
        for (int h : heat) s += h;
        return s;
    }
};

struct Section {
    int start;
    int end;
    int deaths;
};

struct Train {
    bool active = false;
    bool justStarted = false;
    bool cpPlaced = false;
    std::vector<Section> sections;
    int cur = 0;
    float speed = 1.f;
    int streak = 0;
    int mastery = 0;
    int failStreak = 0;
    int attempts = 0;
    int passes = 0;
};

inline LevelData data;
inline Train train;
inline std::string levelKeyStr = "0|Unknown";
inline std::string levelName = "Unknown";
inline int levelID = 0;
inline bool platformer = false;
inline int sessionDeaths = 0;
inline bool deathRecorded = false;
inline bool heatDirty = true;
inline int drawnStamp = -1;
inline int tab = 0;

// ajustes numericos
inline int cfgStart = 50;
inline int cfgStep = 10;
inline int cfgNeed = 3;
inline int cfgMastery = 3;
inline int cfgLead = 3;
inline int cfgTail = 2;
inline int cfgMaxSections = 3;
inline int cfgManualStart = 0;
inline int cfgManualEnd = 10;

// opciones
inline bool optStrip = true;
inline bool optHudText = true;
inline bool optMarkers = true;
inline bool optCountPractice = true;
inline bool optAutoPractice = true;
inline bool optSlowOnFail = true;

struct Setting {
    char const* name;
    int min;
    int max;
    int step;
    int* value;
};

inline Setting settings[] = {
    { "Start speed (%)", 25, 90, 5, &cfgStart },
    { "Speed step (%)", 5, 25, 5, &cfgStep },
    { "Passes to speed up", 1, 10, 1, &cfgNeed },
    { "Passes to master", 1, 10, 1, &cfgMastery },
    { "Lead-in (%)", 0, 10, 1, &cfgLead },
    { "Tail (%)", 0, 5, 1, &cfgTail },
    { "Max sections", 1, 5, 1, &cfgMaxSections },
    { "Manual start (%)", 0, 98, 1, &cfgManualStart },
    { "Manual end (%)", 1, 99, 1, &cfgManualEnd },
};
inline constexpr int settingCount = 9;

inline bool* optPtr(int tag) {
    switch (tag) {
        case 1: return &optStrip;
        case 2: return &optHudText;
        case 3: return &optMarkers;
        case 4: return &optCountPractice;
        case 5: return &optAutoPractice;
        case 6: return &optSlowOnFail;
    }
    return nullptr;
}

// ====================== utilidades ======================

template <class T>
int toInt(T v) {
    if constexpr (requires { v.value(); }) return static_cast<int>(v.value());
    else return static_cast<int>(v);
}

inline std::string clean(std::string s) {
    for (auto& c : s) if (c == '\t' || c == '\n' || c == '\r' || c == '|') c = ' ';
    return s;
}

inline void notify(std::string const& text, NotificationIcon icon = NotificationIcon::Success) {
    Notification::create(text, icon, 1.8f)->show();
}

inline std::vector<std::string> split(std::string const& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

inline ccColor4F lerpColor(ccColor4F a, ccColor4F b, float t) {
    return ccColor4F{
        a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
        a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t
    };
}

// verde -> amarillo -> rojo
inline ccColor4F heatColor(float t, float alpha = .9f) {
    t = std::clamp(t, 0.f, 1.f);
    ccColor4F green{ .2f, .9f, .3f, alpha };
    ccColor4F yellow{ 1.f, .9f, .1f, alpha };
    ccColor4F red{ 1.f, .2f, .1f, alpha };
    if (t < .5f) return lerpColor(green, yellow, t * 2.f);
    return lerpColor(yellow, red, (t - .5f) * 2.f);
}

inline void rect(CCDrawNode* n, float x, float y, float w, float h,
                 ccColor4F fill, ccColor4F border, float bw = .3f) {
    CCPoint v[4] = { { x, y }, { x + w, y }, { x + w, y + h }, { x, y + h } };
    n->drawPolygon(v, 4, fill, bw, border);
}

// ====================== guardado ======================

inline std::string serialize(LevelData const& d) {
    std::ostringstream o;
    o << "v1|" << d.completions << '|' << d.best << '|' << d.trainPasses << '|'
      << d.trainTries << '|' << d.mastered << '|';
    for (size_t i = 0; i < 100; i++) {
        if (i) o << ',';
        o << d.heat[i];
    }
    o << '|';
    for (size_t i = 0; i < d.marks.size(); i++) {
        if (i) o << ',';
        o << d.marks[i].first << ':' << d.marks[i].second;
    }
    return o.str();
}

inline LevelData parse(std::string const& s) {
    LevelData d;
    if (s.empty()) return d;
    auto parts = split(s, '|');
    if (parts.size() < 8 || parts[0] != "v1") return d;

    d.completions = std::atoi(parts[1].c_str());
    d.best = std::atoi(parts[2].c_str());
    d.trainPasses = std::atoi(parts[3].c_str());
    d.trainTries = std::atoi(parts[4].c_str());
    d.mastered = std::atoi(parts[5].c_str());

    auto heat = split(parts[6], ',');
    for (size_t i = 0; i < heat.size() && i < 100; i++) d.heat[i] = std::atoi(heat[i].c_str());

    if (!parts[7].empty()) {
        for (auto const& m : split(parts[7], ',')) {
            auto xy = split(m, ':');
            if (xy.size() == 2) d.marks.push_back({ std::atoi(xy[0].c_str()), std::atoi(xy[1].c_str()) });
        }
    }
    return d;
}

inline void loadData() {
    data = parse(Mod::get()->getSavedValue<std::string>("lvl:" + levelKeyStr));
}

inline void saveData() {
    auto m = Mod::get();
    m->setSavedValue<std::string>("lvl:" + levelKeyStr, serialize(data));

    auto idx = m->getSavedValue<std::string>("levels");
    if (("\n" + idx).find("\n" + levelKeyStr + "\n") == std::string::npos) {
        idx += levelKeyStr + "\n";
        m->setSavedValue<std::string>("levels", idx);
    }
}

inline void resetLevelData() {
    data = LevelData{};
    sessionDeaths = 0;
    heatDirty = true;
    saveData();
}

inline void resetAllData() {
    auto m = Mod::get();
    auto idx = m->getSavedValue<std::string>("levels");
    for (auto const& key : split(idx, '\n')) {
        if (!key.empty()) m->setSavedValue<std::string>("lvl:" + key, "");
    }
    m->setSavedValue<std::string>("levels", "");
    data = LevelData{};
    sessionDeaths = 0;
    heatDirty = true;
}

inline void saveCfg() {
    auto m = Mod::get();
    for (int i = 0; i < settingCount; i++)
        m->setSavedValue<int>("cfg-" + std::to_string(i), *settings[i].value);
    for (int t = 1; t <= 6; t++)
        m->setSavedValue<bool>("opt-" + std::to_string(t), *optPtr(t));
    m->setSavedValue<int>("tab", tab);
}

inline void loadCfg() {
    auto m = Mod::get();
    for (int i = 0; i < settingCount; i++) {
        auto& s = settings[i];
        *s.value = std::clamp(m->getSavedValue<int>("cfg-" + std::to_string(i), *s.value), s.min, s.max);
    }
    if (cfgManualEnd <= cfgManualStart) cfgManualEnd = std::min(99, cfgManualStart + 1);
    for (int t = 1; t <= 6; t++)
        *optPtr(t) = m->getSavedValue<bool>("opt-" + std::to_string(t), *optPtr(t));
    tab = std::clamp(m->getSavedValue<int>("tab", 0), 0, 4);
}

// ====================== analisis ======================

// Busca las zonas con mas muertes. Devuelve las secciones ordenadas por inicio.
inline std::vector<Section> detect(LevelData const& d, int maxCount, int lead, int tail) {
    std::vector<Section> out;
    int total = d.totalDeaths();
    if (total < 3) return out;

    constexpr int W = 4;
    auto heat = d.heat;
    int minScore = std::max(3, total / 20);
    int exclusion = W + lead + tail;

    for (int n = 0; n < maxCount; n++) {
        int bestI = -1;
        int bestS = 0;
        for (int i = 0; i + W <= 100; i++) {
            int s = 0;
            for (int k = 0; k < W; k++) s += heat[i + k];
            if (s > bestS) { bestS = s; bestI = i; }
        }
        if (bestI < 0 || bestS < minScore) break;

        int real = 0;
        for (int k = 0; k < W; k++) real += d.heat[bestI + k];

        out.push_back({ std::max(0, bestI - lead), std::min(99, bestI + W + tail), real });

        for (int k = std::max(0, bestI - exclusion); k < std::min(100, bestI + W + exclusion); k++)
            heat[k] = 0;
    }

    std::sort(out.begin(), out.end(), [](Section const& a, Section const& b) { return a.start < b.start; });
    return out;
}

inline std::vector<Section> detectByDeaths(LevelData const& d) {
    auto secs = detect(d, cfgMaxSections, cfgLead, cfgTail);
    std::sort(secs.begin(), secs.end(), [](Section const& a, Section const& b) { return a.deaths > b.deaths; });
    return secs;
}

inline Section manualSection() {
    int s = std::clamp(cfgManualStart, 0, 98);
    int e = std::clamp(cfgManualEnd, s + 1, 99);
    int deaths = 0;
    for (int i = s; i <= e && i < 100; i++) deaths += data.heat[i];
    return { s, e, deaths };
}

inline bool startTraining(std::vector<Section> secs) {
    if (platformer) {
        notify("Platformer levels are not supported", NotificationIcon::Error);
        return false;
    }
    if (secs.empty()) {
        notify("No sections yet: play the level and die a few times first", NotificationIcon::Error);
        return false;
    }
    train = Train{};
    train.active = true;
    train.justStarted = true;
    train.sections = std::move(secs);
    train.speed = cfgStart / 100.f;
    heatDirty = true;
    notify("Training started: resume the game");
    return true;
}

inline void stopTraining() {
    if (train.active) saveData();
    train = Train{};
    heatDirty = true;
}

// velocidad aplicada al juego (1 = normal)
inline float effectiveSpeed() {
    if (!train.active || train.speed >= .999f) return 1.f;
    if (!PlayLayer::get()) return 1.f;
    auto scene = CCDirector::get()->getRunningScene();
    if (!scene) return 1.f;
    for (auto child : CCArrayExt<CCNode*>(scene->getChildren())) {
        if (typeinfo_cast<PauseLayer*>(child)) return 1.f;
    }
    return train.speed;
}

inline ButtonSprite* button(char const* text, int width, float scale = .6f,
                            char const* texture = "GJ_button_01.png") {
    return ButtonSprite::create(text, width, true, "bigFont.fnt", texture, 25.f, scale);
}

} // namespace coach

$on_mod(Loaded) {
    coach::loadCfg();
}

// ====================== velocidad de entrenamiento ======================

class $modify(CoachScheduler, CCScheduler) {
    void update(float dt) {
        float s = coach::effectiveSpeed();
        CCScheduler::update(s == 1.f ? dt : dt * s);
    }
};

// ====================== juego ======================

class $modify(CoachPlayLayer, PlayLayer) {
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();

        int id = coach::toInt(m_level->m_levelID);
        std::string name = coach::clean(static_cast<std::string>(m_level->m_levelName));
        if (name.empty()) name = "Unnamed";
        std::string key = std::to_string(id) + "|" + name;

        if (key != coach::levelKeyStr) coach::sessionDeaths = 0;
        coach::levelID = id;
        coach::levelName = name;
        coach::levelKeyStr = key;
        coach::platformer = m_level->isPlatformer();
        coach::loadData();

        coach::train = coach::Train{};
        coach::deathRecorded = false;
        coach::heatDirty = true;
        coach::drawnStamp = -1;

        auto win = CCDirector::get()->getWinSize();

        auto strip = CCDrawNode::create();
        strip->setID("strip"_spr);
        this->addChild(strip, 1000);

        auto marker = CCDrawNode::create();
        marker->setID("marker"_spr);
        this->addChild(marker, 1001);

        auto label = CCLabelBMFont::create("", "bigFont.fnt");
        label->setID("label"_spr);
        label->setScale(.33f);
        label->setOpacity(210);
        label->setPosition({ win.width / 2.f, 24.f });
        this->addChild(label, 1002);

        if (m_objectLayer) {
            auto marks = CCDrawNode::create();
            marks->setID("marks"_spr);
            m_objectLayer->addChild(marks, 9998);
        }
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        coach::deathRecorded = false;
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (!coach::deathRecorded && !coach::platformer) {
            coach::deathRecorded = true;
            this->coachOnDeath();
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void levelComplete() {
        if (!coach::platformer && !coach::train.active && !m_isPracticeMode) {
            coach::data.completions++;
            coach::data.best = 100;
            coach::saveData();
            coach::heatDirty = true;

            auto secs = coach::detectByDeaths(coach::data);
            if (!secs.empty()) {
                char buf[96];
                std::snprintf(buf, sizeof buf, "Level complete! Hardest spot: %d-%d%% (%d deaths)",
                    secs[0].start, secs[0].end, secs[0].deaths);
                coach::notify(buf);
            }
        }
        PlayLayer::levelComplete();
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        if (coach::platformer) return;
        this->coachTick();
        this->coachHud();
    }

    void onQuit() {
        coach::saveData();
        coach::train = coach::Train{};
        PlayLayer::onQuit();
    }

    // ---------- muertes ----------

    void coachOnDeath() {
        auto& t = coach::train;
        float pct = this->getCurrentPercent();
        int bucket = std::clamp(static_cast<int>(pct), 0, 99);

        if (t.active) {
            // durante el entrenamiento no se toca el mapa de calor
            if (t.cpPlaced && t.cur < static_cast<int>(t.sections.size())) {
                t.attempts++;
                coach::data.trainTries++;
                t.streak = 0;
                t.mastery = 0;
                t.failStreak++;

                if (coach::optSlowOnFail && t.failStreak >= 3) {
                    float lowest = coach::cfgStart / 100.f;
                    float next = std::max(lowest, t.speed - coach::cfgStep / 200.f);
                    t.failStreak = 0;
                    if (next < t.speed - .001f) {
                        t.speed = next;
                        char buf[48];
                        std::snprintf(buf, sizeof buf, "Slowing down: %d%%", static_cast<int>(std::round(next * 100.f)));
                        coach::notify(buf, NotificationIcon::Warning);
                    }
                }
            }
            return;
        }

        bool practice = m_isPracticeMode;
        if (!practice || coach::optCountPractice) {
            coach::data.heat[bucket]++;
            coach::sessionDeaths++;
            if (m_player1) {
                coach::data.marks.push_back({
                    static_cast<int>(m_player1->getPositionX()),
                    static_cast<int>(m_player1->getPositionY())
                });
                if (coach::data.marks.size() > 120)
                    coach::data.marks.erase(coach::data.marks.begin());
            }
            coach::heatDirty = true;
        }
        if (!practice && static_cast<int>(pct) > coach::data.best) {
            coach::data.best = static_cast<int>(pct);
            coach::notify("New best: " + std::to_string(coach::data.best) + "%");
        }
        coach::saveData();
    }

    // ---------- entrenamiento ----------

    void coachTick() {
        auto& t = coach::train;
        if (!t.active) return;

        if (t.justStarted) {
            t.justStarted = false;
            if (!m_isPracticeMode) {
                if (!coach::optAutoPractice) {
                    coach::notify("Turn on practice mode to train", NotificationIcon::Error);
                    coach::stopTraining();
                    return;
                }
                this->togglePracticeMode(true);
            }
            this->removeAllCheckpoints();
            t.cpPlaced = false;
            this->resetLevel();
            return;
        }

        if (coach::deathRecorded) return;
        if (t.cur >= static_cast<int>(t.sections.size())) return;

        auto const& sec = t.sections[t.cur];
        float pct = this->getCurrentPercent();

        if (!t.cpPlaced) {
            if (pct >= sec.start) {
                this->markCheckpoint();
                t.cpPlaced = true;
            }
        } else if (pct >= sec.end) {
            this->coachSectionPassed();
        }
    }

    void coachSectionPassed() {
        auto& t = coach::train;
        t.attempts++;
        t.passes++;
        t.failStreak = 0;
        coach::data.trainTries++;
        coach::data.trainPasses++;

        bool mastered = false;
        if (t.speed >= .999f) {
            t.mastery++;
            if (t.mastery >= coach::cfgMastery) mastered = true;
        } else {
            t.streak++;
            if (t.streak >= coach::cfgNeed) {
                t.speed = std::min(1.f, t.speed + coach::cfgStep / 100.f);
                t.streak = 0;
                char buf[48];
                std::snprintf(buf, sizeof buf, "Speed up: %d%%", static_cast<int>(std::round(t.speed * 100.f)));
                coach::notify(buf);
            }
        }

        if (!mastered) {
            this->resetLevel(); // vuelve al checkpoint y repite la seccion
            return;
        }

        coach::data.mastered++;
        t.cur++;

        if (t.cur >= static_cast<int>(t.sections.size())) {
            coach::stopTraining();
            coach::saveData();
            FLAlertLayer::create(
                "Coach",
                "All sections mastered!\nTry a full run now.",
                "OK"
            )->show();
            return;
        }

        coach::notify("Section mastered! Next section...");
        t.speed = coach::cfgStart / 100.f;
        t.streak = 0;
        t.mastery = 0;
        t.failStreak = 0;
        t.attempts = 0;
        t.passes = 0;
        t.cpPlaced = false;
        this->removeAllCheckpoints();
        this->markCheckpoint(); // checkpoint justo aqui, para no repetir desde el inicio
        coach::saveData();
    }

    // ---------- HUD ----------

    void coachHud() {
        auto win = CCDirector::get()->getWinSize();
        float w = win.width * .5f;
        float x0 = (win.width - w) / 2.f;
        float y0 = 5.f;
        float h = 7.f;

        auto strip = typeinfo_cast<CCDrawNode*>(this->getChildByID("strip"_spr));
        auto marker = typeinfo_cast<CCDrawNode*>(this->getChildByID("marker"_spr));
        auto label = typeinfo_cast<CCLabelBMFont*>(this->getChildByID("label"_spr));
        CCDrawNode* marks = nullptr;
        if (m_objectLayer) marks = typeinfo_cast<CCDrawNode*>(m_objectLayer->getChildByID("marks"_spr));

        float pct = this->getCurrentPercent();
        auto const& t = coach::train;
        bool trainingNow = t.active && t.cur < static_cast<int>(t.sections.size());

        if (strip && marker) {
            bool vis = coach::optStrip;
            strip->setVisible(vis);
            marker->setVisible(vis);

            int stamp = trainingNow ? t.cur + 1 : 0;
            if (vis && (coach::heatDirty || stamp != coach::drawnStamp)) {
                strip->clear();
                int maxv = *std::max_element(coach::data.heat.begin(), coach::data.heat.end());
                float bw = w / 100.f;
                for (int i = 0; i < 100; i++) {
                    int v = coach::data.heat[i];
                    ccColor4F fill = v == 0
                        ? ccColor4F{ 1.f, 1.f, 1.f, .12f }
                        : coach::heatColor(maxv ? static_cast<float>(v) / maxv : 0.f, .85f);
                    coach::rect(strip, x0 + i * bw, y0, bw, h, fill, fill, .2f);
                }
                if (trainingNow) {
                    auto const& sec = t.sections[t.cur];
                    coach::rect(strip, x0 + sec.start * bw - 1.5f, y0 - 1.5f,
                                (sec.end - sec.start + 1) * bw + 3.f, h + 3.f,
                                ccColor4F{ 0.f, 0.f, 0.f, 0.f }, ccColor4F{ .3f, .9f, 1.f, 1.f }, 1.2f);
                }
                coach::heatDirty = false;
                coach::drawnStamp = stamp;
            }
            if (vis) {
                marker->clear();
                float mx = x0 + w * std::clamp(pct, 0.f, 100.f) / 100.f;
                ccColor4F white{ 1.f, 1.f, 1.f, 1.f };
                coach::rect(marker, mx - 1.f, y0 - 2.f, 2.f, h + 4.f, white, white, .2f);
            }
        }

        if (label) {
            bool vis = trainingNow && coach::optHudText;
            label->setVisible(vis);
            if (vis) {
                auto const& sec = t.sections[t.cur];
                char buf[160];
                std::snprintf(buf, sizeof buf,
                    "Coach  |  Section %d/%d (%d-%d%%)  |  Speed %d%%  |  Streak %d/%d",
                    t.cur + 1, static_cast<int>(t.sections.size()), sec.start, sec.end,
                    static_cast<int>(std::round(t.speed * 100.f)),
                    t.speed >= .999f ? t.mastery : t.streak,
                    t.speed >= .999f ? coach::cfgMastery : coach::cfgNeed);
                label->setString(buf);
            }
        }

        if (marks) {
            marks->clear();
            if (coach::optMarkers && m_player1) {
                float px = m_player1->getPositionX();
                float range = win.width * 1.3f;
                for (auto const& m : coach::data.marks) {
                    float mx = static_cast<float>(m.first);
                    float my = static_cast<float>(m.second);
                    if (std::abs(mx - px) > range) continue;
                    CCPoint v[4] = { { mx, my + 5.f }, { mx + 4.f, my }, { mx, my - 5.f }, { mx - 4.f, my } };
                    marks->drawPolygon(v, 4, ccColor4F{ 1.f, .15f, .15f, .55f }, .5f, ccColor4F{ 1.f, .6f, .6f, .9f });
                }
            }
        }
    }
};

// ====================== menu de pausa ======================

class $modify(CoachPauseLayer, PauseLayer) {
    struct Fields {
        int confirm = 0; // 1 = reset nivel, 2 = reset todo
    };

    void customSetup() {
        PauseLayer::customSetup();

        auto win = CCDirector::get()->getWinSize();
        auto menu = CCMenu::create();
        menu->setID("menu"_spr);
        menu->setPosition({ 0.f, 0.f });

        auto open = CCMenuItemSpriteExtra::create(
            coach::button("Coach", 60, .45f), this, menu_selector(CoachPauseLayer::onOpen)
        );
        open->setPosition({ win.width - 60.f, win.height - 22.f });
        menu->addChild(open);
        this->addChild(menu);
    }

    void setMenusEnabled(bool on) {
        for (auto child : CCArrayExt<CCNode*>(this->getChildren())) {
            if (auto m = typeinfo_cast<CCMenu*>(child)) m->setEnabled(on);
        }
    }

    void buildPanel() {
        if (auto old = this->getChildByID("panel"_spr)) old->removeFromParent();

        auto win = CCDirector::get()->getWinSize();
        float cx = win.width / 2.f;
        float cy = win.height / 2.f;
        float ccx = cx + 46.f;

        auto panel = CCNode::create();
        panel->setID("panel"_spr);
        panel->setZOrder(100);
        panel->addChild(CCLayerColor::create(ccc4(0, 0, 0, 170), win.width, win.height));

        auto frame = CCScale9Sprite::create("GJ_square02.png");
        frame->setContentSize({ 470.f, 262.f });
        frame->setPosition({ cx, cy });
        panel->addChild(frame);

        auto inner = CCScale9Sprite::create("square02_001.png");
        inner->setContentSize({ 357.f, 196.f });
        inner->setColor(ccc3(0, 0, 0));
        inner->setOpacity(110);
        inner->setPosition({ ccx, cy - 6.f });
        panel->addChild(inner);

        auto title = CCLabelBMFont::create("Coach", "goldFont.fnt");
        title->setScale(.75f);
        title->setPosition({ cx - 20.f, cy + 112.f });
        panel->addChild(title);

        auto ver = CCLabelBMFont::create("v1.0", "bigFont.fnt");
        ver->setScale(.3f);
        ver->setOpacity(150);
        ver->setPosition({ cx + 40.f, cy + 108.f });
        panel->addChild(ver);

        auto menu = CCMenu::create();
        menu->setPosition({ 0.f, 0.f });
        panel->addChild(menu);

        auto addBtn = [&](char const* text, int w, SEL_MenuHandler cb, CCPoint pos, int tag = 0,
                          char const* tex = "GJ_button_01.png", float scale = .6f) {
            auto item = CCMenuItemSpriteExtra::create(coach::button(text, w, scale, tex), this, cb);
            item->setPosition(pos);
            item->setTag(tag);
            menu->addChild(item);
            return item;
        };

        auto addLabel = [&](std::string const& text, CCPoint pos, float scale, bool left = false, int opacity = 255) {
            auto l = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
            l->setScale(scale);
            l->setOpacity(opacity);
            if (left) l->setAnchorPoint({ 0.f, .5f });
            l->setPosition(pos);
            panel->addChild(l);
            return l;
        };

        auto addToggle = [&](int tag, char const* label, float y) {
            auto tog = CCMenuItemToggler::createWithStandardSprites(
                this, menu_selector(CoachPauseLayer::onToggle), .6f
            );
            tog->setTag(tag);
            tog->toggle(*coach::optPtr(tag));
            tog->setPosition({ ccx - 155.f, y });
            menu->addChild(tog);
            addLabel(label, { ccx - 135.f, y + 1.f }, .42f, true);
        };

        auto addStepper = [&](int id, float y, float lx, float mx) {
            auto const& s = coach::settings[id];
            addLabel(s.name, { lx, y }, .38f, true);
            addBtn("-", 28, menu_selector(CoachPauseLayer::onStep), { mx, y }, id * 2, "GJ_button_06.png", .5f);
            addLabel(std::to_string(*s.value), { mx + 40.f, y }, .5f);
            addBtn("+", 28, menu_selector(CoachPauseLayer::onStep), { mx + 80.f, y }, id * 2 + 1, "GJ_button_01.png", .5f);
        };

        // cerrar
        auto closeItem = CCMenuItemSpriteExtra::create(
            CCSprite::createWithSpriteFrameName("GJ_closeBtn_001.png"), this,
            menu_selector(CoachPauseLayer::onClose)
        );
        closeItem->setPosition({ cx + 212.f, cy + 112.f });
        menu->addChild(closeItem);

        // pestanas
        char const* names[] = { "Heat", "Train", "Stats", "Tuning", "Options" };
        for (int i = 0; i < 5; i++) {
            addBtn(names[i], 86, menu_selector(CoachPauseLayer::onTab), { cx - 185.f, cy + 70.f - i * 36.f }, i,
                   i == coach::tab ? "GJ_button_01.png" : "GJ_button_04.png", .5f);
        }

        auto const& d = coach::data;
        char buf[160];

        switch (coach::tab) {
            case 0: { // ---------------- HEAT ----------------
                if (coach::platformer) {
                    addLabel("Platformer levels are not supported", { ccx, cy }, .5f);
                    break;
                }

                auto chart = CCDrawNode::create();
                panel->addChild(chart);
                float x0 = ccx - 165.f;
                float w = 330.f;
                float y0 = cy + 24.f;
                float h = 62.f;
                float bw = w / 100.f;

                coach::rect(chart, x0, y0, w, h, ccColor4F{ 0.f, 0.f, 0.f, .45f }, ccColor4F{ 1.f, 1.f, 1.f, .25f }, .6f);

                auto secs = coach::detect(d, coach::cfgMaxSections, coach::cfgLead, coach::cfgTail);
                for (auto const& s : secs) {
                    coach::rect(chart, x0 + s.start * bw, y0, (s.end - s.start + 1) * bw, h,
                                ccColor4F{ .2f, .7f, 1.f, .18f }, ccColor4F{ .2f, .7f, 1.f, .0f }, .2f);
                }

                int maxv = *std::max_element(d.heat.begin(), d.heat.end());
                for (int i = 0; i < 100; i++) {
                    if (d.heat[i] == 0) continue;
                    float t = static_cast<float>(d.heat[i]) / maxv;
                    float bh = std::max(2.f, t * (h - 4.f));
                    auto c = coach::heatColor(t);
                    coach::rect(chart, x0 + i * bw, y0 + 1.f, bw - .4f, bh, c, c, .1f);
                }

                if (d.best > 0) {
                    ccColor4F white{ 1.f, 1.f, 1.f, .9f };
                    coach::rect(chart, x0 + w * d.best / 100.f - .8f, y0, 1.6f, h, white, white, .1f);
                }

                if (maxv == 0)
                    addLabel("No deaths recorded yet", { ccx, y0 + h / 2.f }, .45f, false, 170);

                for (int k = 0; k <= 4; k++) {
                    addLabel(std::to_string(k * 25), { x0 + w * k / 4.f, cy + 15.f }, .22f, false, 170);
                }

                addLabel("Hardest sections", { ccx - 165.f, cy + 2.f }, .36f, true, 210);

                auto sorted = coach::detectByDeaths(d);
                if (sorted.empty()) {
                    addLabel("Sections appear after a few deaths", { ccx, cy - 32.f }, .4f, false, 170);
                }
                for (int i = 0; i < 3 && i < static_cast<int>(sorted.size()); i++) {
                    auto const& s = sorted[i];
                    float y = cy - 20.f - i * 26.f;
                    std::snprintf(buf, sizeof buf, "#%d   %d-%d%%   %d deaths", i + 1, s.start, s.end, s.deaths);
                    addLabel(buf, { ccx - 165.f, y }, .4f, true);
                    addBtn("Train", 70, menu_selector(CoachPauseLayer::onTrainSection), { ccx + 125.f, y }, i, "GJ_button_01.png", .5f);
                }
                break;
            }
            case 1: { // ---------------- TRAIN ----------------
                auto const& t = coach::train;
                addBtn("Auto Train", 100, menu_selector(CoachPauseLayer::onAuto), { ccx - 110.f, cy + 64.f }, 0, "GJ_button_01.png", .5f);
                addBtn("Manual", 90, menu_selector(CoachPauseLayer::onManual), { ccx - 5.f, cy + 64.f }, 0, "GJ_button_01.png", .5f);
                addBtn("Stop", 70, menu_selector(CoachPauseLayer::onStop), { ccx + 85.f, cy + 64.f }, 0, "GJ_button_06.png", .5f);

                addStepper(7, cy + 36.f, ccx - 165.f, ccx + 60.f);
                addStepper(8, cy + 12.f, ccx - 165.f, ccx + 60.f);

                if (t.active && t.cur < static_cast<int>(t.sections.size())) {
                    auto const& sec = t.sections[t.cur];
                    std::snprintf(buf, sizeof buf, "TRAINING  -  section %d/%d (%d-%d%%)",
                        t.cur + 1, static_cast<int>(t.sections.size()), sec.start, sec.end);
                    addLabel(buf, { ccx - 165.f, cy - 16.f }, .38f, true);
                    std::snprintf(buf, sizeof buf, "Speed %d%%   Streak %d/%d   Mastery %d/%d",
                        static_cast<int>(std::round(t.speed * 100.f)), t.streak, coach::cfgNeed,
                        t.mastery, coach::cfgMastery);
                    addLabel(buf, { ccx - 165.f, cy - 33.f }, .38f, true);
                    std::snprintf(buf, sizeof buf, "Section attempts: %d   passed: %d", t.attempts, t.passes);
                    addLabel(buf, { ccx - 165.f, cy - 50.f }, .38f, true);
                } else {
                    addLabel("Status: idle", { ccx - 165.f, cy - 16.f }, .38f, true);
                    auto secs = coach::detect(d, coach::cfgMaxSections, coach::cfgLead, coach::cfgTail);
                    std::snprintf(buf, sizeof buf, "Detected sections: %d (see Heat tab)", static_cast<int>(secs.size()));
                    addLabel(buf, { ccx - 165.f, cy - 33.f }, .38f, true);
                    addLabel("Resume the game after starting a training", { ccx - 165.f, cy - 50.f }, .32f, true, 170);
                }
                std::snprintf(buf, sizeof buf, "Lifetime: %d/%d passes   Sections mastered: %d",
                    d.trainPasses, d.trainTries, d.mastered);
                addLabel(buf, { ccx - 165.f, cy - 72.f }, .34f, true, 200);
                break;
            }
            case 2: { // ---------------- STATS ----------------
                int total = d.totalDeaths();
                float y = cy + 76.f;
                float step = 17.f;
                auto line = [&](std::string const& text) {
                    addLabel(text, { ccx - 165.f, y }, .38f, true);
                    y -= step;
                };

                std::string nm = coach::levelName.size() > 26 ? coach::levelName.substr(0, 25) + "." : coach::levelName;
                line("Level: " + nm);

                std::snprintf(buf, sizeof buf, "Deaths: %d   Completions: %d   This session: %d",
                    total, d.completions, coach::sessionDeaths);
                line(buf);

                float avg = 0.f;
                int peak = 0;
                for (int i = 0; i < 100; i++) {
                    avg += d.heat[i] * (i + .5f);
                    if (d.heat[i] > d.heat[peak]) peak = i;
                }
                if (total > 0) avg /= total;
                std::snprintf(buf, sizeof buf, "Best: %d%%   Average death: %.0f%%", d.best, avg);
                line(buf);

                if (total > 0) {
                    std::snprintf(buf, sizeof buf, "Deadliest spot: %d%% (%d deaths)", peak, d.heat[peak]);
                    line(buf);

                    int q[4] = { 0, 0, 0, 0 };
                    for (int i = 0; i < 100; i++) q[i / 25] += d.heat[i];
                    std::snprintf(buf, sizeof buf, "Deaths by quarter: %d%% / %d%% / %d%% / %d%%",
                        q[0] * 100 / total, q[1] * 100 / total, q[2] * 100 / total, q[3] * 100 / total);
                    line(buf);

                    int safe = 0;
                    for (int i = 0; i < 100; i++) if (d.heat[i] == 0) safe++;
                    std::snprintf(buf, sizeof buf, "Safe zones: %d%% of the level", safe);
                    line(buf);
                } else {
                    line("Die a few times to unlock more stats");
                }

                std::snprintf(buf, sizeof buf, "Training: %d/%d passes   Mastered: %d", d.trainPasses, d.trainTries, d.mastered);
                line(buf);
                std::snprintf(buf, sizeof buf, "Death markers saved: %d", static_cast<int>(d.marks.size()));
                line(buf);
                break;
            }
            case 3: { // ---------------- TUNING ----------------
                for (int i = 0; i < 7; i++) addStepper(i, cy + 70.f - i * 24.f, ccx - 165.f, ccx + 60.f);
                break;
            }
            case 4: { // ---------------- OPTIONS ----------------
                addToggle(1, "Heat strip in-game", cy + 74.f);
                addToggle(2, "Training text in-game", cy + 52.f);
                addToggle(3, "Death markers in level", cy + 30.f);
                addToggle(4, "Count practice deaths", cy + 8.f);
                addToggle(5, "Auto-enable practice", cy - 14.f);
                addToggle(6, "Slow down after failures", cy - 36.f);

                addBtn(m_fields->confirm == 1 ? "Sure? Tap" : "Reset level", 130, menu_selector(CoachPauseLayer::onResetLevel),
                       { ccx - 75.f, cy - 76.f }, 0, "GJ_button_06.png", .5f);
                addBtn(m_fields->confirm == 2 ? "Sure? Tap" : "Reset all", 120, menu_selector(CoachPauseLayer::onResetAll),
                       { ccx + 75.f, cy - 76.f }, 0, "GJ_button_06.png", .5f);
                break;
            }
        }

        this->addChild(panel);
    }

    void rebuild(float) { this->buildPanel(); }

    void doClose(float) {
        if (auto p = this->getChildByID("panel"_spr)) p->removeFromParent();
        this->setMenusEnabled(true);
    }

    void queueRebuild() {
        this->scheduleOnce(schedule_selector(CoachPauseLayer::rebuild), 0.f);
    }

    void onOpen(CCObject*) {
        if (this->getChildByID("panel"_spr)) return;
        m_fields->confirm = 0;
        this->setMenusEnabled(false);
        this->buildPanel();
    }

    void onClose(CCObject*) {
        this->scheduleOnce(schedule_selector(CoachPauseLayer::doClose), 0.f);
    }

    void onTab(CCObject* sender) {
        coach::tab = static_cast<CCNode*>(sender)->getTag();
        m_fields->confirm = 0;
        coach::saveCfg();
        this->queueRebuild();
    }

    void onToggle(CCObject* sender) {
        int tag = static_cast<CCNode*>(sender)->getTag();
        bool value = !static_cast<CCMenuItemToggler*>(sender)->isToggled();
        if (auto p = coach::optPtr(tag)) *p = value;
        coach::heatDirty = true;
        coach::saveCfg();
    }

    void onStep(CCObject* sender) {
        int tag = static_cast<CCNode*>(sender)->getTag();
        int id = tag / 2;
        int dir = (tag % 2) ? 1 : -1;
        if (id < 0 || id >= coach::settingCount) return;

        auto& s = coach::settings[id];
        *s.value = std::clamp(*s.value + dir * s.step, s.min, s.max);

        // el rango manual debe tener inicio < fin
        if (id == 7 && coach::cfgManualEnd <= coach::cfgManualStart)
            coach::cfgManualEnd = std::min(99, coach::cfgManualStart + 1);
        if (id == 8 && coach::cfgManualStart >= coach::cfgManualEnd)
            coach::cfgManualStart = std::max(0, coach::cfgManualEnd - 1);

        coach::saveCfg();
        this->queueRebuild();
    }

    void onAuto(CCObject*) {
        auto secs = coach::detect(coach::data, coach::cfgMaxSections, coach::cfgLead, coach::cfgTail);
        if (coach::startTraining(secs)) this->scheduleOnce(schedule_selector(CoachPauseLayer::doClose), 0.f);
    }

    void onManual(CCObject*) {
        if (coach::startTraining({ coach::manualSection() }))
            this->scheduleOnce(schedule_selector(CoachPauseLayer::doClose), 0.f);
    }

    void onTrainSection(CCObject* sender) {
        auto sorted = coach::detectByDeaths(coach::data);
        size_t idx = static_cast<size_t>(static_cast<CCNode*>(sender)->getTag());
        if (idx >= sorted.size()) return;
        if (coach::startTraining({ sorted[idx] }))
            this->scheduleOnce(schedule_selector(CoachPauseLayer::doClose), 0.f);
    }

    void onStop(CCObject*) {
        if (!coach::train.active) return coach::notify("No training running", NotificationIcon::Warning);
        coach::stopTraining();
        coach::notify("Training stopped");
        this->queueRebuild();
    }

    void onResetLevel(CCObject*) {
        if (m_fields->confirm != 1) {
            m_fields->confirm = 1;
        } else {
            coach::resetLevelData();
            coach::notify("Level data reset");
            m_fields->confirm = 0;
        }
        this->queueRebuild();
    }

    void onResetAll(CCObject*) {
        if (m_fields->confirm != 2) {
            m_fields->confirm = 2;
        } else {
            coach::resetAllData();
            coach::notify("All Coach data reset");
            m_fields->confirm = 0;
        }
        this->queueRebuild();
    }
};
