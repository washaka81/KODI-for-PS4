/*
 * glshim-test (program mode) — draws the Kodi splash through the shim's
 * program path: the real Kodi GLES shaders (gles_shader.vert +
 * gles_shader_texture.frag) compiled offline to GNM blobs, linked by the
 * shim, with uniforms set through glUniform* and attributes through
 * glGetAttribLocation.
 */
#include <stdarg.h>
#include <orbis/libkernel.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libs/stb_image.h"

#include "glshim/glshim.h"

static void tlog(const char* fmt, ...) {
	FILE* f = fopen("/data/glshim-test.log", "a");
	if (!f)
		return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

static unsigned char* readfile(const char* path, long* outlen) {
	FILE* f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char* buf = malloc((size_t)len);
	if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) {
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*outlen = len;
	return buf;
}

typedef struct {
	float pos[4];
	float col[4];
	float cord0[4];
	float cord1[4];
	float snap[4];
} Vertex;

static const float kIdent[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

static int file_exists(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f)
		return 0;
	fclose(f);
	return 1;
}

static GLuint make_program_raw(const char* vpath, const char* fpath, const char* tag) {
	long vlen = 0, flen = 0;
	unsigned char* v = readfile(vpath, &vlen);
	unsigned char* f = readfile(fpath, &flen);
	if (!v || !f) {
		tlog("program %s: read failed", tag);
		free(v);
		free(f);
		return 0;
	}
	GLuint vs = glCreateShader(GL_VERTEX_SHADER);
	GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
	// raw psbc .sb (no Piglet container, no PSSL reflection tables)
	glShaderBinary(1, &vs, 0, v, (GLsizei)vlen);
	glShaderBinary(1, &fs, 0, f, (GLsizei)flen);
	free(v);
	free(f);
	glCompileShader(vs);
	glCompileShader(fs);
	GLuint prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glLinkProgram(prog);
	GLint ok = 0;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	tlog("program %s: link=%d", tag, ok);
	return ok ? prog : 0;
}

static GLuint make_program(const char* vpath, const char* fpath, const char* tag) {
	long vlen = 0, flen = 0;
	unsigned char* v = readfile(vpath, &vlen);
	unsigned char* f = readfile(fpath, &flen);
	if (!v || !f) {
		tlog("program %s: blob read failed", tag);
		free(v);
		free(f);
		return 0;
	}
	GLuint vs = glCreateShader(GL_VERTEX_SHADER);
	GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
	// Kodi's loader strips the first u32 (format) before glShaderBinary.
	glShaderBinary(1, &vs, 0, v + 4, (GLsizei)(vlen - 4));
	glShaderBinary(1, &fs, 0, f + 4, (GLsizei)(flen - 4));
	free(v);
	free(f);
	glCompileShader(vs);
	glCompileShader(fs);
	GLuint prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glLinkProgram(prog);
	GLint ok = 0;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	tlog("program %s: link=%d", tag, ok);
	if (!ok)
		return 0;
	return prog;
}

int main(void) {
	tlog("--- delayed program-mode test ---");
	sceKernelUsleep(5 * 1000 * 1000);
	tlog("delay done, opening VideoOut");
	if (!glShimInit(1920, 1080)) {
		tlog("shim init failed");
		return 1;
	}

	int w = 0, h = 0, n = 0;
	unsigned char* data =
	    stbi_load("/app0/assets/images/splash.png", &w, &h, &n, STBI_rgb_alpha);
	if (!data) {
		tlog("png load failed");
		return 1;
	}
	GLuint tex = 0;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
	stbi_image_free(data);
	tlog("texture %ux%u", w, h);

	const int push_mode = file_exists("/data/glshim-push.txt");
	const int quad_mode = file_exists("/data/glshim-quad.txt");
	tlog("mode: %s", push_mode ? "push-program" : quad_mode ? "quad-program" : "kodi-program");
	const int simple = push_mode || quad_mode;
	GLuint prog = push_mode
			  ? make_program_raw("/app0/assets/misc/push.vert.sb",
					     "/app0/assets/misc/quad.frag.sb", "push")
			  : quad_mode
			  ? make_program_raw("/app0/assets/misc/quad.vert.sb",
					     "/app0/assets/misc/quad.frag.sb", "quad")
			  : make_program("/app0/assets/misc/kodi_vert.bin",
					 "/app0/assets/misc/kodi_frag.bin", "kodi");
	if (!prog) {
		glShimShutdown();
		return 1;
	}
	glUseProgram(prog);

	GLint a_pos = simple ? 0 : glGetAttribLocation(prog, "m_attrpos");
	GLint a_col = simple ? -1 : glGetAttribLocation(prog, "m_attrcol");
	GLint a_c0 = simple ? 1 : glGetAttribLocation(prog, "m_attrcord0");
	GLint a_c1 = simple ? -1 : glGetAttribLocation(prog, "m_attrcord1");
	GLint a_snap = simple ? -1 : glGetAttribLocation(prog, "m_attrsnap");
	GLint u_matrix = push_mode ? 0 : glGetUniformLocation(prog, "m_matrix");
	GLint u_gui = glGetUniformLocation(prog, "m_gui");
	GLint u_c0m = glGetUniformLocation(prog, "m_coord0Matrix");
	GLint u_snap = glGetUniformLocation(prog, "m_snap");
	GLint u_depth = glGetUniformLocation(prog, "m_depth");
	GLint u_unicol = glGetUniformLocation(prog, "m_unicol");
	GLint u_peak = glGetUniformLocation(prog, "m_sdrPeak");
	GLint u_samp0 = glGetUniformLocation(prog, "m_samp0");
	tlog("loc: pos=%d col=%d c0=%d c1=%d snap=%d | matrix=%d gui=%d c0m=%d "
	     "snap=%d depth=%d unicol=%d peak=%d samp0=%d",
	     a_pos, a_col, a_c0, a_c1, a_snap, u_matrix, u_gui, u_c0m, u_snap, u_depth,
	     u_unicol, u_peak, u_samp0);

	glUniformMatrix4fv(u_matrix, 1, GL_FALSE, kIdent);
	glUniformMatrix4fv(u_gui, 1, GL_FALSE, kIdent);
	glUniformMatrix4fv(u_c0m, 1, GL_FALSE, kIdent);
	glUniform1f(u_snap, 0.0f);
	glUniform1f(u_depth, 0.0f);
	glUniform4f(u_unicol, 1.0f, 1.0f, 1.0f, 1.0f);
	glUniform1f(u_peak, 1.0f);
	glUniform1i(u_samp0, 0);

	const float u = (float)w / (float)((w + 63) & ~63);
	const float v = (float)h / (float)((h + 7) & ~7);
	Vertex verts[6];
	const float xs[6] = {-1.0f, 1.0f, 1.0f, -1.0f, 1.0f, -1.0f};
	const float ys[6] = {-1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f};
	const float us[6] = {0.0f, u, u, 0.0f, u, 0.0f};
	const float vs[6] = {v, v, 0.0f, v, 0.0f, 0.0f};
	for (int i = 0; i < 6; i++) {
		memset(&verts[i], 0, sizeof(Vertex));
		verts[i].pos[0] = xs[i];
		verts[i].pos[1] = ys[i];
		verts[i].pos[3] = 1.0f;
		verts[i].col[0] = verts[i].col[1] = verts[i].col[2] = verts[i].col[3] = 1.0f;
		verts[i].cord0[0] = us[i];
		verts[i].cord0[1] = vs[i];
		verts[i].cord1[0] = us[i];
		verts[i].cord1[1] = vs[i];
	}

	GLuint vbo = 0;
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
	if (a_pos >= 0) {
		glEnableVertexAttribArray((GLuint)a_pos);
		glVertexAttribPointer((GLuint)a_pos, simple ? 2 : 4, GL_FLOAT, GL_FALSE,
				      sizeof(Vertex), (const void*)offsetof(Vertex, pos));
	}
	if (a_col >= 0) {
		glEnableVertexAttribArray((GLuint)a_col);
		glVertexAttribPointer((GLuint)a_col, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex),
				      (const void*)offsetof(Vertex, col));
	}
	if (a_c0 >= 0) {
		glEnableVertexAttribArray((GLuint)a_c0);
		glVertexAttribPointer((GLuint)a_c0, simple ? 2 : 4, GL_FLOAT, GL_FALSE,
				      sizeof(Vertex), (const void*)offsetof(Vertex, cord0));
	}
	if (a_c1 >= 0) {
		glEnableVertexAttribArray((GLuint)a_c1);
		glVertexAttribPointer((GLuint)a_c1, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex),
				      (const void*)offsetof(Vertex, cord1));
	}
	if (a_snap >= 0) {
		glEnableVertexAttribArray((GLuint)a_snap);
		glVertexAttribPointer((GLuint)a_snap, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex),
				      (const void*)offsetof(Vertex, snap));
	}

	const int noblend = file_exists("/data/glshim-noblend.txt");
	const int blue = file_exists("/data/glshim-blue.txt");
	tlog("toggles: noblend=%d blue=%d", noblend, blue);
	if (blue)
		glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
	else
		glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
	if (!noblend)
		glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	for (unsigned long long frame = 0;; frame++) {
		glClear(GL_COLOR_BUFFER_BIT);
		glDrawArrays(GL_TRIANGLES, 0, 6);
		glShimPresent();
		GLenum err = glGetError();
		if (err != GL_NO_ERROR)
			tlog("frame %llu error %04x", frame, err);
		if (frame == 0)
			tlog("first frame ok");
		if ((frame % 300ull) == 0ull)
			tlog("frame %llu", frame);
	}
	glShimShutdown();
	return 0;
}
