// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <nx/network/http/auth_tools.h>
#include <nx/network/socket_common.h>
#include <nx/vms/api/data/user_data.h>

namespace nx::vms::client::core {

/**
 * Information about an established client-server connection.
 */
struct ConnectionInfo
{
    nx::network::SocketAddress address;
    nx::network::http::Credentials credentials;
    nx::vms::api::UserType userType = nx::vms::api::UserType::local;

    bool isCloud() const
    {
        return userType == nx::vms::api::UserType::cloud;
    }
};

} // namespace nx::vms::client::core
