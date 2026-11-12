#include "process_view_model.hpp"

#include <QThread>

namespace night::ui {

ProcessViewModel::ProcessViewModel(ProcessModel& source, QObject* parent)
    : QSortFilterProxyModel(parent) {
    setSourceModel(&source);
    setSortRole(ProcessModel::SortRole);
    setDynamicSortFilter(true);
    const auto viewChanged = [this] {
        synchronizeSelection();
        emit changed();
    };
    connect(this, &QAbstractItemModel::rowsInserted, this, viewChanged);
    connect(this, &QAbstractItemModel::rowsRemoved, this, viewChanged);
    connect(this, &QAbstractItemModel::layoutChanged, this, viewChanged);
    connect(this, &QAbstractItemModel::modelReset, this, viewChanged);
    connect(this, &QAbstractItemModel::dataChanged, this, viewChanged);
    // Source quality changes may retain the last observation without model changes.
    connect(&source, &ProcessModel::changed, this, viewChanged);
    sort(ProcessModel::Pid, Qt::AscendingOrder);
}

void ProcessViewModel::setQuery(const QString& query) {
    if (QThread::currentThread() != thread()) return;
    const auto bounded = query.left(256);
    if (bounded == query_) return;
    beginFilterChange();
    query_ = bounded;
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
    synchronizeSelection();
    emit queryChanged();
    emit changed();
}

bool ProcessViewModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const {
    if (query_.isEmpty()) return true;
    const auto* source = sourceModel();
    return source->index(sourceRow, ProcessModel::Name, sourceParent)
               .data(ProcessModel::SortRole).toString().contains(query_, Qt::CaseInsensitive)
        || source->index(sourceRow, ProcessModel::Pid, sourceParent)
               .data(ProcessModel::SortRole).toString().contains(query_, Qt::CaseInsensitive);
}

bool ProcessViewModel::lessThan(const QModelIndex& left, const QModelIndex& right) const {
    const auto lhs = left.data(ProcessModel::SortRole);
    const auto rhs = right.data(ProcessModel::SortRole);
    const bool lhs_available = lhs.isValid();
    const bool rhs_available = rhs.isValid();
    if (lhs_available != rhs_available) {
        // Qt reverses comparisons for descending order; unavailable stays last.
        return sortOrder() == Qt::AscendingOrder ? lhs_available : !lhs_available;
    }
    if (lhs_available) {
        switch (left.column()) {
        case ProcessModel::Pid:
        case ProcessModel::Rss:
        case ProcessModel::Uid:
            if (lhs.toULongLong() != rhs.toULongLong())
                return lhs.toULongLong() < rhs.toULongLong();
            break;
        case ProcessModel::Cpu:
            if (lhs.toDouble() != rhs.toDouble()) return lhs.toDouble() < rhs.toDouble();
            break;
        default: {
            const auto ltext = lhs.toString();
            const auto rtext = rhs.toString();
            const auto folded = QString::compare(ltext, rtext, Qt::CaseInsensitive);
            if (folded != 0) return folded < 0;
            const auto exact = QString::compare(ltext, rtext, Qt::CaseSensitive);
            if (exact != 0) return exact < 0;
            break;
        }
        }
    }
    return left.data(ProcessModel::IdentityRole).toString()
        < right.data(ProcessModel::IdentityRole).toString();
}

void ProcessViewModel::selectRow(int proxyRow) {
    if (QThread::currentThread() != thread()) return;
    if (proxyRow < 0 || proxyRow >= count()) {
        clearSelection();
        return;
    }
    const auto selected = mapToSource(index(proxyRow, ProcessModel::Pid));
    const auto identity = selected.data(ProcessModel::IdentityRole).toString();
    if (identity.isEmpty()) {
        clearSelection();
        return;
    }
    const bool changedIdentity = selected_identity_ != identity;
    const bool changedRow = selected_row_ != proxyRow;
    selected_source_ = selected;
    selected_identity_ = identity;
    selected_row_ = proxyRow;
    updateSelectedDetails();
    if (changedIdentity || changedRow) emit selectionChanged();
}

void ProcessViewModel::sortBy(int column, bool descending) {
    if (QThread::currentThread() != thread() || column < 0 || column >= ProcessModel::ColumnCount)
        return;
    sort(column, descending ? Qt::DescendingOrder : Qt::AscendingOrder);
    synchronizeSelection();
}

void ProcessViewModel::clearSelection() {
    if (QThread::currentThread() != thread()) return;
    const bool hadSelection = !selected_identity_.isEmpty() || selected_row_ != -1;
    selected_source_ = QPersistentModelIndex{};
    selected_identity_.clear();
    selected_row_ = -1;
    updateSelectedDetails();
    if (hadSelection) emit selectionChanged();
}

void ProcessViewModel::updateSelectedDetails() {
    const auto* source = qobject_cast<const ProcessModel*>(sourceModel());
    const auto details = source && selected_source_.isValid()
        && selected_source_.data(ProcessModel::IdentityRole).toString() == selected_identity_
        ? source->details(selected_source_) : QVariantMap{};
    if (details != selected_details_) {
        selected_details_ = details;
        emit selectedDetailsChanged();
    }
}

void ProcessViewModel::synchronizeSelection() {
    if (selected_identity_.isEmpty()) return;
    if (!selected_source_.isValid()
        || selected_source_.data(ProcessModel::IdentityRole).toString() != selected_identity_) {
        clearSelection();
        return;
    }
    const auto visible = mapFromSource(selected_source_);
    // A hidden row has no selection; removing a filter never restores it implicitly.
    if (!visible.isValid()) {
        clearSelection();
        return;
    }
    const bool changedRow = selected_row_ != visible.row();
    selected_row_ = visible.row();
    updateSelectedDetails();
    if (changedRow) emit selectionChanged();
}

} // namespace night::ui
