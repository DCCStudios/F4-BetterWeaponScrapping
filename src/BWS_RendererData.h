#pragma once

#include "REL/Relocation.h"

#include <cstddef>
#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;

namespace BWS::Graphics
{
	/**
	 * Minimal view of RE::BSGraphics::RendererData for ImGui init.
	 * Offsets match the FO4 layout used by PhotoMode / fo4test CommonLib (device @ 0x48, context @ 0x50).
	 */
	class RendererData
	{
	public:
		[[nodiscard]] static RendererData* GetSingleton() noexcept
		{
			static REL::Relocation<RendererData**> singleton{ REL::ID(1235449) };
			return *singleton;
		}

	private:
		std::byte _pad0[0x48]{};

	public:
		ID3D11Device*        device;     // 48
		ID3D11DeviceContext* context;    // 50
		struct RenderWindow
		{
			void*           hwnd;            // 58
			std::int32_t    windowX;         // 60
			std::int32_t    windowY;         // 64
			std::int32_t    windowWidth;     // 68
			std::int32_t    windowHeight;    // 6C
			IDXGISwapChain* swapChain;       // 70
			std::byte       _embeddedRT[0x30];
		};
		RenderWindow renderWindow[32];
	};
}
