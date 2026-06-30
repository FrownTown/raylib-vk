/**********************************************************************************************
*
*   rvk - Vulkan 1.3 backend for raylib (replacement for the OpenGL-based rlgl layer)
*
*   This is the single public C API (C99 compatible) exposing the Vulkan-backed renderer.
*   The implementation lives in src/rvk_*.cpp (C++20). All GPU objects are exposed as
*   `unsigned int` opaque handles, exactly like rlgl's GL ids, so raylib's public structs
*   (Texture2D.id, Shader.id, RenderTexture.id, Mesh.vaoId/vboId[]) stay byte-stable and
*   no example or user program needs to change.
*
*   DESIGN (see plan): C++20 internals using dynamic rendering (VK_KHR_dynamic_rendering,
*   core in 1.3), synchronization2, VMA for all allocations, vk-bootstrap for init, and
*   shaderc for runtime GLSL->SPIR-V compilation (with GL->Vulkan GLSL auto-preprocessing).
*
*   Enum/constant VALUES mirror rlgl exactly so call-site rewiring (rl* -> rvk*) is purely
*   mechanical. During the transition, defining RVK_ENABLE_RLGL_COMPAT (default ON) provides
*   rl* aliases for every type, constant and function, letting raylib's higher-level modules
*   (rcore/rshapes/rtextures/rtext/rmodels) compile unchanged against this header.
*
**********************************************************************************************/

#ifndef RVK_H
#define RVK_H

#define RVK_VERSION_MAJOR 1
#define RVK_VERSION_MINOR 0
#define RVK_VERSION       "1.0-vulkan"

// Function specifiers in case library is build/used as a shared library
#ifndef RVKAPI
    #define RVKAPI
#endif

// Support TRACELOG macros from raylib if available, otherwise no-op handled in impl
//----------------------------------------------------------------------------------
// Defines and Macros
//----------------------------------------------------------------------------------

// Default internal render batch elements limits
#ifndef RL_DEFAULT_BATCH_BUFFER_ELEMENTS
    #define RL_DEFAULT_BATCH_BUFFER_ELEMENTS  8192      // Default internal render batch elements limits
#endif
#ifndef RL_DEFAULT_BATCH_BUFFERS
    #define RL_DEFAULT_BATCH_BUFFERS             1      // Default number of batch buffers (multi-buffering)
#endif
#ifndef RL_DEFAULT_BATCH_DRAWCALLS
    #define RL_DEFAULT_BATCH_DRAWCALLS         256      // Default number of batch draw calls (by state changes: mode, texture)
#endif
#ifndef RL_DEFAULT_BATCH_MAX_TEXTURE_UNITS
    #define RL_DEFAULT_BATCH_MAX_TEXTURE_UNITS   4      // Maximum number of textures units that can be activated on batch drawing
#endif
#ifndef RL_MAX_MATRIX_STACK_SIZE
    #define RL_MAX_MATRIX_STACK_SIZE            32      // Maximum size of Matrix stack
#endif
#ifndef RL_MAX_SHADER_LOCATIONS
    #define RL_MAX_SHADER_LOCATIONS             32      // Maximum number of shader locations supported
#endif
#ifndef RL_CULL_DISTANCE_NEAR
    #define RL_CULL_DISTANCE_NEAR             0.05      // Default near cull distance
#endif
#ifndef RL_CULL_DISTANCE_FAR
    #define RL_CULL_DISTANCE_FAR            4000.0      // Default far cull distance
#endif

// Texture parameters (values kept identical to rlgl/OpenGL tokens for ABI compatibility)
#define RL_TEXTURE_WRAP_S                       0x2802
#define RL_TEXTURE_WRAP_T                       0x2803
#define RL_TEXTURE_MAG_FILTER                   0x2800
#define RL_TEXTURE_MIN_FILTER                   0x2801

#define RL_TEXTURE_FILTER_NEAREST               0x2600
#define RL_TEXTURE_FILTER_LINEAR                0x2601
#define RL_TEXTURE_FILTER_MIP_NEAREST           0x2700
#define RL_TEXTURE_FILTER_NEAREST_MIP_LINEAR    0x2702
#define RL_TEXTURE_FILTER_LINEAR_MIP_NEAREST    0x2701
#define RL_TEXTURE_FILTER_MIP_LINEAR            0x2703
#define RL_TEXTURE_FILTER_ANISOTROPIC           0x3000
#define RL_TEXTURE_MIPMAP_BIAS_RATIO            0x4000

#define RL_TEXTURE_WRAP_REPEAT                  0x2901
#define RL_TEXTURE_WRAP_CLAMP                   0x812F
#define RL_TEXTURE_WRAP_MIRROR_REPEAT           0x8370
#define RL_TEXTURE_WRAP_MIRROR_CLAMP            0x8742

// Matrix modes
#define RL_MODELVIEW                            0x1700
#define RL_PROJECTION                           0x1701
#define RL_TEXTURE                              0x1702

// Primitive assembly draw modes
#define RL_LINES                                0x0001
#define RL_TRIANGLES                            0x0004
#define RL_QUADS                                0x0007

// Data types
#define RL_UNSIGNED_BYTE                        0x1401
#define RL_FLOAT                                0x1406

// Buffer usage hint
#define RL_STREAM_DRAW                          0x88E0
#define RL_STREAM_READ                          0x88E1
#define RL_STREAM_COPY                          0x88E2
#define RL_STATIC_DRAW                          0x88E4
#define RL_STATIC_READ                          0x88E5
#define RL_STATIC_COPY                          0x88E6
#define RL_DYNAMIC_DRAW                         0x88E8
#define RL_DYNAMIC_READ                         0x88E9
#define RL_DYNAMIC_COPY                         0x88EA

// Shader type
#define RL_FRAGMENT_SHADER                      0x8B30
#define RL_VERTEX_SHADER                        0x8B31
#define RL_COMPUTE_SHADER                       0x91B9

// Blending factors (kept as GL tokens; translated to VkBlendFactor in rvk_pipeline.cpp)
#define RL_ZERO                                 0
#define RL_ONE                                  1
#define RL_SRC_COLOR                            0x0300
#define RL_ONE_MINUS_SRC_COLOR                  0x0301
#define RL_SRC_ALPHA                            0x0302
#define RL_ONE_MINUS_SRC_ALPHA                  0x0303
#define RL_DST_ALPHA                            0x0304
#define RL_ONE_MINUS_DST_ALPHA                  0x0305
#define RL_DST_COLOR                            0x0306
#define RL_ONE_MINUS_DST_COLOR                  0x0307
#define RL_SRC_ALPHA_SATURATE                   0x0308
#define RL_CONSTANT_COLOR                       0x8001
#define RL_ONE_MINUS_CONSTANT_COLOR             0x8002
#define RL_CONSTANT_ALPHA                       0x8003
#define RL_ONE_MINUS_CONSTANT_ALPHA             0x8004

// Blending equations (translated to VkBlendOp in rvk_pipeline.cpp)
#define RL_FUNC_ADD                             0x8006
#define RL_MIN                                  0x8007
#define RL_MAX                                  0x8008
#define RL_FUNC_SUBTRACT                        0x800A
#define RL_FUNC_REVERSE_SUBTRACT                0x800B
#define RL_BLEND_EQUATION                       0x8009
#define RL_BLEND_EQUATION_RGB                   0x8009
#define RL_BLEND_EQUATION_ALPHA                 0x883D
#define RL_BLEND_DST_RGB                        0x80C8
#define RL_BLEND_SRC_RGB                        0x80C9
#define RL_BLEND_DST_ALPHA                      0x80CA
#define RL_BLEND_SRC_ALPHA                      0x80CB
#define RL_BLEND_COLOR                          0x8005

#define RL_READ_FRAMEBUFFER                     0x8CA8
#define RL_DRAW_FRAMEBUFFER                     0x8CA9

// Default shader vertex attribute locations
#ifndef RL_DEFAULT_SHADER_ATTRIB_LOCATION_POSITION
    #define RL_DEFAULT_SHADER_ATTRIB_LOCATION_POSITION    0
#endif
#ifndef RL_DEFAULT_SHADER_ATTRIB_LOCATION_TEXCOORD
    #define RL_DEFAULT_SHADER_ATTRIB_LOCATION_TEXCOORD    1
#endif
#ifndef RL_DEFAULT_SHADER_ATTRIB_LOCATION_NORMAL
    #define RL_DEFAULT_SHADER_ATTRIB_LOCATION_NORMAL      2
#endif
#ifndef RL_DEFAULT_SHADER_ATTRIB_LOCATION_COLOR
    #define RL_DEFAULT_SHADER_ATTRIB_LOCATION_COLOR       3
#endif
#ifndef RL_DEFAULT_SHADER_ATTRIB_LOCATION_TANGENT
    #define RL_DEFAULT_SHADER_ATTRIB_LOCATION_TANGENT     4
#endif
#ifndef RL_DEFAULT_SHADER_ATTRIB_LOCATION_TEXCOORD2
    #define RL_DEFAULT_SHADER_ATTRIB_LOCATION_TEXCOORD2   5
#endif
#ifndef RL_DEFAULT_SHADER_ATTRIB_LOCATION_INDICES
    #define RL_DEFAULT_SHADER_ATTRIB_LOCATION_INDICES     6
#endif
#ifndef RL_DEFAULT_SHADER_ATTRIB_LOCATION_BONEINDICES
    #define RL_DEFAULT_SHADER_ATTRIB_LOCATION_BONEINDICES 7
#endif
#ifndef RL_DEFAULT_SHADER_ATTRIB_LOCATION_BONEWEIGHTS
    #define RL_DEFAULT_SHADER_ATTRIB_LOCATION_BONEWEIGHTS 8
#endif
#ifndef RL_DEFAULT_SHADER_ATTRIB_LOCATION_INSTANCETRANSFORM
    #define RL_DEFAULT_SHADER_ATTRIB_LOCATION_INSTANCETRANSFORM 9
#endif

//----------------------------------------------------------------------------------
// Types and Structures Definition
//----------------------------------------------------------------------------------
#if !defined(__cplusplus)
    #if (defined(__STDC__) && __STDC_VERSION__ >= 199901L) || (defined(_MSC_VER) && _MSC_VER >= 1800)
        #include <stdbool.h>
    #elif !defined(bool) && !defined(RL_BOOL_TYPE)
        typedef enum bool { false = 0, true = !false } bool;
        #define RL_BOOL_TYPE
    #endif
#endif

#if !defined(RL_MATRIX_TYPE)
// Matrix, 4x4 components, column major, OpenGL style, right handed
typedef struct Matrix {
    float m0, m4, m8, m12;      // Matrix first row (4 components)
    float m1, m5, m9, m13;      // Matrix second row (4 components)
    float m2, m6, m10, m14;     // Matrix third row (4 components)
    float m3, m7, m11, m15;     // Matrix fourth row (4 components)
} Matrix;
#define RL_MATRIX_TYPE
#endif

// Dynamic vertex buffers (Structure-of-Arrays, identical layout to rlVertexBuffer)
typedef struct rvkVertexBuffer {
    int elementCount;           // Number of elements in the buffer (QUADS)
    float *vertices;            // Vertex position (XYZ - 3 components per vertex) (shader-location = 0)
    float *texcoords;           // Vertex texture coordinates (UV - 2 components per vertex) (shader-location = 1)
    float *normals;             // Vertex normal (XYZ - 3 components per vertex) (shader-location = 2)
    unsigned char *colors;      // Vertex colors (RGBA - 4 components per vertex) (shader-location = 3)
    unsigned int *indices;      // Vertex indices (6 indices per quad)
    unsigned int vaoId;         // Vertex layout handle (rvk: stored vertex-layout descriptor, not a GL VAO)
    unsigned int vboId[5];      // GPU buffer handles (5 types of vertex data)
} rvkVertexBuffer;

// Draw call type
typedef struct rvkDrawCall {
    int mode;                   // Drawing mode: LINES, TRIANGLES, QUADS
    int vertexCount;            // Number of vertex of the draw
    int vertexAlignment;        // Number of vertex required for index alignment (LINES, TRIANGLES)
    unsigned int textureId;     // Texture id to be used on the draw
} rvkDrawCall;

// Render batch type
typedef struct rvkRenderBatch {
    int bufferCount;            // Number of vertex buffers (multi-buffering support)
    int currentBuffer;          // Current buffer tracking in case of multi-buffering
    rvkVertexBuffer *vertexBuffer; // Dynamic buffer(s) for vertex data
    rvkDrawCall *draws;         // Draw calls array, depends on textureId
    int drawCounter;            // Draw calls counter
    float currentDepth;         // Current depth value for next draw
} rvkRenderBatch;

// Renderer backend version
typedef enum {
    RL_OPENGL_SOFTWARE = 0,
    RL_OPENGL_11,
    RL_OPENGL_21,
    RL_OPENGL_33,
    RL_OPENGL_43,
    RL_OPENGL_ES_20,
    RL_OPENGL_ES_30,
    RL_VULKAN = 100             // rvk backend (returned by rvkGetVersion)
} rvkRendererVersion;

// Trace log level (mirrors raylib values)
typedef enum {
    RL_LOG_ALL = 0, RL_LOG_TRACE, RL_LOG_DEBUG, RL_LOG_INFO,
    RL_LOG_WARNING, RL_LOG_ERROR, RL_LOG_FATAL, RL_LOG_NONE
} rvkTraceLogLevel;

// Texture pixel formats
typedef enum {
    RL_PIXELFORMAT_UNCOMPRESSED_GRAYSCALE = 1,
    RL_PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA,
    RL_PIXELFORMAT_UNCOMPRESSED_R5G6B5,
    RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8,
    RL_PIXELFORMAT_UNCOMPRESSED_R5G5B5A1,
    RL_PIXELFORMAT_UNCOMPRESSED_R4G4B4A4,
    RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8,
    RL_PIXELFORMAT_UNCOMPRESSED_R32,
    RL_PIXELFORMAT_UNCOMPRESSED_R32G32B32,
    RL_PIXELFORMAT_UNCOMPRESSED_R32G32B32A32,
    RL_PIXELFORMAT_UNCOMPRESSED_R16,
    RL_PIXELFORMAT_UNCOMPRESSED_R16G16B16,
    RL_PIXELFORMAT_UNCOMPRESSED_R16G16B16A16,
    RL_PIXELFORMAT_COMPRESSED_DXT1_RGB,
    RL_PIXELFORMAT_COMPRESSED_DXT1_RGBA,
    RL_PIXELFORMAT_COMPRESSED_DXT3_RGBA,
    RL_PIXELFORMAT_COMPRESSED_DXT5_RGBA,
    RL_PIXELFORMAT_COMPRESSED_ETC1_RGB,
    RL_PIXELFORMAT_COMPRESSED_ETC2_RGB,
    RL_PIXELFORMAT_COMPRESSED_ETC2_EAC_RGBA,
    RL_PIXELFORMAT_COMPRESSED_PVRT_RGB,
    RL_PIXELFORMAT_COMPRESSED_PVRT_RGBA,
    RL_PIXELFORMAT_COMPRESSED_ASTC_4x4_RGBA,
    RL_PIXELFORMAT_COMPRESSED_ASTC_8x8_RGBA
} rvkPixelFormat;

// Texture filter mode
typedef enum {
    RL_TEXTURE_FILTER_POINT = 0,
    RL_TEXTURE_FILTER_BILINEAR,
    RL_TEXTURE_FILTER_TRILINEAR,
    RL_TEXTURE_FILTER_ANISOTROPIC_4X,
    RL_TEXTURE_FILTER_ANISOTROPIC_8X,
    RL_TEXTURE_FILTER_ANISOTROPIC_16X
} rvkTextureFilter;

// Color blending modes (pre-defined)
typedef enum {
    RL_BLEND_ALPHA = 0,
    RL_BLEND_ADDITIVE,
    RL_BLEND_MULTIPLIED,
    RL_BLEND_ADD_COLORS,
    RL_BLEND_SUBTRACT_COLORS,
    RL_BLEND_ALPHA_PREMULTIPLY,
    RL_BLEND_CUSTOM,
    RL_BLEND_CUSTOM_SEPARATE
} rvkBlendMode;

// Shader location point type
typedef enum {
    RL_SHADER_LOC_VERTEX_POSITION = 0,
    RL_SHADER_LOC_VERTEX_TEXCOORD01,
    RL_SHADER_LOC_VERTEX_TEXCOORD02,
    RL_SHADER_LOC_VERTEX_NORMAL,
    RL_SHADER_LOC_VERTEX_TANGENT,
    RL_SHADER_LOC_VERTEX_COLOR,
    RL_SHADER_LOC_MATRIX_MVP,
    RL_SHADER_LOC_MATRIX_VIEW,
    RL_SHADER_LOC_MATRIX_PROJECTION,
    RL_SHADER_LOC_MATRIX_MODEL,
    RL_SHADER_LOC_MATRIX_NORMAL,
    RL_SHADER_LOC_VECTOR_VIEW,
    RL_SHADER_LOC_COLOR_DIFFUSE,
    RL_SHADER_LOC_COLOR_SPECULAR,
    RL_SHADER_LOC_COLOR_AMBIENT,
    RL_SHADER_LOC_MAP_ALBEDO,
    RL_SHADER_LOC_MAP_METALNESS,
    RL_SHADER_LOC_MAP_NORMAL,
    RL_SHADER_LOC_MAP_ROUGHNESS,
    RL_SHADER_LOC_MAP_OCCLUSION,
    RL_SHADER_LOC_MAP_EMISSION,
    RL_SHADER_LOC_MAP_HEIGHT,
    RL_SHADER_LOC_MAP_CUBEMAP,
    RL_SHADER_LOC_MAP_IRRADIANCE,
    RL_SHADER_LOC_MAP_PREFILTER,
    RL_SHADER_LOC_MAP_BRDF
} rvkShaderLocationIndex;

#define RL_SHADER_LOC_MAP_DIFFUSE       RL_SHADER_LOC_MAP_ALBEDO
#define RL_SHADER_LOC_MAP_SPECULAR      RL_SHADER_LOC_MAP_METALNESS

// Shader uniform data type
typedef enum {
    RL_SHADER_UNIFORM_FLOAT = 0,
    RL_SHADER_UNIFORM_VEC2, RL_SHADER_UNIFORM_VEC3, RL_SHADER_UNIFORM_VEC4,
    RL_SHADER_UNIFORM_INT, RL_SHADER_UNIFORM_IVEC2, RL_SHADER_UNIFORM_IVEC3, RL_SHADER_UNIFORM_IVEC4,
    RL_SHADER_UNIFORM_UINT, RL_SHADER_UNIFORM_UIVEC2, RL_SHADER_UNIFORM_UIVEC3, RL_SHADER_UNIFORM_UIVEC4,
    RL_SHADER_UNIFORM_SAMPLER2D
} rvkShaderUniformDataType;

// Shader attribute data type
typedef enum {
    RL_SHADER_ATTRIB_FLOAT = 0,
    RL_SHADER_ATTRIB_VEC2, RL_SHADER_ATTRIB_VEC3, RL_SHADER_ATTRIB_VEC4
} rvkShaderAttributeDataType;

// Framebuffer attachment type
typedef enum {
    RL_ATTACHMENT_COLOR_CHANNEL0 = 0, RL_ATTACHMENT_COLOR_CHANNEL1, RL_ATTACHMENT_COLOR_CHANNEL2,
    RL_ATTACHMENT_COLOR_CHANNEL3, RL_ATTACHMENT_COLOR_CHANNEL4, RL_ATTACHMENT_COLOR_CHANNEL5,
    RL_ATTACHMENT_COLOR_CHANNEL6, RL_ATTACHMENT_COLOR_CHANNEL7,
    RL_ATTACHMENT_DEPTH = 100, RL_ATTACHMENT_STENCIL = 200
} rvkFramebufferAttachType;

// Framebuffer texture attachment type
typedef enum {
    RL_ATTACHMENT_CUBEMAP_POSITIVE_X = 0, RL_ATTACHMENT_CUBEMAP_NEGATIVE_X,
    RL_ATTACHMENT_CUBEMAP_POSITIVE_Y, RL_ATTACHMENT_CUBEMAP_NEGATIVE_Y,
    RL_ATTACHMENT_CUBEMAP_POSITIVE_Z, RL_ATTACHMENT_CUBEMAP_NEGATIVE_Z,
    RL_ATTACHMENT_TEXTURE2D = 100, RL_ATTACHMENT_RENDERBUFFER = 200
} rvkFramebufferAttachTextureType;

// Face culling mode
typedef enum { RL_CULL_FACE_FRONT = 0, RL_CULL_FACE_BACK } rvkCullMode;

//----------------------------------------------------------------------------------
// Functions Declaration
//----------------------------------------------------------------------------------
#if defined(__cplusplus)
extern "C" {
#endif

//------ Matrix operations ------
RVKAPI void rvkMatrixMode(int mode);
RVKAPI void rvkPushMatrix(void);
RVKAPI void rvkPopMatrix(void);
RVKAPI void rvkLoadIdentity(void);
RVKAPI void rvkTranslatef(float x, float y, float z);
RVKAPI void rvkRotatef(float angle, float x, float y, float z);
RVKAPI void rvkScalef(float x, float y, float z);
RVKAPI void rvkMultMatrixf(const float *matf);
RVKAPI void rvkFrustum(double left, double right, double bottom, double top, double znear, double zfar);
RVKAPI void rvkOrtho(double left, double right, double bottom, double top, double znear, double zfar);
RVKAPI void rvkViewport(int x, int y, int width, int height);
RVKAPI void rvkSetClipPlanes(double nearPlane, double farPlane);
RVKAPI double rvkGetCullDistanceNear(void);
RVKAPI double rvkGetCullDistanceFar(void);

//------ Vertex level operations (immediate mode) ------
RVKAPI void rvkBegin(int mode);
RVKAPI void rvkEnd(void);
RVKAPI void rvkVertex2i(int x, int y);
RVKAPI void rvkVertex2f(float x, float y);
RVKAPI void rvkVertex3f(float x, float y, float z);
RVKAPI void rvkTexCoord2f(float x, float y);
RVKAPI void rvkNormal3f(float x, float y, float z);
RVKAPI void rvkColor4ub(unsigned char r, unsigned char g, unsigned char b, unsigned char a);
RVKAPI void rvkColor3f(float x, float y, float z);
RVKAPI void rvkColor4f(float x, float y, float z, float w);

//------ Vertex buffers state ------
RVKAPI bool rvkEnableVertexArray(unsigned int vaoId);
RVKAPI void rvkDisableVertexArray(void);
RVKAPI void rvkEnableVertexBuffer(unsigned int id);
RVKAPI void rvkDisableVertexBuffer(void);
RVKAPI void rvkEnableVertexBufferElement(unsigned int id);
RVKAPI void rvkDisableVertexBufferElement(void);
RVKAPI void rvkEnableVertexAttribute(unsigned int index);
RVKAPI void rvkDisableVertexAttribute(unsigned int index);

//------ Textures state ------
RVKAPI void rvkActiveTextureSlot(int slot);
RVKAPI void rvkEnableTexture(unsigned int id);
RVKAPI void rvkDisableTexture(void);
RVKAPI void rvkEnableTextureCubemap(unsigned int id);
RVKAPI void rvkDisableTextureCubemap(void);
RVKAPI void rvkTextureParameters(unsigned int id, int param, int value);
RVKAPI void rvkCubemapParameters(unsigned int id, int param, int value);

//------ Shader state ------
RVKAPI void rvkEnableShader(unsigned int id);
RVKAPI void rvkDisableShader(void);

//------ Framebuffer state ------
RVKAPI void rvkEnableFramebuffer(unsigned int id);
RVKAPI void rvkDisableFramebuffer(void);
RVKAPI unsigned int rvkGetActiveFramebuffer(void);
RVKAPI void rvkActiveDrawBuffers(int count);
RVKAPI void rvkBlitFramebuffer(int srcX, int srcY, int srcWidth, int srcHeight, int dstX, int dstY, int dstWidth, int dstHeight, int bufferMask);
RVKAPI void rvkBindFramebuffer(unsigned int target, unsigned int framebuffer);

//------ General render state ------
RVKAPI void rvkEnableColorBlend(void);
RVKAPI void rvkDisableColorBlend(void);
RVKAPI void rvkEnableDepthTest(void);
RVKAPI void rvkDisableDepthTest(void);
RVKAPI void rvkEnableDepthMask(void);
RVKAPI void rvkDisableDepthMask(void);
RVKAPI void rvkEnableBackfaceCulling(void);
RVKAPI void rvkDisableBackfaceCulling(void);
RVKAPI void rvkColorMask(bool r, bool g, bool b, bool a);
RVKAPI void rvkSetCullFace(int mode);
RVKAPI void rvkEnableScissorTest(void);
RVKAPI void rvkDisableScissorTest(void);
RVKAPI void rvkScissor(int x, int y, int width, int height);
RVKAPI void rvkEnablePointMode(void);
RVKAPI void rvkDisablePointMode(void);
RVKAPI void rvkSetPointSize(float size);
RVKAPI float rvkGetPointSize(void);
RVKAPI void rvkEnableWireMode(void);
RVKAPI void rvkDisableWireMode(void);
RVKAPI void rvkSetLineWidth(float width);
RVKAPI float rvkGetLineWidth(void);
RVKAPI void rvkEnableSmoothLines(void);
RVKAPI void rvkDisableSmoothLines(void);
RVKAPI void rvkEnableStereoRender(void);
RVKAPI void rvkDisableStereoRender(void);
RVKAPI bool rvkIsStereoRenderEnabled(void);

RVKAPI void rvkClearColor(unsigned char r, unsigned char g, unsigned char b, unsigned char a);
RVKAPI void rvkClearScreenBuffers(void);
RVKAPI void rvkCheckErrors(void);
RVKAPI void rvkSetBlendMode(int mode);
RVKAPI void rvkSetBlendFactors(int glSrcFactor, int glDstFactor, int glEquation);
RVKAPI void rvkSetBlendFactorsSeparate(int glSrcRGB, int glDstRGB, int glSrcAlpha, int glDstAlpha, int glEqRGB, int glEqAlpha);

//------ Initialization and info ------
RVKAPI void rvkglInit(int width, int height);
RVKAPI void rvkglClose(void);
RVKAPI void rvkLoadExtensions(void *loader);
RVKAPI void *rvkGetProcAddress(const char *procName);
RVKAPI int rvkGetVersion(void);
RVKAPI void rvkSetFramebufferWidth(int width);
RVKAPI int rvkGetFramebufferWidth(void);
RVKAPI void rvkSetFramebufferHeight(int height);
RVKAPI int rvkGetFramebufferHeight(void);
RVKAPI unsigned int rvkGetTextureIdDefault(void);
RVKAPI unsigned int rvkGetShaderIdDefault(void);
RVKAPI int *rvkGetShaderLocsDefault(void);

//------ Render batch management ------
RVKAPI rvkRenderBatch rvkLoadRenderBatch(int numBuffers, int bufferElements);
RVKAPI void rvkUnloadRenderBatch(rvkRenderBatch batch);
RVKAPI void rvkDrawRenderBatch(rvkRenderBatch *batch);
RVKAPI void rvkSetRenderBatchActive(rvkRenderBatch *batch);
RVKAPI void rvkDrawRenderBatchActive(void);
RVKAPI bool rvkCheckRenderBatchLimit(int vCount);
RVKAPI void rvkSetTexture(unsigned int id);

//------ Vertex buffers management ------
RVKAPI unsigned int rvkLoadVertexArray(void);
RVKAPI unsigned int rvkLoadVertexBuffer(const void *buffer, int size, bool dynamic);
RVKAPI unsigned int rvkLoadVertexBufferElement(const void *buffer, int size, bool dynamic);
RVKAPI void rvkUpdateVertexBuffer(unsigned int bufferId, const void *data, int dataSize, int offset);
RVKAPI void rvkUpdateVertexBufferElements(unsigned int id, const void *data, int dataSize, int offset);
RVKAPI void rvkUnloadVertexArray(unsigned int vaoId);
RVKAPI void rvkUnloadVertexBuffer(unsigned int vboId);
RVKAPI void rvkSetVertexAttribute(unsigned int index, int compSize, int type, bool normalized, int stride, int offset);
RVKAPI void rvkSetVertexAttributeDivisor(unsigned int index, int divisor);
RVKAPI void rvkSetVertexAttributeDefault(int locIndex, const void *value, int attribType, int count);
RVKAPI void rvkDrawVertexArray(int offset, int count);
RVKAPI void rvkDrawVertexArrayElements(int offset, int count, const void *buffer);
RVKAPI void rvkDrawVertexArrayInstanced(int offset, int count, int instances);
RVKAPI void rvkDrawVertexArrayElementsInstanced(int offset, int count, const void *buffer, int instances);

//------ Textures management ------
RVKAPI unsigned int rvkLoadTexture(const void *data, int width, int height, int format, int mipmapCount);
RVKAPI unsigned int rvkLoadTextureDepth(int width, int height, bool useRenderBuffer);
RVKAPI unsigned int rvkLoadTextureCubemap(const void *data, int size, int format, int mipmapCount);
RVKAPI void rvkUpdateTexture(unsigned int id, int offsetX, int offsetY, int width, int height, int format, const void *data);
RVKAPI void rvkGetGlTextureFormats(int format, unsigned int *glInternalFormat, unsigned int *glFormat, unsigned int *glType);
RVKAPI const char *rvkGetPixelFormatName(unsigned int format);
RVKAPI void rvkUnloadTexture(unsigned int id);
RVKAPI void rvkGenTextureMipmaps(unsigned int id, int width, int height, int format, int *mipmaps);
RVKAPI void *rvkReadTexturePixels(unsigned int id, int width, int height, int format);
RVKAPI unsigned char *rvkReadScreenPixels(int width, int height);

//------ Framebuffer management ------
RVKAPI unsigned int rvkLoadFramebuffer(void);
RVKAPI void rvkFramebufferAttach(unsigned int id, unsigned int texId, int attachType, int texType, int mipLevel);
RVKAPI bool rvkFramebufferComplete(unsigned int id);
RVKAPI void rvkUnloadFramebuffer(unsigned int id);
RVKAPI void rvkCopyFramebuffer(int x, int y, int width, int height, int format, void *pixels);
RVKAPI void rvkResizeFramebuffer(int width, int height);

//------ Shaders management ------
RVKAPI unsigned int rvkLoadShader(const char *code, int type);
RVKAPI unsigned int rvkLoadShaderProgram(const char *vsCode, const char *fsCode);
RVKAPI unsigned int rvkLoadShaderProgramEx(unsigned int vsId, unsigned int fsId);
RVKAPI unsigned int rvkLoadShaderProgramCompute(unsigned int csId);
RVKAPI void rvkUnloadShader(unsigned int id);
RVKAPI void rvkUnloadShaderProgram(unsigned int id);
RVKAPI int rvkGetLocationUniform(unsigned int id, const char *uniformName);
RVKAPI int rvkGetLocationAttrib(unsigned int id, const char *attribName);
RVKAPI void rvkSetUniform(int locIndex, const void *value, int uniformType, int count);
RVKAPI void rvkSetUniformMatrix(int locIndex, Matrix mat);
RVKAPI void rvkSetUniformMatrices(int locIndex, const Matrix *mat, int count);
RVKAPI void rvkSetUniformSampler(int locIndex, unsigned int textureId);
RVKAPI void rvkSetShader(unsigned int id, int *locs);

//------ Compute shader management ------
RVKAPI void rvkComputeShaderDispatch(unsigned int groupX, unsigned int groupY, unsigned int groupZ);

//------ Shader storage buffer (SSBO) management ------
RVKAPI unsigned int rvkLoadShaderBuffer(unsigned int size, const void *data, int usageHint);
RVKAPI void rvkUnloadShaderBuffer(unsigned int ssboId);
RVKAPI void rvkUpdateShaderBuffer(unsigned int id, const void *data, unsigned int dataSize, unsigned int offset);
RVKAPI void rvkBindShaderBuffer(unsigned int id, unsigned int index);
RVKAPI void rvkReadShaderBuffer(unsigned int id, void *dest, unsigned int count, unsigned int offset);
RVKAPI void rvkCopyShaderBuffer(unsigned int destId, unsigned int srcId, unsigned int destOffset, unsigned int srcOffset, unsigned int count);
RVKAPI unsigned int rvkGetShaderBufferSize(unsigned int id);

//------ Buffer management ------
RVKAPI void rvkBindImageTexture(unsigned int id, unsigned int index, int format, bool readonly);

//------ Matrix state management ------
RVKAPI Matrix rvkGetMatrixModelview(void);
RVKAPI Matrix rvkGetMatrixProjection(void);
RVKAPI Matrix rvkGetMatrixTransform(void);
RVKAPI Matrix rvkGetMatrixProjectionStereo(int eye);
RVKAPI Matrix rvkGetMatrixViewOffsetStereo(int eye);
RVKAPI void rvkSetMatrixProjection(Matrix proj);
RVKAPI void rvkSetMatrixModelview(Matrix view);
RVKAPI void rvkSetMatrixProjectionStereo(Matrix right, Matrix left);
RVKAPI void rvkSetMatrixViewOffsetStereo(Matrix right, Matrix left);

//------ Quick and dirty mesh/model drawing ------
RVKAPI void rvkLoadDrawCube(void);
RVKAPI void rvkLoadDrawQuad(void);

//------ rvk-specific frame lifecycle hooks (called by the platform layer) ------
RVKAPI void rvkInit(void *glfwWindow, int width, int height); // Full Vulkan device/swapchain init (vk-bootstrap)
RVKAPI void rvkBeginFrame(void);                              // Acquire image, begin command buffer + dynamic rendering
RVKAPI void rvkEndFrame(void);                                // End rendering, submit
RVKAPI void rvkPresent(void);                                 // Present swapchain image, advance frame-in-flight
RVKAPI void rvkResizeSwapchain(int width, int height);        // Recreate swapchain on resize/out-of-date

#if defined(__cplusplus)
}
#endif

//----------------------------------------------------------------------------------
// rlgl compatibility shim
//
// Defining RVK_ENABLE_RLGL_COMPAT (default ON) aliases every rl* symbol to its rvk*
// counterpart so raylib's higher-level modules compile against this header without
// edits during the rl*->rvk* migration (plan phase P10 removes the shim).
//----------------------------------------------------------------------------------
#ifndef RVK_DISABLE_RLGL_COMPAT
    #define RVK_ENABLE_RLGL_COMPAT 1
#endif

#if defined(RVK_ENABLE_RLGL_COMPAT)
    // Types
    typedef rvkVertexBuffer  rlVertexBuffer;
    typedef rvkDrawCall      rlDrawCall;
    typedef rvkRenderBatch   rlRenderBatch;
    typedef rvkRendererVersion       rlGlVersion;
    typedef rvkTraceLogLevel         rlTraceLogLevel;
    typedef rvkPixelFormat           rlPixelFormat;
    typedef rvkTextureFilter         rlTextureFilter;
    typedef rvkBlendMode             rlBlendMode;
    typedef rvkShaderLocationIndex   rlShaderLocationIndex;
    typedef rvkShaderUniformDataType rlShaderUniformDataType;
    typedef rvkShaderAttributeDataType rlShaderAttributeDataType;
    typedef rvkFramebufferAttachType rlFramebufferAttachType;
    typedef rvkFramebufferAttachTextureType rlFramebufferAttachTextureType;
    typedef rvkCullMode              rlCullMode;

    // Functions (1:1 name mapping)
    #define rlMatrixMode              rvkMatrixMode
    #define rlPushMatrix              rvkPushMatrix
    #define rlPopMatrix               rvkPopMatrix
    #define rlLoadIdentity            rvkLoadIdentity
    #define rlTranslatef              rvkTranslatef
    #define rlRotatef                 rvkRotatef
    #define rlScalef                  rvkScalef
    #define rlMultMatrixf             rvkMultMatrixf
    #define rlFrustum                 rvkFrustum
    #define rlOrtho                   rvkOrtho
    #define rlViewport                rvkViewport
    #define rlSetClipPlanes           rvkSetClipPlanes
    #define rlGetCullDistanceNear     rvkGetCullDistanceNear
    #define rlGetCullDistanceFar      rvkGetCullDistanceFar
    #define rlBegin                   rvkBegin
    #define rlEnd                     rvkEnd
    #define rlVertex2i                rvkVertex2i
    #define rlVertex2f                rvkVertex2f
    #define rlVertex3f                rvkVertex3f
    #define rlTexCoord2f              rvkTexCoord2f
    #define rlNormal3f                rvkNormal3f
    #define rlColor4ub                rvkColor4ub
    #define rlColor3f                 rvkColor3f
    #define rlColor4f                 rvkColor4f
    #define rlEnableVertexArray       rvkEnableVertexArray
    #define rlDisableVertexArray      rvkDisableVertexArray
    #define rlEnableVertexBuffer      rvkEnableVertexBuffer
    #define rlDisableVertexBuffer     rvkDisableVertexBuffer
    #define rlEnableVertexBufferElement  rvkEnableVertexBufferElement
    #define rlDisableVertexBufferElement rvkDisableVertexBufferElement
    #define rlEnableVertexAttribute   rvkEnableVertexAttribute
    #define rlDisableVertexAttribute  rvkDisableVertexAttribute
    #define rlActiveTextureSlot       rvkActiveTextureSlot
    #define rlEnableTexture           rvkEnableTexture
    #define rlDisableTexture          rvkDisableTexture
    #define rlEnableTextureCubemap    rvkEnableTextureCubemap
    #define rlDisableTextureCubemap   rvkDisableTextureCubemap
    #define rlTextureParameters       rvkTextureParameters
    #define rlCubemapParameters       rvkCubemapParameters
    #define rlEnableShader            rvkEnableShader
    #define rlDisableShader           rvkDisableShader
    #define rlEnableFramebuffer       rvkEnableFramebuffer
    #define rlDisableFramebuffer      rvkDisableFramebuffer
    #define rlGetActiveFramebuffer    rvkGetActiveFramebuffer
    #define rlActiveDrawBuffers       rvkActiveDrawBuffers
    #define rlBlitFramebuffer         rvkBlitFramebuffer
    #define rlBindFramebuffer         rvkBindFramebuffer
    #define rlEnableColorBlend        rvkEnableColorBlend
    #define rlDisableColorBlend       rvkDisableColorBlend
    #define rlEnableDepthTest         rvkEnableDepthTest
    #define rlDisableDepthTest        rvkDisableDepthTest
    #define rlEnableDepthMask         rvkEnableDepthMask
    #define rlDisableDepthMask        rvkDisableDepthMask
    #define rlEnableBackfaceCulling   rvkEnableBackfaceCulling
    #define rlDisableBackfaceCulling  rvkDisableBackfaceCulling
    #define rlColorMask               rvkColorMask
    #define rlSetCullFace             rvkSetCullFace
    #define rlEnableScissorTest       rvkEnableScissorTest
    #define rlDisableScissorTest      rvkDisableScissorTest
    #define rlScissor                 rvkScissor
    #define rlEnablePointMode         rvkEnablePointMode
    #define rlDisablePointMode        rvkDisablePointMode
    #define rlSetPointSize            rvkSetPointSize
    #define rlGetPointSize            rvkGetPointSize
    #define rlEnableWireMode          rvkEnableWireMode
    #define rlDisableWireMode         rvkDisableWireMode
    #define rlSetLineWidth            rvkSetLineWidth
    #define rlGetLineWidth            rvkGetLineWidth
    #define rlEnableSmoothLines       rvkEnableSmoothLines
    #define rlDisableSmoothLines      rvkDisableSmoothLines
    #define rlEnableStereoRender      rvkEnableStereoRender
    #define rlDisableStereoRender     rvkDisableStereoRender
    #define rlIsStereoRenderEnabled   rvkIsStereoRenderEnabled
    #define rlClearColor              rvkClearColor
    #define rlClearScreenBuffers      rvkClearScreenBuffers
    #define rlCheckErrors             rvkCheckErrors
    #define rlSetBlendMode            rvkSetBlendMode
    #define rlSetBlendFactors         rvkSetBlendFactors
    #define rlSetBlendFactorsSeparate rvkSetBlendFactorsSeparate
    #define rlglInit                  rvkglInit
    #define rlglClose                 rvkglClose
    #define rlLoadExtensions          rvkLoadExtensions
    #define rlGetProcAddress          rvkGetProcAddress
    #define rlGetVersion              rvkGetVersion
    #define rlSetFramebufferWidth     rvkSetFramebufferWidth
    #define rlGetFramebufferWidth     rvkGetFramebufferWidth
    #define rlSetFramebufferHeight    rvkSetFramebufferHeight
    #define rlGetFramebufferHeight    rvkGetFramebufferHeight
    #define rlGetTextureIdDefault     rvkGetTextureIdDefault
    #define rlGetShaderIdDefault      rvkGetShaderIdDefault
    #define rlGetShaderLocsDefault    rvkGetShaderLocsDefault
    #define rlLoadRenderBatch         rvkLoadRenderBatch
    #define rlUnloadRenderBatch       rvkUnloadRenderBatch
    #define rlDrawRenderBatch         rvkDrawRenderBatch
    #define rlSetRenderBatchActive    rvkSetRenderBatchActive
    #define rlDrawRenderBatchActive   rvkDrawRenderBatchActive
    #define rlCheckRenderBatchLimit   rvkCheckRenderBatchLimit
    #define rlSetTexture              rvkSetTexture
    #define rlLoadVertexArray         rvkLoadVertexArray
    #define rlLoadVertexBuffer        rvkLoadVertexBuffer
    #define rlLoadVertexBufferElement rvkLoadVertexBufferElement
    #define rlUpdateVertexBuffer      rvkUpdateVertexBuffer
    #define rlUpdateVertexBufferElements rvkUpdateVertexBufferElements
    #define rlUnloadVertexArray       rvkUnloadVertexArray
    #define rlUnloadVertexBuffer      rvkUnloadVertexBuffer
    #define rlSetVertexAttribute      rvkSetVertexAttribute
    #define rlSetVertexAttributeDivisor rvkSetVertexAttributeDivisor
    #define rlSetVertexAttributeDefault rvkSetVertexAttributeDefault
    #define rlDrawVertexArray         rvkDrawVertexArray
    #define rlDrawVertexArrayElements rvkDrawVertexArrayElements
    #define rlDrawVertexArrayInstanced rvkDrawVertexArrayInstanced
    #define rlDrawVertexArrayElementsInstanced rvkDrawVertexArrayElementsInstanced
    #define rlLoadTexture             rvkLoadTexture
    #define rlLoadTextureDepth        rvkLoadTextureDepth
    #define rlLoadTextureCubemap      rvkLoadTextureCubemap
    #define rlUpdateTexture           rvkUpdateTexture
    #define rlGetGlTextureFormats     rvkGetGlTextureFormats
    #define rlGetPixelFormatName      rvkGetPixelFormatName
    #define rlUnloadTexture           rvkUnloadTexture
    #define rlGenTextureMipmaps       rvkGenTextureMipmaps
    #define rlReadTexturePixels       rvkReadTexturePixels
    #define rlReadScreenPixels        rvkReadScreenPixels
    #define rlLoadFramebuffer         rvkLoadFramebuffer
    #define rlFramebufferAttach       rvkFramebufferAttach
    #define rlFramebufferComplete     rvkFramebufferComplete
    #define rlUnloadFramebuffer       rvkUnloadFramebuffer
    #define rlCopyFramebuffer         rvkCopyFramebuffer
    #define rlResizeFramebuffer       rvkResizeFramebuffer
    #define rlLoadShader              rvkLoadShader
    #define rlLoadShaderProgram       rvkLoadShaderProgram
    #define rlLoadShaderProgramEx     rvkLoadShaderProgramEx
    #define rlLoadShaderProgramCompute rvkLoadShaderProgramCompute
    #define rlUnloadShader            rvkUnloadShader
    #define rlUnloadShaderProgram     rvkUnloadShaderProgram
    #define rlGetLocationUniform      rvkGetLocationUniform
    #define rlGetLocationAttrib       rvkGetLocationAttrib
    #define rlSetUniform              rvkSetUniform
    #define rlSetUniformMatrix        rvkSetUniformMatrix
    #define rlSetUniformMatrices      rvkSetUniformMatrices
    #define rlSetUniformSampler       rvkSetUniformSampler
    #define rlSetShader               rvkSetShader
    #define rlComputeShaderDispatch   rvkComputeShaderDispatch
    #define rlLoadShaderBuffer        rvkLoadShaderBuffer
    #define rlUnloadShaderBuffer      rvkUnloadShaderBuffer
    #define rlUpdateShaderBuffer      rvkUpdateShaderBuffer
    #define rlBindShaderBuffer        rvkBindShaderBuffer
    #define rlReadShaderBuffer        rvkReadShaderBuffer
    #define rlCopyShaderBuffer        rvkCopyShaderBuffer
    #define rlGetShaderBufferSize     rvkGetShaderBufferSize
    #define rlBindImageTexture        rvkBindImageTexture
    #define rlGetMatrixModelview      rvkGetMatrixModelview
    #define rlGetMatrixProjection     rvkGetMatrixProjection
    #define rlGetMatrixTransform      rvkGetMatrixTransform
    #define rlGetMatrixProjectionStereo rvkGetMatrixProjectionStereo
    #define rlGetMatrixViewOffsetStereo rvkGetMatrixViewOffsetStereo
    #define rlSetMatrixProjection     rvkSetMatrixProjection
    #define rlSetMatrixModelview      rvkSetMatrixModelview
    #define rlSetMatrixProjectionStereo rvkSetMatrixProjectionStereo
    #define rlSetMatrixViewOffsetStereo rvkSetMatrixViewOffsetStereo
    #define rlLoadDrawCube            rvkLoadDrawCube
    #define rlLoadDrawQuad            rvkLoadDrawQuad
#endif // RVK_ENABLE_RLGL_COMPAT

#endif // RVK_H
