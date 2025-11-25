@ctype mat4  HMM_Mat4
@ctype vec4  HMM_Vec4
@ctype vec3  HMM_Vec3
@ctype vec2  HMM_Vec2

@module mmd

@vs vs
layout(binding=0) uniform vs_params {
    mat4 mvp;
};

in vec3 position;
in vec3 normal;
in vec2 texcoord0;

out vec2 uv;
out vec3 norm;

void main() {
    gl_Position = mvp * vec4(position, 1.0);
    uv = texcoord0;
    norm = normal;
}
@end

@fs fs
in vec2 uv;
in vec3 norm;
out vec4 frag_color;

void main() {
    vec3 light_dir = normalize(vec3(1.0, 1.0, 1.0));
    float ndotl = max(dot(normalize(norm), light_dir), 0.3);
    frag_color = vec4(vec3(0.8, 0.8, 0.9) * ndotl, 1.0);
}
@end

@program mmd vs fs

