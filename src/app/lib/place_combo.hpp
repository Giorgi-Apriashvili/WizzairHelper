#pragma once
// The "From" / "To" place box (01 §4.4): an editable combo (WinForms DropDown)
// with MaxDropDownItems 20 whose drop-down items and autocomplete source are
// the catalog's suggestion names once the route map has loaded.
//
// Autocomplete emulates WinForms AutoCompleteMode.SuggestAppend: while the user
// types at the end of the text, a popup lists the suggestions that start with
// the typed text (case-insensitive) and the rest of the first one is appended
// inline and selected, so typing on replaces it and Backspace removes it. Free
// text is always allowed; Enter never inserts the text into the list.

#include <QComboBox>
#include <QString>
#include <QStringList>

namespace wizz::app {

class PlaceCombo final : public QComboBox {
    Q_OBJECT

public:
    static constexpr int kMaxDropDownItems = 20;

    explicit PlaceCombo(QWidget* parent = nullptr);

    /// Replaces the drop-down items and the autocomplete source; the typed text
    /// is kept (01 §11.2).
    void set_suggestions(const QStringList& names);
    [[nodiscard]] QStringList suggestions() const;

    [[nodiscard]] QString text() const { return currentText(); }
    void set_text(const QString& text);

    /// What SuggestAppend appends to `typed`: the rest of the first suggestion that
    /// starts with it (case-insensitive), or an empty string.
    [[nodiscard]] QString append_completion(const QString& typed) const;

private:
    void on_text_edited(const QString& text);
    void append_inline(const QString& typed);

    QString last_typed_;
};

} // namespace wizz::app
