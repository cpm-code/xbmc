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

precision mediump float;

uniform sampler2D m_sampY;
uniform sampler2D m_sampU;
uniform sampler2D m_sampV;
in vec2 m_cordY;
in vec2 m_cordU;
in vec2 m_cordV;

layout(std140) uniform KodiYuvParamsBlock
{
  vec4 uStepAlphaField;
  vec4 uGamma;
  vec4 uCoefsDst;
  mat4 uYuvMat;
  mat4 uPrimMat;
};

out vec4 fragColor;

void main()
{
  vec4 rgb;
  vec4 yuv;

#if defined(XBMC_YV12) || defined(XBMC_NV12)

  yuv = vec4(texture(m_sampY, m_cordY).r,
      texture(m_sampU, m_cordU).g,
      texture(m_sampV, m_cordV).a,
      1.0);

#elif defined(XBMC_NV12_RRG)

  yuv = vec4(texture(m_sampY, m_cordY).r,
      texture(m_sampU, m_cordU).r,
      texture(m_sampV, m_cordV).g,
      1.0);

#endif

  rgb = uYuvMat * yuv;
  rgb.a = uStepAlphaField.z;

#if defined(XBMC_COL_CONVERSION)
  rgb.rgb = pow(max(vec3(0), rgb.rgb), vec3(uGamma.x));
  rgb.rgb = max(vec3(0), mat3(uPrimMat) * rgb.rgb);
  rgb.rgb = pow(rgb.rgb, vec3(uGamma.y));

#if defined(KODI_TONE_MAPPING_REINHARD)
  float luma = dot(rgb.rgb, uCoefsDst.xyz);
  rgb.rgb *= reinhard(luma) / luma;

#elif defined(KODI_TONE_MAPPING_ACES)
  rgb.rgb = inversePQ(rgb.rgb);
  rgb.rgb *= (10000.0 / uGamma.w) * (2.0 / uGamma.z);
  rgb.rgb = aces(rgb.rgb);
  rgb.rgb *= (1.24 / uGamma.z);
  rgb.rgb = pow(rgb.rgb, vec3(0.27));

#elif defined(KODI_TONE_MAPPING_HABLE)
  rgb.rgb = inversePQ(rgb.rgb);
  rgb.rgb *= uGamma.z;
  float wp = uGamma.w / 100.0;
  rgb.rgb = hable(rgb.rgb * wp) / hable(vec3(wp));
  rgb.rgb = pow(rgb.rgb, vec3(1.0 / 2.2));
#endif

#endif

  fragColor = rgb;
}
