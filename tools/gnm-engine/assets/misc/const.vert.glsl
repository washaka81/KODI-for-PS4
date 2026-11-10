#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in vec2 inpos;
layout(location = 1) in vec2 inuv;

layout(location = 0) out vec2 outuv;

layout(set = 0, binding = 0) uniform Mat {
	mat4 m;
} ubo;

out gl_PerVertex {
	vec4 gl_Position;
};

void main() {
	gl_Position = ubo.m * vec4(inpos, 0.0, 1.0);
	outuv = inuv;
}
