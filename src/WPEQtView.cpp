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
#include "WPEQtView.h"

#include "WPEQtViewBackend.h"
#include "WPEQtViewLoadRequest.h"
#include "WPEQtViewLoadRequestPrivate.h"
#include "WPEQtImContext.h"
#include <QGuiApplication>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QScreen>
#include <QtGlobal>
#include <qpa/qplatformnativeinterface.h>
#include <wtf/glib/GUniquePtr.h>

/*!
  \qmltype WPEView
  \inqmlmodule org.wpewebkit.qtwpe
  \brief A component for displaying web content.

  WPEView is a component for displaying web content which is implemented using native
  APIs on the platforms where this is available, thus it does not necessarily require
  including a full web browser stack as part of the application.

  WPEView provides an API compatible with Qt's QtWebView component. However
  WPEView is limited to Linux platforms supporting EGL KHR extensions. WPEView
  was successfully tested with the EGLFS and Wayland-EGL QPAs.
*/
WPEQtView::WPEQtView(QQuickItem* parent)
    : QQuickItem(parent)
{
    connect(this, &QQuickItem::windowChanged, this, &WPEQtView::configureWindow);
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::AllButtons);
    setAcceptHoverEvents(true);
    setAcceptTouchEvents(true);
}

WPEQtView::~WPEQtView()
{
    g_signal_handlers_disconnect_by_func(m_webView.get(), reinterpret_cast<gpointer>(notifyUrlChangedCallback), this);
    g_signal_handlers_disconnect_by_func(m_webView.get(), reinterpret_cast<gpointer>(notifyTitleChangedCallback), this);
    g_signal_handlers_disconnect_by_func(m_webView.get(), reinterpret_cast<gpointer>(notifyLoadChangedCallback), this);
    g_signal_handlers_disconnect_by_func(m_webView.get(), reinterpret_cast<gpointer>(notifyLoadFailedCallback), this);
    g_signal_handlers_disconnect_by_func(m_webView.get(), reinterpret_cast<gpointer>(notifyLoadProgressCallback), this);
    g_signal_handlers_disconnect_by_func(m_webView.get(), reinterpret_cast<gpointer>(notifyThemeColorChangedCallback), this);
    g_signal_handlers_disconnect_by_func(m_webView.get(), reinterpret_cast<gpointer>(notifyWebProcessTerminatedCallback), this);
    g_signal_handlers_disconnect_by_func(m_webView.get(), reinterpret_cast<gpointer>(notifyRunFileChooserCallback), this);
    g_signal_handlers_disconnect_by_func(m_webView.get(), reinterpret_cast<gpointer>(notifyPermissionRequestCallback), this);
    g_signal_handlers_disconnect_by_func(m_webView.get(), reinterpret_cast<gpointer>(notifyLocationManagerStart), this);
    g_signal_handlers_disconnect_by_func(m_webView.get(), reinterpret_cast<gpointer>(notifyLocationManagerStop), this);
    g_signal_handlers_disconnect_by_func(m_webView.get(), reinterpret_cast<gpointer>(createRequested), this);

    webkit_web_view_terminate_web_process(m_webView.get());
}

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
void WPEQtView::geometryChange(const QRectF& newGeometry, const QRectF&)
#else
void WPEQtView::geometryChanged(const QRectF& newGeometry, const QRectF&)
#endif
{
    m_size = newGeometry.size();
    if (m_backend)
        m_backend->resize(newGeometry.size());
}

void WPEQtView::configureWindow()
{
    auto* win = window();
    if (!win)
        return;

    win->setSurfaceType(QWindow::OpenGLSurface);

    if (win->isSceneGraphInitialized())
        createWebView();
    else
        connect(win, &QQuickWindow::sceneGraphInitialized, this, &WPEQtView::createWebView);
}

static QOpenGLContext *glContext(QQuickWindow *window)
{
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
    RELEASE_ASSERT_WITH_MESSAGE(window->rendererInterface()->graphicsApi() != QSGRendererInterface::OpenGL, "OpenGL renderer required");
    if (window->rendererInterface()->graphicsApi() != QSGRendererInterface::OpenGL)
        return nullptr;
    return static_cast<QOpenGLContext*>(window->rendererInterface()->getResource(window, QSGRendererInterface::OpenGLContextResource));
#else
    return window->openglContext();
#endif
}

void WPEQtView::createWebView()
{
    if (m_backend)
        return;

    auto display = static_cast<EGLDisplay>(QGuiApplication::platformNativeInterface()->nativeResourceForIntegration("egldisplay"));
    auto* context = glContext(window());
    std::unique_ptr<WPEQtViewBackend> backend = WPEQtViewBackend::create(m_size, context, display, QPointer<WPEQtView>(this));
    RELEASE_ASSERT_WITH_MESSAGE(backend, "EGL initialization failed");
    if (!backend)
        return;

    const auto appName = QGuiApplication::applicationName();
    const auto dataHome = QString::fromUtf8(qgetenv("XDG_DATA_HOME"));
    const auto cacheHome = QString::fromUtf8(qgetenv("XDG_CACHE_HOME"));

    const auto cacheDirRoot = QStringLiteral("%1/%2").arg(cacheHome, appName);
    const auto dataDirRoot = QStringLiteral("%1/%2/data").arg(dataHome, appName);
    const auto extensionsDirRoot = QStringLiteral("%1/%2/extensions").arg(dataHome, appName);
    const auto cookiesFile = QStringLiteral("%1/%2/cookies.sqlite").arg(dataHome, appName);

    m_networkSession = adoptGRef(webkit_network_session_new(dataDirRoot.toStdString().c_str(), cacheDirRoot.toStdString().c_str()));
    m_webContext = adoptGRef(webkit_web_context_new());

    webkit_network_session_set_persistent_credential_storage_enabled(m_networkSession.get(), TRUE);

    auto* cookieManager = webkit_network_session_get_cookie_manager(m_networkSession.get());
    webkit_cookie_manager_set_persistent_storage(cookieManager, cookiesFile.toStdString().c_str(), WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);

    webkit_web_context_set_web_process_extensions_directory(m_webContext.get(), extensionsDirRoot.toStdString().c_str());

    const auto userAgent = QStringLiteral("Mozilla/5.0 (X11; Ubuntu; Linux) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.5 Safari/605.1.15 (like iPhone OS)");

    m_backend = backend.get();
    auto settings = adoptGRef(webkit_settings_new_with_settings(
        "enable-developer-extras", TRUE,
        "enable-webgl", TRUE,
        "enable-smooth-scrolling", TRUE,
        "enable-mediasource", TRUE,
        "enable-fullscreen", TRUE,
        "enable-html5-database", TRUE,
        "enable-html5-local-storage", TRUE,
        //"draw-compositing-indicators", TRUE,
        "enable-site-specific-quirks", TRUE,
        "user-agent", userAgent.toStdString().c_str(),
        nullptr)
    );

    m_webView = adoptGRef(WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
        "backend", webkit_web_view_backend_new(m_backend->backend(), [](gpointer data) {
            delete static_cast<WPEQtViewBackend*>(data);
        }, backend.release()),
        "settings", settings.get(),
        "network-session", m_networkSession.get(),
        "web-context", m_webContext.get(),
        nullptr)));

    m_backend->setScaleFactor(window()->devicePixelRatio());

    m_imContext = wpeqt_im_context_new(this);
    webkit_web_view_set_input_method_context(m_webView.get(), m_imContext);

    g_signal_connect_swapped(m_webView.get(), "notify::uri", G_CALLBACK(notifyUrlChangedCallback), this);
    g_signal_connect_swapped(m_webView.get(), "notify::title", G_CALLBACK(notifyTitleChangedCallback), this);
    g_signal_connect_swapped(m_webView.get(), "notify::estimated-load-progress", G_CALLBACK(notifyLoadProgressCallback), this);
    g_signal_connect_swapped(m_webView.get(), "notify::theme-color", G_CALLBACK(notifyThemeColorChangedCallback), this);
    g_signal_connect(m_webView.get(), "load-changed", G_CALLBACK(notifyLoadChangedCallback), this);
    g_signal_connect(m_webView.get(), "load-failed", G_CALLBACK(notifyLoadFailedCallback), this);
    g_signal_connect(m_webView.get(), "create", G_CALLBACK(createRequested), this);
    g_signal_connect(m_webView.get(), "web-process-terminated", G_CALLBACK(notifyWebProcessTerminatedCallback), this);
    g_signal_connect(m_webView.get(), "run-file-chooser", G_CALLBACK(notifyRunFileChooserCallback), this);

    g_signal_connect(m_webView.get(), "permission-request", G_CALLBACK(notifyPermissionRequestCallback), this);

    m_locationManager = webkit_web_context_get_geolocation_manager(m_webContext.get());
    g_signal_connect(m_locationManager, "start", G_CALLBACK(notifyLocationManagerStart), this);
    g_signal_connect(m_locationManager, "stop", G_CALLBACK(notifyLocationManagerStop), this);

    if (!m_url.isEmpty())
        webkit_web_view_load_uri(m_webView.get(), m_url.toString().toUtf8().constData());
    else if (!m_html.isEmpty())
        webkit_web_view_load_html(m_webView.get(), m_html.toUtf8().constData(), m_baseUrl.toString().toUtf8().constData());

    Q_EMIT webViewCreated();
}

void WPEQtView::notifyUrlChangedCallback(WPEQtView* view)
{
    Q_EMIT view->urlChanged();
}

void WPEQtView::notifyTitleChangedCallback(WPEQtView* view)
{
    Q_EMIT view->titleChanged();
}

void WPEQtView::notifyLoadProgressCallback(WPEQtView* view)
{
    Q_EMIT view->loadProgressChanged();
}

void WPEQtView::notifyLoadChangedCallback(WebKitWebView*, WebKitLoadEvent event, WPEQtView* view)
{
    bool statusSet = false;
    WPEQtView::LoadStatus loadStatus;
    switch (event) {
    case WEBKIT_LOAD_STARTED:
        loadStatus = WPEQtView::LoadStatus::LoadStartedStatus;
        statusSet = true;
        break;
    case WEBKIT_LOAD_FINISHED:
        loadStatus = WPEQtView::LoadStatus::LoadSucceededStatus;
        statusSet = !view->errorOccured();
        view->setErrorOccured(false);
        break;
    default:
        break;
    }

    if (statusSet) {
        WPEQtViewLoadRequestPrivate loadRequestPrivate(view->url(), loadStatus, "");
        std::unique_ptr<WPEQtViewLoadRequest> loadRequest = std::make_unique<WPEQtViewLoadRequest>(loadRequestPrivate);
        Q_EMIT view->loadingChanged(loadRequest.get());
    }
}

void WPEQtView::notifyLoadFailedCallback(WebKitWebView*, WebKitLoadEvent, const gchar* failingURI, GError* error, WPEQtView* view)
{
    view->setErrorOccured(true);

    WPEQtView::LoadStatus loadStatus;
    if (g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED))
        loadStatus = WPEQtView::LoadStatus::LoadStoppedStatus;
    else
        loadStatus = WPEQtView::LoadStatus::LoadFailedStatus;

    WPEQtViewLoadRequestPrivate loadRequestPrivate(QUrl(QString(failingURI)), loadStatus, error->message);
    std::unique_ptr<WPEQtViewLoadRequest> loadRequest = std::make_unique<WPEQtViewLoadRequest>(loadRequestPrivate);
    Q_EMIT view->loadingChanged(loadRequest.get());
}

void WPEQtView::notifyThemeColorChangedCallback(WPEQtView* view)
{
    qDebug() << Q_FUNC_INFO << "";
    Q_EMIT view->themeColorChanged();
}

void WPEQtView::notifyWebProcessTerminatedCallback(WebKitWebView*, WebKitWebProcessTerminationReason, WPEQtView* view)
{
    Q_EMIT view->webProcessCrashed();
}

void WPEQtView::notifyRunFileChooserCallback(WebKitWebView*, WebKitFileChooserRequest* request, WPEQtView* view)
{
    view->makeFileChooserRequest(request);
}

void WPEQtView::notifyPermissionRequestCallback(WebKitWebView *web_view, WebKitPermissionRequest *request, WPEQtView* view)
{
    if (!request)
        return;

    webkit_permission_request_allow(request);
}

void WPEQtView::notifyLocationManagerStart(WebKitWebView*, WebKitGeolocationManager* manager, WPEQtView* view)
{
    view->startLocationServices();
}

void WPEQtView::notifyLocationManagerStop(WebKitWebView*, WebKitGeolocationManager* manager, WPEQtView* view)
{
    view->stopLocationServices();
}

void WPEQtView::makeFileChooserRequest(WebKitFileChooserRequest* request)
{
    if (!request)
        return;

    m_currentFileChooserRequest = request;
    g_object_ref(m_currentFileChooserRequest);

    const auto multiple = webkit_file_chooser_request_get_select_multiple(m_currentFileChooserRequest);
    QStringList mimes;

    auto retrievedMimes = webkit_file_chooser_request_get_mime_types(m_currentFileChooserRequest);
    int i = 0;
    if (retrievedMimes) {
        while (true) {
            const auto mime = retrievedMimes[i];
            if (!mime)
                break;

            mimes << QString::fromUtf8(mime);
            ++i;
        }
    } else {
        mimes << QStringLiteral("application/octet-stream");
    }

    Q_EMIT fileSelectionRequested(multiple, mimes);
}

void WPEQtView::confirmFileSelection(const QStringList files)
{
    if (!m_currentFileChooserRequest)
        return;

    std::vector<std::string> stdFiles;
    for (const auto& file : files) {
        const auto resolved = QUrl(file).toLocalFile();
        stdFiles.push_back(resolved.toStdString());
    }

    std::vector<const gchar*> filesToSelect;
    for (const auto& file : stdFiles) {
        filesToSelect.push_back(file.c_str());
    }
    filesToSelect.push_back(nullptr);

    webkit_file_chooser_request_select_files(m_currentFileChooserRequest, (const gchar* const*)filesToSelect.data());
    m_currentFileChooserRequest = nullptr;
}

void WPEQtView::cancelFileSelection()
{
    if (!m_currentFileChooserRequest)
        return;

    webkit_file_chooser_request_cancel(m_currentFileChooserRequest);
    m_currentFileChooserRequest = nullptr;
}

void *WPEQtView::createRequested(WebKitWebView* web_view, WebKitNavigationAction* action, WPEQtView*)
{
    webkit_web_view_load_request(web_view, webkit_navigation_action_get_request(action));
    return nullptr;
}

QSGNode* WPEQtView::updatePaintNode(QSGNode* node, UpdatePaintNodeData*)
{
    if (!m_webView || !m_backend)
        return node;

    auto* textureNode = static_cast<QSGSimpleTextureNode*>(node);
    if (!textureNode)
        textureNode = new QSGSimpleTextureNode();

    GLuint textureId = m_backend->texture(glContext(window()));
    if (!textureId)
        return node;

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
    QSGTexture *texture = QNativeInterface::QSGOpenGLTexture::fromNative(textureId, window(), m_size.toSize(), QQuickWindow::TextureHasAlphaChannel);
#elif (QT_VERSION >= QT_VERSION_CHECK(5, 15, 0))
    auto texture = window()->createTextureFromNativeObject(QQuickWindow::NativeObjectTexture, &textureId, 0, m_size.toSize(), QQuickWindow::TextureHasAlphaChannel);
#else
    auto texture = window()->createTextureFromId(textureId, m_size.toSize(), QQuickWindow::TextureHasAlphaChannel);
#endif
    textureNode->setTexture(texture);
    textureNode->setRect(boundingRect());
    return textureNode;
}

QUrl WPEQtView::url() const
{
    if (!m_webView)
        return m_url;

    const gchar* uri = webkit_web_view_get_uri(m_webView.get());
    return uri ? QUrl(QString(uri)) : m_url;
}

/*!
  \qmlproperty url WPEView::url

  The URL of currently loaded web page. Changing this will trigger
  loading new content.

  The URL is used as-is. URLs that originate from user input should
  be parsed with QUrl::fromUserInput().

  \note The WPEView does not support loading content through the Qt Resource system.
*/
void WPEQtView::setUrl(const QUrl& url)
{
    if (url == m_url)
        return;

    m_errorOccured = false;
    m_url = url;
    if (m_webView)
        webkit_web_view_load_uri(m_webView.get(), m_url.toString().toUtf8().constData());
}

/*!
  \qmlproperty int WPEView::loadProgress
  \readonly

  The current load progress of the web content, represented as
  an integer between 0 and 100.
*/
int WPEQtView::loadProgress() const
{
    if (!m_webView)
        return 0;

    return webkit_web_view_get_estimated_load_progress(m_webView.get()) * 100;
}

/*!
  \qmlproperty string WPEView::title
  \readonly

  The title of the currently loaded web page.
*/
QString WPEQtView::title() const
{
    if (!m_webView)
        return "";

    return webkit_web_view_get_title(m_webView.get());
}

/*!
  \qmlproperty bool WPEView::canGoBack
  \readonly

  Holds \c true if it's currently possible to navigate back in the web history.
*/
bool WPEQtView::canGoBack() const
{
    if (!m_webView)
        return false;

    return webkit_web_view_can_go_back(m_webView.get());
}

/*!
  \qmlproperty bool WPEView::loading
  \readonly

  Holds \c true if the WPEView is currently in the process of loading
  new content, \c false otherwise.

  \sa loadingChanged()
*/

/*!
  \qmlsignal WPEView::loadingChanged(WPEViewLoadRequest loadRequest)

  This signal is emitted when the state of loading the web content changes.
  By handling this signal it's possible, for example, to react to page load
  errors.

  The \a loadRequest parameter holds the \e url and \e status of the request,
  as well as an \e errorString containing an error message for a failed
  request.

  \sa WPEViewLoadRequest
*/
bool WPEQtView::isLoading() const
{
    if (!m_webView)
        return false;

    return webkit_web_view_is_loading(m_webView.get());
}

/*!
  \qmlproperty bool WPEView::canGoForward
  \readonly

  Holds \c true if it's currently possible to navigate forward in the web history.
*/
bool WPEQtView::canGoForward() const
{
    if (!m_webView)
        return false;

    return webkit_web_view_can_go_forward(m_webView.get());
}

QColor WPEQtView::themeColor() const
{
    if (!m_webView)
        return QColor(Qt::white);

    WebKitColor color { 1.0, 1.0, 1.0, 1.0 };
    webkit_web_view_get_theme_color(m_webView.get(), &color);
    const auto qtColor = QColor(color.red * 255, color.green * 255, color.blue * 255, color.alpha * 255);
    return qtColor;
}

/*!
  \qmlmethod void WPEView::goBack()

  Navigates back in the web history.
*/
void WPEQtView::goBack()
{
    if (m_webView)
        webkit_web_view_go_back(m_webView.get());
}

/*!
  \qmlmethod void WPEView::goForward()

  Navigates forward in the web history.
*/
void WPEQtView::goForward()
{
    if (m_webView)
        webkit_web_view_go_forward(m_webView.get());
}

/*!
  \qmlmethod void WPEView::reload()

  Reloads the current \l url.
*/
void WPEQtView::reload()
{
    if (m_webView)
        webkit_web_view_reload(m_webView.get());
}

/*!
  \qmlmethod void WPEView::stop()

  Stops loading the current \l url.
*/
void WPEQtView::stop()
{
    if (m_webView)
        webkit_web_view_stop_loading(m_webView.get());
}

void WPEQtView::startLocationServices()
{
    QSharedPointer<QGeoPositionInfoSource> source = QSharedPointer<QGeoPositionInfoSource>(QGeoPositionInfoSource::createDefaultSource(this));
    if (source.get()) {
        connect(source.get(), &QGeoPositionInfoSource::positionUpdated,
                this,[=](QGeoPositionInfo info){
            // Update WebKit location
            WebKitGeolocationPosition* position =
                webkit_geolocation_position_new(info.coordinate().longitude(),
                                                info.coordinate().latitude(),
                                                15.0);
            webkit_geolocation_manager_update_position(webkit_web_context_get_geolocation_manager(m_webContext.get()), position);
            webkit_geolocation_position_free(position);
        });
        source->startUpdates();
        m_locationSource = source;
    }
}

void WPEQtView::stopLocationServices()
{
    if (!m_locationSource)
        return;

    m_locationSource->stopUpdates();
    m_locationSource.reset(nullptr);
}

/*!
  \qmlmethod void WPEView::loadHtml(string html, url baseUrl)

  Loads the specified \a html content to the web view.

  This method offers a lower-level alternative to the \l url property,
  which references HTML pages via URL.

  External objects such as stylesheets or images referenced in the HTML
  document should be located relative to \a baseUrl. For example, if \a html
  is retrieved from \c http://www.example.com/documents/overview.html, which
  is the base URL, then an image referenced with the relative url, \c diagram.png,
  should be at \c{http://www.example.com/documents/diagram.png}.

  \note The WPEView does not support loading content through the Qt Resource system.

  \sa url
*/
void WPEQtView::loadHtml(const QString& html, const QUrl& baseUrl)
{
    m_html = html;
    m_baseUrl = baseUrl;
    m_errorOccured = false;

    if (m_webView)
        webkit_web_view_load_html(m_webView.get(), html.toUtf8().constData(), baseUrl.toString().toUtf8().constData());
}

struct JavascriptCallbackData {
    JavascriptCallbackData(QJSValue cb, QPointer<WPEQtView> obj)
        : callback(cb)
        , object(obj) { }

    QJSValue callback;
    QPointer<WPEQtView> object;
};

static void jsAsyncReadyCallback(GObject* object, GAsyncResult* result, gpointer userData)
{
    GUniqueOutPtr<GError> error;
    std::unique_ptr<JavascriptCallbackData> data(reinterpret_cast<JavascriptCallbackData*>(userData));

    JSCValue* value = nullptr;

#if WEBKIT_CHECK_VERSION(2, 40, 0)
    value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW (object), result, &error.outPtr());
    if (!value) {
        qWarning("Error running javascript: %s", error->message);
        return;
    }
#else
    WebKitJavascriptResult* jsResult = webkit_web_view_run_javascript_finish(WEBKIT_WEB_VIEW(object), result, &error.outPtr());
    if (!jsResult) {
        qWarning("Error running javascript: %s", error->message);
        return;
    }
    value = webkit_javascript_result_get_js_value(jsResult);
#endif

    if (data->object.data()) {
        QQmlEngine* engine = qmlEngine(data->object.data());
        if (!engine) {
            qWarning("No JavaScript engine, unable to handle JavaScript callback!");
        } else {
            QJSValueList args;
            QVariant variant;
            // FIXME: Handle more value types?
            if (jsc_value_is_string(value)) {
                GUniquePtr<gchar> strValue(jsc_value_to_string(value));
                JSCContext* context = jsc_value_get_context(value);
                JSCException* exception = jsc_context_get_exception(context);
                if (exception) {
                    qWarning("Error running javascript: %s", jsc_exception_get_message(exception));
                    jsc_context_clear_exception(context);
                } else
                    variant.setValue(QString(g_strdup(strValue.get())));
            }
            args.append(engine->toScriptValue(variant));
            data->callback.call(args);
        }
    }
#if !WEBKIT_CHECK_VERSION(2, 40, 0)
    webkit_javascript_result_unref(jsResult);
#endif
}

/*!
  \qmlmethod void WPEView::runJavaScript(string script, variant callback)

  Runs the specified JavaScript.
  In case a \a callback function is provided, it will be invoked after the \a script
  finished running.

  \badcode
  runJavaScript("document.title", function(result) { console.log(result); });
  \endcode
*/
void WPEQtView::runJavaScript(const QString& script, const QJSValue& callback)
{
    std::unique_ptr<JavascriptCallbackData> data = std::make_unique<JavascriptCallbackData>(callback, QPointer<WPEQtView>(this));
#if WEBKIT_CHECK_VERSION(2, 40, 0)
    webkit_web_view_evaluate_javascript(m_webView.get(), script.toUtf8().constData(), -1, nullptr, nullptr, nullptr, jsAsyncReadyCallback, data.release());
#else
    webkit_web_view_run_javascript(m_webView.get(), script.toUtf8().constData(), nullptr, jsAsyncReadyCallback, data.release());
#endif
}

void WPEQtView::mouseMoveEvent(QMouseEvent* event)
{
    if (m_backend)
        m_backend->dispatchMouseMoveEvent(event);
}

void WPEQtView::mousePressEvent(QMouseEvent* event)
{
    forceActiveFocus();
    if (m_backend)
        m_backend->dispatchMousePressEvent(event);
}

void WPEQtView::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_backend)
        m_backend->dispatchMouseReleaseEvent(event);
}

void WPEQtView::hoverEnterEvent(QHoverEvent* event)
{
    if (m_backend)
        m_backend->dispatchHoverEnterEvent(event);
}

void WPEQtView::hoverLeaveEvent(QHoverEvent* event)
{
    if (m_backend)
        m_backend->dispatchHoverLeaveEvent(event);
}

void WPEQtView::hoverMoveEvent(QHoverEvent* event)
{
    if (m_backend)
        m_backend->dispatchHoverMoveEvent(event);
}

void WPEQtView::wheelEvent(QWheelEvent* event)
{
    if (m_backend)
        m_backend->dispatchWheelEvent(event);
}

void WPEQtView::keyPressEvent(QKeyEvent* event)
{
    if (m_backend)
        m_backend->dispatchKeyEvent(event, true);
}

void WPEQtView::keyReleaseEvent(QKeyEvent* event)
{
    if (m_backend)
        m_backend->dispatchKeyEvent(event, false);
}

void WPEQtView::touchEvent(QTouchEvent* event)
{
    forceActiveFocus();
    if (m_backend)
        m_backend->dispatchTouchEvent(event);
}

void WPEQtView::inputMethodEvent(QInputMethodEvent* event)
{
    wpeqt_im_context_event(WPEQT_IM_CONTEXT(m_imContext), event);
}

QVariant WPEQtView::inputMethodQuery(Qt::InputMethodQuery query) const
{
    QVariant out;
    wpeqt_im_context_query(WPEQT_IM_CONTEXT(m_imContext), query, &out);
    return out;
}
