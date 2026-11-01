// Shared between the Linux and Windows shims: the OpenGL hooks that make the editor background translucent.
#pragma once

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#endif
#include <GL/gl.h>

#ifdef _WIN32
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr, GLintptr;
#endif

#ifndef GL_RGB8
#define GL_RGB8 0x8051
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_CURRENT_PROGRAM
#define GL_CURRENT_PROGRAM 0x8B8D
#endif
#ifndef GL_SCISSOR_BOX
#define GL_SCISSOR_BOX 0x0C10
#endif
#ifndef GL_BLEND_SRC_RGB
#define GL_BLEND_SRC_RGB 0x80C9
#endif
#ifndef GL_BLEND_DST_RGB
#define GL_BLEND_DST_RGB 0x80C8
#endif
#ifndef GL_BLEND_SRC_ALPHA
#define GL_BLEND_SRC_ALPHA 0x80CB
#endif
#ifndef GL_BLEND_DST_ALPHA
#define GL_BLEND_DST_ALPHA 0x80CA
#endif
#ifndef GL_BLEND_COLOR
#define GL_BLEND_COLOR 0x8005
#endif
#ifndef GL_CONSTANT_COLOR
#define GL_CONSTANT_COLOR 0x8001
#endif

// X(return type, name, parameters, arguments): every GL function the shim replaces. shim.c defines
// hook_<name>; each platform exposes them to Sublime under the real GL names.
#define SHIM_HOOKS(X) \
	X(void, glRenderbufferStorage, (GLenum target, GLenum format, GLsizei w, GLsizei h), (target, format, w, h)) \
	X(void, glShaderSource, (GLuint shader, GLsizei n, const GLchar *const *src, const GLint *len), (shader, n, src, len)) \
	X(void, glAttachShader, (GLuint program, GLuint shader), (program, shader)) \
	X(void, glDeleteProgram, (GLuint program), (program)) \
	X(void, glBufferData, (GLenum target, GLsizeiptr size, const void *data, GLenum usage), (target, size, data, usage)) \
	X(void, glBufferSubData, (GLenum target, GLintptr offset, GLsizeiptr size, const void *data), (target, offset, size, data)) \
	X(void, glClear, (GLbitfield mask), (mask)) \
	X(void, glDrawArraysInstanced, (GLenum mode, GLint first, GLsizei count, GLsizei instances), (mode, first, count, instances))

#define SHIM_DECLARE(ret, name, params, args) ret hook_##name params;
SHIM_HOOKS(SHIM_DECLARE)
#undef SHIM_DECLARE

// Reads the opacities from the launcher (SUBL_EDITOR, SUBL_CHROME).
void shim_init(void);

// Provided by each platform: the real function behind a GL name.
void *gl_next(const char *name);
