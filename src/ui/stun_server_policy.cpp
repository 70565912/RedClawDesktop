#include "ui/stun_server_policy.h"

#include <QSet>

namespace redclaw::ui {

namespace {

const QString kAutomaticPresetId = QStringLiteral("automatic");
const QString kCustomPresetId = QStringLiteral("custom");

QStringList normalize_server_uris(const QStringList& server_uris) {
  QStringList normalized;
  QSet<QString> seen;
  for (const QString& server_uri : server_uris) {
    const QString trimmed = server_uri.trimmed();
    if (trimmed.isEmpty() || trimmed.startsWith('#') || seen.contains(trimmed)) {
      continue;
    }
    seen.insert(trimmed);
    normalized.push_back(trimmed);
  }
  return normalized;
}

}  // namespace

const QList<StunServerPreset>& stun_server_presets() {
  static const QList<StunServerPreset> presets{
      {
          kAutomaticPresetId,
          QStringLiteral("Automatic (recommended)"),
          QStringLiteral("Prefers Douyu CDN, then uses the fastest responding Google or Cloudflare fallback."),
          {
              QStringLiteral("stun:stun.douyucdn.cn:18000"),
              QStringLiteral("stun:stun.l.google.com:19302"),
              QStringLiteral("stun:stun.cloudflare.com:3478"),
          },
      },
      {
          QStringLiteral("douyu"),
          QStringLiteral("Douyu CDN (best effort)"),
          QStringLiteral("Uses stun.douyucdn.cn:18000. This endpoint has no documented public-service SLA."),
          {QStringLiteral("stun:stun.douyucdn.cn:18000")},
      },
      {
          QStringLiteral("hitv"),
          QStringLiteral("HITV (best effort)"),
          QStringLiteral("Uses stun.hitv.com:3478. This endpoint has no documented public-service SLA."),
          {QStringLiteral("stun:stun.hitv.com:3478")},
      },
      {
          QStringLiteral("bilibili"),
          QStringLiteral("Bilibili (best effort)"),
          QStringLiteral("Uses stun.chat.bilibili.com:3478. This endpoint has no documented public-service SLA."),
          {QStringLiteral("stun:stun.chat.bilibili.com:3478")},
      },
      {
          QStringLiteral("miwifi"),
          QStringLiteral("MiWiFi (best effort)"),
          QStringLiteral("Uses stun.miwifi.com:3478. This endpoint has no documented public-service SLA."),
          {QStringLiteral("stun:stun.miwifi.com:3478")},
      },
      {
          QStringLiteral("google"),
          QStringLiteral("Google"),
          QStringLiteral("Uses stun.l.google.com:19302 only."),
          {QStringLiteral("stun:stun.l.google.com:19302")},
      },
      {
          QStringLiteral("cloudflare"),
          QStringLiteral("Cloudflare"),
          QStringLiteral("Uses Cloudflare's documented public STUN endpoint only."),
          {QStringLiteral("stun:stun.cloudflare.com:3478")},
      },
      {
          kCustomPresetId,
          QStringLiteral("Custom / runtime profile"),
          QStringLiteral("Uses only the additional ICE/TURN entries or the selected runtime profile."),
          {},
      },
  };
  return presets;
}

QString default_stun_server_preset_id() {
  return kAutomaticPresetId;
}

QString custom_stun_server_preset_id() {
  return kCustomPresetId;
}

QString match_stun_server_preset(const QStringList& server_uris) {
  const QStringList normalized = normalize_server_uris(server_uris);
  if (normalized.isEmpty()) {
    return default_stun_server_preset_id();
  }

  for (const auto& preset : stun_server_presets()) {
    if (!preset.server_uris.isEmpty() && normalized == preset.server_uris) {
      return preset.id;
    }
  }
  return custom_stun_server_preset_id();
}

QStringList resolve_ice_server_selection(
    QStringView preset_id,
    const QStringList& additional_server_uris) {
  QStringList selected;
  for (const auto& preset : stun_server_presets()) {
    if (preset.id == preset_id) {
      selected = preset.server_uris;
      break;
    }
  }
  selected.append(additional_server_uris);
  return normalize_server_uris(selected);
}

}  // namespace redclaw::ui
