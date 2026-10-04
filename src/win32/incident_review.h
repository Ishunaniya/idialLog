#pragma once
#include <windows.h>
#include "log_types.h"
namespace dl {
constexpr UINT kIncidentReviewCommand=1180,kSignalGuideCommand=1181;
void ShowIncidentReview(const Outage& selected);
void CloseIncidentReview();
bool RouteIncidentReviewMessage(MSG& message);
}
