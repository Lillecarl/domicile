// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

#include "chrome/browser/domicile/domicile_desk_activity.h"

#include <string>
#include <string_view>

#include "base/no_destructor.h"
#include "base/time/time.h"
#include "components/domicile/browser/desk_activity.h"
#include "content/public/browser/browser_thread.h"
#include "ui/base/user_activity/user_activity_detector.h"
#include "ui/base/user_activity/user_activity_observer.h"
#include "ui/events/devices/device_data_manager.h"
#include "ui/events/devices/input_device.h"
#include "ui/events/event.h"
#include "ui/events/platform/platform_event_observer.h"
#include "ui/events/platform/platform_event_source.h"
#include "ui/events/platform_event.h"

namespace domicile {
namespace {

// The name of the device `id` names, or empty for one not listed, like a
// synthesized event's.
std::string_view DeviceNamed(int id) {
  const ui::DeviceDataManager* devices = ui::DeviceDataManager::GetInstance();
  // Each list holds its own subclass of ui::InputDevice.
  const auto in = [id](const auto& listed) -> const ui::InputDevice* {
    for (const ui::InputDevice& device : listed) {
      if (device.id == id) {
        return &device;
      }
    }
    return nullptr;
  };
  for (const ui::InputDevice* device :
       {in(devices->GetKeyboardDevices()), in(devices->GetMouseDevices()),
        in(devices->GetTouchpadDevices()),
        in(devices->GetTouchscreenDevices())}) {
    if (device != nullptr) {
      return device->name;
    }
  }
  return {};
}

ActivityKind KindOf(const ui::Event& event) {
  if (event.IsKeyEvent()) {
    return ActivityKind::kKey;
  }
  if (event.IsTouchEvent()) {
    return ActivityKind::kTouch;
  }
  // A platform's own gestures are a touchpad's. A touch screen's are made from
  // touches, past the platform's event source.
  if (event.IsMouseEvent() || event.IsScrollEvent() ||
      event.IsGestureEvent()) {
    return ActivityKind::kPointer;
  }
  return ActivityKind::kOther;
}

// Whether the reporter hears `kind` from the event source itself.
bool HeardFromEventSource(ActivityKind kind) {
  return kind == ActivityKind::kKey || kind == ActivityKind::kTouch;
}

void Report(const ui::Event& event, ActivityKind kind) {
  DeskActivity::Get().Heard(
      DescribeInput(event.GetName(), DeviceNamed(event.source_device_id())),
      kind);
}

class DeskActivityReporter : public ui::UserActivityObserver,
                             public ui::PlatformEventObserver {
 public:
  DeskActivityReporter() {
    ui::UserActivityDetector::Get()->AddObserver(this);
    ui::PlatformEventSource::GetInstance()->AddPlatformEventObserver(this);
  }

  // ui::UserActivityObserver:
  void OnUserActivity(const ui::Event* event) override {
    // `event` is null for activity reported with no event.
    if (event == nullptr) {
      DeskActivity::Get().Heard("activity", ActivityKind::kOther);
      return;
    }
    const ActivityKind kind = KindOf(*event);
    if (!HeardFromEventSource(kind)) {
      Report(*event, kind);
    }
  }

  // ui::PlatformEventObserver:
  void WillProcessEvent(const ui::PlatformEvent& event) override {}
  void DidProcessEvent(const ui::PlatformEvent& event) override {
    // Ozone's platform events are ui::Events.
    const ActivityKind kind = KindOf(*event);
    if (HeardFromEventSource(kind) &&
        throttle_.ShouldReport(base::TimeTicks::Now())) {
      Report(*event, kind);
    }
  }
  void PlatformEventSourceDestroying() override {
    ui::PlatformEventSource::GetInstance()->RemovePlatformEventObserver(this);
  }

 private:
  // Keys and touches share it, since both always count.
  ActivityThrottle throttle_;
};

}  // namespace

void StartDeskActivity() {
  CHECK_CURRENTLY_ON(content::BrowserThread::UI);
  // Lives for the life of the browser.
  static base::NoDestructor<DeskActivityReporter> reporter;
}

}  // namespace domicile
