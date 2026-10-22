#pragma once

#include <QStringList>

// Headless commands for scripts and agents: omotion check, frame, sheet, render, and so on.
bool isCliCommand(const QString &word);
QString cliSummary();
int runCli(const QStringList &arguments);
