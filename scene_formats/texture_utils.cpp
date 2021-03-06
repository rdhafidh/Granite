/* Copyright (c) 2017-2018 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "texture_utils.hpp"

namespace Granite
{
namespace SceneFormats
{
struct MipmapGeneratorUnorm
{
	inline vec4 sample(const Vulkan::TextureFormatLayout &layout, const uvec2 &coord,
	                   uint32_t layer, uint32_t mip) const
	{
		u8vec4 &v = *layout.data_generic<u8vec4>(coord.x, coord.y, layer, mip);
		return vec4(v) * (1.0f / 255.0f);
	}

	inline void write(const Vulkan::TextureFormatLayout &layout, const uvec2 &coord,
	                  uint32_t layer, uint32_t mip, const vec4 &v) const
	{
		auto q = clamp(round(v * 255.0f), vec4(0.0f), vec4(255.0f));
		*layout.data_generic<u8vec4>(coord.x, coord.y, layer, mip) = u8vec4(q);
	}
};

struct MipmapGeneratorSrgb
{
	static inline float srgb_gamma_to_linear(float v)
	{
		if (v <= 0.04045f)
			return v * (1.0f / 12.92f);
		else
			return muglm::pow((v + 0.055f) / (1.0f + 0.055f), 2.4f);
	}

	static inline float srgb_linear_to_gamma(float v)
	{
		if (v <= 0.0031308f)
			return 12.92f * v;
		else
			return (1.0f + 0.055f) * muglm::pow(v, 1.0f / 2.4f) - 0.055f;
	}

	static inline vec4 srgb_gamma_to_linear(const vec4 &v)
	{
		return vec4(srgb_gamma_to_linear(v.x),
		            srgb_gamma_to_linear(v.y),
		            srgb_gamma_to_linear(v.z),
		            v.w);
	}

	static inline vec4 srgb_linear_to_gamma(const vec4 &v)
	{
		return vec4(srgb_linear_to_gamma(v.x),
		            srgb_linear_to_gamma(v.y),
		            srgb_linear_to_gamma(v.z),
		            v.w);
	}

	inline vec4 sample(const Vulkan::TextureFormatLayout &layout, const uvec2 &coord,
	                   uint32_t layer, uint32_t mip) const
	{
		u8vec4 &v = *layout.data_generic<u8vec4>(coord.x, coord.y, layer, mip);
		return srgb_gamma_to_linear(vec4(v) * (1.0f / 255.0f));
	}

	inline void write(const Vulkan::TextureFormatLayout &layout, const uvec2 &coord,
	                  uint32_t layer, uint32_t mip, const vec4 &v) const
	{
		auto q = clamp(round(srgb_linear_to_gamma(v) * 255.0f), vec4(0.0f), vec4(255.0f));
		*layout.data_generic<u8vec4>(coord.x, coord.y, layer, mip) = u8vec4(q);
	}
};

template <typename Ops>
inline void generate_mipmaps(const Vulkan::TextureFormatLayout &dst_layout,
                             const Vulkan::TextureFormatLayout &layout, const Ops &op)
{
	memcpy(dst_layout.data(0, 0), layout.data(0, 0), dst_layout.get_layer_size(0) * layout.get_layers());

	for (uint32_t level = 1; level < dst_layout.get_levels(); level++)
	{
		auto &dst_mip = dst_layout.get_mip_info(level);
		auto &src_mip = dst_layout.get_mip_info(level - 1);

		uint32_t dst_width = dst_mip.block_row_length;
		uint32_t dst_height = dst_mip.block_image_height;

		uint32_t src_width = src_mip.block_row_length;
		uint32_t src_height = src_mip.block_image_height;
		uvec2 max_coord(src_width - 1u, src_height - 1u);

		float src_width_f = float(src_mip.block_row_length);
		float src_height_f = float(src_mip.block_image_height);

		float rescale_width = src_width_f / float(dst_width);
		float rescale_height = src_height_f / float(dst_height);

		for (uint32_t layer = 0; layer < dst_layout.get_layers(); layer++)
		{
			for (uint32_t y = 0; y < dst_height; y++)
			{
				float coord_y = (float(y) + 0.5f) * rescale_height - 0.5f;
				for (uint32_t x = 0; x < dst_width; x++)
				{
					float coord_x = (float(x) + 0.5f) * rescale_width - 0.5f;
					vec2 base_coord = vec2(coord_x, coord_y);
					vec2 floor_coord = floor(base_coord);
					vec2 uv = base_coord - floor_coord;
					uvec2 c0(floor_coord);
					uvec2 c1 = min(c0 + uvec2(1, 0), max_coord);
					uvec2 c2 = min(c0 + uvec2(0, 1), max_coord);
					uvec2 c3 = min(c0 + uvec2(1, 1), max_coord);

					auto v0 = op.sample(dst_layout, c0, layer, level - 1);
					auto v1 = op.sample(dst_layout, c1, layer, level - 1);
					auto v2 = op.sample(dst_layout, c2, layer, level - 1);
					auto v3 = op.sample(dst_layout, c3, layer, level - 1);

					auto x0 = mix(v0, v1, uv.x);
					auto x1 = mix(v2, v3, uv.x);
					auto filtered = mix(x0, x1, uv.y);
					op.write(dst_layout, uvec2(x, y), layer, level, filtered);
				}
			}
		}
	}
}

static void copy_dimensions(MemoryMappedTexture &mapped, const Vulkan::TextureFormatLayout &layout, MemoryMappedTextureFlags flags)
{
	switch (layout.get_image_type())
	{
	case VK_IMAGE_TYPE_1D:
		mapped.set_1d(layout.get_format(), layout.get_width(), layout.get_layers(), 0);
		break;

	case VK_IMAGE_TYPE_2D:
		if (flags & MEMORY_MAPPED_TEXTURE_CUBE_MAP_COMPATIBLE_BIT)
			mapped.set_cube(layout.get_format(), layout.get_width(), layout.get_layers() / 6, 0);
		else
			mapped.set_2d(layout.get_format(), layout.get_width(), layout.get_height(), layout.get_layers(), 0);
		break;

	case VK_IMAGE_TYPE_3D:
		throw std::logic_error("3D is not supported for generate_mipmaps.");

	default:
		throw std::logic_error("Unknown image type.");
	}
}

static void generate(const MemoryMappedTexture &mapped, const Vulkan::TextureFormatLayout &layout)
{
	auto &dst_layout = mapped.get_layout();

	switch (layout.get_format())
	{
	case VK_FORMAT_R8G8B8A8_SRGB:
	case VK_FORMAT_B8G8R8A8_SRGB:
		generate_mipmaps(dst_layout, layout, MipmapGeneratorSrgb());
		break;

	case VK_FORMAT_R8G8B8A8_UNORM:
	case VK_FORMAT_B8G8R8A8_UNORM:
		generate_mipmaps(dst_layout, layout, MipmapGeneratorUnorm());
		break;

	default:
		throw std::logic_error("Unsupported format for generate_mipmaps.");
	}
}

MemoryMappedTexture generate_mipmaps_to_file(const std::string &path, const Vulkan::TextureFormatLayout &layout, MemoryMappedTextureFlags flags)
{
	MemoryMappedTexture mapped;
	copy_dimensions(mapped, layout, flags);
	if (!mapped.map_write(path))
		return {};
	generate(mapped, layout);
	return mapped;
}

MemoryMappedTexture generate_mipmaps(const Vulkan::TextureFormatLayout &layout, MemoryMappedTextureFlags flags)
{
	MemoryMappedTexture mapped;
	copy_dimensions(mapped, layout, flags);
	if (!mapped.map_write_scratch())
		return {};
	generate(mapped, layout);
	return mapped;
}
}
}