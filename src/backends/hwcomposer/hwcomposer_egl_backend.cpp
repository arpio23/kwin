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

namespace KWin
{

EglHwcomposerBackend::EglHwcomposerBackend(HwcomposerBackend *backend)
    : AbstractEglBackend()
    , m_backend(backend)
{
    setSupportsNativeFence(true);

    connect(m_backend, &HwcomposerBackend::outputAdded, this, &EglHwcomposerBackend::createEglHwcomposerOutput);
    connect(m_backend, &HwcomposerBackend::outputRemoved, this, [this](Output *output) {
        m_outputs.erase(output);
    });
}

EglHwcomposerBackend::~EglHwcomposerBackend()
{
    cleanup();
}

void EglHwcomposerBackend::cleanupSurfaces()
{
    m_outputs.clear();
}

bool EglHwcomposerBackend::createEglHwcomposerOutput(Output *output)
{
    m_outputs[output] = std::make_unique<EglHwcomposerOutput>(static_cast<HwcomposerOutput *>(output), this);
    return true;
}

bool EglHwcomposerBackend::initializeEgl()
{
    qputenv("EGL_PLATFORM", QByteArrayLiteral("hwcomposer"));
    
    initClientExtensions();

    if (!m_backend->sceneEglDisplayObject()) {
        m_backend->setEglDisplay(EglDisplay::create(eglGetDisplay(EGL_DEFAULT_DISPLAY)));
    }
    auto display = m_backend->sceneEglDisplayObject();
    setEglDisplay(display);

    return true;
}

void EglHwcomposerBackend::init()
{
    if (!initializeEgl()) {
        setFailed("Failed to initialize egl");
        return;
    }
    if (!initRenderingContext()) {
        setFailed("Could not initialize rendering context");
        return;
    }

    setSupportsBufferAge(true);
    initWayland();
}

bool EglHwcomposerBackend::initBufferConfigs()
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
        return false;
    }
    if (count == 0) {
        qCCritical(KWIN_HWCOMPOSER) << "choose config did not return a config";
        return false;
    }
    m_configs = configs[0];
    return true;
}

bool EglHwcomposerBackend::initRenderingContext()
{
    if (!initBufferConfigs()) {
        return false;
    }

    if (!createContext(getConfig())) {
        return false;
    }
    auto hwcOutputs = m_backend->outputs();

    // we only allow to start with at least one output
    if (hwcOutputs.isEmpty()) {
        return false;
    }

    for (auto *out : hwcOutputs) {
        if (!createEglHwcomposerOutput(out)) {
            return false;
        }
    }

    if (m_outputs.empty()) {
        qCCritical(KWIN_HWCOMPOSER) << "Create Window Surfaces failed";
        return false;
    }

    return makeCurrent();
}

EGLConfig EglHwcomposerBackend::getContext()
{
    return m_context.get();
}

void EglHwcomposerBackend::present(Output *output, const std::shared_ptr<OutputFrame> &frame)
{
    if (!output) {
        return;
    }

    const bool correct = eglSwapBuffers(m_backend->sceneEglDisplayObject()->handle(), m_outputs[output].get()->surface());
    if (!correct) {
        qCWarning(KWIN_HWCOMPOSER) << "eglSwapBuffers failed:" << getEglErrorString() << m_outputs[output].get()->surface();
    }

    // static_cast<HwcomposerOutput *>(output)->m_frame = frame.get();
    static_cast<HwcomposerOutput *>(output)->notifyFrame(frame);
}

std::unique_ptr<SurfaceTexture> EglHwcomposerBackend::createSurfaceTextureWayland(SurfacePixmap *pixmap)
{
    return std::make_unique<BasicEGLSurfaceTextureWayland>(this, pixmap);
}

OutputLayer* EglHwcomposerBackend::primaryLayer(Output *output)
{
    if (!output) {
        return nullptr;
    }

    return m_outputs[output].get();
}

EGLConfig EglHwcomposerBackend::getConfig()
{
    return m_configs;
}

EglHwcomposerOutput::EglHwcomposerOutput(HwcomposerOutput *output, EglHwcomposerBackend *backend)
    : OutputLayer(output)
    , m_backend(backend)
{
    m_output = output;
    m_nativeSurface = m_output->createSurface();
    m_surface = eglCreateWindowSurface(m_backend->eglDisplayObject()->handle(), m_backend->getConfig(), (EGLNativeWindowType) static_cast<ANativeWindow *>(m_nativeSurface), nullptr);
    if (m_surface == EGL_NO_SURFACE) {
        qCCritical(KWIN_HWCOMPOSER) << "Create surface failed";
        return;
    }

    m_framebuffer = std::make_unique<GLFramebuffer>(0, m_output->pixelSize());
}

EglHwcomposerOutput::~EglHwcomposerOutput()
{
    if (m_surface != EGL_NO_SURFACE) {
        eglDestroySurface(m_backend->eglDisplayObject(), m_surface);
    }
}

bool EglHwcomposerOutput::makeContextCurrent() const
{
    return m_backend->openglContext()->makeCurrent(m_surface);
}

DrmDevice *EglHwcomposerOutput::scanoutDevice() const
{
    qCWarning(KWIN_HWCOMPOSER) << "EglHwcomposerOutput::scanoutDevice()";
    return nullptr;
}

QHash<uint32_t, QList<uint64_t>> EglHwcomposerOutput::supportedDrmFormats() const
{
    qCWarning(KWIN_HWCOMPOSER) << "EglHwcomposerOutput::supportedDrmFormats()";
    return {};
}

std::optional<OutputLayerBeginFrameInfo> EglHwcomposerOutput::doBeginFrame()
{
    QRegion repair = m_damageJournal.accumulate(0, infiniteRegion());
    if (!makeContextCurrent()) {
        qCWarning(KWIN_HWCOMPOSER) << "EglHwcomposerOutput::doBeginFrame() makeContextCurrent FAILED";
        return std::nullopt;
    }

    // QRegion repair = infiniteRegion();
    // if (m_backend->supportsBufferAge()) {
    //     repair = m_damageJournal.accumulate(m_bufferAge, infiniteRegion());
    // }
    m_query = std::make_unique<GLRenderTimeQuery>(m_backend->openglContextRef());
    m_query->begin();

    return OutputLayerBeginFrameInfo{
        .renderTarget = RenderTarget(framebuffer()),
        .repaint = repair,
    };
}

// void EglHwcomposerOutput::aboutToStartPainting(const QRegion &damagedRegion)
// {
//     if (surface() && m_bufferAge > 0 && !damagedRegion.isEmpty() && m_backend->supportsPartialUpdate()) {
//         QVector<EGLint> rects = m_output->regionToRects(damagedRegion);
//         const bool correct = eglSetDamageRegionKHR(m_backend->eglDisplay(), surface(), rects.data(), rects.count() / 4);
//         if (!correct) {
//             qCWarning(KWIN_HWCOMPOSER) << "eglSetDamageRegionKHR failed:" << getEglErrorString();
//         }
//     }
// }

bool EglHwcomposerOutput::doEndFrame(const QRegion &renderedRegion, const QRegion &damagedRegion, OutputFrame *frame)
{
    Q_UNUSED(damagedRegion)
    m_lastRenderedRegion = renderedRegion;
    m_query->end();
    frame->addRenderTimeQuery(std::move(m_query));
    //glFlush();

    // Q_UNUSED(renderedRegion)
    // m_currentDamage = damagedRegion;
    return true;
}

} // namespace KWin
