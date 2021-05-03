// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/renderer_host/compositor_impl_android.h"

#include <android/bitmap.h>
#include <android/native_window_jni.h>
#include <stdint.h>
#include <utility>

#include "base/android/jni_android.h"
#include "base/android/scoped_java_ref.h"
#include "base/auto_reset.h"
#include "base/bind.h"
#include "base/cancelable_callback.h"
#include "base/command_line.h"
#include "base/containers/hash_tables.h"
#include "base/lazy_instance.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/single_thread_task_runner.h"
#include "base/synchronization/lock.h"
#include "base/thread_task_runner_handle.h"
#include "base/threading/simple_thread.h"
#include "base/threading/thread.h"
#include "base/threading/thread_checker.h"
#include "cc/base/switches.h"
#include "cc/input/input_handler.h"
#include "cc/layers/layer.h"
#include "cc/output/compositor_frame.h"
#include "cc/output/context_provider.h"
#include "cc/output/output_surface.h"
#include "cc/output/output_surface_client.h"
#include "cc/raster/single_thread_task_graph_runner.h"
#include "cc/scheduler/begin_frame_source.h"
#include "cc/surfaces/onscreen_display_client.h"
#include "cc/surfaces/surface_display_output_surface.h"
#include "cc/surfaces/surface_id_allocator.h"
#include "cc/surfaces/surface_manager.h"
#include "cc/trees/layer_tree_host.h"
#include "cc/trees/layer_tree_settings.h"
#include "content/browser/android/child_process_launcher_android.h"
#include "content/browser/compositor/browser_compositor_overlay_candidate_validator_android.h"
#include "content/browser/gpu/browser_gpu_channel_host_factory.h"
#include "content/browser/gpu/browser_gpu_memory_buffer_manager.h"
#include "content/browser/gpu/compositor_util.h"
#include "content/browser/gpu/gpu_surface_tracker.h"
#include "content/browser/renderer_host/render_widget_host_impl.h"
#include "content/common/gpu/client/command_buffer_proxy_impl.h"
#include "content/common/gpu/client/context_provider_command_buffer.h"
#include "content/common/gpu/client/gl_helper.h"
#include "content/common/gpu/client/gpu_channel_host.h"
#include "content/common/gpu/client/webgraphicscontext3d_command_buffer_impl.h"
#include "content/common/gpu/gpu_process_launch_causes.h"
#include "content/common/host_shared_bitmap_manager.h"
#include "content/public/browser/android/compositor.h"
#include "content/public/browser/android/compositor_client.h"
#include "content/public/common/content_switches.h"
#include "gpu/blink/webgraphicscontext3d_in_process_command_buffer_impl.h"
#include "gpu/command_buffer/client/context_support.h"
#include "gpu/command_buffer/client/gles2_interface.h"
#include "third_party/khronos/GLES2/gl2.h"
#include "third_party/khronos/GLES2/gl2ext.h"
#include "third_party/skia/include/core/SkMallocPixelRef.h"
#include "ui/android/window_android.h"
#include "ui/gfx/android/device_display_info.h"
#include "ui/gfx/swap_result.h"

namespace content {

namespace {

const unsigned int kMaxDisplaySwapBuffers = 1U;

// Used to override capabilities_.adjust_deadline_for_parent to false
class OutputSurfaceWithoutParent : public cc::OutputSurface,
                                   public CompositorImpl::VSyncObserver {
 public:
  OutputSurfaceWithoutParent(
      CompositorImpl* compositor,
      const scoped_refptr<ContextProviderCommandBuffer>& context_provider,
      const base::Callback<void(gpu::Capabilities)>&
          populate_gpu_capabilities_callback)
      : cc::OutputSurface(context_provider),
        compositor_(compositor),
        populate_gpu_capabilities_callback_(populate_gpu_capabilities_callback),
        swap_buffers_completion_callback_(
            base::Bind(&OutputSurfaceWithoutParent::OnSwapBuffersCompleted,
                       base::Unretained(this))),
        overlay_candidate_validator_(
            new BrowserCompositorOverlayCandidateValidatorAndroid()) {
    capabilities_.adjust_deadline_for_parent = false;
    capabilities_.max_frames_pending = kMaxDisplaySwapBuffers;
  }

  ~OutputSurfaceWithoutParent() override { compositor_->RemoveObserver(this); }

  void SwapBuffers(cc::CompositorFrame* frame) override {
    GetCommandBufferProxy()->SetLatencyInfo(frame->metadata.latency_info);
    if (frame->gl_frame_data->sub_buffer_rect.IsEmpty()) {
      context_provider_->ContextSupport()->CommitOverlayPlanes();
    } else {
      DCHECK(frame->gl_frame_data->sub_buffer_rect ==
             gfx::Rect(frame->gl_frame_data->size));
      context_provider_->ContextSupport()->Swap();
    }
    client_->DidSwapBuffers();
  }

  bool BindToClient(cc::OutputSurfaceClient* client) override {
    if (!OutputSurface::BindToClient(client))
      return false;

    GetCommandBufferProxy()->SetSwapBuffersCompletionCallback(
        swap_buffers_completion_callback_.callback());

    populate_gpu_capabilities_callback_.Run(
        context_provider_->ContextCapabilities().gpu);
    compositor_->AddObserver(this);

    return true;
  }

  cc::OverlayCandidateValidator* GetOverlayCandidateValidator() const override {
    return overlay_candidate_validator_.get();
  }

 private:
  CommandBufferProxyImpl* GetCommandBufferProxy() {
    ContextProviderCommandBuffer* provider_command_buffer =
        static_cast<content::ContextProviderCommandBuffer*>(
            context_provider_.get());
    CommandBufferProxyImpl* command_buffer_proxy =
        provider_command_buffer->GetCommandBufferProxy();
    DCHECK(command_buffer_proxy);
    return command_buffer_proxy;
  }

  void OnSwapBuffersCompleted(const std::vector<ui::LatencyInfo>& latency_info,
                              gfx::SwapResult result) {
    RenderWidgetHostImpl::CompositorFrameDrawn(latency_info);
    OutputSurface::OnSwapBuffersComplete();
  }

  void OnVSync(base::TimeTicks timebase, base::TimeDelta interval) override {
    CommitVSyncParameters(timebase, interval);
  }

  CompositorImpl* compositor_;
  base::Callback<void(gpu::Capabilities)> populate_gpu_capabilities_callback_;
  base::CancelableCallback<void(const std::vector<ui::LatencyInfo>&,
                                gfx::SwapResult)>
      swap_buffers_completion_callback_;
  scoped_ptr<cc::OverlayCandidateValidator> overlay_candidate_validator_;
};

class ExternalBeginFrameSource : public cc::BeginFrameSourceBase,
                                 public CompositorImpl::VSyncObserver {
 public:
  ExternalBeginFrameSource(CompositorImpl* compositor)
      : compositor_(compositor) {
    compositor_->AddObserver(this);
  }

  ~ExternalBeginFrameSource() override {
    compositor_->RemoveObserver(this);
  }

  // cc::BeginFrameSourceBase implementation:
  void OnNeedsBeginFramesChange(
      bool needs_begin_frames) override {
    compositor_->OnNeedsBeginFramesChange(needs_begin_frames);
  }

  // CompositorImpl::VSyncObserver implementation:
  void OnVSync(base::TimeTicks frame_time,
               base::TimeDelta vsync_period) override {
    CallOnBeginFrame(cc::BeginFrameArgs::Create(
        BEGINFRAME_FROM_HERE, frame_time, base::TimeTicks::Now(), vsync_period,
        cc::BeginFrameArgs::NORMAL));
  }

 private:
  CompositorImpl* compositor_;
};

static bool g_initialized = false;

bool g_use_surface_manager = false;
base::LazyInstance<cc::SurfaceManager> g_surface_manager =
    LAZY_INSTANCE_INITIALIZER;

int g_surface_id_namespace = 0;

class SingleThreadTaskGraphRunner : public cc::SingleThreadTaskGraphRunner {
 public:
  SingleThreadTaskGraphRunner() {
    Start("CompositorTileWorker1",
          base::SimpleThread::Options(base::ThreadPriority::BACKGROUND));
  }

  ~SingleThreadTaskGraphRunner() override {
    Shutdown();
  }
};

base::LazyInstance<SingleThreadTaskGraphRunner> g_task_graph_runner =
    LAZY_INSTANCE_INITIALIZER;

}  // anonymous namespace

// static
Compositor* Compositor::Create(CompositorClient* client,
                               gfx::NativeWindow root_window) {
  return client ? new CompositorImpl(client, root_window) : NULL;
}

// static
void Compositor::Initialize() {
  DCHECK(!CompositorImpl::IsInitialized());
  g_initialized = true;
  g_use_surface_manager = UseSurfacesEnabled();
}

// static
const cc::LayerSettings& Compositor::LayerSettings() {
  return ui::WindowAndroidCompositor::LayerSettings();
}

// static
void Compositor::SetLayerSettings(const cc::LayerSettings& settings) {
  ui::WindowAndroidCompositor::SetLayerSettings(settings);
}

// static
bool CompositorImpl::IsInitialized() {
  return g_initialized;
}

// static
cc::SurfaceManager* CompositorImpl::GetSurfaceManager() {
  if (!g_use_surface_manager)
    return nullptr;
  return g_surface_manager.Pointer();
}

// static
scoped_ptr<cc::SurfaceIdAllocator> CompositorImpl::CreateSurfaceIdAllocator() {
  scoped_ptr<cc::SurfaceIdAllocator> allocator(
      new cc::SurfaceIdAllocator(++g_surface_id_namespace));
  cc::SurfaceManager* manager = GetSurfaceManager();
  DCHECK(manager);
  allocator->RegisterSurfaceIdNamespace(manager);
  return allocator;
}

CompositorImpl::CompositorImpl(CompositorClient* client,
                               gfx::NativeWindow root_window)
    : root_layer_(cc::Layer::Create(Compositor::LayerSettings())),
      resource_manager_(root_window),
      surface_id_allocator_(GetSurfaceManager() ? CreateSurfaceIdAllocator()
                                                : nullptr),
      has_transparent_background_(false),
      device_scale_factor_(1),
      window_(NULL),
      surface_id_(0),
      client_(client),
      root_window_(root_window),
      needs_animate_(false),
      pending_swapbuffers_(0U),
      num_successive_context_creation_failures_(0),
      output_surface_request_pending_(false),
      needs_begin_frames_(false),
      weak_factory_(this) {
  DCHECK(client);
  DCHECK(root_window);
  root_window->AttachCompositor(this);
  CreateLayerTreeHost();
  resource_manager_.Init(host_.get());
}

CompositorImpl::~CompositorImpl() {
  root_window_->DetachCompositor();
  // Clean-up any surface references.
  SetSurface(NULL);
}

ui::UIResourceProvider& CompositorImpl::GetUIResourceProvider() {
  return *this;
}

ui::ResourceManager& CompositorImpl::GetResourceManager() {
  return resource_manager_;
}

void CompositorImpl::SetRootLayer(scoped_refptr<cc::Layer> root_layer) {
  if (subroot_layer_.get()) {
    subroot_layer_->RemoveFromParent();
    subroot_layer_ = NULL;
  }
  if (root_layer.get()) {
    subroot_layer_ = root_layer;
    root_layer_->AddChild(root_layer);
  }
}

void CompositorImpl::SetSurface(jobject surface) {
  JNIEnv* env = base::android::AttachCurrentThread();
  base::android::ScopedJavaLocalRef<jobject> j_surface(env, surface);

  GpuSurfaceTracker* tracker = GpuSurfaceTracker::Get();

  if (window_) {
    // Shut down GL context before unregistering surface.
    SetVisible(false);
    tracker->RemoveSurface(surface_id_);
    ANativeWindow_release(window_);
    window_ = NULL;
    UnregisterViewSurface(surface_id_);
    surface_id_ = 0;
  }

  ANativeWindow* window = NULL;
  if (surface) {
    // Note: This ensures that any local references used by
    // ANativeWindow_fromSurface are released immediately. This is needed as a
    // workaround for https://code.google.com/p/android/issues/detail?id=68174
    base::android::ScopedJavaLocalFrame scoped_local_reference_frame(env);
    window = ANativeWindow_fromSurface(env, surface);
  }

  if (window) {
    window_ = window;
    ANativeWindow_acquire(window);
    surface_id_ = tracker->AddSurfaceForNativeWidget(window);
    tracker->SetSurfaceHandle(
        surface_id_,
        gfx::GLSurfaceHandle(surface_id_, gfx::NATIVE_DIRECT));
    // Register first, SetVisible() might create an OutputSurface.
    RegisterViewSurface(surface_id_, j_surface.obj());
    SetVisible(true);
    ANativeWindow_release(window);
  }
}

void CompositorImpl::CreateLayerTreeHost() {
  DCHECK(!host_);

  cc::LayerTreeSettings settings;
  settings.renderer_settings.refresh_rate = 60.0;
  settings.renderer_settings.allow_antialiasing = false;
  settings.renderer_settings.highp_threshold_min = 2048;
  settings.use_zero_copy = true;
  settings.use_external_begin_frame_source = true;

  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  settings.initial_debug_state.SetRecordRenderingStats(
      command_line->HasSwitch(cc::switches::kEnableGpuBenchmarking));
  if (command_line->HasSwitch(cc::switches::kDisableCompositorPropertyTrees))
    settings.use_property_trees = false;
  settings.single_thread_proxy_scheduler = true;

  settings.use_compositor_animation_timelines = !command_line->HasSwitch(
      switches::kDisableAndroidCompositorAnimationTimelines);

  cc::LayerTreeHost::InitParams params;
  params.client = this;
  params.shared_bitmap_manager = HostSharedBitmapManager::current();
  params.gpu_memory_buffer_manager = BrowserGpuMemoryBufferManager::current();
  params.task_graph_runner = g_task_graph_runner.Pointer();
  params.main_task_runner = base::ThreadTaskRunnerHandle::Get();
  params.settings = &settings;
  params.external_begin_frame_source.reset(new ExternalBeginFrameSource(this));
  host_ = cc::LayerTreeHost::CreateSingleThreaded(this, &params);
  DCHECK(!host_->visible());
  host_->SetRootLayer(root_layer_);
  if (surface_id_allocator_)
    host_->set_surface_id_namespace(surface_id_allocator_->id_namespace());
  host_->SetViewportSize(size_);
  host_->set_has_transparent_background(has_transparent_background_);
  host_->SetDeviceScaleFactor(device_scale_factor_);

  if (needs_animate_)
    host_->SetNeedsAnimate();
}

void CompositorImpl::SetVisible(bool visible) {
  TRACE_EVENT1("cc", "CompositorImpl::SetVisible", "visible", visible);
  if (!visible) {
    DCHECK(host_->visible());
    host_->SetVisible(false);
    if (!host_->output_surface_lost())
      host_->ReleaseOutputSurface();
    pending_swapbuffers_ = 0;
    establish_gpu_channel_timeout_.Stop();
    display_client_.reset();
  } else {
    host_->SetVisible(true);
    if (output_surface_request_pending_)
      RequestNewOutputSurface();
  }
}

void CompositorImpl::setDeviceScaleFactor(float factor) {
  device_scale_factor_ = factor;
  if (host_)
    host_->SetDeviceScaleFactor(factor);
}

void CompositorImpl::SetWindowBounds(const gfx::Size& size) {
  if (size_ == size)
    return;

  size_ = size;
  if (host_)
    host_->SetViewportSize(size);
  if (display_client_)
    display_client_->display()->Resize(size);
  root_layer_->SetBounds(size);
}

void CompositorImpl::SetHasTransparentBackground(bool flag) {
  has_transparent_background_ = flag;
  if (host_)
    host_->set_has_transparent_background(flag);
}

void CompositorImpl::SetNeedsComposite() {
  if (!host_->visible())
    return;
  host_->SetNeedsAnimate();
}

static scoped_ptr<WebGraphicsContext3DCommandBufferImpl>
CreateGpuProcessViewContext(
    const scoped_refptr<GpuChannelHost>& gpu_channel_host,
    const blink::WebGraphicsContext3D::Attributes attributes,
    int surface_id) {
  GURL url("chrome://gpu/Compositor::createContext3D");
  static const size_t kBytesPerPixel = 4;
  gfx::DeviceDisplayInfo display_info;
  size_t full_screen_texture_size_in_bytes =
      display_info.GetDisplayHeight() *
      display_info.GetDisplayWidth() *
      kBytesPerPixel;
  WebGraphicsContext3DCommandBufferImpl::SharedMemoryLimits limits;
  limits.command_buffer_size = 64 * 1024;
  limits.start_transfer_buffer_size = 64 * 1024;
  limits.min_transfer_buffer_size = 64 * 1024;
  limits.max_transfer_buffer_size = std::min(
      3 * full_screen_texture_size_in_bytes, kDefaultMaxTransferBufferSize);
  limits.mapped_memory_reclaim_limit = 2 * 1024 * 1024;
  bool lose_context_when_out_of_memory = true;
  return make_scoped_ptr(
      new WebGraphicsContext3DCommandBufferImpl(surface_id,
                                                url,
                                                gpu_channel_host.get(),
                                                attributes,
                                                lose_context_when_out_of_memory,
                                                limits,
                                                NULL));
}

void CompositorImpl::UpdateLayerTreeHost() {
  client_->UpdateLayerTreeHost();
  if (needs_animate_) {
    needs_animate_ = false;
    root_window_->Animate(base::TimeTicks::Now());
  }
}

void CompositorImpl::OnGpuChannelEstablished() {
  establish_gpu_channel_timeout_.Stop();
  CreateOutputSurface();
}

void CompositorImpl::OnGpuChannelTimeout() {
  LOG(FATAL) << "Timed out waiting for GPU channel.";
}

void CompositorImpl::RequestNewOutputSurface() {
  output_surface_request_pending_ = true;

#if defined(ADDRESS_SANITIZER) || defined(THREAD_SANITIZER) || \
  defined(SYZYASAN) || defined(CYGPROFILE_INSTRUMENTATION)
  const int64_t kGpuChannelTimeoutInSeconds = 40;
#else
  const int64_t kGpuChannelTimeoutInSeconds = 10;
#endif

  BrowserGpuChannelHostFactory* factory =
      BrowserGpuChannelHostFactory::instance();
  if (!factory->GetGpuChannel() || factory->GetGpuChannel()->IsLost()) {
    factory->EstablishGpuChannel(
        CAUSE_FOR_GPU_LAUNCH_WEBGRAPHICSCONTEXT3DCOMMANDBUFFERIMPL_INITIALIZE,
        base::Bind(&CompositorImpl::OnGpuChannelEstablished,
                   weak_factory_.GetWeakPtr()));
    establish_gpu_channel_timeout_.Start(
        FROM_HERE, base::TimeDelta::FromSeconds(kGpuChannelTimeoutInSeconds),
        this, &CompositorImpl::OnGpuChannelTimeout);
    return;
  }

  CreateOutputSurface();
}

void CompositorImpl::DidInitializeOutputSurface() {
  num_successive_context_creation_failures_ = 0;
  output_surface_request_pending_ = false;
}

void CompositorImpl::DidFailToInitializeOutputSurface() {
  LOG(ERROR) << "Failed to init OutputSurface for compositor.";
  LOG_IF(FATAL, ++num_successive_context_creation_failures_ >= 2)
      << "Too many context creation failures. Giving up... ";
  RequestNewOutputSurface();
}

void CompositorImpl::CreateOutputSurface() {
  // We might have had a request from a LayerTreeHost that was then
  // hidden (and hidden means we don't have a native surface).
  // Also make sure we only handle this once.
  if (!output_surface_request_pending_ || !host_->visible())
    return;

  blink::WebGraphicsContext3D::Attributes attrs;
  attrs.shareResources = true;
  attrs.noAutomaticFlushes = true;
  pending_swapbuffers_ = 0;

  DCHECK(window_);
  DCHECK(surface_id_);

  BrowserGpuChannelHostFactory* factory =
      BrowserGpuChannelHostFactory::instance();
  // This channel might be lost (and even if it isn't right now, it might
  // still get marked as lost from the IO thread, at any point in time really).
  // But from here on just try and always lead to either
  // DidInitializeOutputSurface() or DidFailToInitializeOutputSurface().
  scoped_refptr<GpuChannelHost> gpu_channel_host(factory->GetGpuChannel());
  scoped_refptr<ContextProviderCommandBuffer> context_provider(
      ContextProviderCommandBuffer::Create(
          CreateGpuProcessViewContext(gpu_channel_host, attrs, surface_id_),
          BROWSER_COMPOSITOR_ONSCREEN_CONTEXT));
  DCHECK(context_provider.get());

  scoped_ptr<cc::OutputSurface> real_output_surface(
      new OutputSurfaceWithoutParent(
          this, context_provider,
          base::Bind(&CompositorImpl::PopulateGpuCapabilities,
                     base::Unretained(this))));

  cc::SurfaceManager* manager = GetSurfaceManager();
  if (manager) {
    display_client_.reset(
        new cc::OnscreenDisplayClient(std::move(real_output_surface), manager,
                                      HostSharedBitmapManager::current(),
                                      BrowserGpuMemoryBufferManager::current(),
                                      host_->settings().renderer_settings,
                                      base::ThreadTaskRunnerHandle::Get()));
    scoped_ptr<cc::SurfaceDisplayOutputSurface> surface_output_surface(
        new cc::SurfaceDisplayOutputSurface(
            manager, surface_id_allocator_.get(), context_provider, nullptr));

    display_client_->set_surface_output_surface(surface_output_surface.get());
    surface_output_surface->set_display_client(display_client_.get());
    display_client_->display()->Resize(size_);
    host_->SetOutputSurface(std::move(surface_output_surface));
  } else {
    host_->SetOutputSurface(std::move(real_output_surface));
  }
}

void CompositorImpl::PopulateGpuCapabilities(
    gpu::Capabilities gpu_capabilities) {
  gpu_capabilities_ = gpu_capabilities;
}

void CompositorImpl::AddObserver(VSyncObserver* observer) {
  observer_list_.AddObserver(observer);
}

void CompositorImpl::RemoveObserver(VSyncObserver* observer) {
  observer_list_.RemoveObserver(observer);
}

cc::UIResourceId CompositorImpl::CreateUIResource(
    cc::UIResourceClient* client) {
  return host_->CreateUIResource(client);
}

void CompositorImpl::DeleteUIResource(cc::UIResourceId resource_id) {
  host_->DeleteUIResource(resource_id);
}

bool CompositorImpl::SupportsETC1NonPowerOfTwo() const {
  return gpu_capabilities_.texture_format_etc1_npot;
}

void CompositorImpl::DidPostSwapBuffers() {
  TRACE_EVENT0("compositor", "CompositorImpl::DidPostSwapBuffers");
  pending_swapbuffers_++;
}

void CompositorImpl::DidCompleteSwapBuffers() {
  TRACE_EVENT0("compositor", "CompositorImpl::DidCompleteSwapBuffers");
  DCHECK_GT(pending_swapbuffers_, 0U);
  pending_swapbuffers_--;
  client_->OnSwapBuffersCompleted(pending_swapbuffers_);
}

void CompositorImpl::DidAbortSwapBuffers() {
  TRACE_EVENT0("compositor", "CompositorImpl::DidAbortSwapBuffers");
  // This really gets called only once from
  // SingleThreadProxy::DidLoseOutputSurfaceOnImplThread() when the
  // context was lost.
  if (host_->visible())
    host_->SetNeedsCommit();
  client_->OnSwapBuffersCompleted(0);
}

void CompositorImpl::DidCommit() {
  root_window_->OnCompositingDidCommit();
}

void CompositorImpl::AttachLayerForReadback(scoped_refptr<cc::Layer> layer) {
  root_layer_->AddChild(layer);
}

void CompositorImpl::RequestCopyOfOutputOnRootLayer(
    scoped_ptr<cc::CopyOutputRequest> request) {
  root_layer_->RequestCopyOfOutput(std::move(request));
}

void CompositorImpl::OnVSync(base::TimeTicks frame_time,
                             base::TimeDelta vsync_period) {
  FOR_EACH_OBSERVER(VSyncObserver, observer_list_,
                    OnVSync(frame_time, vsync_period));
  if (needs_begin_frames_)
    root_window_->RequestVSyncUpdate();
}

void CompositorImpl::OnNeedsBeginFramesChange(bool needs_begin_frames) {
  if (needs_begin_frames_ == needs_begin_frames)
    return;

  needs_begin_frames_ = needs_begin_frames;
  if (needs_begin_frames_)
    root_window_->RequestVSyncUpdate();
}

void CompositorImpl::SetNeedsAnimate() {
  needs_animate_ = true;
  if (!host_->visible())
    return;

  host_->SetNeedsAnimate();
}

}  // namespace content
