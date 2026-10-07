/* Execute translated fragment GLSL on a real, offscreen OpenGL context. */
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <cassert>
#include <cstdio>
#include <vector>
static GLuint compile(GLenum stage, const char* text) {
    GLuint shader = glCreateShader(stage);
    glShaderSource(shader, 1, &text, nullptr); glCompileShader(shader);
    GLint ok = 0; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[8192]; glGetShaderInfoLog(shader, sizeof log, nullptr, log); std::fprintf(stderr,"%s\n",log); }
    assert(ok); return shader;
}
void render_red(const char* fragment) {
    auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    assert(getPlatformDisplay);
    EGLDisplay display = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    EGLint major, minor; assert(eglInitialize(display, &major, &minor));
    assert(eglBindAPI(EGL_OPENGL_API));
    const EGLint configAttrs[] = {EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,
        EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
    EGLConfig config; EGLint count; assert(eglChooseConfig(display,configAttrs,&config,1,&count) && count);
    const EGLint contextAttrs[] = {EGL_CONTEXT_MAJOR_VERSION,4,EGL_CONTEXT_MINOR_VERSION,3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK,EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,EGL_NONE};
    EGLContext ctx = eglCreateContext(display,config,EGL_NO_CONTEXT,contextAttrs); assert(ctx != EGL_NO_CONTEXT);
    const EGLint surfaceAttrs[] = {EGL_WIDTH,16,EGL_HEIGHT,16,EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display,config,surfaceAttrs); assert(surface != EGL_NO_SURFACE);
    assert(eglMakeCurrent(display,surface,surface,ctx));
    const char* vertex = "#version 430\nvoid main() { vec2 p[3]=vec2[](vec2(-1,-1),vec2(3,-1),vec2(-1,3)); gl_Position=vec4(p[gl_VertexID],0,1); }";
    GLuint vs=compile(GL_VERTEX_SHADER,vertex), ps=compile(GL_FRAGMENT_SHADER,fragment), program=glCreateProgram();
    glAttachShader(program,vs);glAttachShader(program,ps);glLinkProgram(program);
    GLint ok=0;glGetProgramiv(program,GL_LINK_STATUS,&ok);assert(ok);
    glUseProgram(program);GLuint vao;glGenVertexArrays(1,&vao);glBindVertexArray(vao);
    glViewport(0,0,16,16);glClearColor(0,1,0,1);glClear(GL_COLOR_BUFFER_BIT);glDrawArrays(GL_TRIANGLES,0,3);
    std::vector<unsigned char> pixels(16*16*4);glReadPixels(0,0,16,16,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
    assert(glGetError()==GL_NO_ERROR);
    for (unsigned i=0;i<pixels.size();i+=4) assert(pixels[i]==255 && pixels[i+1]==0 && pixels[i+2]==0 && pixels[i+3]==255);
    glDeleteVertexArrays(1,&vao);glDeleteProgram(program);glDeleteShader(vs);glDeleteShader(ps);
    eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    eglDestroySurface(display,surface);eglDestroyContext(display,ctx);eglTerminate(display);
}
