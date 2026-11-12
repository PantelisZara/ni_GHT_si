#pragma once

#include "process_model.hpp"

#include <QPersistentModelIndex>
#include <QSortFilterProxyModel>

namespace night::ui {

// GUI-thread view state. Selection identifies an observation, never an action handle.
class ProcessViewModel final : public QSortFilterProxyModel {
    Q_OBJECT
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(QString selectedIdentity READ selectedIdentity NOTIFY selectionChanged)
    Q_PROPERTY(int selectedRow READ selectedRow NOTIFY selectionChanged)
    Q_PROPERTY(QVariantMap selectedDetails READ selectedDetails NOTIFY selectedDetailsChanged)

public:
    explicit ProcessViewModel(ProcessModel& source, QObject* parent = nullptr);

    QString query() const { return query_; }
    void setQuery(const QString& query);
    int count() const { return rowCount(); }
    QString selectedIdentity() const { return selected_identity_; }
    int selectedRow() const { return selected_row_; }
    QVariantMap selectedDetails() const { return selected_details_; }

    Q_INVOKABLE void selectRow(int proxyRow);
    Q_INVOKABLE void sortBy(int column, bool descending);
    Q_INVOKABLE void clearSelection();

signals:
    void queryChanged();
    void changed();
    void selectionChanged();
    void selectedDetailsChanged();

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;

private:
    void synchronizeSelection();
    void updateSelectedDetails();

    QString query_;
    QPersistentModelIndex selected_source_;
    QString selected_identity_;
    int selected_row_{-1};
    QVariantMap selected_details_;
};

} // namespace night::ui
