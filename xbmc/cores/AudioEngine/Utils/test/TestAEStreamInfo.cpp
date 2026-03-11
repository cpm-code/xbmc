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
constexpr size_t TEST_AC3_FRAME_SIZE{128};
constexpr unsigned int AC3_CRC16_POLY{(1u << 0) | (1u << 2) | (1u << 15) | (1u << 16)};

uint16_t Ac3ByteSwap16(uint16_t x)
{
  return (x >> 8) | (x << 8);
}

unsigned int Ac3MulPoly(unsigned int a, unsigned int b, unsigned int poly)
{
  unsigned int c = 0;
  while (a)
  {
    if (a & 1)
      c ^= b;
    a >>= 1;
    b <<= 1;
    if (b & (1u << 16))
      b ^= poly;
  }
  return c;
}

unsigned int Ac3PowPoly(unsigned int a, unsigned int n, unsigned int poly)
{
  unsigned int r = 1;
  while (n)
  {
    if (n & 1)
      r = Ac3MulPoly(r, a, poly);
    a = Ac3MulPoly(a, a, poly);
    n >>= 1;
  }
  return r;
}

void SetAc3Dialnorm(uint8_t* data, uint8_t acmod, uint8_t value)
{
  unsigned int extra = 0;
  if ((acmod & 0x1) && (acmod != 0x1))
    extra += 2;
  if (acmod & 0x4)
    extra += 2;
  if (acmod == 0x2)
    extra += 2;

  uint32_t bits = (static_cast<uint32_t>(data[6]) << 16) | (static_cast<uint32_t>(data[7]) << 8) | data[8];
  const unsigned int shift = 15 - extra;
  bits = (bits & ~(0x1Fu << shift)) | (static_cast<uint32_t>(value & 0x1F) << shift);
  data[6] = (bits >> 16) & 0xFF;
  data[7] = (bits >> 8) & 0xFF;
  data[8] = bits & 0xFF;
}

uint8_t GetAc3Dialnorm(const uint8_t* data, uint8_t acmod)
{
  unsigned int extra = 0;
  if ((acmod & 0x1) && (acmod != 0x1))
    extra += 2;
  if (acmod & 0x4)
    extra += 2;
  if (acmod == 0x2)
    extra += 2;

  uint32_t bits = (static_cast<uint32_t>(data[6]) << 16) | (static_cast<uint32_t>(data[7]) << 8) | data[8];
  const unsigned int shift = 15 - extra;
  return (bits >> shift) & 0x1F;
}

std::array<uint8_t, TEST_AC3_FRAME_SIZE> MakeAc3Frame(uint8_t dialnorm)
{
  std::array<uint8_t, TEST_AC3_FRAME_SIZE> frame{};
  frame[0] = 0x0B;
  frame[1] = 0x77;
  frame[4] = 0x00;           // fscod=0 (48 kHz), frmsizecod=0 (32 kbps, 128-byte frame)
  frame[5] = static_cast<uint8_t>(8u << 3); // bsid=8, bsmod=0
  frame[6] = 0x00;           // acmod=0, remaining BSI bits cleared
  SetAc3Dialnorm(frame.data(), 0, dialnorm);

  const AVCRC* crcTable = av_crc_get_table(AV_CRC_16_ANSI);
  constexpr unsigned int frameSizeFiveEighths = 80;

  uint16_t crc1 =
      Ac3ByteSwap16(static_cast<uint16_t>(av_crc(crcTable, 0, frame.data() + 4, frameSizeFiveEighths - 4)));
  const unsigned int crcInv =
      Ac3PowPoly((AC3_CRC16_POLY >> 1), (8 * frameSizeFiveEighths) - 16, AC3_CRC16_POLY);
  crc1 = static_cast<uint16_t>(Ac3MulPoly(crcInv, crc1, AC3_CRC16_POLY));
  frame[2] = (crc1 >> 8) & 0xFF;
  frame[3] = crc1 & 0xFF;

  uint16_t crc2 = Ac3ByteSwap16(
      static_cast<uint16_t>(av_crc(crcTable, 0, frame.data() + frameSizeFiveEighths,
                                   TEST_AC3_FRAME_SIZE - frameSizeFiveEighths - 2)));
  if (crc2 == 0x0B77)
  {
    frame[TEST_AC3_FRAME_SIZE - 3] ^= 0x1;
    crc2 ^= 0x8005;
  }
  frame[TEST_AC3_FRAME_SIZE - 2] = (crc2 >> 8) & 0xFF;
  frame[TEST_AC3_FRAME_SIZE - 1] = crc2 & 0xFF;

  return frame;
}

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

TEST(TestAEStreamInfo, DefeatDialNormStillPatchesAc3)
{
  auto frame = MakeAc3Frame(12);
  std::vector<uint8_t> input(frame.begin(), frame.end());
  input.resize(input.size() + 8, 0x00);

  CAEStreamParser parser;
  parser.SetDefeatAC3DialNorm(true);

  uint8_t* packet = nullptr;
  unsigned int packetSize = 0;
  EXPECT_EQ(parser.AddData(input.data(), input.size(), &packet, &packetSize), input.size());
  std::unique_ptr<uint8_t[]> packetHolder(packet);

  ASSERT_NE(packetHolder, nullptr);
  ASSERT_EQ(packetSize, frame.size());
  EXPECT_EQ(GetAc3Dialnorm(packetHolder.get(), 0), 31);
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
