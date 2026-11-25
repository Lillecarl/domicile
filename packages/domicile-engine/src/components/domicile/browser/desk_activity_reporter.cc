// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

#include "components/domicile/browser/desk_activity_reporter.h"

#include <utility>

#include "base/time/tick_clock.h"
#include "ui/events/event.h"

namespace domicile {
namespace {

// Whether the reporter hears `kind` from the event source itself.
bool HeardFromEventSource(ActivityKind kind) {
  return kind == ActivityKind::kKey || kind == ActivityKind::kTouch;
}

}  // namespace

ActivityKind ActivityKindOf(const ui::Event& event) {
  if (event.IsKeyEvent()) {
    return ActivityKind::kKey;
  }
  if (event.IsTouchEvent()) {
    return ActivityKind::kTouch;
  }
  // A platform's own gestures are a touchpad's. A touch screen's are made from
  // touches, past the platform's event source.
  if (event.IsMouseEvent() || event.IsScrollEvent() || event.IsGestureEvent()) {
    return ActivityKind::kPointer;
  }
  return ActivityKind::kOther;
}

DeskActivityReporter::DeskActivityReporter(ui::UserActivityDetector* detector,
                                           ui::PlatformEventSource* source,
                                           const base::TickClock* clock,
                                           Report report)
    : clock_(clock), report_(std::move(report)) {
  detector_observation_.Observe(detector);
  source_observation_.Observe(source);
}

DeskActivityReporter::~DeskActivityReporter() = default;

void DeskActivityReporter::OnUserActivity(const ui::Event* event) {
  if (event == nullptr) {
    report_.Run(nullptr, ActivityKind::kOther);
    return;
  }
  const ActivityKind kind = ActivityKindOf(*event);
  if (!HeardFromEventSource(kind)) {
    report_.Run(event, kind);
  }
}

void DeskActivityReporter::WillProcessEvent(const ui::PlatformEvent& event) {}

void DeskActivityReporter::DidProcessEvent(const ui::PlatformEvent& event) {
  // Ozone's platform events are ui::Events.
  const ActivityKind kind = ActivityKindOf(*event);
  if (HeardFromEventSource(kind) &&
      throttle_.ShouldReport(clock_->NowTicks())) {
    report_.Run(event, kind);
  }
}

void DeskActivityReporter::PlatformEventSourceDestroying() {
  source_observation_.Reset();
}

}  // namespace domicile
