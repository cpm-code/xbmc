/*
 *  Copyright (c) 2007 d4rk
 *  Copyright (C) 2007-2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "YUV2RGBShaderGLES.h"

#include "../RenderFlags.h"
#include "ConvolutionKernels.h"
#include "ToneMappers.h"
#include "rendering/gles/RenderSystemGLES.h"
#include "settings/AdvancedSettings.h"
#include "utils/GLUtils.h"
#include "utils/log.h"

#include <algorithm>
#include <array>
#include <sstream>
#include <string>

using namespace Shaders::GLES;

namespace
{
constexpr GLuint YUV_VERTEX_BINDING_POINT = 0;
constexpr GLuint YUV_FRAGMENT_BINDING_POINT = 1;

struct YuvVertexBlockData
{
  std::array<GLfloat, 16> proj{};
  std::array<GLfloat, 16> model{};
};

struct YuvFragmentBlockData
{
  std::array<GLfloat, 4> stepAlphaField{};
  std::array<GLfloat, 4> gamma{};
  std::array<GLfloat, 4> coefsDst{};
  std::array<GLfloat, 16> yuvMat{};
  std::array<GLfloat, 16> primMat{};
};

void EnsureUniformBuffer(GLuint& buffer, GLsizeiptr size)
{
  if (buffer == 0)
  {
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_UNIFORM_BUFFER, buffer);
    glBufferData(GL_UNIFORM_BUFFER, size, nullptr, GL_DYNAMIC_DRAW);
  }
  else
  {
    glBindBuffer(GL_UNIFORM_BUFFER, buffer);
  }
}
} // namespace

//////////////////////////////////////////////////////////////////////
// BaseYUV2RGBGLSLShader - base class for GLSL YUV2RGB shaders
//////////////////////////////////////////////////////////////////////

BaseYUV2RGBGLSLShader::BaseYUV2RGBGLSLShader(EShaderFormat format,
                                             AVColorPrimaries dstPrimaries,
                                             AVColorPrimaries srcPrimaries,
                                             bool toneMap,
                                             ETONEMAPMETHOD toneMapMethod)
{
  m_width = 1;
  m_height = 1;
  m_field = 0;
  m_format = format;

  m_black = 0.0f;
  m_contrast = 1.0f;

  m_convertFullRange = false;

  if (m_format == SHADER_YV12)
    m_defines += "#define XBMC_YV12\n";
  else if (m_format == SHADER_NV12)
    m_defines += "#define XBMC_NV12\n";
  else if (m_format == SHADER_NV12_RRG)
    m_defines += "#define XBMC_NV12_RRG\n";
  else
    CLog::Log(LOGERROR, "GLES: BaseYUV2RGBGLSLShader - unsupported format {}", m_format);

  if (dstPrimaries != srcPrimaries)
  {
    m_colorConversion = true;
    m_defines += "#define XBMC_COL_CONVERSION\n";
  }

  if (toneMap)
  {
    m_toneMapping = true;
    m_toneMappingMethod = toneMapMethod;
    if (toneMapMethod == VS_TONEMAPMETHOD_REINHARD)
      m_defines += "#define KODI_TONE_MAPPING_REINHARD\n";
    else if (toneMapMethod == VS_TONEMAPMETHOD_ACES)
      m_defines += "#define KODI_TONE_MAPPING_ACES\n";
    else if (toneMapMethod == VS_TONEMAPMETHOD_HABLE)
      m_defines += "#define KODI_TONE_MAPPING_HABLE\n";
  }

  VertexShader()->LoadSource("gles_yuv2rgb.vert", m_defines);

  CLog::Log(LOGDEBUG, "GLES: using shader format: {}", m_format);
  CLog::Log(LOGDEBUG, "GLES: using tonemap method: {}", m_toneMappingMethod);

  m_convMatrix.SetSourceColorPrimaries(srcPrimaries).SetDestinationColorPrimaries(dstPrimaries);
}

BaseYUV2RGBGLSLShader::~BaseYUV2RGBGLSLShader()
{
  Free();
}

void BaseYUV2RGBGLSLShader::OnCompiledAndLinked()
{
  if (KODI::GLES::UsesFixedAttributeLocationsForShader(VertexShader()->GetName()))
  {
    m_hVertex = 0;
    m_hYcoord = 1;
    m_hUcoord = 2;
    m_hVcoord = 3;
  }
  else
  {
    m_hVertex = glGetAttribLocation(ProgramHandle(), "m_attrpos");
    m_hYcoord = glGetAttribLocation(ProgramHandle(), "m_attrcordY");
    m_hUcoord = glGetAttribLocation(ProgramHandle(), "m_attrcordU");
    m_hVcoord = glGetAttribLocation(ProgramHandle(), "m_attrcordV");
  }
  m_hProj = glGetUniformLocation(ProgramHandle(), "m_proj");
  m_hModel = glGetUniformLocation(ProgramHandle(), "m_model");
  m_hAlpha = glGetUniformLocation(ProgramHandle(), "m_alpha");
  m_hYTex = glGetUniformLocation(ProgramHandle(), "m_sampY");
  m_hUTex = glGetUniformLocation(ProgramHandle(), "m_sampU");
  m_hVTex = glGetUniformLocation(ProgramHandle(), "m_sampV");
  m_hYuvMat = glGetUniformLocation(ProgramHandle(), "m_yuvmat");
  m_hStep = glGetUniformLocation(ProgramHandle(), "m_step");
  m_hPrimMat = glGetUniformLocation(ProgramHandle(), "m_primMat");
  m_hGammaSrc = glGetUniformLocation(ProgramHandle(), "m_gammaSrc");
  m_hGammaDstInv = glGetUniformLocation(ProgramHandle(), "m_gammaDstInv");
  m_hCoefsDst = glGetUniformLocation(ProgramHandle(), "m_coefsDst");
  m_hToneP1 = glGetUniformLocation(ProgramHandle(), "m_toneP1");
  m_hLuminance = glGetUniformLocation(ProgramHandle(), "m_luminance");

  m_hVertexBlock = glGetUniformBlockIndex(ProgramHandle(), "KodiYuvVertexBlock");
  if (m_hVertexBlock >= 0)
    glUniformBlockBinding(ProgramHandle(), static_cast<GLuint>(m_hVertexBlock),
                          YUV_VERTEX_BINDING_POINT);

  m_hFragmentBlock = glGetUniformBlockIndex(ProgramHandle(), "KodiYuvParamsBlock");
  if (m_hFragmentBlock >= 0)
    glUniformBlockBinding(ProgramHandle(), static_cast<GLuint>(m_hFragmentBlock),
                          YUV_FRAGMENT_BINDING_POINT);

  VerifyGLState();
}

bool BaseYUV2RGBGLSLShader::OnEnabled()
{
  // set shader attributes once enabled
  glUniform1i(m_hYTex, 0);
  glUniform1i(m_hUTex, 1);
  glUniform1i(m_hVTex, 2);

  m_convMatrix.SetDestinationContrast(m_contrast)
      .SetDestinationBlack(m_black)
      .SetDestinationLimitedRange(!m_convertFullRange);

  Matrix4 yuvMat = m_convMatrix.GetYuvMat();
  Matrix3 primMat = m_convMatrix.GetPrimMat();
  const GLfloat stepX = m_width > 0 ? 1.0f / static_cast<GLfloat>(m_width) : 0.0f;
  const GLfloat stepY = m_height > 0 ? 1.0f / static_cast<GLfloat>(m_height) : 0.0f;
  const GLfloat gammaSrc = m_convMatrix.GetGammaSrc();
  const GLfloat gammaDstInv = 1.0f / m_convMatrix.GetGammaDst();

  float toneP1 = 0.0f;
  float luminance = 0.0f;

  if (m_toneMapping)
  {
    if (m_toneMappingMethod == VS_TONEMAPMETHOD_REINHARD)
    {
      float param = 0.7;

      if (m_hasLightMetadata)
      {
        param = log10(100) / log10(m_lightMetadata.MaxCLL);
      }
      else if (m_hasDisplayMetadata && m_displayMetadata.has_luminance)
      {
        param = log10(100) /
                log10(m_displayMetadata.max_luminance.num / m_displayMetadata.max_luminance.den);
      }

      // Sanity check
      if (param < 0.1f || param > 5.0f)
        param = 0.7f;

      toneP1 = param * m_toneMappingParam;
    }
    else if (m_toneMappingMethod == VS_TONEMAPMETHOD_ACES)
    {
      luminance = CToneMappers::GetLuminanceValue(m_hasDisplayMetadata, m_displayMetadata,
                                                  m_hasLightMetadata, m_lightMetadata);
      toneP1 = m_toneMappingParam;
    }
    else if (m_toneMappingMethod == VS_TONEMAPMETHOD_HABLE)
    {
      luminance = CToneMappers::GetLuminanceValue(m_hasDisplayMetadata, m_displayMetadata,
                                                  m_hasLightMetadata, m_lightMetadata);
      toneP1 = (10000.0f / luminance) * (2.0f / m_toneMappingParam);
    }
  }

  Matrix3x1 coefs = m_convMatrix.GetRGBYuvCoefs(AVColorSpace::AVCOL_SPC_BT709);

  if (m_hVertexBlock >= 0 && m_hFragmentBlock >= 0)
  {
    YuvVertexBlockData vertexBlock;
    std::copy_n(m_proj, 16, vertexBlock.proj.begin());
    std::copy_n(m_model, 16, vertexBlock.model.begin());

    YuvFragmentBlockData fragmentBlock;
    fragmentBlock.stepAlphaField = {stepX, stepY, m_alpha, static_cast<GLfloat>(m_field)};
    fragmentBlock.gamma = {gammaSrc, gammaDstInv, toneP1, luminance};
    fragmentBlock.coefsDst = {coefs[0], coefs[1], coefs[2], 0.0f};
    std::copy_n(yuvMat.ToRaw(), 16, fragmentBlock.yuvMat.begin());
    const float* primMatRaw = primMat.ToRaw();
    fragmentBlock.primMat = {primMatRaw[0], primMatRaw[1], primMatRaw[2], 0.0f,
                 primMatRaw[3], primMatRaw[4], primMatRaw[5], 0.0f,
                 primMatRaw[6], primMatRaw[7], primMatRaw[8], 0.0f,
                 0.0f,         0.0f,         0.0f,         1.0f};

    EnsureUniformBuffer(m_vertexUBO, sizeof(YuvVertexBlockData));
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(YuvVertexBlockData), &vertexBlock);
    glBindBufferBase(GL_UNIFORM_BUFFER, YUV_VERTEX_BINDING_POINT, m_vertexUBO);

    EnsureUniformBuffer(m_fragmentUBO, sizeof(YuvFragmentBlockData));
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(YuvFragmentBlockData), &fragmentBlock);
    glBindBufferBase(GL_UNIFORM_BUFFER, YUV_FRAGMENT_BINDING_POINT, m_fragmentUBO);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
  }
  else
  {
    glUniform2f(m_hStep, stepX, stepY);
    glUniformMatrix4fv(m_hYuvMat, 1, GL_FALSE, yuvMat.ToRaw());
    glUniformMatrix4fv(m_hProj, 1, GL_FALSE, m_proj);
    glUniformMatrix4fv(m_hModel, 1, GL_FALSE, m_model);
    glUniform1f(m_hAlpha, m_alpha);

    if (m_colorConversion)
    {
      glUniformMatrix3fv(m_hPrimMat, 1, GL_FALSE, primMat.ToRaw());
      glUniform1f(m_hGammaSrc, gammaSrc);
      glUniform1f(m_hGammaDstInv, gammaDstInv);
    }

    if (m_toneMapping)
    {
      if (m_toneMappingMethod == VS_TONEMAPMETHOD_REINHARD)
      {
        glUniform3f(m_hCoefsDst, coefs[0], coefs[1], coefs[2]);
        glUniform1f(m_hToneP1, toneP1);
      }
      else
      {
        glUniform1f(m_hLuminance, luminance);
        glUniform1f(m_hToneP1, toneP1);
      }
    }
  }

  VerifyGLState();

  return true;
}

void BaseYUV2RGBGLSLShader::OnDisabled()
{
}

void BaseYUV2RGBGLSLShader::Free()
{
  if (m_vertexUBO != 0)
  {
    glDeleteBuffers(1, &m_vertexUBO);
    m_vertexUBO = 0;
  }

  if (m_fragmentUBO != 0)
  {
    glDeleteBuffers(1, &m_fragmentUBO);
    m_fragmentUBO = 0;
  }

  m_hVertexBlock = -1;
  m_hFragmentBlock = -1;
}

void BaseYUV2RGBGLSLShader::SetColParams(AVColorSpace colSpace, int bits, bool limited,
                                        int textureBits)
{
  if (colSpace == AVCOL_SPC_UNSPECIFIED)
  {
    if (m_width > 1024 || m_height >= 600)
      colSpace = AVCOL_SPC_BT709;
    else
      colSpace = AVCOL_SPC_BT470BG;
  }

  m_convMatrix.SetSourceColorSpace(colSpace)
      .SetSourceBitDepth(bits)
      .SetSourceLimitedRange(limited)
      .SetSourceTextureBitDepth(textureBits);
}

void BaseYUV2RGBGLSLShader::SetDisplayMetadata(bool hasDisplayMetadata,
                                               const AVMasteringDisplayMetadata& displayMetadata,
                                               bool hasLightMetadata,
                                               AVContentLightMetadata lightMetadata)
{
  m_hasDisplayMetadata = hasDisplayMetadata;
  m_displayMetadata = displayMetadata;
  m_hasLightMetadata = hasLightMetadata;
  m_lightMetadata = lightMetadata;
}

//////////////////////////////////////////////////////////////////////
// YUV2RGBProgressiveShader - YUV2RGB with no deinterlacing
// Use for weave deinterlacing / progressive
//////////////////////////////////////////////////////////////////////

YUV2RGBProgressiveShader::YUV2RGBProgressiveShader(EShaderFormat format,
                                                   AVColorPrimaries dstPrimaries,
                                                   AVColorPrimaries srcPrimaries,
                                                   bool toneMap,
                                                   ETONEMAPMETHOD toneMapMethod)
  : BaseYUV2RGBGLSLShader(format, dstPrimaries, srcPrimaries, toneMap, toneMapMethod)
{
  PixelShader()->LoadSource("gles_yuv2rgb_basic.frag", m_defines);
  PixelShader()->InsertSource("gles_tonemap.frag", "void main()");
}


//////////////////////////////////////////////////////////////////////
// YUV2RGBBobShader - YUV2RGB with Bob deinterlacing
//////////////////////////////////////////////////////////////////////

YUV2RGBBobShader::YUV2RGBBobShader(EShaderFormat format,
                                   AVColorPrimaries dstPrimaries,
                                   AVColorPrimaries srcPrimaries,
                                   bool toneMap,
                                   ETONEMAPMETHOD toneMapMethod)
  : BaseYUV2RGBGLSLShader(format, dstPrimaries, srcPrimaries, toneMap, toneMapMethod)
{
  PixelShader()->LoadSource("gles_yuv2rgb_bob.frag", m_defines);
  PixelShader()->InsertSource("gles_tonemap.frag", "void main()");
}

void YUV2RGBBobShader::OnCompiledAndLinked()
{
  BaseYUV2RGBGLSLShader::OnCompiledAndLinked();
  if (m_hFragmentBlock < 0)
  {
    m_hStepX = glGetUniformLocation(ProgramHandle(), "m_stepX");
    m_hStepY = glGetUniformLocation(ProgramHandle(), "m_stepY");
    m_hField = glGetUniformLocation(ProgramHandle(), "m_field");
  }
  VerifyGLState();
}

bool YUV2RGBBobShader::OnEnabled()
{
  if(!BaseYUV2RGBGLSLShader::OnEnabled())
    return false;

  if (m_hFragmentBlock < 0)
  {
    glUniform1i(m_hField, m_field);
    glUniform1f(m_hStepX, 1.0f / static_cast<float>(m_width));
    glUniform1f(m_hStepY, 1.0f / static_cast<float>(m_height));
  }

  VerifyGLState();
  return true;
}

//------------------------------------------------------------------------------
// YUV2RGBFilterShader
//------------------------------------------------------------------------------

YUV2RGBFilterShader::YUV2RGBFilterShader(EShaderFormat format,
                                         AVColorPrimaries dstPrimaries,
                                         AVColorPrimaries srcPrimaries,
                                         bool toneMap,
                                         ETONEMAPMETHOD toneMapMethod,
                                         ESCALINGMETHOD method)
  : BaseYUV2RGBGLSLShader(format, dstPrimaries, srcPrimaries, toneMap, toneMapMethod)
{
  m_scaling = method;
  PixelShader()->LoadSource("gles310_yuv2rgb_filter.frag", m_defines);
  VertexShader()->LoadSource("gles310_yuv2rgb.vert");
  PixelShader()->AppendSource("gl_output.glsl");

  PixelShader()->InsertSource("gl_tonemap.glsl", "void main()");
}

YUV2RGBFilterShader::~YUV2RGBFilterShader()
{
  if (m_kernelTex)
    glDeleteTextures(1, &m_kernelTex);
  m_kernelTex = 0;
}

void YUV2RGBFilterShader::OnCompiledAndLinked()
{
  BaseYUV2RGBGLSLShader::OnCompiledAndLinked();
  m_hKernTex = glGetUniformLocation(ProgramHandle(), "m_kernelTex");

  if (m_scaling != VS_SCALINGMETHOD_LANCZOS3_FAST && m_scaling != VS_SCALINGMETHOD_SPLINE36_FAST)
    m_scaling = VS_SCALINGMETHOD_LANCZOS3_FAST;

  CConvolutionKernel kernel(m_scaling, 256);

  if (m_kernelTex)
  {
    glDeleteTextures(1, &m_kernelTex);
    m_kernelTex = 0;
  }
  glGenTextures(1, &m_kernelTex);

  glActiveTexture(GL_TEXTURE3);
  glBindTexture(GL_TEXTURE_2D, m_kernelTex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);

  GLvoid* data = (GLvoid*)kernel.GetFloatPixels();
#if defined(GL_ES_VERSION_3_0)
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, kernel.GetSize(), 1, 0, GL_RGBA, GL_FLOAT, data);
#endif
  glActiveTexture(GL_TEXTURE0);
  VerifyGLState();
}

bool YUV2RGBFilterShader::OnEnabled()
{
  glActiveTexture(GL_TEXTURE3);
  glBindTexture(GL_TEXTURE_2D, m_kernelTex);
  glUniform1i(m_hKernTex, 3);
  glActiveTexture(GL_TEXTURE0);

  return BaseYUV2RGBGLSLShader::OnEnabled();
}
