// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <QtCore/QObject>

#include <ui/workbench/workbench_context_aware.h>

namespace nx::vms::client::desktop {
namespace ui {
namespace workbench {

class ResourceTreeSettingsActionHandler: public QObject, public QnWorkbenchContextAware
{
    Q_OBJECT
    using base_type = QObject;

public:
    ResourceTreeSettingsActionHandler(QObject* parent = nullptr);

private:
    void showServersInResourceTree();
    void hideServersInResourceTree();
};

} // namespace workbench
} // namespace ui
} // namespace nx::vms::client::desktop
