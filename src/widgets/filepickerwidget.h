#ifndef FILEPICKER_H
#define FILEPICKER_H

#include "customfilesystemmodel.h"
#include "ui_filepickerwidget.h"

#include <QCompleter>
#include <QFileSystemModel>
#include <QWidget>

namespace Ui
{
class FilePickerWidget;
}

class FilePickerWidget : public QWidget
{
    Q_OBJECT

public:
    explicit FilePickerWidget(QWidget *parent = nullptr);
    ~FilePickerWidget();

    void        reset();
    QList<QUrl> getSelectedUrls();
    void        setSelectedUrls(const QList<QUrl> &urls);

public slots:
    void updateFilter(QString filter);
    void setCurrentPath(const QString path);

signals:
    void selectionChanged();
    void focusLost();

protected:
    void keyReleaseEvent(QKeyEvent *event);
    bool eventFilter(QObject *obj, QEvent *event);

private:
    Ui::FilePickerWidget       *_ui;
    CustomFileSystemModel _model;
    QCompleter            _completer;
};

#endif // FILEPICKER_H
