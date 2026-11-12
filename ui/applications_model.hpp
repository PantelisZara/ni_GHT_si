#pragma once
#include "ni_ght_si/process.hpp"
#include <QAbstractTableModel>
#include <memory>

namespace night::ui {
class ApplicationsModel final : public QAbstractTableModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(int unclassified READ unclassified NOTIFY changed)
    Q_PROPERTY(int unavailable READ unavailable NOTIFY changed)
    Q_PROPERTY(QString quality READ quality NOTIFY changed)
    Q_PROPERTY(qint64 dataAgeMs READ dataAgeMs NOTIFY changed)
public:
    enum Column { Scope, Processes, Cpu, Rss, ColumnCount };
    explicit ApplicationsModel(QObject* parent = nullptr);
    bool apply(std::shared_ptr<const ProcessSnapshot> snapshot);
    void refreshAge(Clock::time_point now, std::chrono::milliseconds maximum_age = std::chrono::milliseconds{1500});
    int count() const { return static_cast<int>(groups_.size()); }
    int unclassified() const { return unclassified_; }
    int unavailable() const { return unavailable_; }
    QString quality() const {
        return age_ > maximum_age_.count() && (quality_ == QStringLiteral("valid") || quality_ == QStringLiteral("partial"))
            ? QStringLiteral("stale") : quality_;
    }
    qint64 dataAgeMs() const { return age_; }
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Q_INVOKABLE QString unitAtRow(int row) const;
    Q_INVOKABLE int rowForUnit(const QString& unit) const;
    Q_INVOKABLE QString seedPidForUnit(const QString& unit) const;
signals:
    void changed();
private:
    struct Group {
        std::string key;
        QString name;
        std::uint64_t members{}, rss{}, seed_pid{};
        double cpu{};
        bool cpu_valid{true}, rss_valid{true};
    };
    std::vector<Group> groups_;
    std::uint64_t sequence_{};
    int unclassified_{}, unavailable_{};
    QString quality_{QStringLiteral("warming")};
    std::optional<Clock::time_point> data_end_;
    qint64 age_{-1};
    std::chrono::milliseconds maximum_age_{1500};
};
}
