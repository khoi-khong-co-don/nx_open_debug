// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#ifndef __QN_AUTH_SESSION_H__
#define __QN_AUTH_SESSION_H__

#include <nx/utils/uuid.h>
#include <nx/fusion/model_functions_fwd.h>
#include <nx/string.h>

namespace nx::network { class HostAddress; }
namespace nx::network::http { class Request; }

struct NX_VMS_COMMON_API QnAuthSession
{
    QnUuid id;
    QString userName;
    QString userHost;
    QString userAgent;
    bool isAutoGenerated = false;

    QnAuthSession() = default;
    QnAuthSession(
        const QString& userName,
        const nx::network::http::Request& request,
        const nx::network::HostAddress& hostAddress);

    bool operator==(const QnAuthSession& other) const = default;
    nx::String toString() const;
    void fromString(const nx::String& data);
};

#define QnAuthSession_Fields (id)(userName)(userHost)(userAgent)
QN_FUSION_DECLARE_FUNCTIONS(QnAuthSession,
    (ubjson)(xml)(json)(csv_record)(sql_record),
    NX_VMS_COMMON_API)
NX_REFLECTION_INSTRUMENT(QnAuthSession, QnAuthSession_Fields)

NX_VMS_COMMON_API void serialize_field(const QnAuthSession &authData, QVariant *target);
NX_VMS_COMMON_API void deserialize_field(const QVariant &value, QnAuthSession *target);

#endif // __QN_AUTH_SESSION_H__
