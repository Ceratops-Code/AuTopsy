// Owned folder policy. Identities are opaque values supplied by the native
// adapter; no Track, project, XML, window or sample-engine types cross here.
#pragma once
#include <string>
#include <vector>

namespace TrackGroups {
struct GroupRow {
   std::string id, group;
   bool header{}, collapsed{};
};
struct GroupCreation {
   std::string id;
   std::vector<std::string> members, order;
};
bool IsGroupIdentity(const std::string &id);
void CheckGroupRows(const std::vector<GroupRow> &rows);
GroupCreation PlanTrackGroup(const std::vector<GroupRow> &rows,
   const std::vector<std::string> &selected, const std::string &id);
std::vector<std::string> ArrangeGroupRows(const std::vector<GroupRow> &rows);
std::vector<std::string> MoveGroupRows(const std::vector<GroupRow> &rows,
   const std::string &group, const std::string &before);
bool IsCollapsedGroupMember(const std::vector<GroupRow> &rows, const std::string &member);
}
