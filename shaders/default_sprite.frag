#version 450
//
// rvk default sprite fragment shader (Vulkan GLSL)
// Samples texture0 (set 0, binding 0) and modulates by the per-vertex tint color,
// mirroring rlgl's default fragment shader (texelColor * colDiffuse * fragColor).

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;

layout(set = 0, binding = 0) uniform sampler2D texture0;

layout(location = 0) out vec4 finalColor;

void main()
{
    vec4 texelColor = texture(texture0, fragTexCoord);
    finalColor = texelColor * fragColor;
}
