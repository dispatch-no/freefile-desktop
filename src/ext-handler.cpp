#include <winsock2.h>
#include <windows.h>
#include <io.h>
#include <shlwapi.h>
#include <fcntl.h>
#include <ctype.h>
#include <userenv.h>

#include <string>
#include <QMutexLocker>
#include <QScopedPointer>
#include <QList>
#include <QVector>
#include <QDir>
#include <QTimer>
#include <QDateTime>

#include "filebrowser/file-browser-requests.h"
#include "filebrowser/sharedlink-dialog.h"
#include "rpc/rpc-client.h"
#include "seafile-applet.h"
#include "account-mgr.h"
#include "ext-handler.h"

namespace {

const char *kSeafExtPipeName = "\\\\.\\pipe\\seafile_ext_pipe";
const int kPipeBufSize = 1024;
const char *kRepoRelayAddrProperty = "relay-address";
const int kRefreshShellInterval = 3000;

const quint64 kShellIconForceRefreshMSecs = 5000;
const quint64 kReposInfoCacheMSecs = 2000;

bool
extPipeReadN (HANDLE pipe, void *buf, size_t len)
{
    DWORD bytes_read;
    bool success = ReadFile(
        pipe,                  // handle to pipe
        buf,                   // buffer to receive data
        (DWORD)len,            // size of buffer
        &bytes_read,           // number of bytes read
        NULL);                 // not overlapped I/O

    if (!success || bytes_read != (DWORD)len) {
        DWORD error = GetLastError();
        if (error == ERROR_BROKEN_PIPE) {
            qDebug("[ext] connection closed by extension\n");
        } else {
            qWarning("[ext] Failed to read command from extension(), "
                     "error code %lu\n", error);
        }
        return false;
    }

    return true;
}

bool
extPipeWriteN(HANDLE pipe, void *buf, size_t len)
{
    DWORD bytes_written;
    bool success = WriteFile(
        pipe,                  // handle to pipe
        buf,                   // buffer to receive data
        (DWORD)len,            // size of buffer
        &bytes_written,        // number of bytes written
        NULL);                 // not overlapped I/O

    if (!success || bytes_written != (DWORD)len) {
        DWORD error = GetLastError();
        if (error == ERROR_BROKEN_PIPE) {
            qDebug("[ext] connection closed by extension\n");
        } else {
            qWarning("[ext] Failed to read command from extension(), "
                     "error code %lu\n", error);
        }
        return false;
    }

    FlushFileBuffers(pipe);
    return true;
}

/**
 * Replace "\" with "/", and remove the trailing slash
 */
QString normalizedPath(const QString& path)
{
    QString p = QDir::fromNativeSeparators(path);
    if (p.endsWith("/")) {
        p = p.left(p.size() - 1);
    }
    return p;
}

std::string formatErrorMessage()
{
    DWORD error_code = ::GetLastError();
    if (error_code == 0) {
        return "no error";
    }
    char buf[256] = {0};
    ::FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM,
                    NULL,
                    error_code,
                    MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                    buf,
                    sizeof(buf) - 1,
                    NULL);
    return buf;
}

QString repoStatus(const LocalRepo& repo)
{
    QString status = "normal";
    if (!repo.auto_sync) {
        status = "paused";
    } else if (repo.sync_state == LocalRepo::SYNC_STATE_ING) {
        status = "syncing";
    } else if (repo.sync_state == LocalRepo::SYNC_STATE_ERROR) {
        status = "error";
    }

    // qDebug("repo %s (%s, %s): %s", repo.name.toUtf8().data(),
    //        repo.sync_state_str.toUtf8().data(),
    //        repo.sync_error_str.toUtf8().data(),
    //        status.toUtf8().data());

    return status;
}

} // namespace


SINGLETON_IMPL(SeafileExtensionHandler)

SeafileExtensionHandler::SeafileExtensionHandler()
{
    listener_thread_ = new ExtConnectionListenerThread;

    refresh_local_timer_ = new QTimer(this);
    connect(refresh_local_timer_, SIGNAL(timeout()),
            this, SLOT(refreshRepoShellIcon()));

    connect(listener_thread_, SIGNAL(generateShareLink(const QString&, const QString&, bool)),
            this, SLOT(generateShareLink(const QString&, const QString&, bool)));
}

void SeafileExtensionHandler::start()
{
    listener_thread_->start();
    refresh_local_timer_->start(kRefreshShellInterval);
    ReposInfoCache::instance()->start();
}

void SeafileExtensionHandler::generateShareLink(const QString& repo_id,
                                                const QString& path_in_repo,
                                                bool is_file)
{
    // qDebug("path_in_repo: %s", path_in_repo.toUtf8().data());
    const Account account = findAccountByRepo(repo_id);
    const Account account = seafApplet->accountManager()->getAccountByRepo(repo_id);
    if (!account.isValid()) {
        return;
    }

    GetSharedLinkRequest *req = new GetSharedLinkRequest(
        account, repo_id, path_in_repo, is_file);

    connect(req, SIGNAL(success(const QString&)),
            this, SLOT(onShareLinkGenerated(const QString&)));

    req->send();
}

void SeafileExtensionHandler::onShareLinkGenerated(const QString& link)
{
    SharedLinkDialog *dialog = new SharedLinkDialog(link, NULL);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

// Trigger the shell to update repo worktree folder icons periodically
void SeafileExtensionHandler::refreshRepoShellIcon()
{
    QList<LocalRepo> repos = ReposInfoCache::instance()->getReposInfo();
    quint64 now = QDateTime::currentMSecsSinceEpoch();
    foreach (const LocalRepo& repo, repos) {
        bool status_changed = true;
        quint64 last_ts = last_change_ts_.value(repo.id, 0);

        // Force shell to refresh the repo icon every copule of seconds.
        if (now - last_ts < kShellIconForceRefreshMSecs) {
            foreach (const LocalRepo& last, last_info_) {
                if (last.id == repo.id) {
                    status_changed = last.sync_state != repo.sync_state;
                    break;
                }
            }
        }

        if (status_changed) {
            QString normalized_path = QDir::toNativeSeparators(repo.worktree);
            SHChangeNotify(SHCNE_ATTRIBUTES, SHCNF_PATH, normalized_path.toUtf8().data(), NULL);
            // qDebug("updated shell attributes for %s", normalized_path.toUtf8().data());
            last_change_ts_[repo.id] = now;
        }
    }
    last_info_ = repos;
}


void ExtConnectionListenerThread::run()
{
    while (1) {
        HANDLE pipe = INVALID_HANDLE_VALUE;
        bool connected = false;

        pipe = CreateNamedPipe(
            kSeafExtPipeName,         // pipe name
            PIPE_ACCESS_DUPLEX,       // read/write access
            PIPE_TYPE_MESSAGE |       // message type pipe
            PIPE_READMODE_MESSAGE |   // message-read mode
            PIPE_WAIT,                // blocking mode
            PIPE_UNLIMITED_INSTANCES, // max. instances
            kPipeBufSize,             // output buffer size
            kPipeBufSize,             // input buffer size
            0,                        // client time-out
            NULL);                    // default security attribute

        if (pipe == INVALID_HANDLE_VALUE) {
            qWarning ("Failed to create named pipe, GLE=%lu\n",
                      GetLastError());
            return;
        }

        /* listening on this pipe */
        connected = ConnectNamedPipe(pipe, NULL) ?
            true : (GetLastError() == ERROR_PIPE_CONNECTED);

        if (!connected) {
            qWarning ("Failed on ConnectNamedPipe(), GLE=%lu\n",
                      GetLastError());
            CloseHandle(pipe);
            return;
        }

        qDebug ("[ext pipe] Accepted an extension pipe client\n");
        servePipeInNewThread(pipe);
    }
}

void ExtConnectionListenerThread::servePipeInNewThread(HANDLE pipe)
{
    ExtCommandsHandler *t = new ExtCommandsHandler(pipe);

    connect(t, SIGNAL(generateShareLink(const QString&, const QString&, bool)),
            this, SIGNAL(generateShareLink(const QString&, const QString&, bool)));
    t->start();
}

ExtCommandsHandler::ExtCommandsHandler(HANDLE pipe)
{
    pipe_ = pipe;
}

void ExtCommandsHandler::run()
{
    while (1) {
        QStringList args;
        if (!readRequest(&args)) {
            qWarning ("failed to read request from shell extension: %s",
                      formatErrorMessage().c_str());
            break;
        }

        QString cmd = args.takeAt(0);
        QString resp;
        if (cmd == "list-repos") {
            resp = handleListRepos(args);
        } else if (cmd == "get-share-link") {
            handleGenShareLink(args);
        } else {
            qWarning ("[ext] unknown request command: %s", cmd.toUtf8().data());
        }

        if (!sendResponse(resp)) {
            qWarning ("failed to write response to shell extension: %s",
                      formatErrorMessage().c_str());
            break;
        }
    }

    qWarning ("An extension client is disconnected: GLE=%lu\n",
              GetLastError());
    DisconnectNamedPipe(pipe_);
    CloseHandle(pipe_);
}

bool ExtCommandsHandler::readRequest(QStringList *args)
{
    uint32_t len;
    if (!extPipeReadN(pipe_, &len, sizeof(len)) || len == 0)
        return false;

    QScopedPointer<char> buf(new char[len + 1]);
    buf.data()[len] = 0;
    if (!extPipeReadN(pipe_, buf.data(), len))
        return false;

    QStringList list = QString::fromUtf8(buf.data()).split('\t', QString::SkipEmptyParts);
    if (list.empty()) {
        qWarning("[ext] got an empty request");
        return false;
    }
    *args = list;
    return true;
}

bool ExtCommandsHandler::sendResponse(const QString& resp)
{
    QByteArray raw_resp = resp.toUtf8();
    uint32_t len = raw_resp.length();

    if (!extPipeWriteN(pipe_, &len, sizeof(len))) {
        return false;
    }
    if (len > 0) {
        if (!extPipeWriteN(pipe_, raw_resp.data(), len)) {
            return false;
        }
    }
    return true;
}

QList<LocalRepo> ExtCommandsHandler::listLocalRepos(quint64 ts)
{
    return ReposInfoCache::instance()->getReposInfo(ts);
}

void ExtCommandsHandler::handleGenShareLink(const QStringList& args)
{
    if (args.size() != 1) {
        return;
    }
    QString path = normalizedPath(args[0]);
    foreach (const LocalRepo& repo, listLocalRepos()) {
        QString wt = normalizedPath(repo.worktree);
        // qDebug("path: %s, repo: %s", path.toUtf8().data(), wt.toUtf8().data());
        if (path.length() > wt.length() && path.startsWith(wt) and path.at(wt.length()) == '/') {
            QString path_in_repo = path.mid(wt.size());
            bool is_file = QFileInfo(path).isFile();
            emit generateShareLink(repo.id, path_in_repo, is_file);
            break;
        }
    }
}

QString ExtCommandsHandler::handleListRepos(const QStringList& args)
{
    if (args.size() != 1) {
        return "";
    }
    bool ok;
    quint64 ts = args[0].toULongLong(&ok);
    if (!ok) {
        return "";
    }

    QStringList infos;
    foreach (const LocalRepo& repo, listLocalRepos(ts)) {
        QStringList fields;
        fields << repo.id << repo.name << normalizedPath(repo.worktree) << repoStatus(repo);
        infos << fields.join("\t");
    }

    return infos.join("\n");
}

SINGLETON_IMPL(ReposInfoCache)

ReposInfoCache::ReposInfoCache(QObject * parent)
    : QObject(parent)
{
    cache_ts_ = 0;
    rpc_ = new SeafileRpcClient();
}

void ReposInfoCache::start()
{
    rpc_->connectDaemon();
}

QList<LocalRepo> ReposInfoCache::getReposInfo(quint64 ts)
{
    QMutexLocker lock(&rpc_mutex_);

    quint64 now = QDateTime::currentMSecsSinceEpoch();

    if (cache_ts_ != 0 && cache_ts_ > ts && now - cache_ts_ < kReposInfoCacheMSecs) {
        // qDebug("ReposInfoCache: return cached info");
        return cached_info_;
    }
    // qDebug("ReposInfoCache: fetch from daemon");

    std::vector<LocalRepo> repos;
    rpc_->listLocalRepos(&repos);

    for (size_t i = 0; i < repos.size(); i++) {
        LocalRepo& repo = repos[i];
        rpc_->getSyncStatus(repo);
    }

    cached_info_ = QVector<LocalRepo>::fromStdVector(repos).toList();
    cache_ts_ = QDateTime::currentMSecsSinceEpoch();

    return cached_info_;
}
