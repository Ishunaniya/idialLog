#pragma once
#include <windows.h>
#include "workspace_state.h"
namespace dl {
void LoadWorkspaceRecords();
bool SaveWorkspaceRecords();
void ShowWorkspace(int page=0);
void CloseWorkspaceWindows();
bool RouteWorkspaceMessage(MSG& message);
void EditManualReview(const std::string& key,const std::string& title);
std::string IncidentReviewKey(const Outage& outage);
std::string FindingReviewKey(const Finding& finding);
std::string WorkspaceReviewText();
void ImportEvidencePackage();
}
