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

// Diffuse texture
layout(binding=0) uniform texture2D diffuse_texture;
layout(binding=0) uniform sampler diffuse_smp;

// Prefiltered environment map (with mip levels for roughness)
layout(binding=1) uniform textureCube environment_map;
layout(binding=1) uniform sampler environment_smp;

// Irradiance map (for diffuse IBL)
layout(binding=2) uniform textureCube irradiance_map;
layout(binding=2) uniform sampler irradiance_smp;

layout(binding=3) uniform fs_params {
    vec3 view_pos;
    float max_reflection_lod;  // Number of mip levels - 1
    
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
// Fresnel
// ============================================
vec3 F_Schlick(float VdotH, vec3 F0) {
    return F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);
}

vec3 F_SchlickRoughness(float NdotV, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(1.0 - NdotV, 5.0);
}

// ============================================
// Plastic BRDF (dual-lobe specular)
// ============================================
vec3 PlasticBRDF(
    vec3 N, vec3 V, vec3 L, vec3 H,
    float NdotV, float NdotL, float NdotH, float VdotH,
    vec3 albedo, float rough, float cc, float ccRough
) {
    // Base plastic specular (slightly tinted)
    vec3 F0_base = mix(vec3(0.04), albedo * 0.15 + 0.04, 0.3);
    
    float D1 = D_GGX(NdotH, rough);
    float V1 = V_SmithGGX(NdotV, NdotL, rough);
    vec3 F1 = F_Schlick(VdotH, F0_base);
    vec3 spec_base = D1 * V1 * F1;
    
    // Clear coat layer
    vec3 spec_coat = vec3(0.0);
    if (cc > 0.001) {
        float D2 = D_GGX(NdotH, ccRough);
        float V2 = V_SmithGGX(NdotV, NdotL, ccRough);
        float F2 = 0.04 + 0.96 * pow(1.0 - VdotH, 5.0);
        spec_coat = vec3(D2 * V2 * F2 * cc);
    }
    
    return spec_base * (1.0 - cc * 0.5) + spec_coat;
}

// ============================================
// Subsurface Scattering
// ============================================
vec3 PlasticSSS(
    vec3 N, vec3 L, vec3 V,
    float NdotL, float NdotV,
    vec3 albedo, vec3 sssColor, float sssIntensity
) {
    if (sssIntensity < 0.001) return vec3(0.0);
    
    float wrap = 0.5;
    float wrapDiffuse = (NdotL + wrap) / (1.0 + wrap);
    wrapDiffuse = max(0.0, wrapDiffuse);
    wrapDiffuse = wrapDiffuse * wrapDiffuse;
    
    float backLight = pow(max(0.0, dot(V, -L)), 3.0) * 0.4;
    float viewScatter = (1.0 - NdotV) * 0.3;
    
    float sssFactor = (wrapDiffuse * 0.7 + backLight + viewScatter) * sssIntensity;
    return albedo * sssColor * sssFactor;
}

// ============================================
// Edge Darkening
// ============================================
vec3 EdgeDarkening(vec3 albedo, float NdotV, float intensity) {
    float edgeFactor = 1.0 - NdotV;
    edgeFactor = edgeFactor * edgeFactor;
    vec3 darkened = albedo * albedo;
    return mix(albedo, darkened, edgeFactor * intensity);
}

// ============================================
// Tone Mapping
// ============================================
vec3 ToneMap(vec3 color) {
    float whitePoint = 2.5;
    return color * (1.0 + color / (whitePoint * whitePoint)) / (1.0 + color);
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
    
    // Dot products
    float NdotV = max(dot(N, V), 0.001);
    float NdotL = max(dot(N, L), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.001);
    
    // Edge darkening for plastic depth effect
    vec3 plasticAlbedo = EdgeDarkening(albedo, NdotV, 0.4);
    
    // Energy conservation
    vec3 F0 = vec3(0.04);
    vec3 F = F_Schlick(VdotH, F0);
    vec3 kD = (1.0 - F) * (1.0 - metallic);
    vec3 diffuse = kD * plasticAlbedo;
    
    // Specular
    vec3 specular = PlasticBRDF(N, V, L, H, NdotV, NdotL, NdotH, VdotH,
                                 albedo, roughness, clearcoat, clearcoat_roughness);
    
    // Direct lighting
    vec3 radiance = light_color * light_intensity;
    float shadow = NdotL * 0.8 + 0.2 * max(0.0, (NdotL + 0.3) / 1.3);
    vec3 directLight = (diffuse * shadow + specular * NdotL) * radiance;
    
    // SSS
    vec3 sss = PlasticSSS(N, L, V, NdotL, NdotV, plasticAlbedo, subsurface_color, subsurface);
    sss *= radiance;
    
    // Rim light
    float rim = pow(1.0 - NdotV, rim_power);
    float rimLightFacing = max(0.2, NdotL * 0.5 + 0.5);
    vec3 rimLight = rim_color * rim * rim_intensity * rimLightFacing;
    rimLight *= mix(vec3(1.0), albedo, 0.3);
    
    // ========== IBL (Image-Based Lighting) ==========
    
    // Diffuse IBL from irradiance map
    vec3 irradiance = texture(samplerCube(irradiance_map, irradiance_smp), N).rgb;
    vec3 diffuseIBL = irradiance * plasticAlbedo * kD * ambient_intensity;
    
    // Specular IBL from prefiltered environment map
    // Select mip level based on roughness
    float lod = roughness * max_reflection_lod;
    vec3 prefilteredColor = textureLod(samplerCube(environment_map, environment_smp), R, lod).rgb;
    
    // Fresnel for IBL
    vec3 F_ibl = F_SchlickRoughness(NdotV, F0, roughness);
    
    // Approximate environment BRDF (without LUT, simplified)
    // This is a rough approximation of the split-sum integral's second part
    float envBRDF_x = 1.0 - roughness; // Approximate scale
    float envBRDF_y = roughness * 0.5; // Approximate bias
    vec3 specularIBL = prefilteredColor * (F_ibl * envBRDF_x + envBRDF_y) * env_reflection_intensity;
    
    // Clear coat IBL (sharper reflection)
    vec3 ccIBL = vec3(0.0);
    if (clearcoat > 0.001) {
        float ccLod = clearcoat_roughness * max_reflection_lod;
        vec3 ccPrefilteredColor = textureLod(samplerCube(environment_map, environment_smp), R, ccLod).rgb;
        float ccFresnel = 0.04 + 0.96 * pow(1.0 - NdotV, 5.0);
        ccIBL = ccPrefilteredColor * ccFresnel * clearcoat * env_reflection_intensity * 0.5;
    }
    
    // ========== Final Composition ==========
    vec3 finalColor = vec3(0.0);
    
    // Direct lighting
    finalColor += directLight;
    finalColor += sss;
    
    // IBL (ambient)
    finalColor += diffuseIBL;
    finalColor += specularIBL * (1.0 - clearcoat * 0.3);
    finalColor += ccIBL;
    
    // Rim light
    finalColor += rimLight;
    
    // Post processing
    finalColor = ToneMap(finalColor);
    
    // Saturation boost
    float luma = dot(finalColor, vec3(0.2126, 0.7152, 0.0722));
    finalColor = mix(vec3(luma), finalColor, 1.15);
    
    // Gamma
    finalColor = LinearToGamma(finalColor);
    finalColor = clamp(finalColor, 0.0, 1.0);
    
    frag_color = vec4(finalColor, alpha);
}
@end

@program mmd vs fs

