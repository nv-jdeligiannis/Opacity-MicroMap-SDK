/*
Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.

NVIDIA CORPORATION and its licensors retain all intellectual property
and proprietary rights in and to this software, related documentation
and any modifications thereto. Any use, reproduction, disclosure or
distribution of this software and related documentation without an express
license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

/*
CREDITS:
	Developed by:

	Special thanks:
*/

#include <random>

#include <benchmark/benchmark.h>
#include <omm.h>
#include <shared/bird.h>

class OMMBake : public benchmark::Fixture {
protected:
	void SetUp(const ::benchmark::State& state) override {
		omm::CreateOpacityMicromapBaker({ .type = omm::BakerType::CPU }, &_baker);

		omm::Cpu::TextureFlags flags = (omm::Cpu::TextureFlags)state.range(0);
		_extraBakeFlags = (omm::Cpu::BakeFlags)state.range(1);

		uint32_t w = 1024 * 3;
		uint32_t h = 1024 * 3;


		uint32_t seed = 32;
		std::default_random_engine eng(seed);
		std::uniform_real_distribution<float> distr(0.f, 1.f);

		std::vector<float> texture(w * h);
		for (uint32_t j = 0; j < h; ++j)
		{
			for (uint32_t i = 0; i < w; ++i)
			{
				float val = distr(eng);
				texture.push_back(val);
			}
		}

		omm::Cpu::TextureMipDesc mip;
		mip.width = w;
		mip.height = h;
		mip.textureData = (uint8_t*)texture.data();

		omm::Cpu::TextureDesc desc;
		desc.format = omm::Cpu::TextureFormat::FP32;
		desc.mipCount = 1;
		desc.mips = &mip;
		desc.flags = flags;

		uint32_t idxCount = 512 * 8;
		_indices.resize(idxCount);
		_texCoords.resize(idxCount);
		for (uint32_t i = 0; i < idxCount; ++i) {
			_indices[i] = i;
			_texCoords[i] = float2(distr(eng), distr(eng));
		}

		omm::Cpu::CreateTexture(_baker, desc, &_texture);
	}

	void TearDown(const ::benchmark::State& state) override {
		omm::Cpu::DestroyTexture(_baker, _texture);
		omm::DestroyOpacityMicromapBaker(_baker);
	}

	void RunVmBake(benchmark::State& st, bool parallel, omm::TextureFilterMode filter) {

		st.PauseTiming();
		float alphaCutoff = 0.4f;
		uint32_t subdivisionLevel = 7;

		omm::Cpu::BakeInputDesc desc;
		desc.texture = _texture;
		desc.alphaMode = omm::AlphaMode::Test;
		desc.runtimeSamplerDesc.addressingMode = omm::TextureAddressMode::Clamp;
		desc.runtimeSamplerDesc.filter = omm::TextureFilterMode::Nearest;
		desc.indexFormat = omm::IndexFormat::I32_UINT;
		desc.indexBuffer = _indices.data();
		desc.texCoords = _texCoords.data();
		desc.texCoordFormat = omm::TexCoordFormat::UV32_FLOAT;
		desc.indexCount = (uint32_t)_indices.size();
		desc.maxSubdivisionLevel = subdivisionLevel;
		desc.alphaCutoff = alphaCutoff;
		(uint32_t&)desc.bakeFlags |= (uint32_t)omm::Cpu::BakeFlags::DisableSpecialIndices;
		(uint32_t&)desc.bakeFlags |= (uint32_t)omm::Cpu::BakeFlags::DisableDuplicateDetection;
		(uint32_t&)desc.bakeFlags |= (uint32_t)omm::Cpu::BakeFlags::Force32BitIndices;
		(uint32_t&)desc.bakeFlags |= (uint32_t)_extraBakeFlags;
		if (parallel)
			(uint32_t&)desc.bakeFlags |= (uint32_t)omm::Cpu::BakeFlags::EnableInternalThreads;
		st.ResumeTiming();

		omm::Cpu::BakeResult res = 0;
		omm::Cpu::BakeOpacityMicromap(_baker, desc, &res);

		st.PauseTiming();
		const omm::Cpu::BakeResultDesc* resDesc = nullptr;
		omm::Cpu::GetBakeResultDesc(res, resDesc);
		volatile size_t totalSize = resDesc->ommArrayDataSize;
		volatile size_t totalSize2 = totalSize;

		omm::Debug::Stats stats = omm::Debug::Stats{};
		if (resDesc)
			omm::Debug::GetStats(_baker, resDesc, &stats);

		omm::Cpu::DestroyBakeResult(res);
		st.ResumeTiming();
	}

	omm::Baker _baker = 0;
	omm::Cpu::Texture _texture;
	omm::Cpu::BakeFlags _extraBakeFlags;

	std::vector<uint32_t> _indices;
	std::vector<float2> _texCoords;
};

BENCHMARK_DEFINE_F(OMMBake, BakeSerial)(benchmark::State& st) {
	for (auto s : st)
	{
		RunVmBake(st, false, omm::TextureFilterMode::Nearest);
	}
}

BENCHMARK_DEFINE_F(OMMBake, BakeParallel)(benchmark::State& st) {
	for (auto s : st)
	{
		RunVmBake(st, true, omm::TextureFilterMode::Nearest);
	}
}

BENCHMARK_DEFINE_F(OMMBake, BakeParallelLinear)(benchmark::State& st) {
	for (auto s : st)
	{
		RunVmBake(st, true, omm::TextureFilterMode::Linear);
	}
}

BENCHMARK_REGISTER_F(OMMBake, BakeSerial)->Iterations(2)->Unit(benchmark::kSecond)->Name("Warmup")
->Args({ (uint32_t)omm::Cpu::TextureFlags::None, (uint32_t)omm::Cpu::BakeFlags::None});

BENCHMARK_REGISTER_F(OMMBake, BakeSerial)->Iterations(2)->Unit(benchmark::kSecond)->Name("Morton")
->Args({ (uint32_t)omm::Cpu::TextureFlags::None, (uint32_t)omm::Cpu::BakeFlags::None});
BENCHMARK_REGISTER_F(OMMBake, BakeSerial)->Iterations(2)->Unit(benchmark::kSecond)->Name("Linear")
->Args({ (uint32_t)omm::Cpu::TextureFlags::DisableZOrder, (uint32_t)omm::Cpu::BakeFlags::None});

BENCHMARK_REGISTER_F(OMMBake, BakeParallel)->Iterations(2)->Unit(benchmark::kSecond)->Name("Morton")
->Args({ (uint32_t)omm::Cpu::TextureFlags::None, (uint32_t)omm::Cpu::BakeFlags::None});
BENCHMARK_REGISTER_F(OMMBake, BakeParallel)->Iterations(2)->Unit(benchmark::kSecond)->Name("Linear")
->Args({ (uint32_t)omm::Cpu::TextureFlags::DisableZOrder, (uint32_t)omm::Cpu::BakeFlags::None});

static constexpr omm::Cpu::BakeFlags DisableLevelLineIntersection = (omm::Cpu::BakeFlags)(1u << 8);
BENCHMARK_REGISTER_F(OMMBake, BakeParallel)->Iterations(2)->Unit(benchmark::kSecond)->Name("EnableLevelLineIntersection")
->Args({ (uint32_t)omm::Cpu::TextureFlags::DisableZOrder, (uint32_t)omm::Cpu::BakeFlags::None });
BENCHMARK_REGISTER_F(OMMBake, BakeParallel)->Iterations(2)->Unit(benchmark::kSecond)->Name("DisableLevelLineIntersection")
->Args({(uint32_t)omm::Cpu::TextureFlags::DisableZOrder, (uint32_t)DisableLevelLineIntersection });

static constexpr omm::Cpu::BakeFlags EnableNearDuplicateDetectionBruteForce = (omm::Cpu::BakeFlags)(1u << 9);
BENCHMARK_REGISTER_F(OMMBake, BakeParallel)->Iterations(2)->Unit(benchmark::kSecond)->Name("EnableNearDuplicateDetectionApprox")
->Args({ (uint32_t)omm::Cpu::TextureFlags::DisableZOrder, (uint32_t)omm::Cpu::BakeFlags::EnableNearDuplicateDetection });
BENCHMARK_REGISTER_F(OMMBake, BakeParallel)->Iterations(2)->Unit(benchmark::kSecond)->Name("EnableNearDuplicateDetectionBruteForce")
->Args({ (uint32_t)omm::Cpu::TextureFlags::DisableZOrder, (uint32_t)omm::Cpu::BakeFlags::EnableNearDuplicateDetection | (uint32_t) EnableNearDuplicateDetectionBruteForce });

BENCHMARK_MAIN();
