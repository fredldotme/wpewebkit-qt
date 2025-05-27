/*
 * Copyright (C) 2024 Igalia S.L.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "WPEDisplayQtQuick.h"

#include "WPEToplevelQtQuick.h"
#include "WPEViewQtQuick.h"

#include <epoxy/egl.h>

#include <QGuiApplication>
#include <qpa/qplatformnativeinterface.h>
#include <wtf/glib/GRefPtr.h>
#include <wtf/glib/WTFGType.h>
#include <wtf/text/CString.h>

#ifndef EGL_DRM_RENDER_NODE_FILE_EXT
#define EGL_DRM_RENDER_NODE_FILE_EXT 0x3377
#endif

/**
 * WPEDisplayQtQuick:
 *
 */
struct _WPEDisplayQtQuickPrivate {
    EGLDisplay eglDisplay;
};
WEBKIT_DEFINE_FINAL_TYPE(WPEDisplayQtQuick, wpe_display_qtquick, WPE_TYPE_DISPLAY, WPEDisplay)

static gboolean wpeDisplayQtQuickConnect(WPEDisplay* display, GError** error)
{
    auto* priv = WPE_DISPLAY_QTQUICK(display)->priv;

    auto eglDisplay = static_cast<EGLDisplay>(QGuiApplication::platformNativeInterface()->nativeResourceForIntegration("eglDisplay"));
    if (!eglDisplay) {
        g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Failed to initialize rendering: Cannot access EGL display via Qt");
        return FALSE;
    }

    priv->eglDisplay = eglDisplay;

    return TRUE;
}

static WPEView* wpeDisplayQtQuickCreateView(WPEDisplay* display)
{
    auto* displayQt = WPE_DISPLAY_QTQUICK(display);
    auto* view = wpe_view_qtquick_new(displayQt);

    GRefPtr<WPEToplevel> toplevel = adoptGRef(wpe_toplevel_qtquick_new(displayQt));
    wpe_view_set_toplevel(view, toplevel.get());

    return view;
}

static gpointer wpeDisplayQtQuickGetEGLDisplay(WPEDisplay* display, GError**)
{
    return WPE_DISPLAY_QTQUICK(display)->priv->eglDisplay;
}

static void wpe_display_qtquick_class_init(WPEDisplayQtQuickClass* displayQtQuickClass)
{
    WPEDisplayClass* displayClass = WPE_DISPLAY_CLASS(displayQtQuickClass);
    displayClass->connect = wpeDisplayQtQuickConnect;
    displayClass->create_view = wpeDisplayQtQuickCreateView;
    displayClass->get_egl_display = wpeDisplayQtQuickGetEGLDisplay;
}

WPEDisplay* wpe_display_qtquick_new(void)
{
    return WPE_DISPLAY(g_object_new(WPE_TYPE_DISPLAY_QTQUICK, nullptr));
}
