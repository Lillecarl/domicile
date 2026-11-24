// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

#ifndef CHROME_BROWSER_DOMICILE_DOMICILE_DESK_ACTIVITY_H_
#define CHROME_BROWSER_DOMICILE_DOMICILE_DESK_ACTIVITY_H_

namespace domicile {

// Reports every input event the browser process dispatches to
// `DeskActivity`, for the compositor's idle clock. A key typed into a browser
// window or the shell's page reaches the compositor no other way.
//
// `ui::UserActivityDetector` watches for all but keys and touches: it sees each
// event as the platform dispatches it, whatever page it is for, at most every
// 200 ms, and skips the mouse moves aura synthesizes under a still pointer.
//
// Its 200 ms are shared by every kind of input, so a moving mouse hides keys
// from it, and the compositor may ignore the mouse (`idle.ignore_pointer`).
// Keys and touches are heard from the platform's event source instead, at most
// every 200 ms of their own.
//
// Must be called once, on the UI thread, once the platform's event source
// exists (ChromeBrowserMainParts::PostBrowserStart).
void StartDeskActivity();

}  // namespace domicile

#endif  // CHROME_BROWSER_DOMICILE_DOMICILE_DESK_ACTIVITY_H_
