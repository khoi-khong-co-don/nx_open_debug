// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "archive_stream_reader.h"

#include <stdint.h>
#include <iomanip>
#include <ctime>
#include <sstream>
#include <core/resource/security_cam_resource.h>

#include "utils/common/util.h"
#include "utils/media/externaltimesource.h"
#include "utils/common/synctime.h"
#include "core/resource/media_resource.h"
#include "utils/common/sleep.h"
#include <nx/streaming/video_data_packet.h>
#include <nx/streaming/abstract_data_consumer.h>
#include <utils/media/frame_type_extractor.h>
#include <nx/utils/log/log.h>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <boost/date_time/posix_time/posix_time.hpp>

namespace {

bool isGroupPlayOnly(const QnResourcePtr& resource)
{
    auto camera = resource.dynamicCast<QnSecurityCamResource>();
    return camera && camera->isGroupPlayOnly();
}

} // namespace

using namespace nx::vms::api;

// used in reverse mode.
// seek by 1.5secs. It is prevents too fast seeks for short GOP, also some codecs has bagged seek function. Large step prevent seek
// forward instead seek backward
//static const int MAX_KEY_FIND_INTERVAL = 10 * 1000 * 1000;

//static const int FFMPEG_PROBE_BUFFER_SIZE = 1024 * 512;
static const qint64 LIVE_SEEK_OFFSET = 1000000ll * 10;

constexpr auto kSingleShowWaitTimeoutMSec = 100;

QnArchiveStreamReader::QnArchiveStreamReader(const QnResourcePtr& dev ) :
    QnAbstractArchiveStreamReader(dev),
    //protected
    m_currentTime(0),
    m_topIFrameTime(-1),
    m_bottomIFrameTime(-1),
    m_primaryVideoIdx(-1),
    m_audioStreamIndex(-1),
    m_firstTime(true),
    m_tmpSkipFramesToTime(AV_NOPTS_VALUE),
    //private
    cameraResource(dev),
    m_selectedAudioChannel(0),
    m_eof(false),
    m_frameTypeExtractor(0),
    m_lastGopSeekTime(-1),
    m_IFrameAfterJumpFound(false),
    m_requiredJumpTime(AV_NOPTS_VALUE),
    m_lastRequiredJumpTime(AV_NOPTS_VALUE),
    m_lastUsePreciseSeek(false),
    m_BOF(false),
    m_afterBOFCounter(-1),
    m_dataMarker(0),
    m_newDataMarker(0),
    m_currentTimeHint(AV_NOPTS_VALUE),
    //private section 2
    m_bofReached(false),
    m_externalLocked(false),
    m_exactJumpToSpecifiedFrame(false),
    m_ignoreSkippingFrame(false),
    m_skipFramesToTime(0),
    m_keepLastSkkipingFrame(true),
    m_singleShot(false),
    m_singleQuantProcessed(false),
    m_nextData(0),
    m_quality(MEDIA_Quality_High),
    m_qualityFastSwitch(true),
    m_oldQuality(MEDIA_Quality_High),
    m_oldQualityFastSwitch(true),
    m_isStillImage(false),
    m_speed(1.0),
    m_prevSpeed(1.0),
    m_streamDataFilter(StreamDataFilter::media),
    m_prevStreamDataFilter(StreamDataFilter::media),
    m_outOfPlaybackMask(false),
    m_latPacketTime(DATETIME_NOW),
    m_stopCond(false)
{
    qDebug() << "KHOI TAO QnArchiveStreamReader this -> " << this;
    m_numWidget += 1;
    if (m_numWidget > 4)
    {
        qDebug() << "QnArchiveStreamReader run Sub Stream";
        m_runSubStream = true;
    }
    memset(&m_rewSecondaryStarted, 0, sizeof(m_rewSecondaryStarted));

    m_isStillImage = dev->hasFlags(Qn::still_image);
    if (dev->hasFlags(Qn::still_image) ||                           // disable cycle mode for images
            (
                (dev->hasFlags(Qn::utc) || dev->hasFlags(Qn::live))         // and for live non-local cameras
                && !dev->hasFlags(Qn::local))
            )
        m_cycleMode = false;

}

void QnArchiveStreamReader::callAPIStream(std::string ipHost, std::string portHost)
{
    std::string idCamera = cameraResource->getId().toStdString();
    idCamera.erase(std::remove(idCamera.begin(), idCamera.end(), '{'), idCamera.end());
    idCamera.erase(std::remove(idCamera.begin(), idCamera.end(), '}'), idCamera.end());
    std::string api = "/api/v1/camera/" + idCamera + "/stream";
    qDebug() << nx::format("Get API stream:  %1").arg(api);
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
        qDebug() << "Response Server Oryza stream: " << response_string.c_str();
        QString jsonString = extractJson("{", response_string).c_str();
        QJsonDocument jsonDocument = QJsonDocument::fromJson(jsonString.toUtf8());
        if (!jsonDocument.isNull() && jsonDocument.isObject()) {
            QJsonObject jsonObject = jsonDocument.object();

            if (jsonObject.contains("main_rtsp")) {
                QString mainRtsp = jsonObject["main_rtsp"].toString();
                qDebug() << "Main RTSP:" << mainRtsp;
                m_mainRTSP = mainRtsp.toStdString();
            }

            if (jsonObject.contains("sub_rtsp")) {
                QString subRtsp = jsonObject["sub_rtsp"].toString();
                qDebug() << "Sub RTSP:" << subRtsp;
                m_subRTSP = subRtsp.toStdString();
            }
        } else {
            qDebug() << "Không thể phân tích chuỗi JSON.";
        }
    }
    catch (std::exception &e)
    {
        qDebug() << "Lôi Call API stream: " << e.what();
    }
}



/*
QnArchiveStreamReader::onStatusChanged(nx::vms::api::ResourceStatus oldStatus, nx::vms::api::ResourceStatus newStatus)
{
    if (newStatus == Qn::Offline)
        m_delegate->close();
}
*/

QnArchiveStreamReader::~QnArchiveStreamReader()
{
    stop();

    delete m_frameTypeExtractor;
    m_frameTypeExtractor = 0;
}

void QnArchiveStreamReader::nextFrame()
{
    if (m_navDelegate) {
        m_navDelegate->nextFrame();
        return;
    }
    emit nextFrameOccured();
    NX_MUTEX_LOCKER lock( &m_jumpMtx );
    m_singleQuantProcessed = false;
    m_singleShowWaitCond.wakeAll();
}

void QnArchiveStreamReader::needMoreData()
{
    NX_MUTEX_LOCKER lock( &m_jumpMtx );
    m_singleQuantProcessed = false;
    m_singleShowWaitCond.wakeAll();
}

void QnArchiveStreamReader::previousFrame(qint64 mksec)
{
    --mksec;
    if (m_navDelegate) {
        m_navDelegate->previousFrame(mksec);
        return;
    }
    qDebug() << "KHOI PREV 2";
    emit prevFrameOccured();
    jumpToPreviousFrame(mksec);
}

bool QnArchiveStreamReader::isTimeInRange(int64_t timeToCheck, int64_t startTime, int64_t endTime) {
    return (timeToCheck >= startTime && timeToCheck <= endTime);
}

bool QnArchiveStreamReader::isTimeRecordedInRange(int64_t timeToCheck, int64_t& nearestStartTime, std::string& time) {
    bool foundInRange = false;
    nearestStartTime = std::numeric_limits<int64_t>::max(); // Khởi tạo giá trị gần nhất là giá trị lớn nhất có thể
    if (timeToCheck < m_timeRecordedList[0].startTimeMs)
    {
        nearestStartTime = m_timeRecordedList[0].startTimeMs;
        time = m_timeRecordedList[0].startTimerfc3339;
        return false;
    }
    for (const TimeRecorded& record : m_timeRecordedList) {
        if (record.endTimeMs == -1)
        {
            if (timeToCheck > record.startTimeMs)
            {
                //                qDebug() << "isTimeRecordedInRange 1 timeCheck -> " << timeToCheck << "   start -> " << record.startTimeMs;
                foundInRange = true;
                time = record.startTimerfc3339;
                break;
            }
            else
            {
                qDebug() << "isTimeRecordedInRange 2 timeCheck -> " << timeToCheck << "   start -> " << record.startTimeMs;
                nearestStartTime = record.startTimeMs;
                time = record.startTimerfc3339;
                foundInRange = false;
                break;
            }
        }
        if (isTimeInRange(timeToCheck, record.startTimeMs, record.endTimeMs)) {
            qDebug() << "isTimeRecordedInRange 3 timeCheck -> " << timeToCheck << "   start -> " << record.startTimeMs;
            foundInRange = true;
            time = record.startTimerfc3339;
            break;
        } else  if (record.endTimeMs > timeToCheck) {
            qDebug() << "isTimeRecordedInRange 4 timeCheck -> " << timeToCheck << "   start -> " << record.startTimeMs;
            nearestStartTime = record.startTimeMs;
            time = record.startTimerfc3339;
            break;
        }
    }

    return foundInRange;
}

void QnArchiveStreamReader::resumeMedia()
{

    if (m_navDelegate) {
        m_navDelegate->resumeMedia();
        return;
    }
    if (m_singleShot)
    {
        qDebug() << "KhoiVH START CAM";
        std::string testRTSP = "rtsp://digesttest:a0sm9u0pZgJufg5@192.168.103.5:7001/5e05df9b-d3c7-87a6-cfb2-ac90102b65e9?pos=";
        testRTSP = testRTSP + std::to_string(m_requiredJumpTime);
        emit streamAboutToBeResumed(m_requiredJumpTime);
        if (m_delegate->isServerOryza())
        {
            qDebug() << "isServerOryza m_requiredJumpTime -> " << m_requiredJumpTime;
                if (m_requiredJumpTime != m_lastRequiredJumpTime)
                {
                    int64_t nearestStartTime;
                    std::string time;
                    if (isTimeRecordedInRange(m_requiredJumpTime, nearestStartTime, time))
                    {
                        qDebug() << "Chua time record";
                        nearestStartTime = m_requiredJumpTime;
                        time = ConvertTimeStampToTime(nearestStartTime);
                    }
                    else
                    {
                        qDebug() << "not Chua time record";
                        m_requiredJumpTime = nearestStartTime;
                    }
                    m_delegate->pauseRtsp();
                    while (!m_delegate->isOpenedRTSP())
                    {
                        nearestStartTime += 1000;
                        std::string idCamera = cameraResource->getId().toStdString();
                        std::string rtsp = m_delegate->getUrlRecord(idCamera, ConvertTimeToUrlFormat(time));
                        m_delegate->startRtsp(rtsp);
                    }
                    m_requiredJumpTime = nearestStartTime;
                    m_lastRequiredJumpTime = m_requiredJumpTime;
                    m_isResume = false;
                }
                else
                    {
                    m_isResume = true;
                }

        }
        m_delegate->setSingleshotMode(false);
        m_singleShot = false;
        //resumeDataProcessors();
        NX_MUTEX_LOCKER lock( &m_jumpMtx );
        m_singleShowWaitCond.wakeAll();
        qDebug() << "KhoiVH START CAM 2";
        lock.unlock();
        emit streamResumed();
        //        }
    }
}

void QnArchiveStreamReader::pauseMedia()
{
    qDebug() << "STOP MEDIA";
    if (m_navDelegate) {
        m_navDelegate->pauseMedia();
        return;
    }
    if (!m_singleShot)
    {
        qDebug() << "STOP CAM";

        emit streamAboutToBePaused(m_requiredJumpTime);
        NX_MUTEX_LOCKER lock(&m_jumpMtx);
        m_singleShot = true;
        m_singleQuantProcessed = true;
        if (m_delegate->isServerOryza())
        {

//            if (m_requiredJumpTime != 9223372036854775807)
//            {
//                if (m_requiredJumpTime != m_lastRequiredJumpTime)
//                {

//                }
//            }
        }
        m_delegate->setSingleshotMode(true);

        lock.unlock();
        emit streamPaused();
    }
}

bool QnArchiveStreamReader::isMediaPaused() const
{
    if(m_navDelegate)
        return m_navDelegate->isMediaPaused();
    return m_singleShot;
}

void QnArchiveStreamReader::setCurrentTime(qint64 value)
{
    NX_MUTEX_LOCKER mutex( &m_jumpMtx );
    m_currentTime = value;
}

std::chrono::microseconds QnArchiveStreamReader::currentTime() const
{
    NX_MUTEX_LOCKER mutex( &m_jumpMtx );
    return std::chrono::microseconds(m_skipFramesToTime ? m_skipFramesToTime : m_currentTime);
}

void QnArchiveStreamReader::reopen()
{
    auto currentTime = this->currentTime().count();
    auto isMediaPaused = this->isMediaPaused();
    auto speed = this->getSpeed();

    getArchiveDelegate()->reopen();

    setCurrentTime(currentTime);
    setSpeed(speed);
    if (isMediaPaused)
        pauseMedia();
}

QnConstResourceVideoLayoutPtr QnArchiveStreamReader::getDPVideoLayout() const
{
    if (!(m_delegate->getFlags() & QnAbstractArchiveDelegate::Flag_CanOfflineLayout))
        m_delegate->open(m_resource, m_archiveIntegrityWatcher);
    return m_delegate->getVideoLayout();
}

bool QnArchiveStreamReader::hasVideo() const
{
    if (!m_hasVideo.has_value()) {
        if (!(m_delegate->getFlags() & QnAbstractArchiveDelegate::Flag_CanOfflineHasVideo)) {
            m_delegate->open(m_resource, m_archiveIntegrityWatcher);
        }
        m_hasVideo = m_delegate->hasVideo();
    }
    return *m_hasVideo;
}

AudioLayoutConstPtr QnArchiveStreamReader::getDPAudioLayout() const
{
    if (!(m_delegate->getFlags() & QnAbstractArchiveDelegate::Flag_CanOfflineLayout))
        m_delegate->open(m_resource, m_archiveIntegrityWatcher);
    return m_delegate->getAudioLayout();
}

bool QnArchiveStreamReader::init()
{
    setCurrentTime(0);

    m_jumpMtx.lock();
    qint64 requiredJumpTime = m_requiredJumpTime;
    bool usePreciseSeek = m_lastUsePreciseSeek;
    MediaQuality quality = m_quality;
    QSize resolution = m_customResolution;
    auto streamDataFilter = m_streamDataFilter;
    qint64 jumpTime = qint64(AV_NOPTS_VALUE);
    bool imSeek = m_delegate->getFlags() & QnAbstractArchiveDelegate::Flag_CanSeekImmediatly;
    const double speed = m_speed;
    if (imSeek)
    {
        if (requiredJumpTime != qint64(AV_NOPTS_VALUE))
        {
            jumpTime = requiredJumpTime;
            m_skipFramesToTime = m_tmpSkipFramesToTime;
            m_tmpSkipFramesToTime = 0;
        }
        else if (speed < 0)
        {
            jumpTime = qnSyncTime->currentUSecsSinceEpoch();
        }
    }

    m_jumpMtx.unlock();

    auto canProcessSpeedBeforeOpen = [&](double speed)
    {
        // Ignore zero speed if item is opened on pause.
        // It needs at least one frame to be loaded.
        if (qFuzzyIsNull(speed))
            return false;
        bool imSeek = m_delegate->getFlags().testFlag(
                    QnAbstractArchiveDelegate::Flag_CanSeekImmediatly);
        if (!imSeek)
            return false;
        bool negativeSpeedSupported = m_delegate->getFlags().testFlag(
                    QnAbstractArchiveDelegate::Flag_CanProcessNegativeSpeed);
        if (speed < 0 && !negativeSpeedSupported)
            return false;
        return true;
    };

    m_delegate->setQuality(quality, true, resolution);
    bool isSpeedCommandProcessed = false;
    // It is optimization: open and jump at same time
    if (jumpTime != qint64(AV_NOPTS_VALUE))
    {
        if (canProcessSpeedBeforeOpen(speed))
        {
            m_delegate->setSpeed(jumpTime, speed);
            isSpeedCommandProcessed = true;
        }
        else
        {
            qDebug() << "QnArchiveStreamReader::init() vao seek";
            m_delegate->seek(jumpTime, true);
        }
    }

    m_prevStreamDataFilter = streamDataFilter;
    m_delegate->setStreamDataFilter(streamDataFilter);
    bool opened = m_delegate->open(m_resource, m_archiveIntegrityWatcher);

    if (jumpTime != qint64(AV_NOPTS_VALUE))
    {
        qDebug() << "emitJumpOccured -> " << jumpTime;
        //        emitJumpOccured(jumpTime, usePreciseSeek, m_delegate->getSequence());
    }
    if (m_delegate->isServerOryza()) m_requiredJumpTime = 9223372036854775807;
    if (!opened)
        return false;

    m_delegate->setAudioChannel(m_selectedAudioChannel);

    m_jumpMtx.lock();
    m_oldQuality = quality;
    m_oldQualityFastSwitch = true;
    m_oldResolution = resolution;
    if (isSpeedCommandProcessed)
        m_prevSpeed = speed;
    m_jumpMtx.unlock();

    // Alloc common resources

    if (m_delegate->getFlags() & QnAbstractArchiveDelegate::Flag_SlowSource)
        emit slowSourceHint();

    return true;
}

bool QnArchiveStreamReader::offlineRangeSupported() const
{
    return m_delegate->getFlags() & QnAbstractArchiveDelegate::Flag_CanOfflineRange;
}

qint64 QnArchiveStreamReader::determineDisplayTime(bool reverseMode)
{
    QnlTimeSource* timeSource = 0;
    {
        auto dataProcessors = m_dataprocessors.lock();
        for (int i = 0; i < dataProcessors->size(); ++i)
        {
            QnAbstractDataConsumer* dp = dynamic_cast<QnAbstractDataConsumer*>(dataProcessors->at(i));
            if( !dp )
                continue;
            if (dp->isRealTimeSource())
                return DATETIME_NOW;
            timeSource = dynamic_cast<QnlTimeSource*>(dp);
            if (timeSource)
                break;
        }
    }

    qint64 rez = AV_NOPTS_VALUE;
    if (timeSource)
        rez = timeSource->getExternalTime();

    if(rez == qint64(AV_NOPTS_VALUE))
    {
        if (reverseMode)
            return endTime();
        else
            return startTime();
    }
    return rez;
}

bool QnArchiveStreamReader::getNextVideoPacket()
{
    while(1)
    {
        // Get next video packet and store it
        m_nextData = m_delegate->getNextData();
        if (!m_nextData)
            return false;

        if (m_nextData->dataType == QnAbstractMediaData::EMPTY_DATA)
            return false; // EOF/BOF reached
        if (m_nextData->dataType == QnAbstractMediaData::GENERIC_METADATA)
            m_skippedMetadata << m_nextData;

        QnCompressedVideoDataPtr video = std::dynamic_pointer_cast<QnCompressedVideoData>(m_nextData);
        if (video)
            return true;
    }
}

QnAbstractMediaDataPtr QnArchiveStreamReader::createEmptyPacket(bool isReverseMode)
{
    QnAbstractMediaDataPtr rez(new QnEmptyMediaData());
    rez->timestamp = isReverseMode ? 0 : DATETIME_NOW;
    if (m_BOF)
        rez->flags |= QnAbstractMediaData::MediaFlags_BOF;
    if (m_eof)
        rez->flags |= QnAbstractMediaData::MediaFlags_AfterEOF;
    if (isReverseMode)
        rez->flags |= QnAbstractMediaData::MediaFlags_Reverse;
    if (m_dataMarker)
        rez->opaque = m_dataMarker;
    else
        rez->opaque = m_delegate->getSequence();
    QnSleep::msleep(50);
    return rez;
}

void QnArchiveStreamReader::startPaused(qint64 startTime)
{
    qDebug() << "QnArchiveStreamReader::startPaused";
    m_singleShot = true;
    m_singleQuantProcessed = false;
    m_requiredJumpTime = m_tmpSkipFramesToTime = startTime;
    start();
}

bool QnArchiveStreamReader::isCompatiblePacketForMask(const QnAbstractMediaDataPtr& mediaData) const
{
    if (hasVideo())
    {
        if (mediaData->dataType != QnAbstractMediaData::VIDEO)
            return false;
    }
    else
    {
        if (mediaData->dataType != QnAbstractMediaData::AUDIO)
            return false;
    }
    return !(mediaData->flags & QnAbstractMediaData::MediaFlags_LIVE);
}

std::string QnArchiveStreamReader::extractJson(std::string findStr, const std::string& json) {
    size_t tokenPos = json.find(findStr);
    if (tokenPos != std::string::npos) {
        size_t valueStart = tokenPos + findStr.size(); // Move to the start of the token value
        size_t valueEnd = json.find_first_of("}", valueStart);
        if (valueEnd != std::string::npos) {
            // Extract the token value
            return json.substr(valueStart+1, valueEnd - valueStart + 1);
        }
    }
    return "";
}

std::string QnArchiveStreamReader::getBearerTocken(std::string ipHost, std::string portHost, std::string api)
{
    try
    {
        std::string login = R"({"username": "admin","password": "1"})";
        boost::asio::io_service io_service;
        tcp::resolver resolver(io_service);
        tcp::resolver::query query(ipHost, portHost);
        tcp::resolver::iterator endpoint_iterator = resolver.resolve(query);

        tcp::socket socket(io_service);
        boost::asio::connect(socket, endpoint_iterator);

        // Create the HTTP POST request
        std::string request = "POST " + api +  " HTTP/1.1\r\n"
                                               "Host: " + ipHost + ":" + portHost +"\r\n"
                                                                                   "Content-Type: application/json\r\n"
                                                                                   "Content-Length: " +std::to_string(login.length()) + "\r\n"
                                                                                                                                        "Connection: close\r\n"
                                                                                                                                        "\r\n" + login;

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

        std::string bearer = extractJson(R"("token":)", response_string);
        qDebug() << "Bearer token -> " << bearer.c_str();
        return bearer;
    }
    catch (std::exception &e)
    {
        qDebug() << "Lôi Call API stream: " << e.what();
    }
    return "";
}

void QnArchiveStreamReader::getTimeRecorded()
{
    std::string ipHost = m_delegate->getIpServer();
    std::string portHost = "7005";
    std::string bearer = "Bearer " +  getBearerTocken(ipHost, portHost, "/api/v1/login");
    std::string idCamera = cameraResource->getId().toStdString();
    idCamera.erase(std::remove(idCamera.begin(), idCamera.end(), '{'), idCamera.end());
    idCamera.erase(std::remove(idCamera.begin(), idCamera.end(), '}'), idCamera.end());
    std::string api = "/ec2/recordedTimePeriods?cameraId=" + idCamera + "&detail=1&endTime=9223372036854775807&filter&format=json&groupBy=serverId&limit=2147483647&periodsType=1&startTime=1712183136000&storageLocation=both";
    qDebug() << nx::format("Get API stream:  %1").arg(api);
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
                                                                                  "Authorization: " + bearer + "\r\n"
                                                                                                               "Content-Length: " +std::to_string(0) + "\r\n"
                                                                                                                                                       "Connection: close\r\n"
                                                                                                                                                       "\r\n";

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
        qDebug() << "Response Server Oryza record Time: " << response_string.c_str();
        size_t startPos = response_string.find("{");
        std::string jsonstring = response_string.substr(startPos);

        qDebug() << jsonstring.c_str();
        parseJson(jsonstring.c_str());
    }
    catch (std::exception &e)
    {
        qDebug() << "Lôi Call API stream: " << e.what();
    }
}

long long QnArchiveStreamReader::rfc3339_to_microseconds(const std::string& rfc3339) {
    std::tm tm = {};
    std::istringstream ss(rfc3339);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (ss.fail()) {
        throw std::invalid_argument("Invalid RFC 3339 format");
    }
    std::string micros_str = rfc3339.substr(rfc3339.find('.') + 1);
    micros_str = micros_str.substr(0, micros_str.find('+'));

    int digitsToAdd = 6 - micros_str.length();
    if (digitsToAdd > 0) {
        micros_str.append(digitsToAdd, '0');
    } else if (digitsToAdd < 0) {
        micros_str = micros_str.substr(0, 6);
    }


    long microseconds = std::stol(micros_str);

    std::chrono::system_clock::time_point tp = std::chrono::system_clock::from_time_t(std::mktime(&tm));

    std::string timezone_str = rfc3339.substr(rfc3339.find_last_of('+') + 1, 5);
    int timezone_offset_hours = std::stoi(timezone_str.substr(0, 2));
    int timezone_offset_minutes = std::stoi(timezone_str.substr(3, 2));

    if (timezone_offset_hours > 0)
        tp -= std::chrono::hours(timezone_offset_hours);
    else
        tp += std::chrono::hours(-timezone_offset_hours);
    tp -= std::chrono::minutes(timezone_offset_minutes);

    tp += std::chrono::microseconds(microseconds);
    tp += std::chrono::hours(7);
    auto duration_since_epoch = tp.time_since_epoch();
    long long microsecondsResult = std::chrono::duration_cast<std::chrono::microseconds>(duration_since_epoch).count();
    qDebug() << "Check time rfc3339 -> " << rfc3339.c_str() << "      microseconds -> " << microsecondsResult;

    return microsecondsResult;
}


void QnArchiveStreamReader::parseJson(QString json)
{

    QJsonDocument jsonDoc = QJsonDocument::fromJson(json.toUtf8());


    QJsonArray periodsArray = jsonDoc.object()["periods"].toArray();

    for (const QJsonValue &periodValue : periodsArray) {
        QJsonObject periodObject = periodValue.toObject();

        TimeRecorded timeRecord;
        std::string startTime = periodObject["startTimeMs"].toString().toStdString();
        std::string duration =  periodObject["durationMs"].toString().toStdString();
        long long durationResult;
        if (duration == "-1")
        {
            durationResult = std::stoll(duration);
        }
        else
        {
            float floatValue = std::stof(duration);
            floatValue *= 1000000;
            durationResult = static_cast<long long>(floatValue);
        }
        timeRecord.startTimerfc3339 = startTime;
        long long startTimeMs = rfc3339_to_microseconds(startTime);

        timeRecord.durationMs = durationResult;
        timeRecord.startTimeMs = startTimeMs;
        if (durationResult == -1) timeRecord.endTimeMs = -1;
        else timeRecord.endTimeMs = startTimeMs + durationResult;
        m_timeRecordedList.append(timeRecord);
    }
}

std::string QnArchiveStreamReader::ConvertTimeStampToTime(int64_t timestamp)
{
    boost::posix_time::time_duration duration = boost::posix_time::microseconds(timestamp);

    boost::posix_time::ptime epoch(boost::gregorian::date(1970, 1, 1));
    boost::posix_time::ptime timestampTime = epoch + duration;

    boost::posix_time::time_duration utc_offset(7, 0, 0);
    timestampTime += utc_offset;

    std::ostringstream oss;
    oss << boost::posix_time::to_iso_extended_string(timestampTime) << "+07:00";
    qDebug() << "QnArchiveStreamReader::ConvertTimeStampToTime -> " << oss.str().c_str();
    return oss.str();
}

std::string QnArchiveStreamReader::ConvertTimeToUrlFormat(std::string time)
{
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (auto c : time) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
            continue;
        }

        escaped << std::uppercase;
        escaped << '%' << std::setw(2) << int((unsigned char)c);
        escaped << std::nouppercase;
    }
    qDebug() << "ConvertTimeStampToTime Time URL: " << escaped.str().c_str();
    return escaped.str();
}


void QnArchiveStreamReader::getNextDataOryza(AVPacket** packet, AVCodecContext** pCodecCtx, AVFormatContext** pFormatCtx, qint64* time, std::string rtsp, qint64 *timeStamp)
{
    QMutexLocker locker(&m_qmutex);
    std::string idcam = cameraResource->getId().toStdString();
    idcam = idcam.substr(1, idcam.length() - 2);
    std::string url = "";

    if (m_singleShot)
    {
        LOG_KhoiVH(" m_singleShot true");
        (*packet) = nullptr;
        (*pCodecCtx) = nullptr;
        (*pFormatCtx) = nullptr;
    }
    else
    {
        //        qDebug() << "QnArchiveStreamReader::getNextDataOryza this -> " << this << " timestamp -> " << m_requiredJumpTime;
        std::string testRTSP = "rtsp://digesttest:a0sm9u0pZgJufg5@192.168.103.5:7001/5e05df9b-d3c7-87a6-cfb2-ac90102b65e9";
        if (m_requiredJumpTime == -9223372036854775808)
        {
            LOG_KhoiVH(" m_requiredJumpTime -> -9223372036854775808");
            (*packet) = nullptr;
            (*pCodecCtx) = nullptr;
            (*pFormatCtx) = nullptr;
        }
        else
        {

            if (jumpTime != m_requiredJumpTime)
            {

                jumpTime = m_requiredJumpTime;

                if (jumpTime != 9223372036854775807)
                {
                    LOG_KhoiVH("Run Record");
                    qDebug() << "--------------- Xem Record ---------------";

                    testRTSP = "rtsp://digesttest:a0sm9u0pZgJufg5@192.168.103.5:7001/5e05df9b-d3c7-87a6-cfb2-ac90102b65e9?pos=";
                    testRTSP = testRTSP + std::to_string(m_requiredJumpTime);
                }
                else
                {
//                    if (m_isResume == false)
//                    {
                        LOG_KhoiVH("Run Live");
                        qDebug() << "--------------- Xem Live ---------------";
                        url = m_delegate->getUrlStream(idcam);
                        //                    std::string url = m_delegate->getUrlStream(cameraResource->getId().toStdString());
                        testRTSP = "rtsp://192.168.111.63:8554/mystream1";
                        std::string ipHost = m_delegate->getIpServer();
                        if (ipHost == "192.168.111.63" || ipHost == "localhost") url = "rtsp://192.168.111.63:8554/mystream1";
                        m_delegate->pauseRtsp();
                        m_delegate->startRtsp(url);
//                    }
                }
            }
            *time = jumpTime;
            getNextPacketOryza(packet, pCodecCtx, pFormatCtx, time, url, timeStamp);
            int64_t nearestStartTime;
            std::string time;


            if (m_delegate->readFrameFail())
            {
                qDebug() << "readFrameFail ";
                while (isTimeRecordedInRange(*timeStamp, nearestStartTime, time))
                {
                    qDebug() << "Chua time record *timeStamp -> " << *timeStamp;
                    *timeStamp = *timeStamp + 1000;
                }

                qDebug() << "not Chua time record";
                m_requiredJumpTime = nearestStartTime;
                m_delegate->pauseRtsp();

                std::string idCamera = cameraResource->getId().toStdString();
                std::string rtsp = m_delegate->getUrlRecord(idCamera, ConvertTimeToUrlFormat(time));
                m_delegate->startRtsp(rtsp);

            }
        }

    }

    if (m_firstTime)
    {
        LOG_KhoiVH("Init Stream");
        qDebug() << "QnArchiveStreamReader::getNextDataOryza m_firstTime";
        getTimeRecorded();
        m_BOF = true;
        if (init()) {
            m_firstTime = false;
        }
        else {
            if (m_resource->hasFlags(Qn::local))
                m_firstTime = false; //< Do not try to reopen local file if it can't be opened.
        }
    }
    //    }
}

QnAbstractMediaDataPtr QnArchiveStreamReader::getNextData()
{
    NX_VERBOSE(this, "Next data requested from %1", m_resource);
    while (!m_skippedMetadata.isEmpty())
        return m_skippedMetadata.dequeue();
    if (m_stopCond) {
        NX_MUTEX_LOCKER lock( &m_stopMutex );
        m_delegate->close();
        while (m_stopCond && !needToStop())
            m_stopWaitCond.wait(&m_stopMutex);
        if (needToStop())
        {
            NX_VERBOSE(this, "Need to stop(m_stopCond), return null packet");
            return QnAbstractMediaDataPtr();
        }
        m_delegate->seek(m_latPacketTime, true);
    }

    // =================
    {
        NX_MUTEX_LOCKER mutex( &m_jumpMtx );
        while (m_singleShot
               && m_skipFramesToTime == 0
               && m_singleQuantProcessed
               && m_requiredJumpTime == qint64(AV_NOPTS_VALUE)
               && !needToStop()
               && !isPaused())
        {
            m_singleShowWaitCond.wait(&m_jumpMtx, kSingleShowWaitTimeoutMSec);
        }
        //QnLongRunnable::pause();
    }

    bool singleShotMode = m_singleShot;

begin_label:
    if (needToStop() || m_resource->hasFlags(Qn::removed))
    {
        NX_VERBOSE(this, "Need to stop, return null packet");
        return QnAbstractMediaDataPtr();
    }

    if (m_firstTime)
    {
        // this is here instead if constructor to unload ui thread
        m_BOF = true;
        if (init()) {
            m_firstTime = false;
        }
        else {
            if (m_resource->hasFlags(Qn::local))
                m_firstTime = false; //< Do not try to reopen local file if it can't be opened.
            // If media data can't be opened report 'no data'
            NX_DEBUG(this, "Failed to init, return empty packet");
            return createEmptyPacket(isReverseMode());
        }
    }

    const auto streamDataFilter = m_streamDataFilter;
    if (streamDataFilter != m_prevStreamDataFilter)
    {
        m_delegate->setStreamDataFilter(streamDataFilter);
        m_prevStreamDataFilter = streamDataFilter;
    }

    int channelCount = m_delegate->getVideoLayout()->channelCount();

    m_jumpMtx.lock();
    double speed = m_speed;
    const bool reverseMode = speed < 0;
    const bool prevReverseMode = m_prevSpeed < 0;

    qint64 jumpTime = m_requiredJumpTime;
    bool usePreciseSeek = m_lastUsePreciseSeek;
    MediaQuality quality = m_quality;
    bool qualityFastSwitch = m_qualityFastSwitch;
    QSize resolution = m_customResolution;
    qint64 tmpSkipFramesToTime = m_tmpSkipFramesToTime;
    m_tmpSkipFramesToTime = 0;
    bool exactJumpToSpecifiedFrame = m_exactJumpToSpecifiedFrame;
    qint64 currentTimeHint = m_currentTimeHint;
    m_currentTimeHint = AV_NOPTS_VALUE;

    bool needChangeQuality = m_oldQuality != quality || qualityFastSwitch > m_oldQualityFastSwitch || resolution != m_oldResolution;
    if (needChangeQuality) {
        m_oldQuality = quality;
        m_oldQualityFastSwitch = qualityFastSwitch;
        m_oldResolution = resolution;
    }

    m_dataMarker = m_newDataMarker;

    m_jumpMtx.unlock();

    // change quality checking
    if (needChangeQuality)
    {
        // !m_delegate->isRealTimeSource()
        bool needSeek = m_delegate->setQuality(quality, qualityFastSwitch, resolution);
        if (needSeek && jumpTime == qint64(AV_NOPTS_VALUE) && reverseMode == prevReverseMode)
        {
            qint64 displayTime = determineDisplayTime(reverseMode);
            if (displayTime != qint64(AV_NOPTS_VALUE)) {
                beforeJumpInternal(displayTime);
                if (!exactJumpToSpecifiedFrame && channelCount > 1)
                    setNeedKeyData();
                m_outOfPlaybackMask = false;

                internalJumpTo(displayTime);
                if (displayTime != DATETIME_NOW)
                    setSkipFramesToTime(displayTime, false);

                emitJumpOccured(displayTime, usePreciseSeek, m_delegate->getSequence());
                m_BOF = true;
            }
        }
    }

    // jump command
    if (jumpTime != qint64(AV_NOPTS_VALUE) && reverseMode == prevReverseMode) // if reverse mode is changing, ignore seek, because of reverseMode generate seek operation
    {
        m_outOfPlaybackMask = false;
        /*
        if (m_newDataMarker) {
            QString s;
            QTextStream str(&s);
            str << "setMarker=" << m_newDataMarker
                << " for Time=" << QDateTime::fromMSecsSinceEpoch(m_requiredJumpTime/1000).toString("hh:mm:ss.zzz");
            str.flush();
            NX_INFO(this, s);
        }
        */
        setSkipFramesToTime(tmpSkipFramesToTime, !exactJumpToSpecifiedFrame);
        m_ignoreSkippingFrame = exactJumpToSpecifiedFrame;
        if (!exactJumpToSpecifiedFrame && channelCount > 1)
            setNeedKeyData();
        internalJumpTo(jumpTime);
        emitJumpOccured(jumpTime, usePreciseSeek, m_delegate->getSequence());
        m_BOF = true;
    }
    // reverse mode changing
    bool delegateForNegativeSpeed = m_delegate->getFlags() & QnAbstractArchiveDelegate::Flag_CanProcessNegativeSpeed;

    auto displayTimeToJump = [&]()
    {
        qint64 displayTime = currentTimeHint;
        if (currentTimeHint == DATETIME_NOW) {
            if (reverseMode)
                displayTime = endTime() - BACKWARD_SEEK_STEP;
        }
        else if (currentTimeHint == qint64(AV_NOPTS_VALUE))
        {
            if (jumpTime != qint64(AV_NOPTS_VALUE))
                displayTime = jumpTime;
            else
                displayTime = determineDisplayTime(reverseMode);
        }
        return displayTime;
    };

    if (reverseMode != prevReverseMode)
    {
        if (jumpTime != qint64(AV_NOPTS_VALUE))
            currentTimeHint = jumpTime;
        m_outOfPlaybackMask = false;
        m_bofReached = false;
        qint64 displayTime = displayTimeToJump();

        m_delegate->setSpeed(displayTime, speed);

        if (!delegateForNegativeSpeed)
        {
            if (!exactJumpToSpecifiedFrame && channelCount > 1)
                setNeedKeyData();
            internalJumpTo(displayTime);
            if (reverseMode) {
                if (displayTime != DATETIME_NOW)
                    m_topIFrameTime = displayTime;
            }
            else
                setSkipFramesToTime(displayTime, false);
        }
        else {
            if (!reverseMode && displayTime != DATETIME_NOW && displayTime != qint64(AV_NOPTS_VALUE))
                setSkipFramesToTime(displayTime, false);
        }

        m_lastGopSeekTime = -1;
        m_BOF = true;
        m_afterBOFCounter = 0;
        if (jumpTime != qint64(AV_NOPTS_VALUE))
            emitJumpOccured(displayTime, usePreciseSeek, m_delegate->getSequence());
    }
    else if (speed != m_prevSpeed)
    {
        m_delegate->setSpeed(displayTimeToJump(), speed);
    }

    m_prevSpeed = speed;

    if (m_outOfPlaybackMask)
    {
        if (m_endOfPlaybackHandler)
            m_endOfPlaybackHandler();

        auto result = createEmptyPacket(reverseMode); // EOF reached
        result->flags |= QnAbstractMediaData::MediaFlags_AfterEOF;
        NX_DEBUG(this, "EOF, return empty packet");
        return result;
    }

    if (m_afterMotionData)
    {
        QnAbstractMediaDataPtr result;
        result.swap( m_afterMotionData );
        return result;
    }

    if (m_delegate->startTime() == qint64(AV_NOPTS_VALUE))
    {
        auto result = createEmptyPacket(reverseMode); //< No data at archive
        result->flags |= QnAbstractMediaData::MediaFlags_AfterEOF;
        NX_DEBUG(this, "No data in archive, return empty packet");
        return result;
    }
    QnCompressedVideoDataPtr videoData;

    if (m_skipFramesToTime != 0)
        m_lastGopSeekTime = -1; // after user seek

    // If there is no nextPacket - read it from file, otherwise use saved packet
    if (m_nextData) {
        m_currentData = m_nextData;
        m_nextData.reset();
    }
    else {
        m_currentData = getNextPacket();
    }

    if (m_currentData == 0)
    {
        NX_DEBUG(this, "Null packet from delegate, return null packet");
        return m_currentData;
    }

    if (m_currentData->flags & QnAbstractMediaData::MediaFlags_Skip)
        goto begin_label;

    videoData = std::dynamic_pointer_cast<QnCompressedVideoData>(m_currentData);

    if (m_currentData->timestamp != qint64(AV_NOPTS_VALUE)) {
        setCurrentTime(m_currentData->timestamp);
    }

    // If of archive is reached for reverse mode it need to continue in two cases:
    //  1. it is right (but not left) edge, it need to generate next seek operation
    //  2. Cycle mode flag is set. It need to jump to the end of archive after begin of archive is reached.
    const bool needContinueAfterEof = m_eof && (!m_bofReached || m_cycleMode);
    if (videoData || needContinueAfterEof)
    {
        if (reverseMode && !delegateForNegativeSpeed)
        {
            // I have found example where AV_PKT_FLAG_KEY detected very bad.
            // Same frame sometimes Key sometimes not. It is VC1 codec.
            // Manual detection for it stream better, but has artefacts too. I thinks some data lost in stream after jump
            // (after sequence header always P-frame, not I-Frame. But I returns I, because no I frames at all in other case)

            bool isKeyFrame = false;
            if (videoData)
            {
                isKeyFrame = m_currentData->flags  & AV_PKT_FLAG_KEY;
                if (videoData->context)
                {
                    if (m_frameTypeExtractor == 0 || videoData->context.get() != m_frameTypeExtractor->getContext().get())
                    {
                        delete m_frameTypeExtractor;
                        m_frameTypeExtractor = new FrameTypeExtractor(videoData->context);
                    }
                }

                if (m_frameTypeExtractor)
                {
                    const auto frameType = m_frameTypeExtractor->getFrameType(
                                (const quint8*) videoData->data(), static_cast<int>(videoData->dataSize()));

                    if (frameType != FrameTypeExtractor::UnknownFrameType)
                        isKeyFrame = frameType == FrameTypeExtractor::I_Frame;
                }
            }

            if (m_eof || (m_currentTime == 0 && m_bottomIFrameTime > 0 && m_topIFrameTime >= m_bottomIFrameTime))
            {
                // seek from EOF to BOF occured
                //NX_ASSERT(m_topIFrameTime != DATETIME_NOW);
                setCurrentTime(m_topIFrameTime);
                m_eof = false;
            }

            // Limitation for duration of the first GOP after reverse mode activation
            if (m_afterBOFCounter != -1)
            {
                if (m_afterBOFCounter == 0 && m_currentTime == std::numeric_limits<qint64>::max())
                {
                    // no any packet yet readed from archive and eof reached. So, current time still unknown
                    QnSleep::msleep(10);
                    internalJumpTo(qnSyncTime->currentUSecsSinceEpoch() - BACKWARD_SEEK_STEP);
                    m_afterBOFCounter = 0;
                    goto begin_label;
                }

                m_afterBOFCounter++;
                if (m_afterBOFCounter >= MAX_FIRST_GOP_FRAMES) {
                    m_topIFrameTime = m_currentTime;
                    m_afterBOFCounter = -1;
                }
            }

            // multisensor cameras support
            if (videoData)
            {
                int ch = videoData->channelNumber;
                if (ch > 0 && !m_rewSecondaryStarted[ch])
                {
                    if (isKeyFrame) {
                        videoData->flags |= QnAbstractMediaData::MediaFlags_ReverseBlockStart;
                        m_rewSecondaryStarted[ch] = true;
                    }
                    else
                        goto begin_label; // skip
                }
            }

            if (isKeyFrame || m_currentTime >= m_topIFrameTime)
            {
                if (videoData && m_bottomIFrameTime == -1 && m_currentTime < m_topIFrameTime)
                {
                    m_bottomIFrameTime = m_currentTime;
                    videoData->flags |= QnAbstractMediaData::MediaFlags_ReverseBlockStart;
                    memset(&m_rewSecondaryStarted, 0, sizeof(m_rewSecondaryStarted));
                }
                if (m_currentTime >= m_topIFrameTime)
                {
                    qint64 seekTime;
                    if (m_bofReached)
                    {
                        if (m_cycleMode)
                        {
                            if (m_delegate->endTime() != DATETIME_NOW) {
                                m_topIFrameTime = m_delegate->endTime();
                                m_bottomIFrameTime = seekTime = m_topIFrameTime - BACKWARD_SEEK_STEP;
                            }
                            else {
                                m_topIFrameTime = qnSyncTime->currentUSecsSinceEpoch();
                                seekTime = m_topIFrameTime - LIVE_SEEK_OFFSET;
                            }
                        }
                        else {
                            m_eof = true;
                            NX_DEBUG(this, "EOF reached, return empty packet");
                            return createEmptyPacket(reverseMode);
                        }
                    }
                    else
                    {
                        // sometime av_file_ssek doesn't seek to key frame (seek direct to specified position)
                        // So, no KEY frame may be found after seek. At this case (m_bottomIFrameTime == -1) we increase seek interval
                        qint64 ct = m_currentTime != DATETIME_NOW ? m_currentTime - BACKWARD_SEEK_STEP : m_currentTime;
                        seekTime = m_bottomIFrameTime != -1 ? m_bottomIFrameTime : (m_lastGopSeekTime != -1 ? m_lastGopSeekTime : ct);
                        if (seekTime != DATETIME_NOW)
                            seekTime = qMax(m_delegate->startTime(), seekTime - BACKWARD_SEEK_STEP);
                        else
                            seekTime = qnSyncTime->currentUSecsSinceEpoch() - BACKWARD_SEEK_STEP;
                    }

                    if (m_currentTime != seekTime) {
                        m_currentData.reset();
                        qint64 tmpVal = m_bottomIFrameTime != -1 ? m_bottomIFrameTime : m_topIFrameTime;
                        internalJumpTo(seekTime);
                        m_bofReached = (seekTime == m_delegate->startTime()) || m_topIFrameTime > seekTime;
                        m_lastGopSeekTime = m_topIFrameTime; //seekTime;
                        //NX_ASSERT(m_lastGopSeekTime < DATETIME_NOW/2000ll);
                        m_topIFrameTime = tmpVal;
                        //return getNextData();
                        goto begin_label;
                    }
                    else {
                        m_bottomIFrameTime = m_currentTime;
                        m_topIFrameTime = m_currentTime + BACKWARD_SEEK_STEP;
                    }
                }
            }
            else if (m_bottomIFrameTime == -1) {
                // invalid seek. must be key frame
                m_currentData = QnAbstractMediaDataPtr();
                //return getNextData();
                goto begin_label;
            }
        } // negative speed
    } // videoData || eof

    if (videoData) // in case of video packet
    {
        if (m_skipFramesToTime)
        {
            if (!m_nextData)
            {
                if (!getNextVideoPacket())
                {
                    // Some error or end of file. Stop reading frames.
                    setSkipFramesToTime(0, true);
                    QnAbstractMediaDataPtr tmp;
                    tmp.swap( m_nextData );
                    if (tmp && tmp->dataType == QnAbstractMediaData::EMPTY_DATA)
                    {
                        NX_DEBUG(this, "No video packet, return empty packet");
                        return tmp; //createEmptyPacket(reverseMode); // EOF/BOF reached
                    }
                }
            }

            if (m_nextData)
            {
                if (m_nextData->flags & QnAbstractMediaData::MediaFlags_LIVE)
                    setSkipFramesToTime(0, true);
                else if (!reverseMode && m_nextData->timestamp <= m_skipFramesToTime)
                    videoData->flags |= QnAbstractMediaData::MediaFlags_Ignore;
                else if (reverseMode && m_nextData->timestamp > m_skipFramesToTime)
                    videoData->flags |= QnAbstractMediaData::MediaFlags_Ignore;
                else {
                    if (!m_keepLastSkkipingFrame)
                        videoData->flags |= QnAbstractMediaData::MediaFlags_Ignore; // do not repeat last frame in such mode
                    else
                        videoData->flags &= ~(QnAbstractMediaData::MediaFlags_Ignore);
                    setSkipFramesToTime(0, true);
                }
            }
        }
    }
    else if (m_currentData->dataType == QnAbstractMediaData::AUDIO)
    {
        if (m_skipFramesToTime && m_currentData->timestamp < m_skipFramesToTime)
            goto begin_label;
    }
    else if (m_currentData->dataType == QnAbstractMediaData::EMPTY_DATA)
    {
        m_skipFramesToTime = 0;
    }

    if (videoData && (videoData->flags & QnAbstractMediaData::MediaFlags_Ignore) && m_ignoreSkippingFrame)
    {
        goto begin_label;
    }
    auto mediaRes = m_resource.dynamicCast<QnMediaResource>();
    if (mediaRes && !mediaRes->hasVideo(this))
    {
        if (m_currentData && m_currentData->channelNumber == 0)
        {
            m_codecContext = m_currentData->context;
        }
    }
    else {
        if (videoData && videoData->context)
            m_codecContext = videoData->context;
    }

    if (reverseMode && !delegateForNegativeSpeed)
        m_currentData->flags |= QnAbstractMediaData::MediaFlags_Reverse;

    if (videoData && singleShotMode && !(videoData->flags.testFlag(QnAbstractMediaData::MediaFlags_Ignore)))
    {
        m_singleQuantProcessed = true;
        //m_currentData->flags |= QnAbstractMediaData::MediaFlags_SingleShot;
    }
    if (m_currentData && m_dataMarker)
        m_currentData->opaque = m_dataMarker;

    //if (m_currentData)
    //    qDebug() << "timestamp=" << QDateTime::fromMSecsSinceEpoch(m_currentData->timestamp/1000).toString("hh:mm:ss.zzz") << "flags=" << m_currentData->flags;

    // Do not display archive in a future
    if (!(m_delegate->getFlags() & QnAbstractArchiveDelegate::Flag_UnsyncTime))
    {
        if (!m_resource->hasFlags(Qn::local) &&
                isCompatiblePacketForMask(m_currentData) &&
                m_currentData->timestamp > qnSyncTime->currentUSecsSinceEpoch() && !reverseMode)
        {
            m_outOfPlaybackMask = true;
            NX_DEBUG(this, "Media packet in future, return empty packet");
            return createEmptyPacket(reverseMode); // EOF reached
        }
    }

    // ensure Pos At playback mask
    if (!needToStop() && isCompatiblePacketForMask(m_currentData) && !(m_currentData->flags & QnAbstractMediaData::MediaFlags_Ignore)
            && m_nextData == 0) // check next data because of first current packet may be < required time (but next packet always > required time)
    {
        m_playbackMaskSync.lock();
        qint64 newTime = m_playbackMaskHelper.findTimeAtPlaybackMask(m_currentData->timestamp, !reverseMode);
        m_playbackMaskSync.unlock();

        qint64 maxTime = m_delegate->endTime();
        if (newTime == DATETIME_NOW || newTime == -1 || (maxTime != (qint64)AV_NOPTS_VALUE && newTime > maxTime)) {
            //internalJumpTo(qMax(0ll, newTime)); // seek to end or BOF.
            m_outOfPlaybackMask = true;
            NX_DEBUG(this, "EOF reached(playback mask), return empty packet");
            return createEmptyPacket(reverseMode); // EOF reached
        }

        if (newTime != m_currentData->timestamp)
        {
            if (!exactJumpToSpecifiedFrame && channelCount > 1)
                setNeedKeyData();
            internalJumpTo(newTime);
            setSkipFramesToTime(newTime, true);
            m_BOF = true;
            goto begin_label;
        }
    }

    if (m_currentData && m_currentData->dataType != QnAbstractMediaData::GENERIC_METADATA)
    {
        if (m_eof)
        {
            m_currentData->flags |= QnAbstractMediaData::MediaFlags_AfterEOF;
            m_eof = false;
        }

        /**
         * Consumers, such as CamDisplay, skip data frames if data with MediaFlags_BOF flag has
         * not been received yet. On the other hand, QnAbstractArchiveStreamReader drops non-key
         * frames if it hasn't received one. So, without this condition below
         * (&& m_currentData->flags.testFlag(QnAbstractMediaData::MediaFlags_AVKey)) we may end up
         * with a frame with MediaFlags_BOF flag being dropped because it is not a key frame and
         * consumer won't never get a BOF frame.
         * Above is true only for the video packets => '!videoData' condition.
         */
        if (m_BOF
                && (!videoData || m_currentData->flags.testFlag(QnAbstractMediaData::MediaFlags_AVKey)))
        {
            m_currentData->flags |= QnAbstractMediaData::MediaFlags_BOF;
            m_BOF = false;
        }
    }

    if (m_isStillImage)
        m_currentData->flags |= QnAbstractMediaData::MediaFlags_StillImage;


    // process motion
    if (m_currentData
            && streamDataFilter.testFlag(StreamDataFilter::motion)
            && !m_delegate->providesMotionPackets())
    {
        const int channel = m_currentData->channelNumber;

        updateMetadataReaders(channel, streamDataFilter);

        if (m_motionConnection[channel])
        {
            auto metadata = m_motionConnection[channel]->getMotionData(m_currentData->timestamp);
            if (metadata)
            {
                metadata->flags = m_currentData->flags;
                metadata->opaque = m_currentData->opaque;
                m_afterMotionData = m_currentData;
                return metadata;
            }
        }
    }
    if (m_currentData)
        m_latPacketTime = (m_currentData->flags & QnAbstractMediaData::MediaFlags_LIVE) ? DATETIME_NOW : qMin(qnSyncTime->currentUSecsSinceEpoch(), m_currentData->timestamp);
    return m_currentData;
}

void QnArchiveStreamReader::updateMetadataReaders(int channel, StreamDataFilters filter)
{
    constexpr int kMotionReaderId = 0;

    const bool sendMotion = filter.testFlag(StreamDataFilter::motion);

    if (!m_motionConnection[channel])
        m_motionConnection[channel] = std::make_shared<MetadataMultiplexer>();

    if (sendMotion && !m_motionConnection[channel]->readerById(kMotionReaderId))
    {
        auto motionReader = m_delegate->getMotionConnection(channel);
        if (motionReader)
            m_motionConnection[channel]->add(kMotionReaderId, motionReader);
    }

    if (!sendMotion && m_motionConnection[channel]->readerById(kMotionReaderId))
        m_motionConnection[channel]->removeById(kMotionReaderId);
}

void QnArchiveStreamReader::internalJumpTo(qint64 mksec)
{
    qDebug() << "QQnArchiveStreamReader::internalJumpTo 1";
    m_skippedMetadata.clear();
    m_nextData.reset();
    m_afterMotionData.reset();
    qint64 seekRez = 0;
    if (mksec > 0 || m_resource->hasFlags(Qn::live_cam)) {
        qDebug() << "QQnArchiveStreamReader::internalJumpTo record";
        seekRez = m_delegate->seek(mksec, !m_exactJumpToSpecifiedFrame);
    }
    else {
        // some local files can't correctly jump to 0
        m_delegate->close();
        init();
        m_delegate->open(m_resource, m_archiveIntegrityWatcher);
    }

    m_exactJumpToSpecifiedFrame = false;
    m_wakeup = true;
    m_bottomIFrameTime = -1;
    m_lastGopSeekTime = -1;
    m_topIFrameTime = seekRez != -1 ? seekRez : mksec;
    m_IFrameAfterJumpFound = false;
    m_eof = false;
    m_afterBOFCounter = -1;
    m_bofReached = false;
}

void QnArchiveStreamReader::getNextPacketOryza(AVPacket** packet, AVCodecContext** pCodecCtx, AVFormatContext** pFormatCtx, qint64* time, std::string rtsp, qint64 *timeStamp)
{
    m_delegate->getNextDataOryza(packet, pCodecCtx, pFormatCtx, time, rtsp, timeStamp);
}

QnAbstractMediaDataPtr QnArchiveStreamReader::getNextPacket()
{
    QnAbstractMediaDataPtr result;
    while (!needToStop())
    {

        result = m_delegate->getNextData();
        if (result == 0 && !needToStop())
        {
            if (m_cycleMode)
            {
                if (m_delegate->endTime() < 1000 * 1000 * 5)
                    msleep(200); // prevent to fast file walk for very short files.
                m_delegate->close();
                m_skippedMetadata.clear();
                m_eof = true;

                if (!init())
                    return QnAbstractMediaDataPtr();
                result = m_delegate->getNextData();
                if (result == 0)
                    return result;
            }
            else
            {
                m_eof = true;
                return createEmptyPacket(isReverseMode());
            }
        }

        auto metadata = std::dynamic_pointer_cast<QnAbstractCompressedMetadata>(result);
        if (metadata && metadata->metadataType == MetadataType::ObjectDetection
                && !m_streamDataFilter.testFlag(StreamDataFilter::objects))
        {
            continue;
        }

        if (metadata && metadata->metadataType == MetadataType::Motion
                && !m_streamDataFilter.testFlag(StreamDataFilter::motion))
        {
            continue;
        }

        break;
    }

    return result;
}

unsigned QnArchiveStreamReader::getCurrentAudioChannel() const
{
    return m_selectedAudioChannel;
}

QStringList QnArchiveStreamReader::getAudioTracksInfo() const
{
    QStringList result;
    for (auto& track: m_delegate->getAudioLayout()->tracks())
        result << track.description;
    return result;
}

bool QnArchiveStreamReader::setAudioChannel(unsigned num)
{
    if (!m_delegate->setAudioChannel(num))
        return false;

    m_selectedAudioChannel = num;
    return true;
}

void QnArchiveStreamReader::setSpeedInternal(double value, qint64 currentTimeHint)
{
    if (value == m_speed)
        return;

    bool oldReverseMode = m_speed;
    bool newReverseMode = value;
    m_speed = value;

    if (oldReverseMode != newReverseMode)
    {
        bool useMutex = !m_externalLocked;
        if (useMutex)
            m_jumpMtx.lock();

        m_currentTimeHint = currentTimeHint;
        if (useMutex)
            m_jumpMtx.unlock();
    }

    m_delegate->beforeChangeSpeed(m_speed);
}

/** Is not used and not implemented. */
bool QnArchiveStreamReader::isNegativeSpeedSupported() const
{
    return true; //!m_delegate->getVideoLayout() || m_delegate->getVideoLayout()->channelCount() == 1;
}

bool QnArchiveStreamReader::isSingleShotMode() const
{
    return m_singleShot;
}

void QnArchiveStreamReader::pleaseStop()
{
    QnAbstractArchiveStreamReader::pleaseStop();
    if (m_delegate)
        m_delegate->beforeClose();
    m_singleShowWaitCond.wakeAll();
    m_stopWaitCond.wakeAll();
}

void QnArchiveStreamReader::setEndOfPlaybackHandler(std::function<void()> handler)
{
    m_endOfPlaybackHandler = handler;
    if (m_delegate)
        m_delegate->setEndOfPlaybackHandler(handler);
}

void QnArchiveStreamReader::setErrorHandler(
        std::function<void(const QString& errorString)> handler)
{
    m_errorHandler = handler;
    if (m_delegate)
        m_delegate->setErrorHandler(handler);
}

void QnArchiveStreamReader::setSkipFramesToTime(qint64 skipFramesToTime, bool keepLast)
{
    //NX_MUTEX_LOCKER mutex( &m_jumpMtx );
    m_skipFramesToTime = skipFramesToTime;
    m_keepLastSkkipingFrame = keepLast;
}

bool QnArchiveStreamReader::isSkippingFrames() const
{
    NX_MUTEX_LOCKER mutex( &m_jumpMtx );
    return m_skipFramesToTime != 0 || m_tmpSkipFramesToTime != 0;
}

void QnArchiveStreamReader::channeljumpToUnsync(qint64 mksec, int /*channel*/, qint64 skipTime)
{
    qDebug() << "QnArchiveStreamReader::channeljumpToUnsync " << mksec;

    m_singleQuantProcessed = false;
    m_requiredJumpTime = mksec;
    m_lastUsePreciseSeek = (skipTime != 0);
    m_tmpSkipFramesToTime = skipTime;
    m_singleShowWaitCond.wakeAll();

}

void QnArchiveStreamReader::directJumpToNonKeyFrame(qint64 mksec)
{
    if (mksec == qint64(AV_NOPTS_VALUE))
        return;

    if (m_navDelegate) {
        return m_navDelegate->directJumpToNonKeyFrame(mksec);
    }

    bool useMutex = !m_externalLocked;
    if (useMutex)
        m_jumpMtx.lock();

    beforeJumpInternal(mksec);
    m_exactJumpToSpecifiedFrame = true;
    channeljumpToUnsync(mksec, 0, mksec);

    if (useMutex)
        m_jumpMtx.unlock();
}

void QnArchiveStreamReader::setMarker(int marker)
{
    bool useMutex = !m_externalLocked;
    if (useMutex)
        m_jumpMtx.lock();
    m_newDataMarker = marker;
    if (useMutex)
        m_jumpMtx.unlock();
}

void QnArchiveStreamReader::setSkipFramesToTime(qint64 skipTime)
{
    if (m_navDelegate) {
        return m_navDelegate->setSkipFramesToTime(skipTime);
    }

    setSkipFramesToTime(skipTime, true);
    emit skipFramesTo(skipTime);

    if (isSingleShotMode())
        QnLongRunnable::resume();
}

bool QnArchiveStreamReader::jumpTo(qint64 mksec, qint64 skipTime)
{
    qDebug() << "KHOI PREV 3";
    return jumpToEx(mksec, skipTime, true, nullptr);
}

bool QnArchiveStreamReader::jumpToEx(
        qint64 mksec,
        qint64 skipTime,
        bool bindPositionToPlaybackMask,
        qint64* outJumpTime,
        bool useDelegate)
{
    if (useDelegate && m_navDelegate) {
        return m_navDelegate->jumpTo(mksec, skipTime);
    }

    if (m_resource)
    {
        NX_VERBOSE(this, "Set position %1 for device %2", mksecToDateTime(mksec), m_resource->getId());
    }
    qint64 newTime = mksec;
    if (bindPositionToPlaybackMask)
    {
        m_playbackMaskSync.lock();
        newTime = m_playbackMaskHelper.findTimeAtPlaybackMask(mksec, m_speed >= 0);
        m_playbackMaskSync.unlock();
    }

    if (outJumpTime)
        *outJumpTime = newTime;

    if (newTime != mksec)
        skipTime = 0;

    bool useMutex = !m_externalLocked;
    if (useMutex)
        m_jumpMtx.lock();

    bool usePreciseSeek = (skipTime != 0);
    bool needJump = newTime != m_requiredJumpTime || m_lastUsePreciseSeek != usePreciseSeek;
    if (needJump)
    {

        beforeJumpInternal(newTime);
        qDebug() << nx::format("KHOIVH 3 newtime -> %1").arg(newTime);
        channeljumpToUnsync(newTime, 0, skipTime);
    }

    if(useMutex)
        m_jumpMtx.unlock();

    if (needJump && m_archiveIntegrityWatcher)
        m_archiveIntegrityWatcher->reset();

    if (isSingleShotMode())
        QnLongRunnable::resume();
    return needJump;
}

void QnArchiveStreamReader::beforeJumpInternal(qint64 mksec)
{
    emit beforeJump(mksec);
    m_delegate->beforeSeek(mksec);
}

bool QnArchiveStreamReader::setStreamDataFilter(StreamDataFilters filter)
{
    if (m_delegate->getFlags() & QnAbstractArchiveDelegate::Flag_CanSendMetadata)
    {
        m_streamDataFilter = filter;
        return true;
    }
    return false;
}

nx::vms::api::StreamDataFilters QnArchiveStreamReader::streamDataFilter() const
{
    return m_streamDataFilter;
}

void QnArchiveStreamReader::setStorageLocationFilter(nx::vms::api::StorageLocation filter)
{
    if (m_delegate)
        m_delegate->setStorageLocationFilter(filter);
}

void QnArchiveStreamReader::setPlaybackRange(const QnTimePeriod& playbackRange)
{
    NX_MUTEX_LOCKER lock(&m_playbackMaskSync);
    m_outOfPlaybackMask = false;
    m_playbackMaskHelper.setPlaybackRange(playbackRange);
}

QnTimePeriod QnArchiveStreamReader::getPlaybackRange() const
{
    return m_playbackMaskHelper.getPlaybackRange();
}

void QnArchiveStreamReader::setPlaybackMask(const QnTimePeriodList& playbackMask)
{
    NX_MUTEX_LOCKER lock( &m_playbackMaskSync );
    m_outOfPlaybackMask = false;
    m_playbackMaskHelper.setPlaybackMask(playbackMask);
}

void QnArchiveStreamReader::setQuality(MediaQuality quality, bool fastSwitch, const QSize& resolution)
{
    if (m_quality != quality || fastSwitch > m_qualityFastSwitch || m_customResolution != resolution)
    {
        bool useMutex = !m_externalLocked;
        if (useMutex)
            m_jumpMtx.lock();
        m_quality = quality;
        m_qualityFastSwitch = fastSwitch;
        m_customResolution = resolution;
        if (useMutex)
            m_jumpMtx.unlock();
    }
}

MediaQuality QnArchiveStreamReader::getQuality() const
{
    return m_quality;
}

AVCodecID QnArchiveStreamReader::getTranscodingCodec() const
{
    // TODO: Pass from server. See DEFAULT_VIDEO_CODEC = AV_CODEC_ID_H263P in rtsp_connection.cpp
    return AV_CODEC_ID_H263P;
}

void QnArchiveStreamReader::lock()
{
    m_jumpMtx.lock();
    m_externalLocked = true; // external class locks mutex to perform atomic several params changing
}

void QnArchiveStreamReader::unlock()
{
    m_externalLocked = false;
    m_jumpMtx.unlock();
}

void QnArchiveStreamReader::setArchiveDelegate(QnAbstractArchiveDelegate* contextDelegate)
{
    qDebug() << "QnArchiveStreamReader::setArchiveDelegate";
    m_delegate = contextDelegate;
    if (m_endOfPlaybackHandler)
        m_delegate->setEndOfPlaybackHandler(m_endOfPlaybackHandler);

    if (m_errorHandler)
        m_delegate->setErrorHandler(m_errorHandler);
}

void QnArchiveStreamReader::setSpeed(double value, qint64 currentTimeHint)
{
    if (m_navDelegate) {
        m_navDelegate->setSpeed(value, currentTimeHint);
        return;
    }

    std::unique_ptr<nx::MutexLocker> lock;
    if (!m_externalLocked)
        lock = std::make_unique<nx::MutexLocker>(&m_jumpMtx, __FILE__, __LINE__);
    m_speed = value;
    auto dataProcessors = m_dataprocessors.lock();
    for (int i = 0; i < dataProcessors->size(); ++i)
    {
        QnAbstractDataConsumer* dp = dynamic_cast<QnAbstractDataConsumer*>(dataProcessors->at(i));
        if( !dp )
            continue;
        dp->setSpeed(value);
    }
    setSpeedInternal(value, currentTimeHint);
}

double QnArchiveStreamReader::getSpeed() const
{
    NX_MUTEX_LOCKER mutex(&m_jumpMtx);
    if (m_navDelegate)
        return m_navDelegate->getSpeed();

    return m_speed;
}

CodecParametersConstPtr QnArchiveStreamReader::getCodecContext() const
{
    return m_codecContext;
}

qint64 QnArchiveStreamReader::startTime() const
{
    NX_ASSERT(m_delegate);
    QnTimePeriod p;
    {
        NX_MUTEX_LOCKER lock( &m_playbackMaskSync );
        p = m_playbackMaskHelper.getPlaybackRange();
    }
    if (p.isEmpty())
        return m_delegate->startTime();
    else
        return p.startTimeMs*1000;
}

qint64 QnArchiveStreamReader::endTime() const
{
    NX_ASSERT(m_delegate);
    QnTimePeriod p;
    {
        NX_MUTEX_LOCKER lock( &m_playbackMaskSync );
        p = m_playbackMaskHelper.getPlaybackRange();
    }
    if (p.isEmpty())
        return m_delegate->endTime();
    else
        return p.endTimeMs()*1000;
}

void QnArchiveStreamReader::afterRun()
{
    if (m_delegate)
        m_delegate->close();
}

void QnArchiveStreamReader::setGroupId(const nx::String& guid)
{
    if (m_delegate)
        m_delegate->setGroupId(guid);
}

bool QnArchiveStreamReader::isPaused() const
{
    if (isGroupPlayOnly(getResource()))
    {
        NX_MUTEX_LOCKER lock(&m_stopMutex);
        return m_stopCond;
    }
    else
    {
        return QnAbstractArchiveStreamReader::isPaused();
    }
}

void QnArchiveStreamReader::pause()
{
    if (isGroupPlayOnly(getResource()))
    {
        NX_MUTEX_LOCKER lock( &m_stopMutex );
        m_delegate->beforeClose();
        m_stopCond = true; // for VMAX
    }
    else
    {
        QnAbstractArchiveStreamReader::pause();
    }
}

void QnArchiveStreamReader::resume()
{
    if (isGroupPlayOnly(getResource()))
    {
        NX_MUTEX_LOCKER lock( &m_stopMutex );
        m_stopCond = false; // for VMAX
        m_stopWaitCond.wakeAll();
    }
    else {
        QnAbstractArchiveStreamReader::resume();
    }
}

bool QnArchiveStreamReader::isRealTimeSource() const
{
    return m_delegate && m_delegate->isRealTimeSource() && (m_requiredJumpTime == (qint64)AV_NOPTS_VALUE || m_requiredJumpTime == DATETIME_NOW);
}

bool QnArchiveStreamReader::needKeyData(int channel) const
{
    if (m_quality == MEDIA_Quality_LowIframesOnly)
        return true;
    return base_type::needKeyData(channel);
}

CameraDiagnostics::Result QnArchiveStreamReader::lastError() const
{
    if (!m_delegate)
        return CameraDiagnostics::NoErrorResult();
    return m_delegate->lastError();
}

bool QnArchiveStreamReader::isReverseMode() const
{
    NX_MUTEX_LOCKER lock(&m_jumpMtx);
    return m_speed < 0;
}

bool QnArchiveStreamReader::isJumpProcessing() const
{
    return m_requiredJumpTime != AV_NOPTS_VALUE;
}

void QnArchiveStreamReader::emitJumpOccured(qint64 jumpTime, bool usePreciseSeek, int sequence)
{
    {
        NX_MUTEX_LOCKER mutex(&m_jumpMtx);
        if (m_lastUsePreciseSeek == usePreciseSeek && m_requiredJumpTime == jumpTime)
        {
            m_lastUsePreciseSeek = false;
            m_requiredJumpTime = AV_NOPTS_VALUE;
        }
    }
    emit jumpOccured(jumpTime, sequence);
}
