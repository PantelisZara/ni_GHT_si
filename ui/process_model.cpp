#include "process_model.hpp"

#include <QHash>
#include <QSet>
#include <QStringList>
#include <QThread>

#include <array>
#include <cmath>
#include <limits>

namespace night::ui {
namespace {
QString quality_name(Quality quality) {
    switch (quality) {
    case Quality::valid: return QStringLiteral("valid");
    case Quality::warming: return QStringLiteral("warming");
    case Quality::stale: return QStringLiteral("stale");
    case Quality::permission_denied: return QStringLiteral("permission_denied");
    case Quality::unsupported: return QStringLiteral("unsupported");
    case Quality::error: return QStringLiteral("error");
    }
    return QStringLiteral("error");
}

QString identity(const QString& context_prefix, const ProcessInfo& process) {
    return context_prefix + QString::number(process.pid)
        + QLatin1Char(':') + QString::number(process.start_ticks);
}

QString display_name(const std::string& bytes) {
    const auto decoded = QString::fromUtf8(bytes.data(), static_cast<qsizetype>(bytes.size()));
    QString result;
    result.reserve(decoded.size());
    for (const QChar character : decoded) {
        const auto category = character.category();
        if (category == QChar::Other_Control || category == QChar::Other_Format
            || category == QChar::Separator_Line || category == QChar::Separator_Paragraph) {
            result += QStringLiteral("\\u%1").arg(static_cast<uint>(character.unicode()), 4, 16, QLatin1Char('0'));
        } else {
            result += character;
        }
    }
    return result;
}

bool cpu_available(const ProcessInfo& process) {
    return process.cpu_percent.quality == Quality::valid && process.cpu_percent.value
        && std::isfinite(*process.cpu_percent.value) && *process.cpu_percent.value >= 0;
}

bool rss_available(const ProcessInfo& process) {
    return process.rss_bytes.quality == Quality::valid && process.rss_bytes.value.has_value();
}
} // namespace

ProcessModel::ProcessModel(QObject* parent) : QAbstractTableModel(parent) {}

QVariantMap ProcessModel::details(const QModelIndex& index) const {
    if (!index.isValid() || index.model() != this || index.row() < 0 || index.row() >= count()
        || index.column() < 0 || index.column() >= ColumnCount || !snapshot_) return {};
    const auto& process = snapshot_->processes[rows_[static_cast<std::size_t>(index.row())].source];
    auto scope_quality = process.application_scope.quality;
    QVariant scope_unit;
    if (scope_quality == Quality::valid) {
        if (process.application_scope.value && process.application_scope.value->cgroup_path.size() <= 4096) {
            const auto& candidate = *process.application_scope.value;
            const auto checked = parse_application_scope_candidate("0::" + candidate.cgroup_path + "\n");
            if (checked.value && checked.value->cgroup_path == candidate.cgroup_path
                && checked.value->unit_name == candidate.unit_name) scope_unit = display_name(candidate.unit_name);
            else scope_quality = Quality::error;
        } else scope_quality = Quality::error;
    }
    const QVariant state = process.state >= '!' && process.state <= '~'
        ? QVariant(QString(QChar::fromLatin1(process.state))) : QVariant{};
    return {{QStringLiteral("pid"), QVariant::fromValue<qulonglong>(process.pid)},
            {QStringLiteral("parentPid"), QVariant::fromValue<qulonglong>(process.parent_pid)},
            {QStringLiteral("threads"), QVariant::fromValue<qulonglong>(process.threads)},
            {QStringLiteral("startTicks"), QVariant::fromValue<qulonglong>(process.start_ticks)},
            {QStringLiteral("uid"), QVariant::fromValue<qulonglong>(process.uid)},
            {QStringLiteral("name"), display_name(process.name)},
            {QStringLiteral("state"), state},
            {QStringLiteral("cpuPercent"), cpu_available(process) ? QVariant(*process.cpu_percent.value) : QVariant{}},
            {QStringLiteral("cpuQuality"), quality_name(cpu_available(process) ? Quality::valid :
                process.cpu_percent.quality == Quality::valid ? Quality::error : process.cpu_percent.quality)},
            {QStringLiteral("rssBytes"), rss_available(process) ? QVariant::fromValue<qulonglong>(*process.rss_bytes.value) : QVariant{}},
            {QStringLiteral("rssQuality"), quality_name(rss_available(process) ? Quality::valid :
                process.rss_bytes.quality == Quality::valid ? Quality::error : process.rss_bytes.quality)},
            {QStringLiteral("scopeUnit"), scope_unit},
            {QStringLiteral("scopeQuality"), quality_name(scope_quality)},
            {QStringLiteral("quality"), quality_}};
}

int ProcessModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : count();
}

int ProcessModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant ProcessModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.model() != this || index.row() < 0 || index.row() >= count()
        || index.column() < 0 || index.column() >= ColumnCount || !snapshot_) return {};
    const auto& row = rows_[static_cast<std::size_t>(index.row())];
    if (role == IdentityRole) return row.identity;
    if (role != Qt::DisplayRole && role != SortRole) return {};
    const auto& process = snapshot_->processes[row.source];
    const bool display = role == Qt::DisplayRole;
    const auto missing = [&]() -> QVariant {
        return display ? QVariant(QString(QChar(0x2014))) : QVariant{};
    };
    switch (index.column()) {
    case Pid: return QVariant::fromValue<qulonglong>(process.pid);
    case Name: return display_name(process.name);
    case Cpu:
        if (!cpu_available(process)) return missing();
        return display ? QVariant(QString::number(*process.cpu_percent.value, 'f', 1))
                       : QVariant(*process.cpu_percent.value);
    case Rss:
        if (!rss_available(process)) return missing();
        // Sort raw bytes, avoiding rounding and retaining numeric ordering.
        return display ? QVariant(QString::number(static_cast<double>(*process.rss_bytes.value)
                                                  / (1024.0 * 1024.0), 'f', 1))
                       : QVariant::fromValue<qulonglong>(*process.rss_bytes.value);
    case State:
        return process.state >= '!' && process.state <= '~'
            ? QVariant(QString(QChar::fromLatin1(process.state))) : missing();
    case Uid: return QVariant::fromValue<qulonglong>(process.uid);
    default: return {};
    }
}

QVariant ProcessModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (role != Qt::DisplayRole || section < 0) return {};
    if (orientation == Qt::Vertical) return section < count() ? QVariant(section + 1) : QVariant{};
    static const QStringList headers{QStringLiteral("PID"), QStringLiteral("Name"),
        QStringLiteral("CPU %"), QStringLiteral("RSS MiB"), QStringLiteral("State"), QStringLiteral("UID")};
    return section < ColumnCount ? QVariant(headers[section]) : QVariant{};
}

QHash<int, QByteArray> ProcessModel::roleNames() const {
    auto roles = QAbstractTableModel::roleNames();
    roles.insert(SortRole, "sortValue");
    roles.insert(IdentityRole, "identity");
    return roles;
}

bool ProcessModel::apply(std::shared_ptr<const ProcessSnapshot> snapshot) {
    if (QThread::currentThread() != thread() || !snapshot || snapshot->stamp.sequence <= sequence_)
        return false;

    auto next_quality = snapshot->stamp.quality;
    QString context_prefix;
    QHash<QString, std::size_t> incoming;
    QSet<qulonglong> pids;
    if (next_quality == Quality::valid) {
        if (snapshot->boot_id.empty() || snapshot->pid_namespace.empty()
            || snapshot->processes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            next_quality = Quality::error;
        } else {
            // Encode arbitrary context bytes once for this snapshot; the
            // trailing separator preserves the full per-row identity format.
            context_prefix = QString::fromLatin1(QByteArray::fromStdString(snapshot->boot_id).toHex())
                + QLatin1Char(':')
                + QString::fromLatin1(QByteArray::fromStdString(snapshot->pid_namespace).toHex())
                + QLatin1Char(':');
            for (std::size_t i = 0; i < snapshot->processes.size(); ++i) {
                const auto& process = snapshot->processes[i];
                const auto key = identity(context_prefix, process);
                if (process.pid == 0 || pids.contains(process.pid) || incoming.contains(key)) {
                    next_quality = Quality::error;
                    break;
                }
                pids.insert(process.pid);
                incoming.insert(key, i);
            }
        }
    }
    sequence_ = snapshot->stamp.sequence;
    quality_ = quality_name(next_quality);
    if (next_quality != Quality::valid) {
        emit changed();
        return true;
    }

    const bool context_changed = snapshot_ && (snapshot_->boot_id != snapshot->boot_id
                                                || snapshot_->pid_namespace != snapshot->pid_namespace);
    if (context_changed) {
        beginResetModel();
        rows_.clear();
        rows_.reserve(snapshot->processes.size());
        for (std::size_t i = 0; i < snapshot->processes.size(); ++i)
            rows_.push_back({identity(context_prefix, snapshot->processes[i]), i});
        snapshot_ = std::move(snapshot);
        endResetModel();
    } else {
        // Retained rows keep relative order; Qt updates persistent indices on removal.
        for (int last = count() - 1; last >= 0;) {
            if (incoming.contains(rows_[static_cast<std::size_t>(last)].identity)) { --last; continue; }
            int first = last;
            while (first > 0 && !incoming.contains(rows_[static_cast<std::size_t>(first - 1)].identity)) --first;
            beginRemoveRows({}, first, last);
            rows_.erase(rows_.begin() + first, rows_.begin() + last + 1);
            endRemoveRows();
            last = first - 1;
        }
        const int retained = count();
        std::array<int, ColumnCount> first_changed, last_changed;
        first_changed.fill(retained);
        last_changed.fill(-1);
        QSet<QString> retained_keys;
        for (int position = 0; position < retained; ++position) {
            auto& row = rows_[static_cast<std::size_t>(position)];
            const auto source = incoming.value(row.identity);
            const auto& before = snapshot_->processes[row.source];
            const auto& after = snapshot->processes[source];
            const bool before_cpu = cpu_available(before), after_cpu = cpu_available(after);
            const bool before_rss = rss_available(before), after_rss = rss_available(after);
            const bool before_state = before.state >= '!' && before.state <= '~';
            const bool after_state = after.state >= '!' && after.state <= '~';
            const std::array<bool, ColumnCount> changed{
                false, // Retained identity includes PID.
                before.name != after.name && display_name(before.name) != display_name(after.name),
                before_cpu != after_cpu || (before_cpu && after_cpu
                    && *before.cpu_percent.value != *after.cpu_percent.value),
                before_rss != after_rss || (before_rss && after_rss
                    && *before.rss_bytes.value != *after.rss_bytes.value),
                before_state != after_state || (before_state && before.state != after.state),
                before.uid != after.uid};
            for (std::size_t column = 0; column < changed.size(); ++column) {
                if (!changed[column]) continue;
                if (last_changed[column] < 0) first_changed[column] = position;
                last_changed[column] = position;
            }
            row.source = source;
            retained_keys.insert(row.identity);
        }
        snapshot_ = std::move(snapshot);
        for (int column = 0; column < ColumnCount; ++column) {
            const auto offset = static_cast<std::size_t>(column);
            if (last_changed[offset] >= 0)
                emit dataChanged(index(first_changed[offset], column), index(last_changed[offset], column),
                                 {Qt::DisplayRole, SortRole});
        }
        const int added = static_cast<int>(snapshot_->processes.size()) - retained;
        if (added > 0) {
            beginInsertRows({}, retained, retained + added - 1);
            for (std::size_t i = 0; i < snapshot_->processes.size(); ++i) {
                auto key = identity(context_prefix, snapshot_->processes[i]);
                if (!retained_keys.contains(key)) rows_.push_back({std::move(key), i});
            }
            endInsertRows();
        }
    }
    emit changed();
    return true;
}

} // namespace night::ui
