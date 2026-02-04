/*
 *  Copyright (C) 2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "BitstreamIoWriter.h"
#include "HDR10PlusConvert.h"

std::vector<uint8_t> create_rpu_nalu(VdrDmData& vdr_dm_data);