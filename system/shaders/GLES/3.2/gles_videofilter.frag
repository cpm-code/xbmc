#version 320 es

precision mediump float;

uniform sampler2D img;
in vec2 cord;

layout(std140) uniform KodiVideoFilterVertexBlock
{
  mat4 uProj;
  mat4 uModel;
  vec4 uAlpha;
};

out vec4 fragColor;

void main()
{
  vec4 color = texture(img, cord);
  color.a *= uAlpha.x;
  fragColor = color;
}
