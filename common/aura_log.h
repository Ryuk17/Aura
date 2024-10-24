/*
 * @Author: Ryuk 
 * @Date: 2024-10-23 23:06:56 
 * @Last Modified by: Ryuk
 * @Last Modified time: 2024-10-24 22:36:50
 */


#ifndef AURA_LOG_H 
#define AURA_LOG_H 

#ifdef __cplusplus
extern "C"{
#endif

#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>


#define DEBUG_LEVEL (0)

#define AURA_ERR(format, ...) AURA_LOG (3, "Error", format, ##__VA_ARGS__)
#define AURA_WRN(format, ...) AURA_LOG (2, "Warning", format, ##__VA_ARGS__)
#define AURA_INF(format, ...) AURA_LOG (1, "Info", format, ##__VA_ARGS__)


static struct timeval    tv;
static struct tm         *tm_ptr;


#define AURA_LOG(level, tag, format, ...)                                           \
do {                                                                                \
    if(level>=DEBUG_LEVEL){                                                       \
        gettimeofday(&tv, NULL);                                                    \
        tm_ptr = localtime(&tv.tv_sec);                                             \
        DEBUG("[%d-%02d-%02d %02d:%02d:%02d.%ld]                                    \
                %s @ FUNC:%s FILE:%s LINE:%d \n" format "\n",                       \
                1900+tm_ptr->tm_year, 1+tm_ptr->tm_mon, tm_ptr->tm_mday,            \
                tm_ptr->tm_hour, tm_ptr->tm_min, tm_ptr->tm_sec, tv.tv_usec/1000,   \
                tag, __func__, __FILE__, __LINE__, ##__VA_ARGS__ );                 \
    }                                                                               \
} while (0);




#ifdef __cplusplus
}
#endif

#endif	/* AURA_LOG_H */
