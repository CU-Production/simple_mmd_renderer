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

// IBL textures
layout(binding=0) uniform textureCube irradiance_map;
layout(binding=0) uniform sampler irradiance_smp;
layout(binding=1) uniform textureCube prefilter_map;
layout(binding=1) uniform sampler prefilter_smp;

// Diffuse texture (albedo)
layout(binding=2) uniform texture2D diffuse_texture;
layout(binding=2) uniform sampler diffuse_smp;

// Shadow map
layout(binding=3) uniform texture2D shadow_map;
layout(binding=3) uniform sampler shadow_smp;

layout(binding=1) uniform fs_params {
    vec3 view_pos;
    float roughness;
    float metallic;
    float ibl_strength;
    vec3 light_direction;
    vec3 light_color;
    float light_intensity;
    float shadows_enabled;
    float receive_shadows;
};

const float PI = 3.14159265359;

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

// Fresnel-Schlick approximation
vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(abs(clamp(1.0 - cosTheta, 0.0, 1.0)), 5.0);
}

// Normal Distribution Function (GGX/Trowbridge-Reitz)
float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    
    float num = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
    
    return num / max(denom, 0.0000001);
}

// Geometry function (Schlick-GGX)
float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;
    
    float num = NdotV;
    float denom = NdotV * (1.0 - k) + k;
    
    return num / max(denom, 0.0000001);
}

// Smith's method for geometry obstruction
float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);
    
    return ggx1 * ggx2;
}

void main() {
    vec3 N = normalize(norm);
    vec3 V = normalize(view_pos - world_pos);
    vec3 L = normalize(-light_direction); // Light direction points from light to surface
    vec3 H = normalize(V + L);
    vec3 R = reflect(-V, N);
    
    // Sample textures
    vec3 albedo = texture(sampler2D(diffuse_texture, diffuse_smp), uv).rgb;
    
    // Calculate F0 (base reflectivity) based on metallic
    vec3 F0 = vec3(0.04);
    F0 = mix(F0, albedo, metallic);
    
    // Calculate shadow
    float shadow = CalculateShadow(light_space_pos);
    
    // Cook-Torrance BRDF
    float NDF = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
    
    vec3 kS = F;
    vec3 kD = vec3(1.0) - kS;
    kD *= 1.0 - metallic;
    
    vec3 numerator = NDF * G * F;
    float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001;
    vec3 specular = numerator / denominator;
    
    // Add outgoing radiance Lo
    float NdotL = max(dot(N, L), 0.0);
    
    // Directional light contribution
    vec3 Lo = (kD * albedo / PI + specular) * light_color * light_intensity * NdotL * shadow;
    
    // IBL contribution
    vec3 ambient = vec3(0.0);
    if (ibl_strength > 0.001) {
        // Diffuse IBL (irradiance)
        vec3 irradiance = texture(samplerCube(irradiance_map, irradiance_smp), N).rgb;
        vec3 diffuse_ibl = irradiance * albedo;
        
        // Specular IBL (prefiltered environment)
        // Sample prefilter map at mip level based on roughness
        const float MAX_REFLECTION_LOD = 4.0;
        vec3 prefilteredColor = textureLod(samplerCube(prefilter_map, prefilter_smp), R, roughness * MAX_REFLECTION_LOD).rgb;
        vec3 F_env = fresnelSchlick(max(dot(N, V), 0.0), F0);
        vec3 specular_ibl = prefilteredColor * F_env;
        
        ambient = (diffuse_ibl + specular_ibl) * ibl_strength;
    }
    
    // Final color
    vec3 color = ambient + Lo;
    
    // Tone mapping and gamma correction
    color = color / (color + vec3(1.0));
    color = pow(abs(color), vec3(1.0 / 2.2));
    
    frag_color = vec4(color, 1.0);

    frag_color = vec4(albedo, 1.0) * shadow;
}
@end

@program ground ground_vs ground_fs

