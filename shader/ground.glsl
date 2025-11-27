@ctype mat4  HMM_Mat4
@ctype vec4  HMM_Vec4
@ctype vec3  HMM_Vec3
@ctype vec2  HMM_Vec2

@module ground

@vs ground_vs
layout(binding=0) uniform vs_params {
    mat4 mvp;
    mat4 model;
    mat4 light_mvp; // Light space MVP for shadow mapping
};

in vec3 position;
in vec3 normal;
in vec2 texcoord0;

out vec2 uv;
out vec3 norm;
out vec3 world_pos;
out vec4 light_space_pos; // Position in light space for shadow mapping

void main() {
    vec4 world_pos4 = model * vec4(position, 1.0);
    world_pos = world_pos4.xyz;
    gl_Position = mvp * vec4(position, 1.0);
    uv = texcoord0;
    norm = mat3(transpose(inverse(model))) * normal; // Transform normal to world space
    light_space_pos = light_mvp * vec4(position, 1.0); // Transform to light space
}
@end

@fs ground_fs
in vec2 uv;
in vec3 norm;
in vec3 world_pos;
in vec4 light_space_pos;
out vec4 frag_color;

// Diffuse texture (albedo)
layout(binding=2) uniform texture2D diffuse_texture;
layout(binding=2) uniform sampler diffuse_smp;

// Shadow map
layout(binding=3) uniform texture2D shadow_map;
layout(binding=3) uniform sampler shadow_smp;

layout(binding=1) uniform fs_params {
    float shadows_enabled;
    float receive_shadows;
};

// PCF shadow calculation
float CalculateShadow(vec4 light_space_pos) {
    if (shadows_enabled < 0.5 || receive_shadows < 0.5) {
        return 1.0; // No shadow
    }
    
    // Perspective divide
    vec3 proj_coords = light_space_pos.xyz / light_space_pos.w;
    
    // Transform to [0,1] range (NDC [-1,1] -> texture [0,1])
    proj_coords = proj_coords * 0.5 + 0.5;
    
    // Y-flip for D3D11/Metal (not needed for OpenGL)
    // In DX11, NDC Y is flipped compared to OpenGL, but shadow map is rendered in OpenGL style
    // So we need to flip Y after converting to [0,1] range
    #if !defined(SOKOL_GLSL)
    proj_coords.y = 1.0 - proj_coords.y;
    #endif
    
    // Check if position is outside shadow map bounds
    if (proj_coords.x < -0.001 || proj_coords.x > 1.001 ||
        proj_coords.y < -0.001 || proj_coords.y > 1.001 ||
        proj_coords.z < 0.0 || proj_coords.z > 1.001) {
        return 1.0; // Outside shadow map, no shadow
    }
    
    // Clamp to valid range
    proj_coords.xy = clamp(proj_coords.xy, 0.0, 1.0);
    
    // PCF (Percentage Closer Filtering) - 3x3 kernel (manually unrolled to avoid D3D11 gradient warning)
    float shadow = 0.0;
    #if !defined(SOKOL_GLSL)
    float bias = 0.001; // Shadow bias to prevent shadow acne
    #else
    float bias = -0.001; // Shadow bias to prevent shadow acne
    #endif
    vec2 texel_size = 1.0 / vec2(textureSize(sampler2DShadow(shadow_map, shadow_smp), 0));
    
    // Manually unroll the 3x3 loop to avoid gradient instruction warnings in D3D11
    // Top row
    shadow += texture(sampler2DShadow(shadow_map, shadow_smp), vec3(proj_coords.xy + vec2(-1.0, -1.0) * texel_size, proj_coords.z - bias));
    shadow += texture(sampler2DShadow(shadow_map, shadow_smp), vec3(proj_coords.xy + vec2(0.0, -1.0) * texel_size, proj_coords.z - bias));
    shadow += texture(sampler2DShadow(shadow_map, shadow_smp), vec3(proj_coords.xy + vec2(1.0, -1.0) * texel_size, proj_coords.z - bias));
    
    // Middle row
    shadow += texture(sampler2DShadow(shadow_map, shadow_smp), vec3(proj_coords.xy + vec2(-1.0, 0.0) * texel_size, proj_coords.z - bias));
    shadow += texture(sampler2DShadow(shadow_map, shadow_smp), vec3(proj_coords.xy + vec2(0.0, 0.0) * texel_size, proj_coords.z - bias));
    shadow += texture(sampler2DShadow(shadow_map, shadow_smp), vec3(proj_coords.xy + vec2(1.0, 0.0) * texel_size, proj_coords.z - bias));
    
    // Bottom row
    shadow += texture(sampler2DShadow(shadow_map, shadow_smp), vec3(proj_coords.xy + vec2(-1.0, 1.0) * texel_size, proj_coords.z - bias));
    shadow += texture(sampler2DShadow(shadow_map, shadow_smp), vec3(proj_coords.xy + vec2(0.0, 1.0) * texel_size, proj_coords.z - bias));
    shadow += texture(sampler2DShadow(shadow_map, shadow_smp), vec3(proj_coords.xy + vec2(1.0, 1.0) * texel_size, proj_coords.z - bias));
    
    return clamp(shadow / 9.0, 0.0, 1.0);
}

void main() {
    // Sample textures
    vec3 albedo = texture(sampler2D(diffuse_texture, diffuse_smp), uv).rgb * 0.7;

    // Calculate shadow
    float shadow = CalculateShadow(light_space_pos);

    frag_color = vec4(albedo, 1.0) * shadow;
}
@end

@program ground ground_vs ground_fs

