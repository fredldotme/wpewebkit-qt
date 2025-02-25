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

#pragma once

#include "config.h"

#include <QQmlEngine>
#include <QQuickItem>
#include <QSharedPointer>
#include <QUrl>
#include <QGeoPositionInfoSource>
#include <memory>
#include <wpe/webkit.h>
#include <wtf/glib/GRefPtr.h>

class WPEQtViewBackend;
class WPEQtViewLoadRequest;

class WPEQtView : public QQuickItem {
    Q_OBJECT
    Q_DISABLE_COPY(WPEQtView)
    Q_PROPERTY(QUrl url READ url WRITE setUrl NOTIFY urlChanged)
    Q_PROPERTY(bool loading READ isLoading NOTIFY loadingChanged)
    Q_PROPERTY(int loadProgress READ loadProgress NOTIFY loadProgressChanged)
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY loadingChanged)
    Q_PROPERTY(bool canGoForward READ canGoForward NOTIFY loadingChanged)
    Q_PROPERTY(QColor themeColor READ themeColor NOTIFY themeColorChanged)
    Q_ENUMS(LoadStatus)

public:
    enum LoadStatus {
        LoadStartedStatus,
        LoadStoppedStatus,
        LoadSucceededStatus,
        LoadFailedStatus
    };

    WPEQtView(QQuickItem* parent = nullptr);
    ~WPEQtView();
    QSGNode* updatePaintNode(QSGNode*, UpdatePaintNodeData*) final;

    void triggerUpdate() { QMetaObject::invokeMethod(this, "update"); };

    QUrl url() const;
    void setUrl(const QUrl&);
    int loadProgress() const;
    QString title() const;
    bool canGoBack() const;
    bool isLoading() const;
    bool canGoForward() const;
    QColor themeColor() const;

    void makeFileChooserRequest(WebKitFileChooserRequest* request);

public Q_SLOTS:
    void goBack();
    void goForward();
    void reload();
    void stop();
    void loadHtml(const QString& html, const QUrl& baseUrl = QUrl());
    void runJavaScript(const QString& script, const QJSValue& callback = QJSValue());
    void confirmFileSelection(const QStringList files);
    void cancelFileSelection();
    void startLocationServices();
    void stopLocationServices();

Q_SIGNALS:
    void webViewCreated();
    void urlChanged();
    void titleChanged();
    void loadingChanged(WPEQtViewLoadRequest* loadRequest);
    void loadProgressChanged();
    void themeColorChanged();
    void webProcessCrashed();
    void fileSelectionRequested(const bool multiple, const QStringList mimeTypes);

protected:
    bool errorOccured() const { return m_errorOccured; };
    void setErrorOccured(bool errorOccured) { m_errorOccured = errorOccured; };

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;
#else
    void geometryChanged(const QRectF& newGeometry, const QRectF& oldGeometry) override;
#endif

    void hoverEnterEvent(QHoverEvent*) override;
    void hoverLeaveEvent(QHoverEvent*) override;
    void hoverMoveEvent(QHoverEvent*) override;

    void mouseMoveEvent(QMouseEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;

    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;

    void touchEvent(QTouchEvent*) override;

    void inputMethodEvent(QInputMethodEvent*) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

private Q_SLOTS:
    void configureWindow();
    void createWebView();

private:
    static void notifyUrlChangedCallback(WPEQtView*);
    static void notifyTitleChangedCallback(WPEQtView*);
    static void notifyLoadProgressCallback(WPEQtView*);
    static void notifyLoadChangedCallback(WebKitWebView*, WebKitLoadEvent, WPEQtView*);
    static void notifyLoadFailedCallback(WebKitWebView*, WebKitLoadEvent, const gchar* failingURI, GError*, WPEQtView*);
    static void notifyThemeColorChangedCallback(WPEQtView*);
    static void notifyWebProcessTerminatedCallback(WebKitWebView*, WebKitWebProcessTerminationReason, WPEQtView*);
    static void notifyRunFileChooserCallback(WebKitWebView*, WebKitFileChooserRequest* request, WPEQtView*);
    static void notifyPermissionRequestCallback(WebKitWebView *web_view, WebKitPermissionRequest *permission_request, WPEQtView* view);
    static void notifyLocationManagerStart(WebKitWebView*, WebKitGeolocationManager* manager, WPEQtView*);
    static void notifyLocationManagerStop(WebKitWebView*, WebKitGeolocationManager* manager, WPEQtView*);
    static void *createRequested(WebKitWebView*, WebKitNavigationAction*, WPEQtView*);

    GRefPtr<WebKitWebView> m_webView;
    GRefPtr<WebKitNetworkSession> m_networkSession;
    GRefPtr<WebKitWebContext> m_webContext;
    WebKitFileChooserRequest* m_currentFileChooserRequest { nullptr };
    QUrl m_url;
    QString m_html;
    QUrl m_baseUrl;
    QSizeF m_size;
    WPEQtViewBackend* m_backend { nullptr };
    bool m_errorOccured { false };
    WebKitInputMethodContext *m_imContext = nullptr;
    WebKitGeolocationManager *m_locationManager = nullptr;
    QSharedPointer<QGeoPositionInfoSource> m_locationSource;

    friend class WPEQtViewBackend;
};
