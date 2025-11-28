@ctype mat4  HMM_Mat4
@ctype vec4  HMM_Vec4
@ctype vec3  HMM_Vec3
@ctype vec2  HMM_Vec2

@module shadow

// Shadow mapping vertex shader (depth-only pass)
@vs vs
layout(binding=0) uniform vs_params {
    mat4 light_mvp; // Light space MVP matrix
};

in vec3 position;

void main() {
    gl_Position = light_mvp * vec4(position, 1.0);
}
@end

@fs fs
void main() {
    // Depth-only pass, no color output needed
    // Depth is automatically written to depth buffer
}
@end

@program shadow vs fs


