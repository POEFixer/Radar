# Changelog

## 1.0.6

- No longer switched off by the program as too slow, and no frame-rate drop with the large map open: the radar reads only the monsters, NPCs, chests and area transitions it draws instead of every object in the area (thousands on a map).
- Fewer walkable dots: at least 12 pixels apart, at most 20,000 per frame, thinned evenly when the map is zoomed out.

## 1.0.5

- Much faster with the large map open: the program no longer switches the plugin off as too slow on large areas.
- Walkable dots are drawn only where they are on screen and are thinned out when the map is zoomed far out.
- Map points now sit on the game's map at any terrain height.

## 1.0.4

- Built with the current plugin SDK: a controller that is not XInput reports no port (-1) even when ready.

## 1.0.3

- Follows the program language (ten languages).
- Chests, area transitions and Magic monsters are named as the game names them (Japanese, Traditional Chinese, Spanish and Russian).

## 1.0.2

- Works with the separate menu window (POEFixer 366): the host hands the plugin the right drawing context for its settings tab and for its overlay.
- The settings tab also sets its drawing context itself, so it draws in the right window with the menu in a separate window.

## 1.0.1

- Rebuilt with the updated plugin SDK headers (POEFixer 365 adds optional calls for UI control names, stash objects and tabs, wider entity scans and the host automation flag). No change in what the plugin does.

## 1.0.0

- First versioned release.
