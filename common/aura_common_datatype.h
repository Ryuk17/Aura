/*
 * @Author: Ryuk 
 * @Date: 2024-10-23 23:06:31 
 * @Last Modified by:   Ryuk 
 * @Last Modified time: 2024-10-23 23:06:31 
 */


#ifndef AURA_COMMON_DATATYPE_H 
#define AURA_COMMON_DATATYPE_H 

#ifdef __cplusplus
extern "C"{
#endif


/// data type unsigned char, data length 1 byte
typedef unsigned char                            AURA_U8;         // 1 byte
/// data type unsigned short, data length 2 byte
typedef unsigned short                           AURA_U16;        // 2 bytes
/// data type unsigned int, data length 4 byte
typedef unsigned int                             AURA_U32;        // 4 bytes
/// data type unsigned int, data length 8 byte
typedef unsigned long long                       AURA_U64;        // 8 bytes
/// data type signed char, data length 1 byte
typedef signed char                              AURA_S8;         // 1 byte
/// data type signed short, data length 2 byte
typedef signed short                             AURA_S16;        // 2 bytes
/// data type signed int, data length 4 byte
typedef signed int                               AURA_S32;        // 4 bytes
/// data type signed int, data length 8 byte
typedef signed long long                         AURA_S64;        // 8 bytes
/// data type float, data length 4 byte
typedef float                                    AURA_FLOAT;      // 4 bytes

typedef unsigned char                            AURA_BOOL;


#ifdef __cplusplus
}
#endif

#endif	/* AURA_COMMON_DATATYPE_H */
