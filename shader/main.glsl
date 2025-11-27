@ctype mat4  HMM_Mat4
@ctype vec4  HMM_Vec4
@ctype vec3  HMM_Vec3
@ctype vec2  HMM_Vec2

@module mmd

@vs vs
layout(binding=0) uniform vs_params {
    mat4 mvp;
    mat4 model;
};

in vec3 position;
in vec3 normal;
in vec2 texcoord0;

out vec2 uv;
out vec3 norm;
out vec3 world_pos;

void main() {
    vec4 world_pos4 = model * vec4(position, 1.0);
    world_pos = world_pos4.xyz;
    gl_Position = mvp * vec4(position, 1.0);
    uv = texcoord0;
    norm = mat3(transpose(inverse(model))) * normal; // Transform normal to world space
}
@end

@fs fs
in vec2 uv;
in vec3 norm;
in vec3 world_pos;
out vec4 frag_color;

layout(binding=0) uniform texture2D diffuse_texture;
layout(binding=0) uniform sampler diffuse_smp;

layout(binding=1) uniform fs_params {
    vec3 view_pos;
    // Rim light parameters
    float rim_power; // Rim light power (higher = sharper rim, typical: 2.0-5.0)
    float rim_intensity; // Rim light intensity (typical: 0.5-2.0)
    vec3 rim_color; // Rim light color (typically white or slightly tinted)
    // Specular highlight parameters
    float specular_power; // Specular highlight power (higher = sharper, typical: 32.0-128.0)
    float specular_intensity; // Specular highlight intensity (typical: 0.5-2.0)
    // Light parameters for specular calculation
    vec3 light_direction; // Light direction (normalized, points from light to surface)
    vec3 light_color; // Light color
    float light_intensity; // Light intensity
};


void main() {
    vec3 N = normalize(norm);
    vec3 V = normalize(view_pos - world_pos);
    vec3 L = normalize(-light_direction); // Light direction points from light to surface, so negate for direction to light
    
    // Sample diffuse texture (albedo)
    vec3 albedo = texture(sampler2D(diffuse_texture, diffuse_smp), uv).rgb;
    
    // Calculate Rim Light (edge highlight) - characteristic of figure/resin materials
    // Rim light appears at edges where surface is nearly perpendicular to view
    float NdotV = max(dot(N, V), 0.0);
    float rim_factor = 1.0 - NdotV; // 0 at center, 1 at edge
    rim_factor = pow(abs(rim_factor), rim_power); // Sharpen the rim
    vec3 rim_light = rim_color * rim_intensity * rim_factor;
    
    // Calculate Specular Highlight (Blinn-Phong model)
    // Half vector for Blinn-Phong
    vec3 H = normalize(V + L);
    float NdotH = max(dot(N, H), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    
    // Specular highlight only appears where surface faces the light
    float specular_factor = 0.0;
    if (NdotL > 0.0) {
        specular_factor = pow(abs(NdotH), specular_power);
    }
    vec3 specular_highlight = light_color * light_intensity * specular_intensity * specular_factor;
    
    // Final color: albedo + rim light + specular highlight
     vec3 final_color = albedo + rim_light + specular_highlight;

    // gamma
    // final_color = pow(abs(final_color), vec3(2.2));

    frag_color = vec4(final_color, 1.0);
}
@end

@program mmd vs fs

