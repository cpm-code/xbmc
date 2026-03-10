/*
 *      Copyright (C) 2010-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#version 310 es

layout(location = 0) in vec4 m_attrpos;
layout(location = 1) in vec4 m_attrcol;
layout(location = 2) in vec4 m_attrcord0;
layout(location = 3) in vec4 m_attrcord1;
out vec4 m_cord0;
out vec4 m_cord1;
out lowp vec4 m_colour;

layout(std140) uniform KodiGuiVertexBlock
{
  mat4 uProj;
  mat4 uModel;
};

uniform mat4 m_coord0Matrix;
uniform float m_depth;

void main()
{
  mat4 mvp = uProj * uModel;
  gl_Position = mvp * m_attrpos;
  gl_Position.z = m_depth * gl_Position.w;
  m_colour = m_attrcol;
  m_cord0 = m_coord0Matrix * m_attrcord0;
  m_cord1 = m_attrcord1;
}
