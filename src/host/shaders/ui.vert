#version 450
// Shader de vértice genérico para los programas GXP del menú (Sprite,
// SimpleTextured, Simple...): posición por la MVP del juego, viewport de GXM
// (de NDC a píxeles) y de ahí a las coordenadas de Vulkan del render target.

layout(location = 0) in vec4 inPos;
layout(location = 1) in vec2 inUV0;
layout(location = 2) in vec2 inUV1;
layout(location = 3) in vec4 inCol;

layout(push_constant) uniform PC {
    mat4  mvp;
    vec4  vp;      // xOffset, xScale, yOffset, yScale
    vec4  vz;      // zOffset, zScale, ancho, alto del destino
    ivec4 flags;   // x usa textura, y usa color, z viewport activo, w color constante
    vec4  konst;   // color constante (ClearColor...)
} pc;

layout(location = 0) out vec2 uv0;
layout(location = 1) out vec4 col;

void main() {
    vec4 c = pc.mvp * inPos;
    vec2 size = pc.vz.zw;
    if (pc.flags.z != 0) {
        vec2 ndc = c.xy / c.w;
        vec2 px = vec2(pc.vp.x + pc.vp.y * ndc.x, pc.vp.z + pc.vp.w * ndc.y);
        float z = pc.vz.x + pc.vz.y * (c.z / c.w);
        gl_Position = vec4((px / size * 2.0 - 1.0) * c.w, z * c.w, c.w);
    } else {
        // Sin viewport la posición ya viene en píxeles.
        gl_Position = vec4(c.xy / size * 2.0 - 1.0, clamp(c.z, 0.0, 1.0), 1.0);
    }
    uv0 = inUV0;
    col = inCol;
}
