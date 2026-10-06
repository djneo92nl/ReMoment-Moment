// The SD filesystem object. Most boards wire the slot as SDMMC; the 4848S040's is SPI-only.
#pragma once

#if defined(BOARD_ESP32_4848S040)
#include <SD.h>
#define RM_SD SD
#else
#include <SD_MMC.h>
#define RM_SD SD_MMC
#endif
