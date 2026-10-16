#pragma once

#include <QString>
#include <QStringList>

// Explicit, opt-in isolation for acceptance testing (see INTEGRATION.md,
// "Scratch test profile"). A test profile is ONE folder that holds everything a
// scratch TaskNest instance would otherwise put in the user's real locations:
//
//   <root>/tasknest-test-profile.json   marker written by TaskNest itself
//   <root>/data/tasknest.db             task database and settings
//   <root>/integration/                 integration discovery file
//   <root>/native-host/                 host manifest and launcher
//   HKCU\Software\TaskNestTestProfiles\<hash of root>   browser host keys
//
// It is selected only by the command-line option "--test-profile <folder>" -
// never by an environment variable, a settings value or a message from a
// browser. Without the option nothing in this file changes any behavior.
//
// Honest limit: browsers only read the real Native Messaging keys, so a
// profile's registry subtree isolates TaskNest's own bookkeeping and tests; it
// does not make a browser discover the host. See the documentation.
namespace TestProfile {

constexpr const char *kOption = "--test-profile";
constexpr const char *kMarkerFile = "tasknest-test-profile.json";

struct Profile
{
    bool requested = false;       // the option was present
    bool active = false;          // requested and valid
    QString error;                // why a requested profile is unusable
    QString root;                 // absolute, cleaned
    QString dataDirectory;
    QString databasePath;
    QString discoveryDirectory;
    QString manifestDirectory;
    QString registryBase;         // HKEY_CURRENT_USER\Software\TaskNestTestProfiles\<hash>
};

// Folders that belong to the real installation and must never be used as (or
// contain, or sit inside) a profile: %APPDATA%\TaskNest, %LOCALAPPDATA%\TaskNest
// and <exe folder>\data. Also the exe folder itself.
QStringList protectedDirectories();

// Validates `root` and derives all locations. With `create`, a missing folder
// (and its marker) is created - only TaskNest.exe passes true. Without it the
// folder must already hold the marker, so a mistyped or missing profile fails
// closed. An existing non-empty folder without a marker is refused.
Profile fromRoot(const QString &root, bool create,
                 const QStringList &protectedDirs = protectedDirectories());

// Finds "--test-profile <folder>" in `arguments` (the first element, the
// program name, is skipped). Absent: requested=false. Present but missing its
// value, repeated or invalid: requested=true, active=false, error set.
Profile fromArguments(const QStringList &arguments, bool create,
                      const QStringList &protectedDirs = protectedDirectories());

// True for "HKEY_CURRENT_USER\Software\TaskNestTestProfiles\<16 hex>" only.
bool isProfileRegistryBase(const QString &base);

} // namespace TestProfile
