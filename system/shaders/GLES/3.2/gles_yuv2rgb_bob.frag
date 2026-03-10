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

  vec2 offsetY = m_cordY;
  vec2 offsetU = m_cordU;
  vec2 offsetV = m_cordV;
  float temp1 = mod(m_cordY.y, 2.0 * uStepAlphaField.y);

  offsetY.y -=
    (temp1 - uStepAlphaField.y / 2.0 + uStepAlphaField.w * uStepAlphaField.y);
  offsetU.y -=
    (temp1 - uStepAlphaField.y / 2.0 + uStepAlphaField.w * uStepAlphaField.y) / 2.0;
  offsetV.y -=
    (temp1 - uStepAlphaField.y / 2.0 + uStepAlphaField.w * uStepAlphaField.y) / 2.0;

  float bstep = step(uStepAlphaField.y, temp1);

  vec2 belowY, belowU, belowV;
  belowY.x = offsetY.x;
  belowY.y = offsetY.y + (2.0 * uStepAlphaField.y * bstep);
  belowU.x = offsetU.x;
  belowU.y = offsetU.y + (uStepAlphaField.y * bstep);
  belowV.x = offsetV.x;
  belowV.y = offsetV.y + (uStepAlphaField.y * bstep);

  vec4 rgbAbove;
  vec4 rgbBelow;
  vec4 yuvAbove;
  vec4 yuvBelow;

  yuvAbove = vec4(texture(m_sampY, offsetY).r,
                  texture(m_sampU, offsetU).g,
                  texture(m_sampV, offsetV).a,
                  1.0);
  rgbAbove = uYuvMat * yuvAbove;
  rgbAbove.a = uStepAlphaField.z;

  yuvBelow = vec4(texture(m_sampY, belowY).r,
                  texture(m_sampU, belowU).g,
                  texture(m_sampV, belowV).a,
                  1.0);
  rgbBelow = uYuvMat * yuvBelow;
  rgbBelow.a = uStepAlphaField.z;

  rgb = mix(rgbAbove, rgbBelow, 0.5);

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
