#include "taskmanager.h"

#include "debug.h"
#include "utils.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QTimer>

#define SUCCESS 0

TaskManager::TaskManager()
    : _threadPool(QThreadPool::globalInstance()),
      _aggressiveNetworking(false),
      _preservePathnames(true),
      _headless(false)
{
}

TaskManager::~TaskManager()
{
}

bool TaskManager::headless() const
{
    return _headless;
}

void TaskManager::setHeadless(bool headless)
{
    _headless = headless;
}

void TaskManager::loadSettings()
{
    QSettings settings;

    _tarsnapDir      = settings.value("tarsnap/path").toString();
    _tarsnapCacheDir = settings.value("tarsnap/cache").toString();
    _tarsnapKeyFile  = settings.value("tarsnap/key").toString();
    _aggressiveNetworking =
        settings.value("tarsnap/aggressive_networking", false).toBool();
    _preservePathnames =
        settings.value("tarsnap/preserve_pathnames", true).toBool();
}

void TaskManager::getTarsnapVersion(QString tarsnapPath)
{
    TarsnapTask *tarsnap = new TarsnapTask();
    if(tarsnapPath.isEmpty())
        tarsnap->setCommand(CMD_TARSNAP);
    else
        tarsnap->setCommand(tarsnapPath + QDir::separator() + CMD_TARSNAP);
    tarsnap->setArguments(QStringList("--version"));
    connect(tarsnap, &TarsnapTask::finished, this,
            &TaskManager::getTarsnapVersionFinished, QUEUED);
    queueTask(tarsnap);
}

void TaskManager::registerMachine(QString user, QString password,
                                  QString machine, QString key,
                                  QString tarsnapPath, QString cachePath)
{
    TarsnapTask *registerTask = new TarsnapTask();
    QStringList  args;
    QFileInfo    keyFile(key);
    if(keyFile.exists())
    {
        // existing key, just check with a tarsnap --print-stats command
        args << "--fsck-prune"
             << "--keyfile" << key << "--cachedir" << cachePath;
        registerTask->setCommand(tarsnapPath + QDir::separator() + CMD_TARSNAP);
        registerTask->setArguments(args);
    }
    else
    {
        // register machine with tarsnap-keygen
        args << "--user" << user << "--machine" << machine << "--keyfile" << key;
        registerTask->setCommand(tarsnapPath + QDir::separator() +
                                 CMD_TARSNAPKEYGEN);
        registerTask->setArguments(args);
        registerTask->setPassword(password);
        registerTask->setRequiresPassword(true);
    }
    connect(registerTask, &TarsnapTask::finished, this,
            &TaskManager::registerMachineFinished, QUEUED);
    queueTask(registerTask);
}

void TaskManager::backupNow(BackupTaskPtr backupTask)
{
    if(backupTask == nullptr)
    {
        DEBUG << "Null BackupTaskPtr passed.";
        return;
    }

    _backupTaskMap[backupTask->uuid()] = backupTask;
    TarsnapTask *bTask = new TarsnapTask();
    QStringList  args;
    if(!_tarsnapKeyFile.isEmpty())
        args << "--keyfile" << _tarsnapKeyFile;
    if(!_tarsnapCacheDir.isEmpty())
        args << "--cachedir" << _tarsnapCacheDir;
    if(_aggressiveNetworking)
        args << "--aggressive-networking";
    if(backupTask->optionDryRun())
        args << "--dry-run";
    if(backupTask->optionSkipNoDump())
        args << "--nodump";
    if(backupTask->optionPreservePaths())
        args << "-P";
    if(!backupTask->optionTraverseMount())
        args << "--one-file-system";
    if(backupTask->optionFollowSymLinks())
        args << "-L";
    if(Utils::tarsnapVersionMinimum("1.0.36"))
        args << "--creationtime"
             << QString::number(backupTask->timestamp().toTime_t());
    args << "--quiet"
         << "--print-stats"
         << "--no-humanize-numbers"
         << "-c"
         << "-f" << backupTask->name();
    foreach(QString exclude, backupTask->getExcludesList())
    {
        args << "--exclude" << exclude;
    }
    foreach(QUrl url, backupTask->urls())
    {
        args << url.toLocalFile();
    }
    bTask->setCommand(makeTarsnapCommand(CMD_TARSNAP));
    bTask->setArguments(args);
    backupTask->setCommand(bTask->command() + " " + bTask->arguments().join(" "));
    bTask->setData(backupTask->uuid());
    connect(bTask, &TarsnapTask::finished, this,
            &TaskManager::backupTaskFinished, QUEUED);
    connect(bTask, &TarsnapTask::started, this, &TaskManager::backupTaskStarted,
            QUEUED);
    connect(backupTask.data(), &BackupTask::statusUpdate, this,
            &TaskManager::notifyBackupTaskUpdate, QUEUED);
    backupTask->setStatus(TaskStatus::Queued);
    queueTask(bTask, true);
}

void TaskManager::getArchives()
{
    TarsnapTask *listArchivesTask = new TarsnapTask();
    QStringList  args;
    if(!_tarsnapKeyFile.isEmpty())
        args << "--keyfile" << _tarsnapKeyFile;
    if(!_tarsnapCacheDir.isEmpty()) // We shouldn't need to pass this as per the
                                    // man page, however Tarsnap CLI seems to
                                    // require it
        args << "--cachedir" << _tarsnapCacheDir;
    args << "--list-archives"
         << "-vv";
    listArchivesTask->setCommand(makeTarsnapCommand(CMD_TARSNAP));
    listArchivesTask->setArguments(args);
    connect(listArchivesTask, &TarsnapTask::finished, this,
            &TaskManager::getArchiveListFinished, QUEUED);
    connect(listArchivesTask, &TarsnapTask::started, this,
            [&]() { emit message(tr("Updating archives list from remote...")); },
            QUEUED);
    queueTask(listArchivesTask);
}

void TaskManager::loadArchives()
{
    _archiveMap.clear();
    PersistentStore &store = PersistentStore::instance();
    if(!store.initialized())
    {
        DEBUG << "PersistentStore was not initialized properly.";
        return;
    }
    QSqlQuery query = store.createQuery();
    if(!query.prepare(QLatin1String("select name from archives")))
    {
        DEBUG << query.lastError().text();
        return;
    }
    if(store.runQuery(query) && query.next())
    {
        do
        {
            ArchivePtr archive(new Archive);
            archive->setName(
                query.value(query.record().indexOf("name")).toString());
            archive->load();
            _archiveMap[archive->name()] = archive;
        } while(query.next());
    }
    emit archiveList(_archiveMap.values());
}

void TaskManager::getArchiveStats(ArchivePtr archive)
{
    if(archive.isNull())
    {
        DEBUG << "Null ArchivePtr passed.";
        return;
    }

    _archiveMap.insert(archive->name(), archive);

    TarsnapTask *statsTask = new TarsnapTask();
    QStringList  args;
    if(!_tarsnapKeyFile.isEmpty())
        args << "--keyfile" << _tarsnapKeyFile;
    if(!_tarsnapCacheDir.isEmpty())
        args << "--cachedir" << _tarsnapCacheDir;
    args << "--print-stats"
         << "--no-humanize-numbers"
         << "-f" << archive->name();
    statsTask->setCommand(makeTarsnapCommand(CMD_TARSNAP));
    statsTask->setArguments(args);
    statsTask->setData(archive->name());
    connect(statsTask, &TarsnapTask::finished, this,
            &TaskManager::getArchiveStatsFinished, QUEUED);
    connect(statsTask, &TarsnapTask::started, this,
            [=]() {
                emit message(tr("Fetching stats for archive <i>%1</i>...")
                                 .arg(archive->name()));
            },
            QUEUED);
    queueTask(statsTask);
}

void TaskManager::getArchiveContents(ArchivePtr archive)
{
    if(archive.isNull())
    {
        DEBUG << "Null ArchivePtr passed.";
        return;
    }

    _archiveMap.insert(archive->name(), archive);

    TarsnapTask *contentsTask = new TarsnapTask();
    QStringList  args;
    if(!_tarsnapKeyFile.isEmpty())
        args << "--keyfile" << _tarsnapKeyFile;
    if(!_tarsnapCacheDir.isEmpty()) // We shouldn't need to pass this as per the
                                    // man page, however Tarsnap CLI seems to
                                    // require it
        args << "--cachedir" << _tarsnapCacheDir;
    if(_preservePathnames)
        args << "-P";
    args << "-t"
         << "-f" << archive->name();
    contentsTask->setCommand(makeTarsnapCommand(CMD_TARSNAP));
    contentsTask->setArguments(args);
    contentsTask->setData(archive->name());
    connect(contentsTask, &TarsnapTask::finished, this,
            &TaskManager::getArchiveContentsFinished, QUEUED);
    connect(contentsTask, &TarsnapTask::started, this,
            [=]() {
                emit message(tr("Fetching contents for archive <i>%1</i>...")
                                 .arg(archive->name()));
            },
            QUEUED);
    queueTask(contentsTask);
}

void TaskManager::deleteArchives(QList<ArchivePtr> archives)
{
    if(archives.isEmpty())
    {
        DEBUG << "Empty QList<ArchivePtr> passed.";
        return;
    }

    TarsnapTask *delArchives = new TarsnapTask();
    QStringList  args;
    if(!_tarsnapKeyFile.isEmpty())
        args << "--keyfile" << _tarsnapKeyFile;
    if(!_tarsnapCacheDir.isEmpty())
        args << "--cachedir" << _tarsnapCacheDir;
    args << "--print-stats"
         << "-d";
    foreach(ArchivePtr archive, archives)
    {
        args << "-f" << archive->name();
    }
    delArchives->setCommand(makeTarsnapCommand(CMD_TARSNAP));
    delArchives->setArguments(args);
    delArchives->setData(QVariant::fromValue(archives));
    connect(delArchives, &TarsnapTask::finished, this,
            &TaskManager::deleteArchivesFinished, QUEUED);
    connect(delArchives, &TarsnapTask::started, this,
            [=](QVariant data) {
                QList<ArchivePtr> archives = data.value<QList<ArchivePtr>>();
                notifyArchivesDeleted(archives, false);
            },
            QUEUED);
    queueTask(delArchives, true);
}

void TaskManager::getOverallStats()
{
    TarsnapTask *overallStats = new TarsnapTask();
    QStringList  args;
    if(!_tarsnapKeyFile.isEmpty())
        args << "--keyfile" << _tarsnapKeyFile;
    if(!_tarsnapCacheDir.isEmpty())
        args << "--cachedir" << _tarsnapCacheDir;
    args << "--print-stats"
         << "--no-humanize-numbers";
    overallStats->setCommand(makeTarsnapCommand(CMD_TARSNAP));
    overallStats->setArguments(args);
    connect(overallStats, &TarsnapTask::finished, this,
            &TaskManager::overallStatsFinished, QUEUED);
    queueTask(overallStats);
}

void TaskManager::fsck(bool prune)
{
    TarsnapTask *fsck = new TarsnapTask();
    QStringList  args;
    if(!_tarsnapKeyFile.isEmpty())
        args << "--keyfile" << _tarsnapKeyFile;
    if(!_tarsnapCacheDir.isEmpty())
        args << "--cachedir" << _tarsnapCacheDir;
    if(prune)
        args << "--fsck-prune";
    else
        args << "--fsck";
    fsck->setCommand(makeTarsnapCommand(CMD_TARSNAP));
    fsck->setArguments(args);
    connect(fsck, &TarsnapTask::finished, this, &TaskManager::fsckFinished,
            QUEUED);
    queueTask(fsck, true);
}

void TaskManager::nuke()
{
    TarsnapTask *nuke = new TarsnapTask();
    QStringList  args;
    if(!_tarsnapKeyFile.isEmpty())
        args << "--keyfile" << _tarsnapKeyFile;
    if(!_tarsnapCacheDir.isEmpty())
        args << "--cachedir" << _tarsnapCacheDir;
    args << "--nuke";
    nuke->setCommand(makeTarsnapCommand(CMD_TARSNAP));
    nuke->setPassword("No Tomorrow");
    nuke->setRequiresPassword(true);
    nuke->setArguments(args);
    connect(nuke, &TarsnapTask::finished, this, &TaskManager::nukeFinished,
            QUEUED);
    connect(nuke, &TarsnapTask::started, this,
            [=]() { emit message(tr("Archives purge initiated...")); }, QUEUED);
    queueTask(nuke, true);
}

void TaskManager::restoreArchive(ArchivePtr archive, ArchiveRestoreOptions options)
{
    if(archive.isNull())
    {
        DEBUG << "Null ArchivePtr passed.";
        return;
    }

    _archiveMap.insert(archive->name(), archive);

    TarsnapTask *restore = new TarsnapTask();
    QStringList  args;
    if(!_tarsnapKeyFile.isEmpty())
        args << "--keyfile" << _tarsnapKeyFile;
    if(options.optionRestore)
        args << "-x"
             << "-P";
    if(options.optionRestoreDir)
        args << "-x"
             << "-C" << options.path;
    if((options.optionRestore || options.optionRestoreDir) &&
       !options.overwriteFiles)
        args << "-k";
    if((options.optionRestore || options.optionRestoreDir) &&
       options.keepNewerFiles)
        args << "--keep-newer-files";
    if(options.optionDownArchive)
    {
        args << "-r";
        restore->setStandardOutputFile(options.path);
    }
    args << "-f" << archive->name();
    restore->setCommand(makeTarsnapCommand(CMD_TARSNAP));
    restore->setArguments(args);
    restore->setData(archive->name());
    connect(restore, &TarsnapTask::finished, this,
            &TaskManager::restoreArchiveFinished, QUEUED);
    connect(restore, &TarsnapTask::started, this,
            [=]() {
                emit message(
                    tr("Restoring archive <i>%1</i>...").arg(archive->name()));
            },
            QUEUED);
    queueTask(restore);
}

void TaskManager::getKeyId(QString key)
{
    QFileInfo keyFile(key);
    if(!keyFile.exists() || !Utils::tarsnapVersionMinimum("1.0.37"))
    {
        DEBUG << "Invalid key path or tarsnap version lower than 1.0.37.";
        return;
    }
    TarsnapTask *keymgmtTask = new TarsnapTask();
    QStringList  args;
    args << "--print-key-id" << key;
    keymgmtTask->setCommand(makeTarsnapCommand(CMD_TARSNAPKEYMGMT));
    keymgmtTask->setArguments(args);
    keymgmtTask->setData(key);
    connect(keymgmtTask, &TarsnapTask::finished, this,
            &TaskManager::getKeyIdFinished, QUEUED);
    queueTask(keymgmtTask);
}

void TaskManager::initializeCache()
{
    if(!Utils::tarsnapVersionMinimum("1.0.38"))
    {
        DEBUG << "Tarsnap CLI version 1.0.38 or higher required to use "
                 "--initialize-cachedir.";
        return;
    }
    TarsnapTask *initTask = new TarsnapTask();
    QStringList  args;
    args << "--initialize-cachedir";
    initTask->setCommand(makeTarsnapCommand(CMD_TARSNAP));
    initTask->setArguments(args);
    queueTask(initTask);
}

void TaskManager::runScheduledJobs()
{
    loadJobs();
    bool nothingToDo = true;
    foreach(JobPtr job, _jobMap)
    {
        if(job->optionScheduledEnabled())
        {
            backupNow(job->createBackupTask());
            nothingToDo = false;
        }
    }
    if(nothingToDo)
        qApp->quit();
}

void TaskManager::stopTasks(bool interrupt, bool running, bool queued)
{
    if(interrupt)
    {
        if(!_runningTasks.isEmpty())
            _runningTasks.first()->interrupt();
    }
    if(queued) // queued should be cleared first to avoid race
    {
        _taskQueue.clear();
    }
    if(running)
    {
        foreach(TarsnapTask *task, _runningTasks)
        {
            if(task)
                task->stop();
        }
    }
}

void TaskManager::backupTaskFinished(QVariant data, int exitCode, QString output)
{
    BackupTaskPtr backupTask = _backupTaskMap[data.toUuid()];
    backupTask->setExitCode(exitCode);
    backupTask->setOutput(output);
    if(exitCode == SUCCESS)
    {
        ArchivePtr archive(new Archive);
        archive->setName(backupTask->name());
        archive->setCommand(backupTask->command());
        archive->setTimestamp(backupTask->timestamp());
        archive->setJobRef(backupTask->jobRef());
        parseArchiveStats(output, true, archive);
        backupTask->setArchive(archive);
        backupTask->setStatus(TaskStatus::Completed);
        _archiveMap.insert(archive->name(), archive);
        foreach(JobPtr job, _jobMap)
        {
            if(job->objectKey() == archive->jobRef())
                emit job->loadArchives();
        }
        emit archiveList(_archiveMap.values());
        parseGlobalStats(output);
    }
    else
    {
        backupTask->setStatus(TaskStatus::Failed);
        parseError(output);
    }
}

void TaskManager::backupTaskStarted(QVariant data)
{
    BackupTaskPtr backupTask = _backupTaskMap[data.toString()];
    backupTask->setStatus(TaskStatus::Running);
}

void TaskManager::registerMachineFinished(QVariant data, int exitCode,
                                          QString output)
{
    Q_UNUSED(data)
    if(exitCode == SUCCESS)
        emit registerMachineStatus(TaskStatus::Completed, output);
    else
        emit registerMachineStatus(TaskStatus::Failed, output);
}

void TaskManager::getArchiveListFinished(QVariant data, int exitCode,
                                         QString output)
{
    Q_UNUSED(data)

    if(exitCode == SUCCESS)
    {
        emit message(tr("Updating archives list from remote...done"));
    }
    else
    {
        emit message(tr("Error: Failed to list archives from remote."),
                     tr("Tarsnap exited with code %1 and output:\n%2")
                         .arg(exitCode)
                         .arg(output));
        parseError(output);
        return;
    }

    QMap<QString, ArchivePtr> _newArchiveMap;
    QStringList lines = output.trimmed().split('\n');
    foreach(QString line, lines)
    {
        QRegExp archiveDetailsRX("^(.+)\\t+(\\S+\\s+\\S+)\\t+(.+)$");
        if(-1 != archiveDetailsRX.indexIn(line))
        {
            QStringList archiveDetails = archiveDetailsRX.capturedTexts();
            archiveDetails.removeFirst();
            QDateTime timestamp =
                QDateTime::fromString(archiveDetails[1], Qt::ISODate);
            ArchivePtr archive(new Archive);
            bool       update = false;
            archive->setName(archiveDetails[0]);
            archive->load();
            if(archive->objectKey().isEmpty())
            {
                update = true;
            }
            else if(archive->timestamp() != timestamp)
            {
                // There is a different archive with the same name on the remote
                archive->purge();
                archive.clear();
                archive = archive.create();
                archive->setName(archiveDetails[0]);
                update = true;
            }
            if(update)
            {
                // New archive
                archive->setTimestamp(timestamp);
                archive->setCommand(archiveDetails[2]);
                archive->save();
                // Automagically set Job ownership
                foreach(JobPtr job, _jobMap)
                {
                    if(archive->name().startsWith(job->archivePrefix()))
                    {
                        archive->setJobRef(job->objectKey());
                    }
                }
                getArchiveStats(archive);
            }
            _newArchiveMap.insert(archive->name(), archive);
            _archiveMap.remove(archive->name());
        }
    }
    // Purge archives left in old _archiveMap (not mirrored by the remote)
    foreach(ArchivePtr archive, _archiveMap)
    {
        archive->purge();
    }
    _archiveMap.clear();
    _archiveMap = _newArchiveMap;
    foreach(JobPtr job, _jobMap)
    {
        emit job->loadArchives();
    }
    emit archiveList(_archiveMap.values(), true);
    getOverallStats();
}

void TaskManager::getArchiveStatsFinished(QVariant data, int exitCode,
                                          QString output)
{
    ArchivePtr archive = _archiveMap[data.toString()];

    if(archive && (exitCode == SUCCESS))
    {
        emit message(
            tr("Fetching stats for archive <i>%1</i>...done").arg(archive->name()));
    }
    else
    {
        emit message(tr("Error: Failed to get archive stats from remote."),
                     tr("Tarsnap exited with code %1 and output:\n%2")
                         .arg(exitCode)
                         .arg(output));
        parseError(output);
        return;
    }

    if(archive)
    {
        parseArchiveStats(output, false, archive);
        parseGlobalStats(output);
    }
}

void TaskManager::getArchiveContentsFinished(QVariant data, int exitCode,
                                             QString output)
{
    ArchivePtr archive = _archiveMap[data.toString()];

    if(archive && (exitCode == SUCCESS))
    {
        emit message(tr("Fetching contents for archive <i>%1</i>...done")
                         .arg(archive->name()));
    }
    else
    {
        emit message(tr("Error: Failed to get archive contents from remote."),
                     tr("Tarsnap exited with code %1 and output:\n%2")
                         .arg(exitCode)
                         .arg(output));
        parseError(output);
        return;
    }

    if(archive)
    {
        archive->setContents(output);
        archive->save();
    }
}

void TaskManager::deleteArchivesFinished(QVariant data, int exitCode,
                                         QString output)
{
    if(exitCode != SUCCESS)
    {
        emit message(tr("Error: Failed to delete archive from remote."),
                     tr("Tarsnap exited with code %1 and output:\n%2")
                         .arg(exitCode)
                         .arg(output));
        parseError(output);
        emit archiveList(_archiveMap.values());
        return;
    }

    QList<ArchivePtr> archives = data.value<QList<ArchivePtr>>();
    if(!archives.empty())
    {
        foreach(ArchivePtr archive, archives)
        {
            _archiveMap.remove(archive->name());
            archive->purge();
        }
        emit archiveList(_archiveMap.values());
        notifyArchivesDeleted(archives, true);
    }
    // We are only interested in the output of the last archive deleted
    QStringList lines = output.split('\n', QString::SkipEmptyParts);
    QStringList lastFive;
    int         count = lines.count();
    for(int i = 0; i < std::min(5, count); ++i)
        lastFive.prepend(lines.takeLast());
    parseGlobalStats(lastFive.join('\n'));
}

void TaskManager::overallStatsFinished(QVariant data, int exitCode, QString output)
{
    Q_UNUSED(data);

    if(exitCode != SUCCESS)
    {
        emit message(tr("Error: Failed to get stats from remote."),
                     tr("Tarsnap exited with code %1 and output:\n%2")
                         .arg(exitCode)
                         .arg(output));
        parseError(output);
        return;
    }

    parseGlobalStats(output);
}

void TaskManager::fsckFinished(QVariant data, int exitCode, QString output)
{
    Q_UNUSED(data)
    if(exitCode == SUCCESS)
    {
        emit message(tr("Cache repair succeeded."), output);
    }
    else
    {
        emit message(tr("Cache repair failed. Hover mouse for details."), output);
        parseError(output);
    }
}

void TaskManager::nukeFinished(QVariant data, int exitCode, QString output)
{
    Q_UNUSED(data)
    if(exitCode == SUCCESS)
    {
        emit message(tr("All archives purged successfully."), output);
        fsck();
        loadArchives();
    }
    else
    {
        emit message(tr("Archives purging failed. Hover mouse for details."),
                     output);
        parseError(output);
        return;
    }
}

void TaskManager::restoreArchiveFinished(QVariant data, int exitCode,
                                         QString output)
{
    ArchivePtr archive = _archiveMap[data.toString()];
    if(archive && (exitCode == SUCCESS))
    {
        emit message(
            tr("Restoring archive <i>%1</i>...done").arg(archive->name()));
    }
    else
    {
        emit message(
            tr("Restoring archive <i>%1</i> failed. Hover mouse for details.")
                .arg(archive->name()),
            output);
        parseError(output);
        return;
    }
}

void TaskManager::notifyBackupTaskUpdate(QUuid uuid, const TaskStatus &status)
{
    QSettings settings;
    bool useIECPrefixes = settings.value("app/iec_prefixes", false).toBool();
    BackupTaskPtr backupTask = _backupTaskMap[uuid];
    if(!backupTask)
    {
        DEBUG << "Backup task update for invalid task";
        return;
    }
    switch(status)
    {
    case TaskStatus::Initialized:
        DEBUG << "Backup task undefined";
        break;
    case TaskStatus::Completed:
    {
        QString msg = tr("Backup <i>%1</i> completed. (%2 new data on Tarsnap)")
                          .arg(backupTask->name())
                          .arg(Utils::humanBytes(
                              backupTask->archive()->sizeUniqueCompressed(),
                              useIECPrefixes));
        emit message(msg, backupTask->archive()->archiveStats());
        emit displayNotification(msg.remove(QRegExp("<[^>]*>")));
        _backupTaskMap.remove(backupTask->uuid());
        break;
    }
    case TaskStatus::Queued:
        emit message(tr("Backup <i>%1</i> queued.").arg(backupTask->name()));
        break;
    case TaskStatus::Running:
    {
        QString msg = tr("Backup <i>%1</i> is running.").arg(backupTask->name());
        emit message(msg);
        emit displayNotification(msg.remove(QRegExp("<[^>]*>")));
        break;
    }
    case TaskStatus::Failed:
    {
        QString msg = tr("Backup <i>%1</i> failed: %2")
                          .arg(backupTask->name())
                          .arg(backupTask->output().simplified());
        emit message(msg, backupTask->output());
        emit displayNotification(msg.remove(QRegExp("<[^>]*>")));
        _backupTaskMap.remove(backupTask->uuid());
        break;
    }
    case TaskStatus::Paused:
        emit message(tr("Backup <i>%1</i> paused.").arg(backupTask->name()));
        break;
    }
}

void TaskManager::notifyArchivesDeleted(QList<ArchivePtr> archives, bool done)
{
    if(archives.count() > 1)
    {
        QString detail(archives[0]->name());
        for(int i = 1; i < archives.count(); ++i)
        {
            ArchivePtr archive = archives.at(i);
            detail.append(QString::fromLatin1(", ") + archive->name());
        }
        emit message(tr("Deleting archive <i>%1</i> and %2 more archives...%3")
                         .arg(archives.first()->name())
                         .arg(archives.count() - 1)
                         .arg(done ? "done" : ""),
                     detail);
    }
    else if(archives.count() == 1)
    {
        emit message(tr("Deleting archive <i>%1</i>...%2")
                         .arg(archives.first()->name())
                         .arg(done ? "done" : ""));
    }
}

void TaskManager::getKeyIdFinished(QVariant data, int exitCode, QString output)
{
    QString key = data.toString();
    if(exitCode == SUCCESS)
    {
        bool ok = false;
        int  id = output.toInt(&ok);
        if(ok)
            emit keyId(key, id);
        else
            DEBUG << "Invalid output from tarsnap-keymgmt for key " << key;
    }
    else
    {
        DEBUG << "Failed to get the id for key " << key;
        parseError(output);
    }
}

void TaskManager::queueTask(TarsnapTask *task, bool exclusive)
{
    if(task == nullptr)
    {
        DEBUG << "NULL argument";
        return;
    }
    if(exclusive && !_runningTasks.isEmpty())
        _taskQueue.enqueue(task);
    else
        startTask(task);
}

void TaskManager::startTask(TarsnapTask *task)
{
    if(task == nullptr)
    {
        if(!_taskQueue.isEmpty())
            task = _taskQueue.dequeue();
        else
            return;
    }
    connect(task, &TarsnapTask::terminated, this, &TaskManager::dequeueTask,
            QUEUED);
    _runningTasks.append(task);
    task->setAutoDelete(false);
    _threadPool->start(task);
    emit idle(false);
}

void TaskManager::dequeueTask()
{
    TarsnapTask *task = qobject_cast<TarsnapTask *>(sender());
    if(task == nullptr)
        return;
    _runningTasks.removeOne(task);
    delete task;
    if(_runningTasks.isEmpty())
    {
        if(_taskQueue.isEmpty())
        {
            // Give a chance for notifications to go through and quit
            if(_headless)
            {
#if(QT_VERSION >= QT_VERSION_CHECK(5, 4, 0))
                QTimer::singleShot(500, qApp, &QCoreApplication::quit);
#else
                QTimer::singleShot(500, qApp, SLOT(quit()));
#endif
            }
            else
            {
                emit idle(true);
            }
        }
        else
        {
            startTask(nullptr); // start another queued task
        }
    }
}

void TaskManager::parseError(QString tarsnapOutput)
{
    if(tarsnapOutput.contains("Error reading cache directory") ||
       tarsnapOutput.contains("Sequence number mismatch: Run --fsck") ||
       tarsnapOutput.contains(
           "Directory is not consistent with archive: Run --fsck"))
    {
        emit error(TarsnapError::CacheError);
    }
    else if(tarsnapOutput.contains("Error fscking archives"))
    {
        emit error(TarsnapError::FsckError);
    }
}

void TaskManager::parseGlobalStats(QString tarsnapOutput)
{
    quint64 sizeTotal;
    quint64 sizeCompressed;
    quint64 sizeUniqueTotal;
    quint64 sizeUniqueCompressed;

    QStringList lines =
        tarsnapOutput.trimmed().split('\n', QString::SkipEmptyParts);
    if(lines.count() < 3)
    {
        DEBUG << "Malformed output from tarsnap CLI:\n" << tarsnapOutput;
        return;
    }
    QRegExp sizeRX("^All archives\\s+(\\d+)\\s+(\\d+)$");
    QRegExp uniqueSizeRX("^\\s+\\(unique data\\)\\s+(\\d+)\\s+(\\d+)$");
    if(-1 != sizeRX.indexIn(lines[1]))
    {
        QStringList captured = sizeRX.capturedTexts();
        captured.removeFirst();
        sizeTotal      = captured[0].toULongLong();
        sizeCompressed = captured[1].toULongLong();
    }
    else
    {
        DEBUG << "Malformed output from tarsnap CLI:\n" << tarsnapOutput;
        return;
    }
    if(-1 != uniqueSizeRX.indexIn(lines[2]))
    {
        QStringList captured = uniqueSizeRX.capturedTexts();
        captured.removeFirst();
        sizeUniqueTotal      = captured[0].toULongLong();
        sizeUniqueCompressed = captured[1].toULongLong();
    }
    else
    {
        DEBUG << "Malformed output from tarsnap CLI:\n" << tarsnapOutput;
        return;
    }
    emit overallStats(sizeTotal, sizeCompressed, sizeUniqueTotal,
                      sizeUniqueCompressed, _archiveMap.count());
}

void TaskManager::parseArchiveStats(QString tarsnapOutput,
                                    bool newArchiveOutput, ArchivePtr archive)
{
    QStringList lines =
        tarsnapOutput.trimmed().split('\n', QString::SkipEmptyParts);
    if(lines.count() != 5)
    {
        DEBUG << "Malformed output from tarsnap CLI:\n" << tarsnapOutput;
        return;
    }
    QString sizeLine       = lines[3];
    QString uniqueSizeLine = lines[4];
    QRegExp sizeRX;
    QRegExp uniqueSizeRX;
    if(newArchiveOutput)
    {
        sizeRX.setPattern("^This archive\\s+(\\d+)\\s+(\\d+)$");
        uniqueSizeRX.setPattern("^New data\\s+(\\d+)\\s+(\\d+)$");
    }
    else
    {
        sizeRX.setPattern(
            QString("^%1\\s+(\\d+)\\s+(\\d+)$").arg(archive->name()));
        uniqueSizeRX.setPattern("^\\s+\\(unique data\\)\\s+(\\d+)\\s+(\\d+)$");
    }
    if(-1 != sizeRX.indexIn(sizeLine))
    {
        QStringList captured = sizeRX.capturedTexts();
        captured.removeFirst();
        archive->setSizeTotal(captured[0].toULongLong());
        archive->setSizeCompressed(captured[1].toULongLong());
    }
    else
    {
        DEBUG << "Malformed output from tarsnap CLI:\n" << tarsnapOutput;
        return;
    }
    if(-1 != uniqueSizeRX.indexIn(uniqueSizeLine))
    {
        QStringList captured = uniqueSizeRX.capturedTexts();
        captured.removeFirst();
        archive->setSizeUniqueTotal(captured[0].toULongLong());
        archive->setSizeUniqueCompressed(captured[1].toULongLong());
    }
    else
    {
        DEBUG << "Malformed output from tarsnap CLI:\n" << tarsnapOutput;
        return;
    }
    archive->save();
}

QString TaskManager::makeTarsnapCommand(QString cmd)
{
    if(_tarsnapDir.isEmpty())
        return cmd;
    else
        return _tarsnapDir + QDir::separator() + cmd;
}

void TaskManager::loadJobs()
{
    _jobMap.clear();
    PersistentStore &store = PersistentStore::instance();
    if(!store.initialized())
    {
        DEBUG << "PersistentStore was not initialized properly.";
        return;
    }
    QSqlQuery query = store.createQuery();
    if(!query.prepare(QLatin1String("select name from jobs")))
    {
        DEBUG << query.lastError().text();
        return;
    }
    if(store.runQuery(query) && query.next())
    {
        do
        {
            JobPtr job(new Job);
            job->setName(query.value(query.record().indexOf("name")).toString());
            connect(job.data(), &Job::loadArchives, this,
                    &TaskManager::loadJobArchives, QUEUED);
            job->load();
            _jobMap[job->name()] = job;
        } while(query.next());
    }
    emit jobsList(_jobMap);
}

void TaskManager::deleteJob(JobPtr job, bool purgeArchives)
{
    if(job)
    {
        if(purgeArchives)
            deleteArchives(job->archives());
        job->purge();
        _jobMap.remove(job->name());
    }
}

void TaskManager::loadJobArchives()
{
    Job *job = qobject_cast<Job *>(sender());
    QList<ArchivePtr> archives;
    foreach(ArchivePtr archive, _archiveMap)
    {
        if(archive->jobRef() == job->objectKey())
            archives << archive;
    }
    job->setArchives(archives);
}

void TaskManager::getTaskInfo()
{
    bool backupTaskRunning = false;
    if(!_runningTasks.isEmpty() && !_backupTaskMap.isEmpty())
    {
        foreach(TarsnapTask *task, _runningTasks)
        {
            if(task && _backupTaskMap.contains(task->data().toUuid()))
            {
                backupTaskRunning = true;
                break;
            }
        }
    }
    emit taskInfo(backupTaskRunning, _runningTasks.count(), _taskQueue.count());
}

void TaskManager::addJob(JobPtr job)
{
    _jobMap[job->name()] = job;
    connect(job.data(), &Job::loadArchives, this, &TaskManager::loadJobArchives,
            QUEUED);
}

void TaskManager::getTarsnapVersionFinished(QVariant data, int exitCode,
                                            QString output)
{
    Q_UNUSED(data)

    if(exitCode != SUCCESS)
    {
        emit message(tr("Error: Failed to get Tarsnap version."),
                     tr("Tarsnap exited with code %1 and output:\n%2")
                         .arg(exitCode)
                         .arg(output));
        return;
    }

    QRegExp versionRx("^tarsnap (\\S+)\\s$");
    if(-1 != versionRx.indexIn(output))
        emit tarsnapVersion(versionRx.cap(1));
}
