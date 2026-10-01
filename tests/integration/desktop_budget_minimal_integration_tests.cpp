#include "desktop_budget_roundtrip_cases.h"

TEST(FfmpegRoundtripIntegration, DesktopBudgetSurvivesLowCadenceResizeAndReconnect) {
    check_desktop_budget_after_low_cadence_resize(redclaw::capture::EncoderBackendType::kSoftware);
}
