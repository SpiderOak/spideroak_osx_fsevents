#import <signal.h>
//#import <syslog.h>
#import <sys/param.h>
#import <unistd.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>

#define MAX_PATHS_TO_WATCH 1024
#define MAX_PATHS_TO_EXCLUDE 128
#define MAX_PATH_NAME_LENGTH 3072
#define STDIN_BUFFER_LENGTH 1024

typedef char PATH_NAME[MAX_PATH_NAME_LENGTH+1];

typedef struct {
    PATH_NAME   notification_path;
    size_t      exclude_len;
    PATH_NAME   exclude[MAX_PATHS_TO_EXCLUDE];
} FSEVENT_CALLBACK_INFO, *FSEVENT_INFO_P;

static void timer_callback(CFRunLoopTimerRef timer, void *info);

static void fsevent_callback( 
    ConstFSEventStreamRef           streamRef, 
    void *                          clientCallBackInfo, 
    int                             numEvents, 
    const char *const               eventPaths[], 
    const FSEventStreamEventFlags   eventFlags[], 
    const FSEventStreamEventId      eventIds[]
);

static CFArrayRef load_paths_to_watch(const char * file_path);
static void load_paths_to_exclude(
    FSEVENT_INFO_P info_p,
    const char * file_path
);
static void handleTERM(int signo);
static FILE * open_temp_file(const char * path);
static FILE * open_error_file();


static PATH_NAME  error_path;
static FILE *     error_file = NULL;

//-----------------------------------------------------------------------------
void timer_callback(CFRunLoopTimerRef timer, void *info) {
//-----------------------------------------------------------------------------
    if (getppid() == 1) {
        //syslog(LOG_NOTICE, "assuming calling process shut down");
        CFRunLoopStop(CFRunLoopGetMain());
    }
} // timer_callback

//-----------------------------------------------------------------------------
FILE * open_temp_file(const char * temp_path) {
//-----------------------------------------------------------------------------
    FILE * file = fopen(temp_path, "w");
    if (NULL == file) {
        /*
        syslog(LOG_ERR, "Failed to open %s: (%d) %s", 
            temp_path, errno, strerror(errno)
        );
        */
        error_file = open_error_file();
        fprintf(error_file, "Failed to open %s: (%d) %s\n", 
            temp_path, errno, strerror(errno)
        );
        fclose(error_file);
        exit(-1);
    }

    return file;
    
} // open_temp_file

//-----------------------------------------------------------------------------
FILE * open_error_file() {
//-----------------------------------------------------------------------------
    FILE * file = fopen(error_path, "w");
    if (NULL == file) {
        /*
        syslog(LOG_ERR, "Failed to open %s: (%d) %s", 
            error_path, errno, strerror(errno)
        );
        */
        exit(-2);
    }

    return file;
    
} // open_error_file

//-----------------------------------------------------------------------------
void fsevent_callback ( 
    ConstFSEventStreamRef           streamRef, 
    void *                          clientCallBackInfo, 
    int                             numEvents, 
    const char *const               eventPaths[], 
    const FSEventStreamEventFlags   eventFlags[], 
    const FSEventStreamEventId      eventIds[]
) {
//-----------------------------------------------------------------------------
    static char temp_path_buffer[MAX_PATH_NAME_LENGTH];
    static char notification_path_buffer[MAX_PATH_NAME_LENGTH];
    static int notification_count = 0;
    int i, j; 
    
    FSEVENT_INFO_P info_p = (FSEVENT_INFO_P) clientCallBackInfo;
    FILE * temp_file = NULL;
    sprintf(temp_path_buffer, "%s/temp", info_p->notification_path);

    int valid_events = 0;
    for (i=0; i<numEvents; i++) { 

        #if defined(DEBUG) 
        /* Ticket #1225: users have complained that this is too verbose */
        /* flags are unsigned long, IDs are uint64_t */ 
        /*
        syslog(LOG_NOTICE, "Notification change %llu in %s, flags %lu\n",
            eventIds[i], 
            eventPaths[i], 
            eventFlags[i]
        );
        */
        #endif
        
        // check for excluded path
        bool exclude = false;
        for (j=0; j < info_p->exclude_len; j++) { 
            int strncmp_result = strncmp(
                eventPaths[i], 
                info_p->exclude[j],
                strlen(info_p->exclude[j])
            );
            if (0 == strncmp_result) {
                /*
                syslog(LOG_NOTICE, "%s excluded by %s\n",
                    eventPaths[i],
                    info_p->exclude[j]
                ); 
                */
                exclude = true;
                break;
            }
        }
        
        if (!exclude) {
            if (NULL == temp_file) {
               temp_file = open_temp_file(temp_path_buffer);
            }
            if (fprintf(temp_file, "%s\n", eventPaths[i]) < 0) {
                /*
                syslog(LOG_ERR, "fprintf failed %s: (%d) %s", 
                    temp_path_buffer, errno, strerror(errno)
                );
                */
                error_file = open_error_file();
                fprintf(error_file, "fprintf failed %s: (%d) %s\n", 
                    temp_path_buffer, errno, strerror(errno)
                );
                fclose(error_file);
                exit(-3);
            }
            valid_events++;
        }
    } 
    
    if (temp_file != NULL) {
        fclose(temp_file);

        notification_count++;
        sprintf(
            notification_path_buffer, 
            "%s/%08d.txt", 
            (const char *) clientCallBackInfo,
            notification_count
        );
            
        if (rename(temp_path_buffer, notification_path_buffer) != 0) {
            /*
            syslog(LOG_ERR, "fsevent_callback rename failed %s %s (%d) %s",
                temp_path_buffer,
                notification_path_buffer,
                errno,
                strerror(errno)
            );
            */
            error_file = open_error_file();
            fprintf(error_file, "fsevent_callback rename failed %s %s (%d) %s\n",
                temp_path_buffer,
                notification_path_buffer,
                errno,
                strerror(errno)
            );
            fclose(error_file);
            exit(-4); 
        }
    }

} // fsevent_callback

//-----------------------------------------------------------------------------
CFArrayRef load_paths_to_watch(const char * file_path) {
//----------------------------------------------------------------------------- 
    // syslog(LOG_NOTICE, "opening %s", file_path);
    FILE *file = fopen(file_path, "r");
    if (NULL == file) {
        /*
        syslog(LOG_ERR, "Failed to open %s: (%d) %s", 
            file_path, errno, strerror(errno)
        );
        */
        error_file = open_error_file();
        fprintf(error_file, "Failed to open %s: (%d) %s\n", 
            file_path, errno, strerror(errno)
        );
        fclose(error_file);
        exit(-5);
    }
    
    PATH_NAME path_buffer;
    CFStringRef path_array[MAX_PATHS_TO_WATCH];
    int path_index = 0;
    int path_len = 0;
    while (1) {
        if (path_index >= MAX_PATHS_TO_WATCH) {
            // syslog(LOG_ERR, "Too many paths");
            error_file = open_error_file();
            fprintf(error_file, "Too many paths\n"); 
            fclose(error_file);
            exit(-6);
        }
        fgets(path_buffer, MAXPATHLEN+1, file);
        if (ferror(file)) {
            /*
            syslog(LOG_ERR, "Error reading %s: (%d) %s", 
                file_path, errno, strerror(errno)
            );
            */
            error_file = open_error_file();
            fprintf(error_file, "Error reading %s: (%d) %s\n", 
                file_path, errno, strerror(errno)
            );
            fclose(error_file);
            exit(-7);
        }
        if (feof(file)) {
            // otherwise EOF
            // syslog(LOG_NOTICE, "EOF reading %s", file_path); 
            break;
        }
        path_len = strlen(path_buffer);
        if (path_len < 2) {
            continue;
        }
        path_buffer[path_len-1] = '\0'; // wipe out newline
        path_array[path_index++] = CFStringCreateWithCString(
            kCFAllocatorDefault,
            path_buffer,
            kCFStringEncodingUTF8
        );
    }
    if (EOF == fclose(file)) {
        /*
        syslog(LOG_ERR, "Error closing %s: (%d) %s", 
            file_path, errno, strerror(errno)
        );
        */
    }    
    
    CFArrayRef paths_to_watch = CFArrayCreate(
        NULL,                       // allocator
        (const void **)path_array,  // values
        path_index,                 // number of values
        NULL                        // callbacks
    ); 
    if (NULL == paths_to_watch) {
        // syslog(LOG_ERR, "load_paths_to_array: CFArrayCreate failed");
        error_file = open_error_file();
        fprintf(
            error_file, 
            "load_paths_to_array: CFArrayCreate failed: (%d) %s\n",
            errno, 
            strerror(errno)
        );
        fclose(error_file);
        exit(-8); 
    }
    
    return paths_to_watch;
    
} // paths_to_watch

//-----------------------------------------------------------------------------
void load_paths_to_exclude(FSEVENT_INFO_P info_p, const char * file_path) {
//-----------------------------------------------------------------------------
    // syslog(LOG_NOTICE, "opening %s", file_path);
    FILE *file = fopen(file_path, "r");
    if (NULL == file) {
        /*
        syslog(LOG_ERR, "Failed to open %s: (%d) %s", 
            file_path, errno, strerror(errno)
        );
        */
        error_file = open_error_file();
        fprintf(error_file, "Failed to open %s: (%d) %s\n", 
            file_path, errno, strerror(errno)
        );
        fclose(error_file);
        exit(-9);
    }
    
    while (1) {
        if (info_p->exclude_len >= MAX_PATHS_TO_EXCLUDE) {
            // syslog(LOG_ERR, "Too many exclude paths");
            error_file = open_error_file();
            fprintf(error_file, "Too many exclude paths\n");
            fclose(error_file);
            exit(-10);
        }
        fgets(info_p->exclude[info_p->exclude_len], MAXPATHLEN, file);
        if (ferror(file)) {
            /*
            syslog(LOG_ERR, "Error reading %s: (%d) %s", 
                file_path, errno, strerror(errno)
            );
            */
            error_file = open_error_file();
            fprintf(error_file, "Error reading %s: (%d) %s\n", 
                file_path, errno, strerror(errno)
            );
            fclose(error_file);
            exit(-11);
        }
        if (feof(file)) {
            // otherwise EOF
            // syslog(LOG_NOTICE, "EOF reading %s", file_path); 
            break;
        }
        size_t path_len = strlen(info_p->exclude[info_p->exclude_len]);
        if (path_len > 1) {
            // wipe out newline
            info_p->exclude[info_p->exclude_len][path_len-1] = '\0'; 
        }
        info_p->exclude_len++;
    }
    if (EOF == fclose(file)) {
        /*
        syslog(LOG_ERR, "Error closing %s: (%d) %s", 
            file_path, errno, strerror(errno)
        );
        */
    }    
                   
} // load_paths_to_exclude
                   
//-----------------------------------------------------------------------------
void handleTERM(int signo) {
//-----------------------------------------------------------------------------
    // syslog(LOG_NOTICE, "SIGTERM");
    CFRunLoopStop(CFRunLoopGetMain());
} // handleKILL

//-----------------------------------------------------------------------------
int main (int argc, const char * argv[]) {
//-----------------------------------------------------------------------------
    openlog("SpiderOak FSEvents", LOG_NDELAY | LOG_CONS, LOG_DAEMON);
    //syslog(LOG_NOTICE, "Program starts");
    
    // expecting: argv[0] is executable
    //            argv[1] is the parent's PID
    //            argv[2] is path to config file listing directories to watch
    //            argv[3] is path to config file listing directories to exclude
    //            argv[4] is path to notification directory
    if (argc != 5) {
        // syslog(LOG_ERR, "Unexpected number of arguments %d", argc);
        return -1;
    }
    
    // we don't use argv[1], because we can call getppid(). We have to 
    // accept it because the windows fsevents program needs it.
    
    CFArrayRef paths_to_watch = load_paths_to_watch(argv[2]);
    if (0 == CFArrayGetCount(paths_to_watch)) {
        // syslog(LOG_NOTICE, "Program terminates: no paths to watch");
        return 0;
    }    
    
    FSEVENT_CALLBACK_INFO callback_info;
    bzero(&callback_info, sizeof callback_info);
    strncpy(callback_info.notification_path, argv[4], MAX_PATH_NAME_LENGTH);
    sprintf(
        error_path, 
        "%s/error.txt", 
        callback_info.notification_path
    );
    load_paths_to_exclude(&callback_info, argv[3]);    
    
    FSEventStreamContext context;
    bzero(&context, sizeof context);
    context.info = (void *) &callback_info;
    
    CFAbsoluteTime latency = 3.0; /* Latency in seconds */
    CFAbsoluteTime fire_date = CFAbsoluteTimeGetCurrent();
    CFTimeInterval interval = 3.0;
    
    /* Create the stream, passing in a callback, */ 
    FSEventStreamRef stream = FSEventStreamCreate(
        kCFAllocatorDefault, 
        (FSEventStreamCallback) fsevent_callback, 
        &context, 
        paths_to_watch, 
        kFSEventStreamEventIdSinceNow, /* Or a previous event ID */ 
        latency, 
        kFSEventStreamCreateFlagWatchRoot 
    ); 

    FSEventStreamScheduleWithRunLoop(
        stream, 
        CFRunLoopGetCurrent(),         
        kCFRunLoopDefaultMode
    ); 
    
    Boolean result = FSEventStreamStart(stream);
    if (!result) {
        // syslog(LOG_ERR, "FSEventStreamStart failed");
        error_file = open_error_file();
        fprintf(error_file, "FSEventStreamStart failed: (%d) %s\n", 
            errno, strerror(errno)
        );
        fclose(error_file);
        FSEventStreamInvalidate(stream);
        FSEventStreamRelease(stream);
        return -12;
    }

    CFRunLoopTimerRef timer = CFRunLoopTimerCreate(
        kCFAllocatorDefault,
        fire_date,
        interval,
        0, /* flags */
        0, /* order */
        (CFRunLoopTimerCallBack) timer_callback,
        NULL /* context */
    );
    
    CFRunLoopAddTimer(
        CFRunLoopGetCurrent(),         
        timer,
        kCFRunLoopDefaultMode
    );
    
    // break out of CFRunLoop on SIGTERM (kill TERM)
    signal(SIGTERM, handleTERM);
    
    // syslog(LOG_NOTICE, "Entering CFRunLoopRun");
    CFRunLoopRun();
    // syslog(LOG_NOTICE, "Exited CFRunLoopRun");
    
    FSEventStreamStop(stream);
    FSEventStreamInvalidate(stream);
    FSEventStreamRelease(stream);
    CFRelease(paths_to_watch);
    
    return 0;
} // main
