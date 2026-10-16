// Windows: load the GL entry points listed in gl_funcs.inc (wglGetProcAddress, or opengl32.dll for GL 1.1).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "../gl.h"
#include "../log.h"

#define PROJECTARENA_GL_DEF(name, type) type name = nullptr;
PROJECTARENA_GL_FUNCS(PROJECTARENA_GL_DEF)
#undef PROJECTARENA_GL_DEF

static void* glProc(const char* name) {
    void* p = (void*)wglGetProcAddress(name);
    if (p == nullptr || p == (void*)1 || p == (void*)2 || p == (void*)3 || p == (void*)-1) {
        static HMODULE gl = LoadLibraryA("opengl32.dll");
        p = gl ? (void*)GetProcAddress(gl, name) : nullptr;
    }
    return p;
}

bool glLoadFunctions() {
    bool ok = true;
#define PROJECTARENA_GL_LOAD(name, type) \
    name = (type)glProc(#name);       \
    if (!name) { LOGE("GL function missing: %s", #name); ok = false; }
    PROJECTARENA_GL_FUNCS(PROJECTARENA_GL_LOAD)
#undef PROJECTARENA_GL_LOAD
    return ok;
}
