// The app's names and where it keeps its files, in one place.
//   - The .nro is BMHero_nx.nro, and everything the app needs on the SD card lives in sdmc:/switch/BMHero_nx/
//     (the ROM, config.ini, saves, mods, logs).
//   - What the launcher shows is just "Bomberman Hero" (BMHNX_DISPLAY_NAME; the NACP in CMakeLists.txt matches it).
// C and C++ both include this, so it holds nothing but macros.
#pragma once

#define BMHNX_APP_NAME "BMHero_nx"
#define BMHNX_DISPLAY_NAME "Bomberman Hero"
#define BMHNX_DATA_DIR "sdmc:/switch/" BMHNX_APP_NAME

// Before the app was named BMHero_nx, its files were in this folder; the first run moves them (main_switch.cpp).
#define BMHNX_OLD_DATA_DIR "sdmc:/switch/bmherorecomp"
