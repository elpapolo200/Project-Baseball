// Port de MLB15: envoltorio en C del traductor de shaders de Vita3K.
// Copyright (C) 2026 VitaRE. GPLv2 o posterior (como el código de Vita3K que usa).

#include "../../host/v3k_api.h"

#include <gxm/types.h>
#include <shader/gxp_parser.h>
#include <shader/spirv_recompiler.h>
#include <shader/usse_types.h>
#include <util/log.h>
#include <util/overloaded.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <string>
#include <variant>
#include <vector>

static int g_level = -1;
namespace shader::usse {
extern thread_local int v3k_corte_pc, v3k_corte_bank, v3k_corte_num, v3k_corte_f32, v3k_corte_hecho, v3k_corte_dst;
}
using shader::usse::v3k_corte_pc;
using shader::usse::v3k_corte_bank;
using shader::usse::v3k_corte_num;
using shader::usse::v3k_corte_f32;
using shader::usse::v3k_corte_hecho;
using shader::usse::v3k_corte_dst;

int v3k_log_level() {
    if (g_level < 0) {
        const char *e = getenv("MLB15_V3K_LOG");
        g_level = e ? atoi(e) : 1;
    }
    return g_level;
}

void v3k_log(int level, const std::string &msg) {
    static int count;
    if (level > 1 || count < 200) {
        count++;
        printf("[v3k] %s\n", msg.c_str());
    }
}

extern "C" void v3k_free(void *p) { free(p); }

extern "C" int v3k_translate(const void *gxp, size_t len, const V3kHints *h, uint32_t **spv, size_t *words,
                             V3kInfo *info, char *err, size_t errlen) {
    *spv = nullptr;
    *words = 0;
    memset(info, 0, sizeof(*info));
    if (err && errlen) err[0] = 0;
    try {
        const SceGxmProgram &program = *reinterpret_cast<const SceGxmProgram *>(gxp);
        if (len < sizeof(SceGxmProgram) || program.size > len) {
            snprintf(err, errlen, "gxp truncado");
            return 0;
        }
        info->is_frag = program.is_fragment();
        if (info->is_frag) {
            info->frag_color = program.is_frag_color_used() ? 1 : 0;
            info->native_color = program.is_native_color() ? 1 : 0;
            info->output_type = static_cast<uint8_t>(program.get_fragment_output_type());
            info->out_param_type = static_cast<uint8_t>(program.vertex_varyings()->output_param_type);
            info->out_start = static_cast<uint8_t>(program.vertex_varyings()->fragment_output_start);
        }

        std::vector<SceGxmVertexAttribute> attrs;
        shader::Hints hints{};
        hints.attributes = &attrs;
        hints.color_format = SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR;
        for (int i = 0; i < SCE_GXM_MAX_TEXTURE_UNITS; i++) {
            const uint32_t f = h && h->tex_format[i] ? h->tex_format[i] : SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR;
            hints.vertex_textures[i] = static_cast<SceGxmTextureFormat>(f);
            hints.fragment_textures[i] = static_cast<SceGxmTextureFormat>(f);
        }
        if (h) {
            for (uint32_t i = 0; i < h->nattr && i < 16; i++) {
                SceGxmVertexAttribute a{};
                a.streamIndex = 0;
                a.offset = 0;
                a.format = static_cast<SceGxmAttributeFormat>(h->attr[i].format);
                a.componentCount = h->attr[i].comps;
                a.regIndex = h->attr[i].reg;
                attrs.push_back(a);
            }
        }

        FeatureState features{};
        features.support_shader_interlock = false;
        features.support_texture_barrier = false;
        features.direct_fragcolor = false;
        features.spirv_shader = true;
        features.support_unknown_format = false;
        features.support_rgb_attributes = true;
        features.enable_memory_mapping = false;
        features.support_scaled_attribute_formats = true;
        features.raw_u16_location0 = h && h->raw16;
        features.use_texture_viewport = false;

        // Entradas (mismo orden y cuenta de locations que create_parameters)
        const auto input = shader::get_program_input(program);
        uint32_t fcount = 0;
        for (const auto &in : input.inputs) {
            if (const auto *s = std::get_if<shader::usse::AttributeInputSource>(&in.source)) {
                const uint32_t comps = in.array_size * in.component_count;
                if (info->nattr < 16) {
                    V3kAttr &a = info->attr[info->nattr++];
                    a.reg = static_cast<uint16_t>(s->index);
                    a.loc = static_cast<uint8_t>(fcount / 4);
                    a.nloc = static_cast<uint8_t>((comps + 3) / 4);
                    a.comps = static_cast<uint8_t>(comps);
                    a.regformat = s->regformat ? 1 : 0;
                    a.semantic = s->semantic == SCE_GXM_PARAMETER_SEMANTIC_INDEX ? 1 : s->semantic == SCE_GXM_PARAMETER_SEMANTIC_INSTANCE ? 2 : 0;
                    if (a.regformat) {   // se empaquetan como uvec4 de componentes de 8/16/32 bits
                        int n = static_cast<int>(comps);
                        if (in.type == shader::usse::DataType::C10) n = (n * 10 + 7) / 8;
                        a.comps = static_cast<uint8_t>(n);
                        a.nloc = static_cast<uint8_t>((n + 3) / 4);
                    }
                }
                fcount += (comps + 3) / 4 * 4;
            }
        }
        if (!info->is_frag && fcount == 0 && program.primary_reg_count != 0) {
            for (size_t i = 0; i < attrs.size() && info->nattr < 16; i++) {
                V3kAttr &a = info->attr[info->nattr++];
                a.reg = attrs[i].regIndex;
                a.loc = static_cast<uint8_t>(i);
                a.nloc = static_cast<uint8_t>((attrs[i].componentCount + 3) / 4 ? (attrs[i].componentCount + 3) / 4 : 1);
                a.comps = attrs[i].componentCount;
            }
        }
        for (uint32_t i = 0; i < info->nattr; i++)
            if (!info->attr[i].semantic && info->attr[i].loc + info->attr[i].nloc > info->nloc)
                info->nloc = info->attr[i].loc + info->attr[i].nloc;

        // Buffers de uniformes (como layout_ssbo_offset_from_uniform_buffer_sizes)
        uint32_t sizes[16] = {};
        for (const auto &b : input.uniform_buffers) {
            if (b.index >= SCE_GXM_REAL_MAX_UNIFORM_BUFFER || b.size == 0) continue;
            const int host = b.index < SCE_GXM_MAX_UNIFORM_BUFFERS ? b.index + SCE_GXM_UNIFORM_BUFFER_OFFSET : SCE_GXM_DEFAULT_UNIFORM_BUFFER_CONTAINER_INDEX;
            sizes[host] = b.size;
        }
        uint32_t off = 0;
        for (int i = 0; i < 16; i++) {
            if (sizes[i]) {
                info->buf_off[i] = off * 4;
                info->buf_size[i] = sizes[i] * 4;
                off += (sizes[i] + 3) & ~3u;
            } else {
                info->buf_off[i] = ~0u;
            }
        }
        info->data_bytes = off * 4;
        for (const auto &smp : input.samplers)
            if (smp.index < 16) info->tex_mask |= 1u << smp.index;

        // MLB15_V3K_VOLCADO=prefijo: guarda el desensamblado USSE (.dsm) y el SPIR-V en texto (.spv)
        const char *pref = getenv("MLB15_V3K_VOLCADO");
        std::function<bool(const std::string &, const std::string &)> dumper;
        if (pref && *pref) {
            const std::string tag = std::to_string(program.size) + (program.is_fragment() ? "f" : "v");
            dumper = [pref, tag](const std::string &ext, const std::string &dump) {
                const std::string path = std::string(pref) + "_" + tag + "." + ext + ".txt";
                FILE *f = fopen(path.c_str(), "wb");
                if (!f) return false;
                fwrite(dump.data(), 1, dump.size(), f);
                fclose(f);
                return true;
            };
        }
        // MLB15_V3K_CORTE=tamaño,pc,banco,num[,f]: solo para el GXP de ese tamaño (depuración)
        if (const char *c = getenv("MLB15_V3K_CORTE")) {
            unsigned sz = 0, pc = 0, num = 0;
            char bank[4] = {}, t = 'h';
            if (sscanf(c, "%u,%u,%3[a-z],%u,%c", &sz, &pc, bank, &num, &t) >= 4 && sz == program.size) {
                const std::string bk = bank;
                v3k_corte_bank = bk == "pa" ? 1 : bk == "o" ? 2 : bk == "sa" ? 3 : bk == "i" ? 4 : 0;
                v3k_corte_num = static_cast<int>(num);
                v3k_corte_f32 = t == 'f';
                v3k_corte_pc = static_cast<int>(pc);
                v3k_corte_hecho = 0;
                v3k_corte_dst = program.is_fragment() ? program.vertex_varyings()->fragment_output_start : 0;
            }
        }
        const auto gen = shader::convert_gxp(program, "mlb15", features, shader::Target::SpirVVulkan, hints, false,
                                             dumper != nullptr, dumper);
        v3k_corte_pc = -1;
        if (gen.spirv.empty()) {
            snprintf(err, errlen, "SPIR-V vacío");
            return 0;
        }
        *words = gen.spirv.size();
        *spv = static_cast<uint32_t *>(malloc(*words * 4));
        if (!*spv) return 0;
        memcpy(*spv, gen.spirv.data(), *words * 4);
        return 1;
    } catch (const std::exception &e) {
        snprintf(err, errlen, "excepción: %s", e.what());
    } catch (...) {
        snprintf(err, errlen, "excepción desconocida");
    }
    return 0;
}
