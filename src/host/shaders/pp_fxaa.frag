#version 450
// Suavizado de bordes (FXAA 3.11, calidad media) a la resolución de la imagen
// del juego: el juego no tiene antialiasing y a 2x en pantalla se notan mucho
// los dientes de sierra de las líneas del campo, el bate y las siluetas.

layout(set = 0, binding = 0) uniform sampler2D src;

layout(push_constant) uniform PC {
    vec4 a;   // xy = 1/tamaño de origen, zw = tamaño de origen
    vec4 b;   // x = suavizado (0/1), y = nitidez, z = destino/origen
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;

float luma(vec4 c) { return dot(c.rgb, vec3(0.299, 0.587, 0.114)); }
float L(vec2 p) { return luma(textureLod(src, p, 0.0)); }

const float PASOS[12] = float[](1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);

void main() {
    vec4 cM = textureLod(src, uv, 0.0);
    if (pc.b.x == 0.0) { o = cM; return; }
    vec2 px = pc.a.xy;
    float lM = luma(cM);
    float lS = luma(textureLodOffset(src, uv, 0.0, ivec2(0, 1)));
    float lN = luma(textureLodOffset(src, uv, 0.0, ivec2(0, -1)));
    float lE = luma(textureLodOffset(src, uv, 0.0, ivec2(1, 0)));
    float lW = luma(textureLodOffset(src, uv, 0.0, ivec2(-1, 0)));
    float lMin = min(lM, min(min(lN, lS), min(lE, lW)));
    float lMax = max(lM, max(max(lN, lS), max(lE, lW)));
    float rango = lMax - lMin;
    if (rango < max(0.0312, lMax * 0.125)) { o = cM; return; }

    float lNW = luma(textureLodOffset(src, uv, 0.0, ivec2(-1, -1)));
    float lNE = luma(textureLodOffset(src, uv, 0.0, ivec2(1, -1)));
    float lSW = luma(textureLodOffset(src, uv, 0.0, ivec2(-1, 1)));
    float lSE = luma(textureLodOffset(src, uv, 0.0, ivec2(1, 1)));
    float lNS = lN + lS, lWE = lW + lE;
    float lWc = lNW + lSW, lEc = lNE + lSE, lNc = lNW + lNE, lSc = lSW + lSE;
    float eH = abs(-2.0 * lW + lWc) + abs(-2.0 * lM + lNS) * 2.0 + abs(-2.0 * lE + lEc);
    float eV = abs(-2.0 * lN + lNc) + abs(-2.0 * lM + lWE) * 2.0 + abs(-2.0 * lS + lSc);
    bool hor = eH >= eV;

    float l1 = hor ? lN : lW, l2 = hor ? lS : lE;
    float g1 = l1 - lM, g2 = l2 - lM;
    bool primero = abs(g1) >= abs(g2);
    float gEsc = 0.25 * max(abs(g1), abs(g2));
    float paso = hor ? px.y : px.x;
    float lMed;
    if (primero) { paso = -paso; lMed = 0.5 * (l1 + lM); } else lMed = 0.5 * (l2 + lM);

    vec2 c = uv;
    if (hor) c.y += paso * 0.5; else c.x += paso * 0.5;
    vec2 off = hor ? vec2(px.x, 0.0) : vec2(0.0, px.y);
    vec2 u1 = c - off, u2 = c + off;
    float f1 = L(u1) - lMed, f2 = L(u2) - lMed;
    bool r1 = abs(f1) >= gEsc, r2 = abs(f2) >= gEsc;
    if (!r1) u1 -= off;
    if (!r2) u2 += off;
    for (int i = 2; i < 12 && !(r1 && r2); i++) {
        if (!r1) f1 = L(u1) - lMed;
        if (!r2) f2 = L(u2) - lMed;
        r1 = abs(f1) >= gEsc; r2 = abs(f2) >= gEsc;
        if (!r1) u1 -= off * PASOS[i];
        if (!r2) u2 += off * PASOS[i];
    }
    float d1 = hor ? uv.x - u1.x : uv.y - u1.y;
    float d2 = hor ? u2.x - uv.x : u2.y - uv.y;
    bool dir1 = d1 < d2;
    float dMin = min(d1, d2);
    float desp = -dMin / (d1 + d2) + 0.5;
    bool ok = ((dir1 ? f1 : f2) < 0.0) != (lM < lMed);
    float fin = ok ? desp : 0.0;

    // subpíxel (flojo: el marcador y los textos son nítidos en el original)
    float lProm = (1.0 / 12.0) * (2.0 * (lNS + lWE) + lWc + lEc);
    float s1 = clamp(abs(lProm - lM) / rango, 0.0, 1.0);
    float s2 = (-2.0 * s1 + 3.0) * s1 * s1;
    fin = max(fin, s2 * s2 * 0.5);

    vec2 uf = uv;
    if (hor) uf.y += fin * paso; else uf.x += fin * paso;
    o = vec4(textureLod(src, uf, 0.0).rgb, 1.0);
}
