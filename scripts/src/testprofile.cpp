#include "testprofile.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>

namespace TestProfile {

namespace {

const QString kRegistryPrefix = QStringLiteral("HKEY_CURRENT_USER\\Software\\TaskNestTestProfiles\\");

Profile refuse(Profile p, const QString &why)
{
    p.active = false;
    p.error = why;
    return p;
}

bool samePath(const QString &a, const QString &b)
{
    return QString::compare(a, b, Qt::CaseInsensitive) == 0;
}

// `inner` is `outer` or below it.
bool isInside(const QString &inner, const QString &outer)
{
    if (samePath(inner, outer))
        return true;
    const QString prefix = outer.endsWith(QLatin1Char('/')) ? outer : outer + QLatin1Char('/');
    return inner.startsWith(prefix, Qt::CaseInsensitive);
}

QString clean(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

} // namespace

QStringList protectedDirectories()
{
    QStringList dirs;
    const QString exeDir = QCoreApplication::applicationDirPath();
    if (!exeDir.isEmpty()) {
        dirs << clean(exeDir);
        dirs << clean(QDir(exeDir).filePath(QStringLiteral("data")));
    }
    for (const char *variable : {"APPDATA", "LOCALAPPDATA"}) {
        const QString base = qEnvironmentVariable(variable);
        if (!base.isEmpty())
            dirs << clean(QDir(base).filePath(QStringLiteral("TaskNest")));
    }
    return dirs;
}

bool isProfileRegistryBase(const QString &base)
{
    static const QRegularExpression re(
        QStringLiteral("^HKEY_CURRENT_USER\\\\Software\\\\TaskNestTestProfiles\\\\[0-9a-f]{16}$"));
    return re.match(base).hasMatch();
}

Profile fromRoot(const QString &root, bool create, const QStringList &protectedDirs)
{
    Profile p;
    p.requested = true;

    if (root.trimmed().isEmpty())
        return refuse(p, QStringLiteral("The test profile folder is empty."));
    for (const QChar c : root) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f)
            return refuse(p, QStringLiteral("The test profile folder contains control characters."));
    }
    const QString normalized = QDir::fromNativeSeparators(root);
    if (normalized.startsWith(QStringLiteral("//")) || normalized.startsWith(QStringLiteral("\\\\")))
        return refuse(p, QStringLiteral("The test profile folder must be a local drive path."));
    if (!QDir::isAbsolutePath(normalized) || normalized.size() < 3 || normalized.at(1) != QLatin1Char(':'))
        return refuse(p, QStringLiteral("The test profile folder must be an absolute path such as C:/scratch/profile."));
    for (const QString &segment : normalized.split(QLatin1Char('/'))) {
        if (segment == QLatin1String("..") || segment == QLatin1String("."))
            return refuse(p, QStringLiteral("The test profile folder must not contain . or .. segments."));
    }
    const QString cleaned = QDir::cleanPath(normalized);
    if (cleaned.size() <= 3) // a drive root
        return refuse(p, QStringLiteral("A drive root cannot be a test profile folder."));

    for (const QString &protectedDir : protectedDirs) {
        if (protectedDir.isEmpty())
            continue;
        if (isInside(cleaned, protectedDir) || isInside(protectedDir, cleaned))
            return refuse(p, QStringLiteral("The test profile folder overlaps a real TaskNest location."));
    }

    p.root = cleaned;
    const QFileInfo info(cleaned);
    const QString marker = QDir(cleaned).filePath(QLatin1String(kMarkerFile));
    if (info.exists()) {
        if (!info.isDir())
            return refuse(p, QStringLiteral("The test profile path is not a folder."));
        if (!QFileInfo::exists(marker)) {
            if (!QDir(cleaned).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System))
                return refuse(p, QStringLiteral("The test profile folder is not empty and is not a TaskNest test profile."));
            if (!create)
                return refuse(p, QStringLiteral("The test profile folder has not been initialized by TaskNest."));
        }
    } else if (!create) {
        return refuse(p, QStringLiteral("The test profile folder does not exist."));
    }

    if (create) {
        if (!QDir().mkpath(cleaned))
            return refuse(p, QStringLiteral("The test profile folder could not be created."));
        if (!QFileInfo::exists(marker)) {
            QSaveFile file(marker);
            const QByteArray body = "{\"tasknest_test_profile\":1}\n";
            if (!file.open(QIODevice::WriteOnly) || file.write(body) != body.size() || !file.commit())
                return refuse(p, QStringLiteral("The test profile marker could not be written."));
        }
    }

    p.dataDirectory = QDir(cleaned).filePath(QStringLiteral("data"));
    // The single-instance lock lives beside the database and is taken before
    // the database layer creates anything, so the folder must already exist
    // (found by launching two instances of one profile: the first one's lock
    // could not be created and the second was not turned away).
    if (create && !QDir().mkpath(p.dataDirectory))
        return refuse(p, QStringLiteral("The test profile data folder could not be created."));
    p.databasePath = QDir(p.dataDirectory).filePath(QStringLiteral("tasknest.db"));
    p.discoveryDirectory = QDir(cleaned).filePath(QStringLiteral("integration"));
    p.manifestDirectory = QDir(cleaned).filePath(QStringLiteral("native-host"));
    const QByteArray hash = QCryptographicHash::hash(cleaned.toLower().toUtf8(), QCryptographicHash::Sha256);
    p.registryBase = kRegistryPrefix + QString::fromLatin1(hash.toHex().left(16));
    p.active = true;
    return p;
}

Profile fromArguments(const QStringList &arguments, bool create, const QStringList &protectedDirs)
{
    Profile none;
    int found = -1;
    for (int i = 1; i < arguments.size(); ++i) {
        if (arguments.at(i) != QLatin1String(kOption))
            continue;
        if (found >= 0) {
            none.requested = true;
            none.error = QStringLiteral("--test-profile was given more than once.");
            return none;
        }
        found = i;
    }
    if (found < 0)
        return none;
    if (found + 1 >= arguments.size()) {
        none.requested = true;
        none.error = QStringLiteral("--test-profile needs a folder.");
        return none;
    }
    return fromRoot(arguments.at(found + 1), create, protectedDirs);
}

} // namespace TestProfile
