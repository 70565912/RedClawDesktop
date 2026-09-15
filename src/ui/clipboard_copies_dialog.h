#pragma once
#include "ui/file_transfer_panel.h"
#include <QDialog>
class QListWidget;
class QLabel;
class QPushButton;
namespace redclaw::ui {
class ClipboardCopiesDialog final : public QDialog {
public:
    ClipboardCopiesDialog(FileTransferPanel::Send send, QWidget* parent);
    void start();
    void receive(const protocol::StreamControlMessageV1& message);
    [[nodiscard]] const std::string& selected_id() const { return selected_id_; }
    [[nodiscard]] protocol::WorkspaceTransferPurposeV1 selected_action() const { return action_; }
private:
    void select(protocol::WorkspaceTransferPurposeV1 action);
    FileTransferPanel::Send send_;
    std::string request_, selected_id_;
    protocol::WorkspaceTransferPurposeV1 action_ = protocol::WorkspaceTransferPurposeV1::kClipboardOpenCopy;
    QListWidget* entries_;
    QLabel* status_;
    QPushButton *open_, *cleanup_;
    bool listing_ok_ = false;
};
}
