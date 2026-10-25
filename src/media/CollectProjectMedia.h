// Native project/serialization boundary. Collection never edits live tracks.
#pragma once
#include <string>
class AudacityProject;
namespace ProjectMedia {
// XML adapters call these on the GUI thread. A scoped collection map writes
// portable references; ordinary saves retain relative paths inside the project.
std::string StoreVideoSourcePath(const AudacityProject *project, const std::string &path);
std::string StoreLocalVideoSourcePath(const AudacityProject *project, const std::string &path);
std::string OpenVideoSourcePath(const AudacityProject *project, const std::string &path,
   const std::string &localPath = {});
}
