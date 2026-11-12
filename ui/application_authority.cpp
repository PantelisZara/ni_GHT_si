#include "application_authority.hpp"
#include "ni_ght_si/descriptor_budget.hpp"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusArgument>
#include <QDBusUnixFileDescriptor>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QDeadlineTimer>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QThread>
#include <QStringList>
#include <algorithm>
#include <optional>
#include <limits>
#include <sys/stat.h>
#include <poll.h>
#include <unistd.h>

namespace night::ui {
namespace {
constexpr auto manager_path = "/org/freedesktop/systemd1";
constexpr auto manager_interface = "org.freedesktop.systemd1.Manager";
constexpr auto unit_interface = "org.freedesktop.systemd1.Unit";
constexpr auto scope_interface = "org.freedesktop.systemd1.Scope";

std::optional<QVariantList> callArguments(const QDBusConnection& bus, const QString& owner, const QString& path,
                             const QString& interface, const QString& method, const QVariantList& arguments,
                             const QDeadlineTimer& deadline) {
    const auto remaining = deadline.remainingTime();
    if (remaining <= 0) return {};
    auto message = QDBusMessage::createMethodCall(owner, path, interface, method);
    message.setArguments(arguments);
    message.setAutoStartService(false);
    const auto reply = bus.call(message, QDBus::Block, static_cast<int>(std::min<qint64>(remaining, 150)));
    if (reply.type() != QDBusMessage::ReplyMessage) return {};
    return reply.arguments();
}
std::optional<QVariant> call(const QDBusConnection& bus, const QString& owner, const QString& path,
                             const QString& interface, const QString& method, const QVariantList& arguments,
                             const QDeadlineTimer& deadline) {
    const auto reply = callArguments(bus, owner, path, interface, method, arguments, deadline);
    if (!reply || reply->size() != 1) return {};
    return reply->front();
}
std::optional<QVariant> property(const QDBusConnection& bus, const QString& owner, const QString& path,
                                 const char* interface, const char* name, const QDeadlineTimer& deadline) {
    const auto reply = call(bus, owner, path, QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"),
                            {QString::fromLatin1(interface), QString::fromLatin1(name)}, deadline);
    if (!reply || reply->metaType() != QMetaType::fromType<QDBusVariant>()) return {};
    return reply->value<QDBusVariant>().variant();
}
bool text(const std::optional<QVariant>& value, const QString& expected) {
    return value && value->metaType().id() == QMetaType::QString && value->toString() == expected;
}
bool boolean(const std::optional<QVariant>& value, bool expected) {
    return value && value->metaType().id() == QMetaType::Bool && value->toBool() == expected;
}
std::optional<QString> owner(const QDBusConnection& bus, const QDeadlineTimer& deadline) {
    const auto value = call(bus, QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
                            QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetNameOwner"),
                            {QStringLiteral("org.freedesktop.systemd1")}, deadline);
    if (!value || value->metaType().id() != QMetaType::QString) return {};
    const auto name = value->toString();
    return name.startsWith(QLatin1Char(':')) && name.size() <= 255 ? std::optional{name} : std::nullopt;
}
std::optional<QString> unitForPid(const QDBusConnection& bus, const QString& manager, std::uint64_t pid,
                                 const QDeadlineTimer& deadline) {
    if (pid > std::numeric_limits<quint32>::max()) return {};
    const auto value = call(bus, manager, QString::fromLatin1(manager_path), QString::fromLatin1(manager_interface),
                            QStringLiteral("GetUnitByPID"), {QVariant::fromValue(static_cast<quint32>(pid))}, deadline);
    if (!value || value->metaType() != QMetaType::fromType<QDBusObjectPath>()) return {};
    const auto path = value->value<QDBusObjectPath>().path();
    return path.startsWith(QStringLiteral("/org/freedesktop/systemd1/unit/")) && path.size() <= 4096
        ? std::optional{path} : std::nullopt;
}
QString escapedId(const QByteArray& id) {
    QByteArray result;
    constexpr char hex[] = "0123456789abcdef";
    for (qsizetype i = 0; i < id.size(); ++i) {
        const auto c = static_cast<unsigned char>(id[i]);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '_' || c == ':' || (c == '.' && i != 0)) result += static_cast<char>(c);
        else { result += "\\x"; result += hex[c >> 4]; result += hex[c & 15]; }
    }
    return QString::fromLatin1(result);
}
std::optional<QString> desktopStem(const QString& unit) {
    if (!unit.startsWith(QStringLiteral("app-")) || !unit.endsWith(QStringLiteral(".scope")) || unit.size() > 255) return {};
    const auto body = unit.mid(4, unit.size() - 10);
    const auto separator = body.indexOf(QLatin1Char('-'));
    if (separator <= 0 || separator + 1 >= body.size()) return {};
    const auto encoded = body.left(separator).toLatin1();
    const auto suffix = body.mid(separator + 1);
    for (const auto c : suffix)
        if (!((c >= QLatin1Char('a') && c <= QLatin1Char('z')) || (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
              || (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || c == QLatin1Char('-') || c == QLatin1Char('_'))) return {};
    QByteArray decoded;
    const auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    for (qsizetype i = 0; i < encoded.size(); ++i) {
        if (encoded[i] != '\\') decoded += encoded[i];
        else {
            if (i + 3 >= encoded.size() || encoded[i + 1] != 'x' || hex(encoded[i + 2]) < 0 || hex(encoded[i + 3]) < 0) return {};
            decoded += static_cast<char>(hex(encoded[i + 2]) * 16 + hex(encoded[i + 3]));
            i += 3;
        }
    }
    if (decoded.isEmpty() || decoded == "." || decoded == ".." || escapedId(decoded) != QString::fromLatin1(encoded)) return {};
    for (const auto c : decoded)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
              || c == '_' || c == '-' || c == '.')) return {};
    return QString::fromLatin1(decoded);
}
bool protectedPath(const QString& path, bool executable) {
    auto current = path;
    bool first = true;
    for (int depth = 0; depth < 64; ++depth) {
        struct stat metadata{};
        const auto bytes = QFile::encodeName(current);
        if (::lstat(bytes.constData(), &metadata) != 0 || metadata.st_uid != 0 || (metadata.st_mode & 0022) != 0) return false;
        if (first) {
            if (!S_ISREG(metadata.st_mode) || (executable && (metadata.st_mode & 0111) == 0)) return false;
        } else if (!S_ISDIR(metadata.st_mode)) return false;
        if (current == QStringLiteral("/")) return true;
        current = QFileInfo(current).absolutePath();
        first = false;
    }
    return false;
}
std::optional<QString> executableToken(const QString& command) {
    const auto input = command.trimmed();
    if (input.isEmpty()) return {};
    QString token;
    if (input.startsWith(QLatin1Char('"'))) {
        const auto end = input.indexOf(QLatin1Char('"'), 1);
        if (end <= 1 || (end + 1 < input.size() && !input[end + 1].isSpace())) return {};
        token = input.mid(1, end - 1);
    } else {
        qsizetype end = 0;
        while (end < input.size() && !input[end].isSpace()) ++end;
        token = input.left(end);
    }
    for (const auto c : token)
        if (!((c >= QLatin1Char('a') && c <= QLatin1Char('z')) || (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
              || (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || QStringLiteral("/_-.+ ").contains(c))) return {};
    if (!token.startsWith(QLatin1Char('/'))) {
        if (token.contains(QLatin1Char('/')) || token.contains(QLatin1Char(' '))) return {};
        token.prepend(QStringLiteral("/usr/bin/")); // Never resolve through the caller's PATH.
    }
    const auto canonical = QFileInfo(token).canonicalFilePath();
    if (canonical.isEmpty() || !protectedPath(canonical, true)) return {};
    return canonical;
}
bool infrastructure(const QString& executable) {
    const auto name = QFileInfo(executable).fileName();
    const QStringList denied{QStringLiteral("systemd"), QStringLiteral("dbus-daemon"), QStringLiteral("dbus-broker"),
        QStringLiteral("dbus-broker-launch"), QStringLiteral("kwin_wayland"), QStringLiteral("kwin_x11"),
        QStringLiteral("plasmashell"), QStringLiteral("gnome-shell"), QStringLiteral("gnome-session"),
        QStringLiteral("gnome-session-binary"), QStringLiteral("ksmserver"), QStringLiteral("kded5"), QStringLiteral("kded6"),
        QStringLiteral("sway"), QStringLiteral("Hyprland"), QStringLiteral("weston"), QStringLiteral("Xorg"), QStringLiteral("Xwayland"),
        QStringLiteral("pipewire"), QStringLiteral("wireplumber"), QStringLiteral("pulseaudio"), QStringLiteral("polkitd"),
        QStringLiteral("polkit-kde-authentication-agent-1"), QStringLiteral("gnome-keyring-daemon"),
        QStringLiteral("kwalletd5"), QStringLiteral("kwalletd6"), QStringLiteral("kglobalacceld"),
        QStringLiteral("at-spi2-registryd"), QStringLiteral("at-spi-bus-launcher"), QStringLiteral("ibus-daemon"),
        QStringLiteral("fcitx5"), QStringLiteral("env"), QStringLiteral("sh"), QStringLiteral("bash"),
        QStringLiteral("dash"), QStringLiteral("zsh"), QStringLiteral("fish"), QStringLiteral("ksh"),
        QStringLiteral("flatpak"), QStringLiteral("bwrap"), QStringLiteral("systemd-run"), QStringLiteral("gtk-launch")};
    return denied.contains(name) || name.startsWith(QStringLiteral("xdg-desktop-portal"))
        || name.startsWith(QStringLiteral("python")) || name.startsWith(QStringLiteral("perl"))
        || name.startsWith(QStringLiteral("ruby")) || name == QStringLiteral("node");
}
const char* installedApplicationFailure(const QString& stem, const ProcessActionContext& context) {
    const auto entry = QStringLiteral("/usr/share/applications/") + stem + QStringLiteral(".desktop");
    const auto canonical_entry = QFileInfo(entry).canonicalFilePath();
    if (!canonical_entry.startsWith(QStringLiteral("/usr/share/applications/")))
        return "installed desktop entry is unavailable";
    if (!protectedPath(canonical_entry, false)) return "desktop entry ownership or permissions are not trusted";
    // These two explicit QFile opens are managed by this resolver, unlike
    // opaque D-Bus transport descriptors. Close both before returning capacity.
    auto desktop_descriptors = DescriptorReservation::try_acquire(2);
    if (!desktop_descriptors) return "temporary descriptor budget unavailable for desktop metadata";
    QFile desktop(canonical_entry);
    if (!desktop.open(QIODevice::ReadOnly)) return "installed desktop metadata cannot be read";
    const auto bytes = desktop.read(65537);
    if (bytes.size() > 65536 || desktop.error() != QFileDevice::NoError || bytes.contains('\0'))
        return "installed desktop metadata is malformed or oversized";
    const auto content = QString::fromUtf8(bytes);
    if (content.contains(QChar::ReplacementCharacter)) return "installed desktop metadata has invalid encoding";
    bool main_group = false, seen_group = false;
    QMap<QString, QString> properties;
    for (const auto& line : content.split(QLatin1Char('\n'))) {
        const auto trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#'))) continue;
        if (trimmed.startsWith(QLatin1Char('['))) {
            main_group = trimmed == QStringLiteral("[Desktop Entry]");
            if (main_group && seen_group) return "installed desktop metadata has duplicate entry groups";
            seen_group = seen_group || main_group;
            continue;
        }
        if (!main_group) continue;
        const auto separator = trimmed.indexOf(QLatin1Char('='));
        if (separator <= 0) return "installed desktop metadata contains malformed properties";
        const auto key = trimmed.left(separator).trimmed();
        if (properties.contains(key)) return "installed desktop metadata contains duplicate properties";
        properties.insert(key, trimmed.mid(separator + 1).trimmed());
    }
    if (!seen_group || properties.value(QStringLiteral("Type")) != QStringLiteral("Application"))
        return "installed desktop entry is not an application";
    for (const auto& key : {QStringLiteral("Hidden"), QStringLiteral("NoDisplay"), QStringLiteral("Terminal")})
        if (properties.contains(key) && properties.value(key) != QStringLiteral("false"))
            return "desktop entry uses a hidden or unsupported launch mode";
    for (auto it = properties.cbegin(); it != properties.cend(); ++it)
        if (it.key().startsWith(QStringLiteral("X-KDE-autostart")) || it.key().startsWith(QStringLiteral("X-GNOME-Autostart-Phase")))
            return "desktop entry identifies protected autostart infrastructure";
    const auto executable = executableToken(properties.value(QStringLiteral("Exec")));
    if (!executable) return "desktop direct executable is unavailable or not trusted";
    if (infrastructure(*executable)) return "desktop executable identifies protected infrastructure";
    QFile binary(*executable);
    if (!binary.open(QIODevice::ReadOnly) || binary.read(4) != QByteArray("\x7f" "ELF", 4))
        return "desktop executable is not an accessible direct ELF image";
    struct stat metadata{};
    if (::fstat(binary.handle(), &metadata) != 0 || metadata.st_uid != 0 || (metadata.st_mode & 0022) != 0)
        return "desktop executable ownership or permissions are not trusted";
    const auto& expected = context.executable.executable;
    if (static_cast<std::uint64_t>(metadata.st_dev) != expected.device ||
        static_cast<std::uint64_t>(metadata.st_ino) != expected.inode)
        return "process executable does not match the installed desktop entry";
    if (expected.owner_uid != 0 || (expected.mode & 0022) != 0 || !S_ISREG(expected.mode) || (expected.mode & 06000) != 0)
        return "process executable ownership or permissions are not trusted";
    return nullptr;
}
struct PeerCredentials {
    quint32 uid{}, pid{};
    QDBusUnixFileDescriptor process;
};
std::optional<PeerCredentials> peerCredentials(const QDBusConnection& bus, const QString& peer,
                                               const QDeadlineTimer& deadline) {
    const auto value = call(bus, QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetConnectionCredentials"), {peer}, deadline);
    if (!value || (value->metaType() != QMetaType::fromType<QDBusArgument>() && value->metaType().id() != QMetaType::QVariantMap)) return {};
    const auto credentials = qdbus_cast<QVariantMap>(*value);
    const auto uid = credentials.value(QStringLiteral("UnixUserID"));
    const auto pid = credentials.value(QStringLiteral("ProcessID"));
    const auto fd = credentials.value(QStringLiteral("ProcessFD"));
    if (uid.metaType().id() != QMetaType::UInt || pid.metaType().id() != QMetaType::UInt ||
        fd.metaType() != QMetaType::fromType<QDBusUnixFileDescriptor>()) return {};
    auto process = fd.value<QDBusUnixFileDescriptor>();
    if (!process.isValid()) return {};
    return PeerCredentials{uid.toUInt(), pid.toUInt(), process};
}
bool running(const QDBusUnixFileDescriptor& process) {
    if (!process.isValid()) return false;
    pollfd fd{process.fileDescriptor(), POLLIN, 0};
    return ::poll(&fd, 1, 0) == 0 && fd.revents == 0;
}
bool nativeSystemSocket() {
    struct stat socket{};
    if (::lstat("/run/dbus/system_bus_socket", &socket) != 0 || socket.st_uid != 0 || !S_ISSOCK(socket.st_mode)) return false;
    for (const char* directory : {"/run/dbus", "/run", "/"}) {
        struct stat metadata{};
        if (::lstat(directory, &metadata) != 0 || metadata.st_uid != 0 || !S_ISDIR(metadata.st_mode) ||
            (metadata.st_mode & 0022) != 0) return false;
    }
    return true;
}
struct UnitByPeer {
    QString path, name;
    QByteArray invocation;
    bool operator==(const UnitByPeer&) const = default;
};
std::optional<UnitByPeer> unitByPeer(const QDBusConnection& bus, const QString& manager,
                                    const QDBusUnixFileDescriptor& process, const QDeadlineTimer& deadline) {
    const auto values = callArguments(bus, manager, QString::fromLatin1(manager_path), QString::fromLatin1(manager_interface),
        QStringLiteral("GetUnitByPIDFD"), {QVariant::fromValue(process)}, deadline);
    if (!values || values->size() != 3 || values->at(0).metaType() != QMetaType::fromType<QDBusObjectPath>() ||
        values->at(1).metaType().id() != QMetaType::QString || values->at(2).metaType().id() != QMetaType::QByteArray) return {};
    const auto invocation = values->at(2).toByteArray();
    if (invocation.size() != 16 || invocation == QByteArray(16, '\0')) return {};
    return UnitByPeer{values->at(0).value<QDBusObjectPath>().path(), values->at(1).toString(), invocation};
}
bool expectedManagerExec(const std::optional<QVariant>& value, const QString& installed) {
    if (!value || value->metaType() != QMetaType::fromType<QDBusArgument>()) return false;
    const auto argument = value->value<QDBusArgument>();
    if (argument.currentSignature() != QStringLiteral("a(sasbttttuii)")) return false;
    argument.beginArray();
    if (argument.atEnd()) { argument.endArray(); return false; }
    argument.beginStructure();
    QString executable; QStringList args; bool ignore_failure{};
    qulonglong start_realtime{}, start_monotonic{}, exit_realtime{}, exit_monotonic{};
    quint32 pid{}; qint32 code{}, status{};
    argument >> executable >> args >> ignore_failure >> start_realtime >> start_monotonic
             >> exit_realtime >> exit_monotonic >> pid >> code >> status;
    argument.endStructure(); const bool only_one = argument.atEnd(); argument.endArray();
    return only_one && !ignore_failure && QFileInfo(executable).canonicalFilePath() == installed &&
        !args.isEmpty() && QFileInfo(args.front()).canonicalFilePath() == installed && args.contains(QStringLiteral("--user"));
}
struct ManagerProvenance {
    PeerCredentials user, system;
    QString system_owner;
    UnitByPeer unit;
    QVariant start_timestamp;
};
const char* managerProvenance(const QDBusConnection& user_bus, const QString& user_owner, quint32 user_pid,
    const QDBusConnection& system_bus, const QString& user_root, const QDeadlineTimer& deadline,
    ManagerProvenance& proof) {
    const auto user_peer = peerCredentials(user_bus, user_owner, deadline);
    if (!user_peer || user_peer->uid != ::geteuid() || user_peer->pid != user_pid || !running(user_peer->process))
        return "exact user-manager broker process descriptor is unavailable";
    proof.user = *user_peer;
    const auto system_owner = owner(system_bus, deadline);
    if (!system_owner) return "root system-manager owner is unavailable";
    const auto system_peer = peerCredentials(system_bus, *system_owner, deadline);
    if (!system_peer || system_peer->uid != 0 || system_peer->pid != 1 || !running(system_peer->process))
        return "root system-manager broker credentials are not trusted";
    proof.system = *system_peer; proof.system_owner = *system_owner;
    const auto unit = unitByPeer(system_bus, *system_owner, user_peer->process, deadline);
    const auto expected = QStringLiteral("user@%1.service").arg(::geteuid());
    if (!unit || unit->name != expected) return "root manager does not attest the exact user-manager process";
    proof.unit = *unit;
    if (!text(property(system_bus, *system_owner, unit->path, unit_interface, "Id", deadline), expected) ||
        !text(property(system_bus, *system_owner, unit->path, unit_interface, "LoadState", deadline), QStringLiteral("loaded")) ||
        !text(property(system_bus, *system_owner, unit->path, unit_interface, "ActiveState", deadline), QStringLiteral("active")) ||
        !text(property(system_bus, *system_owner, unit->path, unit_interface, "SubState", deadline), QStringLiteral("running")))
        return "root-managed user service is not loaded and running";
    constexpr auto service_interface = "org.freedesktop.systemd1.Service";
    const auto main_pid = property(system_bus, *system_owner, unit->path, service_interface, "MainPID", deadline);
    if (!main_pid || main_pid->metaType().id() != QMetaType::UInt || main_pid->toUInt() != user_peer->pid ||
        !text(property(system_bus, *system_owner, unit->path, service_interface, "User", deadline), QString::number(::geteuid())) ||
        !text(property(system_bus, *system_owner, unit->path, service_interface, "ControlGroup", deadline), user_root))
        return "root-managed user service process or membership does not match";
    const auto installed = QFileInfo(QStringLiteral("/usr/lib/systemd/systemd")).canonicalFilePath();
    if (installed.isEmpty() || !protectedPath(installed, true) ||
        !expectedManagerExec(property(system_bus, *system_owner, unit->path, service_interface, "ExecStart", deadline), installed))
        return "root-managed user service has no trusted direct manager command";
    const auto invocation = property(system_bus, *system_owner, unit->path, unit_interface, "InvocationID", deadline);
    const auto start = property(system_bus, *system_owner, unit->path, service_interface, "ExecMainStartTimestampMonotonic", deadline);
    if (!invocation || invocation->metaType().id() != QMetaType::QByteArray || invocation->toByteArray() != unit->invocation ||
        !start || start->metaType().id() != QMetaType::ULongLong || !start->toULongLong())
        return "root-managed user service generation is unavailable";
    proof.start_timestamp = *start;
    return nullptr;
}
bool stableManagerProvenance(const QDBusConnection& bus, const QDeadlineTimer& deadline,
                             const ManagerProvenance& proof, const QString& user_root) {
    constexpr auto service_interface = "org.freedesktop.systemd1.Service";
    const auto unit = unitByPeer(bus, proof.system_owner, proof.user.process, deadline);
    const auto main_pid = property(bus, proof.system_owner, proof.unit.path, service_interface, "MainPID", deadline);
    const auto invocation = property(bus, proof.system_owner, proof.unit.path, unit_interface, "InvocationID", deadline);
    return unit && *unit == proof.unit && running(proof.user.process) && running(proof.system.process) &&
        main_pid && main_pid->metaType().id() == QMetaType::UInt && main_pid->toUInt() == proof.user.pid &&
        text(property(bus, proof.system_owner, proof.unit.path, unit_interface, "ActiveState", deadline), QStringLiteral("active")) &&
        text(property(bus, proof.system_owner, proof.unit.path, service_interface, "ControlGroup", deadline), user_root) &&
        property(bus, proof.system_owner, proof.unit.path, service_interface, "ExecMainStartTimestampMonotonic", deadline) == proof.start_timestamp &&
        invocation && invocation->metaType().id() == QMetaType::QByteArray && invocation->toByteArray() == proof.unit.invocation &&
        owner(bus, deadline) == proof.system_owner;
}

ApplicationAuthority resolve(const ProcessActionContext& context) {
    const auto unavailable = [](const char* reason) {
        ApplicationAuthority result;
        result.reason = reason;
        return result;
    };
    const auto* app = QCoreApplication::instance();
    if (!app || QThread::currentThread() == app->thread())
        return unavailable("application authority requires the action worker");
    const auto pid = context.credentials.stat.pid;
    if (pid <= 1 || pid == static_cast<std::uint64_t>(::getpid()) || pid == static_cast<std::uint64_t>(::getppid()))
        return unavailable("selected process is protected monitor infrastructure");
    const auto unit = QString::fromStdString(context.scope.unit_name);
    const auto cgroup = QString::fromStdString(context.scope.cgroup_path);
    const auto stem = desktopStem(unit);
    if (!stem || cgroup.size() > 4096) return unavailable("scope has no supported installed desktop identity");
    if (const auto failure = installedApplicationFailure(*stem, context)) return unavailable(failure);
    const auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return unavailable("session D-Bus is unavailable");
    const QDeadlineTimer deadline(2000);
    const auto manager = owner(bus, deadline);
    if (!manager) return unavailable("user systemd manager owner is unavailable");
    const auto uid = call(bus, QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
                          QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetConnectionUnixUser"), {*manager}, deadline);
    if (!uid || uid->metaType().id() != QMetaType::UInt || uid->toUInt() != ::geteuid())
        return unavailable("native user-manager account identity could not be verified");
    const auto manager_pid = call(bus, QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
                                  QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetConnectionUnixProcessID"), {*manager}, deadline);
    if (!manager_pid || manager_pid->metaType().id() != QMetaType::UInt || manager_pid->toUInt() == pid)
        return unavailable("native user-manager process identity could not be verified");
    const auto path = unitForPid(bus, *manager, pid, deadline);
    if (!path) return unavailable("user manager does not map this process to a supported unit");
    const auto root = property(bus, *manager, QString::fromLatin1(manager_path), manager_interface, "ControlGroup", deadline);
    if (!root || root->metaType().id() != QMetaType::QString || root->toString().isEmpty() || root->toString() == QStringLiteral("/"))
        return unavailable("user-manager root membership is unavailable");
    if (cgroup != root->toString() + QStringLiteral("/app.slice/") + unit)
        return unavailable("process scope does not match the user-manager application root");
    if (!nativeSystemSocket()) return unavailable("native root system bus socket is not trusted");
    const auto system_bus = QDBusConnection::connectToBus(QStringLiteral("unix:path=/run/dbus/system_bus_socket"),
                                                        QStringLiteral("night-root-authority"));
    if (!system_bus.isConnected() || !(system_bus.connectionCapabilities() & QDBusConnection::UnixFileDescriptorPassing) ||
        !(bus.connectionCapabilities() & QDBusConnection::UnixFileDescriptorPassing))
        return unavailable("native broker process-descriptor transport is unavailable");
    // Receive at most two broker ProcessFDs. All FD-containing Qt values below
    // die before this reservation; shared descriptor copies do not duplicate FDs.
    auto peer_descriptors = DescriptorReservation::try_acquire(2);
    if (!peer_descriptors) return unavailable("temporary descriptor budget unavailable for manager provenance");
    ManagerProvenance provenance;
    if (const auto failure = managerProvenance(bus, *manager, manager_pid->toUInt(), system_bus,
                                             root->toString(), deadline, provenance)) return unavailable(failure);

    if (!text(property(bus, *manager, *path, unit_interface, "Id", deadline), unit))
        return unavailable("manager unit identity does not match the selected scope");
    if (!text(property(bus, *manager, *path, unit_interface, "LoadState", deadline), QStringLiteral("loaded"))
        || !text(property(bus, *manager, *path, unit_interface, "ActiveState", deadline), QStringLiteral("active"))
        || !text(property(bus, *manager, *path, unit_interface, "SubState", deadline), QStringLiteral("running")))
        return unavailable("application scope is not loaded and running");
    if (!boolean(property(bus, *manager, *path, unit_interface, "Transient", deadline), true)
        || !boolean(property(bus, *manager, *path, unit_interface, "Perpetual", deadline), false))
        return unavailable("application scope is not a supported transient unit");
    if (!text(property(bus, *manager, *path, scope_interface, "Slice", deadline), QStringLiteral("app.slice"))
        || !text(property(bus, *manager, *path, scope_interface, "ControlGroup", deadline), cgroup)
        || !boolean(property(bus, *manager, *path, scope_interface, "Delegate", deadline), false))
        return unavailable("scope membership or delegation could not be verified");
    const auto invocation = property(bus, *manager, *path, unit_interface, "InvocationID", deadline);
    if (!invocation || invocation->metaType().id() != QMetaType::QByteArray || invocation->toByteArray().size() != 16
        || invocation->toByteArray() == QByteArray(16, '\0'))
        return unavailable("application invocation generation is unavailable");
    if (!text(property(bus, *manager, *path, unit_interface, "ActiveState", deadline), QStringLiteral("active"))
        || !text(property(bus, *manager, *path, scope_interface, "ControlGroup", deadline), cgroup)
        || property(bus, *manager, *path, unit_interface, "InvocationID", deadline) != invocation
        || unitForPid(bus, *manager, pid, deadline) != path || owner(bus, deadline) != manager
        || !stableManagerProvenance(system_bus, deadline, provenance, root->toString()))
        return unavailable("application or root-managed manager generation changed during verification");
    // This is an authenticated managed-role proof, not a hash of the manager's
    // current image. Exact peer pidfds reject exit/reuse; role/exec can still
    // change after observation. Start timestamps are generation metadata only.
    const auto generation = provenance.system_owner.toStdString() + ":" +
        provenance.unit.invocation.toHex().toStdString() + ":" + invocation->toByteArray().toHex().toStdString();
    return {ApplicationAuthorityClass::application, context.scope.unit_name, context.scope.cgroup_path,
            manager->toStdString(), generation};
}
} // namespace
ApplicationAuthorityResolver makeApplicationAuthorityResolver() {
    return [](const ProcessActionContext& context) { return resolve(context); };
}
} // namespace night::ui
