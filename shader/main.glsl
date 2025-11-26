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

layout(binding=0) uniform textureCube irradiance_map;
layout(binding=0) uniform sampler irradiance_smp;
layout(binding=1) uniform textureCube prefilter_map;
layout(binding=1) uniform sampler prefilter_smp;
layout(binding=2) uniform texture2D diffuse_texture;
layout(binding=2) uniform sampler diffuse_smp;

layout(binding=3) uniform fs_params {
    vec3 view_pos;
    float roughness;
    float metallic;
    float ibl_strength;
};

const float PI = 3.14159265359;

vec3 FresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 FresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
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
        
        if (ibl_strength > 0.0) {
            // Use IBL lighting
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
            
            frag_color = vec4(ambient, 1.0);
        } else {
            // No IBL, just show texture with simple lighting
            // Simple directional light
            vec3 lightDir = normalize(vec3(1.0, 1.0, 1.0));
            float NdotL = max(dot(N, lightDir), 0.0);
            vec3 ambient_color = albedo * 0.3; // Ambient
            vec3 diffuse_color = albedo * NdotL * 0.7; // Diffuse
            frag_color = vec4(ambient_color + diffuse_color, 1.0);
        }
    }
}
@end

@program mmd vs fs

