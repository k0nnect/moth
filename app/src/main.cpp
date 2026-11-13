#include <QApplication>

#include "board.hpp"
#include "core_client.hpp"
#include "main_window.hpp"
#include "paths.hpp"

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  CoreClient core(mothHome(), mothBin());
  Board board(&core);
  MainWindow window(&board);
  window.show();
  return app.exec();
}
