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

float LinearToSrgb(float channel) {
    if (channel <= 0.0031308f) {
        return 12.92f * channel;
    } else {
        return 1.055f * pow(abs(channel), 1.0f / 2.4f) - 0.055f;
    }
}

vec3 LinearToSrgb(vec3 linear) {
    return vec3(LinearToSrgb(linear.r), LinearToSrgb(linear.g), LinearToSrgb(linear.b));
}

float SrgbToLinear(float channel) {
    if (channel <= 0.04045f) {
        return channel / 12.92f;
    } else {
        return pow(abs((channel + 0.055f) / 1.055f), 2.4f);
    }
}

vec3 SrgbToLinear(vec3 srgb) {
    return vec3(SrgbToLinear(srgb.r), SrgbToLinear(srgb.g), SrgbToLinear(srgb.b));
}

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
    
    // Weak diffuse lighting (hardcoded, no shader parameters)
    // Provides subtle base illumination to prevent overly dark areas
    const float diffuse_strength = 0.25; // Weak diffuse intensity (hardcoded)
    vec3 diffuse_light = light_color * light_intensity * diffuse_strength * max(NdotL, 0.0);
    
    // Final color: albedo with diffuse lighting + rim light + specular highlight
    vec3 final_color = albedo * (vec3(0.9) + diffuse_light) + rim_light + specular_highlight;

    // gamma
    final_color = SrgbToLinear(final_color);

    frag_color = vec4(final_color, 1.0);
}
@end

@program mmd vs fs

