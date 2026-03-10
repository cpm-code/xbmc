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

#extension GL_OES_EGL_image_external_essl3 : require

precision mediump float;
uniform samplerExternalOES m_samp0;
in vec4 m_cord0;

layout(std140) uniform KodiGuiFragmentBlock
{
  vec4 uGuiParams0;
  vec4 uGuiParams1;
};

out vec4 fragColor;

void main()
{
  vec4 rgb = texture(m_samp0, m_cord0.xy);
  rgb *= uGuiParams0.y;
  rgb += uGuiParams0.x;
  fragColor = rgb;
}