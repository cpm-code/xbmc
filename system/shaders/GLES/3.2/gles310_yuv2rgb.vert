/*
 *  Copyright (C) 2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#version 320 es

layout(location = 0) in vec4 m_attrpos;
layout(location = 1) in vec2 m_attrcordY;
layout(location = 2) in vec2 m_attrcordU;
layout(location = 3) in vec2 m_attrcordV;
out vec2 m_cordY;
out vec2 m_cordU;
out vec2 m_cordV;

layout(std140) uniform KodiYuvVertexBlock
{
  mat4 uProj;
  mat4 uModel;
};

void main()
{
  mat4 mvp = uProj * uModel;
  gl_Position = mvp * m_attrpos;
  m_cordY = m_attrcordY;
  m_cordU = m_attrcordU;
  m_cordV = m_attrcordV;
}
