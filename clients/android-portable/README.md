# Android Portable Companion (Planned Implementation Path)

This directory is reserved for the Android phone/tablet portable client.

Current boundary:
- Connect to remote desktop developer machine.
- Provide status/log/artifact visibility and guarded command presets.
- No controlled keyboard/mouse input in current scope.

The separate Android *Controller* tech spike (Kotlin + NDK, desktop video) lives in [../android-controller-spike](../android-controller-spike/README.md).

Suggested stack:
- Flutter (Android-first)
- Platform channel only for Android-specific integrations when required.
