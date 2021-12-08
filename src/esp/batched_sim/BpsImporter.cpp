#include <Corrade/Containers/Array.h>
#include <Corrade/Containers/ArrayTuple.h>
#include <Corrade/Containers/GrowableArray.h>
#include <Corrade/Containers/Optional.h>
#include <Corrade/Containers/Pair.h>
#include <Corrade/Containers/StringView.h>
#include <Corrade/Utility/Algorithms.h>
#include <Corrade/Utility/ConfigurationGroup.h>
#include <Corrade/Utility/Json.h>
#include <Corrade/Utility/Path.h>
#include <Magnum/PixelFormat.h>
#include <Magnum/Math/Color.h>
#include <Magnum/Math/Matrix4.h>
#include <Magnum/Trade/AbstractImporter.h>
#include <Magnum/Trade/Data.h>
#include <Magnum/Trade/ImageData.h>
#include <Magnum/Trade/MaterialData.h>
#include <Magnum/Trade/MeshData.h>
#include <Magnum/Trade/SceneData.h>
#include <Magnum/Trade/TextureData.h>
#include <MagnumPlugins/BasisImporter/BasisImporter.h>

using namespace Magnum;

class CORRADE_VISIBILITY_EXPORT BpsImporter: public Trade::AbstractImporter {
    public:
        explicit BpsImporter(PluginManager::AbstractManager& manager, const Containers::StringView& plugin): Trade::AbstractImporter{manager, plugin} {}

        ~BpsImporter() = default;

    private:
        CORRADE_VISIBILITY_LOCAL Trade::ImporterFeatures doFeatures() const override { return {}; }
        CORRADE_VISIBILITY_LOCAL bool doIsOpened() const override { return _in; }
        CORRADE_VISIBILITY_LOCAL void doClose() override { _in = nullptr; }
        CORRADE_VISIBILITY_LOCAL void doOpenFile(Containers::StringView filename) override;

        CORRADE_VISIBILITY_LOCAL UnsignedInt doMeshCount() const override;
        CORRADE_VISIBILITY_LOCAL UnsignedInt doMeshLevelCount(UnsignedInt id) override;
        CORRADE_VISIBILITY_LOCAL Containers::String doMeshAttributeName(UnsignedShort name) override;
        CORRADE_VISIBILITY_LOCAL Trade::MeshAttribute doMeshAttributeForName(Containers::StringView name) override;
        CORRADE_VISIBILITY_LOCAL Containers::Optional<Trade::MeshData> doMesh(UnsignedInt id, UnsignedInt level) override;

        CORRADE_VISIBILITY_LOCAL Containers::String doObjectName(UnsignedLong id) override;
//         CORRADE_VISIBILITY_LOCAL Long doObjectForName(Containers::StringView name) override;

        CORRADE_VISIBILITY_LOCAL UnsignedInt doMaterialCount() const override;
        CORRADE_VISIBILITY_LOCAL Containers::Optional<Trade::MaterialData> doMaterial(UnsignedInt id) override;

        CORRADE_VISIBILITY_LOCAL UnsignedInt doTextureCount() const override;
        CORRADE_VISIBILITY_LOCAL Containers::Optional<Trade::TextureData> doTexture(UnsignedInt id) override;

        CORRADE_VISIBILITY_LOCAL UnsignedInt doImage2DCount() const override;
        CORRADE_VISIBILITY_LOCAL UnsignedInt doImage2DLevelCount(UnsignedInt id) override;
        CORRADE_VISIBILITY_LOCAL Containers::Optional<Trade::ImageData2D> doImage2D(UnsignedInt id, UnsignedInt level) override;

        CORRADE_VISIBILITY_LOCAL UnsignedInt doImage3DCount() const override;
        CORRADE_VISIBILITY_LOCAL UnsignedInt doImage3DLevelCount(UnsignedInt id) override;
        CORRADE_VISIBILITY_LOCAL Containers::Optional<Trade::ImageData3D> doImage3D(UnsignedInt id, UnsignedInt level) override;

        CORRADE_VISIBILITY_LOCAL UnsignedLong doObjectCount() const override;
        CORRADE_VISIBILITY_LOCAL UnsignedInt doSceneCount() const override;
        CORRADE_VISIBILITY_LOCAL Int doDefaultScene() const override;
        CORRADE_VISIBILITY_LOCAL Containers::String doSceneFieldName(UnsignedInt name) override;
        CORRADE_VISIBILITY_LOCAL Trade::SceneField doSceneFieldForName(Containers::StringView name) override;
        CORRADE_VISIBILITY_LOCAL Containers::Optional<Trade::SceneData> doScene(UnsignedInt id) override;

        UnsignedInt _lightCount, _instanceCount;
        std::size_t _lightOffset, _instanceOffset, _dataOffset;
        Containers::StringView _textureDir;
        Containers::Array<Containers::StringView> _textureNames;
        Containers::Array<char> _in;
        Containers::String _basename;

        struct Mapping {
            Containers::String name;
            UnsignedInt mesh, material;
        };
        Containers::Array<Mapping> _mappings;
};

namespace {

/*
 Offset    Size  Content        Value / type
 ------ -------  -------------- ----------------
      0      64  header         BpsHeader
     64          padding
    256    n*20  meshes         BpsMeshInfo[n]
              4  light count
           n*24  lights         BpsLightProperties[n]
             1+  texture dir    char[] (null-terminated)
              4  texture count
           n*1+  texture names  char[][n] (null-terminated)
              4  instance count
           n*56  instances      BpsInstanceProperties[n]
                 padding
 ------ -------  -------------- ----------------
  n*256          mesh data(?)
*/

struct BpsHeader {
    UnsignedInt magic; /* 0x55555555 */
    UnsignedInt version; /* 1 */

    UnsignedInt numMeshes;
    UnsignedInt numVertices;
    UnsignedInt numIndices;
    UnsignedInt numChunks;
    UnsignedInt numMaterials;

    UnsignedLong indexOffset;
    UnsignedLong chunkOffset;
    UnsignedLong materialOffset;

    UnsignedLong totalBytes; /* ... after the base data offset */
};

struct BpsMeshInfo {
    UnsignedInt indexOffset;
    UnsignedInt chunkOffset;
    UnsignedInt numTriangles;
    UnsignedInt numVertices; /* what's this for if there's no vertexOffset? */
    UnsignedInt numChunks;
};

struct BpsLightProperties {
    Vector3 position;
    Color3 color;
};

struct BpsInstanceProperties {
    UnsignedInt meshIndex;
    Int materialIndex; /* used to be an UnsignedInt, we have Int for unassigned materials */
    Matrix4x3 transformation;
};

struct BpsMaterialParams {
    Vector3 baseAlbedo;
    Float roughness;
    /* unsigned in BPS source, but contains values like -1 so i suppose it was
       meant to be signed. Magnum expects unsigned for texture IDs, leaving
       signed only for the raw texIds attribute. */
    Vector4ui texIdxs;
};

}

void BpsImporter::doOpenFile(const Containers::StringView filename) {
    Containers::Optional<Containers::Array<char>> data = Utility::Path::read(filename);
    if(!data) {
        Error{} << "BpsImporter::openFile(): cannot open" << filename;
        return;
    }

    if(data->size() < sizeof(BpsHeader)) {
        Error{} << "BpsImporter::openFile(): thing too small, expected at least" << sizeof(BpsHeader) << "but got" << data->size();
        return;
    }

    /* Save basename for loading other files */
    _basename = Utility::Path::split(filename).first();

    /* Header, mesh infos */
    Containers::ArrayView<const char> f = *data;
    const auto& header = *reinterpret_cast<const BpsHeader*>(f.data());
    f = f.exceptPrefix(256 + header.numMeshes*sizeof(BpsMeshInfo));

    /* Lights */
    _lightCount = *reinterpret_cast<const UnsignedInt*>(f.data());
    f = f.exceptPrefix(4);
    _lightOffset = f.data() - data->data();
    f = f.exceptPrefix(_lightCount*sizeof(BpsLightProperties));

    /* Texture dir */
    _textureDir = f.data();
    f = f.exceptPrefix(_textureDir.size() + 1);

    /* Textures */
    const auto textureCount = *reinterpret_cast<const UnsignedInt*>(f.data());
    _textureNames = Containers::Array<Containers::StringView>{textureCount};
    f = f.exceptPrefix(4);
    for(std::size_t i = 0; i != textureCount; ++i) {
        _textureNames[i] = f.data();
        f = f.exceptPrefix(_textureNames[i].size() + 1);
    }

    /* Instances */
    _instanceCount = *reinterpret_cast<const UnsignedInt*>(f.data());
    f = f.exceptPrefix(4);
    _instanceOffset = f.data() - data->data();
    f = f.exceptPrefix(_instanceCount*sizeof(BpsInstanceProperties));

    /* Padding after */
    _dataOffset = 256*((f.data() - data->data() + 255)/256);

    if(_dataOffset >= data->size()) {
        Error{} << "BpsImporter::openFile(): ended up at" << _dataOffset << "bytes for a" << data->size() << "byte file";
        return;
    }

    if(data->size() != _dataOffset + header.totalBytes) {
        Error{} << "BpsImporter::openFile(): calculated data size" << (data->size() - _dataOffset) << "differs from" << header.totalBytes;
        return;
    }

    /* Extra material/mesh mapping, added as additional scene objects without
       parent or transformation fields */
    Containers::Optional<Utility::Json> mapping = Utility::Json::fromFile(filename + ".mapping.json", Utility::Json::Option::ParseLiterals|Utility::Json::Option::ParseStrings);
    if(!mapping || !mapping->parseUnsignedInts(mapping->root())) {
        Error{} << "BpsImporter::openFile(): cannot parse the mapping file";
        return;
    }
    for(const Utility::JsonToken& i: mapping->root()["mapping"]["meshMappings"].asArray())
        arrayAppend(_mappings, InPlaceInit,
            i["name"].asString(),
            i["meshIdx"].asUnsignedInt(),
            i["mtrlIdx"].asUnsignedInt());

    _in = *std::move(data);
}

UnsignedInt BpsImporter::doMeshCount() const {
    return configuration().value<bool>("meshViews") ?
        1 : reinterpret_cast<const BpsHeader*>(_in.data())->numMeshes;
}

UnsignedInt BpsImporter::doMeshLevelCount(const UnsignedInt id) {
    if(configuration().value<bool>("meshViews") && configuration().value<bool>("meshlets")) {
        CORRADE_INTERNAL_ASSERT(id == 0);
        return 2;
    }

    return 1;
}

namespace {
    constexpr Trade::MeshAttribute MeshAttributeIndexOffset = Trade::meshAttributeCustom(0);
    constexpr Trade::MeshAttribute MeshAttributeChunkOffset = Trade::meshAttributeCustom(1);
    constexpr Trade::MeshAttribute MeshAttributeTriangleCount = Trade::meshAttributeCustom(2);
    constexpr Trade::MeshAttribute MeshAttributeVertexCount = Trade::meshAttributeCustom(3);
    constexpr Trade::MeshAttribute MeshAttributeChunkCount = Trade::meshAttributeCustom(4);
}

Containers::String BpsImporter::doMeshAttributeName(const UnsignedShort name) {
    if(configuration().value<bool>("meshViews")) {
        switch(name) {
            #define _c(name) \
                case meshAttributeCustom(MeshAttribute ## name): return #name;
            _c(IndexOffset)
            _c(ChunkOffset)
            _c(TriangleCount)
            _c(VertexCount)
            _c(ChunkCount)
            #undef _c
        }
    }

    return {};
}

Trade::MeshAttribute BpsImporter::doMeshAttributeForName(const Containers::StringView name) {
    if(configuration().value<bool>("meshViews")) {
        #define _c(name_) \
            if(name == #name_) return MeshAttribute ## name_;
        _c(IndexOffset)
        _c(ChunkOffset)
        _c(TriangleCount)
        _c(VertexCount)
        _c(ChunkCount)
        #undef _c
    }

    return {};
}

Containers::Optional<Trade::MeshData> BpsImporter::doMesh(const UnsignedInt id, UnsignedInt level) {
    const auto& header = *reinterpret_cast<const BpsHeader*>(_in.data());

    if(configuration().value<bool>("meshViews") && level == 1) {
        Containers::Array<char> data{NoInit, header.numMeshes*sizeof(BpsMeshInfo)};
        Containers::StridedArrayView1D<BpsMeshInfo> meshInfos = Containers::arrayCast<BpsMeshInfo>(data);
        Utility::copy(Containers::StridedArrayView1D<const BpsMeshInfo>{reinterpret_cast<const BpsMeshInfo*>(_in.data() + 256), header.numMeshes}, meshInfos);

        return Trade::MeshData{MeshPrimitive::Meshlets, std::move(data), {
            Trade::MeshAttributeData{MeshAttributeIndexOffset,
                meshInfos.slice(&BpsMeshInfo::indexOffset)},
            Trade::MeshAttributeData{MeshAttributeChunkOffset,
                meshInfos.slice(&BpsMeshInfo::chunkOffset)},
            Trade::MeshAttributeData{MeshAttributeTriangleCount,
                meshInfos.slice(&BpsMeshInfo::numTriangles)},
            Trade::MeshAttributeData{MeshAttributeVertexCount,
                meshInfos.slice(&BpsMeshInfo::numVertices)},
            Trade::MeshAttributeData{MeshAttributeChunkCount,
                meshInfos.slice(&BpsMeshInfo::numChunks)},
        }};
    }

    Containers::Array<char> indexData;
    Containers::ArrayView<UnsignedInt> indices;
    if(configuration().value<bool>("meshViews")) {
        indexData = Containers::Array<char>{NoInit, header.numIndices*4};
        indices = Containers::arrayCast<UnsignedInt>(indexData);
        Utility::copy(Containers::arrayCast<const UnsignedInt>(_in.exceptPrefix(_dataOffset + header.indexOffset).prefix(header.numIndices*4)), indices);
    } else {
        const auto& meshInfo = reinterpret_cast<const BpsMeshInfo*>(_in.data() + 256)[id];
        indexData = Containers::Array<char>{NoInit, meshInfo.numTriangles*3*4};
        indices = Containers::arrayCast<UnsignedInt>(indexData);
        Utility::copy(Containers::arrayCast<const UnsignedInt>(_in.exceptPrefix(_dataOffset + header.indexOffset + meshInfo.indexOffset*4).prefix(meshInfo.numTriangles*3*4)), indices);
    }

    struct Vertex {
        Vector3 position;
        Vector3 normal;
        Vector2 textureCoordinates;
    };
    // TODO header.numVertices means, if not using views, each chunk has the
    //  whole copy of the mesh, not exactly great
    // TODO OTOH this could be quite a common use case, introduce
    //  `DataFlag::Shared` and a unique data index (with data retrievable
    //  through AbstractImporter::data()?) so the app can just upload the data
    //  once for all references
    Containers::Array<char> vertexData{NoInit, header.numVertices*sizeof(Vertex)};
    Containers::StridedArrayView1D<Vertex> vertices = Containers::arrayCast<Vertex>(vertexData);
    Utility::copy(Containers::arrayView(reinterpret_cast<const Vertex*>(_in + _dataOffset), header.numVertices), vertices);

    return Trade::MeshData{MeshPrimitive::Triangles,
        std::move(indexData), Trade::MeshIndexData{indices},
        std::move(vertexData), {
            Trade::MeshAttributeData{Trade::MeshAttribute::Position,
                vertices.slice(&Vertex::position)},
            Trade::MeshAttributeData{Trade::MeshAttribute::Normal,
                vertices.slice(&Vertex::normal)},
            Trade::MeshAttributeData{Trade::MeshAttribute::TextureCoordinates,
                vertices.slice(&Vertex::textureCoordinates)},
        }
    };
}

UnsignedInt BpsImporter::doMaterialCount() const {
    return reinterpret_cast<const BpsHeader*>(_in.data())->numMaterials;
}

Containers::Optional<Trade::MaterialData> BpsImporter::doMaterial(const UnsignedInt id) {
    const auto& material = reinterpret_cast<const BpsMaterialParams*>(_in.data() + _dataOffset + reinterpret_cast<const BpsHeader*>(_in.data())->materialOffset)[id];

    Containers::Array<Trade::MaterialAttributeData> attributes;
    arrayAppend(attributes, {
        Trade::MaterialAttributeData{Trade::MaterialAttribute::BaseColor,
            Color4{material.baseAlbedo}},
        Trade::MaterialAttributeData{Trade::MaterialAttribute::Roughness,
            material.roughness},
        /* Just for introspection purposes, in case the other 3 values have
           something important */
        Trade::MaterialAttributeData{"texIdxs", Trade::MaterialAttributeType::Vector4i,
            &material.texIdxs}
    });
    if(configuration().value<bool>("phongMaterialFallback"))
        arrayAppend(attributes, InPlaceInit,
            Trade::MaterialAttribute::DiffuseColor,
            Color4{material.baseAlbedo});

    if(configuration().value<bool>("textureArrays")) {
        if(configuration().value<bool>("textureArraysForceAllMaterialsTextured") || material.texIdxs[0] != ~UnsignedInt{}) {
            arrayAppend(attributes, {
                Trade::MaterialAttributeData{Trade::MaterialAttribute::BaseColorTexture,
                    0u},
                // TODO builtin attributes
                Trade::MaterialAttributeData{"baseColorTextureLayer",
                    material.texIdxs[0] + (configuration().value<bool>("textureArraysForceAllMaterialsTextured") ? 1 : 0)}
            });
            if(configuration().value<bool>("phongMaterialFallback")) {
                arrayAppend(attributes, {
                    Trade::MaterialAttributeData{Trade::MaterialAttribute::DiffuseTexture,
                        0u},
                    // TODO builtin attributes
                    Trade::MaterialAttributeData{"diffuseTextureLayer",
                        material.texIdxs[0] + (configuration().value<bool>("textureArraysForceAllMaterialsTextured") ? 1 : 0)}
                });
            }
        }
    } else if(material.texIdxs[0] != ~UnsignedInt{}) {
        arrayAppend(attributes, InPlaceInit,
            Trade::MaterialAttribute::BaseColorTexture,
            material.texIdxs[0]);
        if(configuration().value<bool>("phongMaterialFallback"))
            arrayAppend(attributes, InPlaceInit,
                Trade::MaterialAttribute::DiffuseTexture,
                material.texIdxs[0]);
    }

    arrayShrink(attributes, DefaultInit); // TODO allow growable deleters

    Trade::MaterialTypes types = Trade::MaterialType::Flat|Trade::MaterialType::PbrMetallicRoughness;
    if(configuration().value<bool>("phongMaterialFallback"))
        types |= Trade::MaterialType::Phong;
    return Trade::MaterialData{types, std::move(attributes)};
}

UnsignedInt BpsImporter::doTextureCount() const {
    return configuration().value<bool>("textureArrays") ? 1 : _textureNames.size();
}

Containers::Optional<Trade::TextureData> BpsImporter::doTexture(const UnsignedInt id) {
    return Trade::TextureData{
        configuration().value<bool>("textureArrays") ?
            Trade::TextureType::Texture2DArray : Trade::TextureType::Texture2D,
        SamplerFilter::Linear,
        SamplerFilter::Linear,
        SamplerMipmap::Linear,
        SamplerWrapping::ClampToEdge,
        configuration().value<bool>("textureArrays") ?
            0 : id
    };
}

UnsignedInt BpsImporter::doImage2DCount() const {
    return configuration().value<bool>("textureArrays") ? 0 : _textureNames.size();
}

UnsignedInt BpsImporter::doImage2DLevelCount(UnsignedInt) {
    // TODO
    return 1;
}

Containers::Optional<Trade::ImageData2D> BpsImporter::doImage2D(const UnsignedInt id, const UnsignedInt level) {
    // TODO reuse the importer, let config be propagated to it from global plugin mgr (how???)

    Trade::BasisImporter importer;
    importer.configuration().setValue("format", configuration().value("basisFormat"));
    if(!importer.openFile(Utility::Path::join({_basename, _textureDir, _textureNames[id]})))
        return {};
    return importer.image2D(0, level);
}

UnsignedInt BpsImporter::doImage3DCount() const {
    return configuration().value<bool>("textureArrays") ? 1 : 0;
}

UnsignedInt BpsImporter::doImage3DLevelCount(UnsignedInt) {
    return Math::log2(configuration().value<Int>("textureArrayMaxLevelSize")) + 1;
}

Containers::Optional<Trade::ImageData3D> BpsImporter::doImage3D(const UnsignedInt, const UnsignedInt level) {
    const Int maxLevelSize = configuration().value<Int>("textureArrayMaxLevelSize");
    if(Math::popcount(UnsignedInt(maxLevelSize)) != 1) {
        Error{} << "BpsImporter::image3D(): the textureArrayMaxLevelSize option has to be a power of two, got" << configuration().value<Containers::StringView>("textureArrayMaxLevelSize");
        return {};
    }

    const Vector3i imageSize{Vector2i{maxLevelSize, maxLevelSize} >> level, Int(_textureNames.size() + (configuration().value<bool>("textureArraysForceAllMaterialsTextured") ? 1 : 0))};

    Trade::BasisImporter importer;
    importer.configuration().setValue("format", configuration().value("basisFormat"));

    Containers::Optional<Trade::ImageData3D> out;
    for(std::size_t i = 0; i != _textureNames.size(); ++i) {
        if(!importer.openFile(Utility::Path::join({_basename, _textureDir, _textureNames[i]})))
            return {};

        /* Assuming the last level is 1x1, pick the Nth level from the end */
        const UnsignedInt levelFromTheEndToLoad = Math::log2(maxLevelSize) - level;
        const UnsignedInt levelCount = importer.image2DLevelCount(0);
        if(levelFromTheEndToLoad >= levelCount) {
            Error{} << "BpsImporter::image3D(): image" << i << "has only" << levelCount << "levels which is not enough to get images of size" << imageSize.xy() << Debug::nospace << ", ignoring";
            continue;
        }

        /* Import desired level */
        Containers::Optional<Trade::ImageData2D> image = importer.image2D(0, levelCount - levelFromTheEndToLoad - 1);
        if(!image) return {};

        if(!image->isCompressed()) {
            Error{} << "BpsImporter::image3D(): expected compressed image, please set basisFormat";
            return {};
        }

        /* Allocate the data as appropriate once we know the format and size */
        // TODO figure out a way to know the block size beforehand
        if(!out) {
            out = Trade::ImageData3D{image->compressedFormat(), imageSize, Containers::Array<char>{ValueInit, ((imageSize + compressedBlockSize(image->compressedFormat()) - Vector3i{1})/compressedBlockSize(image->compressedFormat())).product()*compressedBlockDataSize(image->compressedFormat())}};

            /* fill the first image with 1s to use for texture-less objects */
            if(configuration().value<bool>("textureArraysForceAllMaterialsTextured")) for(char& c: out->mutableData().prefix(out->data().size()/imageSize.z())) {
                c = '\xff'; // TODO ffs need an astc white block
            }
        }

        /* Copy the image data over */
        // TODO implement blocks(), sigh
        const std::size_t levelDataSize = out->data().size()/imageSize.z();
        Utility::copy(image->data(), out->mutableData().slice((i + 1)*levelDataSize, (i + 2)*levelDataSize));
    }

    return out;
}

UnsignedInt BpsImporter::doSceneCount() const {
    return configuration().value<bool>("instanceScene") ? 2 : 1;
}

UnsignedLong BpsImporter::doObjectCount() const {
    return _instanceCount + _mappings.size();
}

Containers::String BpsImporter::doObjectName(UnsignedLong id) {
    if(id < _instanceCount) return {};
    return _mappings[id - _instanceCount].name;
}

Int BpsImporter::doDefaultScene() const {
    return 0;
}

namespace {
    constexpr Trade::SceneField SceneFieldMeshView = Trade::sceneFieldCustom(0);
    constexpr Trade::SceneField SceneFieldMeshViewMaterial = Trade::sceneFieldCustom(1);
    constexpr Trade::SceneField SceneFieldMeshViewIndexOffset = Trade::sceneFieldCustom(2);
    constexpr Trade::SceneField SceneFieldMeshViewIndexCount = Trade::sceneFieldCustom(3);
}

Containers::String BpsImporter::doSceneFieldName(UnsignedInt name) {
    if(configuration().value<bool>("meshViews")) {
        switch(name) {
            #define _c(field, name) \
                case sceneFieldCustom(SceneField ## field): return #name;
            _c(MeshViewIndexOffset, meshViewIndexOffset)
            _c(MeshViewIndexCount, meshViewIndexCount)
            _c(MeshView, meshView)
            _c(MeshViewMaterial, meshViewMaterial)
            #undef _c
        }
    }

    return {};
}

Trade::SceneField BpsImporter::doSceneFieldForName(const Containers::StringView name) {
    if(configuration().value<bool>("meshViews")) {
        #define _c(name_, field) \
            if(name == #name_) return SceneField ## field;
        _c(meshViewIndexOffset, MeshViewIndexOffset)
        _c(meshViewIndexCount, MeshViewIndexCount)
        _c(meshView, MeshView)
        _c(meshViewMaterial, MeshViewMaterial)
        #undef _c
    }

    return {};
}

Containers::Optional<Trade::SceneData> BpsImporter::doScene(const UnsignedInt id) {
    const Containers::StridedArrayView1D<const BpsInstanceProperties> instances{reinterpret_cast<const BpsInstanceProperties*>(_in.data() + _instanceOffset), _instanceCount};

    if((configuration().value<bool>("instanceScene") && id == 1) ||
      (!configuration().value<bool>("instanceScene") && id == 0)) {
        if(configuration().value<bool>("meshViews")) {
            Containers::ArrayView<UnsignedInt> objects;
            Containers::StridedArrayView1D<UnsignedInt> mesh;
            Containers::ArrayView<UnsignedInt> meshIndexOffsets;
            Containers::ArrayView<UnsignedInt> meshIndexCounts;
            Containers::ArrayView<Int> materials;

            Containers::Array<char> data = Containers::ArrayTuple{
                {NoInit, _mappings.size(), objects},
                {NoInit, 1, mesh},
                {NoInit, _mappings.size(), meshIndexOffsets},
                {NoInit, _mappings.size(), meshIndexCounts},
                {NoInit, _mappings.size(), materials},
            };

            const auto* meshViews = reinterpret_cast<const BpsMeshInfo*>(_in.data() + 256);

            mesh[0] = 0;
            for(std::size_t i = 0; i != _mappings.size(); ++i) {
                objects[i] = _instanceCount + i;
                meshIndexOffsets[i] = meshViews[_mappings[i].mesh].indexOffset*4;
                meshIndexCounts[i] = meshViews[_mappings[i].mesh].numTriangles*3;
                materials[i] = _mappings[i].material;
            }

            return Trade::SceneData{Trade::SceneMappingType::UnsignedInt, _instanceCount + _mappings.size(), std::move(data), {
                Trade::SceneFieldData{Trade::SceneField::Mesh,
                    objects,
                    mesh.broadcasted<0>(_mappings.size()),
                    Trade::SceneFieldFlag::ImplicitMapping},
                Trade::SceneFieldData{SceneFieldMeshViewIndexOffset,
                    objects,
                    meshIndexOffsets,
                    Trade::SceneFieldFlag::ImplicitMapping},
                Trade::SceneFieldData{SceneFieldMeshViewIndexCount,
                    objects,
                    meshIndexCounts,
                    Trade::SceneFieldFlag::ImplicitMapping},
                Trade::SceneFieldData{SceneFieldMeshViewMaterial,
                    objects,
                    materials,
                    Trade::SceneFieldFlag::ImplicitMapping},
                /* To mark the scene as 3D */
                Trade::SceneFieldData{Trade::SceneField::Transformation,
                    Trade::SceneMappingType::UnsignedInt, nullptr,
                    Trade::SceneFieldType::Matrix4x4, nullptr}
            }};
        }

        Containers::ArrayView<UnsignedInt> objects;
        Containers::ArrayView<UnsignedInt> meshes;
        Containers::ArrayView<Int> materials;

        Containers::Array<char> data = Containers::ArrayTuple{
            {NoInit, _mappings.size(), objects},
            {NoInit, _mappings.size(), meshes},
            {NoInit, _mappings.size(), materials},
        };

        for(std::size_t i = 0; i != _mappings.size(); ++i) {
            objects[i] = _instanceCount + i;
            meshes[i] = _mappings[i].mesh;
            materials[i] = _mappings[i].material;
        }

        return Trade::SceneData{Trade::SceneMappingType::UnsignedInt, _instanceCount + _mappings.size(), std::move(data), {
            Trade::SceneFieldData{Trade::SceneField::Mesh,
                objects,
                meshes,
                Trade::SceneFieldFlag::ImplicitMapping},
            Trade::SceneFieldData{Trade::SceneField::MeshMaterial,
                objects,
                materials,
                Trade::SceneFieldFlag::ImplicitMapping},
            /* To mark the scene as 3D */
            Trade::SceneFieldData{Trade::SceneField::Transformation,
                Trade::SceneMappingType::UnsignedInt, nullptr,
                Trade::SceneFieldType::Matrix4x4, nullptr}
        }};
    }

    if(configuration().value<bool>("meshViews")) {
        Containers::ArrayView<UnsignedInt> objects;
        Containers::StridedArrayView1D<Int> parent;
        Containers::StridedArrayView1D<UnsignedInt> mesh;
        // TODO change to index offset + count also
        Containers::StridedArrayView1D<BpsInstanceProperties> instancesCopy;
        Containers::Array<char> data = Containers::ArrayTuple{
            {NoInit, _instanceCount, objects},
            {NoInit, 1, parent},
            {NoInit, 1, mesh},
            {NoInit, _instanceCount, instancesCopy},
        };

        for(std::size_t i = 0; i != _instanceCount; ++i)
            objects[i] = i;
        mesh[0] = 0;
        parent[0] = -1;
        Utility::copy(instances, instancesCopy);

        // TODO empty if no instances
        return Trade::SceneData{Trade::SceneMappingType::UnsignedInt, _instanceCount, std::move(data), {
            Trade::SceneFieldData{Trade::SceneField::Parent,
                objects,
                parent.broadcasted<0>(_instanceCount),
                Trade::SceneFieldFlag::ImplicitMapping},
            Trade::SceneFieldData{Trade::SceneField::Mesh,
                objects,
                mesh.broadcasted<0>(_instanceCount),
                Trade::SceneFieldFlag::ImplicitMapping},
            Trade::SceneFieldData{SceneFieldMeshView,
                objects,
                instancesCopy.slice(&BpsInstanceProperties::meshIndex),
                Trade::SceneFieldFlag::ImplicitMapping},
            Trade::SceneFieldData{SceneFieldMeshViewMaterial,
                objects,
                instancesCopy.slice(&BpsInstanceProperties::materialIndex),
                Trade::SceneFieldFlag::ImplicitMapping},
            Trade::SceneFieldData{Trade::SceneField::Transformation,
                objects,
                instancesCopy.slice(&BpsInstanceProperties::transformation),
                Trade::SceneFieldFlag::ImplicitMapping}
        }};
    }

    Containers::ArrayView<UnsignedInt> objects;
    Containers::StridedArrayView1D<Int> parent;
    Containers::StridedArrayView1D<BpsInstanceProperties> instancesCopy;
    Containers::Array<char> data = Containers::ArrayTuple{
        {NoInit, _instanceCount, objects},
        {NoInit, 1, parent},
        {NoInit, _instanceCount, instancesCopy},
    };

    for(std::size_t i = 0; i != _instanceCount; ++i)
        objects[i] = i;
    parent[0] = -1;
    Utility::copy(instances, instancesCopy);

    return Trade::SceneData{Trade::SceneMappingType::UnsignedInt, _instanceCount, std::move(data), {
        Trade::SceneFieldData{Trade::SceneField::Parent,
            objects,
            parent.broadcasted<0>(_instanceCount),
            Trade::SceneFieldFlag::ImplicitMapping},
        Trade::SceneFieldData{Trade::SceneField::Mesh,
            objects,
            instancesCopy.slice(&BpsInstanceProperties::meshIndex),
            Trade::SceneFieldFlag::ImplicitMapping},
        Trade::SceneFieldData{Trade::SceneField::MeshMaterial,
            objects,
            instancesCopy.slice(&BpsInstanceProperties::materialIndex),
            Trade::SceneFieldFlag::ImplicitMapping},
        Trade::SceneFieldData{Trade::SceneField::Transformation,
            objects,
            instancesCopy.slice(&BpsInstanceProperties::transformation),
            Trade::SceneFieldFlag::ImplicitMapping}
    }};
}

CORRADE_PLUGIN_REGISTER(BpsImporter, BpsImporter,
    "cz.mosra.magnum.Trade.AbstractImporter/0.5")

// kate: indent-width 4
