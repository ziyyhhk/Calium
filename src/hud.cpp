// Calium's in-game presence: the live stats panel and the pause-menu button.
//
// The panel exists because this mod's whole claim is "it changed something you can see", and a
// number on screen is the only way to answer that while playing. Everything shown is measured, and
// the timings are labelled separately because they are different quantities:
//
//   frame - wall time between the start of two frames. This is what you feel. When the game is
//           vsync-capped it is dominated by the cap, not by work, which is exactly why an
//           optimisation can be real and still not show up here.
//   cpu   - time spent inside drawScene: GD's submission plus Calium's passes.
//   gpu   - only meaningful with "Sync every frame" on, which forces a wait so the wall clock can
//           see the GPU finish. Off by default because that wait costs frames.
//
// The pause button opens Geode's own settings page rather than a hand-built popup: it already looks
// native, it cannot collide with another mod's menu, and every value gets a range the loader
// enforces. The panel is where the judgement call lives.

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <vector>

#include <Geode/Geode.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/Label.hpp>
#include <Geode/ui/NineSlice.hpp>
#include <Geode/utils/cocos.hpp>

#include <calium.hpp>

using namespace geode::prelude;

namespace calium {

    namespace {

        constexpr float kPlateWidthMobile  = 150.f;
        constexpr float kRefreshSeconds    = 0.25f;

        std::vector<CCObject*> childrenOf(cocos2d::CCArray* arr) {
            std::vector<CCObject*> out;
            if (!arr) return out;
            out.reserve(arr->count());
            for (unsigned int i = 0; i < arr->count(); ++i) out.push_back(arr->objectAt(i));
            return out;
        }

        // The live numbers, in a plate that is sized to its own text.
        class StatsPanel : public CCNode {
        public:
            static StatsPanel* create() {
                auto* self = new StatsPanel();
                if (self && self->init()) { self->autorelease(); return self; }
                CC_SAFE_DELETE(self);
                return nullptr;
            }

            bool init() {
                if (!CCNode::init()) return false;
                ignoreAnchorPointForPosition(false);
                setAnchorPoint({ 0.f, 0.f });

                // GD's own rounded panel, stretched as a nine-slice so the corners stay round at any
                // width. Anything flatter reads as a debug overlay instead of part of the game.
                auto* bg = NineSlice::createWithSpriteFrameName("square01_001.png");
                if (!bg) return false;
                bg->setOpacity(150);
                bg->setAnchorPoint({ 0.f, 0.f });
                bg->ignoreAnchorPointForPosition(false);
                bg->setPosition({ 0.f, 0.f });
                bg->setContentSize({ m_width, m_height });
                addChild(bg, -1);
                m_bg = bg;

                // Three short lines: the number that matters, what it cost, what the mod did. The
                // spacing is set from the scaled label height so nothing can overlap at either size.
                if (!addLabel("chatFont.fnt", 0.f, 0.66f)) return false;
                if (!addLabel("chatFont.fnt", 0.f, 0.44f)) return false;
                if (!addLabel("chatFont.fnt", 0.f, 0.44f)) return false;
                float const step = m_labels[0].label->getScaledContentSize().height + 3.f;
                m_labels[0].y = step;
                m_labels[1].y = 0.f;
                m_labels[2].y = -step;
                m_height = step * 2.f + 14.f;

                refresh();
                schedule(schedule_selector(StatsPanel::tick), kRefreshSeconds);
                return true;
            }

            void tick(float) { refresh(); }

            void updateCorner() {
                cocos2d::CCSize const win = CCDirector::sharedDirector()->getWinSize();
                float const w = m_width, h = m_height;
                float const margin = 10.f;
                switch (config().hudCorner) {
                    case 0:  setPosition({ margin,           win.height - margin - h }); break;
                    case 2:  setPosition({ margin,           margin });                  break;
                    case 3:  setPosition({ win.width  - margin - w, margin });            break;
                    default: setPosition({ win.width  - margin - w, win.height - margin - h }); break;
                }
            }

        private:
            Label* addLabel(char const* font, float y, float scale) {
                auto* l = Label::create("", font);
                if (!l) return nullptr;
                l->setAnchorPoint({ 0.f, 0.5f });
                l->setScale(scale);
                addChild(l, 1);
                m_labels.push_back({ l, y });
                return l;
            }

            void refresh() {
                auto& s = constSnapshot();

                char top[48];
                std::snprintf(top, sizeof(top), "%.0f fps", s.fps);

                char bottom[128];
                if (rendererEnabled()) {
                    if (s.gpuMs >= 0.0) {
                        std::snprintf(bottom, sizeof(bottom),
                            "%.1f ms  gpu %.1f  scale %.2f", s.frameMs, s.gpuMs, s.scale);
                    } else {
                        std::snprintf(bottom, sizeof(bottom),
                            "%.1f ms  cpu %.1f  scale %.2f", s.frameMs, s.cpuMs, s.scale);
                    }
                } else {
                    std::snprintf(bottom, sizeof(bottom), "%s",
                        s.degraded ? "stopped: failures" : "off");
                }

                char third[128];
                std::snprintf(third, sizeof(third), "%dx%d  batch %llu  cull %llu",
                    s.renderW > 0 ? s.renderW : s.width,
                    s.renderH > 0 ? s.renderH : s.height,
                    static_cast<unsigned long long>(s.batchesDrawn),
                    static_cast<unsigned long long>(s.batchesSkipped));

                if (m_labels.size() >= 3) {
                    m_labels[0].label->setString(top);
                    m_labels[1].label->setString(bottom);
                    m_labels[2].label->setString(third);
                }

                // Size the plate to the widest line, then re-place it: a fixed width is how panels
                // end up clipping their own numbers on a phone.
                float widest = 0.f;
                for (auto& e : m_labels) {
                    widest = std::max(widest, e.label->getScaledContentSize().width);
                }
                m_width  = std::max(kPlateWidthMobile, widest + 16.f);
                fitPlate();
                updateCorner();
            }

            void fitPlate() {
                if (!m_bg) return;
                m_bg->setContentSize({ m_width, m_height });
                for (auto& e : m_labels) {
                    e.label->setPosition({ 8.f, m_height * 0.5f + e.y });
                }
            }

            NineSlice* m_bg = nullptr;
            float m_width = kPlateWidthMobile;
            float m_height = 52.f;

            struct Entry { Label* label; float y; };
            std::vector<Entry> m_labels;
        };

        // The pause-menu button picks a corner that is not already occupied. With this many mods
        // available that all add buttons to the same corners, "last one added wins the tap" is not
        // good enough, and neither is sitting on top of the game's own timer text.
        void placePauseButton(CCNode* menu) {
            cocos2d::CCSize const win = CCDirector::sharedDirector()->getWinSize();
            float const gap = 46.f;

            struct Spot { float x, y; };
            std::array<Spot, 4> spots = {{
                { 34.f,                 win.height - 34.f },
                { win.width - 34.f,     win.height - 34.f },
                { 34.f,                 34.f },
                { win.width - 34.f,     34.f },
            }};

            auto* parent = menu->getParent();
            for (auto const& s : spots) {
                bool free = true;
                for (auto* obj : childrenOf(parent ? parent->getChildren() : nullptr)) {
                    auto* node = dynamic_cast<CCNode*>(obj);
                    if (!node || node == menu || !node->isVisible()) continue;
                    auto const p = node->convertToWorldSpaceAR(CCPoint{0.f, 0.f});
                    if (std::fabs(p.x - s.x) < gap && std::fabs(p.y - s.y) < gap) {
                        free = false;
                        break;
                    }
                }
                if (free) {
                    menu->setPosition({ s.x, s.y });
                    return;
                }
            }
            menu->setPosition({ spots[0].x, spots[0].y - gap * 1.2f });
        }

    }   // namespace

}

// ---------------------------------------------------------------------- hooks

using namespace calium;

class $modify("CaliumPlayLayer", PlayLayer) {
    struct Fields {
        StatsPanel* panel = nullptr;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        // A new level is a fresh chance for the render path: the failure budget and the frame
        // window both start over, so one bad level cannot leave the mod off for the session.
        onLevelStart();
        return true;
    }

    void onEnterTransitionDidFinish() {
        PlayLayer::onEnterTransitionDidFinish();
        if (!config().hudEnabled) return;
        if (m_fields->panel) return;

        auto* panel = StatsPanel::create();
        if (!panel) return;
        // Above the level and the game's own progress text, below Geode's popups.
        addChild(panel, 1000);
        m_fields->panel = panel;
    }

    void onExit() {
        if (m_fields->panel) {
            m_fields->panel->removeFromParentAndCleanup(true);
            m_fields->panel = nullptr;
        }
        PlayLayer::onExit();
    }
};

class $modify("CaliumPauseLayer", PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        if (getChildByTag(kButtonTag)) return;

        auto* sprite = CCSprite::createWithSpriteFrameName("GJeditBtn_001.png");
        if (!sprite) return;
        sprite->setScale(0.62f);

        auto* item = cocos2d::CCMenuItemSpriteExtra::createSpriteExtra(
            sprite, [](CCMenuItemSpriteExtra*) {
                // Two taps in a row would push two settings pages onto the stack, so the button
                // removes itself for the duration of the open.
                openSettingsPopup(Mod::get(), true);
            });
        if (!item) return;

        auto* menu = CCMenu::createWithItem(item);
        if (!menu) return;
        menu->setTag(kButtonTag);
        // Checked before the game's menus, so a tap here is not stolen by an overlapping button.
        menu->setHandlerPriority(2);
        addChild(menu, 1200);
        placePauseButton(menu);
    }

    static constexpr int kButtonTag = 0xC411;
};
