// Copyright (c) Facebook, Inc. and its affiliates.
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef ESP_BATCHEDSIM_MAGNUM_RENDERER_STANDALONE_H_
#define ESP_BATCHEDSIM_MAGNUM_RENDERER_STANDALONE_H_

#include "MagnumRenderer.h"

namespace esp { namespace batched_sim {

enum class MagnumRendererStandaloneFlag {
  QuietLog = 1 << 0
};
typedef Corrade::Containers::EnumSet<MagnumRendererStandaloneFlag> MagnumRendererStandaloneFlags;
CORRADE_ENUMSET_OPERATORS(MagnumRendererStandaloneFlags)

struct MagnumRendererStandaloneConfiguration {
  explicit MagnumRendererStandaloneConfiguration();
  ~MagnumRendererStandaloneConfiguration();

  MagnumRendererStandaloneConfiguration& setCudaDevice(Magnum::UnsignedInt id);
  MagnumRendererStandaloneConfiguration& setFlags(MagnumRendererStandaloneFlags flags);

  struct State;
  Corrade::Containers::Pointer<State> state;
};

class MagnumRendererStandalone: public MagnumRenderer {
  public:
    explicit MagnumRendererStandalone(const MagnumRendererConfiguration& configuration, const MagnumRendererStandaloneConfiguration& standaloneConfiguration);
    ~MagnumRendererStandalone();

    Magnum::PixelFormat colorFramebufferFormat() const;
    Magnum::PixelFormat depthFramebufferFormat() const;

    void draw();

    Magnum::Image2D colorImage();
    Magnum::Image2D depthImage();

    const void* colorCudaBufferDevicePointer();
    const void* depthCudaBufferDevicePointer();

  private:
    struct State;
    Corrade::Containers::Pointer<State> _state;
};

}}

#endif
