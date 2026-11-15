#pragma once
// Base of the grid models whose rows have a stable key (the `Idx` column of
// 01 §8.2 / §9.2: the row's index into the view list). A change that reorders
// rows, or inserts rows at scattered sorted positions, is published as one
// layoutAboutToBeChanged/layoutChanged pair: every persistent index (the
// view's current cell, selection, hover) follows its row's key to the row's
// new display position — the way QSortFilterProxyModel re-sorts.

#include <QAbstractTableModel>
#include <QScopeGuard>

#include <cstddef>
#include <utility>
#include <vector>

namespace wizz::app {

class KeyedTableModel : public QAbstractTableModel {
public:
    using QAbstractTableModel::QAbstractTableModel;

protected:
    /// Runs `change` as one vertical layout change. `key_of(display_row)` is the key of a
    /// row; keys are below rowCount() after the change and never change for a row.
    template <class KeyOf, class Change>
    void relayout_by_key(const KeyOf& key_of, Change&& change)
    {
        Q_EMIT layoutAboutToBeChanged({}, QAbstractItemModel::VerticalSortHint);
        const QModelIndexList before = persistentIndexList();
        std::vector<std::size_t> keys;
        keys.reserve(static_cast<std::size_t>(before.size()));
        for (const QModelIndex& moved : before) {
            keys.push_back(key_of(moved.row()));
        }
        const auto finish = qScopeGuard([&] {
            const int rows = rowCount();
            std::vector<int> position(static_cast<std::size_t>(rows), -1);
            for (int row = 0; row < rows; ++row) {
                if (const std::size_t key = key_of(row); key < position.size()) {
                    position[key] = row;
                }
            }
            QModelIndexList after;
            after.reserve(before.size());
            for (qsizetype i = 0; i < before.size(); ++i) {
                const std::size_t key = keys[static_cast<std::size_t>(i)];
                const int row = key < position.size() ? position[key] : -1;
                after.push_back(row >= 0 ? index(row, before[i].column()) : QModelIndex{});
            }
            changePersistentIndexList(before, after);
            Q_EMIT layoutChanged({}, QAbstractItemModel::VerticalSortHint);
        });
        std::forward<Change>(change)();
    }

    /// Runs `change` inside beginResetModel/endResetModel (also when it throws).
    template <class Change>
    void reset_with(Change&& change)
    {
        beginResetModel();
        const auto end = qScopeGuard([this] { endResetModel(); });
        std::forward<Change>(change)();
    }
};

} // namespace wizz::app
