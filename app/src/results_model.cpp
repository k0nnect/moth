#include "results_model.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <algorithm>
#include <utility>

#include "board.hpp"

namespace {

const QStringList heads = {"#", "value", "kind", "address"};

// where a listing has row `n`, or "" when it hasn't
QString addressIn(const Reply &listing, qint64 n) {
  for (const QJsonValue &v : listing.result.toObject().value("rows").toArray())
    if (v.toObject().value("n").toInteger() == n) return v.toObject().value("addr").toString();
  return {};
}

}  // namespace

ResultsModel::ResultsModel(Board *board, QObject *parent) : QAbstractTableModel(parent), board_(board) {
  connect(board, &Board::changed, this, &ResultsModel::follow);
  // a load that was not sent (moth was working) or that failed is tried again; an error stays
  // shown until a reply replaces it
  connect(board, &Board::ticked, this, [this] {
    if (listable() && !listed_ && wanted_.isEmpty()) readPage(1);
  });
  follow();
}

int ResultsModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : int(rows_.size()); }
int ResultsModel::columnCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : int(heads.size()); }

QVariant ResultsModel::data(const QModelIndex &index, int role) const {
  if (!index.isValid() || index.row() >= rows_.size() || (role != Qt::DisplayRole && role != Qt::EditRole)) return {};
  const Row &r = rows_[index.row()];
  switch (index.column()) {
    case Number: return QString::number(r.n);
    case Value: return r.shown;
    case Kind: return r.tag;
    case Address: return r.addr;
    default: return {};
  }
}

QVariant ResultsModel::headerData(int section, Qt::Orientation orientation, int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole || section < 0 || section >= heads.size()) return {};
  return heads[section];
}

Qt::ItemFlags ResultsModel::flags(const QModelIndex &index) const {
  Qt::ItemFlags f = QAbstractTableModel::flags(index);
  if (index.isValid() && index.column() == Value && !writing_ && !board_->working()) f |= Qt::ItemIsEditable;
  return f;
}

bool ResultsModel::all() const { return all_; }
qint64 ResultsModel::total() const { return total_; }
bool ResultsModel::listed() const { return listed_; }
QString ResultsModel::error() const { return error_; }
bool ResultsModel::writing() const { return writing_; }

bool ResultsModel::listable() const {
  const auto &h = board_->hunt();
  return board_->attached().has_value() && h.has_value() && !h->blind;
}

// The hunt as the board has it now: a list that can't be listed is emptied at once (the game
// closed, the hunt ended, a snapshot was taken); one that has just become listable is loaded, as
// when the window opens mid-hunt. A step that changes the list is an event, which revalidates.
void ResultsModel::follow() {
  if (!listable()) {
    following_ = false;
    clear();
  } else if (!std::exchange(following_, true)) {
    reload();
  }
}

void ResultsModel::clear() {
  const bool had = !rows_.isEmpty() || listed_ || !error_.isEmpty();
  ++generation_;
  wanted_.clear();
  if (!rows_.isEmpty()) {
    beginResetModel();
    rows_.clear();
    endResetModel();
  }
  total_ = 0;
  listed_ = false;
  error_.clear();
  if (had) emit listChanged();
}

void ResultsModel::reload() {
  if (writing_) {  // once the write is answered
    owed_ = Owed::Reload;
    return;
  }
  clear();
  following_ = listable();
  if (following_) readPage(1);
}

void ResultsModel::revalidate(int firstRow, int lastRow) {
  if (writing_) {  // once the write is answered, over every range asked for meanwhile
    const bool first = owed_ == Owed::Nothing;
    owedFirst_ = first ? firstRow : std::min(owedFirst_, firstRow);
    owedLast_ = first ? lastRow : std::max(owedLast_, lastRow);
    owed_ = std::max(owed_, Owed::Check);
    return;
  }
  if (!listable()) {
    following_ = false;
    clear();
  } else if (!listed_) {
    reload();
  } else {
    readPage(1);  // a reply with another total, or a held row at another address, reloads
    refresh(firstRow, lastRow);
  }
}

void ResultsModel::setAll(bool all) {
  if (all == all_) return;
  all_ = all;
  reload();
}

// One page, from row `from`. Board::read does not send a read the same as one still out; the
// first one's reply comes instead, and whoever wants that page when it comes takes it, so a
// reload never waits for a reply that will not come. Not sent at all (moth is working): nothing
// waits, and the event that ends the work brings the list. Nothing is read while a write is under
// way: the core's last listing must stay the write's row.
void ResultsModel::readPage(qint64 from) {
  if (writing_ || wanted_.contains(from)) return;
  Opts opts{{"from", QString::number(from)}, {"n", QString::number(page)}};
  if (all_) opts.append(QPair<QString, QString>("all", "true"));
  QPointer<ResultsModel> self(this);
  const bool all = all_;
  if (board_->read("results", {}, opts, [self, from, all](const Reply &r) { if (self) self->took(from, all, r); }))
    wanted_.insert(from);
}

void ResultsModel::took(qint64 from, bool all, const Reply &r) {
  if (all != all_ || !wanted_.remove(from)) return;  // for a list this one no longer is
  if (!r.ok) {
    error_ = r.error;
    emit listChanged();
    return;
  }
  error_.clear();
  apply(r.result.toObject());
}

// A page's rows: the ones held are updated in place, and the ones that follow on are added, up to
// the cap. Another total, or a held row at another address, means another list: it is read again.
void ResultsModel::apply(const QJsonObject &listing) {
  const qint64 total = listing.value("total").toInteger();
  if (listed_ && total != total_) {
    reload();
    return;
  }
  QList<Row> more;
  for (const QJsonValue &v : listing.value("rows").toArray()) {
    const QJsonObject o = v.toObject();
    const Row row{o.value("n").toInteger(), o.value("addr").toString(), o.value("tag").toString(), o.value("shown").toString()};
    const qint64 i = row.n - 1;
    if (i >= 0 && i < rows_.size()) {
      if (rows_[i].addr != row.addr) {
        reload();
        return;
      }
      put(int(i), row);
    } else if (i == rows_.size() + more.size() && i < cap) {
      more.append(row);
    }
  }
  total_ = total;
  listed_ = true;
  if (!more.isEmpty()) {
    beginInsertRows({}, int(rows_.size()), int(rows_.size() + more.size() - 1));
    rows_.append(more);
    endInsertRows();
  }
  emit listChanged();
}

// a held row as the core lists it now: only the cells whose text changed are said to have
void ResultsModel::put(int i, const Row &row) {
  Row &held = rows_[i];
  if (held.shown != row.shown) {
    held.shown = row.shown;
    emit dataChanged(index(i, Value), index(i, Value));
  }
  if (held.tag != row.tag) {
    held.tag = row.tag;
    emit dataChanged(index(i, Kind), index(i, Kind));
  }
}

bool ResultsModel::canFetchMore(const QModelIndex &parent) const {
  return !parent.isValid() && listed_ && !writing_ && rows_.size() < std::min<qint64>(total_, cap) &&
         !wanted_.contains(rows_.size() + 1);
}

void ResultsModel::fetchMore(const QModelIndex &parent) {
  if (canFetchMore(parent)) readPage(rows_.size() + 1);
}

void ResultsModel::refresh(int firstRow, int lastRow) {
  if (rows_.isEmpty()) return;
  firstRow = std::max(firstRow, 0);
  lastRow = std::min(lastRow, int(rows_.size()) - 1);
  for (int p = firstRow / page; p <= lastRow / page && firstRow <= lastRow; ++p) readPage(qint64(p) * page + 1);
}

bool ResultsModel::setData(const QModelIndex &index, const QVariant &value, int role) {
  if (role != Qt::EditRole || !index.isValid() || index.column() != Value || index.row() >= rows_.size()) return false;
  const QString text = value.toString().trimmed();
  // an edit left as it was (a double click, then a click elsewhere) writes nothing
  if (text.isEmpty() || text == rows_[index.row()].shown) return false;
  const int i = index.row(), gen = generation_;
  QPointer<ResultsModel> self(this);
  return act(i, "set", {text}, [self, i, gen, text](const Reply &r) {
    // the cell shows what was written, unless the list has been read again since (and shows what
    // the game holds) or the game put the old value straight back
    if (!self || self->generation_ != gen || r.result.toObject().value("held") == QJsonValue(false)) return;
    Row &row = self->rows_[i];
    if (row.shown == text) return;
    row.shown = text;
    emit self->dataChanged(self->index(i, Value), self->index(i, Value));
  });
}

void ResultsModel::keep(int row, const QString &name) {
  act(row, "keep", name.isEmpty() ? QStringList{} : QStringList{name}, {});
}

// A write names its row by number, and the core looks that number up in the last list it gave
// out, which any read replaces (this window's own among them). So the row is listed alone first,
// and nothing else is read until the write is answered. A row that can't be listed (moth is
// working or starting) is not written: the line says why and the list is read again.
bool ResultsModel::act(int row, const QString &cmd, const QStringList &more, std::function<void(const Reply &)> written) {
  if (writing_ || row < 0 || row >= rows_.size()) return false;
  const Row target = rows_[row];
  Opts alone{{"from", QString::number(target.n)}, {"n", "1"}};
  if (all_) alone.append(QPair<QString, QString>("all", "true"));
  QPointer<ResultsModel> self(this);
  const bool asked = board_->read("results", {}, alone, [self, target, cmd, more, written](const Reply &r) {
    if (self) self->write(target, cmd, more, r, written);
  });
  if (!asked) {
    board_->say(board_->working() ? "moth is busy: " + board_->what() : QString::fromUtf8("moth is starting…"));
    reload();
    return false;
  }
  writing_ = true;
  emit listChanged();
  return true;
}

// The row as the core lists it now. Still at the address it was shown at: written, naming that
// address (`--at`), so the core refuses it if the list changes before the write lands. Anywhere
// else, or not listed, or moth now busy: not written, and the line says why.
void ResultsModel::write(const Row &target, const QString &cmd, const QStringList &more, const Reply &listed,
                         std::function<void(const Reply &)> written) {
  QString why;
  if (!listed.ok) why = listed.error;
  else if (addressIn(listed, target.n) != target.addr)
    why = QString("row %1 is not at %2 any more: the list changed. look again: moth results").arg(target.n).arg(target.addr);
  else if (board_->working()) why = "moth is busy: " + board_->what();
  if (!why.isEmpty()) {
    board_->say(why);
    finish(false);
    return;
  }
  QPointer<ResultsModel> self(this);
  board_->run(cmd, QStringList{QString::number(target.n)} + more, {{"at", target.addr}}, [self, written](const Reply &r) {
    if (!self) return;
    if (r.ok && written) written(r);
    self->finish(r.ok);
  });
}

// A write is over. Not written, or answered with an error: the list is read again. Written: what
// was asked for meanwhile (an event's check, a reload) is done now.
void ResultsModel::finish(bool ok) {
  writing_ = false;
  const Owed owed = std::exchange(owed_, Owed::Nothing);
  emit listChanged();
  if (!ok || owed == Owed::Reload) reload();
  else if (owed == Owed::Check) revalidate(owedFirst_, owedLast_);
}
