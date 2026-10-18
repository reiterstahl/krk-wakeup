#pragma once

#include <string>

void InitDiagnostics(const std::wstring& settingsPath);
bool SetDiagnosticsEnabled(bool enabled);
bool DiagnosticsEnabled();
void LogEvent(const char* event, const std::string& details = {});
std::wstring DiagnosticsPath();
std::wstring DiagnosticsDirectory();
