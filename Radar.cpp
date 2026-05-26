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

#include "sdk/PluginSDK.h"

#include "RadarSettings.h"

#include <imgui.h>

#include <algorithm>
#include <cstdint>

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
        ImGui::Checkbox("Draw walkable map", &m_s.DrawWalkable);
        ImGui::Checkbox("Monsters",          &m_s.DrawMonsters);
        ImGui::Checkbox("NPCs",              &m_s.DrawNpcs);
        ImGui::Checkbox("Chests",            &m_s.DrawChests);
        ImGui::Checkbox("Area transitions",  &m_s.DrawTransitions);
        ImGui::Separator();
        ImGui::ColorEdit4("Normal monster",  &m_s.RarityColor[0].x);
        ImGui::ColorEdit4("Magic monster",   &m_s.RarityColor[1].x);
        ImGui::ColorEdit4("Rare monster",    &m_s.RarityColor[2].x);
        ImGui::ColorEdit4("Unique monster",  &m_s.RarityColor[3].x);
        ImGui::ColorEdit4("NPC",             &m_s.NpcColor.x);
        ImGui::ColorEdit4("Chest",           &m_s.ChestColor.x);
        ImGui::ColorEdit4("Chest (opened)",  &m_s.ChestOpenedColor.x);
        ImGui::ColorEdit4("Transition",      &m_s.TransitionColor.x);
        ImGui::ColorEdit4("Player",          &m_s.PlayerColor.x);
        ImGui::ColorEdit4("Walkable tile",   &m_s.WalkableColor.x);
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
