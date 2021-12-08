// Copyright (c) Facebook, Inc. and its affiliates.
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef ESP_BATCHEDSIM_MAGNUM_RENDERER_STANDALONE_H_
#define ESP_BATCHEDSIM_MAGNUM_RENDERER_STANDALONE_H_

#include "MagnumRenderer.h"

namespace esp { namespace batched_sim {

class MagnumRendererStandalone: public MagnumRenderer {
  public:
    explicit MagnumRendererStandalone(const MagnumRendererConfiguration& configuration);
    ~MagnumRendererStandalone();

    Magnum::PixelFormat framebufferColorFormat() const;
    Magnum::PixelFormat framebufferDepthFormat() const;

    void draw();

    const void* cudaColorBufferDevicePointer();
    const void* cudaDepthBufferDevicePointer();

  private:
    struct State;
    Corrade::Containers::Pointer<State> _state;
};

}}

#endif
