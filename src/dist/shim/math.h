/* math.h mínimo para compilar el código generado sin la libc de MSVC (clang, sin CRT). */
#ifndef MLB15_SHIM_MATH_H
#define MLB15_SHIM_MATH_H
#define sqrt(x)  __builtin_sqrt(x)
#define sqrtf(x) __builtin_sqrtf(x)
#define trunc(x) __builtin_trunc(x)
#define truncf(x) __builtin_truncf(x)
#define fabs(x)  __builtin_fabs(x)
#define fabsf(x) __builtin_fabsf(x)
#define floor(x) __builtin_floor(x)
#define ceil(x)  __builtin_ceil(x)
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define NAN      __builtin_nanf("")
#define INFINITY __builtin_inff()
#endif
