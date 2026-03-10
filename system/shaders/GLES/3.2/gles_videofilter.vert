#version 320 es

layout(location = 0) in vec4 m_attrpos;
layout(location = 1) in vec2 m_attrcord;
out vec2 cord;

layout(std140) uniform KodiVideoFilterVertexBlock
{
  mat4 uProj;
  mat4 uModel;
  vec4 uAlpha;
};

void main()
{
  mat4 mvp = uProj * uModel;
  gl_Position = mvp * m_attrpos;
  cord = m_attrcord.xy;
}
