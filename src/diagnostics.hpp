#pragma once

#include <string>

void InitDiagnostics(const std::wstring& settingsPath);
void LogEvent(const char* event, const std::string& details = {});
const std::wstring& DiagnosticsPath();
