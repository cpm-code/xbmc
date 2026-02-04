#pragma once

#include <string>

#include "HevcSei.h"

struct WellKnownMasteringDisplayColourVolume
{
    uint8_t code; //ISO code
    uint16_t values[8]; // G, B, R, W pairs (x values then y values)
};

static const WellKnownMasteringDisplayColourVolume knownColourVolumes[] =
{
  // Code     G             B             R             W
    { 1, {15000, 30000,  7500,  3000, 32000, 16500, 15635, 16450}}, // BT.709
    { 9, { 8500, 39850,  6550,  2300, 35400, 14600, 15635, 16450}}, // BT.2020
    {11, {13250, 34500,  7500,  3000, 34000, 16000, 15700, 17550}}, // DCI P3
    {12, {13250, 34500,  7500,  3000, 34000, 16000, 15635, 16450}}, // Display P3
};

std::string CodeToColourPrimaries(uint8_t code);
std::string MasteringDisplayColourVolumeText(const MasteringDisplayColourVolume& mdcv);