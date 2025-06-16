/*
 *  Copyright (C) 2017-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "RPProcessInfoAmlogic.h"

using namespace KODI;
using namespace RETRO;

CRPProcessInfoAmlogic::CRPProcessInfoAmlogic() :
  CRPProcessInfo("Amlogic")
{
}

std::unique_ptr<CRPProcessInfo> CRPProcessInfoAmlogic::Create()
{
  return std::make_unique<CRPProcessInfoAmlogic>();
}

void CRPProcessInfoAmlogic::Register()
{
  CRPProcessInfo::RegisterProcessControl(CRPProcessInfoAmlogic::Create);
}
