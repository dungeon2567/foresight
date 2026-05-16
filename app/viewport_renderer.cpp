#include "viewport_renderer.hpp"

#include <cmath>
#include <cstring>
#include <cstdint>

namespace {

// ---------------------------------------------------------------------------
// 4x4 column-major matrix helpers.
// ---------------------------------------------------------------------------
struct Mat4 {
    float m[16];
};

Mat4 mat4_identity()
{
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

Mat4 mat4_mul(const Mat4& a, const Mat4& b)
{
    Mat4 r{};
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) {
                s += a.m[k * 4 + row] * b.m[c * 4 + k];
            }
            r.m[c * 4 + row] = s;
        }
    }
    return r;
}

// Right-handed perspective, depth in [0, 1] (matches wgpu clip space).
Mat4 mat4_perspective(float fov_y_rad, float aspect, float znear, float zfar)
{
    const float f = 1.0f / std::tan(fov_y_rad * 0.5f);
    Mat4 r{};
    r.m[0]  = f / aspect;
    r.m[5]  = f;
    r.m[10] = zfar / (znear - zfar);
    r.m[11] = -1.0f;
    r.m[14] = (znear * zfar) / (znear - zfar);
    return r;
}

Mat4 mat4_look_at(float ex, float ey, float ez,
                  float cx, float cy, float cz,
                  float ux, float uy, float uz)
{
    float fx = cx - ex, fy = cy - ey, fz = cz - ez;
    float fl = std::sqrt(fx * fx + fy * fy + fz * fz);
    fx /= fl; fy /= fl; fz /= fl;

    float sx = fy * uz - fz * uy;
    float sy = fz * ux - fx * uz;
    float sz = fx * uy - fy * ux;
    float sl = std::sqrt(sx * sx + sy * sy + sz * sz);
    sx /= sl; sy /= sl; sz /= sl;

    float ux2 = sy * fz - sz * fy;
    float uy2 = sz * fx - sx * fz;
    float uz2 = sx * fy - sy * fx;

    Mat4 r{};
    r.m[0] = sx;  r.m[1] = ux2;  r.m[2]  = -fx; r.m[3]  = 0.0f;
    r.m[4] = sy;  r.m[5] = uy2;  r.m[6]  = -fy; r.m[7]  = 0.0f;
    r.m[8] = sz;  r.m[9] = uz2;  r.m[10] = -fz; r.m[11] = 0.0f;
    r.m[12] = -(sx * ex + sy * ey + sz * ez);
    r.m[13] = -(ux2 * ex + uy2 * ey + uz2 * ez);
    r.m[14] = (fx * ex + fy * ey + fz * ez);
    r.m[15] = 1.0f;
    return r;
}

// ---------------------------------------------------------------------------
// Cube geometry — 36 vertices (12 tris), per-face color.
// ---------------------------------------------------------------------------
struct Vertex {
    float x, y, z;
    float r, g, b;
};

constexpr Vertex kCubeVerts[] = {
    // +X face (red)
    { 0.5f,-0.5f,-0.5f,  0.85f,0.30f,0.30f}, { 0.5f, 0.5f,-0.5f,  0.85f,0.30f,0.30f}, { 0.5f, 0.5f, 0.5f,  0.85f,0.30f,0.30f},
    { 0.5f,-0.5f,-0.5f,  0.85f,0.30f,0.30f}, { 0.5f, 0.5f, 0.5f,  0.85f,0.30f,0.30f}, { 0.5f,-0.5f, 0.5f,  0.85f,0.30f,0.30f},
    // -X face (dim red)
    {-0.5f,-0.5f, 0.5f,  0.55f,0.20f,0.20f}, {-0.5f, 0.5f, 0.5f,  0.55f,0.20f,0.20f}, {-0.5f, 0.5f,-0.5f,  0.55f,0.20f,0.20f},
    {-0.5f,-0.5f, 0.5f,  0.55f,0.20f,0.20f}, {-0.5f, 0.5f,-0.5f,  0.55f,0.20f,0.20f}, {-0.5f,-0.5f,-0.5f,  0.55f,0.20f,0.20f},
    // +Y face (green)
    {-0.5f, 0.5f,-0.5f,  0.40f,0.80f,0.40f}, {-0.5f, 0.5f, 0.5f,  0.40f,0.80f,0.40f}, { 0.5f, 0.5f, 0.5f,  0.40f,0.80f,0.40f},
    {-0.5f, 0.5f,-0.5f,  0.40f,0.80f,0.40f}, { 0.5f, 0.5f, 0.5f,  0.40f,0.80f,0.40f}, { 0.5f, 0.5f,-0.5f,  0.40f,0.80f,0.40f},
    // -Y face (dim green)
    {-0.5f,-0.5f, 0.5f,  0.20f,0.45f,0.20f}, {-0.5f,-0.5f,-0.5f,  0.20f,0.45f,0.20f}, { 0.5f,-0.5f,-0.5f,  0.20f,0.45f,0.20f},
    {-0.5f,-0.5f, 0.5f,  0.20f,0.45f,0.20f}, { 0.5f,-0.5f,-0.5f,  0.20f,0.45f,0.20f}, { 0.5f,-0.5f, 0.5f,  0.20f,0.45f,0.20f},
    // +Z face (blue)
    {-0.5f,-0.5f, 0.5f,  0.30f,0.55f,0.90f}, { 0.5f,-0.5f, 0.5f,  0.30f,0.55f,0.90f}, { 0.5f, 0.5f, 0.5f,  0.30f,0.55f,0.90f},
    {-0.5f,-0.5f, 0.5f,  0.30f,0.55f,0.90f}, { 0.5f, 0.5f, 0.5f,  0.30f,0.55f,0.90f}, {-0.5f, 0.5f, 0.5f,  0.30f,0.55f,0.90f},
    // -Z face (dim blue)
    { 0.5f,-0.5f,-0.5f,  0.18f,0.32f,0.55f}, {-0.5f,-0.5f,-0.5f,  0.18f,0.32f,0.55f}, {-0.5f, 0.5f,-0.5f,  0.18f,0.32f,0.55f},
    { 0.5f,-0.5f,-0.5f,  0.18f,0.32f,0.55f}, {-0.5f, 0.5f,-0.5f,  0.18f,0.32f,0.55f}, { 0.5f, 0.5f,-0.5f,  0.18f,0.32f,0.55f},
};
constexpr uint32_t kCubeVertCount = sizeof(kCubeVerts) / sizeof(kCubeVerts[0]);

constexpr WGPUTextureFormat kColorFormat = WGPUTextureFormat_RGBA8Unorm;
constexpr WGPUTextureFormat kDepthFormat = WGPUTextureFormat_Depth24Plus;

const char kShaderWGSL[] = R"WGSL(
struct Uniforms {
  mvp : mat4x4<f32>,
};
@group(0) @binding(0) var<uniform> u : Uniforms;

struct VsIn {
  @location(0) pos : vec3<f32>,
  @location(1) col : vec3<f32>,
};
struct VsOut {
  @builtin(position) clip : vec4<f32>,
  @location(0) col : vec3<f32>,
};

@vertex
fn vs_main(in: VsIn) -> VsOut {
  var o : VsOut;
  o.clip = u.mvp * vec4<f32>(in.pos, 1.0);
  o.col = in.col;
  return o;
}

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
  return vec4<f32>(in.col, 1.0);
}
)WGSL";

} // namespace

// ---------------------------------------------------------------------------
// Renderer state.
// ---------------------------------------------------------------------------
struct ViewportRenderer {
    WGPUDevice           device   = nullptr;
    WGPUQueue            queue    = nullptr;

    WGPUShaderModule     shader   = nullptr;
    WGPUBindGroupLayout  bgl      = nullptr;
    WGPURenderPipeline   pipeline = nullptr;

    WGPUBuffer           vbuf     = nullptr;
    WGPUBuffer           ubuf     = nullptr;
    WGPUBindGroup        bg       = nullptr;

    // Render-target resources, recreated on size change.
    uint32_t             rt_w     = 0;
    uint32_t             rt_h     = 0;
    WGPUTexture          color_tex   = nullptr;
    WGPUTextureView      color_view  = nullptr;
    WGPUTexture          depth_tex   = nullptr;
    WGPUTextureView      depth_view  = nullptr;
};

namespace {

void destroy_render_targets(ViewportRenderer& r)
{
    if (r.color_view) { wgpuTextureViewRelease(r.color_view); r.color_view = nullptr; }
    if (r.color_tex)  { wgpuTextureRelease(r.color_tex); r.color_tex = nullptr; }
    if (r.depth_view) { wgpuTextureViewRelease(r.depth_view); r.depth_view = nullptr; }
    if (r.depth_tex)  { wgpuTextureRelease(r.depth_tex); r.depth_tex = nullptr; }
}

bool ensure_render_targets(ViewportRenderer& r, uint32_t w, uint32_t h)
{
    if (w == r.rt_w && h == r.rt_h && r.color_view && r.depth_view) {
        return true;
    }
    destroy_render_targets(r);

    WGPUTextureDescriptor cd{};
    cd.label         = "viewport color";
    cd.usage         = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
    cd.dimension     = WGPUTextureDimension_2D;
    cd.size          = { w, h, 1 };
    cd.format        = kColorFormat;
    cd.mipLevelCount = 1;
    cd.sampleCount   = 1;
    r.color_tex = wgpuDeviceCreateTexture(r.device, &cd);
    if (!r.color_tex) return false;

    WGPUTextureViewDescriptor cvd{};
    cvd.format          = kColorFormat;
    cvd.dimension       = WGPUTextureViewDimension_2D;
    cvd.mipLevelCount   = 1;
    cvd.arrayLayerCount = 1;
    cvd.aspect          = WGPUTextureAspect_All;
    r.color_view = wgpuTextureCreateView(r.color_tex, &cvd);

    WGPUTextureDescriptor dd{};
    dd.label         = "viewport depth";
    dd.usage         = WGPUTextureUsage_RenderAttachment;
    dd.dimension     = WGPUTextureDimension_2D;
    dd.size          = { w, h, 1 };
    dd.format        = kDepthFormat;
    dd.mipLevelCount = 1;
    dd.sampleCount   = 1;
    r.depth_tex = wgpuDeviceCreateTexture(r.device, &dd);

    WGPUTextureViewDescriptor dvd{};
    dvd.format          = kDepthFormat;
    dvd.dimension       = WGPUTextureViewDimension_2D;
    dvd.mipLevelCount   = 1;
    dvd.arrayLayerCount = 1;
    dvd.aspect          = WGPUTextureAspect_DepthOnly;
    r.depth_view = wgpuTextureCreateView(r.depth_tex, &dvd);

    r.rt_w = w;
    r.rt_h = h;
    return true;
}

bool create_pipeline(ViewportRenderer& r)
{
    WGPUShaderModuleWGSLDescriptor wgsl{};
    wgsl.chain.sType = WGPUSType_ShaderModuleWGSLDescriptor;
    wgsl.code = kShaderWGSL;
    WGPUShaderModuleDescriptor sd{};
    sd.nextInChain = &wgsl.chain;
    sd.label       = "viewport shader";
    r.shader = wgpuDeviceCreateShaderModule(r.device, &sd);
    if (!r.shader) return false;

    WGPUBindGroupLayoutEntry bgle{};
    bgle.binding              = 0;
    bgle.visibility           = WGPUShaderStage_Vertex;
    bgle.buffer.type          = WGPUBufferBindingType_Uniform;
    bgle.buffer.minBindingSize = sizeof(float) * 16;
    WGPUBindGroupLayoutDescriptor bgld{};
    bgld.entryCount = 1;
    bgld.entries    = &bgle;
    r.bgl = wgpuDeviceCreateBindGroupLayout(r.device, &bgld);

    WGPUPipelineLayoutDescriptor pld{};
    pld.bindGroupLayoutCount = 1;
    pld.bindGroupLayouts     = &r.bgl;
    WGPUPipelineLayout layout = wgpuDeviceCreatePipelineLayout(r.device, &pld);

    WGPUVertexAttribute attrs[2]{};
    attrs[0].format         = WGPUVertexFormat_Float32x3;
    attrs[0].offset         = 0;
    attrs[0].shaderLocation = 0;
    attrs[1].format         = WGPUVertexFormat_Float32x3;
    attrs[1].offset         = sizeof(float) * 3;
    attrs[1].shaderLocation = 1;

    WGPUVertexBufferLayout vbl{};
    vbl.arrayStride    = sizeof(Vertex);
    vbl.stepMode       = WGPUVertexStepMode_Vertex;
    vbl.attributeCount = 2;
    vbl.attributes     = attrs;

    WGPUColorTargetState cts{};
    cts.format    = kColorFormat;
    cts.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState fs{};
    fs.module      = r.shader;
    fs.entryPoint  = "fs_main";
    fs.targetCount = 1;
    fs.targets     = &cts;

    WGPUDepthStencilState dss{};
    dss.format            = kDepthFormat;
    dss.depthWriteEnabled = true;
    dss.depthCompare      = WGPUCompareFunction_Less;
    dss.stencilFront.compare = WGPUCompareFunction_Always;
    dss.stencilBack.compare  = WGPUCompareFunction_Always;

    WGPURenderPipelineDescriptor rpd{};
    rpd.label  = "viewport pipeline";
    rpd.layout = layout;
    rpd.vertex.module      = r.shader;
    rpd.vertex.entryPoint  = "vs_main";
    rpd.vertex.bufferCount = 1;
    rpd.vertex.buffers     = &vbl;
    rpd.primitive.topology  = WGPUPrimitiveTopology_TriangleList;
    rpd.primitive.cullMode  = WGPUCullMode_Back;
    rpd.primitive.frontFace = WGPUFrontFace_CCW;
    rpd.depthStencil        = &dss;
    rpd.multisample.count   = 1;
    rpd.multisample.mask    = 0xFFFFFFFF;
    rpd.fragment            = &fs;

    r.pipeline = wgpuDeviceCreateRenderPipeline(r.device, &rpd);
    wgpuPipelineLayoutRelease(layout);
    return r.pipeline != nullptr;
}

bool create_buffers(ViewportRenderer& r)
{
    WGPUBufferDescriptor vbd{};
    vbd.label = "viewport cube vbuf";
    vbd.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
    vbd.size  = sizeof(kCubeVerts);
    r.vbuf = wgpuDeviceCreateBuffer(r.device, &vbd);
    if (!r.vbuf) return false;
    wgpuQueueWriteBuffer(r.queue, r.vbuf, 0, kCubeVerts, sizeof(kCubeVerts));

    WGPUBufferDescriptor ubd{};
    ubd.label = "viewport mvp ubuf";
    ubd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    ubd.size  = sizeof(float) * 16;
    r.ubuf = wgpuDeviceCreateBuffer(r.device, &ubd);
    if (!r.ubuf) return false;

    WGPUBindGroupEntry bge{};
    bge.binding = 0;
    bge.buffer  = r.ubuf;
    bge.offset  = 0;
    bge.size    = sizeof(float) * 16;
    WGPUBindGroupDescriptor bgd{};
    bgd.layout     = r.bgl;
    bgd.entryCount = 1;
    bgd.entries    = &bge;
    r.bg = wgpuDeviceCreateBindGroup(r.device, &bgd);
    return r.bg != nullptr;
}

} // namespace

ViewportRenderer* viewport_renderer_create(WGPUDevice device, WGPUQueue queue)
{
    auto* r = new ViewportRenderer();
    r->device = device;
    r->queue  = queue;
    if (!create_pipeline(*r) || !create_buffers(*r)) {
        viewport_renderer_destroy(r);
        return nullptr;
    }
    return r;
}

void viewport_renderer_destroy(ViewportRenderer* r)
{
    if (!r) return;
    destroy_render_targets(*r);
    if (r->bg)       { wgpuBindGroupRelease(r->bg);       r->bg = nullptr; }
    if (r->ubuf)     { wgpuBufferDestroy(r->ubuf); wgpuBufferRelease(r->ubuf); r->ubuf = nullptr; }
    if (r->vbuf)     { wgpuBufferDestroy(r->vbuf); wgpuBufferRelease(r->vbuf); r->vbuf = nullptr; }
    if (r->pipeline) { wgpuRenderPipelineRelease(r->pipeline); r->pipeline = nullptr; }
    if (r->bgl)      { wgpuBindGroupLayoutRelease(r->bgl);     r->bgl      = nullptr; }
    if (r->shader)   { wgpuShaderModuleRelease(r->shader);     r->shader   = nullptr; }
    delete r;
}

ImTextureID viewport_renderer_render(ViewportRenderer* r,
                                     uint32_t pixel_w, uint32_t pixel_h,
                                     float yaw_deg, float pitch_deg,
                                     float zoom,
                                     float pan_x, float pan_y)
{
    if (!r || pixel_w == 0 || pixel_h == 0) return 0;
    if (!ensure_render_targets(*r, pixel_w, pixel_h)) return 0;

    // Orbit camera: yaw rotates around world-Y, pitch tilts. Distance =
    // base / zoom. Pan offsets the look-at target in screen-aligned X / Y so
    // it tracks middle-drag intuitively.
    constexpr float deg2rad = 0.0174533f;
    const float yaw   = yaw_deg   * deg2rad;
    const float pitch = std::fmax(-1.4f, std::fmin(1.4f, pitch_deg * deg2rad));
    const float dist  = 6.0f / std::fmax(0.05f, zoom);

    // Spherical → cartesian eye position around (target).
    const float ex = std::cos(pitch) * std::sin(yaw) * dist;
    const float ey = std::sin(pitch) * dist;
    const float ez = std::cos(pitch) * std::cos(yaw) * dist;

    // Pan deltas are in pixels; scale by distance so motion feels constant.
    const float pan_scale = dist * 0.0025f;
    const float tx = -pan_x * pan_scale;
    const float ty =  pan_y * pan_scale;

    Mat4 view = mat4_look_at(ex + tx, ey + ty, ez,
                             tx,      ty,      0.0f,
                             0.0f,    1.0f,    0.0f);
    Mat4 proj = mat4_perspective(60.0f * deg2rad,
                                 (float)pixel_w / (float)pixel_h,
                                 0.1f, 100.0f);
    Mat4 mvp  = mat4_mul(proj, view);
    wgpuQueueWriteBuffer(r->queue, r->ubuf, 0, mvp.m, sizeof(mvp.m));

    WGPUCommandEncoderDescriptor ed{};
    ed.label = "viewport encoder";
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(r->device, &ed);

    WGPURenderPassColorAttachment ca{};
    ca.view       = r->color_view;
    ca.loadOp     = WGPULoadOp_Clear;
    ca.storeOp    = WGPUStoreOp_Store;
    ca.clearValue = WGPUColor{ 0.10, 0.13, 0.15, 1.0 };
    ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;

    WGPURenderPassDepthStencilAttachment da{};
    da.view              = r->depth_view;
    da.depthLoadOp       = WGPULoadOp_Clear;
    da.depthStoreOp      = WGPUStoreOp_Store;
    da.depthClearValue   = 1.0f;
    da.depthReadOnly     = false;
    da.stencilLoadOp     = WGPULoadOp_Undefined;
    da.stencilStoreOp    = WGPUStoreOp_Undefined;
    da.stencilReadOnly   = true;

    WGPURenderPassDescriptor pd{};
    pd.label                  = "viewport pass";
    pd.colorAttachmentCount   = 1;
    pd.colorAttachments       = &ca;
    pd.depthStencilAttachment = &da;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &pd);

    wgpuRenderPassEncoderSetPipeline(pass, r->pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, r->bg, 0, nullptr);
    wgpuRenderPassEncoderSetVertexBuffer(pass, 0, r->vbuf, 0, sizeof(kCubeVerts));
    wgpuRenderPassEncoderDraw(pass, kCubeVertCount, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);

    WGPUCommandBufferDescriptor cbd{};
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, &cbd);
    wgpuQueueSubmit(r->queue, 1, &cb);
    wgpuCommandBufferRelease(cb);
    wgpuCommandEncoderRelease(enc);

    return (ImTextureID)(intptr_t)r->color_view;
}
