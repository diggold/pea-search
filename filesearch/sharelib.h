﻿#ifdef __cplusplus
extern "C" {
#endif

#ifndef FILE_SEARCH_SHARELIB_H_
#define FILE_SEARCH_SHARELIB_H_

#include "env.h"

struct searchRequest{
	int len;	/* Total length of request, not including this field */
	SearchEnv env;
	int from; /*分页时从第几条数据开始*/
	int rows; /*最多返回多少行数据*/
	WCHAR str[MAX_PATH];
};
typedef struct searchRequest SearchRequest, *pSearchRequest;


struct searchResponse{
	int len;	/* Total length of response, not including this field */
	char json[1];
};
typedef struct searchResponse SearchResponse, *pSearchResponse;

#define MAX_RESPONSE_LEN 409600

#define CS_TIMEOUT 3000

#define SERVER_PIPE L"\\\\.\\PIPE\\GIGASOSERVER"


/* Commands for the statistics maintenance function. */

#define CS_INIT			1
#define CS_RQSTART		2
#define CS_RQCOMPLETE	3
#define CS_REPORT		4
#define CS_TERMTHD		5


#endif  // FILE_SEARCH_SHARELIB_H_

#ifdef __cplusplus
}
#endif
