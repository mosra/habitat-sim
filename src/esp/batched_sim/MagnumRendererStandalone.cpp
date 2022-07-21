// Copyright (c) Facebook, Inc. and its affiliates.
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "MagnumRendererStandalone.h"

#include <Corrade/Containers/Optional.h>
#include <Corrade/Containers/StridedArrayView.h>
#include <Magnum/Image.h>
#include <Magnum/PixelFormat.h>
#include <Magnum/GL/BufferImage.h>
#include <Magnum/GL/Framebuffer.h>
#include <Magnum/GL/Renderbuffer.h>
#include <Magnum/GL/RenderbufferFormat.h>
#include <Magnum/Platform/WindowlessEglApplication.h>

#include <cuda_gl_interop.h>
#include <helper_cuda.h>

#include "esp/batched_sim/configure.h"

namespace Cr = Corrade;
namespace Mn = Magnum;

namespace esp { namespace batched_sim {

struct MagnumRendererStandaloneConfiguration::State {
  /* Not picking any CUDA device by default */
  Magnum::UnsignedInt cudaDevice = ~Magnum::UnsignedInt{};
  MagnumRendererStandaloneFlags flags;
};

MagnumRendererStandaloneConfiguration::MagnumRendererStandaloneConfiguration(): state{Cr::InPlaceInit} {}
MagnumRendererStandaloneConfiguration::~MagnumRendererStandaloneConfiguration() = default;

MagnumRendererStandaloneConfiguration& MagnumRendererStandaloneConfiguration::setCudaDevice(Magnum::UnsignedInt id) {
  state->cudaDevice = id;
  return *this;
}

MagnumRendererStandaloneConfiguration& MagnumRendererStandaloneConfiguration::setFlags(MagnumRendererStandaloneFlags flags) {
  state->flags = flags;
  return *this;
}

struct MagnumRendererStandalone::State {
  Mn::Platform::WindowlessGLContext context;
  Mn::Platform::GLContext magnumContext{Mn::NoCreate};
  Mn::GL::Renderbuffer color{Mn::NoCreate}, depth{Mn::NoCreate};
  Mn::GL::Framebuffer framebuffer{Mn::NoCreate};
  Mn::GL::BufferImage2D colorBuffer{Mn::NoCreate};
  Mn::GL::BufferImage2D depthBuffer{Mn::NoCreate};
  cudaGraphicsResource* cudaColorBuffer{};
  cudaGraphicsResource* cudaDepthBuffer{};

  State(const MagnumRendererStandaloneConfiguration& configuration): context{Mn::Platform::WindowlessGLContext::Configuration{}
    .setCudaDevice(configuration.state->cudaDevice)
    .addFlags(configuration.state->flags & MagnumRendererStandaloneFlag::QuietLog ? Mn::Platform::WindowlessGLContext::Configuration::Flag::QuietLog : Mn::Platform::WindowlessGLContext::Configuration::Flags{})
  } {
      context.makeCurrent();
      magnumContext.create(Mn::GL::Context::Configuration{}
        .addFlags(configuration.state->flags & MagnumRendererStandaloneFlag::QuietLog ? Mn::GL::Context::Configuration::Flag::QuietLog : Mn::GL::Context::Configuration::Flags{})
      );
      color = Mn::GL::Renderbuffer{};
      depth = Mn::GL::Renderbuffer{};
  }

  ~State() {
    /* Should be unmapped before the GL object gets destroyed, I guess? */
    if(cudaColorBuffer)
      checkCudaErrors(cudaGraphicsUnmapResources(1, &cudaColorBuffer, 0));
    if(cudaDepthBuffer)
      checkCudaErrors(cudaGraphicsUnmapResources(1, &cudaDepthBuffer, 0));
  }
};

MagnumRendererStandalone::MagnumRendererStandalone(const MagnumRendererConfiguration& configuration, const MagnumRendererStandaloneConfiguration& standaloneConfiguration): MagnumRenderer{Mn::NoCreate}, _state{Cr::InPlaceInit, standaloneConfiguration} {
  /* Create the renderer only once the GL context is ready */
  create(configuration);

  const Mn::Vector2i size = tileSize()*tileCount();
  _state->color.setStorage(Mn::GL::RenderbufferFormat::RGBA8, size);
  _state->depth.setStorage(Mn::GL::RenderbufferFormat::DepthComponent32F, size);
  _state->framebuffer = Mn::GL::Framebuffer{Mn::Range2Di{{}, size}};
  _state->framebuffer
    .attachRenderbuffer(Mn::GL::Framebuffer::ColorAttachment{0}, _state->color)
    .attachRenderbuffer(Mn::GL::Framebuffer::BufferAttachment::Depth, _state->depth);
  /* Defer the buffer initialization to the point when it's actually read
     into */
  _state->colorBuffer = Mn::GL::BufferImage2D{colorFramebufferFormat()};
  _state->depthBuffer = Mn::GL::BufferImage2D{depthFramebufferFormat()};
}

MagnumRendererStandalone::~MagnumRendererStandalone() {
  /* Yup, shitty, but as we hold the GL context we can't let any GL resources
     to be destructed after our destructor. Better ideas? */
  MagnumRenderer::destroy();
}

Mn::PixelFormat MagnumRendererStandalone::colorFramebufferFormat() const {
  return Mn::PixelFormat::RGBA8Unorm;
}

Mn::PixelFormat MagnumRendererStandalone::depthFramebufferFormat() const {
  return Mn::PixelFormat::Depth32F;
}

void MagnumRendererStandalone::draw() {
  _state->framebuffer.clear(Mn::GL::FramebufferClear::Color|Mn::GL::FramebufferClear::Depth);
  MagnumRenderer::draw(_state->framebuffer);
}

Mn::Image2D MagnumRendererStandalone::colorImage() {
  /* Not using _state->framebuffer.viewport() as it's left pointing to whatever
     tile was rendered last */
  return _state->framebuffer.read({{}, tileCount()*tileSize()}, colorFramebufferFormat());
}

Mn::Image2D MagnumRendererStandalone::depthImage() {
  /* Not using _state->framebuffer.viewport() as it's left pointing to whatever
     tile was rendered last */
  return _state->framebuffer.read({{}, tileCount()*tileSize()}, depthFramebufferFormat());
}

const void* MagnumRendererStandalone::colorCudaBufferDevicePointer() {
  /* If the CUDA buffer exists already, it's mapped from the previous call.
     Unmap it first so we can read into it from GL. */
  if(_state->cudaColorBuffer)
    checkCudaErrors(cudaGraphicsUnmapResources(1, &_state->cudaColorBuffer, 0));

  /* Read to the buffer image, allocating it if it's not already. Can't really
     return a pointer directly to the renderbuffer because the returned device
     pointer is expected to be linearized. */
  _state->framebuffer.read({{}, tileCount()*tileSize()}, _state->colorBuffer, Mn::GL::BufferUsage::DynamicRead);

  /* Initialize the CUDA buffer from the GL buffer image if it's not already */
  if(!_state->cudaColorBuffer) {
    checkCudaErrors(cudaGraphicsGLRegisterBuffer(&_state->cudaColorBuffer, _state->colorBuffer.buffer().id(), cudaGraphicsRegisterFlagsReadOnly));
  }

  /* Map the buffer and return the device pointer */
  checkCudaErrors(cudaGraphicsMapResources(1, &_state->cudaColorBuffer, 0));
  void* pointer;
  std::size_t size;
  checkCudaErrors(cudaGraphicsResourceGetMappedPointer(&pointer, &size, _state->cudaColorBuffer));
  CORRADE_INTERNAL_ASSERT(size == _state->colorBuffer.size().product()*_state->colorBuffer.pixelSize());
  return pointer;
}

const void* MagnumRendererStandalone::depthCudaBufferDevicePointer() {
  /* If the CUDA buffer exists already, it's mapped from the previous call.
     Unmap it first so we can read into it from GL. */
  if(_state->cudaDepthBuffer)
    checkCudaErrors(cudaGraphicsUnmapResources(1, &_state->cudaDepthBuffer, 0));

  /* Read to the buffer image, allocating it if it's not already. Can't really
     return a pointer directly to the renderbuffer because the returned device
     pointer is expected to be linearized. */
  _state->framebuffer.read({{}, tileCount()*tileSize()}, _state->depthBuffer, Mn::GL::BufferUsage::DynamicRead);

  /* Initialize the CUDA buffer from the GL buffer image if it's not already */
  if(!_state->cudaDepthBuffer) {
    checkCudaErrors(cudaGraphicsGLRegisterBuffer(&_state->cudaDepthBuffer, _state->depthBuffer.buffer().id(), cudaGraphicsRegisterFlagsReadOnly));
  }

  /* Map the buffer and return the device pointer */
  checkCudaErrors(cudaGraphicsMapResources(1, &_state->cudaDepthBuffer, 0));
  void* pointer;
  std::size_t size;
  checkCudaErrors(cudaGraphicsResourceGetMappedPointer(&pointer, &size, _state->cudaDepthBuffer));
  CORRADE_INTERNAL_ASSERT(size == _state->depthBuffer.size().product()*_state->depthBuffer.pixelSize());
  return pointer;
}

}}
