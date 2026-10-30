// The vertex stage of the Vulkan renderer (gpu/vkr.c). Vertices arrive
// shaded, clipped and in window space (gpu/draw.c). Every attribute goes to
// the pixel shader divided by w, interpolated linearly in screen space, with
// 1/w beside it: the pixel shader divides, as gpu/draw.c's interpolate()
// does, so both renderers compute the same values.
#version 450

layout(location = 0) in vec4 a_pos;                // x, y (normalised device coordinates), depth, 1/w
layout(location = 1) in vec4 a_attr[6];             // gpu/draw.c's 24 attribute slots, divided by w

layout(location = 0) noperspective out vec4 v_zw;   // x: depth, y: 1/w
layout(location = 1) noperspective out vec4 v_attr[6];

void main()
{
    gl_Position = vec4(a_pos.xy, 0.0, 1.0);
    v_zw = vec4(a_pos.zw, 0.0, 0.0);
    for (int i = 0; i < 6; i++)
        v_attr[i] = a_attr[i];
}
