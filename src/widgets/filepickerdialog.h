#ifndef FILEPICKERDIALOG_H
#define FILEPICKERDIALOG_H

#include <QDialog>
#include <QUrl>

namespace Ui
{
class FilePickerDialog;
}

class FilePickerDialog : public QDialog
{
    Q_OBJECT

public:
    explicit FilePickerDialog(QWidget *parent = nullptr);
    ~FilePickerDialog();

    QList<QUrl> getSelectedUrls();

private:
    Ui::FilePickerDialog *_ui;
};

#endif // FILEPICKERDIALOG_H
