#ifndef BABY_PROTOCOL_H
#define BABY_PROTOCOL_H

#define SERVER      1
    #if SERVER
        #define BABY_ROLE_SERVER 1
        #define BABY_ROLE_CLIENT 0
    #else
        #define BABY_ROLE_SERVER 0
        #define BABY_ROLE_CLIENT 1
    #endif

#endif