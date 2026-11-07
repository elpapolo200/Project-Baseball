#version 450
// Escalado al tamaño de la ventana: bicúbico Catmull-Rom (9 lecturas
// bilineales) al ampliar, bilineal al reducir, y nitidez adaptativa al
// contraste (como AMD CAS) para compensar la blandura del escalado.

layout(set = 0, binding = 0) uniform sampler2D src;

layout(push_constant) uniform PC {
    vec4 a;   // xy = 1/tamaño de origen, zw = tamaño de origen
    vec4 b;   // x = suavizado, y = nitidez (0..1), z = destino/origen
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;

vec3 T(vec2 p) { return textureLod(src, p, 0.0).rgb; }

vec3 catmull_rom(vec2 p) {
    vec2 sz = pc.a.zw, inv = pc.a.xy;
    vec2 sp = p * sz;
    vec2 t1 = floor(sp - 0.5) + 0.5;
    vec2 f = sp - t1;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 t0 = (t1 - 1.0) * inv, t3 = (t1 + 2.0) * inv, t12 = (t1 + w2 / w12) * inv;
    vec3 r = T(vec2(t0.x, t0.y)) * w0.x * w0.y + T(vec2(t12.x, t0.y)) * w12.x * w0.y + T(vec2(t3.x, t0.y)) * w3.x * w0.y
           + T(vec2(t0.x, t12.y)) * w0.x * w12.y + T(vec2(t12.x, t12.y)) * w12.x * w12.y + T(vec2(t3.x, t12.y)) * w3.x * w12.y
           + T(vec2(t0.x, t3.y)) * w0.x * w3.y + T(vec2(t12.x, t3.y)) * w12.x * w3.y + T(vec2(t3.x, t3.y)) * w3.x * w3.y;
    return clamp(r, 0.0, 1.0);
}

void main() {
    vec3 c = pc.b.z > 1.0 ? catmull_rom(uv) : T(uv);
    float k = pc.b.y;
    if (k > 0.0) {
        vec2 px = pc.a.xy;
        vec3 n = T(uv + vec2(0.0, -px.y)), s = T(uv + vec2(0.0, px.y));
        vec3 e = T(uv + vec2(px.x, 0.0)), w = T(uv + vec2(-px.x, 0.0));
        vec3 mn = min(c, min(min(n, s), min(e, w)));
        vec3 mx = max(c, max(max(n, s), max(e, w)));
        vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, 1e-4), 0.0, 1.0));
        vec3 wt = amp * (-1.0 / mix(8.0, 5.0, k));
        c = clamp((c + (n + s + e + w) * wt) / (1.0 + 4.0 * wt), 0.0, 1.0);
    }
    o = vec4(c, 1.0);
}
