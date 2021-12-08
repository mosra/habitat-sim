#include <unordered_map>
#include <Corrade/Containers/ArrayTuple.h>
#include <Corrade/Containers/GrowableArray.h>
#include <Corrade/Containers/Pair.h>
#include <Corrade/Containers/Reference.h>
#include <Corrade/Containers/Triple.h>
#include <Corrade/PluginManager/PluginMetadata.h>
#include <Corrade/Utility/Algorithms.h>
#include <Corrade/Utility/Arguments.h>
#include <Corrade/Utility/ConfigurationGroup.h>
#include <Corrade/Utility/Format.h>
#include <Corrade/Utility/MurmurHash2.h>
#include <Corrade/Utility/Path.h>
#include <Magnum/PixelFormat.h>
#include <Magnum/Math/Matrix4.h>
#include <Magnum/MeshTools/Concatenate.h>
#include <Magnum/SceneTools/FlattenMeshHierarchy.h>
#include <Magnum/Trade/AbstractImporter.h>
#include <Magnum/Trade/AbstractImageConverter.h>
#include <Magnum/Trade/AbstractSceneConverter.h>
#include <Magnum/Trade/ImageData.h>
#include <Magnum/Trade/MaterialData.h>
#include <Magnum/Trade/MeshData.h>
#include <Magnum/Trade/SceneData.h>
#include <Magnum/Trade/TextureData.h>

using namespace Magnum;
using namespace Containers::Literals;

/* std::hash specialization to be able to use Corrade::String in unordered_map */
// TODO put directly into Corrade!! it's duplicated IN EIGHT!! PLACES! ALREADY!
namespace std {
    template<> struct hash<Containers::String> {
        std::size_t operator()(const Containers::String& key) const {
            const Utility::MurmurHash2 hash;
            const Utility::HashDigest<sizeof(std::size_t)> digest = hash(key.data(), key.size());
            return *reinterpret_cast<const std::size_t*>(digest.byteArray());
        }
    };
}

// TODO make these builtin
constexpr Trade::SceneField SceneFieldMeshViewIndexOffset = Trade::sceneFieldCustom(0);
constexpr Trade::SceneField SceneFieldMeshViewIndexCount = Trade::sceneFieldCustom(1);
constexpr Trade::SceneField SceneFieldMeshViewMaterial = Trade::sceneFieldCustom(2);

int main(int argc, char** argv) {
  Utility::Arguments args;
  args.addArgument("input").setHelp("input", "input file prefix")
    .addArgument("output").setHelp("output", "output file")
    .addOption('C', "converter", "GltfSceneConverter").setHelp("converter", "converter plugin to use")
    .parse(argc, argv);

  PluginManager::Manager<Trade::AbstractImporter> importerManager;
  PluginManager::Manager<Trade::AbstractImageConverter> imageConverterManager;
  PluginManager::Manager<Trade::AbstractSceneConverter> converterManager;
  converterManager.registerExternalManager(imageConverterManager);

  /* Reasonable config defaults */
  if(PluginManager::PluginMetadata* m = importerManager.metadata("GltfImporter")) {
    /* Don't need this */
    m->configuration().setValue("phongMaterialFallback", false);
  }
  if(PluginManager::PluginMetadata* m = imageConverterManager.metadata("BasisImageConverter")) {
    // TODO set up once GltfSceneConverter uses it directly
  }

  Containers::Pointer<Trade::AbstractImporter> importer = importerManager.loadAndInstantiate("AnySceneImporter");
  Containers::Pointer<Trade::AbstractSceneConverter> converter = converterManager.loadAndInstantiate(args.value("converter"));

  /* To prevent the file from being opened by unsuspecting libraries */
  converter->configuration().addValue("extensionUsed", "MAGNUMX_mesh_views");
  converter->configuration().addValue("extensionRequired", "MAGNUMX_mesh_views");

  /* Begin file conversion */
  converter->beginFile(args.value("output"));
  converter->setSceneFieldName(SceneFieldMeshViewIndexOffset, "meshViewIndexOffset");
  converter->setSceneFieldName(SceneFieldMeshViewIndexCount, "meshViewIndexCount");
  converter->setSceneFieldName(SceneFieldMeshViewMaterial, "meshViewMaterial");

  /* Materials are put into the file directly, meshes and textures get
     collected and then joined. */
  Containers::Array<Trade::MeshData> inputMeshes;
  Containers::Array<Trade::ImageData2D> inputImages;
  UnsignedInt indexOffset = 0;

  /* Parent, present for all objects */
  struct Parent {
    UnsignedInt mapping;
    Int parent;
  };
  Containers::Array<Parent> parents;

  /* Transformation, present only for nested objects */
  struct Transformation {
    UnsignedInt mapping;
    Matrix4 transformation;
  };
  Containers::Array<Transformation> transformations;

  /* Mesh assignment, present for all objects except a "template root" that
     contains multiple meshes as children */
  struct Mesh {
    UnsignedInt mapping;
    UnsignedInt mesh; /* always 0 at the moment */
    UnsignedInt meshIndexOffset;
    UnsignedInt meshIndexCount;
    Int meshMaterial;
  };
  Containers::Array<Mesh> meshes;
//   const auto import = [&](Containers::StringView filename) {
//     importer->openFile(filename);
//     CORRADE_INTERNAL_ASSERT(importer->meshCount() == 1);
//     CORRADE_INTERNAL_ASSERT(importer->materialCount() == 1);
//
//     Containers::Optional<Trade::MeshData> mesh = importer->mesh(0);
//     Containers::Optional<Trade::MaterialData> material = importer->material(0);
//
//     /* If the material is textured, extract the image as well */
//     // TODO findAttributeId()!
//     if(material->hasAttribute(Trade::MaterialAttribute::BaseColorTexture)) {
//       UnsignedInt& textureId = material->mutableAttribute<UnsignedInt>(Trade::MaterialAttribute::BaseColorTexture);
//
//       Containers::Optional<Trade::TextureData> texture = importer->texture(textureId);
//       CORRADE_INTERNAL_ASSERT(texture && texture->type() == Trade::TextureType::Texture2D);
//
//       /* Patch the material to use the zero texture but add a layer as well and
//          make it just Flat */
//       textureId = 0;
//       CORRADE_INTERNAL_ASSERT(material->layerCount() == 1);
//       Containers::Array<Trade::MaterialAttributeData> attributes = material->releaseAttributeData();
//       arrayAppend(attributes, InPlaceInit, "baseColorTextureLayer", UnsignedInt(inputImages.size()));
//       material = Trade::MaterialData{Trade::MaterialType::Flat, std::move(attributes)};
//
//       arrayAppend(inputImages, *importer->image2D(texture->image()));
//
//     /* Otherwise just make it Flat */
//     } else {
//       CORRADE_INTERNAL_ASSERT(material->layerCount() == 1);
//       material = Trade::MaterialData{Trade::MaterialType::Flat, material->releaseAttributeData()};
//     }
//
//     /* Object setup */
//     Object o;
//     o.id = objects.size();
//     o.mesh = 0;
//     // TODO concatenate() should do this on its own
//     o.meshIndexOffset = indexOffset;
//     o.meshIndexCount = mesh->indexCount();
//     indexOffset += mesh->indexCount();
//     converter->setObjectName(inputMeshes.size(), Utility::Path::split(filename).second());
//     o.meshMaterial = *converter->add(*material);
//
//     /* Mesh setup */
//     // TODO convert from a strip/... if not triangles
//     arrayAppend(inputMeshes, *std::move(mesh));
//
//     arrayAppend(objects, o);
//   };

  const auto importMultiple = [&](Containers::StringView filename, std::unordered_map<Containers::String, Containers::Pair<UnsignedInt, UnsignedInt>>* uniqueMeshes = nullptr) {
    CORRADE_INTERNAL_ASSERT_OUTPUT(importer->openFile(filename));
    CORRADE_INTERNAL_ASSERT(importer->sceneCount() == 1); // TODO multi-scene??

    Containers::Optional<Trade::SceneData> scene = importer->scene(0);
    CORRADE_INTERNAL_ASSERT(scene);

    /* Top-level object, parent of the others */
    Parent root;
    root.mapping = parents.size();
    root.parent = -1;
    arrayAppend(parents, root);
    converter->setObjectName(root.mapping, Utility::Path::splitExtension(Utility::Path::split(filename).second()).first());

    /* Meshes are unfortunately named in a useless way, so override them with
       names from the objects referencing them instead */
    Containers::Array<Containers::String> meshNames{importer->meshCount()};
    for(Containers::Pair<UnsignedInt, Containers::Pair<UnsignedInt, Int>> objectMeshMaterial: scene->meshesMaterialsAsArray()) {
      const UnsignedInt meshId = objectMeshMaterial.second().first();
      Containers::String objectName = importer->objectName(objectMeshMaterial.first());
      if(!objectName)
        Fatal{} << "No name found for object" << objectMeshMaterial.first() << "referencing mesh" << importer->meshName(meshId) << "in" << Utility::Path::split(filename).second();

      if(meshNames[meshId] && meshNames[meshId] != objectName)
        Fatal{} << "Conflicting name for mesh" << importer->meshName(meshId) << Debug::nospace << ":" << meshNames[meshId] << "vs" << objectName << "in" << Utility::Path::split(filename).second();

      meshNames[meshId] = std::move(objectName);
    }

    /* Assuming materials are shared among meshes, remember the ID of already
       imported materials */
    Containers::Array<Containers::Optional<UnsignedInt>> importedMaterialIds{importer->materialCount()};

    /* Node mesh/material assignments. Each entry will be one child of the
       top-level object. */
    for(Containers::Triple<UnsignedInt, Int, Matrix4> transformationMeshMaterial: SceneTools::flattenMeshHierarchy3D(*scene)) {
      Containers::Optional<Trade::MeshData> mesh = importer->mesh(transformationMeshMaterial.first());
      // TODO convert from a strip/... if not triangles
      CORRADE_INTERNAL_ASSERT(mesh && mesh->isIndexed() && mesh->primitive() == MeshPrimitive::Triangles);

      Parent o;
      o.mapping = parents.size();
      o.parent = root.mapping;
      arrayAppend(parents, o);
      arrayAppend(transformations, InPlaceInit,
        o.mapping,
        transformationMeshMaterial.third());
      /* Save the nested object name as well, for debugging purposes. It'll be
         duplicated among different scenes but that's no problem. */
      converter->setObjectName(o.mapping, meshNames[transformationMeshMaterial.first()]);

      Mesh m;
      m.mapping = o.mapping;
      m.mesh = 0;
      m.meshIndexCount = mesh->indexCount();

      /* Check if a mesh of the same name is already present and reuse it in
         that case, otherwise add to the map. Not using
         `importer->meshName(transformationMeshMaterial.first())`
         because the mesh names are useless `Mesh.XYZ` that don't match between
         files. */
      // TODO avoid string copies by making the map over StringViews? but then
      //  it'd have to be changed again once we don't have the extra meshNames
      //  array
      Containers::String meshName = meshNames[transformationMeshMaterial.first()];
      bool duplicate = false;
      if(uniqueMeshes) {
        std::unordered_map<Containers::String, Containers::Pair<UnsignedInt, UnsignedInt>>::iterator found = uniqueMeshes->find(meshName);
        if(found != uniqueMeshes->end()) {
          if(mesh->indexCount() == found->second.second()) {
            m.meshIndexOffset = found->second.first();
            duplicate = true;
          } else {
            Warning{} << "Mesh" << meshName << "in" << Utility::Path::split(filename).second() << "has" << mesh->indexCount() << "indices but expected" << found->second.second() << Debug::nospace << ", adding a new copy";
          }
        }
      }

      if(!duplicate) {
        if(uniqueMeshes) Debug{} << "New mesh" << meshName << "in" << Utility::Path::split(filename).second();

        uniqueMeshes->insert({std::move(meshName), {indexOffset, mesh->indexCount()}});
        m.meshIndexOffset = indexOffset;
        indexOffset += mesh->indexCount()*4; // TODO ints hardcoded

        // TODO convert from a strip/... if not triangles
        arrayAppend(inputMeshes, *std::move(mesh));
      }

      /* If the material is already parsed, reuse its ID */
      if(const Containers::Optional<UnsignedInt> materialId = importedMaterialIds[transformationMeshMaterial.second()]) {
        m.meshMaterial = *materialId;

      /* Otherwise parse it. If textured, extract the image as well. */
      } else {
        Debug{} << "New material for" << meshNames[transformationMeshMaterial.first()] << "in" << Utility::Path::split(filename).second();

        Containers::Optional<Trade::MaterialData> material = importer->material(transformationMeshMaterial.second());
        CORRADE_INTERNAL_ASSERT(material);
        // TODO findAttributeId()!
        if(material->hasAttribute(Trade::MaterialAttribute::BaseColorTexture)) {
          UnsignedInt& textureId = material->mutableAttribute<UnsignedInt>(Trade::MaterialAttribute::BaseColorTexture);

          Containers::Optional<Trade::TextureData> texture = importer->texture(textureId);
          CORRADE_INTERNAL_ASSERT(texture && texture->type() == Trade::TextureType::Texture2D);

          /* Patch the material to use the zero texture but add a layer as well
            and make it just Flat */
          textureId = 0;
          CORRADE_INTERNAL_ASSERT(material->layerCount() == 1);
          Containers::Array<Trade::MaterialAttributeData> attributes = material->releaseAttributeData();
          arrayAppend(attributes, InPlaceInit, "baseColorTextureLayer", UnsignedInt(inputImages.size()));
          material = Trade::MaterialData{Trade::MaterialType::Flat, std::move(attributes)};

          arrayAppend(inputImages, *importer->image2D(texture->image()));

        /* Otherwise just make it Flat */
        } else {
          // TODO make it reference a white texture??
          CORRADE_INTERNAL_ASSERT(material->layerCount() == 1);
          material = Trade::MaterialData{Trade::MaterialType::Flat, material->releaseAttributeData()};
        }

        importedMaterialIds[transformationMeshMaterial.second()] = m.meshMaterial = *converter->add(*material);
      }

      arrayAppend(meshes, m);
    }
  };

  /* Import the stuff. Well, some of it. */
  // TODO haha
//   Containers::String debugModelsPath = Utility::Path::join(args.value("input"), "extra_source_data_v0/debug_models");
//   import(Utility::Path::join(debugModelsPath, "sphere_green_wireframe.glb"));
//   import(Utility::Path::join(debugModelsPath, "sphere_orange_wireframe.glb"));
//   import(Utility::Path::join(debugModelsPath, "cube_gray_shaded.glb"));
//   import(Utility::Path::join(debugModelsPath, "cube_gray_shaded.glb")); // TODO

  Containers::String replicaPath = Utility::Path::join(args.value("input"), "ReplicaCAD_baked_lighting_v1.5/stages_uncompressed");
  std::unordered_map<Containers::String, Containers::Pair<UnsignedInt, UnsignedInt>> uniqueReplicaMeshes;
  for(std::size_t i = 0; i <= 4; ++i) {
    for(std::size_t j = 0; j <= 20; ++j) {
      importMultiple(Utility::Path::join(replicaPath, Utility::format("Baked_sc{}_staging_{:.2}.glb", i, j)), &uniqueReplicaMeshes);
    }
  }

  /* Target layout for the mesh. So far just for flat rendering, no normals
     etc */
  Trade::MeshData mesh{MeshPrimitive::Triangles, nullptr, {
    Trade::MeshAttributeData{Trade::MeshAttribute::Position, VertexFormat::Vector3, nullptr},
    Trade::MeshAttributeData{Trade::MeshAttribute::TextureCoordinates, VertexFormat::Vector2, nullptr},
  }};
  Containers::Array<Containers::Reference<const Trade::MeshData>> inputMeshReferences;
  for(const Trade::MeshData& inputMesh: inputMeshes)
    arrayAppend(inputMeshReferences, inputMesh);
  MeshTools::concatenateInto(mesh, inputMeshReferences);
  CORRADE_INTERNAL_ASSERT(converter->add(mesh));

  /* A combined 3D image */
  Trade::ImageData3D image{PixelFormat::RGB8Unorm, {2048, 2048, Int(inputImages.size())}, Containers::Array<char>{NoInit, 2048*2048*inputImages.size()*4}};
  for(std::size_t i = 0; i != inputImages.size(); ++i) {
    CORRADE_INTERNAL_ASSERT(inputImages[i].format() == PixelFormat::RGB8Unorm);
    Utility::copy(inputImages[i].pixels(), image.mutablePixels()[i]);
  }

  /* Clear the original images array to relieve the memory pressure a bit --
     Basis is HUNGRY */
  inputImages = {};
  CORRADE_INTERNAL_ASSERT(converter->add(image));

  /* And a texture referencing the only image */
  CORRADE_INTERNAL_ASSERT(converter->add(Trade::TextureData{Trade::TextureType::Texture2DArray,
    SamplerFilter::Linear, SamplerFilter::Linear, SamplerMipmap::Linear,
    SamplerWrapping::Repeat, 0}));

  /* Combine the SceneData. In case of glTF the SceneData could be just a view
     on the whole memory, with no combining, but this future-proofs it for
     dumping into a binary representation */
  // TODO use SceneTools::combine() instead once it's public
  Containers::StridedArrayView1D<Parent> outputParents;
  Containers::StridedArrayView1D<Transformation> outputTransformations;
  Containers::StridedArrayView1D<Mesh> outputMeshes;
  Containers::Array<char> data = Containers::ArrayTuple{
    {NoInit, parents.size(), outputParents},
    {NoInit, transformations.size(), outputTransformations},
    {NoInit, meshes.size(), outputMeshes},
  };
  Utility::copy(parents, outputParents);
  Utility::copy(transformations, outputTransformations);
  Utility::copy(meshes, outputMeshes);
  Trade::SceneData scene{Trade::SceneMappingType::UnsignedInt, parents.size(), {}, data, {
    Trade::SceneFieldData{Trade::SceneField::Parent,
      outputParents.slice(&Parent::mapping),
      outputParents.slice(&Parent::parent)},
    Trade::SceneFieldData{Trade::SceneField::Transformation,
      outputTransformations.slice(&Transformation::mapping),
      outputTransformations.slice(&Transformation::transformation)},
    Trade::SceneFieldData{Trade::SceneField::Mesh,
      outputMeshes.slice(&Mesh::mapping),
      outputMeshes.slice(&Mesh::mesh)},
    Trade::SceneFieldData{
                SceneFieldMeshViewIndexOffset,
      outputMeshes.slice(&Mesh::mapping),
      outputMeshes.slice(&Mesh::meshIndexOffset)},
    Trade::SceneFieldData{
                SceneFieldMeshViewIndexCount,
      outputMeshes.slice(&Mesh::mapping),
      outputMeshes.slice(&Mesh::meshIndexCount)},
    Trade::SceneFieldData{
                SceneFieldMeshViewMaterial,
      outputMeshes.slice(&Mesh::mapping),
      outputMeshes.slice(&Mesh::meshMaterial)}
  }};
  CORRADE_INTERNAL_ASSERT(converter->add(scene));

  return converter->endFile() ? 0 : 1;
}
