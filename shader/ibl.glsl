@ctype mat4  HMM_Mat4
@ctype vec4  HMM_Vec4
@ctype vec3  HMM_Vec3
@ctype vec2  HMM_Vec2

@module ibl

// Equirectangular to cubemap conversion shader
@vs equirect_to_cubemap_vs
layout(binding=0) uniform vs_params {
    mat4 mvp;
};

in vec3 position;
out vec3 world_pos;

void main() {
    gl_Position = mvp * vec4(position, 1.0);
    world_pos = position;
}
@end

@fs equirect_to_cubemap_fs
in vec3 world_pos;
out vec4 frag_color;

layout(binding=0) uniform texture2D equirectangular_map;
layout(binding=0) uniform sampler equirectangular_smp;

const float PI = 3.14159265359;

vec2 SampleSphericalMap(vec3 v) {
    vec2 uv = vec2(atan(v.z, v.x), asin(v.y));
    uv *= vec2(0.1591, 0.3183); // 1 / (2 * PI), 1 / PI
    uv += 0.5;
    return uv;
}

void main() {
    vec2 uv = SampleSphericalMap(normalize(world_pos));
    vec3 color = texture(sampler2D(equirectangular_map, equirectangular_smp), uv).rgb;
    frag_color = vec4(color, 1.0);
}
@end

@program equirect_to_cubemap equirect_to_cubemap_vs equirect_to_cubemap_fs

// Irradiance map generation shader
@vs irradiance_vs
layout(binding=0) uniform vs_params {
    mat4 mvp;
};

in vec3 position;
out vec3 world_pos;

void main() {
    gl_Position = mvp * vec4(position, 1.0);
    world_pos = position;
}
@end

@fs irradiance_fs
in vec3 world_pos;
out vec4 frag_color;

layout(binding=0) uniform textureCube environment_map;
layout(binding=0) uniform sampler environment_smp;

const float PI = 3.14159265359;

void main() {
    vec3 N = normalize(world_pos);
    vec3 irradiance = vec3(0.0);
    
    vec3 up = vec3(0.0, 1.0, 0.0);
    vec3 right = normalize(cross(up, N));
    up = normalize(cross(N, right));
    
    float sampleDelta = 0.025;
    float nrSamples = 0.0;
    
    for (float phi = 0.0; phi < 2.0 * PI; phi += sampleDelta) {
        for (float theta = 0.0; theta < 0.5 * PI; theta += sampleDelta) {
            vec3 tangentSample = vec3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            vec3 sampleVec = tangentSample.x * right + tangentSample.y * up + tangentSample.z * N;
            
            irradiance += texture(samplerCube(environment_map, environment_smp), sampleVec).rgb * cos(theta) * sin(theta);
            nrSamples++;
        }
    }
    
    irradiance = PI * irradiance * (1.0 / float(nrSamples));
    frag_color = vec4(irradiance, 1.0);
}
@end

@program irradiance irradiance_vs irradiance_fs

// Prefiltered environment map generation shader
@vs prefilter_vs
layout(binding=0) uniform vs_params {
    mat4 mvp;
};

in vec3 position;
out vec3 world_pos;

void main() {
    gl_Position = mvp * vec4(position, 1.0);
    world_pos = position;
}
@end

@fs prefilter_fs
in vec3 world_pos;
out vec4 frag_color;

layout(binding=0) uniform textureCube environment_map;
layout(binding=0) uniform sampler environment_smp;
layout(binding=1) uniform fs_params {
    float roughness;
};

const float PI = 3.14159265359;

float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    
    float num = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
    
    return num / denom;
}

float RadicalInverse_VdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

vec2 Hammersley(uint i, uint N) {
    return vec2(float(i) / float(N), RadicalInverse_VdC(i));
}

vec3 ImportanceSampleGGX(vec2 Xi, vec3 N, float roughness) {
    float a = roughness * roughness;
    
    float phi = 2.0 * PI * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    
    vec3 H;
    H.x = cos(phi) * sinTheta;
    H.y = sin(phi) * sinTheta;
    H.z = cosTheta;
    
    vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, N));
    vec3 bitangent = cross(N, tangent);
    
    vec3 sampleVec = tangent * H.x + bitangent * H.y + N * H.z;
    return normalize(sampleVec);
}

void main() {
    vec3 N = normalize(world_pos);
    vec3 R = N;
    vec3 V = R;
    
    const uint SAMPLE_COUNT = 1024u;
    float totalWeight = 0.0;
    vec3 prefilteredColor = vec3(0.0);
    
    for (uint i = 0u; i < SAMPLE_COUNT; ++i) {
        vec2 Xi = Hammersley(i, SAMPLE_COUNT);
        vec3 H = ImportanceSampleGGX(Xi, N, roughness);
        vec3 L = normalize(2.0 * dot(V, H) * H - V);
        
        float NdotL = max(dot(N, L), 0.0);
        if (NdotL > 0.0) {
            float D = DistributionGGX(N, H, roughness);
            float NdotH = max(dot(N, H), 0.0);
            float HdotV = max(dot(H, V), 0.0);
            float pdf = D * NdotH / (4.0 * HdotV) + 0.0001;
            
            float resolution = 512.0; // cubemap face width
            float saTexel = 4.0 * PI / (6.0 * resolution * resolution);
            float saSample = 1.0 / (float(SAMPLE_COUNT) * pdf + 0.0001);
            
            float mipLevel = roughness == 0.0 ? 0.0 : 0.5 * log2(saSample / saTexel);
            
            prefilteredColor += textureLod(samplerCube(environment_map, environment_smp), L, mipLevel).rgb * NdotL;
            totalWeight += NdotL;
        }
    }
    
    prefilteredColor = prefilteredColor / totalWeight;
    frag_color = vec4(prefilteredColor, 1.0);
}
@end

@program prefilter prefilter_vs prefilter_fs

// Skybox shader
@vs skybox_vs
layout(binding=0) uniform vs_params {
    mat4 mvp;
};

in vec3 position;
out vec3 tex_coords;

void main() {
    tex_coords = position;
    vec4 pos = mvp * vec4(position, 1.0);
    gl_Position = pos.xyww; // Ensure depth is always 1.0 (furthest)
}
@end

@fs skybox_fs
in vec3 tex_coords;
out vec4 frag_color;

layout(binding=0) uniform textureCube environment_map;
layout(binding=0) uniform sampler environment_smp;

void main() {
    frag_color = vec4(texture(samplerCube(environment_map, environment_smp), tex_coords).rgb, 1.0);
}
@end

@program skybox skybox_vs skybox_fs

