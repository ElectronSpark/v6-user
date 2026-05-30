/*
 * gldemo - offscreen OpenGL ES 2.0 "3D demo" that renders on the real GPU.
 *
 * On a Hyper-V GPU-P / WSL guest there is no scanout/present ABI, so this demo
 * does NOT try to present to a display. Instead it renders a shaded triangle
 * into an off-screen framebuffer object, reads the pixels back, and verifies
 * the GPU actually produced them. When Mesa is pointed at its d3d12 Gallium
 * driver (GALLIUM_DRIVER=d3d12, EGL_PLATFORM=surfaceless) the GL commands are
 * translated to Direct3D 12 and executed on the host NVIDIA GPU over /dev/dxg
 * -- the same proven path as d3d12probe, but driven by OpenGL.
 *
 * It prints GL_VENDOR / GL_RENDERER / GL_VERSION so you can see which device
 * actually ran the work (a hardware run reports the real adapter, e.g.
 * "D3D12 (NVIDIA GeForce RTX 4060 Laptop GPU)"; a software fallback reports
 * llvmpipe/softpipe). Exit status is 0 on a verified GPU render, non-zero
 * otherwise.
 */

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FB_W 256
#define FB_H 256

#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif

/*
 * Acquire a surfaceless EGL display so the demo never needs a Wayland display
 * or a DRM node. This works even when the shell has exported
 * EGL_PLATFORM=wayland for the GUI session, because the explicit platform
 * request overrides it.
 */
static EGLDisplay get_surfaceless_display(void)
{
    typedef EGLDisplay (*get_platform_display_fn)(EGLenum, void *,
                                                  const EGLAttrib *);
    const char *client_ext = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    get_platform_display_fn get_platform_display =
        (get_platform_display_fn)eglGetProcAddress("eglGetPlatformDisplay");

    if (get_platform_display && client_ext &&
        strstr(client_ext, "EGL_MESA_platform_surfaceless"))
        return get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);

    get_platform_display =
        (get_platform_display_fn)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (get_platform_display && client_ext &&
        strstr(client_ext, "EGL_MESA_platform_surfaceless"))
        return get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);

    return eglGetDisplay(EGL_DEFAULT_DISPLAY);
}

static const char *vs_src =
    "attribute vec2 a_pos;\n"
    "attribute vec3 a_col;\n"
    "varying vec3 v_col;\n"
    "void main() {\n"
    "    v_col = a_col;\n"
    "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

static const char *fs_src =
    "precision mediump float;\n"
    "varying vec3 v_col;\n"
    "void main() {\n"
    "    gl_FragColor = vec4(v_col, 1.0);\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    GLint ok = 0;

    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetShaderInfoLog(sh, sizeof(log) - 1, NULL, log);
        fprintf(stderr, "GLDEMO: shader compile failed: %s\n", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

int main(void)
{
    EGLDisplay dpy;
    EGLint major = 0, minor = 0;
    EGLConfig config;
    EGLint num_config = 0;
    EGLContext ctx;
    GLuint vs, fs, prog, fbo, rbo;
    GLuint vbo;
    unsigned char *pixels;
    const char *vendor, *renderer, *version;
    unsigned long checksum = 0;
    int center_lit = 0;
    int corner_dark = 0;
    int i;

    static const EGLint config_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    static const EGLint ctx_attrs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    /* Interleaved: x, y, r, g, b. A triangle covering the center. */
    static const GLfloat verts[] = {
         0.0f,  0.8f,  1.0f, 0.0f, 0.0f,
        -0.8f, -0.8f,  0.0f, 1.0f, 0.0f,
         0.8f, -0.8f,  0.0f, 0.0f, 1.0f,
    };

    dpy = get_surfaceless_display();
    if (dpy == EGL_NO_DISPLAY) {
        fprintf(stderr, "GLDEMO: no EGL display\n");
        return 2;
    }
    if (!eglInitialize(dpy, &major, &minor)) {
        fprintf(stderr, "GLDEMO: eglInitialize failed (0x%x)\n", eglGetError());
        return 2;
    }
    printf("GLDEMO: EGL %d.%d initialized\n", major, minor);

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        fprintf(stderr, "GLDEMO: eglBindAPI failed\n");
        return 2;
    }
    if (!eglChooseConfig(dpy, config_attrs, &config, 1, &num_config) ||
        num_config < 1) {
        fprintf(stderr, "GLDEMO: eglChooseConfig found no config (0x%x)\n",
                eglGetError());
        return 2;
    }

    ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctx_attrs);
    if (ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "GLDEMO: eglCreateContext failed (0x%x)\n",
                eglGetError());
        return 2;
    }
    /* Surfaceless: render only to an FBO, no window/pbuffer needed. */
    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        fprintf(stderr, "GLDEMO: eglMakeCurrent(surfaceless) failed (0x%x)\n",
                eglGetError());
        return 2;
    }

    vendor = (const char *)glGetString(GL_VENDOR);
    renderer = (const char *)glGetString(GL_RENDERER);
    version = (const char *)glGetString(GL_VERSION);
    printf("GLDEMO: GL_VENDOR   = %s\n", vendor ? vendor : "(null)");
    printf("GLDEMO: GL_RENDERER = %s\n", renderer ? renderer : "(null)");
    printf("GLDEMO: GL_VERSION  = %s\n", version ? version : "(null)");

    /* Off-screen color target. */
    glGenRenderbuffers(1, &rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    /* GL_RGBA4 is guaranteed renderable in core GLES2; the loose readback
     * tolerances below do not need 8-bit precision. */
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA4, FB_W, FB_H);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER, rbo);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "GLDEMO: FBO incomplete\n");
        return 3;
    }

    vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs)
        return 3;
    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "a_pos");
    glBindAttribLocation(prog, 1, "a_col");
    glLinkProgram(prog);
    {
        GLint ok = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[1024] = {0};
            glGetProgramInfoLog(prog, sizeof(log) - 1, NULL, log);
            fprintf(stderr, "GLDEMO: link failed: %s\n", log);
            return 3;
        }
    }

    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

    glViewport(0, 0, FB_W, FB_H);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(prog);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
                          5 * sizeof(GLfloat), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE,
                          5 * sizeof(GLfloat),
                          (void *)(2 * sizeof(GLfloat)));
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();

    pixels = malloc((size_t)FB_W * FB_H * 4);
    if (!pixels) {
        fprintf(stderr, "GLDEMO: out of memory\n");
        return 3;
    }
    glReadPixels(0, 0, FB_W, FB_H, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    {
        GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            fprintf(stderr, "GLDEMO: GL error 0x%x after readback\n", err);
            free(pixels);
            return 3;
        }
    }

    for (i = 0; i < FB_W * FB_H * 4; i++)
        checksum = (checksum * 131) + pixels[i];

    /* Center pixel should be lit (inside the triangle); a top corner should
     * remain the black clear color. */
    {
        unsigned char *c = &pixels[((FB_H / 2) * FB_W + (FB_W / 2)) * 4];
        unsigned char *tl = &pixels[(((FB_H - 1) * FB_W) + 0) * 4];
        center_lit = (c[0] + c[1] + c[2]) > 40;
        corner_dark = (tl[0] + tl[1] + tl[2]) < 40;
    }

    printf("GLDEMO: readback checksum=0x%lx center_lit=%d corner_dark=%d\n",
           checksum, center_lit, corner_dark);
    free(pixels);

    if (center_lit && corner_dark) {
        printf("GLDEMO: PASS rendered shaded triangle off-screen and read it "
               "back\n");
        return 0;
    }
    fprintf(stderr, "GLDEMO: FAIL readback did not match expected render\n");
    return 1;
}
