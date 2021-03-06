#ifndef BACKUPTASK_H
#define BACKUPTASK_H

#include <persistentmodel/archive.h>

#include <QObject>

enum TaskStatus
{
    Initialized,
    Queued,
    Running,
    Completed,
    Failed,
    Paused
};

class BackupTask;

typedef QSharedPointer<BackupTask> BackupTaskPtr;

class BackupTask : public QObject
{
    Q_OBJECT

public:
    BackupTask();
    ~BackupTask() {}

    QUuid uuid() const { return _uuid; }
    void setUuid(const QUuid &uuid) { _uuid = uuid; }

    QDateTime timestamp() const { return _timestamp; }
    void      updateTimestamp() { _timestamp = QDateTime::currentDateTime(); }

    QList<QUrl> urls() const { return _urls; }
    void setUrls(const QList<QUrl> &urls) { _urls = urls; }

    QString name() const { return _name; }
    void setName(const QString &name) { _name = name; }

    TaskStatus status() const { return _status; }
    void setStatus(const TaskStatus &status)
    {
        _status = status;
        emit statusUpdate(_uuid, _status);
    }

    int  exitCode() const { return _exitCode; }
    void setExitCode(int exitCode) { _exitCode = exitCode; }

    QString output() const { return _output; }
    void setOutput(const QString &output) { _output = output; }

    ArchivePtr archive() const { return _archive; }
    void setArchive(const ArchivePtr &archive) { _archive = archive; }

    QString jobRef() const { return _jobRef; }
    void setJobRef(const QString &job) { _jobRef = job; }

    bool optionPreservePaths() const;
    void setOptionPreservePaths(bool optionPreservePaths);

    bool optionTraverseMount() const;
    void setOptionTraverseMount(bool optionTraverseMount);

    bool optionFollowSymLinks() const;
    void setOptionFollowSymLinks(bool optionFollowSymLinks);

    quint64 optionSkipFilesSize() const;
    void setOptionSkipFilesSize(const quint64 &optionSkipFilesSize);

    bool optionSkipSystem() const;
    void setOptionSkipSystem(bool optionSkipSystem);

    QStringList optionSkipSystemFiles() const;
    void setOptionSkipSystemFiles(const QStringList &optionSkipSystemFiles);
    void setOptionSkipSystemFiles(const QString string);

    QStringList getExcludesList();

    bool optionDryRun() const;
    void setOptionDryRun(bool optionDryRun);

    bool optionSkipNoDump() const;
    void setOptionSkipNoDump(bool optionSkipNoDump);

    QString command() const;
    void setCommand(const QString &command);

signals:
    void statusUpdate(QUuid uuid, const TaskStatus &status);

private:
    QUuid       _uuid;
    QDateTime   _timestamp;
    QString     _jobRef;
    QList<QUrl> _urls;
    QString     _name;
    bool        _optionPreservePaths;
    bool        _optionTraverseMount;
    bool        _optionFollowSymLinks;
    quint64     _optionSkipFilesSize;
    bool        _optionSkipSystem;
    QStringList _optionSkipSystemFiles;
    bool        _optionDryRun;
    bool        _optionSkipNoDump;

    TaskStatus _status;
    int        _exitCode;
    QString    _output;
    ArchivePtr _archive;
    QString    _command;
};

#endif // BACKUPTASK_H
