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

layout(binding=0) uniform texture2D diffuse_texture;
layout(binding=0) uniform sampler diffuse_smp;

layout(binding=1) uniform textureCube environment_map;
layout(binding=1) uniform sampler environment_smp;

layout(binding=2) uniform fs_params {
    vec3 view_pos;
    float _pad0;
    
    vec3 light_direction;
    float light_intensity;
    vec3 light_color;
    float _pad1;
    
    float roughness;
    float metallic;
    float subsurface;
    float clearcoat;
    
    float clearcoat_roughness;
    float rim_power;
    float rim_intensity;
    float ambient_intensity;
    
    vec3 rim_color;
    float _pad2;
    
    vec3 subsurface_color;
    float env_reflection_intensity;
};

const float PI = 3.14159265359;

// ============================================
// Gamma Conversion
// ============================================
vec3 GammaToLinear(vec3 color) {
    return pow(max(color, vec3(0.0)), vec3(2.2));
}

vec3 LinearToGamma(vec3 color) {
    return pow(max(color, vec3(0.0)), vec3(1.0 / 2.2));
}

// ============================================
// GGX Distribution
// ============================================
float D_GGX(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d + 0.0001);
}

// ============================================
// Visibility (Smith GGX)
// ============================================
float V_SmithGGX(float NdotV, float NdotL, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float GGXV = NdotL * sqrt(NdotV * NdotV * (1.0 - a2) + a2);
    float GGXL = NdotV * sqrt(NdotL * NdotL * (1.0 - a2) + a2);
    return 0.5 / (GGXV + GGXL + 0.0001);
}

// ============================================
// Fresnel - Schlick with roughness
// ============================================
vec3 F_Schlick(float VdotH, vec3 F0) {
    return F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);
}

vec3 F_SchlickRoughness(float NdotV, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(1.0 - NdotV, 5.0);
}

// ============================================
// Plastic/Figure BRDF
// Key: dual-lobe specular + colored subsurface
// ============================================
vec3 PlasticBRDF(
    vec3 N, vec3 V, vec3 L, vec3 H,
    float NdotV, float NdotL, float NdotH, float VdotH,
    vec3 albedo, float rough, float cc, float ccRough
) {
    // === Layer 1: Base plastic specular ===
    // Plastic F0 is around 0.04-0.05, but we tint it slightly with albedo
    vec3 F0_base = mix(vec3(0.04), albedo * 0.15 + 0.04, 0.3);
    
    float D1 = D_GGX(NdotH, rough);
    float V1 = V_SmithGGX(NdotV, NdotL, rough);
    vec3 F1 = F_Schlick(VdotH, F0_base);
    
    vec3 spec_base = D1 * V1 * F1;
    
    // === Layer 2: Clear coat (sharper, on top) ===
    vec3 spec_coat = vec3(0.0);
    if (cc > 0.001) {
        float D2 = D_GGX(NdotH, ccRough);
        float V2 = V_SmithGGX(NdotV, NdotL, ccRough);
        float F2 = 0.04 + 0.96 * pow(1.0 - VdotH, 5.0);
        spec_coat = vec3(D2 * V2 * F2 * cc);
    }
    
    // Combine: base spec + clear coat
    // Clear coat attenuates base layer slightly
    vec3 specular = spec_base * (1.0 - cc * 0.5) + spec_coat;
    
    return specular;
}

// ============================================
// Subsurface Scattering for Plastic
// Simulates light penetrating and scattering inside
// ============================================
vec3 PlasticSSS(
    vec3 N, vec3 L, vec3 V,
    float NdotL, float NdotV,
    vec3 albedo, vec3 sssColor, float sssIntensity
) {
    if (sssIntensity < 0.001) return vec3(0.0);
    
    // Wrap diffuse - key for plastic's soft shadow transition
    float wrap = 0.5;
    float wrapDiffuse = (NdotL + wrap) / (1.0 + wrap);
    wrapDiffuse = max(0.0, wrapDiffuse);
    wrapDiffuse = wrapDiffuse * wrapDiffuse; // Soften
    
    // Transmittance (back-lighting through thin parts)
    float backLight = pow(max(0.0, dot(V, -L)), 3.0) * 0.4;
    
    // View-dependent scattering (more visible at grazing angles)
    float viewScatter = (1.0 - NdotV) * 0.3;
    
    // Combine SSS components
    float sssFactor = (wrapDiffuse * 0.7 + backLight + viewScatter) * sssIntensity;
    vec3 sss = albedo * sssColor * sssFactor;
    
    return sss;
}

// ============================================
// Edge Darkening (plastic depth absorption)
// Light travels further at edges, absorbing more
// ============================================
vec3 EdgeDarkening(vec3 albedo, float NdotV, float intensity) {
    // At grazing angles, light travels through more material
    float edgeFactor = 1.0 - NdotV;
    edgeFactor = edgeFactor * edgeFactor; // Square for smooth falloff
    
    // Darken and saturate at edges
    vec3 darkened = albedo * albedo; // Darken by squaring
    vec3 result = mix(albedo, darkened, edgeFactor * intensity);
    
    return result;
}

// ============================================
// Soft Tone Mapping (preserves plastic look)
// ============================================
vec3 ToneMap(vec3 color) {
    // Reinhard with white point
    float whitePoint = 2.0;
    vec3 mapped = color * (1.0 + color / (whitePoint * whitePoint)) / (1.0 + color);
    return mapped;
}

void main() {
    vec3 N = normalize(norm);
    vec3 V = normalize(view_pos - world_pos);
    vec3 L = normalize(-light_direction);
    vec3 H = normalize(V + L);
    vec3 R = reflect(-V, N);
    
    // Sample albedo
    vec4 albedoSample = texture(sampler2D(diffuse_texture, diffuse_smp), uv);
    vec3 albedo = GammaToLinear(albedoSample.rgb);
    float alpha = albedoSample.a;
    
    // Dot products (clamped)
    float NdotV = max(dot(N, V), 0.001);
    float NdotL = max(dot(N, L), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.001);
    
    // === Apply edge darkening to albedo (plastic depth effect) ===
    vec3 plasticAlbedo = EdgeDarkening(albedo, NdotV, 0.4);
    
    // === Diffuse with energy conservation ===
    // Plastic diffuse: use modified albedo, account for specular energy loss
    vec3 F0 = vec3(0.04);
    vec3 F = F_Schlick(VdotH, F0);
    vec3 kD = (1.0 - F) * (1.0 - metallic);
    
    // Diffuse term (NOT divided by PI for brighter result)
    vec3 diffuse = kD * plasticAlbedo;
    
    // === Specular (dual-lobe plastic BRDF) ===
    vec3 specular = PlasticBRDF(
        N, V, L, H,
        NdotV, NdotL, NdotH, VdotH,
        albedo, roughness, clearcoat, clearcoat_roughness
    );
    
    // === Direct lighting ===
    vec3 radiance = light_color * light_intensity;
    
    // Soft shadow transition using modified Lambert
    float shadow = NdotL;
    // Add slight wrap for softer transition
    shadow = shadow * 0.8 + 0.2 * max(0.0, (NdotL + 0.3) / 1.3);
    
    vec3 directLight = (diffuse * shadow + specular * NdotL) * radiance;
    
    // === Subsurface Scattering ===
    vec3 sss = PlasticSSS(N, L, V, NdotL, NdotV, plasticAlbedo, subsurface_color, subsurface);
    sss *= radiance;
    
    // === Rim Light (figure photography style) ===
    float rim = pow(1.0 - NdotV, rim_power);
    // Modulate by light facing for natural look
    float rimLightFacing = max(0.2, NdotL * 0.5 + 0.5);
    vec3 rimLight = rim_color * rim * rim_intensity * rimLightFacing;
    // Tint rim with albedo slightly for plastic look
    rimLight *= mix(vec3(1.0), albedo, 0.3);
    
    // === Environment Reflection ===
    vec3 envColor = texture(samplerCube(environment_map, environment_smp), R).rgb;
    
    // Fresnel for environment (stronger at edges = plastic shine)
    vec3 F_env = F_SchlickRoughness(NdotV, F0, roughness);
    // Plastic reflects more at edges
    float envFresnel = 0.04 + 0.96 * pow(1.0 - NdotV, 4.0);
    envFresnel *= (1.0 - roughness * 0.5); // Less reflection when rough
    
    vec3 envReflection = envColor * envFresnel * env_reflection_intensity;
    
    // Clear coat env reflection (sharper, more visible)
    vec3 ccEnvReflection = vec3(0.0);
    if (clearcoat > 0.001) {
        float ccFresnel = 0.04 + 0.96 * pow(1.0 - NdotV, 5.0);
        ccEnvReflection = envColor * ccFresnel * clearcoat * env_reflection_intensity * 0.5;
    }
    
    // === Ambient (hemisphere lighting) ===
    float hemi = N.y * 0.5 + 0.5;
    vec3 skyAmbient = vec3(0.7, 0.8, 1.0);   // Cool sky
    vec3 groundAmbient = vec3(0.4, 0.35, 0.3); // Warm ground
    vec3 ambientColor = mix(groundAmbient, skyAmbient, hemi);
    vec3 ambient = plasticAlbedo * ambientColor * ambient_intensity;
    
    // === Final Composition ===
    vec3 finalColor = vec3(0.0);
    
    // Base lighting
    finalColor += directLight;
    finalColor += sss;
    finalColor += ambient;
    
    // Rim light (on top, characteristic of figure photos)
    finalColor += rimLight;
    
    // Environment reflections
    finalColor += envReflection * (1.0 - clearcoat * 0.3);
    finalColor += ccEnvReflection;
    
    // === Post Processing ===
    // Tone mapping
    finalColor = ToneMap(finalColor);
    
    // Subtle saturation boost (figures are vibrant)
    float luma = dot(finalColor, vec3(0.2126, 0.7152, 0.0722));
    finalColor = mix(vec3(luma), finalColor, 1.15);
    
    // Gamma correction
    finalColor = LinearToGamma(finalColor);
    
    // Clamp
    finalColor = clamp(finalColor, 0.0, 1.0);
    
    frag_color = vec4(finalColor, alpha);
}
@end

@program mmd vs fs

