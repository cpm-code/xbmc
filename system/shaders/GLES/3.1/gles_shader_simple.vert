/*
 *  Copyright (C) 2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#version 310 es

layout(location = 0) in vec4 m_attrpos;
layout(location = 1) in vec4 m_attrcol;
layout(location = 2) in vec4 m_attrcord0;
layout(location = 3) in vec4 m_attrcord1;
out vec4 m_cord0;
out vec4 m_cord1;
out lowp vec4 m_colour;
uniform mat4 m_matrix;
uniform float m_depth;

void main()
{
  gl_Position = m_matrix * m_attrpos;
  gl_Position.z = m_depth * gl_Position.w;
  m_colour = m_attrcol;
  m_cord0 = m_attrcord0;
  m_cord1 = m_attrcord1;
}
