// OpenGL: GLES 3 on Quest; on Windows the same GLES 3 API on desktop OpenGL 4.3+ (which runs "#version 300 es"
// shaders through ARB_ES3_compatibility). On PC the functions are pointers loaded once the context exists.
#pragma once
#ifdef __ANDROID__
#include <GLES3/gl3.h>
#else
#define GL_GLES_PROTOTYPES 0
#include <GLES3/gl3.h>   // Khronos header: types, enums, PFN typedefs (copied from the NDK by tools/build_pc.bat)
#include "pc/gl_funcs.inc"
#define PROJECTARENA_GL_DECL(name, type) extern type name;
PROJECTARENA_GL_FUNCS(PROJECTARENA_GL_DECL)
#undef PROJECTARENA_GL_DECL
#ifndef GL_FRAMEBUFFER_SRGB
#define GL_FRAMEBUFFER_SRGB 0x8DB9   // desktop GL: write sRGB-encoded colour to sRGB targets (always on in GLES)
#endif
bool glLoadFunctions();   // call with a context current; false if any function is missing
#endif
