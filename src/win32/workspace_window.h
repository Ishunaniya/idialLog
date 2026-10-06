#pragma once
#include <windows.h>
#include "workspace_state.h"
#include "workspace_tools.h"

namespace dl {
void LoadWorkspaceRecords();
bool SaveWorkspaceRecords();
void ShowWorkspace(int page = 0);
void CloseWorkspaceWindows(bool reset = false);
void EnsureReviewWorkbench();
void RestoreWorkspaceSession(const WorkspaceSession& session);
void ShowWorkbenchIncident(const Outage& outage);
bool RouteWorkspaceMessage(MSG& message);
void EditManualReview(const std::string& key, const std::string& title);
std::string IncidentReviewKey(const Outage& outage);
std::string FindingReviewKey(const Finding& finding);
std::string WorkspaceReviewText();
void ImportEvidencePackage();
}  // namespace dl
