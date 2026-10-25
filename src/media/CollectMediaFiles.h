// Portable collection transaction. The caller owns an exclusive destination lock;
// native project saving and streaming fingerprints enter through narrow callbacks.
#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace ProjectMedia {
struct CollectionFile { std::filesystem::path source; std::string name, digest; };
struct CollectionRequest {
   std::filesystem::path destination;
   std::string documentIdentity;
   std::vector<CollectionFile> files;
};
using CollectionFingerprint = std::function<std::string(const std::filesystem::path&)>;
using CollectionObserver = std::function<void(const char*, const std::filesystem::path&, bool)>;
// Only destination/Project.aup3, Media/<name> and one manifest are produced. A complete
// matching copy is reused by integrity checks. Conflicting folders are untouched.
// A sibling _<name>.collecting tree is private staging, never a usable project;
// fresh calls discard only its marked orphan. Failure leaves originals intact.
std::filesystem::path CollectMediaFiles(const CollectionRequest &request,
   const CollectionFingerprint &fingerprint,
   const std::function<void(const std::filesystem::path&)> &saveProject,
   const CollectionObserver &observe = {});
}
