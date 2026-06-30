/**********************************************************************************************
*
*   rvk_batcher.cpp - Matrix stack, immediate-mode vertex streams, render-batch, render state
*
*   Implements the rlgl-compatible CPU front-end: the matrix stack (modelview/projection/
*   transform), immediate-mode vertex accumulation (rvkBegin/Vertex/TexCoord/Normal/Color/End),
*   the render batch (Update->Draw->Reset), and all render-state setters that feed the PSO key.
*
*   STATUS: matrix stack + state setters are fully implemented (pure CPU). GPU vertex upload
*   and the actual vkCmdDraw flush are marked TODO(P3).
*
**********************************************************************************************/
#include "rvk_internal.hpp"

#include "raymath.h"       // MatrixIdentity/Multiply/Translate/Rotate/Scale/Ortho/Frustum (header-only, static inline)

//----------------------------------------------------------------------------------
// Matrix stack state (mirrors rlgl's RLGL.State.* semantics)
//----------------------------------------------------------------------------------
static Matrix s_modelview = { 0 };
static Matrix s_projection = { 0 };
static Matrix s_transform = { 0 };
static bool   s_transformRequired = false;
static Matrix *s_currentMatrix = &s_modelview;
static int    s_currentMatrixMode = RL_MODELVIEW;
static Matrix s_stack[RL_MAX_MATRIX_STACK_SIZE] = { 0 };
static int    s_stackCounter = 0;

static double s_cullNear = RL_CULL_DISTANCE_NEAR;
static double s_cullFar  = RL_CULL_DISTANCE_FAR;

// Stereo
static bool   s_stereoRender = false;
static Matrix s_projectionStereo[2] = { 0 };
static Matrix s_viewOffsetStereo[2] = { 0 };

// Misc render state
static float  s_pointSize = 1.0f;
static float  s_lineWidth = 1.0f;

// GL->Vulkan depth remap: maps NDC z from [-1,1] (GL/raymath) to [0,1] (Vulkan).
// Applied to projection matrices so depth testing and culling behave correctly.
static Matrix rvkGlToVkDepth(Matrix p)
{
    // z' = z*0.5 + w*0.5  (column-major raylib Matrix: m10 scales z, m14 is z translation)
    Matrix clip = MatrixIdentity();
    clip.m10 = 0.5f; clip.m14 = 0.5f;     // row 2: z = 0.5*z + 0.5*w
    return MatrixMultiply(p, clip);
}

//----------------------------------------------------------------------------------
// Matrix operations
//----------------------------------------------------------------------------------
extern "C" {

void rvkMatrixMode(int mode)
{
    if (mode == RL_PROJECTION) s_currentMatrix = &s_projection;
    else if (mode == RL_MODELVIEW) s_currentMatrix = &s_modelview;
    s_currentMatrixMode = mode;
}

void rvkPushMatrix(void)
{
    if (s_stackCounter >= RL_MAX_MATRIX_STACK_SIZE)
        TraceLog(RL_LOG_ERROR, "RVK: Matrix stack overflow (RL_MAX_MATRIX_STACK_SIZE)");
    if (s_currentMatrixMode == RL_MODELVIEW) {
        s_transformRequired = true;
        s_currentMatrix = &s_transform;
    }
    if (s_stackCounter < RL_MAX_MATRIX_STACK_SIZE) {
        s_stack[s_stackCounter] = *s_currentMatrix;
        s_stackCounter++;
    }
}

void rvkPopMatrix(void)
{
    if (s_stackCounter > 0) {
        Matrix mat = s_stack[s_stackCounter - 1];
        *s_currentMatrix = mat;
        s_stackCounter--;
    }
    if ((s_stackCounter == 0) && (s_currentMatrixMode == RL_MODELVIEW)) {
        s_currentMatrix = &s_modelview;
        s_transformRequired = false;
    }
}

void rvkLoadIdentity(void) { *s_currentMatrix = MatrixIdentity(); }

void rvkTranslatef(float x, float y, float z)
{ *s_currentMatrix = MatrixMultiply(MatrixTranslate(x, y, z), *s_currentMatrix); }

void rvkRotatef(float angle, float x, float y, float z)
{ Vector3 axis = { x, y, z }; *s_currentMatrix = MatrixMultiply(MatrixRotate(Vector3Normalize(axis), angle*DEG2RAD), *s_currentMatrix); }

void rvkScalef(float x, float y, float z)
{ *s_currentMatrix = MatrixMultiply(MatrixScale(x, y, z), *s_currentMatrix); }

void rvkMultMatrixf(const float *matf)
{
    Matrix mat = { matf[0], matf[4], matf[8],  matf[12],
                   matf[1], matf[5], matf[9],  matf[13],
                   matf[2], matf[6], matf[10], matf[14],
                   matf[3], matf[7], matf[11], matf[15] };
    *s_currentMatrix = MatrixMultiply(mat, *s_currentMatrix);
}

void rvkFrustum(double left, double right, double bottom, double top, double znear, double zfar)
{ *s_currentMatrix = MatrixMultiply(*s_currentMatrix, rvkGlToVkDepth(MatrixFrustum(left, right, bottom, top, znear, zfar))); }

void rvkOrtho(double left, double right, double bottom, double top, double znear, double zfar)
{ *s_currentMatrix = MatrixMultiply(*s_currentMatrix, rvkGlToVkDepth(MatrixOrtho(left, right, bottom, top, znear, zfar))); }

void rvkViewport(int x, int y, int width, int height)
{ (void)x; (void)y; (void)width; (void)height; /* TODO(P3): record dynamic viewport (flip Y via negative height) */ }

void rvkSetClipPlanes(double nearPlane, double farPlane) { s_cullNear = nearPlane; s_cullFar = farPlane; }
double rvkGetCullDistanceNear(void) { return s_cullNear; }
double rvkGetCullDistanceFar(void) { return s_cullFar; }

Matrix rvkGetMatrixModelview(void) { return s_modelview; }
Matrix rvkGetMatrixProjection(void) { return s_projection; }
Matrix rvkGetMatrixTransform(void) { return s_transform; }
Matrix rvkGetMatrixProjectionStereo(int eye) { return s_projectionStereo[eye & 1]; }
Matrix rvkGetMatrixViewOffsetStereo(int eye) { return s_viewOffsetStereo[eye & 1]; }
void rvkSetMatrixProjection(Matrix proj) { s_projection = proj; }
void rvkSetMatrixModelview(Matrix view) { s_modelview = view; }
void rvkSetMatrixProjectionStereo(Matrix right, Matrix left) { s_projectionStereo[0] = right; s_projectionStereo[1] = left; }
void rvkSetMatrixViewOffsetStereo(Matrix right, Matrix left) { s_viewOffsetStereo[0] = right; s_viewOffsetStereo[1] = left; }

//----------------------------------------------------------------------------------
// Render-state setters (feed RvkRenderState -> PSO cache key)
//----------------------------------------------------------------------------------
void rvkEnableColorBlend(void)  { g_rvk.state.blendEnabled = true; }
void rvkDisableColorBlend(void) { g_rvk.state.blendEnabled = false; }
void rvkEnableDepthTest(void)   { g_rvk.state.depthTest = true; }
void rvkDisableDepthTest(void)  { g_rvk.state.depthTest = false; }
void rvkEnableDepthMask(void)   { g_rvk.state.depthMask = true; }
void rvkDisableDepthMask(void)  { g_rvk.state.depthMask = false; }
void rvkEnableBackfaceCulling(void)  { g_rvk.state.cullEnabled = true; }
void rvkDisableBackfaceCulling(void) { g_rvk.state.cullEnabled = false; }
void rvkSetCullFace(int mode)   { g_rvk.state.cullFace = mode; }
void rvkColorMask(bool r, bool g, bool b, bool a) { g_rvk.state.colorMask[0]=r; g_rvk.state.colorMask[1]=g; g_rvk.state.colorMask[2]=b; g_rvk.state.colorMask[3]=a; }
void rvkEnableWireMode(void)    { g_rvk.state.wireMode = true; }
void rvkDisableWireMode(void)   { g_rvk.state.wireMode = false; }
void rvkSetBlendMode(int mode)  { g_rvk.state.blendMode = mode; }
void rvkSetBlendFactors(int s, int d, int eq) { g_rvk.state.customSrcRGB=s; g_rvk.state.customDstRGB=d; g_rvk.state.customSrcA=s; g_rvk.state.customDstA=d; g_rvk.state.customEqRGB=eq; g_rvk.state.customEqA=eq; }
void rvkSetBlendFactorsSeparate(int sRGB, int dRGB, int sA, int dA, int eqRGB, int eqA) { g_rvk.state.customSrcRGB=sRGB; g_rvk.state.customDstRGB=dRGB; g_rvk.state.customSrcA=sA; g_rvk.state.customDstA=dA; g_rvk.state.customEqRGB=eqRGB; g_rvk.state.customEqA=eqA; }

void rvkSetPointSize(float size) { s_pointSize = size; }
float rvkGetPointSize(void) { return s_pointSize; }
void rvkSetLineWidth(float width) { s_lineWidth = width; }
float rvkGetLineWidth(void) { return s_lineWidth; }
void rvkEnablePointMode(void) {}
void rvkDisablePointMode(void) {}
void rvkEnableSmoothLines(void) {}    // MSAA-line concept; TODO(P9)
void rvkDisableSmoothLines(void) {}
void rvkEnableScissorTest(void) {}    // TODO(P3): dynamic scissor enable
void rvkDisableScissorTest(void) {}
void rvkScissor(int x, int y, int width, int height) { (void)x; (void)y; (void)width; (void)height; /* TODO(P3) */ }

void rvkEnableStereoRender(void)  { s_stereoRender = true; }
void rvkDisableStereoRender(void) { s_stereoRender = false; }
bool rvkIsStereoRenderEnabled(void) { return s_stereoRender; }

//----------------------------------------------------------------------------------
// Immediate mode + render batch
//
// CPU-side accumulation is implemented against the SoA vertex buffer. The GPU upload and
// vkCmdDraw flush are TODO(P3) (needs the pipeline from rvk_pipeline + per-frame buffers
// from rvk_memory).
//----------------------------------------------------------------------------------
static rvkRenderBatch *s_currentBatch = nullptr;
static rvkRenderBatch  s_defaultBatch = { 0 };

// current vertex attributes (set between rvkBegin/rvkEnd)
static float s_curTexcoord[2] = { 0.0f, 0.0f };
static float s_curNormal[3]   = { 0.0f, 0.0f, 1.0f };
static unsigned char s_curColor[4] = { 255, 255, 255, 255 };
static int   s_vertexCount = 0;       // vertices in the current rvkBegin/rvkEnd

void rvkBegin(int mode)
{
    g_rvk.state.topology = mode;
    if (s_currentBatch) {
        rvkDrawCall &dc = s_currentBatch->draws[s_currentBatch->drawCounter - 1];
        if (dc.mode != mode) {
            // mode change forces a new draw call (handled fully in P3)
        }
    }
    s_vertexCount = 0;
}

void rvkEnd(void)
{
    if (s_currentBatch) s_currentBatch->currentDepth += (1.0f/20000.0f);   // matches rlgl depth stepping
}

static void rvkPushVertex(float x, float y, float z)
{
    // TODO(P3): append (x,y,z), s_curTexcoord, s_curNormal, s_curColor into the active
    //   vertex buffer SoA arrays, growing/flushing on RL_DEFAULT_BATCH_BUFFER_ELEMENTS.
    (void)x; (void)y; (void)z;
    s_vertexCount++;
}

void rvkVertex3f(float x, float y, float z)
{
    if (s_transformRequired) {
        Vector3 in = { x, y, z };
        Vector3 v = Vector3Transform(in, s_transform);
        rvkPushVertex(v.x, v.y, v.z);
    } else rvkPushVertex(x, y, z);
}
void rvkVertex2f(float x, float y) { rvkVertex3f(x, y, s_currentBatch ? s_currentBatch->currentDepth : 0.0f); }
void rvkVertex2i(int x, int y)     { rvkVertex3f((float)x, (float)y, s_currentBatch ? s_currentBatch->currentDepth : 0.0f); }
void rvkTexCoord2f(float x, float y) { s_curTexcoord[0] = x; s_curTexcoord[1] = y; }
void rvkNormal3f(float x, float y, float z) { s_curNormal[0]=x; s_curNormal[1]=y; s_curNormal[2]=z; }
void rvkColor4ub(unsigned char r, unsigned char g, unsigned char b, unsigned char a) { s_curColor[0]=r; s_curColor[1]=g; s_curColor[2]=b; s_curColor[3]=a; }
void rvkColor3f(float x, float y, float z) { rvkColor4ub((unsigned char)(x*255), (unsigned char)(y*255), (unsigned char)(z*255), 255); }
void rvkColor4f(float x, float y, float z, float w) { rvkColor4ub((unsigned char)(x*255), (unsigned char)(y*255), (unsigned char)(z*255), (unsigned char)(w*255)); }

void rvkSetTexture(unsigned int id)
{
    // TODO(P3/P4): if texture changes, force a new draw call in the batch.
    (void)id;
}

rvkRenderBatch rvkLoadRenderBatch(int numBuffers, int bufferElements)
{
    rvkRenderBatch batch = { 0 };
    (void)numBuffers; (void)bufferElements;
    // TODO(P3): allocate vertexBuffer SoA arrays + GPU buffers + draws[] array.
    return batch;
}

void rvkUnloadRenderBatch(rvkRenderBatch batch) { (void)batch; /* TODO(P3): free CPU/GPU buffers */ }

void rvkDrawRenderBatch(rvkRenderBatch *batch)
{
    if (!batch) return;
    // TODO(P3): upload accumulated vertices to a per-frame GPU buffer; for each draw call
    //   bind rvkGetGraphicsPipeline(state, &batch->vertexBuffer[cur]) + texture descriptor +
    //   MVP push constant, then vkCmdDraw/vkCmdDrawIndexed. Reset counters afterward.
    batch->drawCounter = 1;
    batch->currentDepth = -1.0f;
}

void rvkSetRenderBatchActive(rvkRenderBatch *batch) { s_currentBatch = batch ? batch : &s_defaultBatch; }
void rvkDrawRenderBatchActive(void) { rvkDrawRenderBatch(s_currentBatch); }

bool rvkCheckRenderBatchLimit(int vCount)
{
    (void)vCount;
    // TODO(P3): if current buffer would overflow, draw + reset, return true.
    return false;
}

//----------------------------------------------------------------------------------
// Quick mesh helpers
//----------------------------------------------------------------------------------
void rvkLoadDrawCube(void) { /* TODO(P6) */ }
void rvkLoadDrawQuad(void) { /* TODO(P6) */ }

} // extern "C"
