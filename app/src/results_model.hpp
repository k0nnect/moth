#pragma once

#include <QAbstractTableModel>
#include <QList>
#include <QSet>
#include <QString>
#include <functional>

#include "core_client.hpp"

class Board;

// The hunt's results, read from the core a page at a time and never all at once: the first page
// when the list is loaded, the next as the view scrolls to the end, up to 20,000 rows. With no
// game, no hunt, or a snapshot no step has narrowed, it holds no rows and reads nothing. It loads
// itself when a hunt becomes listable (such as when the window opens mid-hunt), and tries again on
// a tick when a load could not be sent. After an event it is checked (revalidate), and kept when
// it is still the same list.
//
// A row's value can be typed over and a row kept. The write names the row's number, the address
// and kind it showed (`at`, `kind`) and `all` when the rarer forms are shown, so the core finds
// that row afresh as this list numbers it and refuses the write if it is anywhere else now. On
// any error from the core the list is read again; a write not sent (moth is busy) leaves it as
// it is.
//
// The value cell of the row being typed in (see setEditedRow) is left as it is when its page is
// read again, so a value the game changes meanwhile does not replace what is being typed.
class ResultsModel : public QAbstractTableModel {
  Q_OBJECT
public:
  enum Column { Number, Value, Kind, Address };
  static constexpr int page = 200;    // rows per read: the most `results -n` takes
  static constexpr int cap = 20000;   // rows it holds at most

  explicit ResultsModel(Board *board, QObject *parent = nullptr);

  int rowCount(const QModelIndex &parent = {}) const override;
  int columnCount(const QModelIndex &parent = {}) const override;
  QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
  Qt::ItemFlags flags(const QModelIndex &index) const override;
  // typing over a value: `set <#n> <text>` with `at`, `kind` and `all`. False when nothing is sent
  // (nothing typed, the number it shows, moth busy: Board::run says so). The cell shows the new
  // value only once the core says it was written
  bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;
  bool canFetchMore(const QModelIndex &parent) const override;
  void fetchMore(const QModelIndex &parent) override;

  void reload();                                 // forgets its rows and reads the first page
  // after an event (the panel calls it on Board::turned, with the rows in view): page 1 and those
  // rows' pages are read again; the same total and the same addresses there keep the list, its
  // selection and its place, with values updated in place; anything else loads it from row 1
  void revalidate(int firstRow, int lastRow);
  void refresh(int firstRow, int lastRow);       // reads those rows' pages again, values in place
  void keep(int row, const QString &name);       // `keep <#n> [name]` with `at`, `kind` and `all`
  void setAll(bool all);                         // the rarer forms too (`all`); reloads
  // the row whose value is being typed in, or -1 when none: asked whenever a page comes back
  void setEditedRow(std::function<int()> row);
  bool all() const;
  qint64 total() const;     // how many the core says the list has
  bool listed() const;      // a page of this list has come back
  QString error() const;    // the core's error for the last read of this list, or ""
  bool writing() const;     // a set or keep of a row has been sent and not yet answered
signals:
  // total(), listed(), error() or writing() changed, or rows were added
  void listChanged();

private:
  struct Row { qint64 n; QString addr, kind, tag, shown; };

  bool listable() const;
  void follow();
  void clear();
  void readPage(qint64 from);
  void took(qint64 from, bool all, const Reply &r);
  void apply(const QJsonObject &listing);
  void put(int i, const Row &row);
  bool act(int row, const QString &cmd, const QStringList &more, std::function<void(const Reply &)> written);

  Board *board_;
  QList<Row> rows_;          // rows 1.. of the list, in order
  qint64 total_ = 0;
  bool all_ = false;
  bool listed_ = false;
  bool following_ = false;   // the hunt was listable when last seen
  bool writing_ = false;
  int generation_ = 0;       // bumps whenever the rows are forgotten
  QString error_;
  QSet<qint64> wanted_;      // the pages (by their first row) this list waits for
  std::function<int()> editedRow_;
};
