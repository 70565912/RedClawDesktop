# ADR: Mobile Companion Tech Choice (Flutter vs .NET MAUI)

Date: 2026-03-13
Status: Accepted
Decision ID: ADR-0001

## Context
The project now includes a mobile/tablet portable companion client (`M11`) for fragmented remote development tasks.
The team needs one cross-platform client stack for Android phone/tablet and iOS/iPadOS while keeping delivery risk low and iteration speed high.

## Decision
Choose Flutter as the primary technology for the mobile/tablet portable companion client.
Keep .NET MAUI as a fallback option only if future team composition and code-sharing priorities materially change.

## Decision Drivers
- Cross-platform consistency for phone/tablet UX.
- Fast UI iteration for status dashboards, logs, and guarded command flows.
- Stable ecosystem for notifications, secure storage, and platform integrations.
- Low coupling with current desktop C++ stack (protocol-first integration).
- VS Code friendly workflow aligned with current development mode.

## Options Considered
1. Flutter
- Strengths:
  - Mature Android/iOS/tablet support and strong UI consistency.
  - High iteration efficiency for product-style companion UI.
  - Good plugin ecosystem for secure storage, biometrics, notifications.
- Trade-offs:
  - Requires Dart/Flutter toolchain onboarding.

2. .NET MAUI
- Strengths:
  - Good if team has heavy C# preference and business logic reuse goals.
  - Integrated experience in Visual Studio ecosystem.
- Trade-offs:
  - Cross-platform UX and plugin maturity can be less predictable for this use case.
  - Higher risk of platform-specific behavior divergence for rapid mobile product iteration.

## Consequences
- Mobile track (`M11`) will be planned and implemented with Flutter-first assumptions.
- Protocol and policy contracts remain platform-agnostic to preserve optional future MAUI pivot.
- Desktop core and host stack remain in C++/CMake/vcpkg.

## Environment Impact (Current Dev Machine)
- Immediate requirement: no additional installation is required to continue core desktop tasks (`M03/M09/M01`).
- Required only when starting `M11` implementation:
  - Flutter SDK (stable channel)
  - Android Studio (SDK + platform-tools + emulator)
  - JDK 17
- iOS build/signing remains Mac-dependent (Xcode required on macOS).

## Revisit Conditions
Re-evaluate this ADR if one of the following occurs:
- Mobile implementation scope shifts to deep C# shared code with backend tools.
- Team staffing becomes predominantly .NET mobile engineers.
- Flutter ecosystem gaps block required security/compliance capabilities.
