// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "abstract_archive_stream_reader.h"

#include <nx/utils/log/log.h>
#include <utils/common/util.h>

#include <recording/time_period.h>
#include <recording/time_period_list.h>
#include "core/resource/media_resource.h"
#include "utils/common/sleep.h"
#include <nx/streaming/video_data_packet.h>
#include "abstract_archive_integrity_watcher.h"
#include <utils/common/synctime.h>

#include <media/filters/abstract_media_data_filter.h>


QnAbstractArchiveStreamReader::QnAbstractArchiveStreamReader(const QnResourcePtr& resource):
    QnAbstractMediaStreamDataProvider(resource)
{
    qDebug() << "Tao QnAbstractArchiveStreamReader";
}

QnAbstractArchiveStreamReader::~QnAbstractArchiveStreamReader()
{

    stop();
    delete m_delegate;

        qDebug() << "~QnAbstractArchiveStreamReader 1";
//        av_packet_unref(packet);
        qDebug() << "~QnAbstractArchiveStreamReader 2";
//        if (pCodecCtx != NULL) avcodec_close(pCodecCtx);
        qDebug() << "~QnAbstractArchiveStreamReader 3";
//        if (pFormatCtx != NULL) avformat_close_input(&pFormatCtx);
        qDebug() << "~QnAbstractArchiveStreamReader 4";
}

QnAbstractNavigator *QnAbstractArchiveStreamReader::navDelegate() const
{
    return m_navDelegate;
}

// ------------------- Audio tracks -------------------------

unsigned QnAbstractArchiveStreamReader::getCurrentAudioChannel() const
{
    return 0;
}

CameraDiagnostics::Result QnAbstractArchiveStreamReader::diagnoseMediaStreamConnection()
{
    //TODO/IMPL
    return CameraDiagnostics::Result(CameraDiagnostics::ErrorCode::unknown );
}

QStringList QnAbstractArchiveStreamReader::getAudioTracksInfo() const
{
    return QStringList();
}

bool QnAbstractArchiveStreamReader::setAudioChannel(unsigned /*num*/)
{
    return false;
}

void QnAbstractArchiveStreamReader::setNavDelegate(QnAbstractNavigator* navDelegate)
{
    m_navDelegate = navDelegate;
}

QnAbstractArchiveDelegate* QnAbstractArchiveStreamReader::getArchiveDelegate() const
{
    return m_delegate;
}

void QnAbstractArchiveStreamReader::setCycleMode(bool value)
{
    m_cycleMode = value;
}

bool QnAbstractArchiveStreamReader::open(AbstractArchiveIntegrityWatcher* archiveIntegrityWatcher)
{
    m_archiveIntegrityWatcher = archiveIntegrityWatcher;
    return m_delegate && m_delegate->open(m_resource, archiveIntegrityWatcher);
}

void QnAbstractArchiveStreamReader::jumpToPreviousFrame(qint64 usec)
{
    qDebug() << "VAO JumpTo 18";
    if (usec != DATETIME_NOW)
        jumpTo(qMax(0ll, usec - 200 * 1000), usec-1);
    else
        jumpTo(usec, 0);
}

quint64 QnAbstractArchiveStreamReader::lengthUsec() const
{
    if (m_delegate)
        return m_delegate->endTime() - m_delegate->startTime();
    else
        return AV_NOPTS_VALUE;
}

void QnAbstractArchiveStreamReader::addMediaFilter(const std::shared_ptr<AbstractMediaDataFilter>& filter)
{
    m_filters.push_back(filter);
}

void QnAbstractArchiveStreamReader::run()
{
//    qDebug() << "Run camera";





    if (m_delegate->isServerOryza())
    {
        LOG_KhoiVH("SERVER ORYZA STREAM");
        qDebug()<< "SERVER ORYZA STREAM";

    ///////////// SERVER ORYZA STREAM//////////
        initSystemThreadId();

        NX_VERBOSE(this, "Started");
        beforeRun();
        qDebug() << "Run camera Oryza";
        qint64 time = 9223372036854775807;
        while(!needToStop())
        {
            pauseDelay();
            if (needToStop())
                break;
//            checkAndFixTimeFromCamera(data);

            if (!dataCanBeAccepted())
            {
                emit waitForDataCanBeAccepted();
                QnSleep::msleep(10);
                continue;
            }
            qint64 timestamp;
            getNextDataOryza(&packet, &pCodecCtx, &pFormatCtx, &time, "", &timestamp);
            if (packet == nullptr)
            {timestamp = time;}
//                int64_t timestamp = pFormatCtx->start_time_realtime + packet->pts*10;
                putDataOryza(std::move(packet), std::move(pCodecCtx), std::move(pFormatCtx), this, timestamp);
                av_free_packet(packet);
//            }
//            else
//            {
//            }
        }

        afterRun();
        NX_VERBOSE(this, "Stopped");
    }
    else
    {
        ///////////// SERVER NX STREAM//////////

        qDebug() << "SERVER NX STREAM";
            initSystemThreadId();

            NX_VERBOSE(this, "Started");
            beforeRun();

            while(!needToStop())
            {
                pauseDelay(); // pause if needed;
                if (needToStop()) // extra check after pause
                    break;

                // check queue sizes

                if (!dataCanBeAccepted())
                {
                    emit waitForDataCanBeAccepted();
                    QnSleep::msleep(10);
                    continue;
                }

                QnAbstractMediaDataPtr data = getNextData();

                if (data)
                {
                    NX_VERBOSE(this, "New data, timestamp: %1, type: %2, flags: %3, channel: %4",
                        data->timestamp, data->dataType, data->flags, data->channelNumber);
                }
                else
                {
                    NX_VERBOSE(this, "Null data");
                }

                for (const auto& filter: m_filters)
                {
                    data = std::const_pointer_cast<QnAbstractMediaData>(
                        std::dynamic_pointer_cast<const QnAbstractMediaData>(filter->processData(data)));
                }


                if (m_noDataHandler && (!data || data->dataType == QnAbstractMediaData::EMPTY_DATA))
                    m_noDataHandler();

                if (!data && !needToStop())
                {
                    setNeedKeyData();
                    onEvent(CameraDiagnostics::BadMediaStreamResult());

                    QnSleep::msleep(30);
                    continue;
                }

                checkAndFixTimeFromCamera(data);

                QnCompressedVideoDataPtr videoData = std::dynamic_pointer_cast<QnCompressedVideoData>(data);

                if (videoData && videoData->channelNumber>CL_MAX_CHANNEL_NUMBER-1)
                {
                    NX_ASSERT(false);
                    continue;
                }

                if (videoData && needKeyData(videoData->channelNumber))
                {
                    if (videoData->flags & AV_PKT_FLAG_KEY)
                    {
                        m_gotKeyFrame.at(videoData->channelNumber)++;
                    }
                    else
                    {
                        NX_VERBOSE(this, "Skip non-key frame: %1, type: %2", data->timestamp, data->dataType);
                        continue; // need key data but got not key data
                    }
                }

                if(data)
                {
                    data->dataProvider = this;
                    if (data->flags &
                        (QnAbstractMediaData::MediaFlags_AfterEOF | QnAbstractMediaData::MediaFlags_BOF))
                    {
                        m_stat[data->channelNumber].reset();
                    }
                }

                auto mediaRes = m_resource.dynamicCast<QnMediaResource>();
                if (mediaRes && !mediaRes->hasVideo(this))
                {
                    if (data)
                        m_stat[data->channelNumber].onData(data);
                }
                else {
                    if (videoData)
                        m_stat[data->channelNumber].onData(data);
                }

                putData(std::move(data));
            }

            afterRun();
            NX_VERBOSE(this, "Stopped");
    }
}

void QnAbstractArchiveStreamReader::setNoDataHandler(
    nx::utils::MoveOnlyFunc<void()> noDataHandler)
{
    m_noDataHandler = std::move(noDataHandler);
}
