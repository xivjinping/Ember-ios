/*
 * Ember: stand-in for <epoxy/gl.h> on iOS.
 *
 * iOS has no desktop OpenGL and no libepoxy. A few xemu headers that the
 * emulation core includes mention OpenGL *types* (a texture handle in a
 * struct, for example) without calling OpenGL. This header supplies those
 * types and nothing else, so any real OpenGL call still fails to compile
 * and gets noticed.
 *
 * It is only on the include path of the iOS build
 * (see configure-xemu-ios.sh).
 */
#ifndef EMBER_SHIM_EPOXY_GL_H
#define EMBER_SHIM_EPOXY_GL_H

typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef int GLint;
typedef unsigned int GLuint;
typedef int GLsizei;
typedef float GLfloat;

#endif
