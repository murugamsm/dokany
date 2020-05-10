#include "filenodes.h"

#include <sddl.h>
#include <spdlog/spdlog.h>

namespace memfs {
fs_filenodes::fs_filenodes() {
  WCHAR buffer[1024];
  WCHAR final_buffer[2048];
  PTOKEN_USER user_token = NULL;
  PTOKEN_GROUPS groups_token = NULL;
  HANDLE token_handle;
  LPTSTR user_sid_str = NULL, group_sid_str = NULL;

  if (OpenProcessToken(GetCurrentProcess(), TOKEN_READ, &token_handle) ==
      FALSE) {
    throw std::runtime_error("Failed init root resources");
  }

  DWORD return_length;
  if (!GetTokenInformation(token_handle, TokenUser, buffer, sizeof(buffer),
                           &return_length)) {
    CloseHandle(token_handle);
    throw std::runtime_error("Failed init root resources");
  }

  user_token = (PTOKEN_USER)buffer;
  if (!ConvertSidToStringSid(user_token->User.Sid, &user_sid_str)) {
    CloseHandle(token_handle);
    throw std::runtime_error("Failed init root resources");
  }

  if (!GetTokenInformation(token_handle, TokenGroups, buffer, sizeof(buffer),
                           &return_length)) {
    CloseHandle(token_handle);
    throw std::runtime_error("Failed init root resources");
  }

  groups_token = (PTOKEN_GROUPS)buffer;
  if (groups_token->GroupCount > 0) {
    if (!ConvertSidToStringSid(groups_token->Groups[0].Sid, &group_sid_str)) {
      CloseHandle(token_handle);
      throw std::runtime_error("Failed init root resources");
    }
    swprintf_s(buffer, 1024, L"O:%lsG:%ls", user_sid_str, group_sid_str);
  } else
    swprintf_s(buffer, 1024, L"O:%ls", user_sid_str);

  LocalFree(user_sid_str);
  LocalFree(group_sid_str);
  CloseHandle(token_handle);

  swprintf_s(final_buffer, 2048, L"%lsD:PAI(A;OICI;FA;;;AU)", buffer);

  PSECURITY_DESCRIPTOR security_descriptor = NULL;
  ULONG size = 0;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptor(
          final_buffer, SDDL_REVISION_1, &security_descriptor, &size))
    throw std::runtime_error("Failed init root resources");

  auto fileNode = std::make_shared<filenode>(L"\\", true,
                                             FILE_ATTRIBUTE_DIRECTORY, nullptr);
  fileNode->security.SetDescriptor(security_descriptor);
  LocalFree(security_descriptor);

  _filenodes[L"\\"] = fileNode;
  _directoryPaths.emplace(L"\\", std::set<std::shared_ptr<filenode>>());
}

NTSTATUS fs_filenodes::add(const std::shared_ptr<filenode>& f) {
  std::lock_guard<std::recursive_mutex> lock(_filesnodes_mutex);

  if (f->fileindex == 0)  // previous init
    f->fileindex = _fs_fileindex_count++;
  const auto filename = f->get_filename();
  const auto parent_path =
      std::filesystem::path(filename).parent_path().wstring();

  // Does target folder exist
  if (!_directoryPaths.count(parent_path)) {
    spdlog::warn(L"Add: No directory: {} exist FilePath: {}", parent_path,
                 filename);
    return STATUS_OBJECT_PATH_NOT_FOUND;
  }

  auto stream_names = get_stream_names(filename);
  if (!stream_names.second.empty()) {
    spdlog::info(
        L"Add file: {} is an alternate stream {} and has {} as main stream",
        filename, stream_names.second, stream_names.first);
    auto main_f = find(parent_path + stream_names.first);
    if (!main_f) return STATUS_OBJECT_PATH_NOT_FOUND;
    main_f->add_stream(f);
    f->main_stream = main_f;
    f->fileindex = main_f->fileindex;
  }

  // If we have a folder, we add it to our directoryPaths
  if (f->is_directory && !_directoryPaths.count(filename))
    _directoryPaths.emplace(filename, std::set<std::shared_ptr<filenode>>());

  // Add our file to the fileNodes and directoryPaths
  _filenodes[filename] = f;
  _directoryPaths[parent_path].insert(f);

  spdlog::info(L"Add file: {} in folder: {}", filename, parent_path);
  return STATUS_SUCCESS;
}

std::shared_ptr<filenode> fs_filenodes::find(const std::wstring& filename) {
  std::lock_guard<std::recursive_mutex> lock(_filesnodes_mutex);
  auto fileNode = _filenodes.find(filename);
  return (fileNode != _filenodes.end()) ? fileNode->second : nullptr;
}

std::set<std::shared_ptr<filenode>> fs_filenodes::list_folder(
    const std::wstring& fileName) {
  std::lock_guard<std::recursive_mutex> lock(_filesnodes_mutex);

  auto it = _directoryPaths.find(fileName);
  return (it != _directoryPaths.end()) ? it->second
                                       : std::set<std::shared_ptr<filenode>>();
}

void fs_filenodes::remove(const std::wstring& filename) {
  return remove(find(filename));
}

void fs_filenodes::remove(const std::shared_ptr<filenode>& f) {
  if (!f) return;

  std::lock_guard<std::recursive_mutex> lock(_filesnodes_mutex);
  auto fileName = f->get_filename();
  spdlog::info(L"Remove: {}", fileName);

  // Remove node from fileNodes and directoryPaths
  _filenodes.erase(fileName);
  _directoryPaths[std::filesystem::path(fileName).parent_path()].erase(
      f);

  // if it was a directory we need to remove it from directoryPaths
  if (f->is_directory) {
    // but first we need to remove the directory content by looking recursively
    // into it
    auto files = list_folder(fileName);
    for (const auto& file : files) remove(file);

    _directoryPaths.erase(fileName);
  }

  // Cleanup streams
  if (f->main_stream) {
    // Is an alternate stream
    f->main_stream->remove_stream(f);
  } else {
    // Is a main stream
    // Remove possible alternate stream
    auto streams = f->get_streams();
    for (const auto& stream : streams) remove(stream.first);
  }
}

NTSTATUS fs_filenodes::move(std::wstring old_filename, std::wstring new_filename,
                         BOOL replace_if_existing) {
  auto f = find(old_filename);
  auto new_f = find(new_filename);

  if (!f) return STATUS_OBJECT_NAME_NOT_FOUND;

  // Cannot move to an existing destination without replace flag
  if (!replace_if_existing && new_f) return STATUS_OBJECT_NAME_COLLISION;

  // Cannot replace read only destination
  if (new_f && new_f->attributes & FILE_ATTRIBUTE_READONLY)
    return STATUS_ACCESS_DENIED;

  // If destination exist - Cannot move directory or replace a directory
  if (new_f && (f->is_directory || new_f->is_directory))
    return STATUS_ACCESS_DENIED;

  auto newParent_path =
      std::filesystem::path(new_filename).parent_path().wstring();

  std::lock_guard<std::recursive_mutex> lock(_filesnodes_mutex);
  if (!_directoryPaths.count(newParent_path)) {
    spdlog::warn(L"Move: No directory: {} exist FilePath: {}", newParent_path,
                 new_filename);
    return STATUS_OBJECT_PATH_NOT_FOUND;
  }

  // Remove destination
  remove(new_f);

  // Update current node with new data
  const auto fileName = f->get_filename();
  auto oldParentPath = std::filesystem::path(fileName).parent_path();
  f->set_filename(new_filename);

  // Move fileNode
  // 1 - by removing current not with oldName as key
  add(f);

  // 2 - If fileNode is a Dir we move content to destination
  if (f->is_directory) {
    // recurse remove sub folders/files
    auto files = list_folder(old_filename);
    for (const auto& file : files) {
      const auto fileName = file->get_filename();
      auto newSubFileName =
          std::filesystem::path(new_filename)
              .append(std::filesystem::path(fileName).filename().wstring())
              .wstring();
      auto n = move(fileName, newSubFileName, replace_if_existing);
      if (n != STATUS_SUCCESS) {
        spdlog::warn(
            L"Move: Subfolder file move {} to {} replaceIfExisting {} failed: "
            L"{}",
            fileName, newSubFileName, replace_if_existing, n);
        return n;  // That's bad...we have not done a full move
      }
    }

    // remove folder from directories
    _directoryPaths.erase(old_filename);
  }

  // 3 - Remove fileNode link with oldFilename
  _filenodes.erase(old_filename);
  if (oldParentPath != newParent_path)  // Same folder destination
    _directoryPaths[oldParentPath].erase(f);

  spdlog::info(L"Move file: {} to folder: {}", old_filename, new_filename);
  return STATUS_SUCCESS;
}

std::pair<std::wstring, std::wstring> fs_filenodes::get_stream_names(
    std::wstring filename) {
  // real_fileName - foo or foo:bar
  const auto real_fileName =
      std::filesystem::path(filename).filename().wstring();
  auto stream_pos = real_fileName.find(L":");
  // foo does not have alternated stream, return an empty alternated stream.
  if (stream_pos == std::string::npos)
    return std::pair<std::wstring, std::wstring>(real_fileName, std::wstring());

  // foo:bar has an alternated stream
  // return first the file name and second the file stream name
  // first: foo - second: bar
  const auto main_stream = real_fileName.substr(0, stream_pos);
  ++stream_pos;
  const auto alternate_stream =
      real_fileName.substr(stream_pos, real_fileName.length() - stream_pos);
  return std::pair<std::wstring, std::wstring>(main_stream, alternate_stream);
}
}  // namespace memfs