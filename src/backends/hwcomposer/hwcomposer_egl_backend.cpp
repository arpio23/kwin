/*
    SPDX-FileCopyrightText: 2010, 2012 Martin Gräßlin <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2020 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "hwcomposer_egl_backend.h"
#include "hwcomposer_backend.h"
#include "hwcomposer_logging.h"
#include <opengl/glplatform.h>
#include <opengl/glutils.h>
#include <opengl/eglimagetexture.h>
#include "platformsupport/scenes/opengl/basiceglsurfacetexture_wayland.h"
#include "platformsupport/scenes/opengl/openglbackend.h"
#include "opengl/glrendertimequery.h"
#include <opengl/eglutils_p.h>
#include <memory>

#include "core/outputbackend.h"
#include "core/outputlayer.h"
#include "core/overlaywindow.h"
#include "core/renderloop_p.h"
#include "opengl/eglcontext.h"
#include "opengl/egldisplay.h"
#include "opengl/glplatform.h"
#include "opengl/glrendertimequery.h"
#include "options.h"
#include <QOpenGLContext>

namespace KWin
{

EglHwcOutputLayer::EglHwcOutputLayer(EglHwcBackend *backend)
    : OutputLayer(nullptr)
    , m_backend(backend)
{
}

std::optional<OutputLayerBeginFrameInfo> EglHwcOutputLayer::doBeginFrame()
{
    return m_backend->beginFrame();
}

bool EglHwcOutputLayer::doEndFrame(const QRegion &renderedRegion, const QRegion &damagedRegion, OutputFrame *frame)
{
    m_backend->endFrame(renderedRegion, damagedRegion, frame);
    return true;
}

DrmDevice *EglHwcOutputLayer::scanoutDevice() const
{
    return nullptr;
}

QHash<uint32_t, QList<uint64_t>> EglHwcOutputLayer::supportedDrmFormats() const
{
    return {};
}

EglHwcBackend::EglHwcBackend(HwcomposerBackend *backend)
    : m_backend(backend)
{
    connect(m_backend, &HwcomposerBackend::outputAdded, this, &EglHwcBackend::createEglHwcOutputLayer);
    connect(m_backend, &HwcomposerBackend::outputRemoved, this, [this](Output *output) {
        m_outputs.erase(output);
    });
}

EglHwcBackend::~EglHwcBackend()
{
    // No completion events will be received for in-flight frames, this may lock the
    // render loop. We need to ensure that the render loop is back to its initial state
    // if the render backend is about to be destroyed.
    // RenderLoopPrivate::get(m_backend->renderLoop())->invalidate();

    m_query.reset();

    if (isFailed() && m_overlayWindow) {
        m_overlayWindow->destroy();
    }
    if (m_surface != EGL_NO_SURFACE) {
        eglDestroySurface(eglDisplayObject()->handle(), m_surface);
    }
    m_context.reset();
}

bool EglHwcBackend::createEglHwcOutputLayer(Output *output)
{
    m_outputs[output] = std::make_unique<EglHwcOutputLayer>(this);
    return true;
}

std::unique_ptr<SurfaceTexture> EglHwcBackend::createSurfaceTextureWayland(SurfacePixmap *pixmap)
{
    return std::make_unique<BasicEGLSurfaceTextureWayland>(this, pixmap);
}

void EglHwcBackend::init()
{
    qputenv("EGL_PLATFORM", QByteArrayLiteral("hwcomposer"));

    if (!m_backend->sceneEglDisplayObject()) {
        m_backend->setEglDisplay(EglDisplay::create(eglGetDisplay(EGL_DEFAULT_DISPLAY)));
    }

    if (m_backend->sceneEglDisplayObject() == EGL_NO_DISPLAY) {
        setFailed(QStringLiteral("Failed to create display"));
        return;
    }

    if (!initRenderingContext()) {
        setFailed("Could not initialize rendering context");
        return;
    }
}

void EglHwcBackend::initClientExtensions()
{
    // Get the list of client extensions
    // const char *clientExtensionsCString = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    // const QByteArray clientExtensionsString = QByteArray::fromRawData(clientExtensionsCString, qstrlen(clientExtensionsCString));
    // if (clientExtensionsString.isEmpty()) {
    //     // If eglQueryString() returned NULL, the implementation doesn't support
    //     // EGL_EXT_client_extensions. Expect an EGL_BAD_DISPLAY error.
    //     (void)eglGetError();
    // }

    // m_clientExtensions = clientExtensionsString.split(' ');
}

bool EglHwcBackend::hasClientExtension(const QByteArray &name)
{
    qCWarning(KWIN_HWCOMPOSER) << "EglHwcBackend::hasClientExtension(const QByteArray &name)";
    // return m_clientExtensions.contains(name);
    return false;
}

bool EglHwcBackend::initRenderingContext()
{
    // initClientExtensions();
    auto display = m_backend->sceneEglDisplayObject();

    setSupportsBufferAge(display->supportsBufferAge());
    setSupportsNativeFence(display->supportsNativeFence());
    setExtensions(display->extensions());
    static std::unique_ptr<EglContext> s_globalShareContext;

    if (!s_globalShareContext) {
        s_globalShareContext = EglContext::create(m_backend->sceneEglDisplayObject(), chooseBufferConfig(), EGL_NO_CONTEXT);
    }
    if (s_globalShareContext) {
        m_backend->setSceneEglGlobalShareContext(s_globalShareContext->handle());
    }

    m_context = EglContext::create(display, chooseBufferConfig(), m_backend->sceneEglGlobalShareContext());
    if (!m_context) {
        qCCritical(KWIN_CORE) << "Create OpenGL context failed";
        return false;
    }

    m_surface = createSurface();
    if (m_surface == EGL_NO_SURFACE) {
        qCCritical(KWIN_CORE) << "Creating egl surface failed";
        return false;
    }

    if (!makeCurrent()) {
        qCCritical(KWIN_CORE) << "Make Context Current failed";
        return false;
    }

    EGLint error = eglGetError();
    if (error != EGL_SUCCESS) {
        qCWarning(KWIN_CORE) << "Error occurred while creating context " << error;
        return false;
    }
    return true;
}

EGLSurface EglHwcBackend::createSurface()
{
    EGLSurface surface = EGL_NO_SURFACE;

    if(m_outputs.size() < 1){
        m_outputs[m_backend->hwcOutputs().first()] = std::make_unique<EglHwcOutputLayer>(this);
    }
    
    surface = eglCreateWindowSurface(eglDisplayObject()->handle(), chooseBufferConfig(), (EGLNativeWindowType) static_cast<ANativeWindow *>(m_backend->hwcOutputs().first()->createSurface()), nullptr);

    m_fbo = std::make_unique<GLFramebuffer>(0, m_backend->hwcOutputs().first()->pixelSize());

    return surface;
}

EGLConfig EglHwcBackend::chooseBufferConfig()
{
    const EGLint config_attribs[] = {
        EGL_RED_SIZE,             8,
        EGL_GREEN_SIZE,           8,
        EGL_BLUE_SIZE,            8,
        EGL_ALPHA_SIZE,           8,
        EGL_RENDERABLE_TYPE,      EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE,         EGL_WINDOW_BIT,
        EGL_NONE,
    };

    EGLint count;
    EGLConfig configs[1024];
    if (eglChooseConfig(eglDisplayObject()->handle(), config_attribs, configs, 1024, &count) == EGL_FALSE) {
        qCCritical(KWIN_HWCOMPOSER) << "choose config failed";
        return EGL_NO_CONFIG_KHR;
    }
    if (count == 0) {
        qCCritical(KWIN_HWCOMPOSER) << "choose config did not return a config";
        return EGL_NO_CONFIG_KHR;
    }
    return configs[0];
}

OutputLayerBeginFrameInfo EglHwcBackend::beginFrame()
{
    makeCurrent();

    QRegion repaint;
    if (supportsBufferAge()) {
        repaint = m_damageJournal.accumulate(m_bufferAge, infiniteRegion());
    }
    eglWaitNative(EGL_CORE_NATIVE_ENGINE);

    m_query = std::make_unique<GLRenderTimeQuery>(m_context);
    m_query->begin();

    return OutputLayerBeginFrameInfo{
        .renderTarget = RenderTarget(m_fbo.get()),
        .repaint = repaint,
    };
}

void EglHwcBackend::endFrame(const QRegion &renderedRegion, const QRegion &damagedRegion, OutputFrame *frame)
{
    m_query->end();
    frame->addRenderTimeQuery(std::move(m_query));
    // Save the damaged region to history
    if (supportsBufferAge()) {
        m_damageJournal.add(damagedRegion);
    }
    m_lastRenderedRegion = renderedRegion;
}

void EglHwcBackend::present(Output *output, const std::shared_ptr<OutputFrame> &frame)
{
    const bool correct = eglSwapBuffers(m_backend->sceneEglDisplayObject()->handle(), m_surface);
    if (!correct) {
        qCWarning(KWIN_HWCOMPOSER) << "eglSwapBuffers failed:" << getEglErrorString() << m_surface;
    }
    static_cast<HwcomposerOutput *>(output)->notifyFrame();
    //presentSurface(m_surface, effectiveRenderedRegion, workspace()->geometry());
}

void EglHwcBackend::presentSurface(EGLSurface surface, const QRegion &damage, const QRect &screenGeometry)
{
    // const bool fullRepaint = supportsBufferAge() || (damage == screenGeometry);

    // if (fullRepaint || !m_havePostSubBuffer) {
    //     // the entire screen changed, or we cannot do partial updates (which implies we enabled surface preservation)
    //     eglSwapBuffers(eglDisplayObject()->handle(), surface);
    //     if (supportsBufferAge()) {
    //         eglQuerySurface(eglDisplayObject()->handle(), surface, EGL_BUFFER_AGE_EXT, &m_bufferAge);
    //     }
    // } else {
    //     // a part of the screen changed, and we can use eglPostSubBufferNV to copy the updated area
    //     for (const QRect &r : damage) {
    //         eglPostSubBufferNV(eglDisplayObject()->handle(), surface, r.left(), screenGeometry.height() - r.bottom() - 1, r.width(), r.height());
    //     }
    // }
}

OverlayWindow *EglHwcBackend::overlayWindow() const
{
    return m_overlayWindow.get();
}

OutputLayer *EglHwcBackend::primaryLayer(Output *output)
{
    if (!output) {
        return nullptr;
    }
    return m_outputs[output].get();
}

void EglHwcBackend::vblank(std::chrono::nanoseconds timestamp)
{
    qCWarning(KWIN_HWCOMPOSER) << "EglHwcBackend::vblank(std::chrono::nanoseconds timestamp)";
    // m_frame->presented(timestamp, PresentationMode::VSync);
    // m_frame.reset();
}

EglDisplay *EglHwcBackend::eglDisplayObject() const
{
    return m_backend->sceneEglDisplayObject();
}

OpenGlContext *EglHwcBackend::openglContext() const
{
    return m_context.get();
}

bool EglHwcBackend::makeCurrent()
{
    return m_context->makeCurrent(m_surface);
}

void EglHwcBackend::doneCurrent()
{
    m_context->doneCurrent();
}

} // namespace KWin
