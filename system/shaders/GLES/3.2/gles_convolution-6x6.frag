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

#version 320 es

precision highp float;

uniform sampler2D img;
uniform vec2 stepxy;
in vec2 cord;
uniform sampler2D kernelTex;

layout(std140) uniform KodiVideoFilterVertexBlock
{
  mat4 uProj;
  mat4 uModel;
  vec4 uAlpha;
};

out vec4 fragColor;

vec3 weight(float pos)
{
#if defined(HAS_FLOAT_TEXTURE)
  return texture(kernelTex, vec2(pos, 0.5)).rgb;
#else
  return texture(kernelTex, vec2(pos - 0.5, 0.0)).rgb * 2.0 - 1.0;
#endif
}

ivec2 clampCoord(ivec2 coord, ivec2 texSize)
{
  return clamp(coord, ivec2(0), texSize - ivec2(1));
}

vec3 sampleLine(int ypos, int xpos, vec3 taps1, vec3 taps2, ivec2 texSize)
{
  vec4 s0 = texelFetch(img, clampCoord(ivec2(xpos, ypos), texSize), 0);
  vec4 s1 = texelFetch(img, clampCoord(ivec2(xpos + 1, ypos), texSize), 0);
  vec4 s2 = texelFetch(img, clampCoord(ivec2(xpos + 2, ypos), texSize), 0);
  vec4 s3 = texelFetch(img, clampCoord(ivec2(xpos + 3, ypos), texSize), 0);
  vec4 s4 = texelFetch(img, clampCoord(ivec2(xpos + 4, ypos), texSize), 0);
  vec4 s5 = texelFetch(img, clampCoord(ivec2(xpos + 5, ypos), texSize), 0);

  return s0.rgb * taps1.x +
      s1.rgb * taps2.x +
      s2.rgb * taps1.y +
      s3.rgb * taps2.y +
      s4.rgb * taps1.z +
      s5.rgb * taps2.z;
}

void main()
{
  vec2 pos = cord + stepxy * 0.5;
  vec2 f = fract(pos / stepxy);
  ivec2 texSize = textureSize(img, 0);
  ivec2 base = ivec2(floor(pos / stepxy));

  vec3 linetaps1 = weight((1.0 - f.x) / 2.0);
  vec3 linetaps2 = weight((1.0 - f.x) / 2.0 + 0.5);
  vec3 columntaps1 = weight((1.0 - f.y) / 2.0);
  vec3 columntaps2 = weight((1.0 - f.y) / 2.0 + 0.5);

  int startX = base.x - 3;
  int startY = base.y - 3;

  vec3 rgb = sampleLine(startY, startX, linetaps1, linetaps2, texSize) * columntaps1.x +
      sampleLine(startY + 1, startX, linetaps1, linetaps2, texSize) * columntaps2.x +
      sampleLine(startY + 2, startX, linetaps1, linetaps2, texSize) * columntaps1.y +
      sampleLine(startY + 3, startX, linetaps1, linetaps2, texSize) * columntaps2.y +
      sampleLine(startY + 4, startX, linetaps1, linetaps2, texSize) * columntaps1.z +
      sampleLine(startY + 5, startX, linetaps1, linetaps2, texSize) * columntaps2.z;

  fragColor = vec4(rgb, uAlpha.x);
}
