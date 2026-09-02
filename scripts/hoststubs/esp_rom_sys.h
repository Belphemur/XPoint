#pragma once

// Host-build stub: BoardConfig.h's power-rail warning path uses
// esp_rom_printf. Declaration only — the dump never calls it.

int esp_rom_printf(const char* fmt, ...);
