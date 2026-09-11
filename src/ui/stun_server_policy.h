#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>

namespace redclaw::ui {

struct StunServerPreset {
  QString id;
  QString label;
  QString detail;
  QStringList server_uris;
};

[[nodiscard]] const QList<StunServerPreset>& stun_server_presets();
[[nodiscard]] QString default_stun_server_preset_id();
[[nodiscard]] QString custom_stun_server_preset_id();
[[nodiscard]] QString match_stun_server_preset(const QStringList& server_uris);
[[nodiscard]] QStringList resolve_ice_server_selection(
    QStringView preset_id,
    const QStringList& additional_server_uris = {});

}  // namespace redclaw::ui
