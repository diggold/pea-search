﻿#ifdef __cplusplus
extern "C" {
#endif

#ifndef FILE_SEARCH_MAC_H_
#define FILE_SEARCH_MAC_H_

#include "env.h"
#include "fs_common.h"

/**
 * 扫描一个根分区
 * @param root 根分区
 */
extern int scanUnix(pFileEntry root, int i);

    /**
     * 给定的文件是否应该被忽略，不索引，不需要查询
     * @param fullpath 文件全路径
     * @param filename 文件名     
     */    
    extern BOOL ignore_file(char *fullpath, char *filename);

#endif  // FILE_SEARCH_MAC_H_

#ifdef __cplusplus
}
#endif
