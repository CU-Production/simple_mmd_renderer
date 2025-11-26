@ctype mat4  HMM_Mat4
@ctype vec4  HMM_Vec4
@ctype vec3  HMM_Vec3
@ctype vec2  HMM_Vec2

@module mmd

@vs vs
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
    light_space_pos = light_mvp * vec4(position, 1.0); // Transform to light space
#if !SOKOL_GLSL
    light_space_pos.y = -light_space_pos.y;
#endif
    uv = texcoord0;
    norm = mat3(transpose(inverse(model))) * normal; // Transform normal to world space
}
@end

@fs fs
in vec2 uv;
in vec3 norm;
in vec3 world_pos;
in vec4 light_space_pos; // Position in light space for shadow mapping
out vec4 frag_color;

layout(binding=0) uniform textureCube irradiance_map;
layout(binding=0) uniform sampler irradiance_smp;
layout(binding=1) uniform textureCube prefilter_map;
layout(binding=1) uniform sampler prefilter_smp;
layout(binding=2) uniform texture2D diffuse_texture;
layout(binding=2) uniform sampler diffuse_smp;
layout(binding=3) uniform texture2D shadow_map;
layout(binding=3) uniform sampler shadow_smp; // Shadow sampler with comparison

layout(binding=4) uniform fs_params {
    vec3 view_pos;
    float roughness;
    float metallic;
    float ibl_strength;
    vec3 light_direction; // Directional light direction
    vec3 light_color; // Directional light color
    float light_intensity; // Directional light intensity
    float shadows_enabled; // Whether shadows are enabled
};

const float PI = 3.14159265359;

vec3 FresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 FresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Calculate shadow factor using PCF (Percentage Closer Filtering)
float CalculateShadow(vec4 light_space_pos) {
    // Perspective divide
    vec3 proj_coords = light_space_pos.xyz / light_space_pos.w;
    // Transform to [0,1] range
    proj_coords = proj_coords * 0.5 + 0.5;
    
    // Check if fragment is outside light frustum
    if (proj_coords.x < 0.0 || proj_coords.x > 1.0 || 
        proj_coords.y < 0.0 || proj_coords.y > 1.0 ||
        proj_coords.z > 1.0) {
        return 1.0; // Not in shadow
    }
    
    float current_depth = proj_coords.z;
    float bias = 0.005; // Shadow bias to prevent shadow acne
    
    // Calculate texel size once (outside loop to avoid gradient instruction in loop)
    vec2 texel_size = 1.0 / vec2(textureSize(sampler2D(shadow_map, shadow_smp), 0));
    
    // Manually unroll 3x3 PCF kernel to avoid gradient instruction warnings
    // This also improves performance by avoiding loop overhead
    float shadow_sum = 0.0;
    
    // Row -1
    vec2 offset = vec2(-1.0, -1.0) * texel_size;
    float depth = texture(sampler2D(shadow_map, shadow_smp), proj_coords.xy + offset).r;
    shadow_sum += (current_depth - bias > depth) ? 0.0 : 1.0;
    
    offset = vec2(0.0, -1.0) * texel_size;
    depth = texture(sampler2D(shadow_map, shadow_smp), proj_coords.xy + offset).r;
    shadow_sum += (current_depth - bias > depth) ? 0.0 : 1.0;
    
    offset = vec2(1.0, -1.0) * texel_size;
    depth = texture(sampler2D(shadow_map, shadow_smp), proj_coords.xy + offset).r;
    shadow_sum += (current_depth - bias > depth) ? 0.0 : 1.0;
    
    // Row 0
    offset = vec2(-1.0, 0.0) * texel_size;
    depth = texture(sampler2D(shadow_map, shadow_smp), proj_coords.xy + offset).r;
    shadow_sum += (current_depth - bias > depth) ? 0.0 : 1.0;
    
    offset = vec2(0.0, 0.0) * texel_size;
    depth = texture(sampler2D(shadow_map, shadow_smp), proj_coords.xy + offset).r;
    shadow_sum += (current_depth - bias > depth) ? 0.0 : 1.0;
    
    offset = vec2(1.0, 0.0) * texel_size;
    depth = texture(sampler2D(shadow_map, shadow_smp), proj_coords.xy + offset).r;
    shadow_sum += (current_depth - bias > depth) ? 0.0 : 1.0;
    
    // Row 1
    offset = vec2(-1.0, 1.0) * texel_size;
    depth = texture(sampler2D(shadow_map, shadow_smp), proj_coords.xy + offset).r;
    shadow_sum += (current_depth - bias > depth) ? 0.0 : 1.0;
    
    offset = vec2(0.0, 1.0) * texel_size;
    depth = texture(sampler2D(shadow_map, shadow_smp), proj_coords.xy + offset).r;
    shadow_sum += (current_depth - bias > depth) ? 0.0 : 1.0;
    
    offset = vec2(1.0, 1.0) * texel_size;
    depth = texture(sampler2D(shadow_map, shadow_smp), proj_coords.xy + offset).r;
    shadow_sum += (current_depth - bias > depth) ? 0.0 : 1.0;
    
    return shadow_sum / 9.0;
}

void main() {
    vec3 N = normalize(norm);
    vec3 V = normalize(view_pos - world_pos);
    
    // For test mode: use normal as color (when normal contains color data)
    // For model mode: use IBL lighting
//    if (norm.x > 0.5f || norm.y > 0.5f || norm.z > 0.5f) {
//        // Likely color data, use directly
//        frag_color = vec4(norm, 1.0);
//    } else
    {
        // Likely normal data, use IBL lighting with texture
        // Sample diffuse texture
        vec3 albedo = texture(sampler2D(diffuse_texture, diffuse_smp), uv).rgb;
        
        // Calculate shadow factor
        float shadow = 1.0;
        if (shadows_enabled > 0.5) {
            shadow = CalculateShadow(light_space_pos);
        }
        
        // Calculate directional light contribution
        vec3 L = normalize(-light_direction); // Light direction points towards light
        float NdotL = max(dot(N, L), 0.0);
        vec3 directional_light = light_color * light_intensity * NdotL * shadow;
        
        if (ibl_strength > 0.0) {
            // Use IBL lighting with directional light
            vec3 R = reflect(-V, N);
            
            // Sample irradiance map for diffuse IBL
            vec3 irradiance = texture(samplerCube(irradiance_map, irradiance_smp), N).rgb;
            
            // Sample prefilter map for specular IBL
            float NdotV = max(dot(N, V), 0.0);
            float lod = roughness * 4.0; // 4.0 is max mip level
            vec3 prefilteredColor = textureLod(samplerCube(prefilter_map, prefilter_smp), R, lod).rgb;
            
            // Fresnel-Schlick with roughness
            vec3 F0 = vec3(0.04); // Dielectric F0
            F0 = mix(F0, albedo, metallic); // For metallic surfaces, use albedo
            vec3 F = FresnelSchlickRoughness(NdotV, F0, roughness);
            
            // Combine diffuse and specular
            vec3 kS = F;
            vec3 kD = 1.0 - kS;
            kD *= 1.0 - metallic;
            
            // Multiply albedo with irradiance for diffuse lighting
            vec3 diffuse = irradiance * albedo;
            vec3 specular = prefilteredColor * F;
            
            vec3 ambient = (kD * diffuse + specular) * ibl_strength;
            
            // Add directional light to IBL
            vec3 final_color = ambient + directional_light * albedo;
            
            frag_color = vec4(final_color, 1.0);
        } else {
            // No IBL, use directional light only
            vec3 ambient_color = albedo * 0.3; // Ambient
            vec3 diffuse_color = albedo * directional_light; // Diffuse with shadow
            frag_color = vec4(ambient_color + diffuse_color, 1.0);
        }
    }
}
@end

@program mmd vs fs

// Ground plane shader (reuses same shader but with different material properties)
@program ground vs fs

