// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include "nx/streaming/media_data_packet.h"
#include "core/resource/resource_media_layout.h"
#include "utils/camera/camera_diagnostics.h"
extern "C"
{
    #include "libavcodec/avcodec.h"
    #include "libavformat/avformat.h"
    #include "libavutil/pixfmt.h"
    #include "libswscale/swscale.h"
}
class QnAbstractMediaStreamProvider
{
public:
    virtual ~QnAbstractMediaStreamProvider() {}

    enum class ErrorCode
    {
        noError,
        frameLost
    };

    virtual CameraDiagnostics::Result openStream() = 0;
    virtual CameraDiagnostics::Result lastOpenStreamResult() const = 0;
    virtual void closeStream() = 0;
    virtual QnAbstractMediaDataPtr getNextData() = 0;
     virtual void getNextDataOryza(AVPacket** packet, AVCodecContext** pCodecCtx, AVFormatContext** pFormatCtx, qint64* time, std::string rtsp, qint64 *timeStamp) = 0;
    virtual bool isStreamOpened() const = 0;
    //!
    /*!
        TODO #akolesnikov does not look appropriate here
    */
    virtual AudioLayoutConstPtr getAudioLayout() const { return AudioLayoutConstPtr(); };
    virtual QnConstResourceVideoLayoutPtr getVideoLayout() const { return QnConstResourceVideoLayoutPtr(); }
};
