#version 450
//
// rvk default sprite vertex shader (Vulkan GLSL)
// Matches rlgl's default vertex attribute layout:
//   location 0: position (vec3)
//   location 1: texcoord (vec2)
//   location 3: color    (vec4, fed from normalized R8G8B8A8)
//
// MVP supplied as a push constant (model-view-projection already combined CPU-side),
// matching how the immediate-mode batcher flushes draws.

layout(location = 0) in vec3 vertexPosition;
layout(location = 1) in vec2 vertexTexCoord;
layout(location = 3) in vec4 vertexColor;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
} pc;

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) out vec4 fragColor;

void main()
{
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    gl_Position = pc.mvp * vec4(vertexPosition, 1.0);
}
