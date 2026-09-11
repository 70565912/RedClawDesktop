# Mobile Portable Roadmap (Android First)

Date: 2026-03-13

## Scope Decision
- Implement now: Android phone/tablet portable companion.
- Planning only (no implementation in current stage):
  - mac portable companion variant
  - generic web mini access client

## Product Boundary
- Mobile only connects to developer-owned remote desktop dev machine.
- No controlled keyboard/mouse input in current mobile scope.
- Primary use: quick status check, logs/artifact review, guarded command presets.

## Milestone Plan
1. M11-T01
- Define portable session capability model and no-controlled-input policy.
- Define guarded command preset contract and risk levels.

2. M11-T02
- Android status dashboard and artifact/log timeline.
- Session resume and low-bandwidth snapshot pull.

3. M11-T03
- Guarded command preset execution flow with confirmation.
- Deliver mac/web planning documents and review checklist.

## Android Dev Environment (When M11 Starts)
- Flutter SDK (stable)
- Android Studio + Android SDK + platform-tools
- JDK 17

## Not Required Now
- macOS/Xcode environment for iOS/mac packaging.
- Web production deployment stack.
