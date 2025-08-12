#ifndef LOGGING_H
#define LOGGING_H


#include <stdint.h>    // For fixed-width integer types
#include <stdio.h>     // For printf and fprintf
#include <string.h>    // For string manipulation functions
#include <windows.h>

#include "include/protocol_frames.h"
#include "include/netendians.h" // For network byte order functions

#ifndef RET_VAL_SUCCESS
#define RET_VAL_SUCCESS 0
#endif
#ifndef RET_VAL_ERROR
#define RET_VAL_ERROR -1
#endif











































// typedef uint8_t LogType;
// enum LogType{
//     LOG_FRAME_RECV = 1,
//     LOG_FRAME_SENT = 2
// };

// Log frame to file
// void log_frame(uint8_t log_type, UdpFrame *frame, const struct sockaddr_in *addr, const char *file_path){

//     if(frame == NULL){
//         fprintf(stderr, "No frame to log!\n");
//         return;
//     }
//     if(file_path == NULL){
//         fprintf(stderr, "Invalid log file pointer address!\n");
//         return;
//     }
//     if(strlen(file_path) == 0){
//         fprintf(stderr, "Invalid log file name!\n");
//         return;
//     }

//     char str_addr[INET_ADDRSTRLEN];
//     inet_ntop(AF_INET, &addr->sin_addr, str_addr, INET_ADDRSTRLEN);
//     uint16_t port = _ntohs(addr->sin_port);

//     FILE *file = fopen(file_path, "ab");

//     time_t current_time;
//     time(&current_time);
//     struct tm* utc_time = gmtime(&current_time);

//     char buffer[255];

//     if (log_type == LOG_FRAME_RECV){
//         snprintf(buffer, sizeof(buffer), "Received frame from %s:%d - [UTC %04d-%02d-%02d %02d:%02d:%02d]\0", 
//                         str_addr, port, utc_time->tm_year + 1900, utc_time->tm_mon + 1, utc_time->tm_mday,
//                         utc_time->tm_hour, utc_time->tm_min, utc_time->tm_sec);
//     } else if(log_type == LOG_FRAME_SENT) {

//         snprintf(buffer, sizeof(buffer), "Sent frame to %s:%d - [UTC %04d-%02d-%02d %02d:%02d:%02d]\0", 
//                         str_addr, port, utc_time->tm_year + 1900, utc_time->tm_mon + 1, utc_time->tm_mday,
//                         utc_time->tm_hour, utc_time->tm_min, utc_time->tm_sec);       
//     } else {
//         fprintf(stdout, "Invalid Log Type!\n");
//     }
//     //-----------------------------------
//     switch(frame->header.frame_type){
//         case FRAME_TYPE_ACK:
//             fprintf(file,"%s\n   FRAME_TYPE_ACK\n   Seq Num: %llu\n   Session ID: %d\n   Checksum: %d\n",
//                                                     buffer,                                           
//                                                     _ntohll(frame->header.seq_num), 
//                                                     _ntohl(frame->header.session_id), 
//                                                     _ntohl(frame->header.checksum));
//             break;
//         case FRAME_TYPE_KEEP_ALIVE:
//             fprintf(file,"%s\n   FRAME_TYPE_KEEP_ALIVE\n   Seq Num: %llu\n   Session ID: %d\n   Checksum: %d\n",
//                                                     buffer,                                           
//                                                     _ntohll(frame->header.seq_num), 
//                                                     _ntohl(frame->header.session_id), 
//                                                     _ntohl(frame->header.checksum));
//             break;
//         case FRAME_TYPE_CONNECT_REQUEST:
//             fprintf(file, "%s\n   FRAME_TYPE_CONNECT_REQUEST\n   Seq Num: %llu\n   Session ID: %d\n   Checksum: %d\n   Client ID: %d\n   Flags: %d\n   Client Name: %s\n", 
//                                                     buffer,
//                                                     _ntohll(frame->header.seq_num), 
//                                                     _ntohl(frame->header.session_id), 
//                                                     _ntohl(frame->header.checksum),
//                                                     _ntohl(frame->payload.request.client_id), 
//                                                     _ntohl(frame->payload.request.flag), frame->payload.request.client_name);
//             break;
//         case FRAME_TYPE_CONNECT_RESPONSE:
//             fprintf(file, "%s\n   FRAME_TYPE_CONNECT_RESPONSE\n   Seq Num: %llu\n   Session ID: %d\n   Checksum: %d\n   Session Timeout: %d\n   Sever Status: %d\n   Server Name: %s\n", 
//                                                     buffer,
//                                                     _ntohll(frame->header.seq_num), 
//                                                     _ntohl(frame->header.session_id), 
//                                                     _ntohl(frame->header.checksum),
//                                                     _ntohl(frame->payload.response.session_timeout), 
//                                                     frame->payload.response.server_status, 
//                                                     frame->payload.response.server_name);
//             break;
//         case FRAME_TYPE_FILE_METADATA:
//             fprintf(file, "%s   FRAME_TYPE_FILE_METADATA\n   Seq Num: %llu\n   Session ID: %d\n   Checksum: %d\n   File ID: %d\n   File Size: %d\n", 
//                                                     buffer,
//                                                     _ntohll(frame->header.seq_num), 
//                                                     _ntohl(frame->header.session_id), 
//                                                     _ntohl(frame->header.checksum),
//                                                     _ntohl(frame->payload.file_metadata.file_id), 
//                                                     _ntohl(frame->payload.file_metadata.file_size));                                                    
//                                                     break;
//         case FRAME_TYPE_FILE_FRAGMENT:
//             fprintf(file, "%s   FRAME_TYPE_FILE_FRAGMENT\n   Seq Num: %llu\n   Session ID: %d\n   Checksum: %d\n   File ID: %d\n   Current Fragment Size: %d\n   Fragment Offset: %d\n   Fragment Bytes: %s\n", 
//                                                     buffer,
//                                                     _ntohll(frame->header.seq_num), 
//                                                     _ntohl(frame->header.session_id), 
//                                                     _ntohl(frame->header.checksum),
//                                                     _ntohl(frame->payload.file_fragment.file_id), 
//                                                     _ntohl(frame->payload.file_fragment.size),
//                                                     _ntohl(frame->payload.file_fragment.offset),
//                                                     frame->payload.file_fragment.bytes);                                                    
//                                                     break;
//         case FRAME_TYPE_LONG_TEXT_MESSAGE:
//             fprintf(file, "%s   FRAME_TYPE_LONG_TEXT_MESSAGE\n   Seq Num: %llu\n   Session ID: %d\n   Checksum: %d\n   Message ID: %d\n   Total Length: %d\n   Fragment Length: %d\n   Fragment Offset: %d\n   Fragment Text: %s\n", 
//                                                     buffer,
//                                                     _ntohll(frame->header.seq_num), 
//                                                     _ntohl(frame->header.session_id), 
//                                                     _ntohl(frame->header.checksum),
//                                                     _ntohl(frame->payload.long_text_msg.message_id), 
//                                                     _ntohl(frame->payload.long_text_msg.message_len),
//                                                     _ntohl(frame->payload.long_text_msg.fragment_len),
//                                                     _ntohl(frame->payload.long_text_msg.fragment_offset), 
//                                                     frame->payload.long_text_msg.fragment_text);
//                                                     break;
//         case FRAME_TYPE_DISCONNECT:
//             fprintf(file, "%s\n   FRAME_TYPE_DISCONNECT\n   Seq Num: %llu\n   Session ID: %d\n   Checksum: %d\n", 
//                                                     buffer,
//                                                     _ntohll(frame->header.seq_num), 
//                                                     _ntohl(frame->header.session_id), 
//                                                     _ntohl(frame->header.checksum));
//             break;
//         default:
//             break;
//     }
//     fclose(file); // Close the file  
//     return;
// }
// // Create file to log frames
// void create_log_frame_file(uint8_t type, const uint32_t session_id, char buffer[]){

//     if(buffer == NULL){
//         fprintf(stdout, "Invalid buffer!\n");
//     }
//     buffer[0] = '\0';
    
//     char* log_folder = "E:\\logs\\";
//     char file_name[PATH_SIZE] = {0};
//     if(type == 0){
//         snprintf(file_name, PATH_SIZE, "srv_%d.txt", session_id);
//     } else {
//         snprintf(file_name, PATH_SIZE, "cli_%d.txt", session_id);
//     } 
    
    // if (CreateDirectory(log_folder, NULL)) {
    //     printf("Created folder '%s' for logs: \n", log_folder);
    // } else {
    //     DWORD folder_create_error = GetLastError();
    //     if (folder_create_error == ERROR_ALREADY_EXISTS) {
    //         //printf("Folder '%s' already existed from a previous run. Good for testing.\n", client_folder_path);
    //     } else {
    //         fprintf(stderr, "Error creating log folder: %lu\n", folder_create_error);
    //         return; // Exit if we can't even set up the test
    //     }
    // }

//     strncpy(buffer, log_folder, strlen(log_folder));
//     strncpy(buffer + strlen(log_folder), file_name, strlen(file_name));
//     buffer[strlen(log_folder) + strlen(file_name)] = '\0';

//     fprintf(stdout, "Session log file: %s\n", buffer);

//     FILE *file = fopen(buffer, "wb");
//     fclose(file);

//     return;

// }












#endif // LOGGING_H