#include "applications_model.hpp"
#include <QThread>
#include <QStringList>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace night::ui {
namespace {
QString qualityName(Quality quality) {
    switch (quality) {
    case Quality::permission_denied: return QStringLiteral("permission_denied");
    case Quality::unsupported: return QStringLiteral("unsupported");
    case Quality::warming: return QStringLiteral("warming");
    case Quality::stale: return QStringLiteral("stale");
    case Quality::valid: return QStringLiteral("valid");
    default: return QStringLiteral("error");
    }
}
}
ApplicationsModel::ApplicationsModel(QObject* parent) : QAbstractTableModel(parent) {}
int ApplicationsModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : count(); }
int ApplicationsModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : ColumnCount; }
QVariant ApplicationsModel::data(const QModelIndex& index, int role) const {
    if (role != Qt::DisplayRole || !index.isValid() || index.model() != this || index.row() >= count()
        || index.row() < 0 || index.column() < 0 || index.column() >= ColumnCount) return {};
    const auto& group = groups_[static_cast<std::size_t>(index.row())];
    const auto missing = QString(QChar(0x2014));
    switch (index.column()) {
    case Scope: return group.name;
    case Processes: return QVariant::fromValue<qulonglong>(group.members);
    case Cpu: return group.cpu_valid ? QString::number(group.cpu, 'f', 1) : missing;
    case Rss: return group.rss_valid ? QString::number(static_cast<double>(group.rss) / 1048576, 'f', 1) : missing;
    default: return {};
    }
}
QVariant ApplicationsModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (role != Qt::DisplayRole || section < 0) return {};
    if (orientation == Qt::Vertical) return section < count() ? QVariant(section + 1) : QVariant{};
    static const QStringList labels{QStringLiteral("Observed scope"), QStringLiteral("Processes"),
        QStringLiteral("CPU % sum"), QStringLiteral("RSS MiB estimate")};
    return section < ColumnCount ? labels[section] : QVariant{};
}
QString ApplicationsModel::unitAtRow(int row) const {
    return row >= 0 && row < count() ? groups_[static_cast<std::size_t>(row)].name : QString{};
}
int ApplicationsModel::rowForUnit(const QString& unit) const {
    int found = -1;
    for (int row = 0; row < count(); ++row) if (unitAtRow(row) == unit) {
        if (found >= 0) return -1; // Ambiguous observational grouping cannot seed an action.
        found = row;
    }
    return found;
}
QString ApplicationsModel::seedPidForUnit(const QString& unit) const {
    const auto row = rowForUnit(unit);
    return row >= 0 ? QString::number(groups_[static_cast<std::size_t>(row)].seed_pid) : QString{};
}
void ApplicationsModel::refreshAge(Clock::time_point now, std::chrono::milliseconds maximum_age) {
    const auto age = data_end_ && now >= *data_end_
        ? std::chrono::duration_cast<std::chrono::milliseconds>(now - *data_end_).count() : -1;
    const auto previous_quality = quality();
    const bool age_changed = age_ != age;
    age_ = age;
    maximum_age_ = maximum_age;
    if (age_changed || quality() != previous_quality) emit changed();
}
bool ApplicationsModel::apply(std::shared_ptr<const ProcessSnapshot> snapshot) {
    if (QThread::currentThread() != thread() || !snapshot || snapshot->stamp.sequence <= sequence_) return false;
    sequence_ = snapshot->stamp.sequence;
    if (snapshot->stamp.quality != Quality::valid) {
        quality_ = qualityName(snapshot->stamp.quality); emit changed(); return true;
    }
    if (snapshot->boot_id.empty() || snapshot->pid_namespace.empty()
        || snapshot->processes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        quality_ = QStringLiteral("error"); emit changed(); return true;
    }
    if (snapshot->cgroup_namespace.empty()) {
        auto quality = Quality::unsupported;
        for (const auto& process : snapshot->processes) {
            if (process.application_scope.quality == Quality::permission_denied) { quality = Quality::permission_denied; break; }
            if (process.application_scope.quality == Quality::error) quality = Quality::error;
            else if (process.application_scope.quality == Quality::stale && quality == Quality::unsupported) quality = Quality::stale;
        }
        quality_ = qualityName(quality); emit changed(); return true;
    }
    std::map<std::string, Group> incoming;
    std::set<std::uint64_t> pids;
    int unclassified = 0, unavailable = 0;
    for (const auto& process : snapshot->processes) {
        if (!process.pid || !pids.insert(process.pid).second) {
            quality_ = QStringLiteral("error"); emit changed(); return true;
        }
        const auto& reading = process.application_scope;
        if (reading.quality == Quality::unsupported) { ++unclassified; continue; }
        if (reading.quality != Quality::valid || !reading.value) { ++unavailable; continue; }
        const auto& candidate = *reading.value;
        if (candidate.cgroup_path.size() > 4096) { ++unavailable; continue; }
        const auto checked = parse_application_scope_candidate("0::" + candidate.cgroup_path + "\n");
        if (!checked.value || checked.value->unit_name != candidate.unit_name
            || checked.value->cgroup_path != candidate.cgroup_path) { ++unavailable; continue; }
        const auto key = snapshot->boot_id + '\0' + snapshot->cgroup_namespace + '\0' + candidate.cgroup_path;
        auto& group = incoming[key];
        group.key = key;
        group.name = QString::fromUtf8(candidate.unit_name.data(), static_cast<qsizetype>(candidate.unit_name.size()));
        ++group.members;
        if (!group.seed_pid || process.pid < group.seed_pid) group.seed_pid = process.pid;
        if (process.cpu_percent.quality != Quality::valid || !process.cpu_percent.value
            || !std::isfinite(*process.cpu_percent.value) || *process.cpu_percent.value < 0) group.cpu_valid = false;
        else { group.cpu += *process.cpu_percent.value; if (!std::isfinite(group.cpu)) group.cpu_valid = false; }
        if (process.rss_bytes.quality != Quality::valid || !process.rss_bytes.value
            || *process.rss_bytes.value > std::numeric_limits<std::uint64_t>::max() - group.rss) group.rss_valid = false;
        else group.rss += *process.rss_bytes.value;
    }
    for (int row = count() - 1; row >= 0; --row) {
        if (!incoming.contains(groups_[static_cast<std::size_t>(row)].key)) {
            beginRemoveRows({}, row, row); groups_.erase(groups_.begin() + row); endRemoveRows();
        }
    }
    const auto retained = count();
    for (auto& group : groups_) { auto found = incoming.find(group.key); group = std::move(found->second); incoming.erase(found); }
    if (retained > 0) emit dataChanged(index(0, 0), index(retained - 1, ColumnCount - 1));
    if (!incoming.empty()) {
        const int added = static_cast<int>(incoming.size());
        beginInsertRows({}, retained, retained + added - 1);
        for (auto& [key, group] : incoming) groups_.push_back(std::move(group));
        endInsertRows();
    }
    unclassified_ = unclassified; unavailable_ = unavailable;
    quality_ = unavailable > 0 ? QStringLiteral("partial") : QStringLiteral("valid");
    data_end_ = snapshot->stamp.end;
    emit changed(); return true;
}
}
