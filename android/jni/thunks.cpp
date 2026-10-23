// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// See thunks.h. Letters (ThunkCaller): i int32, u uint32 (GLenum, GLuint, GLboolean, GLbitfield, EGLBoolean,
// EGLint used as an enum), f float, p guest pointer (0 = NULL), o GL pointer-or-offset (vertex/index data that is
// a buffer offset when a buffer object is bound), h handle in, H handle out. GLsizeiptr/GLintptr are signed
// `long` on the host: `i` sign-extends them to 64 bits as the driver expects.
#include "thunks.h"

#include <EGL/egl.h>
#include <dlfcn.h>

#include <map>
#include <mutex>

namespace rb::launcher {

namespace {

constexpr const char* kEgl = "libEGL.so";
constexpr const char* kGl2 = "libGLESv2.so";
constexpr const char* kGl1 = "libGLESv1_CM.so";
constexpr const char* kGl3 = "libGLESv3.so";

const std::map<std::string, ThunkEntry>& Table() {
  static const std::map<std::string, ThunkEntry> k = [] {
    std::map<std::string, ThunkEntry> t;
    const auto add = [&t](const char* lib, std::initializer_list<std::pair<const char*, const char*>> list) {
      for (const auto& [name, spec] : list) t.emplace(name, ThunkEntry{lib, spec});  // first table wins
    };
    // ---- OpenGL ES 2.0 (gl2.h), every entry point but the hand-written glGetString/glShaderSource and
    // glGetVertexAttribPointerv (which would hand the guest a host pointer).
    add(kGl2, {
        {"glActiveTexture", "v:u"}, {"glAttachShader", "v:uu"}, {"glBindAttribLocation", "v:uup"},
        {"glBindBuffer", "v:uu"}, {"glBindFramebuffer", "v:uu"}, {"glBindRenderbuffer", "v:uu"},
        {"glBindTexture", "v:uu"}, {"glBlendColor", "v:ffff"}, {"glBlendEquation", "v:u"},
        {"glBlendEquationSeparate", "v:uu"}, {"glBlendFunc", "v:uu"}, {"glBlendFuncSeparate", "v:uuuu"},
        {"glBufferData", "v:uipu"}, {"glBufferSubData", "v:uiip"}, {"glCheckFramebufferStatus", "u:u"},
        {"glClear", "v:u"}, {"glClearColor", "v:ffff"}, {"glClearDepthf", "v:f"}, {"glClearStencil", "v:i"},
        {"glColorMask", "v:uuuu"}, {"glCompileShader", "v:u"}, {"glCompressedTexImage2D", "v:uiuiiiip"},
        {"glCompressedTexSubImage2D", "v:uiiiiiuip"}, {"glCopyTexImage2D", "v:uiuiiiii"},
        {"glCopyTexSubImage2D", "v:uiiiiiii"}, {"glCreateProgram", "u:"}, {"glCreateShader", "u:u"},
        {"glCullFace", "v:u"}, {"glDeleteBuffers", "v:ip"}, {"glDeleteFramebuffers", "v:ip"},
        {"glDeleteProgram", "v:u"}, {"glDeleteRenderbuffers", "v:ip"}, {"glDeleteShader", "v:u"},
        {"glDeleteTextures", "v:ip"}, {"glDepthFunc", "v:u"}, {"glDepthMask", "v:u"}, {"glDepthRangef", "v:ff"},
        {"glDetachShader", "v:uu"}, {"glDisable", "v:u"}, {"glDisableVertexAttribArray", "v:u"},
        {"glDrawArrays", "v:uii"}, {"glDrawElements", "v:uiuo"}, {"glEnable", "v:u"},
        {"glEnableVertexAttribArray", "v:u"}, {"glFinish", "v:"}, {"glFlush", "v:"},
        {"glFramebufferRenderbuffer", "v:uuuu"}, {"glFramebufferTexture2D", "v:uuuui"}, {"glFrontFace", "v:u"},
        {"glGenBuffers", "v:ip"}, {"glGenerateMipmap", "v:u"}, {"glGenFramebuffers", "v:ip"},
        {"glGenRenderbuffers", "v:ip"}, {"glGenTextures", "v:ip"}, {"glGetActiveAttrib", "v:uuipppp"},
        {"glGetActiveUniform", "v:uuipppp"}, {"glGetAttachedShaders", "v:uipp"}, {"glGetAttribLocation", "i:up"},
        {"glGetBooleanv", "v:up"}, {"glGetBufferParameteriv", "v:uup"}, {"glGetError", "u:"},
        {"glGetFloatv", "v:up"}, {"glGetFramebufferAttachmentParameteriv", "v:uuup"}, {"glGetIntegerv", "v:up"},
        {"glGetProgramiv", "v:uup"}, {"glGetProgramInfoLog", "v:uipp"}, {"glGetRenderbufferParameteriv", "v:uup"},
        {"glGetShaderiv", "v:uup"}, {"glGetShaderInfoLog", "v:uipp"}, {"glGetShaderPrecisionFormat", "v:uupp"},
        {"glGetShaderSource", "v:uipp"}, {"glGetTexParameterfv", "v:uup"}, {"glGetTexParameteriv", "v:uup"},
        {"glGetUniformfv", "v:uip"}, {"glGetUniformiv", "v:uip"}, {"glGetUniformLocation", "i:up"},
        {"glGetVertexAttribfv", "v:uup"}, {"glGetVertexAttribiv", "v:uup"}, {"glHint", "v:uu"},
        {"glIsBuffer", "u:u"}, {"glIsEnabled", "u:u"}, {"glIsFramebuffer", "u:u"}, {"glIsProgram", "u:u"},
        {"glIsRenderbuffer", "u:u"}, {"glIsShader", "u:u"}, {"glIsTexture", "u:u"}, {"glLineWidth", "v:f"},
        {"glLinkProgram", "v:u"}, {"glPixelStorei", "v:ui"}, {"glPolygonOffset", "v:ff"},
        {"glReadPixels", "v:iiiiuup"}, {"glReleaseShaderCompiler", "v:"}, {"glRenderbufferStorage", "v:uuii"},
        {"glSampleCoverage", "v:fu"}, {"glScissor", "v:iiii"}, {"glShaderBinary", "v:ipupi"},
        {"glStencilFunc", "v:uiu"}, {"glStencilFuncSeparate", "v:uuiu"}, {"glStencilMask", "v:u"},
        {"glStencilMaskSeparate", "v:uu"}, {"glStencilOp", "v:uuu"}, {"glStencilOpSeparate", "v:uuuu"},
        {"glTexImage2D", "v:uiiiiiuup"}, {"glTexParameterf", "v:uuf"}, {"glTexParameterfv", "v:uup"},
        {"glTexParameteri", "v:uui"}, {"glTexParameteriv", "v:uup"}, {"glTexSubImage2D", "v:uiiiiiuup"},
        {"glUniform1f", "v:if"}, {"glUniform1fv", "v:iip"}, {"glUniform1i", "v:ii"}, {"glUniform1iv", "v:iip"},
        {"glUniform2f", "v:iff"}, {"glUniform2fv", "v:iip"}, {"glUniform2i", "v:iii"}, {"glUniform2iv", "v:iip"},
        {"glUniform3f", "v:ifff"}, {"glUniform3fv", "v:iip"}, {"glUniform3i", "v:iiii"}, {"glUniform3iv", "v:iip"},
        {"glUniform4f", "v:iffff"}, {"glUniform4fv", "v:iip"}, {"glUniform4i", "v:iiiii"},
        {"glUniform4iv", "v:iip"}, {"glUniformMatrix2fv", "v:iiup"}, {"glUniformMatrix3fv", "v:iiup"},
        {"glUniformMatrix4fv", "v:iiup"}, {"glUseProgram", "v:u"}, {"glValidateProgram", "v:u"},
        {"glVertexAttrib1f", "v:uf"}, {"glVertexAttrib1fv", "v:up"}, {"glVertexAttrib2f", "v:uff"},
        {"glVertexAttrib2fv", "v:up"}, {"glVertexAttrib3f", "v:ufff"}, {"glVertexAttrib3fv", "v:up"},
        {"glVertexAttrib4f", "v:uffff"}, {"glVertexAttrib4fv", "v:up"}, {"glVertexAttribPointer", "v:uiuuio"},
        {"glViewport", "v:iiii"},
        // Common OES extensions games call by name (resolved through eglGetProcAddress on the host).
        {"glDiscardFramebufferEXT", "v:uip"}, {"glBindVertexArrayOES", "v:u"}, {"glGenVertexArraysOES", "v:ip"},
        {"glDeleteVertexArraysOES", "v:ip"}, {"glIsVertexArrayOES", "u:u"},
    });
    // ---- OpenGL ES 3.0 / 3.1 (gl3.h, gl31.h): what Unity 5+ resolves for its ES3 renderer. Hand-written instead:
    // glGetStringi (host strings), glMapBufferRange/glUnmapBuffer/glFlushMappedBufferRange (driver memory, shadowed in
    // the guest), glGetUniformIndices/glTransformFeedbackVaryings (arrays of guest string pointers). GLsync is a handle;
    // GLuint64/GLint64 are 'l'; GLintptr/GLsizeiptr are 'i' (sign-extended).
    add(kGl3, {
        {"glReadBuffer", "v:u"}, {"glDrawRangeElements", "v:uuuiuo"}, {"glTexImage3D", "v:uiiiiiiuuo"},
        {"glTexSubImage3D", "v:uiiiiiiiuuo"}, {"glCopyTexSubImage3D", "v:uiiiiiiii"},
        // Unity probes the OES_spelled 3D entry points through eglGetProcAddress and stores the answer in its GLES
        // device, then calls that slot without a null check. The core names above are the same functions: on GLES2
        // (where OES_texture_3D is the extension) the device exposes them under these names only.
        {"glTexImage3DOES", "v:uiiiiiiuuo"}, {"glTexSubImage3DOES", "v:uiiiiiiiuuo"},
        {"glCompressedTexImage3D", "v:uiuiiiiio"}, {"glCompressedTexSubImage3D", "v:uiiiiiiiuio"},
        {"glGenQueries", "v:ip"}, {"glDeleteQueries", "v:ip"}, {"glIsQuery", "u:u"}, {"glBeginQuery", "v:uu"},
        {"glEndQuery", "v:u"}, {"glGetQueryiv", "v:uup"}, {"glGetQueryObjectuiv", "v:uup"},
        {"glDrawBuffers", "v:ip"}, {"glUniformMatrix2x3fv", "v:iiup"}, {"glUniformMatrix3x2fv", "v:iiup"},
        {"glUniformMatrix2x4fv", "v:iiup"}, {"glUniformMatrix4x2fv", "v:iiup"}, {"glUniformMatrix3x4fv", "v:iiup"},
        {"glUniformMatrix4x3fv", "v:iiup"}, {"glBlitFramebuffer", "v:iiiiiiiiuu"},
        {"glRenderbufferStorageMultisample", "v:uiuii"}, {"glFramebufferTextureLayer", "v:uuuii"},
        {"glBindVertexArray", "v:u"}, {"glDeleteVertexArrays", "v:ip"}, {"glGenVertexArrays", "v:ip"},
        {"glIsVertexArray", "u:u"}, {"glGetIntegeri_v", "v:uup"}, {"glBeginTransformFeedback", "v:u"},
        {"glEndTransformFeedback", "v:"}, {"glBindBufferRange", "v:uuuii"}, {"glBindBufferBase", "v:uuu"},
        {"glGetTransformFeedbackVarying", "v:uuipppp"}, {"glVertexAttribIPointer", "v:uiuio"},
        {"glGetVertexAttribIiv", "v:uup"}, {"glGetVertexAttribIuiv", "v:uup"}, {"glVertexAttribI4i", "v:uiiii"},
        {"glVertexAttribI4ui", "v:uuuuu"}, {"glVertexAttribI4iv", "v:up"}, {"glVertexAttribI4uiv", "v:up"},
        {"glGetUniformuiv", "v:uip"}, {"glGetFragDataLocation", "i:up"}, {"glUniform1ui", "v:iu"},
        {"glUniform2ui", "v:iuu"}, {"glUniform3ui", "v:iuuu"}, {"glUniform4ui", "v:iuuuu"},
        {"glUniform1uiv", "v:iip"}, {"glUniform2uiv", "v:iip"}, {"glUniform3uiv", "v:iip"}, {"glUniform4uiv", "v:iip"},
        {"glClearBufferiv", "v:uip"}, {"glClearBufferuiv", "v:uip"}, {"glClearBufferfv", "v:uip"},
        {"glClearBufferfi", "v:uifi"}, {"glCopyBufferSubData", "v:uuiii"}, {"glGetActiveUniformsiv", "v:uipup"},
        {"glGetUniformBlockIndex", "u:up"}, {"glGetActiveUniformBlockiv", "v:uuup"},
        {"glGetActiveUniformBlockName", "v:uuipp"}, {"glUniformBlockBinding", "v:uuu"},
        {"glDrawArraysInstanced", "v:uiii"}, {"glDrawElementsInstanced", "v:uiuoi"}, {"glFenceSync", "H:uu"},
        {"glIsSync", "u:h"}, {"glDeleteSync", "v:h"}, {"glClientWaitSync", "u:hul"}, {"glWaitSync", "v:hul"},
        {"glGetInteger64v", "v:up"}, {"glGetSynciv", "v:huipp"}, {"glGetInteger64i_v", "v:uup"},
        {"glGetBufferParameteri64v", "v:uup"}, {"glGenSamplers", "v:ip"}, {"glDeleteSamplers", "v:ip"},
        {"glIsSampler", "u:u"}, {"glBindSampler", "v:uu"}, {"glSamplerParameteri", "v:uui"},
        {"glSamplerParameteriv", "v:uup"}, {"glSamplerParameterf", "v:uuf"}, {"glSamplerParameterfv", "v:uup"},
        {"glGetSamplerParameteriv", "v:uup"}, {"glGetSamplerParameterfv", "v:uup"}, {"glVertexAttribDivisor", "v:uu"},
        {"glBindTransformFeedback", "v:uu"}, {"glDeleteTransformFeedbacks", "v:ip"},
        {"glGenTransformFeedbacks", "v:ip"}, {"glIsTransformFeedback", "u:u"}, {"glPauseTransformFeedback", "v:"},
        {"glResumeTransformFeedback", "v:"}, {"glGetProgramBinary", "v:uippp"}, {"glProgramBinary", "v:uupi"},
        {"glProgramParameteri", "v:uui"}, {"glInvalidateFramebuffer", "v:uip"},
        {"glInvalidateSubFramebuffer", "v:uipiiii"}, {"glTexStorage2D", "v:uiuii"}, {"glTexStorage3D", "v:uiuiii"},
        {"glGetInternalformativ", "v:uuuip"},
        // 3.1
        {"glDispatchCompute", "v:uuu"}, {"glDispatchComputeIndirect", "v:i"}, {"glDrawArraysIndirect", "v:uo"},
        {"glDrawElementsIndirect", "v:uuo"}, {"glFramebufferParameteri", "v:uui"},
        {"glGetProgramInterfaceiv", "v:uuup"}, {"glGetProgramResourceIndex", "u:uup"},
        {"glGetProgramResourceName", "v:uuuipp"}, {"glGetProgramResourceiv", "v:uuuipipp"},
        {"glGetProgramResourceLocation", "i:uup"}, {"glBindImageTexture", "v:uuiuiuu"}, {"glMemoryBarrier", "v:u"},
        {"glMemoryBarrierByRegion", "v:u"}, {"glTexStorage2DMultisample", "v:uiuiiu"},
        {"glGetMultisamplefv", "v:uup"}, {"glSampleMaski", "v:uu"}, {"glGetTexLevelParameteriv", "v:uiup"},
        {"glGetTexLevelParameterfv", "v:uiup"}, {"glBindVertexBuffer", "v:uuii"}, {"glVertexAttribFormat", "v:uiuuu"},
        {"glVertexAttribIFormat", "v:uiuu"}, {"glVertexAttribBinding", "v:uu"}, {"glVertexBindingDivisor", "v:uu"},
        {"glUseProgramStages", "v:uuu"}, {"glActiveShaderProgram", "v:uu"}, {"glGenProgramPipelines", "v:ip"},
        {"glDeleteProgramPipelines", "v:ip"}, {"glBindProgramPipeline", "v:u"}, {"glIsProgramPipeline", "u:u"},
        {"glValidateProgramPipeline", "v:u"}, {"glGetProgramPipelineiv", "v:uup"},
        {"glGetProgramPipelineInfoLog", "v:uipp"},
        {"glProgramUniform1i", "v:uii"}, {"glProgramUniform2i", "v:uiii"}, {"glProgramUniform3i", "v:uiiii"},
        {"glProgramUniform4i", "v:uiiiii"}, {"glProgramUniform1ui", "v:uiu"}, {"glProgramUniform2ui", "v:uiuu"},
        {"glProgramUniform3ui", "v:uiuuu"}, {"glProgramUniform4ui", "v:uiuuuu"}, {"glProgramUniform1f", "v:uif"},
        {"glProgramUniform2f", "v:uiff"}, {"glProgramUniform3f", "v:uifff"}, {"glProgramUniform4f", "v:uiffff"},
        {"glProgramUniform1iv", "v:uiip"}, {"glProgramUniform2iv", "v:uiip"}, {"glProgramUniform3iv", "v:uiip"},
        {"glProgramUniform4iv", "v:uiip"}, {"glProgramUniform1uiv", "v:uiip"}, {"glProgramUniform2uiv", "v:uiip"},
        {"glProgramUniform3uiv", "v:uiip"}, {"glProgramUniform4uiv", "v:uiip"}, {"glProgramUniform1fv", "v:uiip"},
        {"glProgramUniform2fv", "v:uiip"}, {"glProgramUniform3fv", "v:uiip"}, {"glProgramUniform4fv", "v:uiip"},
        {"glProgramUniformMatrix2fv", "v:uiiup"}, {"glProgramUniformMatrix3fv", "v:uiiup"},
        {"glProgramUniformMatrix4fv", "v:uiiup"}, {"glProgramUniformMatrix2x3fv", "v:uiiup"},
        {"glProgramUniformMatrix3x2fv", "v:uiiup"}, {"glProgramUniformMatrix2x4fv", "v:uiiup"},
        {"glProgramUniformMatrix4x2fv", "v:uiiup"}, {"glProgramUniformMatrix3x4fv", "v:uiiup"},
        {"glProgramUniformMatrix4x3fv", "v:uiiup"},
        // 3.2 / extensions Unity probes; absent on a device, they answer 0 and are named once.
        {"glFramebufferTexture", "v:uuui"}, {"glPatchParameteri", "v:ui"}, {"glTexStorage3DMultisample", "v:uiuiiiu"},
        {"glCopyImageSubData", "v:uuiiiiuuiiiiiii"},
    });
    add(kGl2, {
        {"glFramebufferTexture3D", "v:uuuuii"}, {"glFramebufferTexture3DOES", "v:uuuuii"},
        {"glTexImage2DMultisample", "v:uiuiiu"},
        // Extensions engines fetch with eglGetProcAddress when the host advertises them (Unity: multisampled
        // render-to-texture, discard, debug markers, program binaries, timer queries).
        {"glFramebufferTexture2DMultisampleEXT", "v:uuuuii"}, {"glRenderbufferStorageMultisampleEXT", "v:uiuii"},
        {"glFramebufferTexture2DMultisampleIMG", "v:uuuuii"}, {"glRenderbufferStorageMultisampleIMG", "v:uiuii"},
        {"glDiscardFramebufferEXT", "v:uip"}, {"glPushGroupMarkerEXT", "v:ip"}, {"glPopGroupMarkerEXT", "v:"},
        {"glInsertEventMarkerEXT", "v:ip"}, {"glLabelObjectEXT", "v:uuip"}, {"glGetObjectLabelEXT", "v:uuipp"},
        {"glGetProgramBinaryOES", "v:uippp"}, {"glProgramBinaryOES", "v:uupi"}, {"glTexStorage2DEXT", "v:uiuii"},
        {"glBindVertexArrayOES", "v:u"}, {"glGenVertexArraysOES", "v:ip"}, {"glDeleteVertexArraysOES", "v:ip"},
        {"glIsVertexArrayOES", "u:u"}, {"glQueryCounterEXT", "v:uu"}, {"glGetQueryObjectui64vEXT", "v:uup"},
        {"glGetQueryObjecti64vEXT", "v:uup"}, {"glGenQueriesEXT", "v:ip"}, {"glDeleteQueriesEXT", "v:ip"},
        {"glBeginQueryEXT", "v:uu"}, {"glEndQueryEXT", "v:u"}, {"glGetQueryObjectuivEXT", "v:uup"},
        {"glDrawBuffersEXT", "v:ip"}, {"glDrawBuffersNV", "v:ip"}, {"glBlitFramebufferNV", "v:iiiiiiiiuu"},
        {"glDrawArraysInstancedEXT", "v:uiii"}, {"glDrawElementsInstancedEXT", "v:uiuoi"},
        {"glVertexAttribDivisorEXT", "v:uu"}, {"glTexStorage3DEXT", "v:uiuiii"},
    });
    // ---- OpenGL ES 1.1 entry points 2.0 does not have (fixed function). Pointers are pointer-or-offset (VBOs).
    add(kGl1, {
        {"glAlphaFunc", "v:uf"}, {"glClientActiveTexture", "v:u"}, {"glColor4f", "v:ffff"},
        {"glColor4ub", "v:uuuu"}, {"glColorPointer", "v:iuio"}, {"glDisableClientState", "v:u"},
        {"glEnableClientState", "v:u"}, {"glFogf", "v:uf"}, {"glFogfv", "v:up"}, {"glFrustumf", "v:ffffff"},
        {"glLightf", "v:uuf"}, {"glLightfv", "v:uup"}, {"glLightModelf", "v:uf"}, {"glLightModelfv", "v:up"},
        {"glLoadIdentity", "v:"}, {"glLoadMatrixf", "v:p"}, {"glMaterialf", "v:uuf"}, {"glMaterialfv", "v:uup"},
        {"glMatrixMode", "v:u"}, {"glMultMatrixf", "v:p"}, {"glNormal3f", "v:fff"}, {"glNormalPointer", "v:uio"},
        {"glOrthof", "v:ffffff"}, {"glPointSize", "v:f"}, {"glPopMatrix", "v:"}, {"glPushMatrix", "v:"},
        {"glRotatef", "v:ffff"}, {"glScalef", "v:fff"}, {"glShadeModel", "v:u"}, {"glTexCoordPointer", "v:iuio"},
        {"glTexEnvf", "v:uuf"}, {"glTexEnvfv", "v:uup"}, {"glTexEnvi", "v:uui"}, {"glTranslatef", "v:fff"},
        {"glVertexPointer", "v:iuio"}, {"glPointParameterf", "v:uf"}, {"glPointParameterfv", "v:up"},
        {"glGetTexEnviv", "v:uup"}, {"glMultiTexCoord4f", "v:uffff"}, {"glColor4x", "v:iiii"},
        {"glTexParameterx", "v:uui"},
        {"glBindFramebufferOES", "v:uu"}, {"glGenFramebuffersOES", "v:ip"}, {"glDeleteFramebuffersOES", "v:ip"},
        {"glFramebufferTexture2DOES", "v:uuuui"}, {"glCheckFramebufferStatusOES", "u:u"},
        {"glBindRenderbufferOES", "v:uu"}, {"glGenRenderbuffersOES", "v:ip"},
        {"glDeleteRenderbuffersOES", "v:ip"}, {"glRenderbufferStorageOES", "v:uuii"},
        {"glFramebufferRenderbufferOES", "v:uuuu"}, {"glGenerateMipmapOES", "v:u"},
    });
    // ---- EGL 1.4. Display, config, context and surface are host pointers: handles. eglQueryString,
    // eglGetProcAddress, eglChooseConfig and eglGetConfigs are hand-written (strings, out-arrays of configs).
    add(kEgl, {
        {"eglGetDisplay", "H:u"}, {"eglInitialize", "u:hpp"}, {"eglTerminate", "u:h"}, {"eglGetError", "i:"},
        {"eglGetConfigAttrib", "u:hhip"}, {"eglCreateWindowSurface", "H:hhhp"},
        {"eglCreatePbufferSurface", "H:hhp"}, {"eglDestroySurface", "u:hh"}, {"eglQuerySurface", "u:hhip"},
        {"eglBindAPI", "u:u"}, {"eglQueryAPI", "u:"}, {"eglWaitClient", "u:"}, {"eglReleaseThread", "u:"},
        {"eglSurfaceAttrib", "u:hhii"}, {"eglBindTexImage", "u:hhi"}, {"eglReleaseTexImage", "u:hhi"},
        {"eglSwapInterval", "u:hi"}, {"eglCreateContext", "H:hhhp"}, {"eglDestroyContext", "u:hh"},
        {"eglMakeCurrent", "u:hhhh"}, {"eglGetCurrentContext", "H:"}, {"eglGetCurrentSurface", "H:i"},
        {"eglGetCurrentDisplay", "H:"}, {"eglQueryContext", "u:hhip"}, {"eglWaitGL", "u:"},
        {"eglWaitNative", "u:i"}, {"eglSwapBuffers", "u:hh"}, {"eglCopyBuffers", "u:hhh"},
    });
    // ---- libandroid: assets and native windows (the JNI-object and host-memory ones are specials, thunks_android).
    // off_t is the guest's 32-bit long (`i`, sign-extended into the host's 64-bit off_t); off64_t is `l`.
    add("libandroid.so", {
        {"AAssetManager_open", "H:hpi"}, {"AAssetManager_openDir", "H:hp"}, {"AAssetDir_rewind", "v:h"},
        {"AAsset_read", "i:hpu"}, {"AAsset_seek", "i:hii"}, {"AAsset_seek64", "l:hli"},
        {"AAsset_getLength", "i:h"}, {"AAsset_getLength64", "l:h"}, {"AAsset_getRemainingLength", "i:h"},
        {"AAsset_getRemainingLength64", "l:h"}, {"AAsset_isAllocated", "i:h"},
        {"ANativeWindow_acquire", "v:h"}, {"ANativeWindow_release", "v:h"}, {"ANativeWindow_getWidth", "i:h"},
        {"ANativeWindow_getHeight", "i:h"}, {"ANativeWindow_getFormat", "i:h"},
        {"ANativeWindow_setBuffersGeometry", "i:hiii"},
        // Sensors and loopers (Cars). ASensorEvent is 104 bytes with the same offsets on armeabi and arm64, so getEvents
        // writes the guest's array in place. createEventQueue is a special (thunks_android): its callback is guest code.
        {"ASensorManager_getInstance", "H:"}, {"ASensorManager_getDefaultSensor", "H:hi"},
        {"ASensorEventQueue_enableSensor", "i:hh"}, {"ASensorEventQueue_disableSensor", "i:hh"},
        {"ASensorEventQueue_setEventRate", "i:hhi"}, {"ASensorEventQueue_getEvents", "i:hpu"},
        // The rest of what Unity's libunity imports (measured: Angry Birds Epic's 12 unanswered names, 2026-10-18).
        // Every one is a getter over a sensor or key handle this table issued, so the generic caller serves them:
        // the answer is the device's own, and a handle that was never issued is refused by name instead of dereferenced.
        {"ASensorEventQueue_hasEvents", "i:h"}, {"ASensorManager_destroyEventQueue", "i:h"},
        {"ASensor_getType", "i:h"}, {"ASensor_getMinDelay", "i:h"}, {"ASensor_getResolution", "f:h"},
        {"AKeyEvent_getAction", "i:h"}, {"AKeyEvent_getMetaState", "i:h"},
        {"ALooper_forThread", "H:"}, {"ALooper_prepare", "H:i"},
    });
    return t;
  }();
  return k;
}

}  // namespace

const ThunkEntry* FindThunk(const std::string& name) {
  const auto it = Table().find(name);
  return it == Table().end() ? nullptr : &it->second;
}

void* HostSymbol(const char* lib, const std::string& name) {
  static std::mutex mu;
  static std::map<std::string, void*> libs;
  void* h = nullptr;
  {
    std::lock_guard<std::mutex> lk(mu);
    auto it = libs.find(lib);
    if (it == libs.end()) it = libs.emplace(lib, dlopen(lib, RTLD_NOW | RTLD_LOCAL)).first;
    h = it->second;
  }
  void* fn = h != nullptr ? dlsym(h, name.c_str()) : nullptr;
  if (fn == nullptr && name.rfind("gl", 0) == 0) fn = reinterpret_cast<void*>(eglGetProcAddress(name.c_str()));
  return fn;
}

}  // namespace rb::launcher
