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
out vec3 tangent_out;
out vec3 bitangent_out;

void main() {
    vec4 world_pos4 = model * vec4(position, 1.0);
    world_pos = world_pos4.xyz;
    gl_Position = mvp * vec4(position, 1.0);
    uv = texcoord0;
    norm = mat3(transpose(inverse(model))) * normal;
    
    vec3 local_tangent = vec3(0.0, 1.0, 0.0);
    tangent_out = normalize(mat3(model) * local_tangent);
    bitangent_out = normalize(cross(norm, tangent_out));
}
@end

@fs fs
in vec2 uv;
in vec3 norm;
in vec3 world_pos;
in vec3 tangent_out;
in vec3 bitangent_out;
out vec4 frag_color;

layout(binding=0) uniform texture2D diffuse_texture;
layout(binding=0) uniform sampler diffuse_smp;

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
    
    float is_stocking;
    float stocking_density;
    float stocking_sigma;
    
    float aniso_intensity;
    float aniso_power;
    float aniso_spread;
    float aniso_noise;
    
    float micro_normal_strength;
    float micro_roughness_var;
    float micro_scale;
    float micro_weave_scale;
    
    // Subsurface scattering parameters
    vec3 sss_color;           // Color of subsurface scattering (skin tone)
    float sss_intensity;      // SSS intensity (0.0 - 1.0)
    float sss_distortion;     // Light direction distortion for SSS
    float sss_power;          // Falloff power for SSS
    float sss_thickness;      // Simulated thickness (affects transmission)
    
    // Fresnel parameters
    float fresnel_power;      // Fresnel falloff power (typically 5.0)
    float fresnel_intensity;  // Fresnel reflection intensity
    float fresnel_bias;       // Minimum fresnel (F0 for dielectrics)
    
    // Stocking color tint
    vec3 stocking_tint_color; // Color to tint the stocking (black, nude, white, etc.)
    
    // Feature toggles for A/B comparison
    float enable_aniso;       // Enable anisotropic specular
    float enable_microsurface; // Enable microsurface noise
    float enable_sss;         // Enable subsurface scattering
    float enable_fresnel;     // Enable fresnel effect
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
// Noise functions
// ============================================
float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    
    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));
    
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float fbm(vec2 p, int octaves) {
    float value = 0.0;
    float amplitude = 0.5;
    float frequency = 1.0;
    float total_amplitude = 0.0;
    
    for (int i = 0; i < octaves; i++) {
        value += amplitude * noise(p * frequency);
        total_amplitude += amplitude;
        amplitude *= 0.5;
        frequency *= 2.0;
    }
    
    return value / total_amplitude;
}

// ============================================
// Microsurface functions
// ============================================
vec3 getMicrosurfaceNormal(vec2 tex_uv, vec3 N, vec3 T, vec3 B, float strength, float scale) {
    vec2 uv_scaled = tex_uv * scale;
    
    float eps = 0.01;
    float h = fbm(uv_scaled, 4);
    float hx = fbm(uv_scaled + vec2(eps, 0.0), 4);
    float hy = fbm(uv_scaled + vec2(0.0, eps), 4);
    
    float dx = (hx - h) / eps;
    float dy = (hy - h) / eps;
    
    vec3 perturbation = vec3(-dx * strength, -dy * strength, 1.0);
    perturbation = normalize(perturbation);
    
    mat3 TBN = mat3(T, B, N);
    return normalize(TBN * perturbation);
}

float getWeavePattern(vec2 tex_uv, float scale) {
    vec2 uv_scaled = tex_uv * scale;
    vec2 grid = fract(uv_scaled);
    
    float diamond = abs(grid.x - 0.5) + abs(grid.y - 0.5);
    diamond = smoothstep(0.3, 0.5, diamond);
    
    vec2 cell = floor(uv_scaled);
    float cell_var = hash(cell) * 0.3;
    
    return diamond + cell_var;
}

float getMicrosurfaceRoughness(vec2 tex_uv, float base_roughness, float variation, float scale, float weave_scale) {
    float noise1 = fbm(tex_uv * scale, 3);
    float noise2 = noise(tex_uv * scale * 3.0);
    float weave = getWeavePattern(tex_uv, weave_scale);
    
    float roughness_var = (noise1 * 0.6 + noise2 * 0.4) * variation;
    roughness_var += weave * variation * 0.3;
    
    return clamp(base_roughness + (roughness_var - 0.5) * variation, 0.05, 1.0);
}

float getMicroSparkle(vec2 tex_uv, vec3 V, vec3 L, vec3 N, float scale) {
    vec2 uv_high = tex_uv * scale * 10.0;
    
    float sparkle_noise = hash(floor(uv_high));
    
    vec3 H = normalize(V + L);
    float NdotH = max(dot(N, H), 0.0);
    
    float threshold = 0.85 + sparkle_noise * 0.1;
    float sparkle = smoothstep(threshold, threshold + 0.05, NdotH);
    
    float NdotV = max(dot(N, V), 0.0);
    sparkle *= smoothstep(0.0, 0.3, NdotV);
    sparkle *= hash(floor(uv_high) + vec2(17.3, 41.7));
    
    return sparkle;
}

// ============================================
// Anisotropic specular (Kajiya-Kay)
// ============================================
float KajiyaKaySpecular(vec3 T, vec3 H, float power) {
    float TdotH = dot(T, H);
    float sinTH = sqrt(max(0.0, 1.0 - TdotH * TdotH));
    return pow(sinTH, power);
}

vec3 ShiftTangent(vec3 T, vec3 N, float shift) {
    return normalize(T + N * shift);
}

// ============================================
// Fresnel effect (Schlick approximation)
// ============================================
float fresnelSchlick(float NdotV, float F0, float power) {
    return F0 + (1.0 - F0) * pow(max(1.0 - NdotV, 0.0), power);
}

vec3 fresnelSchlickVec3(float NdotV, vec3 F0, float power) {
    return F0 + (1.0 - F0) * pow(max(1.0 - NdotV, 0.0), power);
}

// ============================================
// Subsurface Scattering (SSS)
// Fast approximation for thin translucent materials
// ============================================
vec3 subsurfaceScattering(
    vec3 N, vec3 V, vec3 L, 
    vec3 lightColor, vec3 sssColor,
    float intensity, float distortion, float power, float thickness
) {
    // Distort light direction through the surface
    vec3 L_distorted = L + N * distortion;
    
    // Calculate view-dependent transmission
    float VdotL = max(dot(V, -L_distorted), 0.0);
    float transmission = pow(VdotL, power) * thickness;
    
    // Back-lighting contribution (light coming through from behind)
    float NdotL_back = max(dot(-N, L), 0.0);
    float backlight = pow(NdotL_back, power * 0.5) * thickness;
    
    // Combine transmission and backlight
    float sss_factor = (transmission + backlight * 0.5) * intensity;
    
    return lightColor * sssColor * sss_factor;
}

// Enhanced SSS with wrap lighting for stocking
vec3 stockingSSS(
    vec3 N, vec3 V, vec3 L,
    vec3 albedo, vec3 lightColor, vec3 sssColor,
    float intensity, float distortion, float power, float thickness,
    float NdotV
) {
    // Basic SSS transmission
    vec3 sss = subsurfaceScattering(N, V, L, lightColor, sssColor, intensity, distortion, power, thickness);
    
    // Wrap lighting for soft shadow transition
    float NdotL = dot(N, L);
    float wrap = 0.5;
    float wrapDiffuse = max(0.0, (NdotL + wrap) / (1.0 + wrap));
    
    // Edge-enhanced SSS (more visible at grazing angles where stocking is denser)
    float edgeFactor = 1.0 - NdotV;
    float edgeSSS = pow(edgeFactor, 2.0) * intensity * 0.5;
    
    // Blend skin color through stocking based on view angle
    // At center (high NdotV): more skin shows through
    // At edges (low NdotV): stocking color dominates
    vec3 skinBleed = albedo * sssColor * wrapDiffuse * intensity * NdotV;
    
    return sss + skinBleed + lightColor * sssColor * edgeSSS;
}

// ============================================
// Stocking effect (MME Stockingize style)
// ============================================
float Gaussian(float x, float sigma) {
    return exp(-(x * x) / (2.0 * sigma * sigma));
}

vec4 AlphaBlend(vec4 bg, vec4 fg) {
    float a1 = bg.a;
    float a2 = fg.a;
    float out_alpha = a1 + a2 - a1 * a2;
    
    if (out_alpha < 0.001) return bg;
    
    vec3 out_rgb = (a2 * fg.rgb + (1.0 - a2) * a1 * bg.rgb) / out_alpha;
    return vec4(out_rgb, out_alpha);
}

vec4 ApplyStocking(vec4 base_color, vec3 N, vec3 V, vec2 tex_uv, float density, float sigma, float fresnel_factor, vec3 tint_color) {
    float NdotV = dot(N, V);
    float gaussian_val = Gaussian(NdotV, sigma);
    float u = 1.0 - gaussian_val;
    
    vec4 stocking_sample = texture(sampler2D(stocking_texture, stocking_smp), vec2(u, tex_uv.y));
    
    // The texture is grayscale - use it as intensity and apply tint color
    // Darker areas of the texture = more stocking color visible
    // Lighter areas = more transparent
    float gray = stocking_sample.r; // Grayscale intensity
    
    // Invert so darker texture = more opaque stocking
    float opacity = 1.0 - gray;
    
    // Apply tint color to the stocking
    vec3 tinted_color = tint_color * (1.0 - gray * 0.5); // Preserve some shading variation
    
    // Modulate density with fresnel - edges are more opaque
    float fresnel_density = mix(density * 0.7, density, fresnel_factor);
    float final_alpha = opacity * fresnel_density * stocking_sample.a;
    
    vec4 stocking_color = vec4(tinted_color, final_alpha);
    
    return AlphaBlend(base_color, stocking_color);
}

vec3 StockingAnisotropicSpecular(
    vec3 N, vec3 V, vec3 L, vec3 T,
    vec2 tex_uv, vec3 light_col,
    float intensity, float power, float spread, float noise_amount,
    float roughness_variation
) {
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    
    if (NdotL <= 0.0 || intensity <= 0.0) return vec3(0.0);
    
    float n = noise(tex_uv * 50.0) * 2.0 - 1.0;
    vec3 T_noisy = normalize(T + N * n * noise_amount * 0.3);
    
    float local_power = power * (1.0 + (roughness_variation - 0.5) * 0.5);
    
    vec3 T1 = ShiftTangent(T_noisy, N, -spread * 0.5);
    float spec1 = KajiyaKaySpecular(T1, H, local_power);
    
    vec3 T2 = ShiftTangent(T_noisy, N, spread * 0.5);
    float spec2 = KajiyaKaySpecular(T2, H, local_power * 0.5) * 0.5;
    
    float NdotV = max(dot(N, V), 0.0);
    float fresnel = pow(1.0 - NdotV, 3.0) * 0.5;
    
    float noise_var = 1.0 + (noise(tex_uv * 100.0) - 0.5) * noise_amount;
    
    float total_spec = (spec1 + spec2 + fresnel) * noise_var * NdotL;
    
    return light_col * intensity * total_spec;
}

void main() {
    vec3 N = normalize(norm);
    vec3 V = normalize(view_pos - world_pos);
    vec3 L = normalize(-light_direction);
    vec3 T = normalize(tangent_out - N * dot(tangent_out, N));
    vec3 B = normalize(bitangent_out);
    
    vec4 albedo_sample = texture(sampler2D(diffuse_texture, diffuse_smp), uv);
    vec3 albedo = albedo_sample.rgb;
    float alpha = albedo_sample.a;
    
    vec3 N_surface = N;
    float roughness_var = 0.5;
    
    // Microsurface noise (controlled by toggle)
    if (is_stocking > 0.5 && enable_microsurface > 0.5 && micro_normal_strength > 0.0) {
        N_surface = getMicrosurfaceNormal(uv, N, T, B, micro_normal_strength, micro_scale);
        roughness_var = getMicrosurfaceRoughness(uv, 0.5, micro_roughness_var, micro_scale, micro_weave_scale);
    }
    
    float NdotV = max(dot(N_surface, V), 0.0);
    float NdotL = max(dot(N_surface, L), 0.0);
    vec3 H = normalize(V + L);
    float NdotH = max(dot(N_surface, H), 0.0);
    
    // Calculate Fresnel for stocking (controlled by toggle)
    float fresnel_factor = 0.0;
    if (is_stocking > 0.5 && enable_fresnel > 0.5) {
        fresnel_factor = fresnelSchlick(NdotV, fresnel_bias, fresnel_power);
    }
    
    // Rim Light
    float rim_factor = 1.0 - NdotV;
    rim_factor = pow(abs(rim_factor), rim_power);
    vec3 rim_light = rim_color * rim_intensity * rim_factor;
    
    // Specular Highlight (Blinn-Phong)
    float specular_factor = 0.0;
    if (NdotL > 0.0) {
        float adjusted_power = specular_power;
        if (is_stocking > 0.5) {
            adjusted_power *= (1.0 + (roughness_var - 0.5) * micro_roughness_var);
        }
        specular_factor = pow(abs(NdotH), adjusted_power);
    }
    vec3 specular_highlight = light_color * light_intensity * specular_intensity * specular_factor;
    
    // Diffuse lighting
    const float diffuse_strength = 0.25;
    vec3 diffuse_light = light_color * light_intensity * diffuse_strength * max(NdotL, 0.0);
    
    // Base lit color
    vec3 lit_color = albedo * (vec3(0.9) + diffuse_light) + rim_light + specular_highlight;
    vec4 final_color = vec4(lit_color, alpha);
    
    // Apply stocking effects
    if (is_stocking > 0.5) {
        // Subsurface scattering - skin showing through stocking (controlled by toggle)
        if (enable_sss > 0.5) {
            vec3 sss = stockingSSS(
                N_surface, V, L,
                albedo, light_color * light_intensity, sss_color,
                sss_intensity, sss_distortion, sss_power, sss_thickness,
                NdotV
            );
            final_color.rgb += sss;
        }
        
        // Apply stocking texture with fresnel-modulated density and color tint
        final_color = ApplyStocking(final_color, N_surface, V, uv, stocking_density, stocking_sigma, fresnel_factor, stocking_tint_color);
        
        // Fresnel reflection overlay (shiny edges) (controlled by toggle)
        if (enable_fresnel > 0.5) {
            vec3 fresnel_reflection = light_color * fresnel_factor * fresnel_intensity;
            final_color.rgb += fresnel_reflection;
        }
        
        // Anisotropic specular for fiber shimmer (controlled by toggle)
        if (enable_aniso > 0.5) {
            vec3 aniso_spec = StockingAnisotropicSpecular(
                N_surface, V, L, T, uv,
                light_color * light_intensity,
                aniso_intensity,
                aniso_power,
                aniso_spread,
                aniso_noise,
                roughness_var
            );
            final_color.rgb += aniso_spec;
            
            // Micro sparkle (part of anisotropic effect)
            float sparkle = getMicroSparkle(uv, V, L, N_surface, micro_scale);
            final_color.rgb += light_color * light_intensity * sparkle * aniso_intensity * 0.3;
        }
        
        // Weave pattern color variation (part of microsurface effect)
        if (enable_microsurface > 0.5) {
            float weave = getWeavePattern(uv, micro_weave_scale);
            final_color.rgb *= 1.0 + (weave - 0.5) * micro_roughness_var * 0.1;
        }
    }
    
    // Gamma correction
    final_color.rgb = SrgbToLinear(final_color.rgb);
    
    frag_color = final_color;
}
@end

@program mmd vs fs
