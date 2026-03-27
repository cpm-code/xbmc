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

#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform sampler2D m_samp0;
in vec4 m_cord0;
#if defined(KODI_HDR_PGS_PQ_OUTPUT) || defined(KODI_HDR_PGS_SDR_OUTPUT)
#endif

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

#if defined(KODI_HDR_PGS_PQ_OUTPUT) || defined(KODI_HDR_PGS_SDR_OUTPUT)
vec3 decodePQ(vec3 x)
{
  const float ST2084_m1 = 2610.0 / (4096.0 * 4.0);
  const float ST2084_m2 = (2523.0 / 4096.0) * 128.0;
  const float ST2084_c1 = 3424.0 / 4096.0;
  const float ST2084_c2 = (2413.0 / 4096.0) * 32.0;
  const float ST2084_c3 = (2392.0 / 4096.0) * 32.0;

  x = clamp(x, vec3(0.0), vec3(1.0));
  vec3 p = pow(x, vec3(1.0 / ST2084_m2));
  vec3 num = max(p - vec3(ST2084_c1), vec3(0.0));
  vec3 den = max(vec3(ST2084_c2) - vec3(ST2084_c3) * p, vec3(1e-6));
  return pow(num / den, vec3(1.0 / ST2084_m1));
}

vec3 encodePQ(vec3 x)
{
  const float ST2084_m1 = 2610.0 / (4096.0 * 4.0);
  const float ST2084_m2 = (2523.0 / 4096.0) * 128.0;
  const float ST2084_c1 = 3424.0 / 4096.0;
  const float ST2084_c2 = (2413.0 / 4096.0) * 32.0;
  const float ST2084_c3 = (2392.0 / 4096.0) * 32.0;

  x = max(x, vec3(0.0));
  vec3 p = pow(x, vec3(ST2084_m1));
  vec3 y = (vec3(ST2084_c1) + vec3(ST2084_c2) * p) / (vec3(1.0) + vec3(ST2084_c3) * p);
  y = pow(y, vec3(ST2084_m2));
  return clamp(y, vec3(0.0), vec3(1.0));
}

vec3 convertHdrPgsForPqOutput(vec3 pq)
{
  vec3 linear = decodePQ(pq);
  vec3 luma = vec3(dot(linear, vec3(0.2627, 0.6780, 0.0593)));
  linear = mix(luma, linear, uGuiParams1.y);
  linear = max(linear, vec3(0.0));
  linear *= uGuiParams1.x;
  return encodePQ(linear);
}

vec3 convertHdrPgsForSdrOutput(vec3 pq)
{
  const mat3 bt2020ToBt709 = mat3(
      1.6605, -0.5876, -0.0728,
     -0.1246,  1.1329, -0.0083,
     -0.0182, -0.1006,  1.1187);

  vec3 linear = decodePQ(pq);
  vec3 luma = vec3(dot(linear, vec3(0.2627, 0.6780, 0.0593)));
  linear = mix(luma, linear, uGuiParams1.y);
  linear = max(linear, vec3(0.0)) * (100.0 * uGuiParams1.x);
  linear = clamp(bt2020ToBt709 * linear, vec3(0.0), vec3(1.0));

  return pow(linear, vec3(0.45));
}
#endif

void main()
{
  vec4 rgb = texture(m_samp0, m_cord0.xy);

#if defined(KODI_TRANSFER_PQ)
  rgb.rgb = convertGuiForPqOutput(rgb.rgb);
#elif defined(KODI_HDR_PGS_PQ_OUTPUT)
  rgb.rgb = convertHdrPgsForPqOutput(rgb.rgb);
#elif defined(KODI_HDR_PGS_SDR_OUTPUT)
  rgb.rgb = convertHdrPgsForSdrOutput(rgb.rgb);
#endif

#if defined(KODI_LIMITED_RANGE)
  rgb.rgb *= (235.0 - 16.0) / 255.0;
  rgb.rgb += 16.0 / 255.0;
#endif

  fragColor = rgb;
}