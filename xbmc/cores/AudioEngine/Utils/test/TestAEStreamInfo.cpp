/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "cores/AudioEngine/Utils/AEStreamInfo.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <memory>
#include <vector>

namespace
{
constexpr size_t TEST_EAC3_FRAME_SIZE{10};

std::array<uint8_t, TEST_EAC3_FRAME_SIZE> MakeEac3Frame(uint8_t strmtyp,
                                                        uint8_t dialnorm,
                                                        uint8_t byte6LowBits = 0x00)
{
  return {0x0B,
          0x77,
          static_cast<uint8_t>((strmtyp << 6) | 0x00),
          0x04,
          0x34,
          static_cast<uint8_t>((11u << 3) | ((dialnorm >> 2) & 0x07)),
          static_cast<uint8_t>(((dialnorm & 0x03) << 6) | (byte6LowBits & 0x3F)),
          0x12,
          0x34,
          0x56};
}

uint8_t GetEac3Dialnorm(const uint8_t* frame)
{
  return ((frame[5] & 0x07) << 2) | ((frame[6] >> 6) & 0x03);
}
} // namespace

TEST(TestAEStreamInfo, DefeatDialNormStillPatchesIndependentEac3)
{
  auto mainFrame = MakeEac3Frame(0, 12, 0x15);
  std::vector<uint8_t> input(mainFrame.begin(), mainFrame.end());
  input.resize(input.size() + 8, 0x00);

  CAEStreamParser parser;
  parser.SetDefeatAC3DialNorm(true);

  uint8_t* packet = nullptr;
  unsigned int packetSize = 0;
  EXPECT_EQ(parser.AddData(input.data(), input.size(), &packet, &packetSize), input.size());
  std::unique_ptr<uint8_t[]> packetHolder(packet);

  ASSERT_NE(packetHolder, nullptr);
  ASSERT_EQ(packetSize, mainFrame.size());
  EXPECT_EQ(GetEac3Dialnorm(packetHolder.get()), 31);
}

TEST(TestAEStreamInfo, DefeatDialNormLeavesDependentEac3SubstreamUntouched)
{
  auto mainFrame = MakeEac3Frame(0, 12, 0x15);
  auto dependentFrame = MakeEac3Frame(1, 7, 0x2A);

  std::vector<uint8_t> input(mainFrame.begin(), mainFrame.end());
  input.insert(input.end(), dependentFrame.begin(), dependentFrame.end());

  CAEStreamParser parser;
  parser.SetDefeatAC3DialNorm(true);

  uint8_t* packet = nullptr;
  unsigned int packetSize = 0;
  EXPECT_EQ(parser.AddData(input.data(), input.size(), &packet, &packetSize), input.size());
  std::unique_ptr<uint8_t[]> packetHolder(packet);

  ASSERT_NE(packetHolder, nullptr);
  ASSERT_EQ(packetSize, input.size());
  EXPECT_EQ(GetEac3Dialnorm(packetHolder.get()), 31);
  EXPECT_EQ(GetEac3Dialnorm(packetHolder.get() + mainFrame.size()), 7);
  EXPECT_EQ(std::memcmp(packetHolder.get() + mainFrame.size(), dependentFrame.data(),
                        dependentFrame.size()),
            0);
}
