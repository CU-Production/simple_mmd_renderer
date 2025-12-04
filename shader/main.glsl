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
    norm = mat3(transpose(inverse(model))) * normal;
}
@end

@fs fs
in vec2 uv;
in vec3 norm;
in vec3 world_pos;
out vec4 frag_color;

// Diffuse texture (slot 0)
layout(binding=0) uniform texture2D diffuse_texture;
layout(binding=0) uniform sampler diffuse_smp;

// Stocking texture (slot 1) - grey2.png pattern
layout(binding=1) uniform texture2D stocking_texture;
layout(binding=1) uniform sampler stocking_smp;

layout(binding=2) uniform fs_params {
    vec3 view_pos;
    float rim_power;
    
    float rim_intensity;
    vec3 rim_color;
    
    float specular_power;
    float specular_intensity;
    vec3 light_direction;
    
    vec3 light_color;
    float light_intensity;
    
    // Stocking effect parameters
    float is_stocking;      // 1.0 if this part should have stocking effect, 0.0 otherwise
    float stocking_density; // Stocking opacity/density (0.0 - 1.0)
    float stocking_sigma;   // Gaussian sigma for edge detection (typical: 1.4)
    float _pad0;
};

// ============================================
// Color space conversion
// ============================================
float LinearToSrgb(float channel) {
    if (channel <= 0.0031308) {
        return 12.92 * channel;
    } else {
        return 1.055 * pow(abs(channel), 1.0 / 2.4) - 0.055;
    }
}

vec3 LinearToSrgb(vec3 linear) {
    return vec3(LinearToSrgb(linear.r), LinearToSrgb(linear.g), LinearToSrgb(linear.b));
}

float SrgbToLinear(float channel) {
    if (channel <= 0.04045) {
        return channel / 12.92;
    } else {
        return pow(abs((channel + 0.055) / 1.055), 2.4);
    }
}

vec3 SrgbToLinear(vec3 srgb) {
    return vec3(SrgbToLinear(srgb.r), SrgbToLinear(srgb.g), SrgbToLinear(srgb.b));
}

// ============================================
// Stocking effect (MME Stockingize style)
// ============================================
float Gaussian(float x, float sigma) {
    // Gaussian function: exp(-(x^2)/(2*sigma^2))
    return exp(-(x * x) / (2.0 * sigma * sigma));
}

vec4 AlphaBlend(vec4 bg, vec4 fg) {
    // Standard alpha blending
    float a1 = bg.a;
    float a2 = fg.a;
    float out_alpha = a1 + a2 - a1 * a2;
    
    if (out_alpha < 0.001) return bg;
    
    vec3 out_rgb = (a2 * fg.rgb + (1.0 - a2) * a1 * bg.rgb) / out_alpha;
    return vec4(out_rgb, out_alpha);
}

vec4 ApplyStocking(vec4 base_color, vec3 N, vec3 V, vec2 tex_uv, float density, float sigma) {
    // Calculate view-dependent stocking intensity
    // At grazing angles (edges), stocking appears darker/denser
    // At normal view angles, stocking appears lighter/more transparent
    float NdotV = dot(N, V);
    
    // Gaussian falloff based on NdotV
    // When NdotV is high (facing camera), Gaussian is high, u is low (light stocking)
    // When NdotV is low (edge), Gaussian is low, u is high (dark stocking)
    float gaussian_val = Gaussian(NdotV, sigma);
    float u = 1.0 - gaussian_val;
    
    // Sample stocking texture
    // U coordinate: based on view angle (0 = edge/dark, 1 = center/light)
    // V coordinate: use original texture Y coordinate for vertical pattern
    vec4 stocking_color = texture(sampler2D(stocking_texture, stocking_smp), vec2(u, tex_uv.y));
    
    // Apply density control
    stocking_color.a *= density;
    
    // Blend stocking over base color
    return AlphaBlend(base_color, stocking_color);
}

void main() {
    vec3 N = normalize(norm);
    vec3 V = normalize(view_pos - world_pos);
    vec3 L = normalize(-light_direction);
    
    // Sample diffuse texture (albedo)
    vec4 albedo_sample = texture(sampler2D(diffuse_texture, diffuse_smp), uv);
    vec3 albedo = albedo_sample.rgb;
    float alpha = albedo_sample.a;
    
    // Calculate Rim Light
    float NdotV = max(dot(N, V), 0.0);
    float rim_factor = 1.0 - NdotV;
    rim_factor = pow(abs(rim_factor), rim_power);
    vec3 rim_light = rim_color * rim_intensity * rim_factor;
    
    // Calculate Specular Highlight (Blinn-Phong)
    vec3 H = normalize(V + L);
    float NdotH = max(dot(N, H), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    
    float specular_factor = 0.0;
    if (NdotL > 0.0) {
        specular_factor = pow(abs(NdotH), specular_power);
    }
    vec3 specular_highlight = light_color * light_intensity * specular_intensity * specular_factor;
    
    // Diffuse lighting
    const float diffuse_strength = 0.25;
    vec3 diffuse_light = light_color * light_intensity * diffuse_strength * max(NdotL, 0.0);
    
    // Combine base lighting
    vec3 lit_color = albedo * (vec3(0.9) + diffuse_light) + rim_light + specular_highlight;
    vec4 final_color = vec4(lit_color, alpha);
    
    // Apply stocking effect if this part is marked
    if (is_stocking > 0.5) {
        final_color = ApplyStocking(final_color, N, V, uv, stocking_density, stocking_sigma);
    }
    
    // Gamma correction
    final_color.rgb = SrgbToLinear(final_color.rgb);
    
    frag_color = final_color;
}
@end

@program mmd vs fs

