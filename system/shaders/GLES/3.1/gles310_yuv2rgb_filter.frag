/*
 *  Copyright (C) 2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#version 310 es

precision highp float;

uniform sampler2D m_sampY;
uniform sampler2D m_sampU;
uniform sampler2D m_sampV;
uniform float m_stretch;
uniform sampler2D m_kernelTex;

layout(std140) uniform KodiYuvParamsBlock
{
  vec4 uStepAlphaField;
  vec4 uGamma;
  vec4 uCoefsDst;
  mat4 uYuvMat;
  mat4 uPrimMat;
};

in vec2 m_cordY;
in vec2 m_cordU;
in vec2 m_cordV;
out vec4 fragColor;

vec4[4] load4x4_0(sampler2D sampler, vec2 pos)
{
  vec4[4] tex4x4;
  vec4 tex2x2 = textureGather(sampler, pos, 0);
  tex4x4[0].xy = tex2x2.wz;
  tex4x4[1].xy = tex2x2.xy;
  tex2x2 = textureGatherOffset(sampler, pos, ivec2(2,0), 0);
  tex4x4[0].zw = tex2x2.wz;
  tex4x4[1].zw = tex2x2.xy;
  tex2x2 = textureGatherOffset(sampler, pos, ivec2(0,2), 0);
  tex4x4[2].xy = tex2x2.wz;
  tex4x4[3].xy = tex2x2.xy;
  tex2x2 = textureGatherOffset(sampler, pos, ivec2(2,2), 0);
  tex4x4[2].zw = tex2x2.wz;
  tex4x4[3].zw = tex2x2.xy;
  return tex4x4;
}

float filter_0(sampler2D sampler, vec2 coord)
{
  vec2 pos = coord + uStepAlphaField.xy * 0.5;
  vec2 f = fract(pos / uStepAlphaField.xy);

  vec4 linetaps = texture(m_kernelTex, vec2(1.0 - f.x, 0.));
  vec4 coltaps = texture(m_kernelTex, vec2(1.0 - f.y, 0.));
  mat4 conv;
  conv[0] = linetaps * coltaps.x;
  conv[1] = linetaps * coltaps.y;
  conv[2] = linetaps * coltaps.z;
  conv[3] = linetaps * coltaps.w;

  vec2 startPos = (-1.0 - f) * uStepAlphaField.xy + pos;
  vec4[4] tex4x4 = load4x4_0(sampler, startPos);

    return dot(tex4x4[0], conv[0]) +
      dot(tex4x4[1], conv[1]) +
      dot(tex4x4[2], conv[2]) +
      dot(tex4x4[3], conv[3]);
}

void main()
{
  vec4 rgb;
  vec4 yuv;

#if defined(XBMC_YV12) || defined(XBMC_NV12)

  yuv = vec4(filter_0(m_sampY, m_cordY),
             texture(m_sampU, m_cordU).g,
             texture(m_sampV, m_cordV).a,
             1.0);

#elif defined(XBMC_NV12_RRG)

  yuv = vec4(filter_0(m_sampY, m_cordY),
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
