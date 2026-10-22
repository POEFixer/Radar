# Radar — POE2Fixer Plugin SDK Example

Current version: 1.0.1

Requires POE2Fixer 364 or newer.

Minimal radar overlay plugin demonstrating the v6 Plugin SDK. Draws
monsters / NPCs / chests / area transitions and the walkable map onto
the in-game large map.

## What It Does

Hooks into POE2Fixer's plugin system. When enabled, it draws colored
dots on the in-game large map for nearby entities, plus a translucent
walkable-tile overlay. Per-rarity monster colors, NPCs, chests
(closed vs opened), and area transitions are configurable from the
Plugins settings tab inside POE2Fixer.

## Install

Install it from POE2Fixer → Plugins → Plugin Manager (one click, updates included), or download `Radar.zip` from [Releases](https://github.com/POEFixer/Radar/releases/latest) and extract it into the POE2Fixer folder (it creates `Plugins\Radar\`). Then enable the plugin in the Plugins tab.

## Build from source

This repository is the plugin folder from the POE2Fixer source tree (`Plugins/Radar/`) together with the host files it compiles against, kept at the same relative paths:

- `POEFixer/plugin_sdk/` - the POE2Fixer Plugin SDK v6 headers (`PluginAbi.h`, `PluginSDK.h`)
- `POEFixer/imgui/` - the Dear ImGui sources the DLL compiles against

1. Install Visual Studio 2022 with the "Desktop development with C++" workload (MSVC v143, Windows 10/11 SDK).
2. Open `Radar.sln`, select **Release | x64** and build the solution.
3. The plugin is written to `x64\Release\Plugins\Radar\Radar.dll` (Release builds carry no debug information, see `Directory.Build.targets`).
4. Copy `Radar.dll` into `Plugins\Radar\` next to the POE2Fixer executable.
