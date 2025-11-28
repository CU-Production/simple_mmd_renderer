@ctype mat4  HMM_Mat4
@ctype vec4  HMM_Vec4
@ctype vec3  HMM_Vec3
@ctype vec2  HMM_Vec2

@module taa

@vs vs
in vec2 position;
out vec2 uv;

void main() {
    gl_Position = vec4(position, 0.0, 1.0);
    uv = position * 0.5 + 0.5; // Convert from [-1,1] to [0,1]
    uv.y = 1.0 - uv.y; // Flip Y for correct texture sampling
}
@end

@fs fs
in vec2 uv;
out vec4 frag_color;

layout(binding=0) uniform texture2D current_frame;
layout(binding=0) uniform sampler current_smp;

layout(binding=1) uniform texture2D history_frame;
layout(binding=1) uniform sampler history_smp;

layout(binding=2) uniform texture2D depth_texture;
layout(binding=2) uniform sampler depth_smp;

layout(binding=3) uniform taa_params {
    vec2 jitter_offset;      // Current frame jitter offset
    vec2 prev_jitter_offset; // Previous frame jitter offset
    vec2 screen_size;         // Screen resolution (width, height)
    float blend_factor;       // TAA blend factor (typically 0.05-0.2)
};

// Clamp history color to neighborhood of current color to reduce ghosting
vec3 ClipToAABB(vec3 color, vec3 min_color, vec3 max_color) {
    vec3 center = (min_color + max_color) * 0.5;
    vec3 extents = max_color - center;
    
    // Clamp to AABB
    vec3 dist = color - center;
    vec3 clamped = dist;
    
    // Find the axis that needs the most clamping
    vec3 abs_dist = abs(dist);
    vec3 abs_extents = abs(extents);
    
    for (int i = 0; i < 3; i++) {
        if (abs_extents[i] > 0.0001) {
            float scale = abs_extents[i] / max(abs_dist[i], 0.0001);
            if (scale < 1.0) {
                clamped = dist * scale;
            }
        }
    }
    
    return center + clamped;
}

// Sample 3x3 neighborhood for color clamping
vec3 GetNeighborhoodMin(vec2 uv) {
    vec2 texel_size = 1.0 / screen_size;
    vec3 min_color = vec3(1e6);
    
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec2 offset = vec2(float(x), float(y)) * texel_size;
            vec3 color = texture(sampler2D(current_frame, current_smp), uv + offset).rgb;
            min_color = min(min_color, color);
        }
    }
    return min_color;
}

vec3 GetNeighborhoodMax(vec2 uv) {
    vec2 texel_size = 1.0 / screen_size;
    vec3 max_color = vec3(-1e6);
    
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec2 offset = vec2(float(x), float(y)) * texel_size;
            vec3 color = texture(sampler2D(current_frame, current_smp), uv + offset).rgb;
            max_color = max(max_color, color);
        }
    }
    return max_color;
}

void main() {
    vec2 texel_size = 1.0 / screen_size;
    
    // Sample current frame
    vec3 current = texture(sampler2D(current_frame, current_smp), uv).rgb;
    
    // Calculate reprojected UV using jitter offsets
    vec2 reprojected_uv = uv;
    // Reproject using depth and jitter difference
    // For simplicity, we use a basic reprojection based on jitter
    vec2 jitter_diff = (jitter_offset - prev_jitter_offset) * texel_size;
    reprojected_uv = uv - jitter_diff;
    
    // Clamp reprojected UV to valid range
    reprojected_uv = clamp(reprojected_uv, vec2(0.0), vec2(1.0));
    
    // Sample history frame
    vec3 history = texture(sampler2D(history_frame, history_smp), reprojected_uv).rgb;
    
    // Get neighborhood for color clamping (reduces ghosting)
    vec3 min_color = GetNeighborhoodMin(uv);
    vec3 max_color = GetNeighborhoodMax(uv);
    
    // Clamp history to neighborhood
    history = ClipToAABB(history, min_color, max_color);
    
    // Blend current and history
    vec3 result = mix(history, current, blend_factor);
    
    frag_color = vec4(result, 1.0);
}
@end

@program taa vs fs

