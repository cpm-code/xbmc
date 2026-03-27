/*
 *  Copyright (C) 2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#version 310 es

#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform sampler2D m_samp0;
uniform lowp vec4 m_unicol;
in vec4 m_cord0;

layout(std140) uniform KodiGuiFragmentBlock
{
  vec4 uGuiParams0;
  vec4 uGuiParams1;
};

out vec4 fragColor;

highp float interleavedGradientNoise(highp vec2 co)
{
  return fract(52.9829189 * fract(0.06711056 * co.x + 0.00583715 * co.y));
}

vec3 convertGuiForPqOutput(vec3 x)
{
  const float ST2084_m1 = 2610.0 / (4096.0 * 4.0);
  const float ST2084_m2 = (2523.0 / 4096.0) * 128.0;
  const float ST2084_c1 = 3424.0 / 4096.0;
  const float ST2084_c2 = (2413.0 / 4096.0) * 32.0;
  const float ST2084_c3 = (2392.0 / 4096.0) * 32.0;

  const mat3 matx = mat3(
      0.627402, 0.069095, 0.016394,
      0.329292, 0.919544, 0.088028,
      0.043306, 0.011360, 0.895578);

  x = max(x, vec3(0.0));
  x = pow(x, vec3(1.0 / 0.45));
  x = matx * x;
  x = max(x, vec3(0.0));

  vec3 luma = vec3(dot(x, vec3(0.2627, 0.6780, 0.0593)));
  x = mix(luma, x, uGuiParams0.w);
  x = max(x, vec3(0.0));

  float peakNits = 100.0 * uGuiParams0.z;
  x = pow(x * (peakNits / 10000.0), vec3(ST2084_m1));
  x = (ST2084_c1 + ST2084_c2 * x) / (1.0 + ST2084_c3 * x);
  x = pow(x, vec3(ST2084_m2));

  float dither = (interleavedGradientNoise(gl_FragCoord.xy) - 0.5) / 1024.0;
  return clamp(x + vec3(dither), vec3(0.0), vec3(1.0));
}

void main()
{
  vec4 rgb = m_unicol;
  rgb.a *= texture(m_samp0, m_cord0.xy).r;

#if defined(KODI_TRANSFER_PQ)
  rgb.rgb = convertGuiForPqOutput(rgb.rgb);
#endif

#if defined(KODI_LIMITED_RANGE)
  rgb.rgb *= (235.0 - 16.0) / 255.0;
  rgb.rgb += 16.0 / 255.0;
#endif

  fragColor = rgb;
}