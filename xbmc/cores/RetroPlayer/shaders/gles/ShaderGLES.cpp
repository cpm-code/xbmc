/*
 *  Copyright (C) 2019-2025 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "ShaderGLES.h"

#include "ServiceBroker.h"
#include "ShaderTextureGLES.h"
#include "ShaderUtilsGLES.h"
#include "application/Application.h"
#include "cores/RetroPlayer/rendering/RenderContext.h"
#include "cores/RetroPlayer/shaders/IShaderLut.h"
#include "cores/RetroPlayer/shaders/ShaderUtils.h"
#include "filesystem/Directory.h"
#include "filesystem/File.h"
#include "rendering/RenderSystem.h"
#include "rendering/gl/RenderSystemGL.h"
#include "utils/Crc32.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/log.h"

#include <cstring>

using namespace KODI::SHADER;

namespace
{
#if defined(HAS_GLES) && HAS_GLES == 3
constexpr uint32_t RETRO_PLAYER_SHADER_BINARY_CACHE_MAGIC = 0x4B535233;
constexpr uint32_t RETRO_PLAYER_SHADER_BINARY_CACHE_VERSION = 1;

struct ShaderBinaryCacheHeader
{
  uint32_t magic;
  uint32_t version;
  uint32_t format;
  uint32_t length;
};

bool SupportsProgramBinaryCache()
{
  const auto renderSystem = CServiceBroker::GetRenderSystem();
  if (!renderSystem)
    return false;

  unsigned int major{0};
  unsigned int minor{0};
  renderSystem->GetRenderVersion(major, minor);
  if (major < 3)
    return false;

  GLint numBinaryFormats{0};
  glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &numBinaryFormats);
  return numBinaryFormats > 0;
}

uint32_t ComputeShaderCacheKey(const std::string& shaderPath,
                               const std::string& vertexShaderSource,
                               const std::string& fragmentShaderSource)
{
  Crc32 crc;
  const auto renderSystem = CServiceBroker::GetRenderSystem();

  const auto addString = [&crc](const std::string& value) {
    crc.Compute(value.data(), value.size());
  };

  addString(shaderPath);
  addString(vertexShaderSource);
  addString(fragmentShaderSource);

  if (renderSystem)
  {
    addString(renderSystem->GetRenderVendor());
    addString(renderSystem->GetRenderRenderer());
    addString(renderSystem->GetRenderVersionString());
  }

  return crc;
}

std::string GetShaderBinaryCachePath(const std::string& shaderPath,
                                     const std::string& vertexShaderSource,
                                     const std::string& fragmentShaderSource)
{
  constexpr auto cacheDir = "special://profile/cache/shaders";
  XFILE::CDirectory::Create(cacheDir);
  return URIUtils::AddFileToFolder(
      cacheDir,
      StringUtils::Format("gles-retroplayer-program-{:08x}.bin",
                          ComputeShaderCacheKey(shaderPath, vertexShaderSource, fragmentShaderSource)));
}

bool LoadProgramBinaryCache(GLuint program,
                            const std::string& shaderPath,
                            const std::string& vertexShaderSource,
                            const std::string& fragmentShaderSource)
{
  std::vector<uint8_t> fileData;
  XFILE::CFile file;
  const std::string cachePath =
      GetShaderBinaryCachePath(shaderPath, vertexShaderSource, fragmentShaderSource);

  if (file.LoadFile(cachePath, fileData) <= static_cast<ssize_t>(sizeof(ShaderBinaryCacheHeader)))
    return false;

  ShaderBinaryCacheHeader header;
  std::memcpy(&header, fileData.data(), sizeof(header));
  if (header.magic != RETRO_PLAYER_SHADER_BINARY_CACHE_MAGIC ||
      header.version != RETRO_PLAYER_SHADER_BINARY_CACHE_VERSION || header.length == 0 ||
      fileData.size() != sizeof(header) + header.length)
  {
    XFILE::CFile::Delete(cachePath);
    return false;
  }

  glProgramBinary(program, header.format, fileData.data() + sizeof(header), header.length);

  GLint linked{GL_FALSE};
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (linked != GL_TRUE)
  {
    CLog::Log(LOGDEBUG, "CShaderGLES::Create: Program binary cache rejected for {}", shaderPath);
    XFILE::CFile::Delete(cachePath);
    return false;
  }

  CLog::Log(LOGDEBUG, "CShaderGLES::Create: Loaded program binary cache for {}", shaderPath);
  return true;
}

void SaveProgramBinaryCache(GLuint program,
                            const std::string& shaderPath,
                            const std::string& vertexShaderSource,
                            const std::string& fragmentShaderSource)
{
  GLint binaryLength{0};
  glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &binaryLength);
  if (binaryLength <= 0)
    return;

  std::vector<uint8_t> binary(binaryLength);
  GLenum binaryFormat{0};
  GLsizei actualLength{0};
  glGetProgramBinary(program, binaryLength, &actualLength, &binaryFormat, binary.data());
  if (actualLength <= 0)
    return;

  ShaderBinaryCacheHeader header{RETRO_PLAYER_SHADER_BINARY_CACHE_MAGIC,
                                 RETRO_PLAYER_SHADER_BINARY_CACHE_VERSION,
                                 static_cast<uint32_t>(binaryFormat),
                                 static_cast<uint32_t>(actualLength)};

  XFILE::CFile file;
  const std::string cachePath =
      GetShaderBinaryCachePath(shaderPath, vertexShaderSource, fragmentShaderSource);
  if (!file.OpenForWrite(cachePath, true))
    return;

  if (file.Write(&header, sizeof(header)) != static_cast<ssize_t>(sizeof(header)) ||
      file.Write(binary.data(), actualLength) != actualLength)
  {
    CLog::Log(LOGWARNING, "CShaderGLES::Create: Failed to write shader program binary cache {}",
              cachePath);
    file.Close();
    XFILE::CFile::Delete(cachePath);
    return;
  }

  file.Close();
}
#endif
} // namespace

CShaderGLES::CShaderGLES() = default;

CShaderGLES::~CShaderGLES()
{
  Destroy();
}

bool CShaderGLES::Create(unsigned int passIdx,
                         std::string passAlias,
                         std::string shaderPath,
                         std::string shaderSource,
                         ShaderParameterMap shaderParameters,
                         std::vector<std::shared_ptr<IShaderLut>> luts,
                         unsigned int frameCountMod)
{
  if (shaderPath.empty())
  {
    CLog::Log(LOGERROR, "CShaderGLES::Create: Can't load empty shader path");
    return false;
  }

  m_passIdx = passIdx;
  m_passAlias = std::move(passAlias);
  m_shaderPath = std::move(shaderPath);
  m_shaderSource = CShaderUtils::StripParameterPragmas(std::move(shaderSource));
  m_shaderParameters = std::move(shaderParameters);
  m_luts = std::move(luts);
  m_frameCountMod = frameCountMod;

  std::string defineVersion = CShaderUtilsGLES::GetGLSLVersion(m_shaderSource);
  std::string defineVertex = "#define VERTEX\n#define PARAMETER_UNIFORM\n";
  std::string defineFragment = "#define FRAGMENT\n#define PARAMETER_UNIFORM\n";
  std::string vertexShaderSourceStr = defineVersion + defineVertex + m_shaderSource;
  std::string fragmentShaderSourceStr = defineVersion + defineFragment + m_shaderSource;
  const GLchar* vertexShaderSource = vertexShaderSourceStr.c_str();
  const GLchar* fragmentShaderSource = fragmentShaderSourceStr.c_str();

  const auto finishProgramSetup = [this]() {
    glUseProgram(m_shaderProgram);

    GetUniformLocs();

    GLint paramLoc = glGetUniformLocation(m_shaderProgram, "Texture");
    glUniform1i(paramLoc, 0);

    const GLubyte idx[4] = {0, 1, 3, 2}; // Determines order of triangle strip

    // Set up VBO
    glGenBuffers(3, m_shaderVertexVBO.data());

    glGenBuffers(1, &m_shaderIndexVBO);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_shaderIndexVBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(GLubyte) * 4, idx, GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glUseProgram(0);
  };

#if defined(HAS_GLES) && HAS_GLES == 3
  const bool useProgramBinaryCache = SupportsProgramBinaryCache();
  if (useProgramBinaryCache)
  {
    m_shaderProgram = glCreateProgram();
    if (m_shaderProgram != 0)
    {
      if (LoadProgramBinaryCache(m_shaderProgram, m_shaderPath, vertexShaderSourceStr,
                                 fragmentShaderSourceStr))
      {
        finishProgramSetup();
        return true;
      }

      glDeleteProgram(m_shaderProgram);
      m_shaderProgram = 0;
    }
  }
#endif

  m_shaderProgram = glCreateProgram();

  GLint status;
  GLuint vShader;
  GLuint fShader;

  vShader = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(vShader, 1, &vertexShaderSource, nullptr);
  glCompileShader(vShader);
  glGetShaderiv(vShader, GL_COMPILE_STATUS, &status);

  if (status == GL_FALSE)
  {
    GLint maxLength = 0;
    glGetShaderiv(vShader, GL_INFO_LOG_LENGTH, &maxLength);
    std::vector<GLchar> errorLog(maxLength);
    glGetShaderInfoLog(vShader, maxLength, &maxLength, errorLog.data());
    CLog::Log(LOGERROR, "CShaderGLES::Create: Vertex shader compile error:\n{}",
              std::string(errorLog.begin(), errorLog.end()));
  }

  fShader = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(fShader, 1, &fragmentShaderSource, nullptr);
  glCompileShader(fShader);
  glGetShaderiv(fShader, GL_COMPILE_STATUS, &status);

  if (status == GL_FALSE)
  {
    GLint maxLength = 0;
    glGetShaderiv(fShader, GL_INFO_LOG_LENGTH, &maxLength);
    std::vector<GLchar> errorLog(maxLength);
    glGetShaderInfoLog(fShader, maxLength, &maxLength, errorLog.data());
    CLog::Log(LOGERROR, "CShaderGLES::Create: Fragment shader compile error:\n{}",
              std::string(errorLog.begin(), errorLog.end()));
  }

  glAttachShader(m_shaderProgram, vShader);
  glAttachShader(m_shaderProgram, fShader);

  glBindAttribLocation(m_shaderProgram, 0, "VertexCoord");
  glBindAttribLocation(m_shaderProgram, 1, "COLOR");
  glBindAttribLocation(m_shaderProgram, 2, "TexCoord");

#if defined(HAS_GLES) && HAS_GLES == 3
  if (useProgramBinaryCache)
    glProgramParameteri(m_shaderProgram, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
#endif

  glLinkProgram(m_shaderProgram);

  glDeleteShader(vShader);
  glDeleteShader(fShader);

  glGetProgramiv(m_shaderProgram, GL_LINK_STATUS, &status);

  if (status == GL_FALSE)
  {
    GLint maxLength = 0;
    glGetProgramiv(m_shaderProgram, GL_INFO_LOG_LENGTH, &maxLength);
    std::vector<GLchar> errorLog(maxLength);
    glGetProgramInfoLog(m_shaderProgram, maxLength, &maxLength, errorLog.data());
    CLog::Log(LOGERROR, "CShaderGLES::Create: Shader program link error:\n{}",
              std::string(errorLog.begin(), errorLog.end()));
    CLog::Log(LOGERROR, "CShaderGLES::Create: Failed to load video shader: {}", m_shaderPath);
    return false;
  }

#if defined(HAS_GLES) && HAS_GLES == 3
  if (useProgramBinaryCache)
    SaveProgramBinaryCache(m_shaderProgram, m_shaderPath, vertexShaderSourceStr,
                           fragmentShaderSourceStr);
#endif

  finishProgramSetup();
  return true;
}

void CShaderGLES::Render(IShaderTexture& source, IShaderTexture& target)
{
  auto& sourceGL = static_cast<CShaderTextureGLES&>(source);

  glUseProgram(m_shaderProgram);

  SetShaderParameters(sourceGL);

  glBindBuffer(GL_ARRAY_BUFFER, m_shaderVertexVBO[0]);
  glBufferData(GL_ARRAY_BUFFER, sizeof(m_VertexCoords), m_VertexCoords.data(), GL_DYNAMIC_DRAW);

  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
  glEnableVertexAttribArray(0);

  glBindBuffer(GL_ARRAY_BUFFER, m_shaderVertexVBO[1]);
  glBufferData(GL_ARRAY_BUFFER, sizeof(m_colors), m_colors.data(), GL_DYNAMIC_DRAW);

  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
  glEnableVertexAttribArray(1);

  glBindBuffer(GL_ARRAY_BUFFER, m_shaderVertexVBO[2]);
  glBufferData(GL_ARRAY_BUFFER, sizeof(m_TexCoords), m_TexCoords.data(), GL_DYNAMIC_DRAW);

  glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
  glEnableVertexAttribArray(2);

  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_shaderIndexVBO);

  glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_BYTE, nullptr);

  glDisableVertexAttribArray(0);
  glDisableVertexAttribArray(1);
  glDisableVertexAttribArray(2);

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

  glUseProgram(0);
}

void CShaderGLES::SetSizes(const float2& prevSize,
                           const float2& prevTextureSize,
                           const float2& nextSize)
{
  m_inputSize = prevSize;
  m_inputTextureSize = prevTextureSize;
  m_outputSize = nextSize;
}

void CShaderGLES::PrepareParameters(
    const RETRO::ViewportCoordinates& dest,
    const float2 fullDestSize,
    IShaderTexture& sourceTexture,
    const std::vector<std::unique_ptr<IShaderTexture>>& pShaderTextures,
    const std::vector<std::unique_ptr<IShader>>& pShaders,
    uint64_t frameCount)
{
  if (m_passIdx + 1 != pShaders.size()) // Not last pass
  {
    // bottom left x,y
    m_VertexCoords[0][0] = -m_outputSize.x / 2;
    m_VertexCoords[0][1] = -m_outputSize.y / 2;
    // bottom right x,y
    m_VertexCoords[1][0] = m_outputSize.x / 2;
    m_VertexCoords[1][1] = -m_outputSize.y / 2;
    // top right x,y
    m_VertexCoords[2][0] = m_outputSize.x / 2;
    m_VertexCoords[2][1] = m_outputSize.y / 2;
    // top left x,y
    m_VertexCoords[3][0] = -m_outputSize.x / 2;
    m_VertexCoords[3][1] = m_outputSize.y / 2;

    // Set destination rectangle size
    m_destSize = m_outputSize;
  }
  else // Last pass
  {
    // bottom left x,y
    m_VertexCoords[0][0] = dest[3].x - m_outputSize.x / 2;
    m_VertexCoords[0][1] = dest[3].y - m_outputSize.y / 2;
    // bottom right x,y
    m_VertexCoords[1][0] = dest[2].x - m_outputSize.x / 2;
    m_VertexCoords[1][1] = dest[2].y - m_outputSize.y / 2;
    // top right x,y
    m_VertexCoords[2][0] = dest[1].x - m_outputSize.x / 2;
    m_VertexCoords[2][1] = dest[1].y - m_outputSize.y / 2;
    // top left x,y
    m_VertexCoords[3][0] = dest[0].x - m_outputSize.x / 2;
    m_VertexCoords[3][1] = dest[0].y - m_outputSize.y / 2;

    // Set destination rectangle size for the last pass
    m_destSize = fullDestSize;
  }

  // bottom left z, tu, tv, r, g, b
  m_VertexCoords[0][2] = 0;
  m_colors[0][0] = 0.0f;
  m_colors[0][1] = 0.0f;
  m_colors[0][2] = 0.0f;
  m_TexCoords[0][0] = 0.0f;
  m_TexCoords[0][1] = 1.0f;
  // bottom right z, tu, tv, r, g, b
  m_VertexCoords[1][2] = 0;
  m_colors[1][0] = 0.0f;
  m_colors[1][1] = 0.0f;
  m_colors[1][2] = 0.0f;
  m_TexCoords[1][0] = 1.0f;
  m_TexCoords[1][1] = 1.0f;
  // top right z, tu, tv, r, g, b
  m_VertexCoords[2][2] = 0;
  m_colors[2][0] = 0.0f;
  m_colors[2][1] = 0.0f;
  m_colors[2][2] = 0.0f;
  m_TexCoords[2][0] = 1.0f;
  m_TexCoords[2][1] = 0.0f;
  // top left z, tu, tv, r, g, b
  m_VertexCoords[3][2] = 0;
  m_colors[3][0] = 0.0f;
  m_colors[3][1] = 0.0f;
  m_colors[3][2] = 0.0f;
  m_TexCoords[3][0] = 0.0f;
  m_TexCoords[3][1] = 0.0f;

  UpdateUniformInputs(sourceTexture, pShaderTextures, pShaders, frameCount);
}

void CShaderGLES::UpdateMVP()
{
  GLfloat xScale = 1.0f / m_outputSize.x * 2.0f;
  GLfloat yScale = -1.0f / m_outputSize.y * 2.0f;

  // Update projection matrix
  m_MVP = {{{xScale, 0, 0, 0}, {0, yScale, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
}

void CShaderGLES::Destroy()
{
  glDeleteProgram(m_shaderProgram);
  m_shaderProgram = 0;

  glDeleteBuffers(1, &m_shaderIndexVBO);
  glDeleteBuffers(3, m_shaderVertexVBO.data());
  m_shaderIndexVBO = 0;
  m_shaderVertexVBO = {0, 0, 0};
}

void CShaderGLES::UpdateUniformInputs(
    IShaderTexture& sourceTexture,
    const std::vector<std::unique_ptr<IShaderTexture>>& pShaderTextures,
    const std::vector<std::unique_ptr<IShader>>& pShaders,
    uint64_t frameCount)
{
  m_uniformInputs = GetInputData(frameCount);

  if (m_passIdx > 0) // Not first pass
  {
    auto& shaderTextureGL = static_cast<CShaderTextureGLES&>(*pShaderTextures[m_passIdx - 1]);
    m_uniformFrameInputs = GetFrameInputData(shaderTextureGL.GetTextureID());
  }
  else // First pass
  {
    auto& sourceTextureGL = static_cast<CShaderTextureGLES&>(sourceTexture);
    m_uniformFrameInputs = GetFrameInputData(sourceTextureGL.GetTextureID());
  }

  // Set frame uniforms of previous passes
  m_passesUniformFrameInputs.clear();

  for (unsigned int i = 0; i < m_passIdx + 1; ++i)
  {
    const auto& shader = static_cast<const CShaderGLES&>(*pShaders[i]);
    UniformFrameInputs frameInput = shader.GetFrameUniformInputs();
    m_passesUniformFrameInputs.emplace_back(frameInput);
  }
}

CShaderGLES::UniformInputs CShaderGLES::GetInputData(uint64_t frameCount) const
{
  if (m_frameCountMod != 0)
    frameCount %= m_frameCountMod;

  const UniformInputs input = {
      {m_inputSize}, // video_size
      {m_inputTextureSize}, // texture_size
      {m_destSize}, // output_size
      // Current frame count that can be modulo'ed
      static_cast<GLint>(frameCount), // frame_count
      // Time always flows forward
      1 // frame_direction
  };
  return input;
}

CShaderGLES::UniformFrameInputs CShaderGLES::GetFrameInputData(GLuint texture) const
{
  const UniformFrameInputs frameInput = {
      {m_inputSize}, // input_size
      {m_inputTextureSize}, // texture_size
      texture, // texture
      m_passAlias // alias
  };
  return frameInput;
}

void CShaderGLES::GetUniformLocs()
{
  m_FrameDirectionLoc = glGetUniformLocation(m_shaderProgram, "FrameDirection");
  m_FrameCountLoc = glGetUniformLocation(m_shaderProgram, "FrameCount");
  m_OutputSizeLoc = glGetUniformLocation(m_shaderProgram, "OutputSize");
  m_TextureSizeLoc = glGetUniformLocation(m_shaderProgram, "TextureSize");
  m_InputSizeLoc = glGetUniformLocation(m_shaderProgram, "InputSize");
  m_MVPMatrixLoc = glGetUniformLocation(m_shaderProgram, "MVPMatrix");
}

void CShaderGLES::SetShaderParameters(CShaderTextureGLES& sourceTexture)
{
  // Set shader uniforms
  glUniform1i(m_FrameDirectionLoc, m_uniformInputs.frame_direction);
  glUniform1i(m_FrameCountLoc, m_uniformInputs.frame_count);
  glUniform2f(m_OutputSizeLoc, m_uniformInputs.output_size.x, m_uniformInputs.output_size.y);
  glUniform2f(m_TextureSizeLoc, m_uniformInputs.texture_size.x, m_uniformInputs.texture_size.y);
  glUniform2f(m_InputSizeLoc, m_uniformInputs.video_size.x, m_uniformInputs.video_size.y);
  glUniformMatrix4fv(m_MVPMatrixLoc, 1, GL_FALSE, reinterpret_cast<const GLfloat*>(&m_MVP));

  // Set #pragma parameters
  for (const auto& [paramName, paramValue] : m_shaderParameters)
  {
    GLint paramLoc = glGetUniformLocation(m_shaderProgram, paramName.c_str());
    glUniform1f(paramLoc, paramValue);
  }

  // Set source texture
  unsigned int textureUnit = 0;
  sourceTexture.BindToUnit(textureUnit);
  textureUnit++;

  // Regenerate source texture mipmaps
  if (sourceTexture.IsMipmapped())
    glGenerateMipmap(GL_TEXTURE_2D);

  // Set lookup textures
  for (const std::shared_ptr<IShaderLut>& lut : m_luts)
  {
    auto* texture = static_cast<CGLESTexture*>(lut->GetTexture());
    if (texture != nullptr)
    {
      const GLint paramLoc = glGetUniformLocation(m_shaderProgram, lut->GetID().c_str());
      glUniform1i(paramLoc, textureUnit);
      texture->BindToUnit(textureUnit);
      textureUnit++;
    }
  }

  // Set FBO textures
  for (unsigned int i = 0; i < m_passIdx + 1; ++i)
  {
    GLint paramLoc;

    std::string paramPass = i ? "Pass" + std::to_string(i) : "Orig";
    paramLoc = glGetUniformLocation(m_shaderProgram, (paramPass + "InputSize").c_str());
    glUniform2f(paramLoc, m_passesUniformFrameInputs[i].input_size.x,
                m_passesUniformFrameInputs[i].input_size.y);
    paramLoc = glGetUniformLocation(m_shaderProgram, (paramPass + "TextureSize").c_str());
    glUniform2f(paramLoc, m_passesUniformFrameInputs[i].texture_size.x,
                m_passesUniformFrameInputs[i].texture_size.y);
    paramLoc = glGetUniformLocation(m_shaderProgram, (paramPass + "Texture").c_str());
    glUniform1i(paramLoc, textureUnit);

    if (i < m_passIdx)
    {
      std::string paramPassPrev = "PassPrev" + std::to_string(m_passIdx + 1 - i);
      paramLoc = glGetUniformLocation(m_shaderProgram, (paramPassPrev + "InputSize").c_str());
      glUniform2f(paramLoc, m_passesUniformFrameInputs[i].input_size.x,
                  m_passesUniformFrameInputs[i].input_size.y);
      paramLoc = glGetUniformLocation(m_shaderProgram, (paramPassPrev + "TextureSize").c_str());
      glUniform2f(paramLoc, m_passesUniformFrameInputs[i].texture_size.x,
                  m_passesUniformFrameInputs[i].texture_size.y);
      paramLoc = glGetUniformLocation(m_shaderProgram, (paramPassPrev + "Texture").c_str());
      glUniform1i(paramLoc, textureUnit);

      if (i > 0)
      {
        std::string paramAlias = m_passesUniformFrameInputs[i - 1].alias;
        if (!paramAlias.empty())
        {
          paramLoc = glGetUniformLocation(m_shaderProgram, (paramAlias + "InputSize").c_str());
          glUniform2f(paramLoc, m_passesUniformFrameInputs[i].input_size.x,
                      m_passesUniformFrameInputs[i].input_size.y);
          paramLoc = glGetUniformLocation(m_shaderProgram, (paramAlias + "TextureSize").c_str());
          glUniform2f(paramLoc, m_passesUniformFrameInputs[i].texture_size.x,
                      m_passesUniformFrameInputs[i].texture_size.y);
          paramLoc = glGetUniformLocation(m_shaderProgram, (paramAlias + "Texture").c_str());
          glUniform1i(paramLoc, textureUnit);
        }
      }
    }

    glActiveTexture(GL_TEXTURE0 + textureUnit);
    glBindTexture(GL_TEXTURE_2D, m_passesUniformFrameInputs[i].texture);
    textureUnit++;
  }
}
