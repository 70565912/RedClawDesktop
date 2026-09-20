#pragma once
#include <QString>
class QWidget;
class QDialog;
namespace redclaw::ui {
QDialog* create_file_transfer_results_page(const QString& journal, QWidget* parent);
}
