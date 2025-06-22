#include <stdio.h>
#include <time.h>
#include "logging.h"

void log_event(event_type_t event_type, const char* event_message) {

    time_t current_time;
    time(&current_time);
    struct tm* utc_time = gmtime(&current_time);

    const char* event_str = NULL;
    // const char* level_str = (event_type == LOG_INFO)  ? "INFO" :
    //                          (event_type == LOG_DEBUG) ? "DEBUG" :
    //                          (event_type == LOG_ERROR) ? "ERROR" : "UNKNOWN";
    if (event_type == LOG_INFO){
        event_str = "INFO";
    } else if(event_type == LOG_DEBUG){
        event_str = "DEBUG";
    } else if(event_type == LOG_ERROR){
        event_str = "ERROR";
    } else {
        event_str = "UNKNOWN";
    }
   
    fprintf(stderr,"[%s] [UTC %04d-%02d-%02d %02d:%02d:%02d] - %s\n",
        event_str,
        utc_time->tm_year + 1900,
        utc_time->tm_mon + 1,
        utc_time->tm_mday,
        utc_time->tm_hour,
        utc_time->tm_min,
        utc_time->tm_sec,
        event_message);
}

int main(){

log_event(LOG_INFO,"fgdshjgksdjgl;kdsfjglkjdf");



}
