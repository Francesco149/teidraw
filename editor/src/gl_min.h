// ── gl_min.h — the GL surface teidraw's Linux backend actually uses ─────────
// SDL3 owns the context, so entry points come from SDL_GL_GetProcAddress:
// no GL headers, no -lGL, no loader dependency (imgui_impl_opengl3.cpp loads
// its own subset internally; the two never share a TU). Plain GL names are
// mapped onto a per-process table so call sites read like normal GL.
//
// Add a function by adding one X() row; signatures are the GL 3.3 core ones.
#pragma once
#include <SDL3/SDL.h>
#include <cstddef>

typedef unsigned int  GLenum;
typedef unsigned char GLboolean;
typedef unsigned int  GLbitfield;
typedef void          GLvoid;
typedef signed char   GLbyte;
typedef short         GLshort;
typedef int           GLint;
typedef unsigned char GLubyte;
typedef unsigned short GLushort;
typedef unsigned int  GLuint;
typedef int           GLsizei;
typedef float         GLfloat;
typedef float         GLclampf;
typedef char          GLchar;
typedef ptrdiff_t     GLintptr;
typedef ptrdiff_t     GLsizeiptr;

#define TEI_GL_FUNCS(X)                                                                              \
    X(void,    TexImage2D,   (GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h,         \
                              GLint border, GLenum fmt, GLenum type, const void* px))                \
    X(void,    TexSubImage2D,(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,   \
                              GLenum fmt, GLenum type, const void* px))                              \
    X(void,    TexParameteri, (GLenum target, GLenum pname, GLint p))                               \
    X(void,    GenTextures,  (GLsizei n, GLuint* tex))                                               \
    X(void,    DeleteTextures,(GLsizei n, const GLuint* tex))                                        \
    X(void,    BindTexture,  (GLenum target, GLuint tex))                                            \
    X(void,    ActiveTexture,(GLenum unit))                                                          \
    X(void,    PixelStorei,  (GLenum pname, GLint p))                                                \
    X(void,    GenerateMipmap,(GLenum target))                                                       \
    X(void,    GenFramebuffers,(GLsizei n, GLuint* fb))                                              \
    X(void,    BindFramebuffer,(GLenum target, GLuint fb))                                           \
    X(void,    FramebufferTexture2D,(GLenum target, GLenum att, GLenum textarget, GLuint tex,       \
                              GLint level))                                                          \
    X(GLenum,  CheckFramebufferStatus,(GLenum target))                                               \
    X(void,    DeleteFramebuffers,(GLsizei n, const GLuint* fb))                                     \
    X(void,    Viewport,     (GLint x, GLint y, GLsizei w, GLsizei h))                               \
    X(void,    Scissor,      (GLint x, GLint y, GLsizei w, GLsizei h))                               \
    X(void,    ClearColor,   (GLfloat r, GLfloat g, GLfloat b, GLfloat a))                           \
    X(void,    Clear,        (GLbitfield mask))                                                      \
    X(void,    ReadPixels,   (GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type,      \
                              void* px))                                                             \
    X(const GLubyte*, GetString,(GLenum name))                                                       \
    X(const GLubyte*, GetStringi,(GLenum name, GLuint idx))                                          \
    X(void,    GetIntegerv,  (GLenum pname, GLint* out))                                             \
    X(void,    GetFloatv,    (GLenum pname, GLfloat* out))                                           \
    X(GLenum,  GetError,     (void))                                                                 \
    X(void,    Finish,       (void))

struct GlMinProcs {
#define TEI_GL_X(ret, name, args) ret (*name) args;
    TEI_GL_FUNCS(TEI_GL_X)
#undef TEI_GL_X
};
extern GlMinProcs g_glprocs;

#define glTexImage2D          g_glprocs.TexImage2D
#define glTexSubImage2D       g_glprocs.TexSubImage2D
#define glTexParameteri       g_glprocs.TexParameteri
#define glGenTextures         g_glprocs.GenTextures
#define glDeleteTextures      g_glprocs.DeleteTextures
#define glBindTexture         g_glprocs.BindTexture
#define glActiveTexture       g_glprocs.ActiveTexture
#define glPixelStorei         g_glprocs.PixelStorei
#define glGenerateMipmap      g_glprocs.GenerateMipmap
#define glGenFramebuffers     g_glprocs.GenFramebuffers
#define glBindFramebuffer     g_glprocs.BindFramebuffer
#define glFramebufferTexture2D g_glprocs.FramebufferTexture2D
#define glCheckFramebufferStatus g_glprocs.CheckFramebufferStatus
#define glDeleteFramebuffers  g_glprocs.DeleteFramebuffers
#define glViewport            g_glprocs.Viewport
#define glScissor             g_glprocs.Scissor
#define glClearColor          g_glprocs.ClearColor
#define glClear               g_glprocs.Clear
#define glReadPixels          g_glprocs.ReadPixels
#define glGetString           g_glprocs.GetString
#define glGetStringi          g_glprocs.GetStringi
#define glGetIntegerv         g_glprocs.GetIntegerv
#define glGetFloatv           g_glprocs.GetFloatv
#define glGetError            g_glprocs.GetError
#define glFinish              g_glprocs.Finish

// GL constants used by the backend (core 3.3 + the anisotropy extension).
#define GL_TEXTURE_2D_            0x0DE1
#define GL_TEXTURE_MIN_FILTER_    0x2801
#define GL_TEXTURE_MAG_FILTER_    0x2800
#define GL_TEXTURE_WRAP_S_        0x2802
#define GL_TEXTURE_WRAP_T_        0x2803
#define GL_NEAREST_               0x2600
#define GL_LINEAR_                0x2601
#define GL_LINEAR_MIPMAP_LINEAR_  0x2703
#define GL_CLAMP_TO_EDGE_         0x812F
#define GL_RED_                   0x1903
#define GL_RG_                    0x8227
#define GL_R8_                    0x8229
#define GL_RG8_                   0x822B
#define GL_RGBA_                  0x1908
#define GL_RGBA8_                 0x8058
#define GL_UNSIGNED_BYTE_         0x1401
#define GL_UNPACK_ALIGNMENT_      0x0CF5
#define GL_PACK_ALIGNMENT_        0x0D05
#define GL_COLOR_BUFFER_BIT_      0x00004000
#define GL_FRAMEBUFFER_           0x8D40
#define GL_COLOR_ATTACHMENT0_     0x8CE0
#define GL_FRAMEBUFFER_COMPLETE_  0x8CD5
#define GL_EXTENSIONS_            0x1F03
#define GL_NUM_EXTENSIONS_        0x821D
#define GL_VENDOR_                0x1F00
#define GL_RENDERER_              0x1F01
#define GL_VERSION_                0x1F02
#define GL_TEXTURE_MAX_ANISOTROPY_EXT_ 0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT_ 0x84FF

// Resolve every entry point against the current context. False if any is
// missing (then GL rendering cannot work at all — caller must bail out).
inline bool gl_min_load() {
#define TEI_GL_X(ret, name, args) \
    g_glprocs.name = (ret(*)args)SDL_GL_GetProcAddress("gl" #name); \
    if (!g_glprocs.name) return false;
    TEI_GL_FUNCS(TEI_GL_X)
#undef TEI_GL_X
    return true;
}
