#include "d3d12_utils.hpp"

namespace nova::renderer::rhi {
    void set_object_name(ID3D12Object* object, const rx::string& name) {
        const auto wide_name = name.to_utf16();

        object->SetName(reinterpret_cast<LPCWSTR>(wide_name.data()));
    }

    D3D12_FILTER to_d3d12_filter(const TextureFilter min_filter, const TextureFilter mag_filter) {
        switch(min_filter) {
            case TextureFilter::Point: {
                switch(mag_filter) {
                    case TextureFilter::Point:
                        return D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;

                    case TextureFilter::Bilinear:
                        return D3D12_FILTER_COMPARISON_MIN_POINT_MAG_LINEAR_MIP_POINT;

                    case TextureFilter::Trilinear:
                        return D3D12_FILTER_COMPARISON_ANISOTROPIC;
                }
            } break;

            case TextureFilter::Bilinear: {
                switch(mag_filter) {
                    case TextureFilter::Point:
                        return D3D12_FILTER_COMPARISON_MIN_LINEAR_MAG_POINT_MIP_LINEAR;

                    case TextureFilter::Bilinear:
                        return D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;

                    case TextureFilter::Trilinear:
                        return D3D12_FILTER_COMPARISON_ANISOTROPIC;
                }
            } break;

            case TextureFilter::Trilinear:
                return D3D12_FILTER_COMPARISON_ANISOTROPIC;
        }

        // If nothing else works cause someone passed in an invalid enum value for some stupid reason, you get a linear filter and you get
        // to like it
        return D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
    }

    D3D12_TEXTURE_ADDRESS_MODE to_d3d12_address_mode(const TextureCoordWrapMode wrap_mode) {
        switch(wrap_mode) {
            case TextureCoordWrapMode::Repeat:
                return D3D12_TEXTURE_ADDRESS_MODE_WRAP;

            case TextureCoordWrapMode::MirroredRepeat:
                return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;

            case TextureCoordWrapMode::ClampToEdge:
                return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

            case TextureCoordWrapMode::ClampToBorder:
                return D3D12_TEXTURE_ADDRESS_MODE_BORDER;

            case TextureCoordWrapMode::MirrorClampToEdge:
                return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;

            default:
                return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        }
    }

    DXGI_FORMAT to_dxgi_format(const PixelFormat format) {
        switch(format) {
            case PixelFormat::Rgba16F:
                return DXGI_FORMAT_R16G16B16A16_FLOAT;

            case PixelFormat::Rgba32F:
                return DXGI_FORMAT_R32G32B32A32_FLOAT;

            case PixelFormat::Depth32:
                return DXGI_FORMAT_D32_FLOAT;

            case PixelFormat::Depth24Stencil8:
                return DXGI_FORMAT_D24_UNORM_S8_UINT;

            case PixelFormat::Rgba8:
                [[fallthrough]];
            default:
                return DXGI_FORMAT_R8G8B8A8_UNORM;
        }
    }
} // namespace nova::renderer::rhi