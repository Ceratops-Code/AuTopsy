#include "EditTrackGroups.h"
#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace TrackGroups {
bool IsGroupIdentity(const std::string &id)
{
   return id.size() == 32 && id.find_first_not_of("0123456789abcdef") == std::string::npos;
}
void CheckGroupRows(const std::vector<GroupRow> &rows)
{
   if (rows.size() > 100000) throw std::runtime_error("Too many tracks for a track group layout.");
   std::unordered_set<std::string> identities;
   for (const auto &row : rows) {
      if (!IsGroupIdentity(row.id) || !identities.insert(row.id).second ||
          (!row.group.empty() && !IsGroupIdentity(row.group)) || (row.header && !row.group.empty()))
         throw std::runtime_error("Track group identities are missing, duplicated or nested.");
   }
}
std::vector<std::string> ArrangeGroupRows(const std::vector<GroupRow> &rows)
{
   CheckGroupRows(rows);
   std::unordered_set<std::string> headers;
   for (const auto &row : rows) if (row.header) headers.insert(row.id);
   std::vector<std::string> result;
   for (const auto &row : rows) {
      if (!row.group.empty() && headers.count(row.group)) continue;
      result.push_back(row.id);
      if (row.header) for (const auto &member : rows)
         if (member.group == row.id) result.push_back(member.id);
   }
   return result;
}
GroupCreation PlanTrackGroup(const std::vector<GroupRow> &rows,
   const std::vector<std::string> &selected, const std::string &id)
{
   CheckGroupRows(rows);
   if (!IsGroupIdentity(id) || std::any_of(rows.begin(), rows.end(), [&](const auto &row) { return row.id == id; }))
      throw std::runtime_error("Choose an unused track group identity.");
   const std::unordered_set<std::string> selection(selected.begin(), selected.end());
   if (selection.size() != selected.size() || selected.size() < 2)
      throw std::runtime_error("Select at least two different tracks to group.");
   auto changed = rows;
   GroupCreation result{id, {}, {}};
   size_t first = changed.size();
   for (size_t index{}; index < changed.size(); ++index) {
      auto &row = changed[index];
      if (!selection.count(row.id)) continue;
      if (row.header || !row.group.empty()) throw std::runtime_error("Ungroup the selected tracks before grouping them again.");
      first = std::min(first, index);
      result.members.push_back(row.id); row.group = id;
   }
   if (result.members.size() != selected.size()) throw std::runtime_error("A selected track is no longer present.");
   changed.insert(changed.begin() + first, {id, {}, true, false});
   result.order = ArrangeGroupRows(changed);
   return result;
}
std::vector<std::string> MoveGroupRows(const std::vector<GroupRow> &rows,
   const std::string &group, const std::string &before)
{
   const auto current = ArrangeGroupRows(rows);
   const auto header = std::find_if(rows.begin(), rows.end(), [&](const auto &row) { return row.header && row.id == group; });
   if (header == rows.end()) throw std::runtime_error("The track group is no longer present.");
   std::unordered_set<std::string> block{group};
   for (const auto &row : rows) if (row.group == group) block.insert(row.id);
   if (block.count(before)) return current;
   auto destination = before;
   if (!before.empty()) {
      const auto target = std::find_if(rows.begin(), rows.end(), [&](const auto &row) { return row.id == before; });
      if (target == rows.end()) throw std::runtime_error("The destination track is no longer present.");
      // A group cannot be inserted inside another group.
      if (!target->group.empty()) destination = target->group;
   }
   std::vector<std::string> moving, result;
   for (const auto &id : current) (block.count(id) ? moving : result).push_back(id);
   auto position = destination.empty() ? result.end() : std::find(result.begin(), result.end(), destination);
   result.insert(position, moving.begin(), moving.end());
   return result;
}
bool IsCollapsedGroupMember(const std::vector<GroupRow> &rows, const std::string &member)
{
   const auto found = std::find_if(rows.begin(), rows.end(), [&](const auto &row) { return row.id == member; });
   if (found == rows.end() || found->header || found->group.empty()) return false;
   return std::any_of(rows.begin(), rows.end(), [&](const auto &row) {
      return row.header && row.id == found->group && row.collapsed;
   });
}
}
