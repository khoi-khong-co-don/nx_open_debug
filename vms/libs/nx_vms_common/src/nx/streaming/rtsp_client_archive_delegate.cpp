// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "rtsp_client_archive_delegate.h"

#include <QtConcurrent/QtConcurrentFilter>
#include <QtCore/QBuffer>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkCookie>
#include <QJsonDocument>
#include <QJsonObject>
extern "C"
{
// For declarations only.
#include <libavcodec/avcodec.h>
}

#include <api/network_proxy_factory.h>
#include <core/resource_management/resource_pool.h>
#include <core/resource/camera_history.h>
#include <core/resource/camera_resource.h>
#include <core/resource/media_server_resource.h>
#include <core/resource/resource_media_layout.h>
#include <nx/network/http/custom_headers.h>
#include <nx/reflect/string_conversion.h>
#include <nx/streaming/archive_stream_reader.h>
#include <nx/streaming/av_codec_media_context.h>
#include <nx/streaming/media_data_packet.h>
#include <nx/streaming/rtp/parsers/nx_rtp_parser.h>
#include <nx/streaming/rtp/rtp.h>
#include <nx/streaming/rtsp_client.h>
#include <nx/utils/suppress_exceptions.h>
#include <nx/vms/common/system_context.h>
#include <nx/vms/common/system_settings.h>
#include <utils/common/sleep.h>
#include <utils/common/synctime.h>
#include <utils/common/util.h>
#include <utils/media/av_codec_helper.h>
#include "./Logger_KhoiVH.h"
#include <chrono>

static const int MAX_RTP_BUFFER_SIZE = 65536;
static const int REOPEN_TIMEOUT = 1000;

namespace
{
static const QByteArray kMediaQualityParamName = "x-media-quality";
static const QByteArray kResolutionParamName = "x-resolution";
static const QByteArray kSpeedParamName = "x-speed";

QString resolutionToString(const QSize& size)
{
    if (size.width() > 0)
        return lit("%1x%2").arg(size.width()).arg(size.height());
    else
        return lit("%1p").arg(size.height());
}

bool isSpecialTimeValue(qint64 value)
{
    return value == DATETIME_NOW || value == qint64(AV_NOPTS_VALUE);
}

}

QnRtspClientArchiveDelegate::QnRtspClientArchiveDelegate(
        QnArchiveStreamReader* reader,
        nx::network::http::Credentials credentials,
        const QString& rtpLogTag,
        bool sleepIfEmptySocket)
    :
      QnAbstractArchiveDelegate(),
      m_rtspSession(new QnRtspClient(
                        QnRtspClient::Config{
                            .shouldGuessAuthDigest = true,
                            .backChannelAudioOnly = false,
                            .disableKeepAlive = true,
                            .sleepIfEmptySocket = sleepIfEmptySocket})),
      m_rtpData(0),
      m_tcpMode(true),
      m_position(DATETIME_NOW),
      m_lastMediaFlags(-1),
      m_closing(false),
      m_singleShotMode(false),
      m_sendedCSec(0),
      m_lastSeekTime(AV_NOPTS_VALUE),
      m_lastReceivedTime(AV_NOPTS_VALUE),
      m_blockReopening(false),
      m_quality(MEDIA_Quality_High),
      m_qualityFastSwitch(true),
      m_lastMinTimeTime(0),
      m_globalMinArchiveTime(AV_NOPTS_VALUE),
      m_forcedEndTime(AV_NOPTS_VALUE),
      m_isMultiserverAllowed(true),
      m_playNowModeAllowed(true),
      m_reader(reader),
      m_frameCnt(0),
      m_credentials(std::move(credentials)),
      m_maxSessionDurationMs(std::numeric_limits<qint64>::max()),
      m_rtpLogTag(rtpLogTag),
      m_sleepIfEmptySocket(sleepIfEmptySocket),
      m_lastError(CameraDiagnostics::NoErrorResult())
{
    qDebug() << "KHOI TAO QnRtspClientArchiveDelegate this -> " << this;
    m_rtspSession->setPlayNowModeAllowed(true); //< Default value.
    m_footageUpToDate.test_and_set();
    m_currentServerUpToDate.test_and_set();
    m_rtpDataBuffer = new quint8[MAX_RTP_BUFFER_SIZE];
    m_flags |= Flag_SlowSource;
    m_flags |= Flag_CanProcessNegativeSpeed;
    m_flags |= Flag_CanProcessMediaStep;
    m_flags |= Flag_CanSendMetadata;
    m_flags |= Flag_CanSeekImmediatly;
    m_flags |= Flag_CanOfflineHasVideo;

    // These signals are emitted from the same thread. It is safe to call close();
    auto closeIfExpired =
            [this]()
    {
        if (isConnectionExpired())
            close();
    };
    if (reader)
    {
        connect(reader, &QnLongRunnable::paused, this, closeIfExpired, Qt::DirectConnection);
        connect(reader, &QnArchiveStreamReader::waitForDataCanBeAccepted,
                this, closeIfExpired, Qt::DirectConnection);
    }
}


void QnRtspClientArchiveDelegate::setCamera(const QnSecurityCamResourcePtr& camera)
{
    qDebug() << "QnRtspClientArchiveDelegate::setCamera";
    if (m_camera == camera)
        return;

    if (m_camera && m_camera->systemContext())
        m_camera->systemContext()->cameraHistoryPool()->disconnect(this);

    m_camera = camera;

    if (!NX_ASSERT(m_camera && m_camera->systemContext()))
        return;

    m_server = camera->getParentServer();

    auto context = camera->systemContext();
    auto maxSessionDuration = context->globalSettings()->maxRtspConnectDuration();
    if (maxSessionDuration.count() > 0)
        m_maxSessionDurationMs = maxSessionDuration;
    else
        m_maxSessionDurationMs = std::chrono::milliseconds::max();

    connect(context->cameraHistoryPool(),
            &QnCameraHistoryPool::cameraHistoryChanged,
            this,
            [this](const QnSecurityCamResourcePtr& camera)
    {
        /* Ignore camera history if fixed server is set. */
        if (m_fixedServer || !m_isMultiserverAllowed)
            return;

        /* Ignore other cameras changes. */
        if (camera != m_camera)
            return;

        if (m_server != getServerOnTime(m_position))
            m_currentServerUpToDate.clear();
    });

    connect(context->cameraHistoryPool(),
            &QnCameraHistoryPool::cameraFootageChanged,
            this,
            [this](const QnSecurityCamResourcePtr& camera)
    {
        /* Ignore camera history if fixed server is set. */
        if (m_fixedServer || !m_isMultiserverAllowed)
            return;

        /* Ignore other cameras changes. */
        if (camera != m_camera)
            return;
        m_footageUpToDate.clear();
    });
    nx::network::SocketAddress serverHost = m_server->getPrimaryAddress();
    std::stringstream ss;
    ss << serverHost.toString();
    qDebug() << "serverHost:   " << QString::fromStdString(ss.str());
    std::string address = QString::fromStdString(ss.str()).toStdString();

    std::string port;

    port = address.substr(address.length() - 4);
    if (port == "7005")         //7005
    {
        qDebug() << "Server Oryza port 7005";
        m_isServerOryza = true;
    }
    else
    {
        m_isServerOryza = false;
    }
    setupRtspSession(camera, m_server, m_rtspSession.get());
}

void QnRtspClientArchiveDelegate::setFixedServer(const QnMediaServerResourcePtr &server)
{
    m_server = server;
    m_fixedServer = m_server;
}

void QnRtspClientArchiveDelegate::updateCredentials(
        const nx::network::http::Credentials& credentials)
{
    m_credentials = credentials;
    setupRtspSession(m_camera, m_server, m_rtspSession.get());
}

QnRtspClientArchiveDelegate::~QnRtspClientArchiveDelegate()
{
    if (isServerOryza())
    {
        QMutexLocker locker(&m_qmutex);
        if (m_isOpenedRTSP)
        {
            m_isOpenedRTSP = false;
            av_free(out_buffer);
            qDebug() << "~QnRtspClientArchiveDelegate 1";
            av_frame_free(&pFrameRGB);
            qDebug() << "~QnRtspClientArchiveDelegate 2";
            av_frame_free(&pFrame);
            av_packet_unref(_packet);
            qDebug() << "~QnRtspClientArchiveDelegate 3";
            if (_pCodecCtx != NULL) avcodec_close(_pCodecCtx);
            qDebug() << "~QnRtspClientArchiveDelegate 4";
            if (_pFormatCtx != NULL) avformat_close_input(&_pFormatCtx);
            sws_freeContext(imgConvertCtx);
            qDebug() << "~QnRtspClientArchiveDelegate 5";
        }
    }
    close();
    delete [] m_rtpDataBuffer;
}

QnMediaServerResourcePtr QnRtspClientArchiveDelegate::getNextMediaServerFromTime(const QnSecurityCamResourcePtr &camera, qint64 time)
{
    if (!camera)
        return QnMediaServerResourcePtr();
    if (m_fixedServer)
        return QnMediaServerResourcePtr();

    return camera->systemContext()->cameraHistoryPool()->getNextMediaServerAndPeriodOnTime(
                camera,
                time,
                m_rtspSession->getScale() >= 0,
                &m_serverTimePeriod);
}

namespace {



QString getUrl(const QnSecurityCamResourcePtr& camera, const QnMediaServerResourcePtr& _server)
{
    // if camera is null we can't get its parent in any case
    auto server = (_server || (!camera)) ? _server : camera->getParentServer();
    if (!server)
        return QString();

    qDebug() << nx::format("Server Stream RTSP    IP : %1   PORT:   %2").args(_server->getPrimaryAddressOryza(), _server->getPort());
    nx::network::SocketAddress rtsp = _server->getPrimaryAddress();
    std::stringstream ss;
    ss << rtsp.toString();
    qDebug() << "RTSP stream:   " << QString::fromStdString(ss.str());
    std::string address = QString::fromStdString(ss.str()).toStdString();

    std::string ipAddress;
    std::string port;
    QString url = "";
    size_t colonPos = address.find(':');

    if (colonPos == std::string::npos) {
        qDebug() << "Invalid input format.\n";
    }

    ipAddress = address.substr(0, colonPos);
    port = address.substr(colonPos + 1);

    if (port == "7005")         //7005
    {
        std::string urlstr = "rtsp://" + ipAddress + ":" + port + "/";
        url = QString::fromStdString(urlstr);
    }
    else{
        url = server->rtspUrl() + QLatin1Char('/');
        qDebug() << "getUrl   url:     "  << url;
    }
    if (camera)
        url += camera->getPhysicalId();
    else
        url += server->rtspUrl();
    qDebug() << "URL CAM IN URL: " << url;
    return url;
}

struct ArchiveTimeCheckInfo
{
    ArchiveTimeCheckInfo(
            const QnSecurityCamResourcePtr& camera,
            const QnMediaServerResourcePtr& server,
            qint64* result)
        :
          camera(camera),
          server(server),
          result(result)
    {
    }

    QnSecurityCamResourcePtr camera;
    QnMediaServerResourcePtr server;
    qint64* result = nullptr;
};

} // namespace

void QnRtspClientArchiveDelegate::checkGlobalTimeAsync(
        const QnSecurityCamResourcePtr& camera, const QnMediaServerResourcePtr& server, qint64* result)
{
    QnRtspClient client(
                QnRtspClient::Config{
                    .shouldGuessAuthDigest = true,
                    .backChannelAudioOnly = false,
                    .sleepIfEmptySocket = m_sleepIfEmptySocket});
    QnRtspClientArchiveDelegate::setupRtspSession(camera, server, &client);
    const auto url = getUrl(camera, server);
    const auto error = client.open(url).errorCode;
    if (error != CameraDiagnostics::ErrorCode::noError)
    {
        NX_DEBUG(this, "%1(%2, %3) failed to open RTSP session to %4 with error %5",
                 __func__, camera, server, url, error);
        return;
    }

    qint64 startTime = client.startTime();
    if (startTime != qint64(AV_NOPTS_VALUE) && startTime != DATETIME_NOW)
    {
        if (startTime < *result || *result == qint64(AV_NOPTS_VALUE))
            *result = startTime;
    }
}

void QnRtspClientArchiveDelegate::checkMinTimeFromOtherServer(const QnSecurityCamResourcePtr &camera)
{
    if (!camera || !camera->resourcePool())
    {
        m_globalMinArchiveTime = qint64(AV_NOPTS_VALUE);
        return;
    }

    QnMediaServerResourceList mediaServerList =
            camera->systemContext()->cameraHistoryPool()->getCameraFootageData(camera, true);

    /* Check if no archive available on any server. */
    /* Or check if archive belong to the current server only */
    if (mediaServerList.isEmpty() ||
            (mediaServerList.size() == 1 && mediaServerList[0] == camera->getParentServer()))
    {
        m_globalMinArchiveTime = qint64(AV_NOPTS_VALUE);
        return;
    }

    QList<ArchiveTimeCheckInfo> checkList;
    qint64 currentMinTime = qint64(AV_NOPTS_VALUE);
    qint64 otherMinTime = qint64(AV_NOPTS_VALUE);
    for (const auto& server: mediaServerList)
    {
        checkList << ArchiveTimeCheckInfo(
                         camera, server, server == m_server ? &currentMinTime : &otherMinTime);
    }
    QtConcurrent::blockingFilter(checkList, nx::suppressExceptions(
                                     [this](const ArchiveTimeCheckInfo& checkInfo)
    {
        checkGlobalTimeAsync(checkInfo.camera, checkInfo.server, checkInfo.result);
        return true;
    },
    this,
    [](auto item)
    {
        return NX_FMT("checkGlobalTimeAsync(%1, %2) opening RTSP session to %3",
                      item.camera, item.server, nx::suppressExceptions(&getUrl)(item.camera, item.server));
    }));
    if ((otherMinTime != qint64(AV_NOPTS_VALUE)) && (currentMinTime == qint64(AV_NOPTS_VALUE) || otherMinTime < currentMinTime))
        m_globalMinArchiveTime = otherMinTime;
    else
        m_globalMinArchiveTime = qint64(AV_NOPTS_VALUE);
}

QnMediaServerResourcePtr QnRtspClientArchiveDelegate::getServerOnTime(qint64 timeUsec) {
    qDebug() << "QnRtspClientArchiveDelegate::getServerOnTime";
    if (!m_camera)
        return QnMediaServerResourcePtr();
    QnMediaServerResourcePtr currentServer = m_camera->getParentServer();

    if (timeUsec == DATETIME_NOW)
        return currentServer;

    qint64 timeMs = timeUsec / 1000;

    QnMediaServerResourcePtr mediaServer =
            m_camera->systemContext()->cameraHistoryPool()->getMediaServerOnTime(
                m_camera,
                timeMs,
                &m_serverTimePeriod);

    if (!mediaServer)
        return currentServer;

    if (mediaServer != m_server)
        qDebug() << "switch to server " << mediaServer->getUrl();
    return mediaServer;

}

bool QnRtspClientArchiveDelegate::open(const QnResourcePtr &resource,
                                       AbstractArchiveIntegrityWatcher * /*archiveIntegrityWatcher*/)
{
    qDebug() << "QnRtspClientArchiveDelegate::open";
    m_idCamera = resource->getId().toStdString();
    QnSecurityCamResourcePtr camera = resource.dynamicCast<QnSecurityCamResource>();
    NX_ASSERT(camera);
    if (!camera)
        return false;

    setCamera(camera);

    bool rez = openInternal();
    if (!rez) {
        for (int i = 0; i < REOPEN_TIMEOUT/10 && !m_closing; ++i)
            QnSleep::msleep(10);
    }
    return rez;
}

std::string QnRtspClientArchiveDelegate::getIpServer()
{
    nx::network::SocketAddress rtsp = m_server->getPrimaryAddress();
    std::stringstream ss;
    ss << rtsp.toString();
    std::string address = QString::fromStdString(ss.str()).toStdString();

    std::string ipAddress;
    std::string port;
    std::string url = "";
    size_t colonPos = address.find(':');

    if (colonPos == std::string::npos) {
        qDebug() << "Invalid input format.\n";
    }

    ipAddress = address.substr(0, colonPos);
    return ipAddress;
}

std::string QnRtspClientArchiveDelegate::getUrlRecord(std::string idcam, std::string timestamp)
{
    //http://localhost:9996/get?path=mystream1&start=2024-04-04T10%3A57%3A55.390%2B07%3A00&duration=200.5
    nx::network::SocketAddress rtsp = m_server->getPrimaryAddress();
    std::stringstream ss;
    ss << rtsp.toString();
    std::string address = QString::fromStdString(ss.str()).toStdString();

    std::string ipAddress;
    std::string port;
    std::string url = "";
    size_t colonPos = address.find(':');

    if (colonPos == std::string::npos) {
        qDebug() << "Invalid input format.\n";
    }

    ipAddress = address.substr(0, colonPos);
    port = address.substr(colonPos + 1);

    if (port == "7005")         //7005
    {
        port = "9996";
        std::string urlstr = "http://" + ipAddress + ":" + port + "/get?path=mystream1&start=" + timestamp + "&duration=9223372036854775807";
        url = urlstr;
    }
    else
    {
        url = m_server->rtspUrl().toStdString() + "/";
        qDebug() << "getUrl   url:     "  << url.c_str();
    }
    qDebug() << "QnRtspClientArchiveDelegate::getUrlRecord " << url.c_str();
    m_isLive = false;

    return url;
}

std::string QnRtspClientArchiveDelegate::getUrlStream(std::string idcam)
{
    qDebug() << nx::format("Server Stream RTSP    IP : %1   PORT:   %2").args(m_server->getPrimaryAddressOryza(), m_server->getPort());
    nx::network::SocketAddress rtsp = m_server->getPrimaryAddress();
    std::stringstream ss;
    ss << rtsp.toString();
    qDebug() << "RTSP stream:   " << QString::fromStdString(ss.str());
    std::string address = QString::fromStdString(ss.str()).toStdString();

    std::string ipAddress;
    std::string port;
    std::string url = "";
    size_t colonPos = address.find(':');

    if (colonPos == std::string::npos) {
        qDebug() << "Invalid input format.\n";
    }

    ipAddress = address.substr(0, colonPos);
    port = address.substr(colonPos + 1);

    if (port == "7005")         //7005
    {
        port = "8554";
        std::string urlstr = "rtsp://" + ipAddress + ":" + port + "/";
        url = urlstr;
    }
    else
    {
        url = m_server->rtspUrl().toStdString() + "/";
        qDebug() << "getUrl   url:     "  << url.c_str();
    }
    url = url + idcam + "_1";
    qDebug() << "QnRtspClientArchiveDelegate::getUrlStream " << url.c_str();
    m_isLive = true;

    return url;
}

bool QnRtspClientArchiveDelegate::openInternal()
{
    qDebug() << "vao openInternal";
    if (m_rtspSession->isOpened())
        return true;

    {
        NX_MUTEX_LOCKER lock(&m_mutex);
        m_parsers.clear();
    }
    m_frameCnt = 0;
    m_closing = false;
    m_lastMediaFlags = -1;
    m_sessionTimeout.invalidate();
    setCustomVideoLayout(QnCustomResourceVideoLayoutPtr());

    m_globalMinArchiveTime = startTime(); // force current value to avoid flicker effect while current server is being changed

    if (!m_fixedServer)
    {
        m_server = getServerOnTime(m_position); // try to update server
        qDebug() << nx::format("QnRtspClientArchiveDelegate::openInternal Get Server:   %1").arg(m_server);
        if (m_server == 0 || m_server->getStatus() == nx::vms::api::ResourceStatus::offline)
        {
            if (m_isMultiserverAllowed && isSpecialTimeValue(m_globalMinArchiveTime))
                checkMinTimeFromOtherServer(m_camera);
            return false;
        }
    }

    if (m_playNowModeAllowed) {
        m_channelCount = 1;
        QnConstResourceVideoLayoutPtr videoLayout = m_camera->getVideoLayout(0);
        if (videoLayout)
            m_channelCount = videoLayout->channelCount();
    }
    setupRtspSession(m_camera, m_server, m_rtspSession.get());
    bool isOpened;
    QString rtsp = getUrl(m_camera, m_server);
    qDebug() << "Get rtsp: -> " << rtsp;

    isOpened = m_rtspSession->open(getUrl(m_camera, m_server), m_lastSeekTime).errorCode == CameraDiagnostics::ErrorCode::noError;
    if (!rtsp.contains("7005"))
    {
        qDebug() << "khong phai server Oryza";
        m_isServerOryza = false;
        isOpened = true;
        m_isOpenedRTSP = true;
    }
    qDebug() << nx::format("URL CAM: %1     ").arg(getUrl(m_camera, m_server));

    if (isOpened)
    {
        qint64 endTime = m_position;
        if (m_forcedEndTime)
            endTime = m_forcedEndTime;
        qDebug() << "ID Camera -> " << m_idCamera.c_str();
        m_rtspSession->setIdCamera(m_idCamera);
        m_rtspSession->play(m_position, endTime, m_rtspSession->getScale());
        if (m_isMultiserverAllowed)
            checkMinTimeFromOtherServer(m_camera);

        m_rtpData = nullptr;
        if (m_playNowModeAllowed)
        {
            // temporary solution
            ///// SỬA LOẠI TRANSPORT CHỖ NÀY
            m_rtspDevice = std::make_unique<QnRtspIoDevice>(
                        m_rtspSession.get(),
                        nx::vms::api::RtpTransportType::tcp);

            m_rtpData = m_rtspDevice.get();
        }
        else
        {
            auto& trackInfo =  m_rtspSession->getTrackInfo();
            if (!trackInfo.empty())
                m_rtpData = trackInfo[0].ioDevice.get();
        }
        if (!m_rtpData)
            m_rtspSession->stop();
        //        }
    }
    else {
        m_rtspSession->stop();
    }
    m_sendedCSec = m_rtspSession->lastSendedCSeq();

    if (isOpened)
    {
        m_sessionTimeout.restart();

        QStringList audioSDP = m_rtspSession->getSdpByType(nx::streaming::Sdp::MediaType::Audio);
        parseAudioSDP(audioSDP);

        QString vLayout = m_rtspSession->getVideoLayout();
        if (!vLayout.isEmpty())
        {
            auto newValue = QnCustomResourceVideoLayout::fromString(vLayout);
            bool isChanged =  getVideoLayout()->toString() != newValue->toString();
            setCustomVideoLayout(newValue);
            if (m_reader && isChanged)
                emit m_reader->videoLayoutChanged();
        }
    }
    return isOpened;
}

void QnRtspClientArchiveDelegate::parseAudioSDP(const QStringList& audioSDP)
{
    NX_MUTEX_LOCKER lock(&m_mutex);
    for (int i = 0; i < audioSDP.size(); ++i)
    {
        if (audioSDP[i].startsWith("a=fmtp"))
        {
            int configPos = audioSDP[i].indexOf("config=");
            if (configPos > 0)
            {
                auto data = QByteArray::fromBase64(audioSDP[i].mid(configPos + 7).toUtf8());
                auto context = std::make_shared<CodecParameters>();
                if (!context->deserialize(data.data(), data.size()))
                {
                    NX_DEBUG(this, "Failed to deserialize codec parameters");
                    continue;
                }
                if (context && context->getCodecType() == AVMEDIA_TYPE_AUDIO)
                    m_audioLayout.reset(new AudioLayout(context));
            }
        }
    }
}

void QnRtspClientArchiveDelegate::beforeClose()
{
    //m_waitBOF = false;
    m_closing = true;
    m_rtspSession->shutdown();
}

void QnRtspClientArchiveDelegate::close()
{
    NX_MUTEX_LOCKER lock(&m_mutex);
    m_rtspSession->stop();
}

qint64 QnRtspClientArchiveDelegate::startTime() const
{
    qint64 result = m_globalMinArchiveTime;
    if (result == qint64(AV_NOPTS_VALUE))
        result = m_rtspSession->startTime();

    if (result == DATETIME_NOW || result <= qnSyncTime->currentUSecsSinceEpoch())
        return result;
    else
        return DATETIME_NOW; // archive in a future
}

qint64 QnRtspClientArchiveDelegate::endTime() const
{
    /*
    if (m_rtspSession->endTime() > qnSyncTime->currentUSecsSinceEpoch())
        return m_rtspSession->endTime();
    else
    */
    return DATETIME_NOW; // LIVE or archive future point as right edge for server video
}

bool QnRtspClientArchiveDelegate::reopen()
{
    qDebug() << "QnRtspClientArchiveDelegate::reopen";
    close();

    if (m_blockReopening)
        return false;

    if (m_reopenTimer.isValid() && m_reopenTimer.elapsed() < REOPEN_TIMEOUT)
    {
        for (int i = 0; i < (REOPEN_TIMEOUT - m_reopenTimer.elapsed()) / 10 && !m_closing; ++i)
            QnSleep::msleep(10);
    }
    m_reopenTimer.restart();

    if (m_camera)
        return openInternal();

    return false;
}

bool QnRtspClientArchiveDelegate::isConnectionExpired() const
{
    return m_sessionTimeout.isValid() && m_sessionTimeout.hasExpired(m_maxSessionDurationMs.count());
}

void QnRtspClientArchiveDelegate::startRtsp(std::string rtsp)
{
    LOG_KhoiVH(" Start RTSP -> " + rtsp);
    int videoStream, i, numBytes;
    avformat_network_init();
    av_register_all();


    _pFormatCtx = avformat_alloc_context();

    AVDictionary *avdic=NULL;
    char option_key[]="rtsp_transport";
    char option_value[]="tcp";
    av_dict_set(&avdic,option_key,option_value,0);
    char option_key2[]="max_delay";
    char option_value2[]="100";
    av_dict_set(&avdic,option_key2,option_value2,0);

    if (avformat_open_input(&_pFormatCtx, rtsp.c_str(), NULL, &avdic) != 0) {
//        qDebug() << "can't open the file " << rtsp.c_str();
        return;
    }

    if (avformat_find_stream_info(_pFormatCtx, NULL) < 0) {
        qDebug() << "Could't find stream infomation.\n";
    }

    videoStream = -1;

    for (i = 0; i < _pFormatCtx->nb_streams; i++) {
        if (_pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStream = i;
        }
    }

    if (videoStream == -1) {
        qDebug() << "Didn't find a video stream.\n";
    }

    _pCodecCtx = _pFormatCtx->streams[videoStream]->codec;
    pCodec = avcodec_find_decoder(_pCodecCtx->codec_id);
    _pCodecCtx->bit_rate =0;
    _pCodecCtx->time_base.num=1;
    _pCodecCtx->time_base.den=10;
    _pCodecCtx->frame_number=1;

    if (pCodec == NULL) {
        qDebug() << "Codec not found.\n";
    }

    if (avcodec_open2(_pCodecCtx, pCodec, NULL) < 0) {
        qDebug() << "Could not open codec.\n";
    }

    pFrame = av_frame_alloc();
    pFrameRGB = av_frame_alloc();

    imgConvertCtx = sws_getContext(_pCodecCtx->width, _pCodecCtx->height,
                                   _pCodecCtx->pix_fmt, _pCodecCtx->width, _pCodecCtx->height,
                                   AV_PIX_FMT_RGB32, SWS_BICUBIC, NULL, NULL, NULL);

    numBytes = avpicture_get_size(AV_PIX_FMT_RGB32, _pCodecCtx->width,_pCodecCtx->height);

    out_buffer = (uint8_t *) av_malloc(numBytes * sizeof(uint8_t));
    avpicture_fill((AVPicture *) pFrameRGB, out_buffer, AV_PIX_FMT_RGB32,
                   _pCodecCtx->width, _pCodecCtx->height);

    int y_size = _pCodecCtx->width * _pCodecCtx->height;

    _packet = (AVPacket *) malloc(sizeof(AVPacket));
    av_new_packet(_packet, y_size);
    qDebug() << "KhoiVH QnRtspClient::open Mo RTSP thanh cong";

    m_isOpenedRTSP = true;
    frameRate = (float) _pCodecCtx->framerate.num / (float) _pCodecCtx->framerate.den;
    qDebug() << "FrameRate of RTSP: " << frameRate;
    if (callRecord == true) callRecord = false;
    LOG_KhoiVH("------------------ MO RTSP THANH CONG ---------------------- ");
}

void QnRtspClientArchiveDelegate::pauseRtsp()
{
    QMutexLocker locker(&m_qmutex);
    if (m_isOpenedRTSP)
    {
        LOG_KhoiVH("------------------ PAUSE RTSP ---------------------- ");
        qDebug() << "QnRtspClientArchiveDelegate::pauseRtsp";
        m_isOpenedRTSP = false;
        av_free(out_buffer);
        av_frame_free(&pFrameRGB);
        av_frame_free(&pFrame);
        av_packet_unref(_packet);
//        if (_pCodecCtx != NULL) avcodec_close(_pCodecCtx);
//        if (_pFormatCtx != NULL) avformat_close_input(&_pFormatCtx);
        sws_freeContext(imgConvertCtx);
        qDebug() << " QnRtspClientArchiveDelegate::pauseRtsp pause sucess";
    }
}

void QnRtspClientArchiveDelegate::getNextDataOryza(AVPacket** packet, AVCodecContext** pCodecCtx, AVFormatContext** pFormatCtx, qint64* time, std::string rtsp, qint64 *timeStamp)
{
    if (!m_isOpenedRTSP) startRtsp(rtsp);
    else
        getNextDataInternalOryza(packet, pCodecCtx, pFormatCtx, timeStamp, time);
}


QnAbstractMediaDataPtr QnRtspClientArchiveDelegate::getNextData()
{
    if (!m_currentServerUpToDate.test_and_set() || isConnectionExpired())
        reopen();

    if (!m_footageUpToDate.test_and_set()) {
        if (m_isMultiserverAllowed)
            checkMinTimeFromOtherServer(m_camera);
    }

    QnAbstractMediaDataPtr result = getNextDataInternal();
    if (!result && !m_blockReopening && !m_closing)
        result = getNextDataInternal(); // try again in case of RTSP reconnect

    if (!m_serverTimePeriod.isNull() && m_isMultiserverAllowed)
    {
        qDebug() << "Check if archive moved to other video server";
        // Check if archive moved to other video server
        qint64 timeMs = AV_NOPTS_VALUE;
        if (result && result->timestamp >= 0)
            timeMs = result->timestamp / 1000; // do not switch server if AV_NOPTS_VALUE and any other invalid packet timings
        bool outOfRange = timeMs != AV_NOPTS_VALUE
                && ((m_rtspSession->getScale() >= 0 && timeMs >= m_serverTimePeriod.endTimeMs())
                    || (m_rtspSession->getScale() <  0 && timeMs < m_serverTimePeriod.startTimeMs));
        if (result == 0 || outOfRange || result->dataType == QnAbstractMediaData::EMPTY_DATA)
        {
            if (m_lastSeekTime == AV_NOPTS_VALUE)
                m_lastSeekTime = qnSyncTime->currentUSecsSinceEpoch();
            QnMediaServerResourcePtr newServer = getNextMediaServerFromTime(m_camera, m_lastSeekTime / 1000);
            if (newServer)
            {
                qDebug() << "Reached the edge for archive in a current server. packetTime="
                         << QDateTime::fromMSecsSinceEpoch(timeMs).toString()
                         << "period: " << QDateTime::fromMSecsSinceEpoch(m_serverTimePeriod.startTimeMs).toString()
                         << "-" << QDateTime::fromMSecsSinceEpoch(m_serverTimePeriod.endTimeMs()).toString();

                m_server = newServer;
                m_lastSeekTime = m_serverTimePeriod.startTimeMs * 1000;
                if (m_rtspSession->getScale() > 0)
                    m_position = m_serverTimePeriod.startTimeMs * 1000;
                else
                    m_position = (m_serverTimePeriod.endTimeMs() - 1) * 1000;
                close();
                openInternal();

                result = getNextData();
                if (result)
                    result->flags |= QnAbstractMediaData::MediaFlags_NewServer;
            }
            else
            {
                m_serverTimePeriod.clear();
            }
        }
    }
    if (!result)
        m_lastError.errorCode = CameraDiagnostics::ErrorCode::serverTerminated;
    else
        m_lastError.errorCode = CameraDiagnostics::ErrorCode::noError;

    return result;
}

std::string QnRtspClientArchiveDelegate::extractJson(std::string findStr, const std::string& json) {
    size_t tokenPos = json.find(findStr);
    if (tokenPos != std::string::npos) {
        size_t valueStart = tokenPos + findStr.size(); // Move to the start of the token value
        size_t valueEnd = json.find_first_of("}", valueStart);
        if (valueEnd != std::string::npos) {
            // Extract the token value
            return json.substr(valueStart-1, valueEnd - valueStart + 3);
        }
    }
    return "";
}

void QnRtspClientArchiveDelegate::callAPIRecord(std::string ipHost, std::string portHost, int64_t time)
{
    QnResourcePtr resource =  m_reader->getResource();
    time = time/1000000;
    std::string idCamera = resource->getId().toStdString();
    idCamera.erase(std::remove(idCamera.begin(), idCamera.end(), '{'), idCamera.end());
    idCamera.erase(std::remove(idCamera.begin(), idCamera.end(), '}'), idCamera.end());
    std::string api = "/api/v1/camera/" + idCamera + "/record" + "?startTime=" + std::to_string(time);
    qDebug() << nx::format("Get API record:  %1").arg(api);
    try
    {
        boost::asio::io_service io_service;
        tcp::resolver resolver(io_service);
        tcp::resolver::query query(ipHost, portHost);
        tcp::resolver::iterator endpoint_iterator = resolver.resolve(query);

        tcp::socket socket(io_service);
        boost::asio::connect(socket, endpoint_iterator);

        // Create the HTTP POST request
        std::string request = "GET " + api +  " HTTP/1.1\r\n"
                                              "Host: " + ipHost + ":" + portHost +"\r\n"
                                                                                  "Content-Type: application/json\r\n"
                                                                                  "Authorization: Bearer ImV5SjBlWEFpT2lKS1YxUWlMQ0poYkdjaU9pSklVekkxTmlKOS5leUpwWkNJNklqWmxOV1pqWmpZeU9UZ3pPRFEyTXpOaE9UZ3dNRGRrWWpVell6RmlNMll5SWl3aVpXMWhhV3dpT2lKaFpHMXBia0J2Y25sNllTNTJiaUlzSW5ScGJXVmZaWGh3YVhKbFpDSTZJakl3TWpRdE1ETXRNamtnTVRVNk16YzZNVElpTENKcGMxOWhaRzFwYmlJNmRISjFaWDAuVlFnY2tNRnFBc0hqa3BIT1JwZF9aSUdHR1RUbUhrLVFJTEU2UVZPZS0wNCI=\r\n"
                                                                                  "Content-Length: " +std::to_string(0) + "\r\n"
                                                                                                                          "Connection: close\r\n"
                                                                                                                          "\r\n";
        qDebug() << "DATA RECORD: " << request.c_str();

        boost::asio::write(socket, boost::asio::buffer(request));

        boost::asio::streambuf response;
        boost::system::error_code error;

        while (boost::asio::read(socket, response, boost::asio::transfer_at_least(1), error)) {
        }

        if (error != boost::asio::error::eof) {
            qDebug() << "Error: " << error.message().c_str();
        }

        std::string response_string(boost::asio::buffers_begin(response.data()),
                                    boost::asio::buffers_end(response.data()));
        qDebug() << "Response Server Oryza record: " << response_string.c_str();
        QString jsonString = extractJson("{", response_string).c_str();
        QJsonDocument jsonDocument = QJsonDocument::fromJson(jsonString.toUtf8());
        if (!jsonDocument.isNull() && jsonDocument.isObject()) {
            QJsonObject jsonObject = jsonDocument.object();

            if (jsonObject.contains("rtsp")) {
                QString rtsp = jsonObject["rtsp"].toString();
                qDebug() << "Main RTSP:" << rtsp;
                m_runRTSP = rtsp.toStdString();
                //                m_runRTSP = "tcp://192.168.103.91:52155";
            }
        } else {
            qDebug() << "Không thể phân tích chuỗi JSON.";
        }
    }
    catch (std::exception &e)
    {
        qDebug() << "Lôi Call API: " << e.what();
    }
}

void QnRtspClientArchiveDelegate::getNextDataInternalOryza(AVPacket** packet, AVCodecContext** pCodecCtx, AVFormatContext** pFormatCtx, qint64* timestamp, qint64* time)
{

    QMutexLocker locker(&m_qmutex);
    if (m_isOpenedRTSP && _packet )
    {
//        int64_t timestamp1 = _packet->pts*10;
//        qDebug() << "getNextDataInternalOryza Timestamp -> " << timestamp1;
        (*packet) = _packet;
        (*pFormatCtx) = _pFormatCtx;
        (*pCodecCtx) = _pCodecCtx;
        if (av_read_frame(_pFormatCtx, _packet) < 0)
        {
            m_readFrameFail = true;
        }
        else
        {
            m_readFrameFail = false;
            qint64 parserPosition = qint64(AV_NOPTS_VALUE);
            parserPosition = (*pFormatCtx)->start_time_realtime + (*packet)->pts*10;
            m_timestamp = parserPosition;
            if (parserPosition != qint64(AV_NOPTS_VALUE))
                m_position = parserPosition;
            m_lastMediaFlags = QnAbstractMediaData::MediaFlags_Delayed;
            m_frameCnt++;


            *timestamp = *time + _packet->pts*10;
            m_lastReceivedTime = qnSyncTime->currentMSecsSinceEpoch();
            if (!m_isLive) std::this_thread::sleep_for(std::chrono::milliseconds(22));
        }
    }
    else
    {
        (*packet) = nullptr;
        (*pFormatCtx) = nullptr;
        *timestamp = *time + _packet->pts*10;
    }


}

QnAbstractMediaDataPtr QnRtspClientArchiveDelegate::getNextDataInternal()
{
    // sometime function may return zero packet if no data arrived
    QnAbstractMediaDataPtr result;
    //int errCnt = 0;
    QElapsedTimer receiveTimer;
    receiveTimer.start();
    while(!result)
    {
        if (!m_rtpData || (!m_rtspSession->isOpened() && !m_closing))
        {
            //m_rtspSession->stop(); // reconnect
            reopen();
            return result;
        }

        int rtpChannelNum = 0;
        int blockSize  = m_rtpData->read((char*)m_rtpDataBuffer, MAX_RTP_BUFFER_SIZE);

        if (blockSize <= 0 && !m_closing) {
            //m_rtspSession->stop(); // reconnect
            reopen();
            return result;
        }
        /*
        else if (blockSize == 0) {
            static const int MAX_ERROR_COUNT = 10;

            errCnt++;
            if (errCnt >= MAX_ERROR_COUNT)
                break;
            else
                continue;
        }
        errCnt = 0;
        */

#ifdef DEBUG_RTSP
        static QFile* binaryFile = 0;
        if (!binaryFile) {
            binaryFile = new QFile("c:/binary2.rtsp");
            binaryFile->open(QFile::WriteOnly);
        }
        binaryFile->write((const char*) m_rtpDataBuffer, blockSize);
        binaryFile->flush();

#endif

        quint8* data = m_rtpDataBuffer;
        //qDebug() << "RTP data Buffer: " << (char*)m_rtpDataBuffer;
        if (m_tcpMode)
        {
            if (blockSize < 4) {
                //                qWarning() << Q_FUNC_INFO << __LINE__ << "strange RTP/TCP packet. len < 4. Ignored";
                return result;
            }
            rtpChannelNum = m_rtpDataBuffer[1];
            blockSize -= 4;                         // loại bỏ 4 byte đầu của header
            data += 4;                              // di chuyển con trỏ lên 4 byte, để trỏ tới phần dữ liệu thực sự của gói tin RTP, bỏ qua header.
        }
        else {
            rtpChannelNum = m_rtpData->getMediaSocket()->getLocalAddress().port;
        }

        QString codecName;
        if (m_playNowModeAllowed)
            codecName = rtpChannelNum < (m_channelCount + 1) * 2 ? "ffmpeg" : "ffmpeg-metadata";
        else
            codecName = m_rtspSession->getTrackCodec(rtpChannelNum).toLower();
        qint64 parserPosition = qint64(AV_NOPTS_VALUE);

        if (codecName == QLatin1String("ffmpeg"))
        {
            auto [packet, success] =  processFFmpegRtpPayload(data, blockSize, rtpChannelNum/2, &parserPosition);
                    result = std::dynamic_pointer_cast<QnAbstractMediaData>(packet);
                    if (!result && m_frameCnt == 0 && receiveTimer.elapsed() > 4000)
                    emit dataDropped(m_reader); // if client can't receive first frame too long inform that stream is slow
                    if (!success)
                    return result;
        }
                    else if (codecName == QLatin1String("ffmpeg-metadata"))
            {
                processMetadata(data, blockSize);
            }
            else
            {
                NX_WARNING(this, "Unsupported codec format '%1'", codecName);
            }

            if (result && m_sendedCSec != result->opaque)
                result.reset(); // ignore old archive data
            if (result && parserPosition != qint64(AV_NOPTS_VALUE))
                m_position = parserPosition;
            /*
        if (result && m_waitBOF)
        {
            if ((result->flags & QnAbstractMediaData::MediaFlags_BOF) &&
                ((result->flags & QnAbstractMediaData::MediaFlags_LIVE) ||  m_sendedCSec == result->opaque))
            {
                if (qAbs(result->timestamp - m_lastSeekTime) > 1000ll*1000*10)
                {
                    QString s;
                    QTextStream str(&s);
                    str << "recvSsrc=" << m_sendedCSec
                        << " expectedTime=" << QDateTime::fromMSecsSinceEpoch(m_lastSeekTime/1000).toString("hh:mm:ss.zzz")
                        << " packetTime=" << QDateTime::fromMSecsSinceEpoch(result->timestamp/1000).toString("hh:mm:ss.zzz");
                    str.flush();
                }
                m_waitBOF = false;
            }
            else
                result.clear();
        }
        */
        }
        if (!result && !m_closing)
            reopen();
        if (result)
        {
            if (result->dataType != QnAbstractMediaData::EMPTY_DATA)
                m_lastMediaFlags = result->flags;
            m_frameCnt++;
        }

        m_lastReceivedTime = qnSyncTime->currentMSecsSinceEpoch();
        return result;
    }

    void QnRtspClientArchiveDelegate::setAdditionalAttribute(const QByteArray& name, const QByteArray& value)
    {
        m_rtspSession->setAdditionAttribute(name, value);
    }

    qint64 QnRtspClientArchiveDelegate::seek(qint64 startTime, qint64 endTime)
    {
        qDebug() << "QnRtspClientArchiveDelegate::seek startTime";
        m_forcedEndTime = endTime;
        return seek(startTime, true);
    }

    qint64 QnRtspClientArchiveDelegate::seek(qint64 time, bool findIFrame)
    {
        qDebug() << "QnRtspClientArchiveDelegate::seek";
        NX_DEBUG(this,
                 "Set position %1 for device %2",
                 nx::utils::timestampToDebugString(time / 1000),
                 m_camera);

        m_blockReopening = false;

        //if (time == m_position)
        //    return time;

        //deleteContexts(); // context is going to create again on first data after SEEK, so ignore rest of data before seek
        m_lastSeekTime = m_position = time;
        if (m_isMultiserverAllowed) {
            QnMediaServerResourcePtr newServer = getServerOnTime(m_position);
            if (newServer != m_server)
            {
                close();
                m_server = newServer;
            }
        }

        if (!findIFrame)
            m_rtspSession->setAdditionAttribute("x-no-find-iframe", "1");

        if ((!m_rtspSession->isOpened() && m_camera) || m_isOpenedRTSP == false)
        {
            if (!m_isServerOryza)
            {
                qDebug() << "Mo stream";
                if (!openInternal() && m_isMultiserverAllowed)
                {
                    // Try next server in the list immediately, It is improve seek time if current server is offline and next server is exists
                    while (!m_closing)
                    {
                        QnMediaServerResourcePtr nextServer = getNextMediaServerFromTime(m_camera, m_lastSeekTime/1000);
                        if (nextServer && nextServer != m_server)
                        {
                            m_server = nextServer;
                            m_lastSeekTime = m_serverTimePeriod.startTimeMs*1000;
                            if (m_rtspSession->getScale() > 0)
                                m_position = m_serverTimePeriod.startTimeMs*1000;
                            else
                                m_position = (m_serverTimePeriod.endTimeMs()-1)*1000;
                            close();
                            if (openInternal())
                                break;
                        }
                        else {
                            break;
                        }
                    }
                }
            }


        }
        else {
            qDebug() << "Xem lại record";
            qint64 endTime = AV_NOPTS_VALUE;
            if (m_forcedEndTime)
                endTime = m_forcedEndTime;
            else if (m_singleShotMode)
                endTime = time;
            m_rtspSession->sendPlay(time, endTime, m_rtspSession->getScale());
            m_rtspSession->removeAdditionAttribute("x-no-find-iframe");
        }
        m_sendedCSec = m_rtspSession->lastSendedCSeq();
        return time;
    }

    int QnRtspClientArchiveDelegate::getSequence() const
    {
        return m_sendedCSec;
    }

    void QnRtspClientArchiveDelegate::setSingleshotMode(bool value)
    {
        if (value == m_singleShotMode)
            return;
        //        if (value == true) pauseRtsp();
        m_singleShotMode = value;
    }

    void QnRtspClientArchiveDelegate::setCustomVideoLayout(const QnCustomResourceVideoLayoutPtr& value)
    {
        NX_MUTEX_LOCKER lock(&m_mutex);
        m_customVideoLayout = value;
    }

    QnConstResourceVideoLayoutPtr QnRtspClientArchiveDelegate::getVideoLayout()
    {
        NX_MUTEX_LOCKER lock(&m_mutex);
        if (m_customVideoLayout)
            return m_customVideoLayout;
        else if (m_camera && m_camera->resourcePool())
            return m_camera->getVideoLayout();
        else
            return QnMediaResource::getDefaultVideoLayout();
    }

    AudioLayoutConstPtr QnRtspClientArchiveDelegate::getAudioLayout()
    {
        NX_MUTEX_LOCKER lock(&m_mutex);
        if (!m_audioLayout)
        {
            m_audioLayout.reset(new AudioLayout());
            for (auto itr = m_parsers.begin(); itr != m_parsers.end(); ++itr)
            {
                CodecParametersConstPtr context = itr.value()->mediaContext();
                if (context && context->getCodecType() == AVMEDIA_TYPE_AUDIO)
                    m_audioLayout->addTrack(context, context->getAudioCodecDescription());
            }
        }
        return m_audioLayout;
    }

    void QnRtspClientArchiveDelegate::processMetadata(const quint8* data, int dataSize)
    {
        // RtpHeader* rtpHeader = (RtpHeader*) data;
        const quint8* payload = data + nx::streaming::rtp::RtpHeader::kSize;
        QByteArray ba((const char*)payload, data + dataSize - payload);
        if (ba.startsWith("clock="))
            m_rtspSession->parseRangeHeader(QLatin1String(ba));
        else if (ba.startsWith("drop-report"))
            emit dataDropped(m_reader);
    }

    namespace {

    /**
 * @return Zero version if serverString is invalid.
 */
    nx::utils::SoftwareVersion extractServerVersion(const nx::String& serverString)
    {
        int versionStartPos = serverString.indexOf("/") + 1;
        int versionEndPos = serverString.indexOf(" ", versionStartPos);

        return nx::utils::SoftwareVersion(
                    serverString.mid(versionStartPos, versionEndPos - versionStartPos));
    }

    } // namespace

    std::pair<QnAbstractDataPacketPtr, bool> QnRtspClientArchiveDelegate::processFFmpegRtpPayload(
                quint8* data, int dataSize, int channelNum, qint64* parserPosition)
    {
        NX_MUTEX_LOCKER lock( &m_mutex );

        QnAbstractMediaDataPtr result;

        auto itr = m_parsers.find(channelNum);
        if (itr == m_parsers.end())
        {
            auto parser = new nx::streaming::rtp::QnNxRtpParser(m_camera->getId(), m_rtpLogTag);
            // TODO: Use nx::network::http::header::Server here
            // to get RFC2616-conformant Server header parsing function.
            auto serverVersion = extractServerVersion(m_rtspSession->serverInfo());
            parser->setServerVersion(serverVersion);
            if (!serverVersion.isNull() && serverVersion < nx::utils::SoftwareVersion(3, 0))
                parser->setAudioEnabled(false);
            itr = m_parsers.insert(channelNum, nx::streaming::rtp::QnNxRtpParserPtr(parser));
        }
        nx::streaming::rtp::QnNxRtpParserPtr parser = itr.value();
        bool gotData = false;
        const auto parseResult = parser->processData(data, dataSize, gotData);
        if (!parseResult.success)
        {
            NX_DEBUG(this, "RTP parser error: %1", parseResult.message);
            return {QnAbstractDataPacketPtr(), parseResult.success}; //< Report error to reopen connection.
        }
        *parserPosition = parser->position();
        if (gotData) {
            result = parser->nextData();
            if (result)
                result->channelNumber = channelNum;
        }
        return {result, parseResult.success};
    }

    void QnRtspClientArchiveDelegate::setSpeed(qint64 displayTime, double value)
    {
        // Don't seek stream on pause if not reverse play currently
        if (value == 0.0 && m_rtspSession->getScale() >= 0)
            return;

        m_position = displayTime;

        bool oldReverseMode = m_rtspSession->getScale() < 0;
        bool newReverseMode = value < 0;
        m_rtspSession->setScale(value);

        bool needSendSeek = !m_rtspSession->isOpened() || oldReverseMode != newReverseMode
                || m_camera->hasCameraCapabilities(
                    nx::vms::api::DeviceCapability::isPlaybackSpeedSupported);
        if (!needSendSeek)
        {
            m_rtspSession->sendSetParameter(kSpeedParamName, QByteArray::number(value));
            return;
        }

        bool fromLive = newReverseMode && m_position == DATETIME_NOW;
        m_blockReopening = false;

        seek(displayTime, /* findIFrame */ true);
        if (fromLive)
            m_position = AV_NOPTS_VALUE;
    }

    bool QnRtspClientArchiveDelegate::isRealTimeSource() const
    {
        if (m_lastMediaFlags == -1)
            return m_position == DATETIME_NOW;
        else
            return m_lastMediaFlags & QnAbstractMediaData::MediaFlags_LIVE;
    }

    bool QnRtspClientArchiveDelegate::setQuality(MediaQuality quality, bool fastSwitch, const QSize& resolution)
    {
        if (m_quality == quality && fastSwitch <= m_qualityFastSwitch && m_resolution == resolution)
            return false;
        m_quality = quality;
        m_qualityFastSwitch = fastSwitch;
        m_resolution = resolution;

        if (!isRealTimeSource() && m_camera
                && m_camera->hasCameraCapabilities(
                    nx::vms::api::DeviceCapability::dualStreamingForLiveOnly))
        {
            return false;
        }

        if (m_quality == MEDIA_Quality_CustomResolution)
        {
            m_rtspSession->setAdditionAttribute(kResolutionParamName, resolutionToString(m_resolution).toLatin1());
            m_rtspSession->removeAdditionAttribute(kMediaQualityParamName);

            if (!fastSwitch)
                m_rtspSession->sendSetParameter(kMediaQualityParamName, resolutionToString(m_resolution).toLatin1());
        }
        else {
            const auto value = QByteArray::fromStdString(nx::reflect::toString(quality));

            m_rtspSession->setAdditionAttribute(kMediaQualityParamName, value);
            m_rtspSession->removeAdditionAttribute(kResolutionParamName);

            if (!fastSwitch)
                m_rtspSession->sendSetParameter(kMediaQualityParamName, value);
        }

        if (!m_rtspSession->isOpened())
            return false;

        if (!fastSwitch)
            return false; // switch quality without seek (it is take more time)
        else
            return true; // need send seek command
    }

    void QnRtspClientArchiveDelegate::setStreamDataFilter(nx::vms::api::StreamDataFilters filter)
    {
        if (m_streamDataFilter == filter)
            return;
        m_streamDataFilter = filter;

        const auto value = QByteArray::fromStdString(nx::reflect::toString(filter));
        const auto sendMotionValue = filter.testFlag(nx::vms::api::StreamDataFilter::motion) ? "1" : "0";

        m_rtspSession->setAdditionAttribute(Qn::RTSP_DATA_FILTER_HEADER_NAME, value);
        m_rtspSession->setAdditionAttribute(Qn::RTSP_DATA_SEND_MOTION_HEADER_NAME, sendMotionValue);
        if (m_rtspSession->isOpened())
        {
            m_rtspSession->sendSetParameter(Qn::RTSP_DATA_FILTER_HEADER_NAME, value);
            m_rtspSession->sendSetParameter(Qn::RTSP_DATA_SEND_MOTION_HEADER_NAME, sendMotionValue);
        }
    }

    void QnRtspClientArchiveDelegate::setStorageLocationFilter(nx::vms::api::StorageLocation filter)
    {
        m_storageLocationFilter = filter;
        const auto value = QByteArray::fromStdString(nx::reflect::toString(filter));
        m_rtspSession->setAdditionAttribute(Qn::RTSP_DATA_CHUNKS_FILTER_HEADER_NAME, value);
        if (m_rtspSession->isOpened())
            m_rtspSession->sendSetParameter(Qn::RTSP_DATA_CHUNKS_FILTER_HEADER_NAME, value);
    }

    nx::vms::api::StreamDataFilters QnRtspClientArchiveDelegate::streamDataFilter() const
    {
        return m_streamDataFilter;
    }

    void QnRtspClientArchiveDelegate::setMotionRegion(const QRegion& region)
    {
        if (region.isEmpty())
        {
            m_rtspSession->removeAdditionAttribute("x-motion-region");
        }
        else
        {
            QBuffer buffer;
            buffer.open(QIODevice::WriteOnly);
            QDataStream stream(&buffer);
            stream << region;
            buffer.close();
            QByteArray s = buffer.data().toBase64();
            m_rtspSession->setAdditionAttribute("x-motion-region", s);
        }
    }

    void QnRtspClientArchiveDelegate::beforeSeek(qint64 time)
    {
        if (m_camera
                && (m_camera->isGroupPlayOnly()
                    || m_camera->hasCameraCapabilities(
                        nx::vms::api::DeviceCapability::synchronousChannel)))
        {
            return; // avoid close/open for NVR
        }

        qint64 diff = qAbs(m_lastReceivedTime - qnSyncTime->currentMSecsSinceEpoch());
        bool longNoData = ((m_position == DATETIME_NOW || time == DATETIME_NOW) && diff > 250) || diff > 1000*10;
        if (longNoData || m_quality == MEDIA_Quality_Low || m_quality == MEDIA_Quality_LowIframesOnly)
        {
            beforeClose();
        }
    }

    void QnRtspClientArchiveDelegate::beforeChangeSpeed(double speed)
    {
        bool oldReverseMode = m_rtspSession->getScale() < 0;
        bool newReverseMode = speed < 0;

        // Reconnect If camera is offline and it is switch from live to archive
        if (oldReverseMode != newReverseMode && m_position == DATETIME_NOW)
        {
            if (newReverseMode)
                beforeSeek(AV_NOPTS_VALUE);
            else
                m_blockReopening = false;
        }
    }

    void QnRtspClientArchiveDelegate::setRange(qint64 startTime, qint64 endTime, qint64 frameStep)
    {
        NX_VERBOSE(this,
                   "Set range %1 - %2, framestep %3 us",
                   nx::utils::timestampToDebugString(startTime),
                   nx::utils::timestampToDebugString(endTime),
                   frameStep);

        setAdditionalAttribute("x-media-step", QByteArray::number(frameStep));
        seek(startTime, endTime);
    }

    void QnRtspClientArchiveDelegate::setMultiserverAllowed(bool value)
    {
        m_isMultiserverAllowed = value;
    }

    void QnRtspClientArchiveDelegate::setupRtspSession(
                const QnSecurityCamResourcePtr& camera,
                const QnMediaServerResourcePtr& server,
                QnRtspClient* session) const
    {
        session->setCredentials(m_credentials, nx::network::http::header::AuthScheme::digest);
        session->setAdditionAttribute(Qn::EC2_RUNTIME_GUID_HEADER_NAME,
                                      camera->systemContext()->sessionId().toByteArray());
        session->setAdditionAttribute(Qn::EC2_INTERNAL_RTP_FORMAT, "1" );
        session->setAdditionAttribute(Qn::CUSTOM_USERNAME_HEADER_NAME,
                                      QString::fromStdString(m_credentials.username).toUtf8());

        /* We can get here while client is already closing. */
        if (server)
        {
            QNetworkProxy proxy = QnNetworkProxyFactory::proxyToResource(server);
            if (proxy.type() != QNetworkProxy::NoProxy)
                session->setProxyAddr(proxy.hostName(), proxy.port());
            session->setAdditionAttribute(Qn::SERVER_GUID_HEADER_NAME, server->getId().toByteArray());
        }

        session->setTransport(nx::vms::api::RtpTransportType::tcp);
        const auto settings = m_camera->systemContext()->globalSettings();
        session->setTcpRecvBufferSize(settings->mediaBufferSizeKb() * 1024);

    }

    void QnRtspClientArchiveDelegate::setPlayNowModeAllowed(bool value)
    {
        m_playNowModeAllowed = value;
        m_rtspSession->setPlayNowModeAllowed(value);
    }

    int QnRtspClientArchiveDelegate::protocol() const
    {
        int result = 0;
        if (m_rtspDevice)
        {
            if (const auto socket = m_rtspDevice->getMediaSocket())
                socket->getProtocol(&result);
        }
        return result;
    }

    bool QnRtspClientArchiveDelegate::hasVideo() const
    {
        return m_camera && m_camera->hasVideo(nullptr);
    }

    void QnRtspClientArchiveDelegate::pleaseStop()
    {
        // TODO: #vbreus It should be done atomically under lock. To be fixed in 5.1 since there is no
        // way to do it right without significant changes.
        m_blockReopening = true;
        if (m_rtspSession->isOpened())
            m_rtspSession->shutdown();
    }

    void QnRtspClientArchiveDelegate::setMediaRole(PlaybackMode mode)
    {
        setAdditionalAttribute(Qn::EC2_MEDIA_ROLE, nx::reflect::toString(mode).data());
    }

    CameraDiagnostics::Result QnRtspClientArchiveDelegate::lastError() const
    {
        return m_lastError;
    }
