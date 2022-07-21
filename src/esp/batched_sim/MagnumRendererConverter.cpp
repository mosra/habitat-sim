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
#include <Magnum/Math/Color.h>
#include <Magnum/Math/Matrix3.h>
#include <Magnum/Math/Matrix4.h>
#include <Magnum/MeshTools/Concatenate.h>
#include <Magnum/MeshTools/RemoveDuplicates.h>
#include <Magnum/SceneTools/FlattenMeshHierarchy.h>
#include <Magnum/TextureTools/Atlas.h>
#include <Magnum/Trade/AbstractImporter.h>
#include <Magnum/Trade/AbstractImageConverter.h>
#include <Magnum/Trade/AbstractSceneConverter.h>
#include <Magnum/Trade/ImageData.h>
#include <Magnum/Trade/MaterialData.h>
#include <Magnum/Trade/MeshData.h>
#include <Magnum/Trade/SceneData.h>
#include <Magnum/Trade/TextureData.h>

using namespace Magnum;
using namespace Math::Literals;
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

constexpr Vector2i TextureAtlasSize{2048};

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
  if(PluginManager::PluginMetadata* m = converterManager.metadata("GltfSceneConverter")) {
    // TODO fix the damn texcoord orientation in the converter already
    m->configuration().setValue("orientation", "rdo");
  }

  /* Magnum's OBJ importer is ... well, not great. It'll get replaced
     eventually. */
  if(importerManager.loadState("ObjImporter") != PluginManager::LoadState::NotFound)
    importerManager.setPreferredPlugins("ObjImporter", {"AssimpImporter"});

  /* Use StbImageImporter because for it we can override channel count */
  {
    PluginManager::PluginMetadata* m = importerManager.metadata("StbImageImporter");
    CORRADE_INTERNAL_ASSERT(m);
    m->configuration().setValue("forceChannelCount", 3);
    importerManager.setPreferredPlugins("PngImporter", {"StbImageImporter"});
    importerManager.setPreferredPlugins("JpegImporter", {"StbImageImporter"});
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

  /* Meshes and textures get collected, then joined / packed, then added to the
     converter */
  Containers::Array<Trade::MeshData> inputMeshes;
  Containers::Array<Trade::ImageData2D> inputImages;

  /* Reserve the first image for texture-less objects -- a single white pixel */
  constexpr Color3ub WhitePixel[]{0xffffff_rgb};
  arrayAppend(inputImages, Trade::ImageData2D{
    PixelStorage{}.setAlignment(1),
    PixelFormat::RGB8Unorm, {1, 1},
    Trade::DataFlags{}, WhitePixel});

  /* As textures get packed into an atlas, the materials will need to be
     updated with final layer IDs and offsets. Store them temporarily in an
     array, using the imported image index and zero offset as placeholders. */
  Containers::Array<Trade::MaterialData> inputMaterials;

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

  const auto importSingleMesh = [&](
    Containers::StringView filename
  ) {
    CORRADE_INTERNAL_ASSERT_OUTPUT(importer->openFile(filename));
    CORRADE_INTERNAL_ASSERT(importer->meshCount() == 1);

    Parent root;
    root.mapping = parents.size();
    root.parent = -1;

    arrayAppend(parents, root);
    converter->setObjectName(root.mapping,
      Utility::Path::splitExtension(Utility::Path::split(filename).second()).first());

    Containers::Optional<Trade::MeshData> mesh = importer->mesh(0);
    CORRADE_INTERNAL_ASSERT(mesh && mesh->primitive() == MeshPrimitive::Triangles);
    /* Assumin STL files, which are not indexed and thus with many duplicate
       vertices. Create an index buffer by deduplicating them. */
    // TODO possibly useful to filter and recreate normals also?
    mesh = MeshTools::removeDuplicates(*mesh);

    Mesh m;
    m.mapping = root.mapping;
    m.mesh = 0;
    m.meshIndexCount = mesh->indexCount();
    m.meshMaterial = inputMaterials.size();
    m.meshIndexOffset = indexOffset;
    indexOffset += mesh->indexCount()*4; // TODO ints hardcoded

    /* Add an empty white material */
    arrayAppend(inputMaterials, Trade::MaterialData{Trade::MaterialType::Flat, {
      Trade::MaterialAttributeData{Trade::MaterialAttribute::BaseColorTexture, 0u},
      Trade::MaterialAttributeData{Trade::MaterialAttribute::BaseColorTextureLayer, 0u},
      Trade::MaterialAttributeData{Trade::MaterialAttribute::BaseColorTextureMatrix,
        Matrix3::scaling(Vector2{1}/Vector2(TextureAtlasSize))}
    }});

    arrayAppend(meshes, m);
  };

  const auto import = [&](
    Containers::StringView filename,
    Containers::StringView name = {},
    Containers::Optional<Color4> forceColor = {},
    std::unordered_map<Containers::String, Containers::Pair<UnsignedInt, UnsignedInt>>* uniqueMeshes = nullptr
  ) {
    CORRADE_INTERNAL_ASSERT_OUTPUT(importer->openFile(filename));
    CORRADE_INTERNAL_ASSERT(importer->sceneCount() == 1); // TODO multi-scene??

    Containers::Optional<Trade::SceneData> scene = importer->scene(0);
    CORRADE_INTERNAL_ASSERT(scene);

    /* Top-level object, parent of the others */
    Parent root;
    root.mapping = parents.size();
    root.parent = -1;
    arrayAppend(parents, root);
    converter->setObjectName(root.mapping,
      name ? name :
      Utility::Path::splitExtension(Utility::Path::split(filename).second()).first());

    /* Meshes are unfortunately named in a useless way, so override them with
       names from the objects referencing them instead */
    // TODO this is replica-specific, make a dedicated function
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
      Containers::String meshName = meshNames[transformationMeshMaterial.first()];

      /* Skip non-triangle meshes */
      if(mesh->primitive() != MeshPrimitive::Triangles &&
         mesh->primitive() != MeshPrimitive::TriangleFan &&
         mesh->primitive() != MeshPrimitive::TriangleStrip) {
        Warning{} << "Mesh" << meshName << "in" << Utility::Path::split(filename).second() << "is" << mesh->primitive() << Debug::nospace << ", skipping";
        continue;
      }

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
        if(uniqueMeshes) {
          Debug{} << "New mesh" << meshName << "in" << Utility::Path::split(filename).second();
          uniqueMeshes->insert({std::move(meshName), {indexOffset, mesh->indexCount()}});
        }
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
        Containers::Optional<Trade::MaterialData> material = importer->material(transformationMeshMaterial.second());
        CORRADE_INTERNAL_ASSERT(material);

        /* Override the color if the attribute is already there. Otherwise a
           new attribute gets added to the attributes array below */
        bool hasColorAttribute = false;
        if(forceColor) {
          if(material->hasAttribute(Trade::MaterialAttribute::BaseColor)) {
            hasColorAttribute = true;
            material->mutableAttribute<Color4>(Trade::MaterialAttribute::BaseColor) = *forceColor;
          }
        /* If it's a Phong material (an OBJ), make sure BaseColor is set
           instead below */
        // TODO some compat for this? Assimp should have something, a similar
        //  feature got reverted in 5.1
        } else if(material->hasAttribute(Trade::MaterialAttribute::DiffuseColor)) {
          forceColor = material->attribute<Color4>(Trade::MaterialAttribute::DiffuseColor);
        }

        /* Not calling releaseAttributeData() yet either, as we need to ask
           hasAttribute() first */
        Containers::Array<Trade::MaterialAttributeData> attributes;
        // TODO findAttributeId()!
        if(material->hasAttribute(Trade::MaterialAttribute::BaseColorTexture) ||
           material->hasAttribute(Trade::MaterialAttribute::DiffuseTexture)) {
          Debug{} << "New textured material for" << meshNames[transformationMeshMaterial.first()] << "in" << Utility::Path::split(filename).second();

          const bool hasBaseColorTexture = material->hasAttribute(Trade::MaterialAttribute::BaseColorTexture);

          UnsignedInt& textureId = material->mutableAttribute<UnsignedInt>(
            hasBaseColorTexture ?
              Trade::MaterialAttribute::BaseColorTexture :
              Trade::MaterialAttribute::DiffuseTexture
          );

          Containers::Optional<Trade::TextureData> texture = importer->texture(textureId);
          CORRADE_INTERNAL_ASSERT(texture && texture->type() == Trade::TextureType::Texture2D);

          /* Patch the material to use the zero texture but add a layer as
             well, referencing the just added image (plus one for the first
             white layer) and make it just Flat */
          textureId = 0;
          CORRADE_INTERNAL_ASSERT(material->layerCount() == 1);
          attributes = material->releaseAttributeData();
          /* If there's DiffuseTexture, add a BaseColorTexture instead */
          if(!hasBaseColorTexture)
            arrayAppend(attributes, InPlaceInit, Trade::MaterialAttribute::BaseColorTexture, 0u);
          arrayAppend(attributes, InPlaceInit, Trade::MaterialAttribute::BaseColorTextureLayer, UnsignedInt(inputImages.size()));

          Containers::Optional<Trade::ImageData2D> image = importer->image2D(texture->image());
          // TODO detection of single-color images here
          CORRADE_INTERNAL_ASSERT((image->size() <= TextureAtlasSize).all());
          /* Add texture scaling if the image is smaller than 2K */
          if((image->size() < TextureAtlasSize).any())
            arrayAppend(attributes, InPlaceInit, Trade::MaterialAttribute::BaseColorTextureMatrix,
            Matrix3::scaling(Vector2(image->size())/Vector2(TextureAtlasSize)));

          arrayAppend(inputImages, *std::move(image));

        /* Otherwise make it reference the first image, which is a 1x1 white
           pixel */
        } else {
          Debug{} << "New untextured material for" << meshNames[transformationMeshMaterial.first()] << "in" << Utility::Path::split(filename).second();

          attributes = material->releaseAttributeData();
          arrayAppend(attributes, {
            Trade::MaterialAttributeData{Trade::MaterialAttribute::BaseColorTexture, 0u},
            Trade::MaterialAttributeData{Trade::MaterialAttribute::BaseColorTextureLayer, 0u},
            Trade::MaterialAttributeData{Trade::MaterialAttribute::BaseColorTextureMatrix,
              Matrix3::scaling(Vector2{1}/Vector2(TextureAtlasSize))}
          });
        }

        if(forceColor && !hasColorAttribute) {
          arrayAppend(attributes, InPlaceInit, Trade::MaterialAttribute::BaseColor, *forceColor);
        }

        /* Make it just Flat */
        material = Trade::MaterialData{Trade::MaterialType::Flat, std::move(attributes)};

        importedMaterialIds[transformationMeshMaterial.second()] = m.meshMaterial = inputMaterials.size();
        arrayAppend(inputMaterials, *std::move(material));
      }

      arrayAppend(meshes, m);
    }
  };

  /* Spot */
  Containers::String spotPath = Utility::Path::join(args.value("input"), "extra_source_data_v0/spot_arm_textured/spot_arm/spot_arm/meshes");
  for(auto&& i: std::initializer_list<Containers::Pair<const char*, Color4>>{
    // TODO why not take the colored models instead of having to manually set
    //  the color?
    // TODO or, rather, just parse the URDF already
    {"arm0.link_el0.obj", 0x3f3f3f_rgbf},
    {"arm0.link_el1.obj", 0xffff00_rgbf},
    {"arm0.link_fngr.obj", 0x7f7f7f_rgbf},
    {"arm0.link_hr0.obj", 0xffff00_rgbf},
    {"arm0.link_sh0.obj", 0x3f3f3f_rgbf},
    {"arm0.link_sh1.obj", 0x7f7f7f_rgbf},
    {"arm0.link_wr0.obj", 0x3f3f3f_rgbf},
    {"arm0.link_wr1.obj", 0xffff00_rgbf},
    {"base.obj", 0x4cc6ff_rgbf},
    {"fl.hip.obj", 0x3f3f3f_rgbf},
    {"fl.lleg.obj", 0x3f3f3f_rgbf},
    {"fl.uleg.obj", 0x4cc6ff_rgbf},
    {"fr.hip.obj", 0x3f3f3f_rgbf},
    {"fr.lleg.obj", 0x3f3f3f_rgbf},
    {"fr.uleg.obj", 0x4cc6ff_rgbf},
    {"hl.hip.obj", 0x3f3f3f_rgbf},
    {"hl.lleg.obj", 0x3f3f3f_rgbf},
    {"hl.uleg.obj", 0x4cc6ff_rgbf},
    {"hr.hip.obj", 0x3f3f3f_rgbf},
    {"hr.lleg.obj", 0x3f3f3f_rgbf},
    {"hr.uleg.obj", 0x4cc6ff_rgbf}
  })
    import(Utility::Path::join(spotPath, i.first()), {}, i.second());

  /* ReplicaCAD articulated objects */
  // TODO this needs the images deduplicated ... some content-addressing
  //  file callback?
  Containers::String replicaArticulatedObjectsPath = Utility::Path::join(args.value("input"), "ReplicaCAD_baked_lighting_v1.5/urdf_uncompressed");
  for(const char* filename: {
    "fridge/body_brighter2.glb", // TODO "fixed" model instead
    "fridge/bottom_door_brighter2.glb", // TODO "fixed"
    "fridge/top_door_brighter2.glb", // TODO "fixed"
    "kitchen_counter/kitchen_counter.glb",
    "kitchen_counter/drawer1.glb",
    "kitchen_counter/drawer2.glb",
    "kitchen_counter/drawer3.glb",
    "kitchen_counter/drawer4.glb",
    "kitchen_cupboards/kitchencupboard_base.glb",
    "kitchen_cupboards/kitchencupboard_doorWhole_L.glb",
    "kitchen_cupboards/kitchencupboard_doorWhole_R.glb",
    "kitchen_cupboards/kitchencupboard_doorWindow_L.glb",
    "kitchen_cupboards/kitchencupboard_doorWindow_R.glb",
    "doors/door2.glb",
    "cabinet/cabinet.glb",
    "cabinet/door.glb",
    "chest_of_drawers/chestOfDrawers_base.glb", // TODO "fixed"
    "chest_of_drawers/chestOfDrawers_DrawerBot.glb", // TODO "fixed"
    "chest_of_drawers/chestOfDrawers_DrawerMid.glb", // TODO "fixed"
    "chest_of_drawers/chestOfDrawers_DrawerTop.glb" // TODO "fixed"
  })
    import(Utility::Path::join(replicaArticulatedObjectsPath, filename));

  /* Fetch */
  // TODO there was some "preprocessed_fetch_meshes" glb file instead
  Containers::String fetchPath = Utility::Path::join(args.value("input"), "hab_fetch_v1.0/meshes");
  for(const char* filename: {
//     "base_link.dae", // TODO it's a 4K texture, FFS
    "elbow_flex_link.dae",
    "forearm_roll_link.dae",
    "gripper_link.dae",
    "head_pan_link.dae",
    "head_tilt_link.dae",
    "shoulder_lift_link.dae",
    "shoulder_pan_link.dae",
    "torso_fixed_link.dae",
    "torso_lift_link.dae",
    "upperarm_roll_link.dae",
    "wrist_flex_link.dae",
    "wrist_roll_link.dae"
  })
    import(Utility::Path::join(fetchPath, filename));
  for(const char* filename: {
    "bellows_link.STL",
    "estop_link.STL", // TODO or .dae?
    "l_wheel_link.STL",
    "l_gripper_finger_link_opt.stl",
    "laser_link.STL",
    "r_gripper_finger_link_opt.stl",
    "r_wheel_link.STL",
  })
    importSingleMesh(Utility::Path::join(fetchPath, filename));

  /* Debug models */
  // TODO generate these all from Primitives instead of doing crazy stuff with
  //  glTF files
  Containers::String debugModelsPath = Utility::Path::join(args.value("input"), "extra_source_data_v0/debug_models");
  for(const char* filename: {
    // TODO make the "wireframe" a reasonable wireframe mesh
    "sphere_green_wireframe.glb",
    "sphere_orange_wireframe.glb",
    "sphere_blue_wireframe.glb",
    "sphere_pink_wireframe.glb",
    "cube_gray_shaded.glb",
    "cube_green.glb",
    "cube_blue.glb",
    "cube_pink.glb",
    "cube_green_wireframe.glb",
    "cube_orange_wireframe.glb",
    "cube_blue_wireframe.glb",
    "cube_pink_wireframe.glb"
  })
    import(Utility::Path::join(debugModelsPath, filename));

  /* YCB */
  Containers::String ycbPath = Utility::Path::join(args.value("input"), "hab_ycb_v1.1/ycb/");
  for(const char* name: {
    "024_bowl",
    "003_cracker_box",
    "010_potted_meat_can",
    "002_master_chef_can",
    "004_sugar_box",
    "005_tomato_soup_can",
    "009_gelatin_box",
    "008_pudding_box",
    "007_tuna_fish_can"
  })
    import(Utility::Path::join({ycbPath, name, "google_16k/textured.obj"}), name);

  /* ReplicaCAD */
  Containers::String replicaPath = Utility::Path::join(args.value("input"), "ReplicaCAD_baked_lighting_v1.5/stages_uncompressed");
  std::unordered_map<Containers::String, Containers::Pair<UnsignedInt, UnsignedInt>> uniqueReplicaMeshes;
  for(std::size_t i = 0; i <= 4; ++i) {
    for(std::size_t j = 0; j <= 20; ++j) {
      import(
        Utility::Path::join(replicaPath, Utility::format("Baked_sc{}_staging_{:.2}.glb", i, j)),
        {}, {},
        &uniqueReplicaMeshes);
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
  for(Vector2& i: mesh.mutableAttribute<Vector2>(Trade::MeshAttribute::TextureCoordinates)) {
    // TODO remmove this once GltfSceneConverter does that itself
    i.y() = 1.0f - i.y();
  }
  CORRADE_INTERNAL_ASSERT_OUTPUT(converter->add(mesh));

  /* Pack input images into an atlas */
  Containers::Pair<Int, Containers::Array<Vector3i>> layerCountOffsets =
    TextureTools::atlasArrayPowerOfTwo(TextureAtlasSize, Containers::StridedArrayView1D<const Trade::ImageData2D>(inputImages).slice(&Trade::ImageData2D::size));

  /* Update layer and offset info in the materials, add them to the converter */
  for(Trade::MaterialData& inputMaterial: inputMaterials) {
    UnsignedInt& layer = inputMaterial.mutableAttribute<UnsignedInt>(Trade::MaterialAttribute::BaseColorTextureLayer);
    const UnsignedInt imageId = layer;

    layer = layerCountOffsets.second()[imageId].z();

    /* If the material has a texture matrix (textures that are same as atlas
       layer size don't have it), update the offset there */
    // TODO findMaterial()!!
    if(inputMaterial.hasAttribute(Trade::MaterialAttribute::BaseColorTextureMatrix)) {
      Matrix3& matrix = inputMaterial.mutableAttribute<Matrix3>(Trade::MaterialAttribute::BaseColorTextureMatrix);
      matrix = Matrix3::translation(Vector2{layerCountOffsets.second()[imageId].xy()}/Vector2{TextureAtlasSize})*matrix;
    }

    CORRADE_INTERNAL_ASSERT_OUTPUT(converter->add(inputMaterial));
  }

  /* A combined 3D image */
  Trade::ImageData3D image{PixelFormat::RGB8Unorm,
    {TextureAtlasSize, layerCountOffsets.first()},
    Containers::Array<char>{NoInit, std::size_t(TextureAtlasSize.product()*layerCountOffsets.first()*3)}};
  /* Copy the images to their respective locations, calculate waste ratio
     during the process */
  std::size_t inputImageArea = 0;
  for(std::size_t i = 0; i != inputImages.size(); ++i) {
    inputImageArea += inputImages[i].size().product();
    // TODO handle RGBA (drop alpha), or convert everything to RGBA instead
    CORRADE_INTERNAL_ASSERT(inputImages[i].format() == PixelFormat::RGB8Unorm);
    Utility::copy(inputImages[i].pixels(),
      /** @todo sliceCount(), finally */
      image.mutablePixels()[layerCountOffsets.second()[i].z()].exceptPrefix({
        // TODO have implicit conversion of Vector to StridedDimensions, FINALLY
        std::size_t(layerCountOffsets.second()[i].x()),
        std::size_t(layerCountOffsets.second()[i].y()),
        0
      }).prefix({
        // TODO have implicit conversion of Vector to StridedDimensions, FINALLY
        std::size_t(inputImages[i].size().x()),
        std::size_t(inputImages[i].size().y()),
        std::size_t(inputImages[i].pixelSize())
      }));
  }

  Debug{} << inputImages.size() << "images packed to" << layerCountOffsets.first() << "layers," << Utility::format("{:.2f}", 100.0f - 100.0f*inputImageArea/(TextureAtlasSize.product()*layerCountOffsets.first())) << Debug::nospace << "% area wasted";

  /* Clear the original images array to relieve the memory pressure a bit --
     Basis is HUNGRY */
  inputImages = {};
  CORRADE_INTERNAL_ASSERT_OUTPUT(converter->add(image));

  /* And a texture referencing the only image */
  CORRADE_INTERNAL_ASSERT_OUTPUT(converter->add(Trade::TextureData{Trade::TextureType::Texture2DArray,
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
    Trade::SceneFieldData{SceneFieldMeshViewIndexOffset,
      outputMeshes.slice(&Mesh::mapping),
      outputMeshes.slice(&Mesh::meshIndexOffset)},
    Trade::SceneFieldData{SceneFieldMeshViewIndexCount,
      outputMeshes.slice(&Mesh::mapping),
      outputMeshes.slice(&Mesh::meshIndexCount)},
    Trade::SceneFieldData{SceneFieldMeshViewMaterial,
      outputMeshes.slice(&Mesh::mapping),
      outputMeshes.slice(&Mesh::meshMaterial)}
  }};
  CORRADE_INTERNAL_ASSERT_OUTPUT(converter->add(scene));

  return converter->endFile() ? 0 : 1;
}
