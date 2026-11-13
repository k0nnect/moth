#include "kept_panel.hpp"

#include <QAbstractTableModel>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>

#include "board.hpp"

// The kept values as the core last listed them. There are only a handful, so one read fills it.
// The same names in the same order are updated in place (selection stays); anything else is
// rebuilt. A typed value shows in its cell only once the core says it was written.
class KeptModel : public QAbstractTableModel {
public:
  enum Column { Name, Value, Kind, Note };
  struct Row { QString name, tag, shown, note; };

  explicit KeptModel(Board *board, QObject *parent = nullptr) : QAbstractTableModel(parent), board_(board) {}

  int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : int(rows_.size()); }
  int columnCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : 4; }
  QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override {
    if (!index.isValid() || index.row() >= rows_.size() || (role != Qt::DisplayRole && role != Qt::EditRole)) return {};
    return cell(rows_[index.row()], index.column());
  }
  QVariant headerData(int section, Qt::Orientation o, int role = Qt::DisplayRole) const override {
    static const QStringList heads = {"name", "value", "kind", "note"};
    if (o != Qt::Horizontal || role != Qt::DisplayRole || section < 0 || section >= heads.size()) return {};
    return heads[section];
  }
  Qt::ItemFlags flags(const QModelIndex &index) const override {
    Qt::ItemFlags f = QAbstractTableModel::flags(index);
    if (index.isValid() && index.column() == Value && !board_->working()) f |= Qt::ItemIsEditable;
    return f;
  }
  // `set <name> <text>`. False when nothing is sent (nothing typed, the value it shows, moth busy)
  bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override {
    if (role != Qt::EditRole || !index.isValid() || index.column() != Value || index.row() >= rows_.size()) return false;
    const QString text = value.toString().trimmed();
    const QString name = rows_[index.row()].name;
    if (text.isEmpty() || text == rows_[index.row()].shown || board_->working()) return false;
    QPointer<KeptModel> self(this);
    board_->run("set", {name, text}, {}, [self, name, text](const Reply &r) {
      if (self && r.ok) self->show(name, text);
    });
    return true;
  }

  QString nameAt(int row) const { return row >= 0 && row < rows_.size() ? rows_[row].name : QString(); }
  bool empty() const { return rows_.isEmpty(); }

  void clear() {
    if (rows_.isEmpty()) return;
    beginResetModel();
    rows_.clear();
    endResetModel();
  }

  // The list as the core gave it. The value cell of `editing` (a name, or "") is left alone.
  void load(const QJsonArray &list, const QString &editing) {
    QList<Row> fresh;
    for (const QJsonValue &v : list) {
      const QJsonObject o = v.toObject();
      fresh.append({o.value("name").toString(), o.value("tag").toString(), o.value("shown").toString(), o.value("note").toString()});
    }
    bool same = fresh.size() == rows_.size();
    for (int i = 0; same && i < fresh.size(); ++i) same = fresh[i].name == rows_[i].name;
    if (!same) {
      beginResetModel();
      rows_ = fresh;
      endResetModel();
      return;
    }
    for (int i = 0; i < fresh.size(); ++i)
      for (int c = Value; c <= Note; ++c) {
        if (c == Value && rows_[i].name == editing) continue;
        if (cell(rows_[i], c) == cell(fresh[i], c)) continue;
        (c == Value ? rows_[i].shown : c == Kind ? rows_[i].tag : rows_[i].note) = cell(fresh[i], c);
        emit dataChanged(index(i, c), index(i, c));
      }
  }

private:
  static QString cell(const Row &r, int column) {
    switch (column) {
      case Name: return r.name;
      case Value: return r.shown;
      case Kind: return r.tag;
      case Note: return r.note;
      default: return {};
    }
  }
  void show(const QString &name, const QString &text) {
    for (int i = 0; i < rows_.size(); ++i)
      if (rows_[i].name == name && rows_[i].shown != text) {
        rows_[i].shown = text;
        emit dataChanged(index(i, Value), index(i, Value));
      }
  }

  Board *board_;
  QList<Row> rows_;
};

// a table that can say whether a cell is being typed in (the view keeps that to itself)
class KeptTable : public QTableView {
public:
  using QTableView::QTableView;
  bool editing() const { return state() == EditingState; }
};

KeptPanel::KeptPanel(Board *board, QWidget *parent) : QWidget(parent), board_(board) {
  setObjectName("keptPanel");
  model_ = new KeptModel(board, this);

  auto *column = new QVBoxLayout(this);
  auto *top = new QHBoxLayout;
  auto *title = new QLabel("kept", this);
  forget_ = new QPushButton("forget", this);
  forget_->setObjectName("forget");
  undo_ = new QPushButton("undo last change", this);
  undo_->setObjectName("undo");
  top->addWidget(title, 1);
  top->addWidget(forget_);
  top->addWidget(undo_);
  column->addLayout(top);

  view_ = new KeptTable(this);
  view_->setObjectName("kept");
  view_->setModel(model_);
  view_->setSelectionBehavior(QAbstractItemView::SelectRows);
  view_->setSelectionMode(QAbstractItemView::SingleSelection);
  view_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                         QAbstractItemView::AnyKeyPressed);
  view_->verticalHeader()->hide();
  view_->horizontalHeader()->setStretchLastSection(true);
  column->addWidget(view_, 1);

  nothing_ = new QLabel("nothing kept yet. select a result and press keep.", this);
  nothing_->setObjectName("nothing");
  nothing_->setTextFormat(Qt::PlainText);
  nothing_->setWordWrap(true);
  column->addWidget(nothing_);

  connect(forget_, &QPushButton::clicked, this, &KeptPanel::forgetSelected);
  connect(undo_, &QPushButton::clicked, this, [this] { board_->run("undo"); });
  connect(view_->selectionModel(), &QItemSelectionModel::selectionChanged, this, &KeptPanel::showState);
  connect(model_, &QAbstractItemModel::modelReset, this, &KeptPanel::showState);

  // read `kept` again; the cell being typed in keeps its text. With no game nothing is read.

  auto refresh = [this] {
    if (!board_->attached()) return;
    QPointer<KeptPanel> self(this);
    board_->read("kept", {}, {}, [self](const Reply &r) {
      if (!self || !r.ok || !self->board_->attached()) return;
      const QModelIndex cur = self->view_->currentIndex();
      const QString editing =
          self->view_->editing() ? self->model_->nameAt(cur.row()) : QString();
      self->model_->load(r.result.toArray(), editing);
      self->loaded_ = true;
      self->showState();
    });
  };
  connect(board, &Board::ticked, this, refresh);
  connect(board, &Board::turned, this, refresh);
  connect(board, &Board::changed, this, [this, refresh] {
    if (!board_->attached()) {
      loaded_ = false;
      model_->clear();
    } else if (!wasAttached_) {
      refresh();
    }
    wasAttached_ = board_->attached().has_value();
    showState();
  });
  wasAttached_ = board->attached().has_value();
  if (wasAttached_) refresh();
  showState();
}

void KeptPanel::showState() {
  const bool idle = !board_->working();
  forget_->setEnabled(idle && view_->selectionModel()->hasSelection());
  undo_->setEnabled(idle);
  const bool none = loaded_ && board_->attached() && model_->empty();
  nothing_->setVisible(none);
  view_->setVisible(!none);
}

void KeptPanel::forgetSelected() {
  const QModelIndexList picked = view_->selectionModel()->selectedRows();
  if (picked.isEmpty()) return;
  board_->run("forget", {model_->nameAt(picked.first().row())});
}
