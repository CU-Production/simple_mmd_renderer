@ctype mat4  HMM_Mat4
@ctype vec4  HMM_Vec4
@ctype vec3  HMM_Vec3
@ctype vec2  HMM_Vec2

@module ibl

// Equirectangular to cubemap conversion shader
@vs equirect_to_cubemap_vs
layout(binding=0) uniform vs_params {
    mat4 mvp;
};

in vec3 position;
out vec3 world_pos;

void main() {
    gl_Position = mvp * vec4(position, 1.0);
    world_pos = position;
}
@end

@fs equirect_to_cubemap_fs
in vec3 world_pos;
out vec4 frag_color;

layout(binding=0) uniform texture2D equirectangular_map;
layout(binding=0) uniform sampler equirectangular_smp;

const float PI = 3.14159265359;

vec2 SampleSphericalMap(vec3 v) {
    vec2 uv = vec2(atan(v.z, v.x), asin(v.y));
    uv *= vec2(0.1591, 0.3183); // 1 / (2 * PI), 1 / PI
    uv += 0.5;
    return uv;
}

void main() {
    vec2 uv = SampleSphericalMap(normalize(world_pos));
    vec3 color = texture(sampler2D(equirectangular_map, equirectangular_smp), uv).rgb;
    frag_color = vec4(color, 1.0);
}
@end

@program equirect_to_cubemap equirect_to_cubemap_vs equirect_to_cubemap_fs

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

void main() {
    frag_color = vec4(texture(samplerCube(environment_map, environment_smp), tex_coords).rgb, 1.0);
}
@end

@program skybox skybox_vs skybox_fs

