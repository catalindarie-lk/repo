#ifndef CLIENT_API_H
#define CLIENT_API_H


int RequestConnect(const char *server_ip);
void RequestDisconnect();
void SendTextMessage(const char *buffer, const size_t len);
int SendTextInFile(const char *fpath, size_t fpath_len);
void SendSingleFile(const char *fpath, size_t len);
void SendAllFilesInFolder(const char *fd_path, size_t len);
void SendAllFilesInFolderAndSubfolders(const char *fd_path, size_t len);



#endif