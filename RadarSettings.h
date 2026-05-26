// File: Plugins/Radar/src/RadarSettings.h
//
// Settings POCO for the Radar test plugin (SDK v6).
//
// Persists to <PluginDirectory>/config/settings.json. The JSON serializer is
// hand-rolled (no nlohmann dependency) so the plugin DLL stays self-contained.
// Only the 5 draw-toggle bools are persisted — colors stay at their compile-time
// defaults, which keeps the schema trivial. Adding new fields is a simple matter
// of extending Save/Load with another line.

#pragma once

#include <imgui.h>

#include <filesystem>
#include <fstream>
#include <string>

struct RadarSettings {
    bool DrawWalkable    = true;
    bool DrawMonsters    = true;
    bool DrawNpcs        = true;
    bool DrawChests      = true;
    bool DrawTransitions = true;

    ImVec4 RarityColor[4] = {
        {1.f, 1.f, 1.f, 1.f},     // normal
        {0.4f, 0.7f, 1.f, 1.f},   // magic
        {1.f, 1.f, 0.4f, 1.f},    // rare
        {1.f, 0.5f, 0.f, 1.f},    // unique
    };
    ImVec4 NpcColor         {0.4f, 1.f,  0.4f, 1.f};
    ImVec4 ChestColor       {0.f,  1.f,  1.f,  1.f};
    ImVec4 ChestOpenedColor {0.3f, 0.5f, 0.5f, 1.f};
    ImVec4 TransitionColor  {0.8f, 0.4f, 1.f,  1.f};
    ImVec4 PlayerColor      {1.f,  1.f,  0.f,  1.f};
    // Pure white at ~78% alpha — matches the host radar's walkable tile
    // overlay color so the demo plugin reads at a glance.
    ImVec4 WalkableColor    {1.0f, 1.0f, 1.0f, 200.0f / 255.0f};

    // fs::path overloads — Unicode-safe. Plugins should pass DirectoryPath()
    // (UTF-8 → wide conversion baked in) rather than Directory(), because
    // fs::path(std::string) on Windows interprets the bytes via the system
    // ANSI codepage and produces mojibake when the host's exe lives in a
    // folder with non-ASCII chars (Cyrillic, CJK).
    void Save(const std::filesystem::path& directory) const;
    void Load(const std::filesystem::path& directory);
};

inline void RadarSettings::Save(const std::filesystem::path& directory) const {
    std::filesystem::path p = directory / "config" / "settings.json";
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p);
    if (!out.is_open()) return;
    out << "{\n";
    out << "  \"DrawWalkable\":"    << (DrawWalkable    ? "true" : "false") << ",\n";
    out << "  \"DrawMonsters\":"    << (DrawMonsters    ? "true" : "false") << ",\n";
    out << "  \"DrawNpcs\":"        << (DrawNpcs        ? "true" : "false") << ",\n";
    out << "  \"DrawChests\":"      << (DrawChests      ? "true" : "false") << ",\n";
    out << "  \"DrawTransitions\":" << (DrawTransitions ? "true" : "false") << "\n";
    out << "}\n";
}

inline void RadarSettings::Load(const std::filesystem::path& directory) {
    std::filesystem::path p = directory / "config" / "settings.json";
    if (!std::filesystem::exists(p)) return;
    std::ifstream in(p);
    if (!in.is_open()) return;
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    auto readBool = [&](const char* key) -> bool {
        return content.find(std::string("\"") + key + "\":true") != std::string::npos;
    };
    DrawWalkable    = readBool("DrawWalkable");
    DrawMonsters    = readBool("DrawMonsters");
    DrawNpcs        = readBool("DrawNpcs");
    DrawChests      = readBool("DrawChests");
    DrawTransitions = readBool("DrawTransitions");
}
