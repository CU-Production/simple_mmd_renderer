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

// Improved AABB clipping algorithm to reduce ghosting
vec3 ClipToAABB(vec3 color, vec3 min_color, vec3 max_color) {
    vec3 center = (min_color + max_color) * 0.5;
    vec3 extents = max_color - center;
    
    // Clamp to AABB - find the minimum scale factor across all axes
    vec3 dist = color - center;
    vec3 abs_dist = abs(dist);
    vec3 abs_extents = abs(extents);
    
    // Find minimum scale factor that keeps color within AABB
    float min_scale = 1.0;
    for (int i = 0; i < 3; i++) {
        if (abs_extents[i] > 0.0001 && abs_dist[i] > abs_extents[i]) {
            float scale = abs_extents[i] / abs_dist[i];
            min_scale = min(min_scale, scale);
        }
    }
    
    // Apply the minimum scale to all axes to keep color within AABB
    return center + dist * min_scale;
}

void main() {
    vec2 texel_size = 1.0 / screen_size;
    
    // Sample current frame with bilinear filtering
    vec3 current = texture(sampler2D(current_frame, current_smp), uv).rgb;
    
    // Calculate reprojected UV using jitter offsets
    // Convert jitter from pixel space to NDC space, then to UV space
    vec2 jitter_diff_ndc = (jitter_offset - prev_jitter_offset) / screen_size;
    vec2 reprojected_uv = uv - jitter_diff_ndc;
    
    // Check if reprojected UV is valid (within bounds)
    bool is_reprojection_valid = all(greaterThanEqual(reprojected_uv, vec2(0.0))) && 
                                  all(lessThanEqual(reprojected_uv, vec2(1.0)));
    
    // Clamp reprojected UV to valid range
    reprojected_uv = clamp(reprojected_uv, vec2(0.0), vec2(1.0));
    
    // Sample history frame with bilinear filtering
    vec3 history = texture(sampler2D(history_frame, history_smp), reprojected_uv).rgb;
    
    // Get neighborhood for color clamping (reduces ghosting)
    // Use 3x3 neighborhood for better clamping
    vec3 min_color = vec3(1e6);
    vec3 max_color = vec3(-1e6);
    
    // Sample 3x3 neighborhood
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec2 offset = vec2(float(x), float(y)) * texel_size;
            vec3 sample_color = texture(sampler2D(current_frame, current_smp), uv + offset).rgb;
            min_color = min(min_color, sample_color);
            max_color = max(max_color, sample_color);
        }
    }
    
    // Expand AABB slightly to allow for small variations (reduces over-clipping)
    vec3 center = (min_color + max_color) * 0.5;
    vec3 extents = max_color - center;
    extents *= 1.1; // Expand by 10%
    min_color = center - extents;
    max_color = center + extents;
    
    // Clamp history to neighborhood
    vec3 clipped_history = ClipToAABB(history, min_color, max_color);
    
    // Calculate reprojection confidence based on UV distance
    vec2 reprojection_error = abs(reprojected_uv - uv);
    float max_error = max(reprojection_error.x, reprojection_error.y);
    float confidence = 1.0 - smoothstep(0.0, 0.1, max_error); // Confidence decreases with reprojection error
    
    // If reprojection is invalid or has high error, reduce history contribution
    if (!is_reprojection_valid) {
        confidence = 0.0;
    }
    
    // Dynamic blend factor based on confidence
    float dynamic_blend = mix(blend_factor * 0.5, blend_factor * 2.0, confidence);
    dynamic_blend = clamp(dynamic_blend, 0.05, 0.3); // Clamp to reasonable range
    
    // Calculate color difference to detect disocclusions
    float color_diff = length(clipped_history - current);
    float color_threshold = 0.1; // Threshold for detecting disocclusions
    
    // Increase blend factor if color difference is large (likely disocclusion)
    if (color_diff > color_threshold) {
        dynamic_blend = min(dynamic_blend * 2.0, 0.5);
    }
    
    // Blend current and history
    vec3 result = mix(clipped_history, current, dynamic_blend);
    
    // Optional: Apply slight sharpening to compensate for temporal smoothing
    vec3 sharpened = result * 1.05 - current * 0.05;
    result = mix(result, sharpened, 0.3);
    
    frag_color = vec4(result, 1.0);
}
@end

@program taa vs fs

