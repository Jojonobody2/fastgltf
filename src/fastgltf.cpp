#include <array>
#include <fstream>
#include <functional>
#include <utility>

#include "simdjson.h"

#include "fastgltf_parser.hpp"
#include "fastgltf_types.hpp"
#include "base64_decode.hpp"

namespace fg = fastgltf;
namespace fs = std::filesystem;

namespace fastgltf {
    constexpr std::string_view mimeTypeJpeg = "image/jpeg";
    constexpr std::string_view mimeTypePng = "image/png";
    constexpr std::string_view mimeTypeKtx = "image/ktx2";
    constexpr std::string_view mimeTypeDds = "image/vnd-ms.dds";
    constexpr std::string_view mimeTypeGltfBuffer = "application/gltf-buffer";
    constexpr std::string_view mimeTypeOctetStream = "application/octet-stream";

    struct ParserData {
        // Can simdjson not store this data itself?
        std::vector<uint8_t> bytes;
        simdjson::dom::document doc;
        simdjson::dom::object root;
    };

    [[nodiscard]] std::tuple<bool, bool, size_t> getImageIndexForExtension(simdjson::dom::object& object, std::string_view extension);
    [[nodiscard]] bool parseTextureExtensions(fastgltf::Texture& texture, simdjson::dom::object& extensions, fastgltf::Options options);

    [[nodiscard]] std::pair<bool, Error> iterateOverArray(simdjson::dom::object& parent, std::string_view arrayName, const std::function<bool(simdjson::dom::element&)>& callback);
}

std::tuple<bool, bool, size_t> fg::getImageIndexForExtension(simdjson::dom::object& object, std::string_view extension) {
    using namespace simdjson;

    // Both KHR_texture_basisu and MSFT_texture_dds allow specifying an alternative
    // image source index.
    dom::object sourceExtensionObject;
    if (object[extension].get_object().get(sourceExtensionObject) != SUCCESS) {
        return std::make_tuple(false, true, 0);
    }

    // Check if the extension object provides a source index.
    size_t imageIndex;
    if (sourceExtensionObject["source"].get_uint64().get(imageIndex) != SUCCESS) {
        return std::make_tuple(true, false, 0);
    }

    return std::make_tuple(false, false, imageIndex);
};

std::pair<bool, fg::Error> fg::iterateOverArray(simdjson::dom::object& parent, std::string_view arrayName,
                                                 const std::function<bool(simdjson::dom::element&)>& callback) {
    using namespace simdjson;

    dom::array array;
    if (parent[arrayName].get_array().get(array) != SUCCESS) {
        return std::make_pair(false, fg::Error::None);
    }

    /*size_t count = 0;
    if (array.count_elements().get(count) != SUCCESS) {
        return std::make_tuple(false, 0, fg::Error::InvalidJson);
    }*/

    for (auto field : array) {
        if (!callback(field)) {
            return std::make_pair(false, fg::Error::InvalidGltf);
        }
    }

    return std::make_pair(true, fg::Error::None);
}

bool fg::parseTextureExtensions(fastgltf::Texture& texture, simdjson::dom::object& extensions, fastgltf::Options options) {
    if (hasBit(options, fastgltf::Options::LoadKTXExtension)) {
        auto [invalidGltf, extensionNotPresent, imageIndex] = getImageIndexForExtension(extensions, "KHR_texture_basisu");
        if (invalidGltf) {
            return false;
        }

        if (!extensionNotPresent) {
            texture.imageIndex = imageIndex;
            return true;
        }
    }

    if (hasBit(options, fastgltf::Options::LoadDDSExtension)) {
        auto [invalidGltf, extensionNotPresent, imageIndex] = getImageIndexForExtension(extensions, "MSFT_texture_dds");
        if (invalidGltf) {
            return false;
        }

        if (!extensionNotPresent) {
            texture.imageIndex = imageIndex;
            return true;
        }
    }

    return false;
}

#pragma region glTF
fg::glTF::glTF(std::unique_ptr<fastgltf::ParserData> data, std::filesystem::path directory, Options options) : data(std::move(data)), directory(std::move(directory)), options(options) {
    parsedAsset = std::make_unique<Asset>();
}

// We define the destructor here as otherwise the definition would be generated in other cpp files
// in which the definition for ParserData is not available.
fg::glTF::~glTF() = default;

bool fg::glTF::checkAssetField() {
    using namespace simdjson;

    dom::object asset;
    if (data->root["asset"].get_object().get(asset) != SUCCESS) {
        errorCode = Error::InvalidOrMissingAssetField;
        return false;
    }

    std::string_view version;
    if (asset["version"].get_string().get(version) != SUCCESS) {
        errorCode = Error::InvalidOrMissingAssetField;
        return false;
    }

    return true;
}

std::tuple<fg::Error, fg::DataSource, fg::DataLocation> fg::glTF::decodeUri(std::string_view uri) const {
    if (uri.substr(0, 4) == "data") {
        // This is a data URI.
        auto index =  uri.find(';');
        auto encodingEnd = uri.find(',', index + 1);
        if (index == std::string::npos || encodingEnd == std::string::npos) {
            return std::make_tuple(fg::Error::InvalidGltf, fg::DataSource {}, fg::DataLocation::None);
        }

        auto encoding = uri.substr(index + 1, encodingEnd - index - 1);
        if (encoding != "base64") {
            return std::make_tuple(fg::Error::InvalidGltf, fg::DataSource {}, fg::DataLocation::None);
        }

        // Decode the base64 data.
        auto encodedData = uri.substr(encodingEnd);
        std::vector<uint8_t> uriData;
        if (hasBit(options, Options::DontUseSIMD)) {
            uriData = base64::fallback_decode(encodedData);
        } else {
            uriData = base64::decode(encodedData);
        }

        fg::DataSource source = {};
        source.mimeType = getMimeTypeFromString(uri.substr(5, index - 5));
        source.bytes = std::move(uriData);
        return std::make_tuple(Error::None, source, fg::DataLocation::VectorWithMime);
    } else {
        fg::DataSource source = {};
        source.path = directory / uri;
        return std::make_tuple(Error::None, source, fg::DataLocation::FilePathWithByteRange);
    }
}

fastgltf::MimeType fg::glTF::getMimeTypeFromString(std::string_view mime) {
    if (mime == mimeTypeJpeg) {
        return MimeType::JPEG;
    } else if (mime == mimeTypePng) {
        return MimeType::PNG;
    } else if (mime == mimeTypeKtx) {
        return MimeType::KTX2;
    } else if (mime == mimeTypeDds) {
        return MimeType::DDS;
    } else if (mime == mimeTypeGltfBuffer) {
        return MimeType::GltfBuffer;
    } else if (mime == mimeTypeOctetStream) {
        return MimeType::OctetStream;
    } else {
        return MimeType::None;
    }
}

std::unique_ptr<fg::Asset> fg::glTF::getParsedAsset() {
    // If there has been any errors we don't want the caller to get the partially parsed asset.
    if (errorCode != Error::None) {
        return nullptr;
    }
    return std::move(parsedAsset);
}

fg::Asset* fg::glTF::getParsedAssetPointer() {
    if (errorCode != Error::None) {
        return nullptr;
    }
    return parsedAsset.get();
}

fg::Error fg::glTF::parseBuffers() {
    using namespace simdjson;
    auto [foundBuffers, bufferError] = iterateOverArray(data->root, "buffers", [this](auto& value) mutable -> bool {
        // Required fields: "byteLength"
        Buffer buffer = {};
        dom::object bufferObject;
        if (value.get_object().get(bufferObject) != SUCCESS) {
            return false;
        }

        if (bufferObject["byteLength"].get_uint64().get(buffer.byteLength) != SUCCESS) {
            return false;
        }

        // When parsing GLB, there's a buffer object that will point to the BUF chunk in the
        // file. Otherwise, data must be specified in the "uri" field.
        std::string_view uri;
        if (bufferObject["uri"].get_string().get(uri) == SUCCESS) {
            auto [error, source, location] = decodeUri(uri);
            if (error != Error::None)
                return false;

            buffer.data = source;
            buffer.location = location;
        }

        if (buffer.location == DataLocation::None) {
            return false;
        }

        // name is optional.
        std::string_view name;
        if (bufferObject["name"].get_string().get(name) == SUCCESS) {
            buffer.name = std::string { name };
        }

        parsedAsset->buffers.emplace_back(std::move(buffer));
        return true;
    });

    if (!foundBuffers && bufferError != Error::None) {
        errorCode = bufferError;
    }

    return errorCode;
}

fg::Error fg::glTF::parseBufferViews() {
    using namespace simdjson;
    auto [foundBufferViews, bufferViewError] = iterateOverArray(data->root, "bufferViews", [this](auto& value) mutable -> bool {
        // Required fields: "bufferIndex", "byteLength"
        BufferView view = {};
        dom::object bufferViewObject;
        if (value.get_object().get(bufferViewObject) != SUCCESS) {
            return false;
        }

        // Required with normal glTF, not necessary with GLB files.
        if (bufferViewObject["buffer"].get_uint64().get(view.bufferIndex) != SUCCESS) {
            return false;
        }

        if (bufferViewObject["byteLength"].get_uint64().get(view.byteLength) != SUCCESS) {
            return false;
        }

        // byteOffset is optional, but defaults to 0
        if (bufferViewObject["byteOffset"].get_uint64().get(view.byteOffset) != SUCCESS) {
            view.byteOffset = 0;
        }

        size_t byteStride;
        if (bufferViewObject["byteStride"].get_uint64().get(byteStride) == SUCCESS) {
            view.byteStride = byteStride;
        }

        // target is optional
        size_t target;
        if (bufferViewObject["target"].get_uint64().get(target) == SUCCESS) {
            view.target = static_cast<BufferTarget>(target);
        }

        // name is optional.
        std::string_view name;
        if (bufferViewObject["name"].get_string().get(name) == SUCCESS) {
            view.name = std::string { name };
        }

        parsedAsset->bufferViews.emplace_back(std::move(view));
        return true;
    });

    if (!foundBufferViews && bufferViewError != Error::None) {
        errorCode = bufferViewError;
    }
    return errorCode;
}

fg::Error fg::glTF::parseAccessors() {
    using namespace simdjson;
    auto [foundAccessors, accessorError] = iterateOverArray(data->root, "accessors", [this](auto& value) mutable -> bool {
        // Required fields: "componentType", "count"
        Accessor accessor = {};
        dom::object accessorObject;
        if (value.get_object().get(accessorObject) != SUCCESS) {
            return false;
        }

        size_t componentType;
        if (accessorObject["componentType"].get_uint64().get(componentType) != SUCCESS) {
            return false;
        } else {
            accessor.componentType = getComponentType(static_cast<std::underlying_type_t<ComponentType>>(componentType));
            if (accessor.componentType == ComponentType::Double && !hasBit(options, Options::AllowDouble)) {
                return false;
            }
        }

        std::string_view accessorType;
        if (accessorObject["type"].get_string().get(accessorType) != SUCCESS) {
            return false;
        } else {
            accessor.type = getAccessorType(accessorType);
        }

        if (accessorObject["count"].get_uint64().get(accessor.count) != SUCCESS) {
            return false;
        }

        size_t bufferView;
        if (accessorObject["bufferView"].get_uint64().get(bufferView) == SUCCESS) {
            accessor.bufferViewIndex = bufferView;
        }

        // byteOffset is optional, but defaults to 0
        if (accessorObject["byteOffset"].get_uint64().get(accessor.byteOffset) != SUCCESS) {
            accessor.byteOffset = 0;
        }

        if (accessorObject["normalized"].get_bool().get(accessor.normalized) != SUCCESS) {
            accessor.normalized = false;
        }

        // name is optional.
        std::string_view name;
        if (accessorObject["name"].get_string().get(name) == SUCCESS) {
            accessor.name = std::string { name };
        }

        parsedAsset->accessors.emplace_back(std::move(accessor));
        return true;
    });

    if (!foundAccessors && accessorError != Error::None) {
        errorCode = accessorError;
    }
    return errorCode;
}

fg::Error fg::glTF::parseImages() {
    using namespace simdjson;
    auto [foundImages, imageError] = iterateOverArray(data->root, "images", [this](auto& value) mutable -> bool {
        Image image;
        dom::object imageObject;
        if (value.get_object().get(imageObject) != SUCCESS) {
            return false;
        }

        std::string_view uri;
        if (imageObject["uri"].get_string().get(uri) == SUCCESS) {
            if (imageObject["bufferView"].error() == SUCCESS) {
                // If uri is declared, bufferView cannot be declared.
                return false;
            }
            auto [error, source, location] = decodeUri(uri);
            if (error != Error::None)
                return false;

            image.data = source;
            image.location = location;

            std::string_view mimeType;
            if (imageObject["mimeType"].get_string().get(mimeType) == SUCCESS) {
                image.data.mimeType = getMimeTypeFromString(mimeType);
            }
        }

        size_t bufferViewIndex;
        if (imageObject["bufferView"].get_uint64().get(bufferViewIndex) == SUCCESS) {
            std::string_view mimeType;
            if (imageObject["mimeType"].get_string().get(mimeType) != SUCCESS) {
                // If bufferView is defined, mimeType needs to also be defined.
                return false;
            }

            image.data.bufferViewIndex = bufferViewIndex;
            image.data.mimeType = getMimeTypeFromString(mimeType);
        }

        if (image.location == DataLocation::None) {
            return false;
        }

        // name is optional.
        std::string_view name;
        if (imageObject["name"].get_string().get(name) == SUCCESS) {
            image.name = std::string{name};
        }

        parsedAsset->images.emplace_back(std::move(image));
        return true;
    });

    if (!foundImages && imageError != fg::Error::None) {
        errorCode = imageError;
    }
    return errorCode;
}

fg::Error fg::glTF::parseTextures() {
    using namespace simdjson;
    auto [foundTextures, textureError] = iterateOverArray(data->root, "textures", [this](auto& value) mutable -> bool {
        Texture texture;
        dom::object textureObject;
        if (value.get_object().get(textureObject) != SUCCESS) {
            return false;
        }

        bool hasExtensions = false;
        dom::object extensionsObject;
        if (textureObject["extensions"].get_object().get(extensionsObject) == SUCCESS) {
            hasExtensions = true;
        }

        texture.imageIndex = std::numeric_limits<size_t>::max();
        if (textureObject["source"].get_uint64().get(texture.imageIndex) != SUCCESS && !hasExtensions) {
            // "The index of the image used by this texture. When undefined, an extension or other
            // mechanism SHOULD supply an alternate texture source, otherwise behavior is undefined."
            // => We'll have it be invalid.
            return false;
        }

        // If we have extensions, we'll use the normal "source" as the fallback and then parse
        // the extensions for any "source" field.
        if (hasExtensions) {
            if (texture.imageIndex != std::numeric_limits<size_t>::max()) {
                // If the source was specified we'll use that as a fallback.
                texture.fallbackImageIndex = texture.imageIndex;
            }
            if (!parseTextureExtensions(texture, extensionsObject, options)) {
                return false;
            }
        }

        // The index of the sampler used by this texture. When undefined, a sampler with
        // repeat wrapping and auto filtering SHOULD be used.
        size_t samplerIndex;
        if (textureObject["sampler"].get_uint64().get(samplerIndex) == SUCCESS) {
            texture.samplerIndex = samplerIndex;
        }

        // name is optional.
        std::string_view name;
        if (textureObject["name"].get_string().get(name) == SUCCESS) {
            texture.name = std::string { name };
        }

        parsedAsset->textures.emplace_back(std::move(texture));
        return true;
    });

    if (!foundTextures && textureError != fg::Error::None) {
        errorCode = textureError;
    }
    return errorCode;
}

fg::Error fg::glTF::parseMeshes() {
    using namespace simdjson;
    auto [foundMeshes, meshError] = iterateOverArray(data->root, "meshes", [this](auto& value) mutable -> bool {
        // Required fields: "primitives"
        Mesh mesh;
        dom::object meshObject;
        if (value.get_object().get(meshObject) != SUCCESS) {
            return false;
        }

        auto [foundPrimitives, primitiveError] = iterateOverArray(meshObject, "primitives", [&primitives = mesh.primitives](auto& value) mutable -> bool {
            using namespace simdjson; // Why MSVC?
            // Required fields: "attributes"
            Primitive primitive = {};
            dom::object primitiveObject;
            if (value.get_object().get(primitiveObject) != SUCCESS) {
                return false;
            }

            {
                dom::object attributesObject;
                if (primitiveObject["attributes"].get_object().get(attributesObject) != SUCCESS) {
                    return false;
                }

                // We iterate through the JSON object and write each key/pair value into the
                // attributes map. This is not filtered for actual values. TODO?
                for (auto field : attributesObject) {
                    std::string_view key = field.key;

                    auto& attribute = primitive.attributes[std::string { key }];
                    if (field.value.get_uint64().get(attribute) != SUCCESS) {
                        return false;
                    }
                }
            }

            {
                // Mode shall default to 4.
                uint64_t mode;
                if (primitiveObject["mode"].get_uint64().get(mode) != SUCCESS) {
                    mode = 4;
                }
                primitive.type = static_cast<PrimitiveType>(mode);
            }

            size_t indicesAccessor;
            if (primitiveObject["indices"].get_uint64().get(indicesAccessor) == SUCCESS) {
                primitive.indicesAccessor = indicesAccessor;
            }

            size_t materialIndex;
            if (primitiveObject["material"].get_uint64().get(materialIndex) == SUCCESS) {
                primitive.materialIndex = materialIndex;
            }

            primitives.emplace_back(std::move(primitive));
            return true;
        });

        if (!foundPrimitives && primitiveError != Error::None) {
            errorCode = Error::InvalidGltf;
            return false;
        }

        // name is optional.
        std::string_view name;
        if (meshObject["name"].get_string().get(name) == SUCCESS) {
            mesh.name = std::string { name };
        }

        parsedAsset->meshes.emplace_back(std::move(mesh));
        return true;
    });

    if (!foundMeshes && meshError != fg::Error::None) {
        errorCode = meshError;
    }
    return errorCode;
}

fg::Error fg::glTF::parseNodes() {
    using namespace simdjson;
    auto [foundNodes, nodeError] = iterateOverArray(data->root, "nodes", [this](auto& value) -> bool {
        Node node = {};
        dom::object nodeObject;
        if (value.get_object().get(nodeObject) != SUCCESS) {
            return false;
        }

        size_t meshIndex;
        if (nodeObject["mesh"].get_uint64().get(meshIndex) == SUCCESS) {
            node.meshIndex = meshIndex;
        }

        auto [foundChildren, childrenError] = iterateOverArray(nodeObject, "children", [&node](auto& value) -> bool {
            size_t index;
            if (value.get_uint64().get(index) != SUCCESS) {
                return false;
            }

            node.children.emplace_back(index);
            return true;
        });

        if (foundChildren && childrenError != Error::None) {
            return false;
        }

        dom::array matrix;
        if (nodeObject["matrix"].get_array().get(matrix) == SUCCESS) {
            node.hasMatrix = true;
            auto i = 0U;
            for (auto num : matrix) {
                double val;
                if (num.get_double().get(val) != SUCCESS) {
                    node.hasMatrix = false;
                    break;
                }
                node.matrix[i] = static_cast<float>(val);
            }
        }

        // name is optional.
        {
            std::string_view name;
            if (nodeObject["name"].get_string().get(name) == SUCCESS) {
                node.name = std::string { name };
            }
        }

        parsedAsset->nodes.emplace_back(std::move(node));
        return true;
    });

    if (!foundNodes && nodeError != fg::Error::None) {
        errorCode = nodeError;
    }
    return errorCode;
}

fg::Error fg::glTF::parseScenes() {
    using namespace simdjson;

    size_t defaultScene;
    if (data->root["scene"].get_uint64().get(defaultScene) == SUCCESS) {
        parsedAsset->defaultScene = defaultScene;
    }

    auto [foundScenes, sceneError] = iterateOverArray(data->root, "scenes", [this](auto& value) -> bool {
        // The scene object can be completely empty
        Scene scene = {};
        dom::object sceneObject;
        if (value.get_object().get(sceneObject) != SUCCESS) {
            return false;
        }

        // name is optional.
        std::string_view name;
        if (sceneObject["name"].get_string().get(name) == SUCCESS) {
            scene.name = std::string { name };
        }

        // Parse the array of nodes.
        auto [foundNodes, nodeError] = iterateOverArray(sceneObject, "nodes", [&indices = scene.nodeIndices](auto& value) mutable -> bool {
            size_t index;
            if (value.get_uint64().get(index) != SUCCESS) {
                return false;
            }

            indices.push_back(index);
            return true;
        });

        if (!foundNodes && nodeError != fg::Error::None) {
            errorCode = nodeError;
            return false;
        }

        parsedAsset->scenes.emplace_back(std::move(scene));
        return true;
    });

    // No error means the array has just not been found. However, it's optional, but the spec
    // still requires we parse everything else.
    if (!foundScenes && sceneError != Error::None) {
        errorCode = sceneError;
    }
    return errorCode;
}
#pragma endregion

#pragma region JsonData
fg::JsonData::JsonData(uint8_t* bytes, size_t byteCount) noexcept {
    using namespace simdjson;
    data = std::make_unique<padded_string>(reinterpret_cast<char*>(bytes), byteCount);
}

fg::JsonData::JsonData(std::filesystem::path path) noexcept {
    using namespace simdjson;
    data = std::make_unique<padded_string>();
    if (padded_string::load(path.string()).get(*data) != SUCCESS) {
        // Not sure?
    }
}

fg::JsonData::~JsonData() = default;

const uint8_t* fg::JsonData::getData() const {
    return data->u8data();
}
#pragma endregion

#pragma region Parser
fg::Parser::Parser() noexcept {
    jsonParser = new simdjson::dom::parser();
}

fg::Parser::~Parser() {
    delete static_cast<simdjson::dom::parser*>(jsonParser);
}

fg::Error fg::Parser::getError() const {
    return errorCode;
}

std::unique_ptr<fg::glTF> fg::Parser::loadGLTF(JsonData* jsonData, fs::path directory, Options options) {
    using namespace simdjson;

    if (!fs::is_directory(directory)) {
        errorCode = Error::InvalidPath;
        return nullptr;
    }

    errorCode = Error::None;

    auto data = std::make_unique<ParserData>();
    auto* parser = static_cast<dom::parser*>(jsonParser);

    if (hasBit(options, Options::DontUseSIMD)) {
        simdjson::get_active_implementation() = simdjson::get_available_implementations()["fallback"];
    }

    if (parser->parse(*jsonData->data).get(data->root) != SUCCESS) {
        errorCode = Error::InvalidJson;
        return nullptr;
    }

    auto gltf = std::unique_ptr<glTF>(new glTF(std::move(data), std::move(directory), options));
    if (!hasBit(options, Options::DontRequireValidAssetMember) && !gltf->checkAssetField()) {
        errorCode = Error::InvalidOrMissingAssetField;
        return nullptr;
    }
    return gltf;
}

std::unique_ptr<fg::glTF> fg::Parser::loadGLTF(JsonData* jsonData, std::string_view directory, Options options) {
    fs::path parsed = { directory };
    if (parsed.empty() || !fs::is_directory(parsed)) {
        errorCode = Error::InvalidPath;
        return nullptr;
    }
    return loadGLTF(jsonData, parsed, options);
}
#pragma endregion
