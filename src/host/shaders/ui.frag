#version 450
// Shader de fragmento genérico: textura 0 por el color de vértice.

layout(set = 0, binding = 0) uniform sampler2D tex0;

layout(push_constant) uniform PC {
    mat4  mvp;
    vec4  vp;
    vec4  vz;
    ivec4 flags;
    vec4  konst;
} pc;

layout(location = 0) in vec2 uv0;
layout(location = 1) in vec4 col;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 c = pc.flags.x != 0 ? texture(tex0, uv0) : vec4(1.0);
    if (pc.flags.y != 0) c *= col;
    if (pc.flags.w != 0) c *= pc.konst;
    outColor = c;
}
