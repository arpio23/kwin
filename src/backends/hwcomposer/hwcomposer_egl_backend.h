/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-3.0-or-later
*/

#pragma once

#include "platformsupport/scenes/opengl/openglbackend.h"
#include "core/outputlayer.h"
#include "utils/common.h"
#include "utils/damagejournal.h"
#include <memory>
#include <optional>

#include "opengl/gltexture.h"
#include "opengl/gltexture_p.h"
#include "options.h"
#include <epoxy/egl.h>

#include <KWayland/Client/buffer.h>

namespace KWin
{

class HwcomposerBackend;
class HwcomposerOutput;
class HwcomposerWindow;
class EglHwcBackend; // Forward declaration
class GLRenderTimeQuery;
class EglDisplay;
class EglContext;

class EglHwcOutputLayer : public OutputLayer
{
public:
    EglHwcOutputLayer(EglHwcBackend *backend);

    std::optional<OutputLayerBeginFrameInfo> doBeginFrame() override;
    bool doEndFrame(const QRegion &renderedRegion, const QRegion &damagedRegion, OutputFrame *frame) override;
    DrmDevice *scanoutDevice() const override;
    QHash<uint32_t, QList<uint64_t>> supportedDrmFormats() const override;

private:
    EglHwcBackend *const m_backend;
};

class EglHwcBackend : public OpenGLBackend
{
public:
    EglHwcBackend(HwcomposerBackend *backend);
    ~EglHwcBackend() override;

    void init() override;

    std::unique_ptr<SurfaceTexture> createSurfaceTextureWayland(SurfacePixmap *pixmap) override;
    OutputLayerBeginFrameInfo beginFrame();
    void endFrame(const QRegion &renderedRegion, const QRegion &damagedRegion, OutputFrame *frame);
    void present(Output *output, const std::shared_ptr<OutputFrame> &frame) override;
    OverlayWindow *overlayWindow() const override;
    OutputLayer *primaryLayer(Output *output) override;
    EglDisplay *eglDisplayObject() const override;
    OpenGlContext *openglContext() const override;
    bool makeCurrent() override;
    void doneCurrent() override;

private:
    EGLConfig chooseBufferConfig();
    bool initRenderingContext();
    void initClientExtensions();
    bool hasClientExtension(const QByteArray &name);
    void screenGeometryChanged();
    void presentSurface(::EGLSurface surface, const QRegion &damage, const QRect &screenGeometry);
    void vblank(std::chrono::nanoseconds timestamp);
    EGLSurface createSurface();

    bool createEglHwcOutputLayer(Output *output);
    std::map<Output *, std::unique_ptr<EglHwcOutputLayer>> m_outputs;

    HwcomposerBackend *m_backend;
    std::unique_ptr<OverlayWindow> m_overlayWindow;
    DamageJournal m_damageJournal;
    std::unique_ptr<GLFramebuffer> m_fbo;
    int m_bufferAge = 0;
    QRegion m_lastRenderedRegion;
    std::unique_ptr<EglHwcOutputLayer> m_layer;
    std::unique_ptr<GLRenderTimeQuery> m_query;
    int m_havePostSubBuffer = false;
    bool m_havePlatformBase = false;
    Options::GlSwapStrategy m_swapStrategy = Options::AutoSwapStrategy;
    std::shared_ptr<OutputFrame> m_frame;

    QList<QByteArray> m_clientExtensions;
    std::shared_ptr<EglContext> m_context;
    EGLSurface m_surface = EGL_NO_SURFACE;
};

} // namespace KWin
