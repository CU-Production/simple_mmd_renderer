@ctype mat4  HMM_Mat4
@ctype vec4  HMM_Vec4
@ctype vec3  HMM_Vec3
@ctype vec2  HMM_Vec2

@module ibl

// Skybox shader
@vs skybox_vs
layout(binding=0) uniform vs_params {
    mat4 mvp;
};

in vec3 position;
out vec3 tex_coords;

void main() {
    tex_coords = position;
    vec4 pos = mvp * vec4(position, 1.0);
    gl_Position = pos.xyww; // Ensure depth is always 1.0 (furthest)
}
@end

@fs skybox_fs
in vec3 tex_coords;
out vec4 frag_color;

layout(binding=0) uniform textureCube environment_map;
layout(binding=0) uniform sampler environment_smp;

// ACES Filmic tonemapping (approximation)
// This provides better visual results than simple Reinhard
vec3 ACESFilmicToneMap(vec3 x) {
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 hdr_color = texture(samplerCube(environment_map, environment_smp), tex_coords).rgb;
    
    // Apply ACES Filmic tonemapping for better visual quality
    // This provides more natural color reproduction and better highlight handling
    vec3 ldr_color = ACESFilmicToneMap(hdr_color);
    
    frag_color = vec4(ldr_color, 1.0);
}
@end

@program skybox skybox_vs skybox_fs

