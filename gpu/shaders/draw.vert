// The vertex stage of the Vulkan renderer (gpu/vkr.c). Vertices arrive
// shaded, clipped and in window space (gpu/draw.c); position is
// (x_ndc * w, y_ndc * w, 0, w), so the hardware's perspective-correct
// interpolation is the same arithmetic as the software rasteriser's.
#version 450

layout(location = 0) in vec4 a_pos;
layout(location = 1) in vec4 a_color;
layout(location = 2) in float a_z;

layout(location = 0) out vec4 v_color;
layout(location = 1) noperspective out float v_z;   // the PICA's depth: linear in screen space

void main()
{
    gl_Position = a_pos;
    v_color = a_color;
    v_z = a_z;
}
