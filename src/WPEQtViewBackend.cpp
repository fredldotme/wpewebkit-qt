/*
 * Copyright (C) 2018, 2019 Igalia S.L
 * Copyright (C) 2018, 2019 Zodiac Inflight Innovations
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include "WPEQtViewBackend.h"

#include "WPEQtView.h"
#include <QGuiApplication>
#include <QOpenGLFunctions>
#include <QtGlobal>
#include <vector>

static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC imageTargetTexture2DOES;

std::unique_ptr<WPEQtViewBackend> WPEQtViewBackend::create(const QSizeF& size, QPointer<QOpenGLContext> context, EGLDisplay eglDisplay, QPointer<WPEQtView> view)
{
    if (!context || !view)
        return nullptr;

    if (eglDisplay == EGL_NO_DISPLAY)
        return nullptr;

    eglInitialize(eglDisplay, nullptr, nullptr);

    if (!eglBindAPI(EGL_OPENGL_ES_API) || !wpe_fdo_initialize_for_egl_display(eglDisplay))
        return nullptr;

    static const EGLint configAttributes[13] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 1,
        EGL_GREEN_SIZE, 1,
        EGL_BLUE_SIZE, 1,
        EGL_ALPHA_SIZE, 1,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };

    EGLint count = 0;
    if (!eglGetConfigs(eglDisplay, nullptr, 0, &count) || count < 1)
        return nullptr;

    EGLConfig eglConfig;
    EGLint matched = 0;
    EGLContext eglContext = nullptr;
    if (eglChooseConfig(eglDisplay, configAttributes, &eglConfig, 1, &matched) && !!matched) {
        static const EGLint contextAttributes[3] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
        eglContext = eglCreateContext(eglDisplay, eglConfig, nullptr, contextAttributes);
    }

    if (!eglContext)
        return nullptr;

    return std::make_unique<WPEQtViewBackend>(size, eglDisplay, eglContext, context, view);
}

WPEQtViewBackend::WPEQtViewBackend(const QSizeF& size, EGLDisplay display, EGLContext eglContext, QPointer<QOpenGLContext> context, QPointer<WPEQtView> view)
    : m_eglDisplay(display)
    , m_eglContext(eglContext)
    , m_view(view)
    , m_size(size)
{
    wpe_loader_init("libWPEBackend-fdo-1.0.so");

    imageTargetTexture2DOES = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));

    static const char* vertexShaderSource =
        "attribute vec2 pos;\n"
        "attribute vec2 texture;\n"
        "varying vec2 v_texture;\n"
        "void main() {\n"
        "  v_texture = texture;\n"
        "  gl_Position = vec4(pos, 0, 1);\n"
        "}\n";
    static const char* fragmentShaderSource =
        "precision mediump float;\n"
        "uniform sampler2D u_texture;\n"
        "varying vec2 v_texture;\n"
        "void main() {\n"
        "  gl_FragColor = texture2D(u_texture, v_texture);\n"
        "}\n";

    QOpenGLFunctions* glFunctions = context->functions();
    GLuint vertexShader = glFunctions->glCreateShader(GL_VERTEX_SHADER);
    glFunctions->glShaderSource(vertexShader, 1, &vertexShaderSource, nullptr);
    glFunctions->glCompileShader(vertexShader);

    GLuint fragmentShader = glFunctions->glCreateShader(GL_FRAGMENT_SHADER);
    glFunctions->glShaderSource(fragmentShader, 1, &fragmentShaderSource, nullptr);
    glFunctions->glCompileShader(fragmentShader);

    m_program = glFunctions->glCreateProgram();
    glFunctions->glAttachShader(m_program, vertexShader);
    glFunctions->glAttachShader(m_program, fragmentShader);
    glFunctions->glLinkProgram(m_program);

    glFunctions->glBindAttribLocation(m_program, 0, "pos");
    glFunctions->glBindAttribLocation(m_program, 1, "texture");
    m_textureUniform = glFunctions->glGetUniformLocation(m_program, "u_texture");

    static struct wpe_view_backend_exportable_fdo_egl_client exportableClient = {
        // export_egl_image
        nullptr,
        [](void* data, struct wpe_fdo_egl_exported_image* image)
        {
            static_cast<WPEQtViewBackend*>(data)->displayImage(image);
        },
        // padding
        nullptr, nullptr, nullptr
    };

    m_exportable = wpe_view_backend_exportable_fdo_egl_create(&exportableClient, this, m_size.width(), m_size.height());

    wpe_view_backend_add_activity_state(backend(), wpe_view_activity_state_visible | wpe_view_activity_state_focused | wpe_view_activity_state_in_window);

    m_surface.setFormat(context->format());
    m_surface.create();
}

WPEQtViewBackend::~WPEQtViewBackend()
{
    if (m_lockedImage)
        wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image(m_exportable, m_lockedImage);
    if (m_lockedImageOld)
        wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image(m_exportable, m_lockedImageOld);

    wpe_view_backend_exportable_fdo_destroy(m_exportable);
    eglDestroyContext(m_eglDisplay, m_eglContext);
}

void WPEQtViewBackend::setScaleFactor(float factor)
{
    m_scale = factor;
    auto backend = wpe_view_backend_exportable_fdo_get_view_backend(m_exportable);
    wpe_view_backend_dispatch_set_device_scale_factor(backend, m_scale);
}

void WPEQtViewBackend::resize(const QSizeF& newSize)
{
    if (!newSize.isValid())
        return;

    m_size = newSize;
    wpe_view_backend_dispatch_set_size(backend(), m_size.width(), m_size.height());
}

GLuint WPEQtViewBackend::texture(QOpenGLContext* context)
{
    if (!m_lockedImage && m_lockedImageOld)
        std::swap(m_lockedImage, m_lockedImageOld);

    if (!m_lockedImage || !hasValidSurface())
        return m_textureId;

    QSurface *oldSurface = context->surface();
    context->makeCurrent(&m_surface);

    QOpenGLFunctions* glFunctions = context->functions();
    if (!m_textureId) {
        glFunctions->glGenTextures(1, &m_textureId);
        glFunctions->glBindTexture(GL_TEXTURE_2D, m_textureId);
        glFunctions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glFunctions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFunctions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glFunctions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glFunctions->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_size.width(), m_size.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glFunctions->glBindTexture(GL_TEXTURE_2D, 0);
    }

    glFunctions->glClearColor(1, 0, 0, 1);
    glFunctions->glClear(GL_COLOR_BUFFER_BIT);

    glFunctions->glUseProgram(m_program);

    glFunctions->glActiveTexture(GL_TEXTURE0);
    glFunctions->glBindTexture(GL_TEXTURE_2D, m_textureId);
    imageTargetTexture2DOES(GL_TEXTURE_2D, wpe_fdo_egl_exported_image_get_egl_image(m_lockedImage));
    glFunctions->glUniform1i(m_textureUniform, 0);

    static const GLfloat vertices[4][2] = {
        { -1.0, 1.0  },
        {  1.0, 1.0  },
        { -1.0, -1.0 },
        {  1.0, -1.0 },
    };

    static const GLfloat texturePos[4][2] = {
        { 0, 0 },
        { 1, 0 },
        { 0, 1 },
        { 1, 1 },
    };

    glFunctions->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    glFunctions->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, texturePos);

    glFunctions->glEnableVertexAttribArray(0);
    glFunctions->glEnableVertexAttribArray(1);

    glFunctions->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glFunctions->glDisableVertexAttribArray(0);
    glFunctions->glDisableVertexAttribArray(1);

    wpe_view_backend_exportable_fdo_dispatch_frame_complete(m_exportable);
    if (m_lockedImageOld)
        wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image(m_exportable, m_lockedImageOld);
    m_lockedImageOld = m_lockedImage;
    m_lockedImage = nullptr;

    context->makeCurrent(oldSurface);
    return m_textureId;
}

void WPEQtViewBackend::displayImage(struct wpe_fdo_egl_exported_image* image)
{
    RELEASE_ASSERT(!m_lockedImage);
    m_lockedImage = image;
    if (m_view)
        m_view->triggerUpdate();
}

uint32_t WPEQtViewBackend::modifiers() const
{
    uint32_t mask = m_keyboardModifiers;
    if (m_mouseModifiers)
        mask |= m_mouseModifiers;
    return mask;
}

void WPEQtViewBackend::dispatchHoverEnterEvent(QHoverEvent*)
{
    m_hovering = true;
    m_mouseModifiers = 0;
}

void WPEQtViewBackend::dispatchHoverLeaveEvent(QHoverEvent*)
{
    m_hovering = false;
}

void WPEQtViewBackend::dispatchHoverMoveEvent(QHoverEvent* event)
{
    if (!m_hovering)
        return;

    uint32_t state = !!m_mousePressedButton;
    struct wpe_input_pointer_event wpeEvent = { wpe_input_pointer_event_type_motion,
        static_cast<uint32_t>(event->timestamp()),
        static_cast<int>(event->pos().x() * m_scale),
        static_cast<int>(event->pos().y() * m_scale),
        m_mousePressedButton, state, modifiers() };
    wpe_view_backend_dispatch_pointer_event(backend(), &wpeEvent);
}

void WPEQtViewBackend::dispatchMouseMoveEvent(QMouseEvent* event)
{
    uint32_t state = !!m_mousePressedButton;
    struct wpe_input_pointer_event wpeEvent = { wpe_input_pointer_event_type_motion,
        static_cast<uint32_t>(event->timestamp()),
        int(event->pos().x() * m_scale), int(event->pos().y() * m_scale),
        m_mousePressedButton, state, modifiers() };
    wpe_view_backend_dispatch_pointer_event(backend(), &wpeEvent);
}

void WPEQtViewBackend::dispatchMousePressEvent(QMouseEvent* event)
{
    uint32_t button = 0;
    uint32_t modifier = 0;
    switch (event->button()) {
    case Qt::LeftButton:
        button = 1;
        modifier = wpe_input_pointer_modifier_button1;
        break;
    case Qt::RightButton:
        button = 2;
        modifier = wpe_input_pointer_modifier_button2;
        break;
    default:
        break;
    }
    m_mousePressedButton = button;
    uint32_t state = 1;
    m_mouseModifiers |= modifier;
    struct wpe_input_pointer_event wpeEvent = { wpe_input_pointer_event_type_button,
        static_cast<uint32_t>(event->timestamp()),
        static_cast<int>(event->x() * m_scale),
        static_cast<int>(event->y() * m_scale),
        button, state, modifiers() };
    wpe_view_backend_dispatch_pointer_event(backend(), &wpeEvent);
}

void WPEQtViewBackend::dispatchMouseReleaseEvent(QMouseEvent* event)
{
    uint32_t button = 0;
    uint32_t modifier = 0;
    switch (event->button()) {
    case Qt::LeftButton:
        button = 1;
        modifier = wpe_input_pointer_modifier_button1;
        break;
    case Qt::RightButton:
        button = 2;
        modifier = wpe_input_pointer_modifier_button2;
        break;
    default:
        break;
    }
    m_mousePressedButton = 0;
    uint32_t state = 0;
    m_mouseModifiers &= ~modifier;
    struct wpe_input_pointer_event wpeEvent = { wpe_input_pointer_event_type_button,
        static_cast<uint32_t>(event->timestamp()),
        static_cast<int>(event->x() * m_scale),
        static_cast<int>(event->y() * m_scale),
        button, state, modifiers() };
    wpe_view_backend_dispatch_pointer_event(backend(), &wpeEvent);
}

#if (QT_VERSION >= QT_VERSION_CHECK(5, 14, 0))
#define QWHEEL_POSITION position()
#else
#define QWHEEL_POSITION posF()
#endif

void WPEQtViewBackend::dispatchWheelEvent(QWheelEvent* event)
{
    QPoint delta = event->angleDelta();
    QPoint numDegrees = delta / 8;
    struct wpe_input_axis_2d_event wpeEvent;
    if (delta.y() == event->QWHEEL_POSITION.y())
        wpeEvent.x_axis = numDegrees.x();
    else
        wpeEvent.y_axis = numDegrees.y();
    wpeEvent.base.type = static_cast<wpe_input_axis_event_type>(wpe_input_axis_event_type_mask_2d | wpe_input_axis_event_type_motion_smooth);
    wpeEvent.base.x = event->QWHEEL_POSITION.x();
    wpeEvent.base.y = event->QWHEEL_POSITION.y();
    wpe_view_backend_dispatch_axis_event(backend(), &wpeEvent.base);
}

static uint32_t qt_key_to_xkb_sym(int key)
{
    // This will probably only be used for IME input.
    // Most of the keys sets text string that can be
    // forwarded with committed signal.
    //
    // Keys that needs to be sent as real key event
    // are handled here.

    switch (key) {
    case Qt::Key_Backspace: return WPE_KEY_BackSpace;
    case Qt::Key_Enter: return WPE_KEY_Return;
    case Qt::Key_Return: return WPE_KEY_Return;
    default: return 0;
    }
}

void WPEQtViewBackend::dispatchKeyEvent(QKeyEvent* event, bool state)
{
    // IME input
    if (!event->nativeVirtualKey() && !event->nativeScanCode()) {
        if (!event->text().isEmpty()) {
            if (event->type() == QEvent::KeyPress)
                g_signal_emit_by_name(m_view->m_imContext, "committed", qPrintable(event->text()));
            return;
        }
    }

    uint32_t keysym = event->nativeVirtualKey();
    if (!keysym)
        keysym = qt_key_to_xkb_sym(event->key());

    uint32_t modifiers = 0;
    Qt::KeyboardModifiers qtModifiers = event->modifiers();
    if (!qtModifiers)
        qtModifiers = QGuiApplication::keyboardModifiers();

    if (qtModifiers & Qt::ShiftModifier)
        modifiers |= wpe_input_keyboard_modifier_shift;

    if (qtModifiers & Qt::ControlModifier)
        modifiers |= wpe_input_keyboard_modifier_control;
    if (qtModifiers & Qt::MetaModifier)
        modifiers |= wpe_input_keyboard_modifier_meta;
    if (qtModifiers & Qt::AltModifier)
        modifiers |= wpe_input_keyboard_modifier_alt;

    struct wpe_input_keyboard_event wpeEvent = { static_cast<uint32_t>(event->timestamp()),
        keysym, event->nativeScanCode(), state, modifiers };
    wpe_view_backend_dispatch_keyboard_event(backend(), &wpeEvent);
}

void WPEQtViewBackend::dispatchTouchEvent(QTouchEvent* event)
{
    std::vector<wpe_input_touch_event_raw> touches;
    wpe_input_touch_event_type grandType = wpe_input_touch_event_type_null;

    for (auto& point : event->touchPoints()) {
        wpe_input_touch_event_type eventType;

        switch (point.state()) {
        case Qt::TouchPointPressed:
            eventType = wpe_input_touch_event_type_down;
            break;
        case Qt::TouchPointMoved:
            eventType = wpe_input_touch_event_type_motion;
            break;
        case Qt::TouchPointReleased:
            eventType = wpe_input_touch_event_type_up;
            break;
        default:
            eventType = wpe_input_touch_event_type_null;
            break;
        }

        struct wpe_input_touch_event_raw rawEvent = {
            eventType,
            static_cast<uint32_t>(event->timestamp()),
            point.id(),
            static_cast<int32_t>(point.pos().x() * m_scale),
            static_cast<int32_t>(point.pos().y() * m_scale)
        };
        touches.push_back(rawEvent);
    }
    
    struct wpe_input_touch_event wpeEvent = {
        touches.data(),
        touches.size(),
        grandType,
        static_cast<int32_t>(0),
        static_cast<uint32_t>(event->timestamp()),
        modifiers()
    };

    wpe_view_backend_dispatch_touch_event(backend(), &wpeEvent);
}
