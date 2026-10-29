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
#include <cstdint>

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

        // Initial grid load. We do NOT rely on OnAreaChange: the event fires
        // synchronously when the worker thread detects AreaChangeCounter++,
        // but at that instant the new walkable grid may not have been parsed
        // yet — Subscribe would race against the parser. Instead, DrawUI
        // polls per-frame and swaps when the grid pointer changes (cheap:
        // GetWalkableGrid just bumps a shared_ptr refcount and returns the
        // raw pointer).
        m_walkable = ctx()->Terrain.GetWalkableGrid();

        ctx()->Log.Info("Radar v6 enabled");
    }

    void OnDisable() override {
        m_walkable.Reset();
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
        PluginSDK::Snapshot snap = ctx()->Game.GetSnapshot();
        if (!snap.LargeMap.IsVisible) return;

        // Poll the walkable grid pointer each frame: when the host re-parses
        // it on area change, the underlying buffer pointer flips and we swap
        // our cached handle. Cheap (one ABI call, one pointer compare) and
        // avoids the race where OnAreaChange fires before the new grid is
        // ready.
        auto current = ctx()->Terrain.GetWalkableGrid();
        if (current.Data() != m_walkable.Data()) {
            m_walkable = std::move(current);
        }

        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        if (!dl) return;

        if (m_s.DrawWalkable) DrawWalkable(dl);
        DrawEntities(dl, snap);
        DrawPlayer(dl, snap.Player);
    }

    void SaveSettings() override {
        m_s.Save(DirectoryPath());
    }

private:
    void DrawEntities(ImDrawList* dl, const PluginSDK::Snapshot& snap) {
        for (const auto& e : snap.Entities) {
            if (!e.IsValid) continue;
            switch (e.EntityType) {
                case PluginSDK::EntityType::Monster:
                    if (m_s.DrawMonsters && e.CurrentHP > 0) {
                        int idx = std::clamp(e.Rarity, 0, 3);
                        DrawDot(dl, e, m_s.RarityColor[idx]);
                    }
                    break;
                case PluginSDK::EntityType::NPC:
                    if (m_s.DrawNpcs) DrawDot(dl, e, m_s.NpcColor);
                    break;
                case PluginSDK::EntityType::Chest:
                    if (m_s.DrawChests) {
                        DrawDot(dl, e,
                                e.IsChestOpened ? m_s.ChestOpenedColor
                                                : m_s.ChestColor);
                    }
                    break;
                case PluginSDK::EntityType::AreaTransition:
                    if (m_s.DrawTransitions) DrawDot(dl, e, m_s.TransitionColor);
                    break;
                default:
                    break;
            }
        }
    }

    void DrawPlayer(ImDrawList* dl, const PluginSDK::Entity& p) {
        if (!p.IsValid) return;
        DrawDot(dl, p, m_s.PlayerColor, 6.f);
    }

    void DrawDot(ImDrawList* dl, const PluginSDK::Entity& e,
                 const ImVec4& color, float radius = 4.f) {
        float sx = 0.f, sy = 0.f;
        if (!ctx()->Render.GridToLargeMap(
                e.GridPositionX, e.GridPositionY,
                e.TerrainHeight, sx, sy)) return;
        dl->AddCircleFilled(ImVec2(sx, sy), radius,
                            ImGui::ColorConvertFloat4ToU32(color));
    }

    void DrawWalkable(ImDrawList* dl) {
        if (!m_walkable.Valid()) return;
        const uint8_t* grid = m_walkable.Data();
        const int w = m_walkable.Width();
        const int h = m_walkable.Height();
        const size_t sizeBytes = m_walkable.SizeBytes();
        if (!grid || w <= 0 || h <= 0 || sizeBytes == 0) return;

        // POE2 packs the walkable grid as 4-bit-per-tile (even column = low
        // nibble, odd column = high nibble). Reference: ComputeWalkablePixelsImpl
        // in POEFixer/radar/radar/Radar.cpp. Non-zero cell == walkable.
        const int rowStrideBytes = w / 2;
        auto isOpen = [&](int gx, int gy) -> bool {
            if (gx < 0 || gy < 0 || gx >= w || gy >= h) return false;
            const size_t byteIdx = static_cast<size_t>(gy)
                                 * static_cast<size_t>(rowStrideBytes)
                                 + static_cast<size_t>(gx / 2);
            // Belt + braces: even after the host-side atomic-snapshot fix,
            // bound the read by the handle's reported buffer size. Prevents
            // an AV in case dims drift out of sync with the underlying buffer
            // (which used to crash the host past the SEH wrap when the AV
            // landed in another process's mapped-region tail).
            if (byteIdx >= sizeBytes) return false;
            const uint8_t cell = (gx & 1) ? (grid[byteIdx] >> 4)
                                          : (grid[byteIdx] & 0x0F);
            return cell != 0;
        };

        const ImU32 color = ImGui::ColorConvertFloat4ToU32(m_s.WalkableColor);
        // Decimate by 4x in both axes — enough to demonstrate the walkable
        // grid renders without scanning every tile per frame.
        for (int gy = 0; gy < h; gy += 4) {
            for (int gx = 0; gx < w; gx += 4) {
                if (!isOpen(gx, gy)) continue;
                const float worldZ = ctx()->Terrain.GetTerrainHeight(gx, gy);
                float sx = 0.f, sy = 0.f;
                if (!ctx()->Render.GridToLargeMap(
                        static_cast<float>(gx), static_cast<float>(gy),
                        worldZ, sx, sy)) continue;
                dl->AddRectFilled(ImVec2(sx, sy), ImVec2(sx + 1.f, sy + 1.f),
                                  color);
            }
        }
    }

    RadarSettings                   m_s;
    PluginSDK::WalkableGridHandle   m_walkable;
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
    "1.0.3",
    "POEFixer",
    "Draws nearby monsters, NPCs, chests, area transitions and the walkable map on the in-game large map.")
