#include "place_combo.hpp"

#include "modern_style.hpp"

#include <QAbstractItemView>
#include <QCompleter>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QStyledItemDelegate>

namespace wizz::app {

PlaceCombo::PlaceCombo(QWidget* parent) : QComboBox(parent)
{
    setEditable(true);
    setInsertPolicy(QComboBox::NoInsert);
    setMaxVisibleItems(kMaxDropDownItems);

    // SuggestAppend = the filtered popup (here) + the inline append (append_inline).
    auto* completer = new QCompleter(model(), this);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchStartsWith);
    completer->setCompletionMode(QCompleter::PopupCompletion);
    completer->setMaxVisibleItems(kMaxDropDownItems);
    setCompleter(completer);
    // Both lists draw their items through the style (hover, selection tint and pill).
    ModernStyle::use_styled_popup(*this);
    completer->popup()->setItemDelegate(new QStyledItemDelegate(completer->popup()));

    connect(lineEdit(), &QLineEdit::textEdited, this, &PlaceCombo::on_text_edited);
}

void PlaceCombo::set_suggestions(const QStringList& names)
{
    const QString text = currentText();
    {
        const QSignalBlocker blocker(this);
        clear();
        addItems(names);
        setCurrentIndex(-1);
    }
    setEditText(text);
    last_typed_ = text;
}

QStringList PlaceCombo::suggestions() const
{
    QStringList names;
    names.reserve(count());
    for (int i = 0; i < count(); ++i) {
        names.push_back(itemText(i));
    }
    return names;
}

void PlaceCombo::set_text(const QString& text)
{
    setEditText(text);
    last_typed_ = text;
}

QString PlaceCombo::append_completion(const QString& typed) const
{
    if (typed.isEmpty()) {
        return {};
    }
    for (int i = 0; i < count(); ++i) {
        const QString name = itemText(i);
        if (name.size() > typed.size() && name.startsWith(typed, Qt::CaseInsensitive)) {
            return name.sliced(typed.size());
        }
    }
    return {};
}

void PlaceCombo::on_text_edited(const QString& text)
{
    // Append only while the user adds characters at the end (not after Backspace,
    // Delete or an edit in the middle), like the Windows autocomplete.
    const bool grew = text.size() > last_typed_.size();
    last_typed_ = text;
    if (!grew || lineEdit()->cursorPosition() != text.size()) {
        return;
    }
    // After the completer has filtered its popup by the typed prefix.
    QMetaObject::invokeMethod(this, [this, text] { append_inline(text); }, Qt::QueuedConnection);
}

void PlaceCombo::append_inline(const QString& typed)
{
    QLineEdit* edit = lineEdit();
    if (edit == nullptr || edit->text() != typed) {
        return;  // the user typed on in the meantime
    }
    const QString rest = append_completion(typed);
    if (rest.isEmpty()) {
        return;
    }
    edit->setText(typed + rest);
    edit->setSelection(static_cast<int>(typed.size()), static_cast<int>(rest.size()));
}

} // namespace wizz::app
