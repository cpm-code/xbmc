/*
 *  Copyright (C) 2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#version 310 es

in vec4 m_attrpos;
in vec4 m_attrcol;
in vec4 m_attrcord0;
in vec4 m_attrcord1;
out vec4 m_cord0;
out vec4 m_cord1;
out lowp vec4 m_colour;
uniform mat4 m_matrix;
uniform vec4 m_shaderClip;
uniform vec4 m_cordStep;
uniform float m_depth;

void main()
{
  vec4 position = m_attrpos;
  position.xy = clamp(position.xy, m_shaderClip.xy, m_shaderClip.zw);
  gl_Position = m_matrix * position;
  gl_Position.z = m_depth * gl_Position.w;

  vec2 clipDist = m_attrpos.xy - position.xy;
  m_cord0.xy = m_attrcord0.xy - clipDist * m_cordStep.xy;
  m_cord1.xy = m_attrcord1.xy - clipDist * m_cordStep.zw;
  m_cord0.zw = m_attrcord0.zw;
  m_cord1.zw = m_attrcord1.zw;

  m_colour = m_attrcol;
}