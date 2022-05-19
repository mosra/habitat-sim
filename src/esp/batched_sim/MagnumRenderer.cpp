// Copyright (c) Facebook, Inc. and its affiliates.
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "MagnumRenderer.h"

#include <unordered_map>
#include <Corrade/Containers/Array.h>
#include <Corrade/Containers/GrowableArray.h>
#include <Corrade/Containers/Optional.h>
#include <Corrade/Containers/Pair.h>
#include <Corrade/Containers/StringStl.h>
#include <Corrade/PluginManager/Manager.h>
#include <Corrade/PluginManager/PluginMetadata.h>
#include <Corrade/Utility/Algorithms.h>
#include <Corrade/Utility/ConfigurationGroup.h>
#include <Corrade/Utility/Directory.h>
#include <Corrade/Utility/MurmurHash2.h>
#include <Magnum/ImageView.h>
#include <Magnum/GL/AbstractFramebuffer.h>
#include <Magnum/GL/Buffer.h>
#include <Magnum/GL/Mesh.h>
#include <Magnum/GL/Renderer.h>
#include <Magnum/GL/TextureArray.h>
#include <Magnum/GL/TextureFormat.h>
#include <Magnum/Math/PackingBatch.h>
#include <Magnum/MeshTools/Compile.h>
#include <Magnum/Shaders/Phong.h>
#include <Magnum/Shaders/PhongGL.h>
#include <Magnum/Shaders/Generic.h>
#include <Magnum/Trade/AbstractImporter.h>
#include <Magnum/Trade/FlatMaterialData.h>
#include <Magnum/Trade/ImageData.h>
#include <Magnum/Trade/MeshData.h>
#include <Magnum/Trade/SceneData.h>
#include <Magnum/Trade/TextureData.h>

#include "esp/batched_sim/configure.h"

#ifdef MAGNUM_BUILD_STATIC
void importStaticPlugins() {
  CORRADE_PLUGIN_IMPORT(BpsImporter)
}
#endif

namespace Cr = Corrade;
namespace Mn = Magnum;

/* std::hash specialization to be able to use Corrade::String in unordered_map  */
// TODO put directly into Corrade, it's duplicated ON SEVEN PLACES ALREADY!!!
namespace std {
    template<> struct hash<Cr::Containers::String> {
        std::size_t operator()(const Cr::Containers::String& key) const {
            const Cr::Utility::MurmurHash2 hash;
            const Cr::Utility::HashDigest<sizeof(std::size_t)> digest = hash(key.data(), key.size());
            return *reinterpret_cast<const std::size_t*>(digest.byteArray());
        }
    };
}

namespace esp { namespace batched_sim {

using namespace Cr::Containers::Literals;

struct MagnumRendererConfiguration::State {
  MagnumRendererFlags flags;
  Magnum::Vector2i tileSize{128, 128};
  Magnum::Vector2i tileCount{16, 12};
  Magnum::UnsignedInt textureArrayMaxLevelSize{128};
};

MagnumRendererConfiguration::MagnumRendererConfiguration(): state{Cr::InPlaceInit} {}
MagnumRendererConfiguration::~MagnumRendererConfiguration() = default;

MagnumRendererConfiguration& MagnumRendererConfiguration::setFlags(MagnumRendererFlags flags) {
  state->flags = flags;
  return *this;
}
MagnumRendererConfiguration& MagnumRendererConfiguration::setTileSizeCount(const Magnum::Vector2i& tileSize, const Magnum::Vector2i& tileCount) {
  state->tileSize = tileSize;
  state->tileCount = tileCount;
  return *this;
}
MagnumRendererConfiguration& MagnumRendererConfiguration::setTextureArrayMaxLevelSize(Magnum::UnsignedInt size) {
  state->textureArrayMaxLevelSize = size;
  return *this;
}

namespace {

struct MeshView {
  // TODO also mesh ID, once we have multiple meshes
  Mn::UnsignedInt indexOffsetInBytes;
  Mn::UnsignedInt indexCount;
  Mn::Int materialId; /* is never -1 tho */
  // TODO also parent, when we are able to fetch the whole hierarchy for a
  //  particular root object name
  Mn::Matrix4 transformation;
};

struct DrawCommand {
  std::size_t indexOffsetInBytes;
  Mn::UnsignedInt indexCount;
};

struct Scene {
  /* Appended to with add() */
  Cr::Containers::Array<Mn::Int> parents; /* parents[i] < i, always */
  Cr::Containers::Array<Mn::Matrix4> transformations;
  // TODO have this temporary and just once for all scenes, doesn't need to
  //  be stored
  Cr::Containers::Array<Mn::Shaders::TransformationUniform3D> absoluteTransformations;
  Cr::Containers::Array<Mn::Shaders::PhongDrawUniform> draws;
  Cr::Containers::Array<Mn::Shaders::TextureTransformationUniform> textureTransformations;
  // TODO make the layout match GL to avoid a copy in draw()
  Cr::Containers::Array<DrawCommand> drawCommands;

  /* Updated every frame */
  Mn::GL::Buffer transformationUniform;
  Mn::GL::Buffer drawUniform;
  Mn::GL::Buffer textureTransformationUniform;
};

struct TextureTransformation {
  Mn::UnsignedInt layer;
  Mn::Matrix3 transformation;
};

}

struct MagnumRenderer::State {
  MagnumRendererFlags flags;
  Mn::Vector2i tileSize, tileCount;
  Mn::Shaders::PhongGL shader{Mn::NoCreate};

  /* Filled at the beginning */
  Mn::GL::Texture2DArray texture{Mn::NoCreate};
  Mn::GL::Mesh mesh{Mn::NoCreate};
  Mn::GL::Buffer materialUniform;
  /* Contains texture layer for each material. Used by add() to populate the
     draw list. */
  // TODO have the materials deduplicated, and then this should have a layer
  //  for each mesh view instead
  // TODO also the scale + offset, once we have non-shitty atlasing
  Cr::Containers::Array<TextureTransformation> textureTransformations;

  /* Pairs of mesh views (index byte offset and count), material IDs and
     initial transformations for draws. Used by add() to populate the draw
     list. */
  Cr::Containers::Array<MeshView> meshViews;
  /* Range of mesh views and materials corresponding to a particular name */
  std::unordered_map<Cr::Containers::String, Cr::Containers::Pair<Mn::UnsignedInt, Mn::UnsignedInt>> meshViewRangeForName;

  /* Updated from camera() */
  Cr::Containers::Array<Mn::Shaders::ProjectionUniform3D> projections;
  /* Updated every frame */
  Mn::GL::Buffer projectionUniform;

  Cr::Containers::Array<Scene> scenes;
};

MagnumRenderer::MagnumRenderer(Magnum::NoCreateT) {};

void MagnumRenderer::create(const MagnumRendererConfiguration& configurationWrapper) {
  #ifdef MAGNUM_BUILD_STATIC
  importStaticPlugins();
  #endif

  const MagnumRendererConfiguration::State& configuration = *configurationWrapper.state;

  CORRADE_INTERNAL_ASSERT(!_state);
  _state.emplace();
  _state->flags = configuration.flags;
  _state->tileSize = configuration.tileSize;
  _state->tileCount = configuration.tileCount;
  const std::size_t sceneCount = configuration.tileCount.product();
  _state->projections = Cr::Containers::Array<Mn::Shaders::ProjectionUniform3D>{sceneCount};
  _state->scenes = Cr::Containers::Array<Scene>{sceneCount};
  /* Have one extra transformation slot in each scene for easier transform
     calculation in draw() */
  for(Scene& scene: _state->scenes)
    arrayAppend(scene.absoluteTransformations, Cr::InPlaceInit);

  // TODO move this outside
  Mn::GL::Renderer::enable(Mn::GL::Renderer::Feature::FaceCulling);
  Mn::GL::Renderer::enable(Mn::GL::Renderer::Feature::DepthTest);
}

MagnumRenderer::~MagnumRenderer() = default;

Mn::Vector2i MagnumRenderer::tileCount() const {
  return _state->tileCount;
}

Mn::Vector2i MagnumRenderer::tileSize() const {
  return _state->tileSize;
}

std::size_t MagnumRenderer::sceneCount() const {
  return _state->scenes.size();
}

void MagnumRenderer::addFile(const Cr::Containers::StringView filename) {
  return addFile(filename, filename.hasSuffix(".bps"_s) ?
      #ifdef MAGNUM_BUILD_STATIC
      "BpsImporter"_s
      #else
      BPSIMPORTER_PLUGIN_FILENAME
      #endif
    : "AnySceneImporter");
}

void MagnumRenderer::addFile(const Cr::Containers::StringView filename, const Cr::Containers::StringView importerPlugin) {
  CORRADE_ASSERT(!_state->texture.id(),
    "MagnumRenderer::addFile(): sorry, only one file is supported at the moment", );

  Cr::PluginManager::Manager<Mn::Trade::AbstractImporter> manager;
  Cr::Containers::Pointer<Mn::Trade::AbstractImporter> importer = manager.loadAndInstantiate(importerPlugin);
  CORRADE_INTERNAL_ASSERT(importer);

  if(importerPlugin.contains("BpsImporter")) {
    // TODO why dafuq is this not propagated from BasisImporter config??
    importer->configuration().setValue("basisFormat", "Astc4x4RGBA");
    importer->configuration().setValue("meshViews", true);
    importer->configuration().setValue("instanceScene", false);
    importer->configuration().setValue("textureArrays", true);
    importer->configuration().setValue("textureArraysForceAllMaterialsTextured", true);
  } else if(importerPlugin.contains("GltfImporter") ||
            importerPlugin.contains("AnySceneImporter")) {
    importer->configuration().setValue("ignoreRequiredExtensions", true);
    importer->configuration().setValue("experimentalKhrTextureKtx", true);
  }

  if(Cr::PluginManager::PluginMetadata* const metadata = manager.metadata("BasisImporter")) {
    metadata->configuration().setValue("format", "Astc4x4RGBA"); // TODO
  }

  // TODO memory-map self-contained files (have a config option? do implicitly
  //  for glb, bps and ply?)
  CORRADE_INTERNAL_ASSERT_OUTPUT(importer->openFile(filename));

  /* One texture for the whole scene */
  if(!(_state->flags & MagnumRendererFlag::NoTextures)) {
      CORRADE_ASSERT(importer->textureCount() == 1,
        "MagnumRenderer::addFile(): expected a file with exactly one texture, got" << importer->textureCount(), );
      const Cr::Containers::Optional<Mn::Trade::TextureData> texture = importer->texture(0);
      CORRADE_INTERNAL_ASSERT(texture && texture->type() == Mn::Trade::TextureType::Texture2DArray);

      const Mn::UnsignedInt levelCount = importer->image3DLevelCount(texture->image());
      Cr::Containers::Optional<Mn::Trade::ImageData3D> image = importer->image3D(texture->image());
      CORRADE_INTERNAL_ASSERT(image);
      _state->texture = Mn::GL::Texture2DArray{};
      _state->texture
          .setMinificationFilter(texture->minificationFilter(), texture->mipmapFilter())
          .setMagnificationFilter(texture->magnificationFilter())
          .setWrapping(texture->wrapping().xy());
      if(image->isCompressed()) {
        _state->texture
            .setStorage(levelCount, Mn::GL::textureFormat(image->compressedFormat()), image->size())
            .setCompressedSubImage(0, {}, *image);
        for(Mn::UnsignedInt level = 1; level != levelCount; ++level) {
            Cr::Containers::Optional<Mn::Trade::ImageData3D> levelImage = importer->image3D(texture->image(), level);
            CORRADE_INTERNAL_ASSERT(levelImage && levelImage->isCompressed() && levelImage->compressedFormat() == image->compressedFormat());
            _state->texture.setCompressedSubImage(level, {}, *levelImage);
        }
      } else {
        _state->texture
            .setStorage(levelCount, Mn::GL::textureFormat(image->format()), image->size())
            .setSubImage(0, {}, *image);
      }
  }

  /* One mesh for the whole scene */
  CORRADE_ASSERT(importer->meshCount() == 1,
    "MagnumRenderer::addFile(): expected a file with exactly one mesh, got" << importer->meshCount(), );
  _state->mesh = Mn::MeshTools::compile(*CORRADE_INTERNAL_ASSERT_EXPRESSION(importer->mesh(0)));

  /* Immutable material data. Save layers to a temporary array to apply
     them to draws instead */
  // TODO have a step that deduplicates materials and puts the layer to the
  //  scene instead
  _state->textureTransformations = Cr::Containers::Array<TextureTransformation>{Cr::DefaultInit, importer->materialCount()};
  {
    Cr::Containers::Array<Mn::Shaders::PhongMaterialUniform> materialData{Cr::DefaultInit, importer->materialCount()};
    for(std::size_t i = 0; i != materialData.size(); ++i) {
      const Cr::Containers::Optional<Mn::Trade::MaterialData> material = importer->material(i);
      CORRADE_INTERNAL_ASSERT(material);
      const auto& flatMaterial = material->as<Mn::Trade::FlatMaterialData>();
      materialData[i].setAmbientColor(flatMaterial.color());

      CORRADE_ASSERT(flatMaterial.hasTexture(),
        "MagnumRenderer::addFile(): material" << i << "is not textured", );
      CORRADE_ASSERT(flatMaterial.texture() == 0,
        "MagnumRenderer::addFile(): expected material" << i << "to reference the only texture, got" << flatMaterial.texture(), );

      _state->textureTransformations[i] = {
        // TODO builtin attribute for this
        flatMaterial.attribute<Mn::UnsignedInt>("baseColorTextureLayer"_s),
        // TODO fix gltf converter to flip texcoords (ffs!!)
        flatMaterial.hasTextureTransformation() ? Mn::Matrix3::translation(Mn::Vector2::yAxis(1.0f))*Mn::Matrix3::scaling(Mn::Vector2::yScale(-1.0f))*flatMaterial.textureMatrix()*Mn::Matrix3::translation(Mn::Vector2::yAxis(1.0f))*Mn::Matrix3::scaling(Mn::Vector2::yScale(-1.0f)) : Mn::Matrix3{}
      };
    }

    // TODO immutable buffer storage
    _state->materialUniform.setData(materialData);
  }

  /* Fill initial projection data for each view. Will be uploaded afresh every
     draw. */
  _state->projections = Cr::Containers::Array<Mn::Shaders::ProjectionUniform3D>{Cr::DefaultInit, std::size_t(_state->tileCount.product())};
  // TODO (mutable) buffer storage

  {
    CORRADE_ASSERT(importer->sceneCount() == 1,
      "MagnumRenderer::addFile(): expected exactly one scene, got" << importer->sceneCount(), );
    Cr::Containers::Optional<Mn::Trade::SceneData> scene = importer->scene(0);
    CORRADE_INTERNAL_ASSERT(scene);

    /* Populate the mesh and material list */
    const Cr::Containers::Optional<Mn::UnsignedInt> meshViewIndexOffsetFieldId = scene->findFieldId(importer->sceneFieldForName("meshViewIndexOffset"));
    CORRADE_ASSERT(meshViewIndexOffsetFieldId,
      "MagnumRenderer::addFile(): no meshViewIndexOffset field in the scene", );
    const Cr::Containers::Optional<Mn::UnsignedInt> meshViewIndexCountFieldId = scene->findFieldId(importer->sceneFieldForName("meshViewIndexCount"));
    CORRADE_ASSERT(meshViewIndexCountFieldId,
      "MagnumRenderer::addFile(): no meshViewIndexCount field in the scene", );
    const Cr::Containers::Optional<Mn::UnsignedInt> meshViewMaterialFieldId = scene->findFieldId(importer->sceneFieldForName("meshViewMaterial"));
    CORRADE_ASSERT(meshViewMaterialFieldId,
      "MagnumRenderer::addFile(): no meshViewMaterial field in the scene", );
    /* SceneData and copy() will assert if the types or sizes don't match, so
       we don't have to */
    _state->meshViews = Cr::Containers::Array<MeshView>{Cr::NoInit, scene->fieldSize(*meshViewIndexCountFieldId)};
    if(importerPlugin.contains("BpsImporter")) {
      Cr::Utility::copy(scene->field<Mn::UnsignedInt>(*meshViewIndexOffsetFieldId),
        stridedArrayView(_state->meshViews).slice(&MeshView::indexOffsetInBytes));
      Cr::Utility::copy(scene->field<Mn::UnsignedInt>(*meshViewIndexCountFieldId),
        stridedArrayView(_state->meshViews).slice(&MeshView::indexCount));
      Cr::Utility::copy(scene->field<Mn::Int>(*meshViewMaterialFieldId),
        stridedArrayView(_state->meshViews).slice(&MeshView::materialId));
    } else {
      // TODO implement parsing ints in the importer
      Mn::Math::castInto(
        // TODO make stridedArrayView implicitly convertible to higher
        //  dimensions, like images
        Cr::Containers::arrayCast<2, const Mn::Double>(scene->field<Mn::Double>(*meshViewIndexOffsetFieldId)),
        Cr::Containers::arrayCast<2, Mn::UnsignedInt>(stridedArrayView(_state->meshViews).slice(&MeshView::indexOffsetInBytes)));
      Mn::Math::castInto(
        // TODO make stridedArrayView implicitly convertible to higher
        //  dimensions, like images
        Cr::Containers::arrayCast<2, const Mn::Double>(scene->field<Mn::Double>(*meshViewIndexCountFieldId)),
        Cr::Containers::arrayCast<2, Mn::UnsignedInt>(stridedArrayView(_state->meshViews).slice(&MeshView::indexCount)));
      Mn::Math::castInto(
        // TODO make stridedArrayView implicitly convertible to higher
        //  dimensions, like images
        Cr::Containers::arrayCast<2, const Mn::Double>(scene->field<Mn::Double>(*meshViewMaterialFieldId)),
        Cr::Containers::arrayCast<2, Mn::Int>(stridedArrayView(_state->meshViews).slice(&MeshView::materialId)));
    }
    /* Transformations of all objects in the scene. Objects that don't have
       this field default to an indentity transform. */
    Cr::Containers::Array<Mn::Matrix4> transformations{scene->mappingBound()};
    for(Cr::Containers::Pair<Mn::UnsignedInt, Mn::Matrix4> transformation: scene->transformations3DAsArray()) {
      transformations[transformation.first()] = transformation.second();
    }

    /* Populate transforms of all mesh views. Assuming all three fields have
       the same mapping. */
    const Cr::Containers::StridedArrayView1D<const Mn::UnsignedInt> meshViewMapping = scene->mapping<Mn::UnsignedInt>(*meshViewIndexCountFieldId);
    for(std::size_t i = 0; i != meshViewMapping.size(); ++i) {
      _state->meshViews[i].transformation = transformations[meshViewMapping[i]];
    }

//     /* Populate name mapping. Assuming all three fields have it the same. */
//     for(std::size_t i = 0; i != mapping.size(); ++i) {
//       Cr::Containers::String name = importer->objectName(mapping[i]);
//       CORRADE_ASSERT(name, "MagnumRenderer::addFile(): node" << i << "has no name", );
//       // TODO this will get an actual range once we fetch the whole hierarchy
//       //  for each name
//       // TODO fetch parents and transformations, order their mapping
//       //  depth-first (orderParentsDepthFirst()) so each top-level object has
//       //  its children next to itself, then populate / reshuffle the mesh views
//       //  to match this mapping (how!?), and ultimately fill
//       //  meshViewRangeForName only with names of top-level objects (with
//       //  parent = -1) and the child count (i.e., # of elements until another
//       //  -1 parent)
//       _state->meshViewRangeForName.insert({name, {Mn::UnsignedInt(i), Mn::UnsignedInt(i + 1)}});
//     }

    /* Templates are the root objects with their names. Their immediate
       children are the actual meshes. Assumes the order matches the order of
       the custom fields. */
    // TODO hacky and brittle! doesn't handle nested children properly, doesn't
    //  account for a different order of the field vs the child lists
    Mn::UnsignedInt offset = 0;
    for(Mn::UnsignedLong root: scene->childrenFor(-1)) {
      Cr::Containers::Array<Mn::UnsignedLong> children = scene->childrenFor(root);

      Cr::Containers::String name = importer->objectName(root);
      CORRADE_ASSERT(name, "MagnumRenderer::addFile(): node" << root << "has no name", );
      _state->meshViewRangeForName.insert({name, {offset, offset + Mn::UnsignedInt(children.size())}});
      offset += children.size();
    }
    CORRADE_INTERNAL_ASSERT(offset = _state->meshViews.size());
  }

  /* Setup a zero-light (flat) shader, bind buffers that don't change
     per-view */
  Mn::Shaders::PhongGL::Flags flags =
    Mn::Shaders::PhongGL::Flag::MultiDraw|
    Mn::Shaders::PhongGL::Flag::UniformBuffers;
  if(!(_state->flags >= MagnumRendererFlag::NoTextures)) flags |=
    Mn::Shaders::PhongGL::Flag::AmbientTexture|
    Mn::Shaders::PhongGL::Flag::TextureArrays|
    Mn::Shaders::PhongGL::Flag::TextureTransformation;
  // TODO 1024 is 64K divided by 64 bytes needed for one draw uniform, have
  //  that fetched from actual GL limits instead
  _state->shader = Mn::Shaders::PhongGL{flags, 0, importer->materialCount(), 1024};
  _state->shader
    .bindMaterialBuffer(_state->materialUniform);
  if(!(_state->flags >= MagnumRendererFlag::NoTextures)) _state->shader
    .bindAmbientTexture(_state->texture);
}

std::size_t MagnumRenderer::add(const Mn::UnsignedInt sceneId, const Cr::Containers::StringView name, const Mn::Matrix4& transformation) {
  CORRADE_ASSERT(sceneId < _state->scenes.size(),
    "MagnumRenderer::add(): index" << sceneId << "out of range for" << _state->scenes.size() << "scenes", {});

  Scene& scene = _state->scenes[sceneId];
  /* Using a non-owning wrapper over the view to avoid an allocated string copy
     because yes hello STL you're uhhmazing */
  const auto found = _state->meshViewRangeForName.find(Cr::Containers::String::nullTerminatedView(name));
  CORRADE_ASSERT(found != _state->meshViewRangeForName.end(),
    "MagnumRenderer::add(): name" << name << "not found", {});

  /* Add a top-level object */
  const std::size_t id = scene.transformations.size();
  // TODO this adds an empty draw, which is useless; do better (separate
  //  transforms from draws)
  arrayAppend(scene.parents, -1);
  arrayAppend(scene.transformations, transformation);
  arrayAppend(scene.absoluteTransformations, Cr::InPlaceInit);
  arrayAppend(scene.draws, Cr::InPlaceInit)
    .setMaterialId(0);
  arrayAppend(scene.textureTransformations, Cr::InPlaceInit)
    .setLayer(0);
  arrayAppend(scene.drawCommands, Cr::InPlaceInit, 0u, 0u);

  /* Add the whole hierarchy under this name */
  for(std::size_t i = found->second.first(); i != found->second.second(); ++i) {
    const MeshView& meshView = _state->meshViews[i];
    /* The following meshes are children of the first one, inheriting its
       transformation */
    arrayAppend(scene.parents, id);
    arrayAppend(scene.transformations, meshView.transformation);

    /* The actual absolute transformation will get filled each time draw() is
       called */
    arrayAppend(scene.absoluteTransformations, Cr::InPlaceInit);

    arrayAppend(scene.draws, Cr::InPlaceInit)
      .setMaterialId(meshView.materialId);
    arrayAppend(scene.textureTransformations, Cr::InPlaceInit)
      .setTextureMatrix(_state->textureTransformations[meshView.materialId].transformation)
      .setLayer(_state->textureTransformations[meshView.materialId].layer);
    arrayAppend(scene.drawCommands, Cr::InPlaceInit,
      meshView.indexOffsetInBytes, meshView.indexCount);
  }

  /* Assuming add() is called relatively infrequently compared to draw(),
     upload the changed draw and texture transform buffers. Transformation
     buffer will be updated in draw(). */
  scene.drawUniform.setData(scene.draws);
  scene.textureTransformationUniform.setData(scene.textureTransformations);

  return id;
}

std::size_t MagnumRenderer::add(const Mn::UnsignedInt scene, const Cr::Containers::StringView name) {
  return add(scene, name, Mn::Matrix4{});
}

void MagnumRenderer::clear(const Mn::UnsignedInt sceneId) {
  CORRADE_ASSERT(sceneId < _state->scenes.size(),
    "MagnumRenderer::clear(): index" << sceneId << "out of range for" << _state->scenes.size() << "scenes", );

  Scene& scene = _state->scenes[sceneId];
  // TODO have arrayClear()
  /* Resizing instead of `= {}` to not discard the memory */
  arrayResize(scene.parents, 0);
  arrayResize(scene.transformations, 0);
  /* Keep the root absolute transform here tho (same state as when initially
     constructed) */
  arrayResize(scene.absoluteTransformations, 1);
  arrayResize(scene.draws, 0);
  arrayResize(scene.textureTransformations, 0);
  arrayResize(scene.drawCommands, 0);
}

Mn::Matrix4& MagnumRenderer::camera(const Mn::UnsignedInt scene) {
  return _state->projections[scene].projectionMatrix;
}

Cr::Containers::StridedArrayView1D<Mn::Matrix4> MagnumRenderer::transformations(const Mn::UnsignedInt scene) {
  return _state->scenes[scene].transformations;
}

void MagnumRenderer::draw(Mn::GL::AbstractFramebuffer& framebuffer) {
  /* Calculate absolute transformations */
  for(std::size_t sceneId = 0; sceneId != _state->scenes.size(); ++sceneId) {
    Scene& scene = _state->scenes[sceneId];

    // TODO have a tool for this
    scene.absoluteTransformations[0].setTransformationMatrix(Mn::Matrix4{});
//     if(sceneId == 0) !Mn::Debug{} << stridedArrayView(scene.transformations).slice(&Mn::Matrix4::translation);
    for(std::size_t i = 0; i != scene.transformations.size(); ++i)
      scene.absoluteTransformations[i + 1].setTransformationMatrix(
        scene.absoluteTransformations[scene.parents[i] + 1].transformationMatrix *scene.transformations[i]);
//     if(sceneId == 0) !Mn::Debug{} << stridedArrayView(scene.absoluteTransformations).slice(&Mn::Shaders::TransformationUniform3D::transformationMatrix).slice(&Mn::Matrix4::translation);
  }

  /* Upload projection and transformation uniforms, assuming they change every
     frame. Do it before the draw loop to minimize stalls. */
  _state->projectionUniform.setData(_state->projections);
  for(std::size_t sceneId = 0; sceneId != _state->scenes.size(); ++sceneId)
    // TODO have this somehow in a single buffer instead
    // TODO needs arrayInsert(), heh
    _state->scenes[sceneId].transformationUniform.setData(_state->scenes[sceneId].absoluteTransformations.exceptPrefix(1));

  for(Mn::Int y = 0; y != _state->tileCount.y(); ++y) {
    for(Mn::Int x = 0; x != _state->tileCount.x(); ++x) {
      framebuffer.setViewport(Mn::Range2Di::fromSize(Mn::Vector2i{x, y}*_state->tileSize, _state->tileSize));

      const std::size_t scene = y*_state->tileCount.x() + x;

//       !Debug{} << scene << _state->scenes[scene].textureTransformations

      // TODO bind the actual view
      // TODO what is the above TODO about, actually?!

      // TODO split by draw count limit
      _state->shader
        // TODO bind all buffers together with a multi API
        .bindProjectionBuffer(_state->projectionUniform,
          scene*sizeof(Mn::Shaders::ProjectionUniform3D),
          sizeof(Mn::Shaders::ProjectionUniform3D))
        .bindTransformationBuffer(_state->scenes[scene].transformationUniform)
        .bindDrawBuffer(_state->scenes[scene].drawUniform);
      if(!(_state->flags & MagnumRendererFlag::NoTextures))
        _state->shader.bindTextureTransformationBuffer(_state->scenes[scene].textureTransformationUniform);
      _state->shader
        .draw(_state->mesh,
          stridedArrayView(_state->scenes[scene].drawCommands).slice(&DrawCommand::indexCount),
          nullptr,
          stridedArrayView(_state->scenes[scene].drawCommands).slice(&DrawCommand::indexOffsetInBytes));
    }
  }
}

}}
