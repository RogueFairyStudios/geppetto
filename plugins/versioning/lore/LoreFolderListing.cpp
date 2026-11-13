/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "LoreFolderListing.h"

#include <QDir>
#include <QTemporaryDir>

#include <algorithm>

#include <klocalizedstring.h>

#include <KisVersioningJob.h>

#include "LoreOperation.h"

using namespace KisVersioning;

namespace {

QByteArray fromHex(const QString &hex)
{
    return QByteArray::fromHex(hex.toLatin1());
}

bool isZeroHash(const QString &hex)
{
    return std::all_of(hex.begin(), hex.end(), [](QChar c) { return c == QLatin1Char('0'); });
}

/// Folders of a project, read without downloading it: a clone with no files, then the library's
/// revision tree. Every handle opened is closed before the job ends.
class FolderListing : public LoreOperation
{
public:
    FolderListing(KisVersioningJob *job, const LoreSettings &settings, const ProjectInfo &project)
        : LoreOperation(job, settings, QString())
        , m_project(project)
        , m_url(loreRepositoryUrl(project).toUtf8())
    {
    }

    void start()
    {
        if (!m_temporary.isValid()) {
            fail(KisVersioningJob::Failed, failText());
            return;
        }
        LoreGlobals g = globals();
        g.repositoryPath = lorePath(m_temporary.filePath(QStringLiteral("tree")));
        g.workingDirectory = lorePath(m_temporary.path());
        const QByteArray url = m_url;
        run(g,
            [url](const lore_global_args_t *globals, lore_event_callback_config_t callback) {
                lore_repository_clone_args_t args;
                std::memset(&args, 0, sizeof args);
                args.repository_url = toLore(url);
                args.bare = 1;
                lore_repository_clone_async(globals, &args, callback);
            },
            [this](const LoreEvent &event) {
                if (event.tag == LORE_EVENT_REPOSITORY_CLONE_BEGIN) {
                    m_repository = fromHex(event.fields.value(QStringLiteral("repository")).toString());
                } else if (event.tag == LORE_EVENT_REPOSITORY_CLONE_END) {
                    m_revision = event.fields.value(QStringLiteral("revision")).toString();
                }
            },
            [this](int status, const QString &rawText) {
                if (status != 0) {
                    end(status, rawText);
                    return;
                }
                if (m_revision.isEmpty() || isZeroHash(m_revision)) {
                    end(0, QString()); // nothing was ever sent to the project
                    return;
                }
                openStorage();
            });
    }

private:
    QString failText() const
    {
        return i18n("Could not list the folders of %1.", m_project.name);
    }

    LoreGlobals treeGlobals() const
    {
        LoreGlobals g = globals();
        g.repositoryPath.clear();
        g.workingDirectory.clear();
        return g;
    }

    /// Checked between two calls: what is open is closed, then the job ends as cancelled.
    bool cancelling()
    {
        if (job() && job()->cancelRequested()) {
            m_cancelled = true;
            end(0, QString());
            return true;
        }
        return false;
    }

    void openStorage()
    {
        if (cancelling()) {
            return;
        }
        const QByteArray path = lorePath(m_temporary.filePath(QStringLiteral("tree")));
        const QByteArray url = m_url;
        run(treeGlobals(),
            [path, url](const lore_global_args_t *globals, lore_event_callback_config_t callback) {
                lore_storage_open_args_t args;
                std::memset(&args, 0, sizeof args);
                args.repository_path = toLore(path);
                args.has_remote_config = 1;
                args.remote_config.remote_url = toLore(url);
                lore_storage_open_async(globals, &args, callback);
            },
            [this](const LoreEvent &event) {
                if (event.tag == LORE_EVENT_STORAGE_OPENED) {
                    m_store = event.fields.value(QStringLiteral("handle")).toULongLong();
                }
            },
            [this](int status, const QString &rawText) {
                if (status != 0 || m_store == 0) {
                    end(status ? status : -1, rawText);
                    return;
                }
                loadTree();
            });
    }

    void loadTree()
    {
        if (cancelling()) {
            return;
        }
        const quint64 store = m_store;
        const QByteArray repository = m_repository;
        const QByteArray revision = fromHex(m_revision);
        run(treeGlobals(),
            [store, repository, revision](const lore_global_args_t *globals, lore_event_callback_config_t callback) {
                lore_revision_tree_load_args_t args;
                std::memset(&args, 0, sizeof args);
                args.store.handle_id = store;
                std::memcpy(args.repository.data, repository.constData(),
                            std::min<size_t>(sizeof args.repository.data, size_t(repository.size())));
                std::memcpy(args.revision_hash.data, revision.constData(),
                            std::min<size_t>(sizeof args.revision_hash.data, size_t(revision.size())));
                lore_revision_tree_load_async(globals, &args, callback);
            },
            [this](const LoreEvent &event) {
                if (event.tag == LORE_EVENT_REVISION_TREE_LOADED) {
                    m_tree = event.fields.value(QStringLiteral("handle")).toULongLong();
                }
            },
            [this](int status, const QString &rawText) {
                if (status != 0 || m_tree == 0) {
                    end(status ? status : -1, rawText);
                    return;
                }
                resolveRoot();
            });
    }

    void resolveRoot()
    {
        if (cancelling()) {
            return;
        }
        const quint64 tree = m_tree;
        run(treeGlobals(),
            [tree](const lore_global_args_t *globals, lore_event_callback_config_t callback) {
                lore_revision_tree_resolve_path_args_t args;
                std::memset(&args, 0, sizeof args);
                args.id = 1;
                args.handle.handle_id = tree;
                lore_revision_tree_resolve_path_async(globals, &args, callback); // empty path: the root
            },
            [this](const LoreEvent &event) {
                if (event.tag == LORE_EVENT_REVISION_TREE_RESOLVE_PATH_COMPLETE
                    && event.fields.value(QStringLiteral("errorCode")).toInt() == 0) {
                    m_root = quint32(event.fields.value(QStringLiteral("node")).toULongLong());
                    m_rootFound = true;
                }
            },
            [this](int status, const QString &rawText) {
                if (status != 0 || !m_rootFound) {
                    end(status ? status : -1, rawText);
                    return;
                }
                listRoot();
            });
    }

    void listRoot()
    {
        if (cancelling()) {
            return;
        }
        const quint64 tree = m_tree;
        const quint32 root = m_root;
        run(treeGlobals(),
            [tree, root](const lore_global_args_t *globals, lore_event_callback_config_t callback) {
                lore_revision_tree_list_children_args_t args;
                std::memset(&args, 0, sizeof args);
                args.id = 2;
                args.handle.handle_id = tree;
                args.parent_node_id = root;
                lore_revision_tree_list_children_async(globals, &args, callback);
            },
            [this](const LoreEvent &event) {
                if (event.tag == LORE_EVENT_REVISION_TREE_CHILD && event.fields.value(QStringLiteral("errorCode")).toInt() == 0
                    && event.fields.value(QStringLiteral("isDirectory")).toBool()) {
                    m_folders << event.fields.value(QStringLiteral("name")).toString();
                }
            },
            [this](int status, const QString &rawText) { end(status, rawText); });
    }

    /// Closes what is open, then ends the job with the outcome of the listing.
    void end(int status, const QString &rawText)
    {
        if (m_tree != 0) {
            const quint64 tree = m_tree;
            m_tree = 0;
            run(treeGlobals(),
                [tree](const lore_global_args_t *globals, lore_event_callback_config_t callback) {
                    lore_revision_tree_close_args_t args;
                    std::memset(&args, 0, sizeof args);
                    args.id = 3;
                    args.handle.handle_id = tree;
                    lore_revision_tree_close_async(globals, &args, callback);
                },
                nullptr, [this, status, rawText](int, const QString &) { end(status, rawText); });
            return;
        }
        if (m_store != 0) {
            const quint64 store = m_store;
            m_store = 0;
            run(treeGlobals(),
                [store](const lore_global_args_t *globals, lore_event_callback_config_t callback) {
                    lore_storage_close_args_t args;
                    std::memset(&args, 0, sizeof args);
                    args.handle.handle_id = store;
                    lore_storage_close_async(globals, &args, callback);
                },
                nullptr, [this, status, rawText](int, const QString &) { end(status, rawText); });
            return;
        }
        if (!m_cloneRemoved && isLoreProject(m_temporary.filePath(QStringLiteral("tree")))) {
            m_cloneRemoved = true;
            // The clone is registered with the library like any download: it is taken out of
            // its registry and off the disk, so that listings leave nothing behind.
            LoreGlobals g = globals();
            g.repositoryPath = lorePath(m_temporary.filePath(QStringLiteral("tree")));
            g.workingDirectory = lorePath(m_temporary.path());
            run(g,
                [](const lore_global_args_t *globals, lore_event_callback_config_t callback) {
                    lore_repository_instance_delete_args_t args;
                    std::memset(&args, 0, sizeof args);
                    lore_repository_instance_delete_async(globals, &args, callback);
                },
                nullptr,
                [this, status, rawText](int code, const QString &deleteText) {
                    if (code != 0) {
                        qWarning("Lore: removing the folder listing's clone failed (status %d): %s", code,
                                 qUtf8Printable(deleteText));
                    }
                    QDir(m_temporary.filePath(QStringLiteral("tree"))).removeRecursively();
                    end(status, rawText);
                });
            return;
        }
        m_temporary.remove();
        if (m_cancelled) {
            fail(KisVersioningJob::Cancelled, LoreText::cancelled());
            return;
        }
        if (status != 0) {
            failFromLore("listing folders", status, rawText, failText());
            return;
        }
        m_folders.sort(Qt::CaseInsensitive);
        finish(QVariant::fromValue(m_folders));
    }

    ProjectInfo m_project;
    QByteArray m_url;
    QTemporaryDir m_temporary;
    QByteArray m_repository;
    QString m_revision;
    quint64 m_store {0};
    quint64 m_tree {0};
    quint32 m_root {0};
    bool m_rootFound {false};
    bool m_cloneRemoved {false};
    bool m_cancelled {false};
    QStringList m_folders;
};

} // namespace

KisVersioningJob *startLoreFolderListing(const LoreSettings &settings, const ProjectInfo &project)
{
    auto *job = new KisVersioningJob();
    auto *operation = new FolderListing(job, settings, project);
    operation->start();
    return job;
}
