#include "Cafe/HW/Latte/Renderer/Metal/MetalLayerHandle.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalLayer.h"

#include "gui/interface/WindowSystem.h"

MetalLayerHandle::MetalLayerHandle(MTL::Device* device, const Vector2i& size, bool mainWindow)
{
    const auto& windowInfo = (mainWindow ? WindowSystem::GetWindowInfo().window_main : WindowSystem::GetWindowInfo().window_pad);

    m_layer = (CA::MetalLayer*)CreateMetalLayer(windowInfo.surface, m_layerScaleX, m_layerScaleY, &m_view);
    m_layer->setDevice(device);
    m_layer->setDrawableSize(CGSize{(float)size.x * m_layerScaleX, (float)size.y * m_layerScaleY});
    m_layer->setFramebufferOnly(true);
}

MetalLayerHandle::MetalLayerHandle(MetalLayerHandle&& other) noexcept
{
    *this = std::move(other);
}

MetalLayerHandle& MetalLayerHandle::operator=(MetalLayerHandle&& other) noexcept
{
    if (this != &other)
    {
        Release();
        m_layer = std::exchange(other.m_layer, nullptr);
        m_view = std::exchange(other.m_view, nullptr);
        m_drawable = std::exchange(other.m_drawable, nullptr);
        m_layerScaleX = other.m_layerScaleX;
        m_layerScaleY = other.m_layerScaleY;
    }
    return *this;
}

MetalLayerHandle::~MetalLayerHandle()
{
    Release();
}

void MetalLayerHandle::Release()
{
    // the drawable is owned by the layer's pool; only drop our pointer
    m_drawable = nullptr;
    if (m_layer || m_view)
        DestroyMetalLayer(m_layer, m_view);
    m_layer = nullptr;
    m_view = nullptr;
}

void MetalLayerHandle::Resize(const Vector2i& size)
{
    m_layer->setDrawableSize(CGSize{(float)size.x * m_layerScaleX, (float)size.y * m_layerScaleY});
}

bool MetalLayerHandle::AcquireDrawable()
{
    if (m_drawable)
        return true;

    m_drawable = m_layer->nextDrawable();
    if (!m_drawable)
    {
        cemuLog_log(LogType::Force, "layer {} failed to acquire next drawable", (void*)this);
        return false;
    }

    return true;
}

void MetalLayerHandle::PresentDrawable(MTL::CommandBuffer* commandBuffer)
{
    commandBuffer->presentDrawable(m_drawable);
    m_drawable = nullptr;
}
