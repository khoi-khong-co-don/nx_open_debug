// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "server_certificate_watcher.h"

#include <QtCore/QPointer>

#include <common/common_module.h>
#include <core/resource/media_server_resource.h>
#include <nx/network/ssl/certificate.h>
#include <nx/utils/log/assert.h>
#include <nx/vms/client/core/resource/session_resources_signal_listener.h>

#include "certificate_verifier.h"

namespace {

const auto kMinimumSupportedVersion = nx::utils::SoftwareVersion(5, 0);

} // namespace

namespace nx::vms::client::core {

ServerCertificateWatcher::ServerCertificateWatcher(
    QnCommonModule* commonModule,
    CertificateVerifier* certificateVerifier,
    QObject* parent)
    :
    base_type(parent)
{
    auto pinCertificates =
        [commonModule, certificateVerifier = QPointer<CertificateVerifier>(certificateVerifier)](
            const QnMediaServerResourcePtr& server)
        {
            if (!NX_ASSERT(certificateVerifier))
                return;

            // Servers v4.2 or older shouldn't have certificate data.
            if (server->getVersion() < kMinimumSupportedVersion)
                return;

            using namespace nx::network::ssl;

            auto updateKey =
                [commonModule, certificateVerifier, serverId = server->getId()](
                    const std::string& pem,
                    CertificateVerifier::CertificateType certType)
                {
                    if (pem.empty())
                        return;

                    std::vector<Certificate> certificates = Certificate::parse(pem);
                    if (certificates.empty())
                        return;

                    const auto key = certificates[0].publicKey();

                    // Update cached certificate and pin it if possible.
                    certificateVerifier->updateCertificate(serverId, key, certType);
                };
            updateKey(
                server->certificate(),
                CertificateVerifier::CertificateType::autogenerated);
            updateKey(
                server->userProvidedCertificate(),
                CertificateVerifier::CertificateType::connection);
        };

    auto removeCertificatesFromCache =
        [this, certificateVerifier = QPointer<CertificateVerifier>(certificateVerifier)](
            const QnMediaServerResourcePtr& server)
        {
            if (!NX_ASSERT(certificateVerifier))
                return;

            certificateVerifier->removeCertificatesFromCache(server->getId());
        };

    auto serverListener = new SessionResourcesSignalListener<QnMediaServerResource>(this);

    serverListener->setOnAddedHandler(
        [this, pinCertificates](const QnMediaServerResourceList& servers)
        {
            for (const auto& server: servers)
                pinCertificates(server);
        });

    serverListener->addOnSignalHandler(
        &QnMediaServerResource::certificateChanged,
        pinCertificates);

    serverListener->setOnRemovedHandler(
        [this, removeCertificatesFromCache](const QnMediaServerResourceList& servers)
        {
            for (const auto& server: servers)
                removeCertificatesFromCache(server);
        });

    serverListener->start();
}

} // namespace nx::vms::client::core