// NUI game view ('CfxGameRenderHandle'): like CaptureBufferOutput in rage-graphics-five, the back buffer is copied into a
// texture shared with CEF's ANGLE device through an NT handle, after the game's frame and before NUI draws.

#include "StdInc.h"

#include <DrawCommands.h>

#include <Hooking.h>
#include <HostSharedData.h>
#include <MinHook.h>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>

#include <deque>

#pragma comment(lib, "dxgi.lib")

namespace WRL = Microsoft::WRL;

struct GameRenderData
{
	HANDLE handle = NULL;
	int width = 0;
	int height = 0;
	bool requested = false;
};

// rage::sga::GraphicsContext's command buffer (Vulkan) and command list (D3D12)
static uint32_t g_commandBufferOffset;
static uint32_t g_commandListOffset;

static VkPhysicalDevice g_physicalDevice;
static bool g_canCopyBackbuffer;

static PFN_vkCreateSwapchainKHR g_origCreateSwapchainKHR;

// the game doesn't make the back buffer a copy source
static VkResult VKAPI_CALL CreateSwapchainKHRHook(VkDevice device, const VkSwapchainCreateInfoKHR* info, const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain)
{
	VkSwapchainCreateInfoKHR createInfo = *info;
	VkSurfaceCapabilitiesKHR capabilities = {};
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_physicalDevice, createInfo.surface, &capabilities);

	g_canCopyBackbuffer = (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	createInfo.imageUsage |= (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

	return g_origCreateSwapchainKHR(device, &createInfo, allocator, swapchain);
}

void GameView_OnDeviceCreated(VkPhysicalDevice physicalDevice, VkDevice device)
{
	g_physicalDevice = physicalDevice;

	MH_Initialize();
	MH_CreateHook(vkGetDeviceProcAddr(device, "vkCreateSwapchainKHR"), CreateSwapchainKHRHook, (void**)&g_origCreateSwapchainKHR);
	MH_EnableHook(MH_ALL_HOOKS);
}

struct SharedTarget
{
	WRL::ComPtr<ID3D11Texture2D> texture; // Vulkan: made on D3D11, imported as `image`
	WRL::ComPtr<ID3D12Resource> resource; // D3D12
	HANDLE handle = NULL;
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;

	~SharedTarget()
	{
		if (image)
		{
			vkDestroyImage((VkDevice)GetGraphicsDriverHandle(), image, nullptr);
			vkFreeMemory((VkDevice)GetGraphicsDriverHandle(), memory, nullptr);
		}

		if (handle)
		{
			CloseHandle(handle);
		}
	}
};

static std::unique_ptr<SharedTarget> CreateVulkanTarget(int width, int height, DXGI_FORMAT format)
{
	// nui-core hooks D3D11CreateDevice and turns shared textures into read-only NT handles, which Vulkan can't write to,
	// so the texture is made on a device of its own, on the adapter nui-core gives ANGLE (PatchAdapter)
	static WRL::ComPtr<ID3D11Device> device = []()
	{
		using TCoreCreateDevice = HRESULT(WINAPI*)(IDXGIFactory*, IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*);

		wchar_t systemDir[MAX_PATH];
		GetSystemDirectoryW(systemDir, std::size(systemDir));

		auto coreCreateDevice = (TCoreCreateDevice)GetProcAddress(LoadLibraryW(va(L"%s\\d3d11.dll", systemDir)), "D3D11CoreCreateDevice");
		const D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;

		WRL::ComPtr<IDXGIFactory6> factory;
		WRL::ComPtr<IDXGIAdapter> adapter;
		WRL::ComPtr<ID3D11Device> device;

		if (coreCreateDevice && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter))))
		{
			coreCreateDevice(nullptr, adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &featureLevel, 1, D3D11_SDK_VERSION, &device, nullptr);
		}

		return device;
	}();

	auto target = std::make_unique<SharedTarget>();
	auto desc = CD3D11_TEXTURE2D_DESC(format, width, height, 1, 1, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, D3D11_USAGE_DEFAULT, 0, 1, 0, D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE);
	WRL::ComPtr<IDXGIResource1> resource;

	// Vulkan writes through a read-only handle never show up on the D3D11 side
	if (!device || FAILED(device->CreateTexture2D(&desc, nullptr, &target->texture)) || FAILED(target->texture.As(&resource)) || FAILED(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &target->handle)))
	{
		return {};
	}

	auto vkDevice = (VkDevice)GetGraphicsDriverHandle();

	VkExternalMemoryImageCreateInfo externalInfo = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, nullptr, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT };
	VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &externalInfo, 0, VK_IMAGE_TYPE_2D, (format == DXGI_FORMAT_R8G8B8A8_UNORM) ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM, { uint32_t(width), uint32_t(height), 1 }, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_DST_BIT };

	if (vkCreateImage(vkDevice, &imageInfo, nullptr, &target->image) != VK_SUCCESS)
	{
		return {};
	}

	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(vkDevice, target->image, &requirements);

	unsigned long typeIndex;
	_BitScanForward(&typeIndex, requirements.memoryTypeBits);

	VkMemoryDedicatedAllocateInfo dedicatedInfo = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr, target->image };
	VkImportMemoryWin32HandleInfoKHR importInfo = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &dedicatedInfo, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, target->handle };
	VkMemoryAllocateInfo allocateInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &importInfo, requirements.size, typeIndex };

	if (vkAllocateMemory(vkDevice, &allocateInfo, nullptr, &target->memory) != VK_SUCCESS || vkBindImageMemory(vkDevice, target->image, target->memory, 0) != VK_SUCCESS)
	{
		return {};
	}

	return target;
}

static std::unique_ptr<SharedTarget> CreateD3D12Target(int width, int height, DXGI_FORMAT format)
{
	auto device = (ID3D12Device*)GetGraphicsDriverHandle();
	auto target = std::make_unique<SharedTarget>();

	// simultaneous access: the copy writes it from COMMON without barriers, while D3D11 reads it on its own device
	D3D12_HEAP_PROPERTIES heapProperties = { D3D12_HEAP_TYPE_DEFAULT };
	D3D12_RESOURCE_DESC desc = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, UINT64(width), UINT(height), 1, 1, format, { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS };

	if (FAILED(device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&target->resource))) || FAILED(device->CreateSharedHandle(target->resource.Get(), nullptr, GENERIC_ALL, nullptr, &target->handle)))
	{
		return {};
	}

	return target;
}

// not destroyed on exit, the device is gone by then; replaced ones are kept for a few frames, the GPU may still copy into them
static SharedTarget* g_target;
static std::deque<std::pair<uint64_t, SharedTarget*>> g_retiredTargets;

void GameView_Capture(void* context, rage::sga::Texture* backbuffer, void* nativeBackbuffer)
{
	static HostSharedData<GameRenderData> handleData("CfxGameRenderHandle");
	static uint64_t frame;
	static rage::sga::ImageParams targetParams;

	auto& params = *(rage::sga::ImageParams*)((char*)backbuffer + 0x18);
	auto isVulkan = (GetCurrentGraphicsAPI() == GraphicsAPI::Vulkan);

	// 8-bit back buffers are copied as they are (sRGB values stay encoded, same as Five)
	DXGI_FORMAT format;

	switch (params.bufferFormat)
	{
		case rage::sga::BufferFormat::R8G8B8A8_UNORM:
		case rage::sga::BufferFormat::R8G8B8A8_UNORM_SRGB:
			format = DXGI_FORMAT_R8G8B8A8_UNORM;
			break;
		case rage::sga::BufferFormat::B8G8R8A8_UNORM:
		case rage::sga::BufferFormat::B8G8R8A8_UNORM_SRGB:
			format = DXGI_FORMAT_B8G8R8A8_UNORM;
			break;
		default:
			return;
	}

	frame++;

	while (!g_retiredTargets.empty() && g_retiredTargets.front().first + 8 < frame)
	{
		delete g_retiredTargets.front().second;
		g_retiredTargets.pop_front();
	}

	// like on Five, the shared texture exists before any page asks for it; a failed one is retried once the back buffer changes
	if (params.width != targetParams.width || params.height != targetParams.height || params.bufferFormat != targetParams.bufferFormat)
	{
		targetParams = params;

		if (g_target)
		{
			g_retiredTargets.emplace_back(frame, g_target);
		}

		g_target = ((isVulkan) ? CreateVulkanTarget(params.width, params.height, format) : CreateD3D12Target(params.width, params.height, format)).release();

		handleData->width = params.width;
		handleData->height = params.height;
		handleData->handle = (g_target) ? g_target->handle : NULL;
	}

	if (!g_target || !handleData->requested)
	{
		return;
	}

	// the back buffer is still a render target at this point
	if (isVulkan)
	{
		auto commandBuffer = *(VkCommandBuffer*)((char*)context + g_commandBufferOffset);

		if (!commandBuffer || !g_canCopyBackbuffer)
		{
			return;
		}

		auto barrier = [](VkImage image, VkImageLayout from, VkImageLayout to)
		{
			return VkImageMemoryBarrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, from, to, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
		};

		VkImageMemoryBarrier before[] = { barrier((VkImage)nativeBackbuffer, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL), barrier(g_target->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) };
		VkImageMemoryBarrier after[] = { barrier((VkImage)nativeBackbuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL), barrier(g_target->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL) };
		VkImageCopy region = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, {}, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, {}, { params.width, params.height, 1 } };

		vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, std::size(before), before);
		vkCmdCopyImage(commandBuffer, (VkImage)nativeBackbuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_target->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, std::size(after), after);
	}
	else if (auto commandList = *(ID3D12GraphicsCommandList**)((char*)context + g_commandListOffset))
	{
		D3D12_RESOURCE_BARRIER barrier = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
		barrier.Transition = { (ID3D12Resource*)nativeBackbuffer, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE };

		commandList->ResourceBarrier(1, &barrier);
		commandList->CopyResource(g_target->resource.Get(), (ID3D12Resource*)nativeBackbuffer);

		std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
		commandList->ResourceBarrier(1, &barrier);
	}
}

static HookFunction hookFunction([]()
{
	g_commandBufferOffset = *hook::get_pattern<uint32_t>("48 8B 4D 80 48 8B 89 ? ? ? ? FF 15", 7);
	g_commandListOffset = *hook::get_pattern<uint32_t>("48 8D 9A ? ? ? ? 4D 8B E9 4C 8B 63 40", 3) + 0x40;
});
