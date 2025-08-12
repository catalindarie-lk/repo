#ifndef FOLDERS_H
#define FOLDERS_H

#include <stdbool.h>

bool FileExists(const char* fileName);
bool DriveExists(const char* drivePath);
bool CreateAbsoluteFolderRecursive(const char *absolutePathToCreate);
bool CreateRelativeFolderRecursive(const char *rootDirectory, const char *relativePath);
FILE* FopenRename(const char* in_fpath, char* out_fpath, size_t fpath_max_size, const char* mode);
void ReadSessionIDsInRoot(const char* rootPath, const char* sidFolder);

#endif