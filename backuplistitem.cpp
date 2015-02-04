#include "backuplistitem.h"
#include "utils.h"

#include <QDebug>
#include <QFileInfo>
#include <QDesktopServices>
#include <QThreadPool>

BackupListItem::BackupListItem(QUrl url):_count(0), _size(0)
{
    _ui.setupUi(&_widget);
    _widget.addAction(_ui.actionOpen);
    _widget.addAction(_ui.actionRemove);
    _ui.browseButton->setDefaultAction(_ui.actionOpen);
    _ui.removeButton->setDefaultAction(_ui.actionRemove);
    connect(_ui.actionRemove, SIGNAL(triggered()), this, SIGNAL(requestDelete()), Qt::QueuedConnection);
    connect(_ui.actionOpen, SIGNAL(triggered()), this, SLOT(browseUrl()), Qt::QueuedConnection);
    setUrl(url);
}

BackupListItem::~BackupListItem()
{
}

QWidget* BackupListItem::widget()
{
    return &_widget;
}
QUrl BackupListItem::url() const
{
    return _url;
}

void BackupListItem::setUrl(const QUrl &url)
{
    _url = url;

    if(!_url.isEmpty())
    {
        QString fileUrl = _url.toLocalFile();
        if(fileUrl.isEmpty())
            return;
        QFileInfo file(fileUrl);
        if(!file.exists())
            return;
        _ui.pathLabel->setText(fileUrl);
        _ui.pathLabel->setToolTip(fileUrl);
        if(file.isDir())
        {
            QPixmap icon(":/resources/folder-2x.png");
            _ui.iconLabel->setPixmap(icon);
            QDir dir(file.absoluteFilePath());
            QThreadPool *threadPool = QThreadPool::globalInstance();
            Utils::GetDirInfoTask *task = new Utils::GetDirInfoTask(dir);
            task->setAutoDelete(true);
            connect(task, SIGNAL(result(qint64, qint64)), this, SLOT(updateDirDetail(qint64, qint64)), Qt::QueuedConnection);
            threadPool->start(task);
        }
        else if(file.isFile())
        {
            QPixmap icon(":/resources/file-2x.png");
            _ui.iconLabel->setPixmap(icon);
            _count = 1;
            _size  = file.size();
            _ui.detailLabel->setText(QString::number(_size) + " bytes");
        }
        else
        {
            // could be a device file, fifo or whatever, thus ignore
            return;
        }
    }
}

void BackupListItem::browseUrl()
{
    QDesktopServices::openUrl(_url);
}

void BackupListItem::updateDirDetail(qint64 size, qint64 count)
{
    _size = size;
    _count = count;
    _ui.detailLabel->setText(QString::number(_count) + " items totalling "
                             + QString::number(_size) + " bytes");
    emit requestUpdate();
}
qint64 BackupListItem::size() const
{
    return _size;
}

void BackupListItem::setSize(const qint64 &size)
{
    _size = size;
}

qint64 BackupListItem::count() const
{
    return _count;
}

void BackupListItem::setCount(const qint64 &count)
{
    _count = count;
}

