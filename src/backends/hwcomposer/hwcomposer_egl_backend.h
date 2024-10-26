/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-3.0-or-later
*/

#pragma once

#include "platformsupport/scenes/opengl/abstract_egl_backend.h"
#include "core/outputlayer.h"
#include "utils/common.h"
#include "utils/damagejournal.h"
#include <memory>
#include <optional>

#include <KWayland/Client/buffer.h>

namespace KWin
{

class HwcomposerBackend;
class HwcomposerOutput;
class HwcomposerWindow;
class EglHwcomposerOutput; // Forward declaration
class GLRenderTimeQuery;

class EglHwcomposerBackend : public AbstractEglBackend
{
public:
    EglHwcomposerBackend(HwcomposerBackend *backend);
    ~EglHwcomposerBackend() override;

    std::unique_ptr<SurfaceTexture> createSurfaceTextureWayland(SurfacePixmap *pixmap) override;
    OutputLayer *primaryLayer(Output *output) override;
    void init() override;
    void present(Output *output, const std::shared_ptr<OutputFrame> &frame) override;
    EGLConfig getConfig();
    EGLConfig getContext();

private:
    bool initializeEgl();
    bool initBufferConfigs();
    bool initRenderingContext();
    bool createEglHwcomposerOutput(Output *output);
    void cleanupSurfaces() override;

    EGLConfig m_configs;
    HwcomposerBackend *m_backend;
    std::shared_ptr<EglContext> m_context;
    std::map<Output *, std::unique_ptr<EglHwcomposerOutput>> m_outputs;
};

class EglHwcomposerOutput : public OutputLayer
{
public:
    EglHwcomposerOutput(HwcomposerOutput *output, EglHwcomposerBackend *backend);
    ~EglHwcomposerOutput() override;

    std::optional<OutputLayerBeginFrameInfo> doBeginFrame() override;
    bool doEndFrame(const QRegion &renderedRegion, const QRegion &damagedRegion, OutputFrame *frame) override;

    DrmDevice *scanoutDevice() const override;
    QHash<uint32_t, QList<uint64_t>> supportedDrmFormats() const override;

    EGLSurface surface() const
    {
        return m_surface;
    }
    GLFramebuffer *framebuffer() const
    {
        return m_framebuffer.get();
    }

    OutputFrame *m_frame;
private:
    bool makeContextCurrent() const;

    HwcomposerWindow *m_nativeSurface = nullptr;
    EGLSurface m_surface = EGL_NO_SURFACE;
    std::unique_ptr<GLFramebuffer> m_framebuffer;
    QRegion m_lastRenderedRegion;
    DamageJournal m_damageJournal;
    // QRegion m_currentDamage;
    // DamageJournal m_damageJournal;
    int m_bufferAge = 0;

    HwcomposerOutput *m_output;
    EglHwcomposerBackend *m_backend;

    // std::shared_ptr<EglSwapchain> m_swapchain;
    // std::shared_ptr<EglSwapchainSlot> m_current;
    std::unique_ptr<GLRenderTimeQuery> m_query;
};

} // namespace KWin
