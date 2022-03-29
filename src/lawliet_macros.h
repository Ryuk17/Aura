/*
 * lawliet_macros.h
 *
 *  Created on: 2022年3月16日
 *      Author: ryuk
 */

#ifndef LAWLIET_MACROS_H_
#define LAWLIET_MACROS_H_

#include <stdio.h>

#define LAWLIET_MESSAGE 	("LAWLIET_MESSAGE")
#define LAWLIET_DEBUG		("LAWLIET_DEBUG")
#define LAWLIET_WARNING		("LAWLIET_WARNING")
#define LAWLIET_ERROR		("LAWLIET_ERROR")


#define LAWLIET_LOG(level, format, ...) \
        printf("[%s|%s@%s:%d] " format "\n", \
            level, __func__, __FILE__, __LINE__, ##__VA_ARGS__ )



#ifndef PI
#define PI 					(3.1415926535898)
#endif


#endif /* LAWLIET_MACROS_H_ */
