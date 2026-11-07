/* Compilación de GLSL a SPIR-V en tiempo de ejecución (glslang, interfaz C). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "glslang/Include/glslang_c_interface.h"
#include "glslang/Public/resource_limits_c.h"

#include "shc.h"

static int g_init;

int shc_compile(const char *glsl, int frag, uint32_t **spv, size_t *nwords, char *err, size_t errlen) {
    if (!g_init) { glslang_initialize_process(); g_init = 1; }
    glslang_input_t in;
    memset(&in, 0, sizeof(in));
    in.language = GLSLANG_SOURCE_GLSL;
    in.stage = frag ? GLSLANG_STAGE_FRAGMENT : GLSLANG_STAGE_VERTEX;
    in.client = GLSLANG_CLIENT_VULKAN;
    in.client_version = GLSLANG_TARGET_VULKAN_1_1;
    in.target_language = GLSLANG_TARGET_SPV;
    in.target_language_version = GLSLANG_TARGET_SPV_1_3;
    in.code = glsl;
    in.default_version = 450;
    in.default_profile = GLSLANG_NO_PROFILE;
    in.messages = GLSLANG_MSG_DEFAULT_BIT | GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT;
    in.resource = glslang_default_resource();
    glslang_shader_t *sh = glslang_shader_create(&in);
    int ok = 0;
    if (!glslang_shader_preprocess(sh, &in) || !glslang_shader_parse(sh, &in)) {
        if (err) snprintf(err, errlen, "%s", glslang_shader_get_info_log(sh));
        glslang_shader_delete(sh);
        return 0;
    }
    glslang_program_t *pr = glslang_program_create();
    glslang_program_add_shader(pr, sh);
    if (glslang_program_link(pr, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT)) {
        glslang_program_SPIRV_generate(pr, in.stage);
        size_t n = glslang_program_SPIRV_get_size(pr);
        *spv = (uint32_t *)malloc(n * 4);
        if (*spv) { glslang_program_SPIRV_get(pr, *spv); *nwords = n; ok = 1; }
    } else if (err) snprintf(err, errlen, "%s", glslang_program_get_info_log(pr));
    glslang_program_delete(pr);
    glslang_shader_delete(sh);
    return ok;
}
