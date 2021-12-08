// Copyright (c) Facebook, Inc. and its affiliates.
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef ESP_BATCHEDSIM_MAGNUM_RENDERER_H_
#define ESP_BATCHEDSIM_MAGNUM_RENDERER_H_

#include <cstddef>
#include <Corrade/Containers/Pointer.h>
#include <Magnum/Magnum.h>
#include <Magnum/GL/GL.h>

namespace esp { namespace batched_sim {

enum class MagnumRendererFlag {
  NoTextures = 1 << 0,
  // TODO memory-map
};
typedef Corrade::Containers::EnumSet<MagnumRendererFlag> MagnumRendererFlags;
CORRADE_ENUMSET_OPERATORS(MagnumRendererFlags)

struct MagnumRendererConfiguration {
  explicit MagnumRendererConfiguration();
  ~MagnumRendererConfiguration();

  MagnumRendererConfiguration& setFlags(MagnumRendererFlags flags);
  MagnumRendererConfiguration& setTileSizeCount(const Magnum::Vector2i& tileSize, const Magnum::Vector2i& tileCount);
  // TODO drop
  MagnumRendererConfiguration& setTextureArrayMaxLevelSize(Magnum::UnsignedInt size);

  struct State;
  Corrade::Containers::Pointer<State> state;
};

class MagnumRenderer {
  public:
    explicit MagnumRenderer(const MagnumRendererConfiguration& configuration): MagnumRenderer{Magnum::NoCreate} {
      create(configuration);
    }

    ~MagnumRenderer();

    Magnum::Vector2i tileSize() const;
    Magnum::Vector2i tileCount() const;
    /* Same as tileCount().product() */
    std::size_t sceneCount() const;

    // TODO take an importer instead? that way the consumer can configure it
    void addFile(Corrade::Containers::StringView filename);
    void addFile(Corrade::Containers::StringView filename, Corrade::Containers::StringView importerPlugin);

    // TODO "if there's a scene, name corresponds to a root bject name,
    //  otherwise it's the whole file"
    // TODO or use empty name for the whole scene? but which file??? or use a
    //  scene name for the whole scene??
    // TODO document this may not return consecutive IDs in case the added name
    //  is multiple meshes together
    std::size_t add(Magnum::UnsignedInt sceneId, Corrade::Containers::StringView name);
    std::size_t add(Magnum::UnsignedInt sceneId, Corrade::Containers::StringView name, const Magnum::Matrix4& transformation);
    void clear(Magnum::UnsignedInt sceneId);

    Magnum::Matrix4& camera(Magnum::UnsignedInt sceneId);
    Corrade::Containers::StridedArrayView1D<Magnum::Matrix4> transformations(Magnum::UnsignedInt sceneId);

    void draw(Magnum::GL::AbstractFramebuffer& framebuffer);

  protected:
    /* used by MagnumRendererStandalone */
    explicit MagnumRenderer(Magnum::NoCreateT);
    void create(const MagnumRendererConfiguration& configuration);

  private:
    struct State;
    Corrade::Containers::Pointer<State> _state;
};

}}

#endif
