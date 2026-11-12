#pragma once

#include "ni_ght_si/process.hpp"

#include <QAbstractTableModel>
#include <QString>
#include <QVariantMap>

#include <memory>

namespace night::ui {

// GUI-thread adapter only. These identities are observational, never action handles.
class ProcessModel final : public QAbstractTableModel {
    Q_OBJECT
    Q_PROPERTY(qulonglong sequence READ sequence NOTIFY changed)
    Q_PROPERTY(QString quality READ quality NOTIFY changed)
    Q_PROPERTY(int count READ count NOTIFY changed)

public:
    enum Column { Pid, Name, Cpu, Rss, State, Uid, ColumnCount };
    enum Role { SortRole = Qt::UserRole + 1, IdentityRole };
    Q_ENUM(Column)
    Q_ENUM(Role)

    explicit ProcessModel(QObject* parent = nullptr);
    bool apply(std::shared_ptr<const ProcessSnapshot> snapshot);

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    // Retained snapshot fields only; this performs no operating-system reads.
    QVariantMap details(const QModelIndex& index) const;

    qulonglong sequence() const { return sequence_; }
    QString quality() const { return quality_; }
    int count() const { return static_cast<int>(rows_.size()); }

signals:
    void changed();

private:
    struct Row { QString identity; std::size_t source; };
    std::shared_ptr<const ProcessSnapshot> snapshot_;
    std::vector<Row> rows_;
    qulonglong sequence_{};
    QString quality_{QStringLiteral("warming")};
};

} // namespace night::ui
