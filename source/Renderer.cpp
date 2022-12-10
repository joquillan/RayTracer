//External includes
#include "SDL.h"
#include "SDL_surface.h"

//Project includes
#include "Renderer.h"
#include "Math.h"
#include "Matrix.h"
#include "Material.h"
#include "Scene.h"
#include "Utils.h"

#include <ppl.h>
#include <future>
#include <iostream>

using namespace dae;



//#define ASYNC
#define PARALLEL_FOR



Renderer::Renderer(SDL_Window * pWindow) :
	m_pWindow(pWindow),
	m_pBuffer(SDL_GetWindowSurface(pWindow))
{
	//Initialize
	SDL_GetWindowSize(pWindow, &m_Width, &m_Height);
	m_pBufferPixels = static_cast<uint32_t*>(m_pBuffer->pixels);
}

void Renderer::Render(Scene* pScene) const
{
	

	Camera& camera = pScene->GetCamera();
	camera.CalculateCameraToWorld();

	const float fovAngle = camera.fovAngle * TO_RADIANS;
	const float fov = tan(fovAngle / 2.0f);

	float aspectRatio = static_cast<float>(m_Width) / static_cast<float>(m_Height);

	auto& materials = pScene->GetMaterials();
	auto& lights = pScene->GetLights();

	const uint32_t numPixels = m_Width * m_Height;


#if defined(ASYNC)


	//----------------- Async --------------------------------------
	//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
	const uint32_t numCores = std::thread::hardware_concurrency();
	std::vector<std::future<void>> async_futures{};
	const uint32_t numPixelsPerTask = numPixels / numCores;
	uint32_t numUnassignedPixels = numPixels % numCores;
	uint32_t currPixelIndex = 0;

	for (uint32_t coreId{0}; coreId < numCores ; ++coreId)
	{
		uint32_t taskSize = numPixelsPerTask;
		if (numUnassignedPixels > 0)
		{
			++taskSize;
			--numUnassignedPixels;
		}

		async_futures.emplace_back(std::async(std::launch::async, [=, this]
			{
				//Render all pixels for this task (currPixelIndex > currPixelIndex + taskSize)
				const uint32_t pixelIndexEnd = currPixelIndex + taskSize;
				for (uint32_t pixelIndex{ currPixelIndex }; pixelIndex < pixelIndexEnd; ++pixelIndex)
				{
					RenderPixel(pScene, pixelIndex, fov, aspectRatio, camera, lights, materials);
				}

			}));

		currPixelIndex += taskSize;
	}


	//Wait for async completion of all tasks
	for (const std::future<void>& f : async_futures)
	{
		f.wait();
	}

#elif defined(PARALLEL_FOR)

	//----------------- Parallel For ---------------------------
	//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
	concurrency::parallel_for(0u, numPixels, [=, this](int i) {
		RenderPixel(pScene, i, fov, aspectRatio, camera, lights, materials);
		});


#else

	//----------------- No Threading ---------------------------
	//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
	for (uint32_t i = 0; i < numPixels; ++i)
	{
		RenderPixel(pScene, i, fov, aspectRatio, camera, lights, materials);
	}

#endif


	//@END
	//Update SDL Surface
	SDL_UpdateWindowSurface(m_pWindow);
}

void dae::Renderer::RenderPixel(Scene* pScene, uint32_t pixelIndex, float fov, float aspectRatio, const Camera& camera, const std::vector<Light>& lights, const std::vector<Material*>& materials) const
{
	const int px = pixelIndex % m_Width;
	const int py = pixelIndex / m_Width;

	float rx = px + 0.5f;
	float ry = py + 0.5f;


	float x = (2 * (rx /float(m_Width)) - 1) * aspectRatio * fov;
	float y = (1 - (2 * (ry / float(m_Height)))) * fov;


	Vector3 cameraSpaceDir{ x, y, 1 };

	Vector3 rayDirection{ camera.cameraToWorld.TransformVector(cameraSpaceDir).Normalized() };

	Ray viewRay{ camera.origin, rayDirection };

	ColorRGB finalColor{ };

	HitRecord closestHit{};

	pScene->GetClosestHit(viewRay, closestHit);


	if (closestHit.didHit)
	{
		Ray shadowRay{};
		shadowRay.min = 0.01f;

		Vector3 shadowDir{};

		for (size_t i = 0; i < lights.size(); ++i)
		{
			shadowDir = LightUtils::GetDirectionToLight(lights[i], closestHit.origin);
			shadowRay.max = shadowDir.Normalize();
			shadowRay.origin = closestHit.origin + shadowRay.min * shadowDir;
			shadowRay.direction = shadowDir;

			Vector3 v = closestHit.origin - viewRay.direction;

			if (Vector3::Dot(closestHit.normal, shadowDir) < 0)
				continue;

			if (pScene->DoesHit(shadowRay) && m_ShadowsEnabled)
				continue;

			ColorRGB E = LightUtils::GetRadiance(lights[i], closestHit.origin);

			ColorRGB BRDFrgb = materials[closestHit.materialIndex]->Shade(closestHit, shadowDir.Normalized(), viewRay.direction.Normalized());

			switch (m_CurrentLightingMode)
			{
			case LightingMode::ObservedArea:

				finalColor += ColorRGB{ 1.f, 1.f, 1.f } *Vector3::Dot(closestHit.normal, shadowDir);
				break;
			case LightingMode::Radiance:

				finalColor += E;
				break;
			case LightingMode::BRDF:
				finalColor += BRDFrgb;
				break;
			case LightingMode::Combined:
				finalColor += E * BRDFrgb * (Vector3::Dot(closestHit.normal, shadowDir));
				break;
			}
		}
	}


	//Update Color in Buffer
	finalColor.MaxToOne();


	m_pBufferPixels[px + (py * m_Width)] = SDL_MapRGB(m_pBuffer->format,
		static_cast<uint8_t>(finalColor.r * 255),
		static_cast<uint8_t>(finalColor.g * 255),
		static_cast<uint8_t>(finalColor.b * 255));


}


bool Renderer::SaveBufferToImage() const
{
	return SDL_SaveBMP(m_pBuffer, "RayTracing_Buffer.bmp");
}

void dae::Renderer::CycleLightingMode()
{
	switch (m_CurrentLightingMode)
	{
	case LightingMode::ObservedArea:
		m_CurrentLightingMode = LightingMode::Radiance;
		break;
	case LightingMode::Radiance:
		m_CurrentLightingMode = LightingMode::BRDF;
		break;
	case LightingMode::BRDF:
		m_CurrentLightingMode = LightingMode::Combined;
		break;
	case LightingMode::Combined:
		m_CurrentLightingMode = LightingMode::ObservedArea;
		break;
	default:
		break;
	};


}
