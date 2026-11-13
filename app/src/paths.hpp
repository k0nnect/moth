#pragma once

#include <QString>

// moth's home, where its socket and logs are: MOTH_HOME made absolute, else
// ~/Library/Application Support/moth (the rule the moth command uses)
QString mothHome();

// the moth command: MOTH_BIN, else moth next to the running program
QString mothBin();
