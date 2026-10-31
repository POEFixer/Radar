// File: Plugins/Radar/src/Radar.cpp
//
// Radar test plugin (SDK v6).
//
// Demonstrates that the v6 SDK is sufficient for a real radar overlay:
//   - No direct memory reads (everything goes through ctx()->Service.Method())
//   - No hardcoded offsets
//   - Renders Monsters / NPCs / Chests / AreaTransitions / Player + walkable
//     map onto the large-map overlay via ctx()->Render.GridToLargeMap()
//
// Settings persist via RadarSettings::Save/Load (see RadarSettings.h).
//
// PLUGIN_EXPORTS is set in the vcxproj preprocessor definitions, which makes
// PluginSDK.h emit the PluginSDK_AttachHost export and define PLUGIN_API as
// __declspec(dllexport).
//
// Texts follow the program language (PluginI18n.h): lang/<code>.json next to the
// DLL, English compiled in through lang_en.generated.h (python
// tools/plugin_i18n/gen.py Plugins/Radar after editing lang/en.json).

#include "../../../POEFixer/plugin_sdk/PluginSDK.h"
#include "lang_en.generated.h"   // once per DLL, after PluginSDK.h

#include "RadarSettings.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace I18n = PluginSDK::I18n;

class RadarPlugin : public PluginSDK::Plugin {
public:
    const char* GetName() const override { return "Radar"; }
    bool        WantsOverlay() const override { return true; }

    void OnEnable(bool /*isGameAttached*/) override {
        if (ctx()->ImGuiContext) {
            ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx()->ImGuiContext));
        }
        m_s.Load(DirectoryPath());

        // No grid load here and no OnAreaChange: the event fires when the worker
        // sees the area change, before the new walkable grid may be parsed.
        // DrawUI polls the grid pointer instead and rebuilds its point cache
        // when it changes.

        ctx()->Log.Info("Radar v6 enabled");
    }

    void OnDisable() override {
        m_walkable.Reset();
        for (auto& level : m_levels) std::vector<WalkPoint>().swap(level);
        std::vector<ImVec2>().swap(m_dots);
        std::vector<PluginSDK::Entity>().swap(m_entities);
        m_heightFactor = 0.f;
        ctx()->Log.Info("Radar v6 disabled");
    }

    void DrawSettings() override {
        // A separate menu window draws the settings in its own ImGui context.
        if (ctx()->ImGuiContext) {
            ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx()->ImGuiContext));
        }
        // TrId: "<text>###<key>", so each widget keeps its ImGui id in every language.
        ImGui::Checkbox(I18n::TrId("settings_draw_walkable"),   &m_s.DrawWalkable);
        ImGui::Checkbox(I18n::TrId("settings_draw_monsters"),   &m_s.DrawMonsters);
        ImGui::Checkbox(I18n::TrId("settings_draw_npcs"),       &m_s.DrawNpcs);
        ImGui::Checkbox(I18n::TrId("settings_draw_chests"),     &m_s.DrawChests);
        ImGui::Checkbox(I18n::TrId("settings_draw_transitions"), &m_s.DrawTransitions);
        ImGui::Separator();
        // Labels are built every frame (they follow the language), and the inputs give up width
        // so the longest translated label still fits beside them in a narrow tab.
        const char* const colorLabels[] = {
            I18n::TrId("color_normal_monster"),
            I18n::TrId("color_magic_monster"),
            I18n::TrId("color_rare_monster"),
            I18n::TrId("color_unique_monster"),
            I18n::TrId("color_npc"),
            I18n::TrId("color_chest"),
            I18n::TrId("color_chest_opened"),
            I18n::TrId("color_transition"),
            I18n::TrId("color_player"),
            I18n::TrId("color_walkable_tile"),
        };
        ImVec4* const colors[] = {
            &m_s.RarityColor[0], &m_s.RarityColor[1], &m_s.RarityColor[2], &m_s.RarityColor[3],
            &m_s.NpcColor, &m_s.ChestColor, &m_s.ChestOpenedColor, &m_s.TransitionColor,
            &m_s.PlayerColor, &m_s.WalkableColor,
        };
        static_assert(sizeof(colorLabels) / sizeof(colorLabels[0]) == sizeof(colors) / sizeof(colors[0]),
                      "one label per color");
        float labelWidth = 0.0f;
        for (const char* label : colorLabels) {
            labelWidth = (std::max)(labelWidth, ImGui::CalcTextSize(label, nullptr, true).x);
        }
        const float fitWidth = ImGui::GetContentRegionAvail().x - labelWidth - ImGui::GetStyle().ItemInnerSpacing.x;
        const float inputsWidth = (std::max)(ImGui::GetFrameHeight() * 6.0f, (std::min)(ImGui::CalcItemWidth(), fitWidth));
        ImGui::PushItemWidth(inputsWidth);
        for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); ++i) {
            ImGui::ColorEdit4(colorLabels[i], &colors[i]->x);
        }
        ImGui::PopItemWidth();
    }

    void DrawUI() override {
        if (!ctx()->Game.IsInGame()) return;
        if (ctx()->ImGuiContext) {
            ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx()->ImGuiContext));
        }
        // One transform per frame, checked before the entities are fetched: that is
        // wasted work on the frames the large map is closed.
        const PluginSDK::MapTransform t = ctx()->Render.GetLargeMapTransform();
        if (!t.IsVisible) return;

        const PluginSDK::Entity player = CollectEntities();
        // The host projection takes heights RELATIVE to the player (the built-in
        // radar passes entityZ - playerZ); absolute heights shift every point.
        const float playerZ = player.IsValid ? player.TerrainHeight : 0.f;

        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        if (!dl) return;

        if (m_s.DrawWalkable) {
            RefreshWalkableCache();
            DrawWalkable(dl, t, playerZ);
        } else if (m_walkable.Valid()) {
            // Do not keep an old area's grid pinned while the layer is off.
            m_walkable.Reset();
            for (auto& level : m_levels) level.clear();
        }
        DrawEntities(dl, playerZ);
        DrawPlayer(dl, player, playerZ);
    }

    void SaveSettings() override {
        m_s.Save(DirectoryPath());
    }

private:
    // Every kStep-th cell on each axis is a walkable dot.
    static constexpr int   kStep = 4;
    // Neighbouring walkable dots are at least this far apart on screen: the
    // lattice is thinned (by powers of two) until they are.
    static constexpr float kMinDotSpacingPx = 12.f;
    // At most this many walkable dots per frame. A zoomed-out large map has
    // hundreds of thousands of on-screen samples; drawing them all costs the whole
    // program its frame rate even when DrawUI itself is fast (the vertices are
    // rebuilt and uploaded every frame).
    static constexpr size_t kMaxDots = 20000;
    // Lattice levels: a point is in level L when ix and iy are both multiples of
    // 2^L but not both of 2^(L+1) (the last level keeps the rest), so levels >= L
    // form an even lattice with step kStep * 2^L.
    static constexpr int kLevels = 6;

    struct WalkPoint {
        uint16_t ix, iy;   // grid cell = (ix * kStep, iy * kStep)
        float    z;        // terrain height of that cell
    };

    static int LevelOf(unsigned ix, unsigned iy) {
        const unsigned v = ix | iy;
        int level = 0;
        while (level < kLevels - 1 && (v & (1u << level)) == 0) ++level;
        return level;
    }

    // Fetches the player and the entity types the radar draws into m_entities.
    // GetSnapshot() without a filter copies EVERY entity with its strings (about
    // 10 us each): a map holds 6000+ entities, mostly ones the radar never draws,
    // and that alone took 55-85 ms per frame (measured). The type filter skips the
    // other entities before their strings are read.
    PluginSDK::Entity CollectEntities() {
        const PluginSDK::EntityType types[] = {
            PluginSDK::EntityType::Monster, PluginSDK::EntityType::NPC,
            PluginSDK::EntityType::Chest,   PluginSDK::EntityType::AreaTransition,
        };
        const bool wanted[] = { m_s.DrawMonsters, m_s.DrawNpcs, m_s.DrawChests, m_s.DrawTransitions };
        m_entities.clear();
        PluginSDK::Entity player;
        bool havePlayer = false;
        for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
            if (!wanted[i]) continue;
            PluginSDK::Snapshot part = ctx()->Game.GetSnapshot(types[i]);
            if (!havePlayer) {
                player = std::move(part.Player);
                havePlayer = true;
            }
            for (auto& e : part.Entities) m_entities.push_back(std::move(e));
        }
        if (!havePlayer) player = ctx()->Entities.GetPlayer();
        return player;
    }

    void DrawEntities(ImDrawList* dl, float playerZ) {
        for (const auto& e : m_entities) {
            if (!e.IsValid) continue;
            switch (e.EntityType) {
                case PluginSDK::EntityType::Monster:
                    if (m_s.DrawMonsters && e.CurrentHP > 0) {
                        int idx = std::clamp(e.Rarity, 0, 3);
                        DrawDot(dl, e, playerZ, m_s.RarityColor[idx]);
                    }
                    break;
                case PluginSDK::EntityType::NPC:
                    if (m_s.DrawNpcs) DrawDot(dl, e, playerZ, m_s.NpcColor);
                    break;
                case PluginSDK::EntityType::Chest:
                    if (m_s.DrawChests) {
                        DrawDot(dl, e, playerZ,
                                e.IsChestOpened ? m_s.ChestOpenedColor
                                                : m_s.ChestColor);
                    }
                    break;
                case PluginSDK::EntityType::AreaTransition:
                    if (m_s.DrawTransitions) DrawDot(dl, e, playerZ, m_s.TransitionColor);
                    break;
                default:
                    break;
            }
        }
    }

    void DrawPlayer(ImDrawList* dl, const PluginSDK::Entity& p, float playerZ) {
        if (!p.IsValid) return;
        DrawDot(dl, p, playerZ, m_s.PlayerColor, 6.f);
    }

    void DrawDot(ImDrawList* dl, const PluginSDK::Entity& e, float playerZ,
                 const ImVec4& color, float radius = 4.f) {
        float sx = 0.f, sy = 0.f;
        if (!ctx()->Render.GridToLargeMap(
                e.GridPositionX, e.GridPositionY,
                e.TerrainHeight - playerZ, sx, sy)) return;
        dl->AddCircleFilled(ImVec2(sx, sy), radius,
                            ImGui::ColorConvertFloat4ToU32(color));
    }

    // The walkable dots are sampled ONCE per grid, not per frame. Two host calls
    // per sampled cell per frame (height + projection) made DrawUI scale with the
    // whole area and pass the host's 50 ms watchdog budget on large areas.
    // The host re-parses the grid on area change and the buffer pointer flips;
    // the pinned handle keeps the old address from being reused.
    void RefreshWalkableCache() {
        auto current = ctx()->Terrain.GetWalkableGrid();
        if (current.Data() == m_walkable.Data()) return;
        PluginSDK::HeightGridHandle heights = ctx()->Terrain.GetHeightGrid();
        // A publish between the two calls would pair this grid with another
        // area's heights: try again next frame.
        if (ctx()->Terrain.GetWalkableGrid().Data() != current.Data()) return;
        m_walkable = std::move(current);
        BuildWalkPoints(heights);
    }

    void BuildWalkPoints(const PluginSDK::HeightGridHandle& heights) {
        for (auto& level : m_levels) level.clear();
        if (!m_walkable.Valid()) return;
        const uint8_t* grid = m_walkable.Data();
        const int w = m_walkable.Width();
        const int h = m_walkable.Height();
        const size_t sizeBytes = m_walkable.SizeBytes();
        if (!grid || w <= 0 || h <= 0 || sizeBytes == 0) return;
        if (w / kStep > 0xFFFF || h / kStep > 0xFFFF) return;

        const float* hd = heights.Valid() ? heights.Data() : nullptr;
        const int hw = heights.Width();
        const int hh = heights.Height();

        // POE2 packs the walkable grid as 4-bit-per-tile (even column = low
        // nibble, odd column = high nibble). Reference: ComputeWalkablePixelsImpl
        // in POEFixer/radar/radar/Radar.cpp. Non-zero cell == walkable.
        const size_t rowStrideBytes = static_cast<size_t>(w / 2);
        for (int gy = 0; gy < h; gy += kStep) {
            for (int gx = 0; gx < w; gx += kStep) {
                // Bounded by the handle's buffer size in case dims and buffer
                // ever drift apart.
                const size_t byteIdx = static_cast<size_t>(gy) * rowStrideBytes
                                     + static_cast<size_t>(gx / 2);
                if (byteIdx >= sizeBytes) continue;
                const uint8_t cell = (gx & 1) ? (grid[byteIdx] >> 4)
                                              : (grid[byteIdx] & 0x0F);
                if (cell == 0) continue;
                // The height grid has its own dimensions; outside it the height
                // is 0, as Terrain.GetTerrainHeight answers.
                float z = 0.f;
                if (hd && gx < hw && gy < hh)
                    z = hd[static_cast<size_t>(gy) * static_cast<size_t>(hw) + static_cast<size_t>(gx)];
                const unsigned ix = static_cast<unsigned>(gx / kStep);
                const unsigned iy = static_cast<unsigned>(gy / kStep);
                m_levels[LevelOf(ix, iy)].push_back(
                    { static_cast<uint16_t>(ix), static_cast<uint16_t>(iy), z });
            }
        }
    }

    // The projection's height factor is the profile's world-to-grid constant
    // (about 0.092), NOT Terrain.GetWorldToGridConvertor() (world units per cell,
    // about 10.9). Read it back from the host projection: at the player's own
    // cell only the height term moves the point. Both probes match the transform
    // exactly when they saw the same host frame; otherwise keep the last value.
    float HeightFactor(const PluginSDK::MapTransform& t) {
        constexpr float kProbeZ = 1000.f;
        float ax = 0.f, ay = 0.f, bx = 0.f, by = 0.f;
        if (ctx()->Render.GridToLargeMap(t.PlayerGridX, t.PlayerGridY, 0.f, ax, ay) &&
            ctx()->Render.GridToLargeMap(t.PlayerGridX, t.PlayerGridY, kProbeZ, bx, by) &&
            ax == t.CenterX && ay == t.CenterY && bx == t.CenterX) {
            const float f = (by - ay) / (kProbeZ * t.ScaleY);
            if (std::isfinite(f)) m_heightFactor = f;
        }
        if (m_heightFactor == 0.f) {
            const float conv = ctx()->Terrain.GetWorldToGridConvertor();
            if (conv > 0.f) m_heightFactor = 1.f / conv;
        }
        return m_heightFactor;
    }

    // Projects the cached points locally with the frame's transform (the same
    // math as Render.GridToLargeMap) and draws only the on-screen ones.
    void DrawWalkable(ImDrawList* dl, const PluginSDK::MapTransform& t, float playerZ) {
        if (!std::isfinite(t.ScaleX) || !std::isfinite(t.ScaleY) ||
            t.ScaleX == 0.f || t.ScaleY == 0.f) return;
        const float k = HeightFactor(t);
        const float spacing = kStep * std::hypot(t.ScaleX, t.ScaleY);
        const ImVec2 clipMin = dl->GetClipRectMin();
        const ImVec2 clipMax = dl->GetClipRectMax();

        // Coarse to fine: a finer level is added only while the dots stay at least
        // kMinDotSpacingPx apart and the frame stays within kMaxDots.
        m_dots.clear();
        for (int level = kLevels - 1; level >= 0; --level) {
            if (spacing * static_cast<float>(1u << level) < kMinDotSpacingPx) break;
            const size_t before = m_dots.size();
            for (const WalkPoint& p : m_levels[level]) {
                const float dX = static_cast<float>(p.ix * kStep) - t.PlayerGridX;
                const float dY = static_cast<float>(p.iy * kStep) - t.PlayerGridY;
                const float sx = t.CenterX + t.ScaleX * (dX - dY);
                const float sy = t.CenterY + t.ScaleY * ((p.z - playerZ) * k - (dX + dY));
                if (sx < clipMin.x - 2.f || sx > clipMax.x || sy < clipMin.y - 2.f || sy > clipMax.y)
                    continue;
                m_dots.push_back(ImVec2(sx, sy));
            }
            if (m_dots.size() > kMaxDots) {
                m_dots.resize(before);
                break;
            }
        }

        const float size = 2.f;
        const ImU32 color = ImGui::ColorConvertFloat4ToU32(m_s.WalkableColor);
        for (const ImVec2& d : m_dots) {
            // StrokeLegacy keeps the 4-vertex rectangle: since ImGui 1.93 a rect at
            // fractional coordinates otherwise takes the anti-aliased fill (8
            // vertices, 30 indices), about three times the cost.
            dl->AddRectFilled(d, ImVec2(d.x + size, d.y + size), color,
                              0.0f, ImDrawFlags_StrokeLegacy);
        }
    }

    RadarSettings                   m_s;
    PluginSDK::WalkableGridHandle   m_walkable;
    std::vector<WalkPoint>          m_levels[kLevels];
    std::vector<ImVec2>             m_dots;   // this frame's walkable dots
    std::vector<PluginSDK::Entity>  m_entities;   // this frame's drawn entity types
    float                           m_heightFactor = 0.f;
};

// ----------------------------------------------------------------------------
// v6 factory exports
// ----------------------------------------------------------------------------

extern "C" PLUGIN_API PluginSDK::Plugin* CreatePlugin() {
    return new RadarPlugin();
}

extern "C" PLUGIN_API void DestroyPlugin(PluginSDK::Plugin* p) {
    delete p;
}

// Plugin info: the host checks it before CreatePlugin; the publish tool reads it.
POEFIXER_PLUGIN_INFO(
    "Radar",
    "Radar",
    "1.0.6",
    "POEFixer",
    "Draws nearby monsters, NPCs, chests, area transitions and the walkable map on the in-game large map.")
