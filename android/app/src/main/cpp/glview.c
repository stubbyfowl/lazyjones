/*
 * glview.c - draws the game picture with OpenGL ES 2.0 (see glview.h).
 *
 * Filters:
 *   SHARP      "sharp bilinear": nearest neighbour inside a texel, a linear
 *              blend only on the one output pixel that straddles a texel
 *              edge. Sharp pixels at any size, no uneven pixel widths.
 *   NEAREST    plain nearest neighbour
 *   SMOOTH     plain bilinear
 *   SCANLINES  SHARP, and each C64 line is darker at its top and bottom
 *              edge, like the lines of a CRT (from 2 screen pixels per line)
 */
#include <math.h>
#include <stdio.h>
#include <GLES2/gl2.h>
#include "glview.h"

static void (*log_fn)(const char *msg);

void glview_set_log(void (*fn)(const char *msg)) { log_fn = fn; }

static struct {
    int ready;
    GLuint prog_sharp, prog_scan, prog_plain, tex;
    GLint sharp_tex, sharp_size, sharp_scale;
    GLint scan_tex, scan_size, scan_scale;
    GLint plain_tex;
} gv;

static const char *vs_src =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";

#define PRECISION \
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n" \
    "precision highp float;\n" \
    "#else\n" \
    "precision mediump float;\n" \
    "#endif\n"

#define SHARP_SAMPLE \
    "uniform sampler2D u_tex;\n" \
    "uniform vec2 u_size;\n" \
    "uniform vec2 u_scale;\n" \
    "varying vec2 v_uv;\n" \
    "vec4 sharp(vec2 t) {\n" \
    "  vec2 c = fract(t) - 0.5;\n" \
    "  vec2 r = 0.5 - 0.5 / u_scale;\n" \
    "  vec2 s = (c - clamp(c, -r, r)) * u_scale + 0.5;\n" \
    "  return texture2D(u_tex, (floor(t) + s) / u_size);\n" \
    "}\n"

static const char *fs_sharp_src =
    PRECISION SHARP_SAMPLE
    "void main() { gl_FragColor = sharp(v_uv * u_size); }\n";

static const char *fs_scan_src =
    PRECISION SHARP_SAMPLE
    "void main() {\n"
    "  vec2 t = v_uv * u_size;\n"
    "  vec4 col = sharp(t);\n"
    "  float d = fract(t.y) - 0.5;\n"
    /* full effect from 2 screen pixels per C64 line, none at 1 */
    "  float on = clamp(u_scale.y - 1.0, 0.0, 1.0);\n"
    "  float k = (1.0 - on * 2.0 * d * d) * (1.0 + on * 0.15);\n"
    "  gl_FragColor = vec4(min(col.rgb * k, vec3(1.0)), 1.0);\n"
    "}\n";

static const char *fs_plain_src =
    "precision mediump float;\n"
    "uniform sampler2D u_tex;\n"
    "varying vec2 v_uv;\n"
    "void main() { gl_FragColor = texture2D(u_tex, v_uv); }\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512], msg[540];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        snprintf(msg, sizeof msg, "shader: %s", log);
        if (log_fn)
            log_fn(msg);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint link_program(const char *fs)
{
    GLuint v = compile(GL_VERTEX_SHADER, vs_src);
    GLuint f = compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) {
        if (v) glDeleteShader(v);
        if (f) glDeleteShader(f);
        return 0;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindAttribLocation(p, 0, "a_pos");
    glBindAttribLocation(p, 1, "a_uv");
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

int glview_init(void)
{
    gv.prog_plain = link_program(fs_plain_src);
    if (!gv.prog_plain)
        return 0;
    gv.plain_tex = glGetUniformLocation(gv.prog_plain, "u_tex");
    /* the other two are optional: without them SHARP and SCANLINES fall
     * back to the plain program */
    gv.prog_sharp = link_program(fs_sharp_src);
    if (gv.prog_sharp) {
        gv.sharp_tex = glGetUniformLocation(gv.prog_sharp, "u_tex");
        gv.sharp_size = glGetUniformLocation(gv.prog_sharp, "u_size");
        gv.sharp_scale = glGetUniformLocation(gv.prog_sharp, "u_scale");
    }
    gv.prog_scan = link_program(fs_scan_src);
    if (gv.prog_scan) {
        gv.scan_tex = glGetUniformLocation(gv.prog_scan, "u_tex");
        gv.scan_size = glGetUniformLocation(gv.prog_scan, "u_size");
        gv.scan_scale = glGetUniformLocation(gv.prog_scan, "u_scale");
    }
    glGenTextures(1, &gv.tex);
    glBindTexture(GL_TEXTURE_2D, gv.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, FE_W, FE_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    gv.ready = 1;
    return 1;
}

void glview_forget(void)
{
    gv.prog_plain = gv.prog_sharp = gv.prog_scan = gv.tex = 0;
    gv.ready = 0;
}

int glview_ready(void) { return gv.ready; }

int glview_has_sharp(void) { return gv.prog_sharp != 0 && gv.prog_scan != 0; }

void glview_dest(int ww, int wh, int sw, int sh, const fe_settings_t *s,
                 float *x, float *y, float *w, float *h)
{
    float dw, dh;
    if (s->scale == FE_SCALE_STRETCH) {
        dw = (float)ww;
        dh = (float)wh;
    } else if (s->scale == FE_SCALE_INTEGER) {
        int k = ww / sw < wh / sh ? ww / sw : wh / sh;
        if (k < 1)
            k = 1;
        dw = (float)(sw * k);
        dh = (float)(sh * k);
        if (dw > (float)ww || dh > (float)wh) {
            /* too small a screen for integer scaling: fit instead */
            float aspect = (float)sw / (float)sh;
            if ((float)ww / (float)wh > aspect) { dh = (float)wh; dw = dh * aspect; }
            else { dw = (float)ww; dh = dw / aspect; }
        }
    } else {
        float aspect = (float)sw * FE_PAR / (float)sh;
        if ((float)ww / (float)wh > aspect) {
            dh = (float)wh;
            dw = floorf(dh * aspect + 0.5f);
        } else {
            dw = (float)ww;
            dh = floorf(dw / aspect + 0.5f);
        }
    }
    *w = dw;
    *h = dh;
    *x = floorf(((float)ww - dw) * 0.5f);
    *y = floorf(((float)wh - dh) * 0.5f);
}

void glview_draw(int ww, int wh, const uint32_t *rgba, int sx, int sy, int sw, int sh,
                 const fe_settings_t *s, float dx, float dy, float dw, float dh)
{
    glViewport(0, 0, ww, wh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gv.tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, FE_W, FE_H, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    GLuint prog = gv.prog_plain;
    GLint u_tex = gv.plain_tex, u_size = -1, u_scale = -1;
    if (s->filter == FE_FILTER_SHARP && gv.prog_sharp) {
        prog = gv.prog_sharp;
        u_tex = gv.sharp_tex;
        u_size = gv.sharp_size;
        u_scale = gv.sharp_scale;
    } else if (s->filter == FE_FILTER_SCANLINES && gv.prog_scan) {
        prog = gv.prog_scan;
        u_tex = gv.scan_tex;
        u_size = gv.scan_size;
        u_scale = gv.scan_scale;
    }
    /* the sharp programs blend themselves and need linear texture reads */
    GLint filt = s->filter == FE_FILTER_NEAREST ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);

    float x0 = dx / (float)ww * 2.0f - 1.0f, x1 = (dx + dw) / (float)ww * 2.0f - 1.0f;
    float y0 = 1.0f - dy / (float)wh * 2.0f, y1 = 1.0f - (dy + dh) / (float)wh * 2.0f;
    float u0 = (float)sx / FE_W, u1 = (float)(sx + sw) / FE_W;
    float v0 = (float)sy / FE_H, v1 = (float)(sy + sh) / FE_H;
    const GLfloat verts[] = {
        x0, y0, u0, v0,
        x1, y0, u1, v0,
        x0, y1, u0, v1,
        x1, y1, u1, v1,
    };
    glUseProgram(prog);
    glUniform1i(u_tex, 0);
    if (u_size >= 0)
        glUniform2f(u_size, (float)FE_W, (float)FE_H);
    if (u_scale >= 0) {
        float scx = dw / (float)sw, scy = dh / (float)sh;
        glUniform2f(u_scale, scx < 1.0f ? 1.0f : scx, scy < 1.0f ? 1.0f : scy);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), verts);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), verts + 2);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}
