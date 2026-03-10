/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "Shader.h"

#include "ServiceBroker.h"
#include "filesystem/Directory.h"
#include "filesystem/File.h"
#include "rendering/RenderSystem.h"
#include "utils/Crc32.h"
#include "utils/GLUtils.h"
#include "utils/URIUtils.h"
#include "utils/StringUtils.h"
#include "utils/log.h"

#include <cstring>
#include <mutex>
#include <unordered_map>

#ifdef HAS_GLES
#define GLchar char
#endif

#define LOG_SIZE 1024

using namespace Shaders;
using namespace XFILE;

namespace
{
using ShaderSourceCache = std::unordered_map<std::string, std::string>;

struct ShaderSourceCacheState
{
  std::mutex mutex;
  ShaderSourceCache cache;
};

ShaderSourceCacheState& GetShaderSourceCacheState()
{
  static ShaderSourceCacheState state;
  return state;
}

bool LoadShaderSourceFile(const std::string& filename, std::string& path, std::string& source)
{
  const auto renderSystem = CServiceBroker::GetRenderSystem();
  if (!renderSystem)
    return false;

  path = "special://xbmc/system/shaders/";
  path += renderSystem->GetShaderPath(filename);
  path += filename;

  {
    auto& cacheState = GetShaderSourceCacheState();
    std::scoped_lock lock(cacheState.mutex);
    const auto it = cacheState.cache.find(path);
    if (it != cacheState.cache.cend())
    {
      source = it->second;
      return true;
    }
  }

  CFileStream file;
  if (!file.Open(path))
    return false;

  getline(file, source, '\0');

  {
    auto& cacheState = GetShaderSourceCacheState();
    std::scoped_lock lock(cacheState.mutex);
    cacheState.cache.emplace(path, source);
  }

  return true;
}

#if defined(HAS_GLES) && HAS_GLES == 3
constexpr uint32_t SHADER_BINARY_CACHE_MAGIC = 0x4B534233;
constexpr uint32_t SHADER_BINARY_CACHE_VERSION = 1;

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

uint32_t ComputeShaderCacheKey(const CShader& vertexShader, const CShader& pixelShader)
{
  Crc32 crc;
  const auto renderSystem = CServiceBroker::GetRenderSystem();

  const auto addString = [&crc](const std::string& value) {
    crc.Compute(value.data(), value.size());
  };

  addString(vertexShader.GetName());
  addString(vertexShader.GetSource());
  addString(pixelShader.GetName());
  addString(pixelShader.GetSource());

  if (renderSystem)
  {
    addString(renderSystem->GetRenderVendor());
    addString(renderSystem->GetRenderRenderer());
    addString(renderSystem->GetRenderVersionString());
  }

  return crc;
}

std::string GetShaderBinaryCachePath(const CShader& vertexShader, const CShader& pixelShader)
{
  constexpr auto cacheDir = "special://profile/cache/shaders";
  CDirectory::Create(cacheDir);
  return URIUtils::AddFileToFolder(
      cacheDir, StringUtils::Format("gles-program-{:08x}.bin", ComputeShaderCacheKey(vertexShader, pixelShader)));
}

bool LoadProgramBinaryCache(GLuint program, const CShader& vertexShader, const CShader& pixelShader)
{
  std::vector<uint8_t> fileData;
  CFile file;
  const std::string cachePath = GetShaderBinaryCachePath(vertexShader, pixelShader);

  if (file.LoadFile(cachePath, fileData) <= static_cast<ssize_t>(sizeof(ShaderBinaryCacheHeader)))
    return false;

  ShaderBinaryCacheHeader header;
  std::memcpy(&header, fileData.data(), sizeof(header));
  if (header.magic != SHADER_BINARY_CACHE_MAGIC || header.version != SHADER_BINARY_CACHE_VERSION ||
      header.length == 0 || fileData.size() != sizeof(header) + header.length)
  {
    CFile::Delete(cachePath);
    return false;
  }

  glProgramBinary(program, header.format, fileData.data() + sizeof(header), header.length);

  GLint linked{GL_FALSE};
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (linked != GL_TRUE)
  {
    CLog::Log(LOGDEBUG, "GL: Program binary cache rejected for {} {}", vertexShader.GetName(),
              pixelShader.GetName());
    CFile::Delete(cachePath);
    return false;
  }

  CLog::Log(LOGDEBUG, "GL: Loaded program binary cache for {} {}", vertexShader.GetName(),
            pixelShader.GetName());
  return true;
}

void SaveProgramBinaryCache(GLuint program, const CShader& vertexShader, const CShader& pixelShader)
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

  ShaderBinaryCacheHeader header{SHADER_BINARY_CACHE_MAGIC, SHADER_BINARY_CACHE_VERSION,
                                 static_cast<uint32_t>(binaryFormat), static_cast<uint32_t>(actualLength)};

  CFile file;
  const std::string cachePath = GetShaderBinaryCachePath(vertexShader, pixelShader);
  if (!file.OpenForWrite(cachePath, true))
    return;

  if (file.Write(&header, sizeof(header)) != static_cast<ssize_t>(sizeof(header)) ||
      file.Write(binary.data(), actualLength) != actualLength)
  {
    CLog::Log(LOGWARNING, "GL: Failed to write shader program binary cache {}", cachePath);
    file.Close();
    CFile::Delete(cachePath);
    return;
  }

  file.Close();
}
#endif
} // namespace

//////////////////////////////////////////////////////////////////////
// CShader
//////////////////////////////////////////////////////////////////////
bool CShader::LoadSource(const std::string& filename, const std::string& prefix)
{
  if(filename.empty())
    return true;

  std::string path;
  if (!LoadShaderSourceFile(filename, path, m_source))
  {
    CLog::Log(LOGERROR, "CYUVShaderGLSL::CYUVShaderGLSL - failed to open file {}", filename);
    return false;
  }

  size_t pos = 0;
  size_t versionPos = m_source.find("#version");
  if (versionPos != std::string::npos)
  {
    versionPos = m_source.find('\n', versionPos);
    if (versionPos != std::string::npos)
      pos = versionPos + 1;
  }
  m_source.insert(pos, prefix);

  m_filenames = filename;

  return true;
}

bool CShader::AppendSource(const std::string& filename)
{
  if(filename.empty())
    return true;

  std::string temp;
  std::string path;
  if (!LoadShaderSourceFile(filename, path, temp))
  {
    CLog::Log(LOGERROR, "CShader::AppendSource - failed to open file {}", filename);
    return false;
  }
  m_source.append(temp);

  m_filenames.append(" " + filename);

  return true;
}

bool CShader::InsertSource(const std::string& filename, const std::string& loc)
{
  if(filename.empty())
    return true;

  std::string temp;
  std::string path;
  if (!LoadShaderSourceFile(filename, path, temp))
  {
    CLog::Log(LOGERROR, "CShader::InsertSource - failed to open file {}", filename);
    return false;
  }

  size_t locPos = m_source.find(loc);
  if (locPos == std::string::npos)
  {
    CLog::Log(LOGERROR, "CShader::InsertSource - could not find location {}", loc);
    return false;
  }

  m_source.insert(locPos, temp);

  m_filenames.append(" " + filename);

  return true;
}

std::string CShader::GetSourceWithLineNumbers() const
{
  int i{1};
  auto lines = StringUtils::Split(m_source, "\n");
  for (auto& line : lines)
  {
    line.insert(0, StringUtils::Format("{:3}: ", i));
    i++;
  }

  auto output = StringUtils::Join(lines, "\n");

  return output;
}


//////////////////////////////////////////////////////////////////////
// CGLSLVertexShader
//////////////////////////////////////////////////////////////////////

bool CGLSLVertexShader::Compile()
{
  GLint params[4];

  Free();

  m_vertexShader = glCreateShader(GL_VERTEX_SHADER);
  const char *ptr = m_source.c_str();
  glShaderSource(m_vertexShader, 1, &ptr, 0);
  glCompileShader(m_vertexShader);
  glGetShaderiv(m_vertexShader, GL_COMPILE_STATUS, params);
  VerifyGLState();
  if (params[0] != GL_TRUE)
  {
    GLchar log[LOG_SIZE];
    CLog::Log(LOGERROR, "GL: Error compiling vertex shader");
    GLsizei length;
    glGetShaderInfoLog(m_vertexShader, LOG_SIZE, &length, log);
    if (length > 0)
    {
      CLog::Log(LOGERROR, "GL: Vertex Shader compilation log:");
      CLog::Log(LOGERROR, "{}", log);
    }
    m_lastLog = log;
    m_compiled = false;
  }
  else
  {
    GLchar log[LOG_SIZE];
    GLsizei length;
    glGetShaderInfoLog(m_vertexShader, LOG_SIZE, &length, log);
    if (length > 0)
    {
      CLog::Log(LOGERROR, "GL: Vertex Shader compilation log:");
      CLog::Log(LOGERROR, "{}", log);
    }
    m_lastLog = log;
    m_compiled = true;
  }
  return m_compiled;
}

void CGLSLVertexShader::Free()
{
  if (m_vertexShader)
    glDeleteShader(m_vertexShader);
  m_vertexShader = 0;
}

//////////////////////////////////////////////////////////////////////
// CGLSLPixelShader
//////////////////////////////////////////////////////////////////////
bool CGLSLPixelShader::Compile()
{
  GLint params[4];

  Free();

  // Pixel shaders are not mandatory.
  if (m_source.empty())
  {
    CLog::Log(LOGINFO, "GL: No pixel shader, fixed pipeline in use");
    return true;
  }

  m_pixelShader = glCreateShader(GL_FRAGMENT_SHADER);
  const char *ptr = m_source.c_str();
  glShaderSource(m_pixelShader, 1, &ptr, 0);
  glCompileShader(m_pixelShader);
  glGetShaderiv(m_pixelShader, GL_COMPILE_STATUS, params);
  if (params[0] != GL_TRUE)
  {
    GLchar log[LOG_SIZE];
    CLog::Log(LOGERROR, "GL: Error compiling pixel shader");
    GLsizei length;
    glGetShaderInfoLog(m_pixelShader, LOG_SIZE, &length, log);
    if (length > 0)
    {
      CLog::Log(LOGERROR, "GL: Pixel Shader compilation log:");
      CLog::Log(LOGERROR, "{}", log);
    }
    m_lastLog = log;
    m_compiled = false;
  }
  else
  {
    GLchar log[LOG_SIZE];
    GLsizei length;
    glGetShaderInfoLog(m_pixelShader, LOG_SIZE, &length, log);
    if (length > 0)
    {
      CLog::Log(LOGERROR, "GL: Pixel Shader compilation log:");
      CLog::Log(LOGERROR, "{}", log);
    }
    m_lastLog = log;
    m_compiled = true;
  }
  return m_compiled;
}

void CGLSLPixelShader::Free()
{
  if (m_pixelShader)
    glDeleteShader(m_pixelShader);
  m_pixelShader = 0;
}

//////////////////////////////////////////////////////////////////////
// CGLSLShaderProgram
//////////////////////////////////////////////////////////////////////
CGLSLShaderProgram::CGLSLShaderProgram()
{
  m_pFP = new CGLSLPixelShader();
  m_pVP = new CGLSLVertexShader();
}

CGLSLShaderProgram::CGLSLShaderProgram(const std::string& vert,
                                       const std::string& frag)
{
  m_pFP = new CGLSLPixelShader();
  m_pFP->LoadSource(frag);
  m_pVP = new CGLSLVertexShader();
  m_pVP->LoadSource(vert);
}

CGLSLShaderProgram::~CGLSLShaderProgram()
{
  Free();
}

void CGLSLShaderProgram::Free()
{
  m_pVP->Free();
  VerifyGLState();
  m_pFP->Free();
  VerifyGLState();
  if (m_shaderProgram)
  {
    glDeleteProgram(m_shaderProgram);
  }
  m_shaderProgram = 0;
  m_ok = false;
  m_lastProgram = 0;
}

bool CGLSLShaderProgram::CompileAndLink()
{
  GLint params[4];

  // free resources
  Free();

#if defined(HAS_GLES) && HAS_GLES == 3
  if (SupportsProgramBinaryCache())
  {
    if ((m_shaderProgram = glCreateProgram()) != 0)
    {
      if (LoadProgramBinaryCache(m_shaderProgram, *m_pVP, *m_pFP))
      {
        m_validated = false;
        m_ok = true;
        OnCompiledAndLinked();
        VerifyGLState();
        return true;
      }

      glDeleteProgram(m_shaderProgram);
      m_shaderProgram = 0;
    }
  }
#endif

  // compiled vertex shader
  if (!m_pVP->Compile())
  {
    CLog::Log(LOGERROR, "GL: Error compiling vertex shader: {}", m_pVP->GetName());
    CLog::Log(LOGDEBUG, "GL: vertex shader source:\n{}", m_pVP->GetSourceWithLineNumbers());
    return false;
  }

  // compile pixel shader
  if (!m_pFP->Compile())
  {
    m_pVP->Free();
    CLog::Log(LOGERROR, "GL: Error compiling fragment shader: {}", m_pFP->GetName());
    CLog::Log(LOGDEBUG, "GL: fragment shader source:\n{}", m_pFP->GetSourceWithLineNumbers());
    return false;
  }

  // create program object
  if (!(m_shaderProgram = glCreateProgram()))
  {
    CLog::Log(LOGERROR, "GL: Error creating shader program handle");
    goto error;
  }

#if defined(HAS_GLES) && HAS_GLES == 3
  if (SupportsProgramBinaryCache())
    glProgramParameteri(m_shaderProgram, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
#endif

  // attach the vertex shader
  glAttachShader(m_shaderProgram, m_pVP->Handle());
  VerifyGLState();

  // if we have a pixel shader, attach it. If not, fixed pipeline
  // will be used.
  if (m_pFP->Handle())
  {
    glAttachShader(m_shaderProgram, m_pFP->Handle());
    VerifyGLState();
  }

  // link the program
  glLinkProgram(m_shaderProgram);
  glGetProgramiv(m_shaderProgram, GL_LINK_STATUS, params);
  if (params[0]!=GL_TRUE)
  {
    GLchar log[LOG_SIZE];
    CLog::Log(LOGERROR, "GL: Error linking shader");
    glGetProgramInfoLog(m_shaderProgram, LOG_SIZE, NULL, log);
    CLog::Log(LOGERROR, "{}", log);
    goto error;
  }
  VerifyGLState();

  m_validated = false;
  m_ok = true;

#if defined(HAS_GLES) && HAS_GLES == 3
  if (SupportsProgramBinaryCache())
    SaveProgramBinaryCache(m_shaderProgram, *m_pVP, *m_pFP);
#endif

  OnCompiledAndLinked();
  VerifyGLState();
  return true;

 error:
  m_ok = false;
  Free();
  return false;
}

bool CGLSLShaderProgram::Enable()
{
  if (OK())
  {
    glUseProgram(m_shaderProgram);
    if (OnEnabled())
    {
      if (!m_validated)
      {
        // validate the program
        GLint params[4];
        glValidateProgram(m_shaderProgram);
        glGetProgramiv(m_shaderProgram, GL_VALIDATE_STATUS, params);
        if (params[0]!=GL_TRUE)
        {
          GLchar log[LOG_SIZE];
          CLog::Log(LOGERROR, "GL: Error validating shader");
          glGetProgramInfoLog(m_shaderProgram, LOG_SIZE, NULL, log);
          CLog::Log(LOGERROR, "{}", log);
        }
        m_validated = true;
      }
      VerifyGLState();
      return true;
    }
    else
    {
      glUseProgram(0);
      return false;
    }
    return true;
  }
  return false;
}

void CGLSLShaderProgram::Disable()
{
  if (OK())
  {
    glUseProgram(0);
    OnDisabled();
  }
}
