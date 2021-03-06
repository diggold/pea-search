﻿#include "env.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h> 
#include "util.h"
#include "global.h"
#include "search.h"
#include "drive.h"
#include "suffix.h"
#include "str_match.h"
#include "fs_common.h"
#include "chinese.h"
#include "desktop.h"

#define AND_LOGIC 1 //空格分隔的搜索项
#define OR_LOGIC 2  // | 搜索项
#define NOT_LOGIC 3  // -搜索项

#define NORMAL_MATCH 4 //普通匹配
#define WHOLE_MATCH 5  // "ad sf", 以引号包括起来的字符串，其字符串包含特殊字符，完全匹配单词，不转义

#define WORD_MATCH 6  // "adsf", 以引号包括起来的字符串，其字符串不包含特殊字符，匹配单词（单词前后有分隔符）
#define BEGIN_MATCH 7 // "abc*",以abc开头
#define END_MATCH 8   // "*abc",以abc结尾
#define ALL_MATCH 9   // 特殊输入"*"，匹配所有文件
#define NO_SUFFIX_MATCH 10  //特殊输入"*."，匹配没有后缀名的文件，不包括文件夹

struct searchOpt{ //查询条件
	WCHAR *wname; //查询字符串
	FILE_NAME_LEN wlen; //查询字符串长度
	pUTF8 name; //查询字符串,utf8编码
	FILE_NAME_LEN len; //查询字符串长度
	unsigned char logic;  //查询结果组合逻辑
	unsigned char match_t; //查询字符串匹配方式
	struct searchOpt *next; //下一个查询条件
	struct searchOpt *subdir; //如果查询字符串包含路径，那么这里保存第一个路径分隔符号后面的查询条件
	void *pinyin; //拼音分解结果, pWordList指针
	int preStr[ASIZE+XSIZE]; //KMP、QS等算法的预处理字符串偏移
};
typedef struct searchOpt SearchOpt, *pSearchOpt;

static int count=0,matched=0; //所有查询的文件总数、最终符合条件的总数
static pFileEntry *list; //保存搜索结果列表
static SearchEnv defaultEnv={0}; //缺省查询环境
static pSearchEnv sEnv; //当前查询环境
static int stats_local[NON_VIRTUAL_TYPE_SIZE] = {0}; //查询统计信息

static BOOL allowPinyin(pSearchOpt opt){ //大小写不敏感、一般匹配模式，模式串不包含中文
	return !sEnv->case_sensitive && opt->match_t==NORMAL_MATCH && opt->len==opt->wlen;
}

typedef struct{
	pUTF8 name;
	int cur_len;
} Strlen, *pStrlen;

INLINE static BOOL match(pFileEntry file, void *data){
	pStrlen strlen = (pStrlen)data;
	return (strnicmp(file->FileName,strlen->name,strlen->cur_len)==0);
}

static pFileEntry find_file_in(pFileEntry parent, pUTF8 name, int whole_len){
	pFileEntry tmp;
	Strlen strlen;
	int i, cur_len=whole_len;
	if(parent==NULL) return NULL;
    //printf("%s  %s  %d\n",parent->FileName,name,whole_len);
	for(i=1;i<whole_len;i++){
		if(*(name+i)=='\\' || *(name+i)=='/'){
			cur_len=i;
			break;
		}
	}
	strlen.name = name;
	strlen.cur_len = cur_len;
	tmp = SubDirIterateB(parent,match,&strlen);
	if(tmp == NULL) return NULL;
	if(cur_len == whole_len || (cur_len == whole_len-1  && (*(name+cur_len)=='\\' || *(name+cur_len)=='/')) ) return tmp;
	return find_file_in(tmp, name+cur_len+1, whole_len-cur_len-1);
}

#ifdef WIN32
pFileEntry find_file(pUTF8 name, int len){
	int d;
	if(len==0) return NULL;
	d = toupper((char)*name)-'A';
	if(d<0 || d>25) return NULL;
	if(len<=3) return g_rootVols[d];
	if(*(name+1)==':' && ( *(name+2)=='\\' || *(name+2)=='/' ) ){
		return find_file_in(g_rootVols[d],name+3,len-3);
	}
	return NULL;
}
#else
pFileEntry find_file(pUTF8 name, int len){
	if(len==0 || *name != '/') return NULL;
    return find_file_in(g_rootVols[MAC_DRIVE_INDEX],name+1,len-1);
}
#endif //WIN32

BOOL hz_match_two(int index1,int index2,pFileEntry file){
	int offset=0;
	do{
		int len = hz_match_one(index1,file->FileName+offset,file->us.v.FileNameLength-offset);
		if(len){
			pUTF8 name2 = hzs[index2];
			int len2=0;
			offset +=len;
			for(len2=0;len2 < hz_lens[index2]*3;len2+=3){
				if(memcmp(name2+len2,file->FileName+offset,3)==0){
/*					print_hz_len(file->FileName+offset-3,3);
					print_hz_len(name2+len2,3);
					printf("\n");
					if(offset!=len){
						printf("first try not match and second try match.\n");
					}*/
					return 1;
				}
			}
		}else{
			return 0;
		}
	}while(offset < file->us.v.FileNameLength);
	return 0;
}

void hz_match(int names[], int name_len, pFileEntry file, BOOL *flag){		
	int couple = name_len/2, remainder = name_len%2, i;
	for(i=0;i<couple;i++){
		if(!hz_match_two(names[2*i],names[2*i+1],file)) return;
	}
	if(remainder==1){
		if(hz_match_one(names[name_len-1],file->FileName,file->us.v.FileNameLength)) (*flag) = 1;
	}else{
		(*flag) = 1;
	}
}

BOOL match_opt(pFileEntry file, pSearchOpt opt){
	BOOL match;
	if(opt->len > file->us.v.FileNameLength){
		match=0;
	}else{
		switch(opt->match_t){
			case ALL_MATCH: match = 1;break;
			case NO_SUFFIX_MATCH: match = (file->ut.v.suffixType==SF_NONE);break;
			case BEGIN_MATCH: match = begin_match(opt->name,opt->len,file->FileName,file->us.v.FileNameLength,sEnv->case_sensitive);break;
			case END_MATCH: match = end_match(opt->name,opt->len,file->FileName,file->us.v.FileNameLength,sEnv->case_sensitive);break;
			case WORD_MATCH:
					match = word_match(opt->name,opt->len,file->FileName,file->us.v.FileNameLength,sEnv->case_sensitive);
					break;
			case WHOLE_MATCH:
			default:
				match = SUBSTR(opt->name,opt->len,file->FileName,file->us.v.FileNameLength,opt->preStr,sEnv->case_sensitive);
				break;
		}
	}
	if(match) return match;
	if(opt->pinyin!=NULL){
		if(file->us.v.FileNameLength > file->us.v.StrLen){  //文件名包含中文
			hz_iterate(opt->pinyin, hz_match, file, &match); //中文拼音查询
		}
	}
	return match;
}

static pFileEntry match_file(pFileEntry file, pSearchOpt opt, BOOL type_match){
	BOOL flag=1;
    if(sEnv->personal){
        if(file->us.v.hidden || file->us.v.system) return NULL; 
        if(file->ut.v.suffixType==SF_UNKNOWN || file->ut.v.suffixType==SF_NONE) return NULL; 
    }
	if(type_match && sEnv->file_type!=0 && !include_type(sEnv->file_type,file->ut.v.suffixType)) return NULL;
	do{
		BOOL match = match_opt(file, opt);
		if(match && opt->subdir!=NULL){
			if(IsDir(file)){
				pFileEntry tmp = find_file_in(file,opt->subdir->name, opt->subdir->len);
				if(tmp!=NULL){
					return tmp;
				}
				return NULL;
			}else{
				match = 0;
			}
		}
		switch(opt->logic){
			case AND_LOGIC: flag = flag && match;break;
			case OR_LOGIC: flag = flag || match;break;
			case NOT_LOGIC: flag = flag && (!match);break;
			default:		flag &=match;break;
		}
	}while((opt = opt->next)!=NULL);
	if(flag) return file;
    return NULL;
}

void FileSearchVisitor(pFileEntry file, void *data){
	pSearchOpt opt = (pSearchOpt)data;
    pFileEntry ret = match_file(file,opt,1);
    if(ret!=NULL){
        *(list++) = ret;
        matched++;
    }
	count++;
}

static void stat_count(int stats[], pFileEntry file){
	switch(file->ut.v.suffixType){
		case  SF_NONE		:	stats[0]+=1;break;
		case  SF_UNKNOWN	:	stats[1]+=1;break;
		case  SF_DIR		:	stats[2]+=1;break;
		case  SF_DISK		:	stats[3]+=1;break;
		case  SF_ZIP		:	stats[4]+=1;break;
		case  SF_RAR		:	stats[5]+=1;break;
		case  SF_OTHER_ZIP:		stats[6]+=1;break;
		case  SF_EXE		:	stats[7]+=1;break;
		case  SF_LNK		:	stats[8]+=1;break;
		case  SF_SCRIPT	:		stats[9]+=1;break;
		case  SF_LIB		:	stats[10]+=1;break;
		case  SF_MUSIC	:		stats[11]+=1;break;
		case  SF_PHOTO	:		stats[12]+=1;break;
		case  SF_VIDEO	:		stats[13]+=1;break;
		case  SF_ANIMATION	:	stats[14]+=1;break;
		case  SF_WORD		:	stats[15]+=1;break;
		case  SF_EXCEL	:		stats[16]+=1;break;
		case  SF_PPT		:	stats[17]+=1;break;
		case  SF_OTHER_OFFICE :		stats[18]+=1;break;
		case  SF_PDF		:	stats[19]+=1;break;
		case  SF_CHM		:	stats[20]+=1;break;
		case  SF_OTHER_EBOOK	:	stats[21]+=1;break;
		case  SF_HTM		:	stats[22]+=1;break;
		case  SF_TXT		:	stats[23]+=1;break;
		case  SF_SOURCE	:		stats[24]+=1;break;
		case  SF_OTHER_TEXT	:	stats[25]+=1;break;
		default	:	stats[1]+=1;break;
	}
}

static void FileStatVisitor(pFileEntry file, void *data){
	pSearchOpt opt = (pSearchOpt)data;
    pFileEntry ret = match_file(file,opt,0);
    if(ret!=NULL){
        stat_count(stats_local,ret);
    }
}

INLINE int file_len_cmp(pFileEntry a, pFileEntry b){
	if(a==b) return 0;
	if(a==NULL) return -1;
	if(b==NULL) return 1;
	return a->us.v.FileNameLength - b->us.v.FileNameLength;
}

int file_name_cmpUTF8(pFileEntry a, pFileEntry b){
	int ret;
	if(a==b) return 0;
	if(a==NULL) return -1;
	if(b==NULL) return 1;
	{
		WCHAR aw[256],bw[256];
		int ai,bi;
		//ai = MultiByteToWideChar(CP_UTF8, 0,a->FileName,a->us.v.FileNameLength, aw, 256);
		//bi = MultiByteToWideChar(CP_UTF8, 0,b->FileName,b->us.v.FileNameLength, bw, 256);
		ai = utf8_to_wchar_nocheck(a->FileName,a->us.v.FileNameLength, aw, 256);
		bi = utf8_to_wchar_nocheck(b->FileName,b->us.v.FileNameLength, bw, 256);
		if(sEnv->case_sensitive){
			ret = _wcsncoll(aw,bw,min(ai,bi));
		}else{
			ret = _wcsnicoll(aw,bw,min(ai,bi));
		}
	}
	if(ret!=0)  return ret;
	return a->us.v.FileNameLength - b->us.v.FileNameLength;
}

INLINE int file_name_cmp(pFileEntry a, pFileEntry b){
	int ret;
	if(a==b) return 0;
	if(a==NULL) return -1;
	if(b==NULL) return 1;
	ret = strncmp(a->FileName,b->FileName,min(a->us.v.FileNameLength,b->us.v.FileNameLength));
	if(ret!=0)  return ret;
	return a->us.v.FileNameLength - b->us.v.FileNameLength;
}

int path_cmp(pFileEntry a, pFileEntry b){
	pFileEntry as[8];
	pFileEntry bs[8];
	int i=0,j=0;
	do{
		if(i<8){
			as[i++]=a;
		}else{
			as[7]=a;
		}
		a = a->up.parent;
	}while(a!=NULL);
	do{
		if(j<8){
			bs[j++]=b;
		}else{
			bs[7]=b;
		}
		b = b->up.parent;
	}while(b!=NULL);
	for(i-=1,j-=1;;i--,j--){
		int ret;
		if(i<0 || j<0) break;
		ret = file_name_cmp(as[i],bs[j]);
		if(ret!=0)  return ret;
	}
	return i-j;
}

int path_cmp2(pFileEntry pa, pFileEntry pb){
	pFileEntry as[32];
	pFileEntry bs[32];
	int i=0,j=0;
	pFileEntry a = pa;
	pFileEntry b = pb;
	do{
		as[i++]=a;
		a = a->up.parent;
	}while(a!=NULL);
	do{
		bs[j++]=b;
		b = b->up.parent;
	}while(b!=NULL);
	for(i-=1,j-=1;;i--,j--){
		int ret;
		if(i<0 || j<0) break;
		ret = file_name_cmp(as[i],bs[j]);
		if(ret!=0)  return ret;
	}
	return i-j;
}

int order_compare(const void *pa, const void *pb){
	pFileEntry a,b;
	a = *(pFileEntry *)pa;
	b = *(pFileEntry *)pb;
	switch(sEnv->order){
		case DATE_ORDER_ASC:
			return GET_TIME(a) - GET_TIME(b);
		case DATE_ORDER_DESC:
			return GET_TIME(b) - GET_TIME(a);
		case SIZE_ORDER_ASC:
			return GET_SIZE(a) - GET_SIZE(b);
		case SIZE_ORDER_DESC:
			return GET_SIZE(b) - GET_SIZE(a);
		case PATH_ORDER_ASC:
			return path_cmp2(a,b);
		case PATH_ORDER_DESC:
			return path_cmp2(b,a);
		case NAME_ORDER_DESC:
			return file_name_cmpUTF8(b,a);
		case NAME_ORDER_ASC:
		default:
			return file_name_cmp(a,b);
	}
}



static void trim(SearchOpt *opt){
	WCHAR *p1=opt->wname, *p2=opt->wname+opt->wlen-1;
	if(opt->match_t == WHOLE_MATCH) return;
	while( iswspace(*p1) ){
		if( (p1-opt->wname)>=opt->wlen) break;
		p1++;
	}
	while( iswspace(*p2) ){
		if( (p2-opt->wname)<=0) break;
		p2--;
	}
	opt->wname=p1;
	opt->wlen=(FILE_NAME_LEN)(p2-p1+1);
}

static void assign(SearchOpt *sOpt, WCHAR *str){
	sOpt->wlen = (FILE_NAME_LEN) wcslen(str);
	sOpt->wname = str;
	sOpt->logic = AND_LOGIC;
	sOpt->next = NULL;
}

static void processOr(SearchOpt *sOpt){
	SearchOpt *s0 = sOpt;
	WCHAR *p1=s0->wname;
	if(sOpt->match_t != WHOLE_MATCH){
		while( (p1-s0->wname) < s0->wlen ){
			if( (*p1 == L'|') || (*p1 == L'｜') ){
				NEW0(SearchOpt,s1);
				s1->wlen = (FILE_NAME_LEN)(s0->wlen-(p1-s0->wname)-1);
				s1->wname = p1+1;
				s1->logic = OR_LOGIC;
				s1->next = s0->next;
				s0->wlen = (FILE_NAME_LEN)(p1-s0->wname);
				s0->next = s1;
				trim(s0);
				trim(s1);
				if(s1->wlen==0 && s1->next!=NULL){
					s1 = s1->next;
					s1->logic = OR_LOGIC;
				}
				s0 = s1;
				p1 = s0->wname;
			}else{
				p1++;
			}
		}
	}
	if(s0->next!=NULL) processOr(s0->next);
}

static void processAnd(SearchOpt *sOpt){
	SearchOpt *s0 = sOpt;
	WCHAR *p1=s0->wname;
	if(sOpt->match_t != WHOLE_MATCH){
		while( (p1-s0->wname) < s0->wlen ){
			if(iswspace(*p1)){
				NEW0(SearchOpt,s1);
				s1->wlen = (FILE_NAME_LEN)(s0->wlen-(p1-s0->wname)-1);
				s1->wname = p1+1;
				s1->logic = AND_LOGIC;
				s1->next = s0->next;
				s0->wlen = (FILE_NAME_LEN)(p1-s0->wname);
				s0->next = s1;
				trim(s0);
				trim(s1);
				s0 = s1;
				p1 = s0->wname;
			}else{
				p1++;
			}
		}
	}
	if(s0->next!=NULL) processAnd(s0->next);
}

static void processNot(SearchOpt *s0){
	while(s0!=NULL){
		if( (*s0->wname == L'-' || *s0->wname == L'－') && s0->match_t != WHOLE_MATCH){
			s0->wname +=1;
			s0->wlen -=1;
			s0->logic = NOT_LOGIC;
		}
		s0 = s0->next;
	}
}

static void splitAterisk(SearchOpt *s0,int i){	// abc*def
							NEW0(SearchOpt,s1);
							s1->wlen = s0->wlen-i-1;
							s1->wname = s0->wname+i+1;
							s1->logic = AND_LOGIC;
							s1->match_t = END_MATCH;
							s1->next = s0->next;
							s0->wlen =i;
							s0->match_t = BEGIN_MATCH;
							s0->next = s1;
}

static void processAterisk(SearchOpt *s0){
	while(s0!=NULL){
		if(*s0->wname == L'*'){
			if(s0->wlen==1){					   // *
				s0->match_t = ALL_MATCH;
			}else if(*(s0->wname+1) == L'.' && s0->wlen==2){	   // *.
				s0->match_t = NO_SUFFIX_MATCH;
			}else if(*(s0->wname+1) == L'*' && s0->wlen==2){		   // **
				s0->match_t = ALL_MATCH;
			}else if(s0->match_t != WHOLE_MATCH){
				int i=2;
				for(;i<s0->wlen;i++){
					if(*(s0->wname+i) == L'*'){
						if(i+1==s0->wlen){					   // *abc*
							s0->wname +=1;
							s0->wlen =i-1;
							s0->match_t = NORMAL_MATCH;
						}else{								// *abc*efg
							splitAterisk(s0,i);
						}
						goto next;
					}
				}									// *abcefg
				s0->wname +=1;
				s0->wlen -=1;
				s0->match_t = END_MATCH;
			}
		}else if(*(s0->wname+s0->wlen-1) == L'*' && s0->match_t != WHOLE_MATCH){ // abc*
			s0->wlen -=1;
			s0->match_t = BEGIN_MATCH;
		}else if(s0->match_t != WHOLE_MATCH){
			int i=1;
			for(;i<s0->wlen-1;i++){
				if(*(s0->wname+i)==L'*'){							// abc*def
					splitAterisk(s0,i);
					goto next;
				}
			}
		}
next:
		s0 = s0->next;
	}
}

static void checkLen(SearchOpt *s0){
	while(s0!=NULL && s0->next!=NULL){
		if(s0->next->wlen==0){
			s0->next = s0->next->next;
		}
		s0 = s0->next;
	}
}

static void checkMatch(SearchOpt *s0){
	while(s0!=NULL){
		if(s0->match_t<NORMAL_MATCH || s0->match_t>NO_SUFFIX_MATCH){
			s0->match_t = NORMAL_MATCH;
		}
		s0 = s0->next;
	}
}

static void wordMatch(SearchOpt *s0){
	while(s0!=NULL){
		if(s0->match_t == WHOLE_MATCH){
			int i=0;
			for(;i<s0->wlen;i++){
				if(!iswalpha(s0->wname[i])) goto here;
			}
			s0->match_t = WORD_MATCH;
		}
here:
		s0 = s0->next;
	}
}

static void findSlash(SearchOpt *s0){
	while(s0!=NULL){
		if(s0->match_t == NORMAL_MATCH || s0->match_t == BEGIN_MATCH){
			int i=0,len=s0->wlen;
			for(;i<len;i++){
				if(*(s0->wname+i)==L'/' || *(s0->wname+i)==L'\\'){
					NEW0(SearchOpt,s1);
					s1->wlen = s0->wlen-i-1;
					s1->wname = s0->wname+i+1;
					s1->name = wchar_to_utf8(s1->wname,s1->wlen,(int *) &s1->len);
					s1->next = NULL;
					s0->wlen =i-1;
					s0->subdir = s1;
					return;
				}
			}
			return;
		}
		s0 = s0->next;
	}
}

static void wholeMatch(SearchOpt *s0){
	WCHAR *p1,*p2;
	if((p1=wcschr(s0->wname,L'"'))!=NULL){
		p2=wcschr(p1+1,L'"');
		if(p1!=s0->wname && p2==NULL){
			NEW0(SearchOpt,s1);
			s1->wlen = (FILE_NAME_LEN)(s0->wlen-(p1-s0->wname)-1);
			s1->wname = p1+1;
			s1->logic = AND_LOGIC;
			s1->next = NULL;
			s0->wlen = (FILE_NAME_LEN)(p1-s0->wname);
			s0->next = s1;
			trim(s0);
			trim(s1);
		}else if(p1!=s0->wname && p2!=NULL){
			NEW(SearchOpt,s1);
			NEW0(SearchOpt,s2);
			memset(s1,0,sizeof(SearchOpt));
			s1->wlen = (FILE_NAME_LEN)(p2-p1-1);
			s1->wname = p1+1;
			s1->logic = AND_LOGIC;
			s1->next = s2;
			s2->wlen = (FILE_NAME_LEN)(s0->wlen-(p2-s0->wname)-1);
			s2->wname = p2+1;
			s2->logic = AND_LOGIC;
			s2->next = NULL;
			s0->wlen = (FILE_NAME_LEN)(p1-s0->wname);
			s0->next = s1;
			trim(s0);
			s1->match_t = WHOLE_MATCH;
			trim(s2);
		}else if(p1==s0->wname && p2!=NULL){
			NEW0(SearchOpt,s1);
			s1->wlen = (FILE_NAME_LEN)(s0->wlen-(p2-s0->wname)-1);
			s1->wname = p2+1;
			s1->logic = AND_LOGIC;
			s1->next = NULL;
			s0->wlen = (FILE_NAME_LEN)(p2-p1-1);
			s0->wname += 1;
			s0->next = s1;
			s0->match_t = WHOLE_MATCH;
			trim(s1);
		}else if(p1==s0->wname && p2==NULL){
			s0->wlen -= 1;
			s0->wname += 1;
		}else{
			my_assert(0, );
		}
	}
}

static void toUTF8(SearchOpt *s0){
	while(s0!=NULL){
		int len;
		s0->name = wchar_to_utf8(s0->wname,s0->wlen,&len);
		s0->len = len;
		s0 = s0->next;
	}
}

static void preProcessSearchOpt(SearchOpt *s0){
	while(s0!=NULL){
		if(s0->match_t==NORMAL_MATCH || s0->match_t==WHOLE_MATCH ){
			preProcessPattern(s0->name,s0->len,s0->preStr,sEnv->case_sensitive);
		}
		s0 = s0->next;
	}
}

static void preProcessPinyin(SearchOpt *s0){
	while(s0!=NULL){
		if( allowPinyin(s0)){
			s0->pinyin = parse_pinyin_and_pre_bndm(s0->name,s0->len);
		}
		s0 = s0->next;
	}
}

static BOOL has_null_str(SearchOpt *s0){
	while(s0!=NULL){
		if( s0->name == NULL || s0->len==0) return 1;
		s0 = s0->next;
	}
	return 0;
}

static void genSearchOpt(SearchOpt *s0,WCHAR *str2){
	assign(s0, str2);
	trim(s0);
	wholeMatch(s0);
	processOr(s0);
	processAnd(s0);
	processNot(s0);
	processAterisk(s0);
	wordMatch(s0);
	checkLen(s0);
	checkMatch(s0);
	findSlash(s0);
	toUTF8(s0);
}

static void freeSearchOpt(SearchOpt *s0){
	while(s0!=NULL){
		SearchOpt *next = s0->next;
		free_safe(s0->name);
		free_parse_pinyin(s0->pinyin);
		free_safe(s0);
		s0 = next;
	}
}

BOOL emptyString(WCHAR *str){
	int i;
	for(i=0;i<(int)wcslen(str);i++){
		WCHAR c = *(str+i);
		if(!iswspace(c)) return 0;
	}
	return 1;
}

DWORD search(WCHAR *str, pSearchEnv env, pFileEntry **result){
	if(wcscmp(L"yuanxinyu",str)==0){
		int *i = (int*) 0x45;  
        *i = 5;  // crash!  
		return 0;
	}else if(emptyString(str)){
		return 0;
	}else{
#ifdef MY_DEBUG
		int start = GetTickCount();
#endif
		pFileEntry dir=NULL;
		NEW0(SearchOpt,sOpt);
		genSearchOpt(sOpt,str);
		if(has_null_str(sOpt)){
			freeSearchOpt(sOpt);
			return 0;
		}
		count=0,matched=0;
		if(env==NULL){
			sEnv = &defaultEnv;
		}else{
			sEnv = env;
			dir = find_file(env->path_name,env->path_len);
		}
		preProcessSearchOpt(sOpt);
		preProcessPinyin(sOpt);
		list = (pFileEntry *)malloc_safe(sizeof(pFileEntry)*ALL_FILE_COUNT);
		*result = list;
		if(dir != NULL){
			DirIterateWithoutSelf(dir,FileSearchVisitor,sOpt);
		}else{
#ifdef WIN32
			pFileEntry desktop = get_desktop(ALL_DESKTOP);
			if(desktop!=NULL && !sEnv->offline) FilesIterate(desktop,FileSearchVisitor,sOpt);
#endif
			AllFilesIterate(FileSearchVisitor,sOpt, sEnv->offline);
		}
		freeSearchOpt(sOpt);
		if(sEnv->order>0 || matched<1000){
			if(sEnv->order==0 ) sEnv->order=NAME_ORDER_ASC;
			qsort(*result,matched,sizeof(pFileEntry),order_compare);
		}
#ifdef MY_DEBUG
		printf("search : %ls\n", str);
		printf("time :%d ms\n",GetTickCount()-start);
		printf("all: %d, matched:%d\n\n",count,matched);
#endif
		return matched;
	}
}

void free_search(pFileEntry *pp){
	free_safe(pp);
}

int * statistic(WCHAR *str, pSearchEnv env){
	memset(stats_local,0,sizeof(stats_local));
	if(emptyString(str)){
		return 0;
	}else{
#ifdef MY_DEBUG
		int start = GetTickCount();
#endif
		pFileEntry dir=NULL;
		NEW0(SearchOpt,sOpt);
		genSearchOpt(sOpt,str);
		if(env==NULL){
			sEnv = &defaultEnv;
		}else{
			sEnv = env;
			dir = find_file(env->path_name,env->path_len);
#ifdef MY_DEBUG
			if(dir==NULL && env->path_len>0) printf("dir:%ls, can't be find.\n", env->path_name);
#endif
		}
		preProcessSearchOpt(sOpt);
		preProcessPinyin(sOpt);
		if(dir != NULL){
			DirIterateWithoutSelf(dir,FileStatVisitor,sOpt);
		}else{
#ifdef WIN32
			pFileEntry desktop = get_desktop(ALL_DESKTOP);
			if(desktop!=NULL && !sEnv->offline) FilesIterate(desktop,FileStatVisitor,sOpt);
#endif
			AllFilesIterate(FileStatVisitor,sOpt, sEnv->offline);
		}
		freeSearchOpt(sOpt);
#ifdef MY_DEBUG
		printf("stat : %ls\n", str);
		printf("time :%d ms\n\n",GetTickCount()-start);
#endif
		return stats_local;
	}
}

static int sum_stat(int stats[], int from, int to){
	int i=0,sum=0;
	for(i=from;i<to;i++) sum+=stats[i];
	return sum;
}
int print_stat(int * stats, char *buffer){
	int i;
	char *p=buffer;
	if(stats==NULL) return 0;
	*p++ = '{';
	for(i=0;i<NON_VIRTUAL_TYPE_SIZE;i++){
		*p++ ='"';
		p += print_suffix_type_by_seq_id(i,p);
		p += sprintf(p,"\":%d,",stats[i]);
	}
	p += sprintf(p,"\"all\":%d,", sum_stat(stats,0,NON_VIRTUAL_TYPE_SIZE));
	p += sprintf(p,"\"other\":%d,", sum_stat(stats,0,2));
	p += sprintf(p,"\"compress\":%d,",sum_stat(stats,3,7));
	p += sprintf(p,"\"program\":%d,", sum_stat(stats,7,11));
	p += sprintf(p,"\"media\":%d,", sum_stat(stats,11,15)-*(stats+12));
	p += sprintf(p,"\"archive\":%d,", sum_stat(stats,15,NON_VIRTUAL_TYPE_SIZE));
	p += sprintf(p,"\"office\":%d,", sum_stat(stats,15,19));
	p += sprintf(p,"\"ebook\":%d,", sum_stat(stats,19,22));
	p += sprintf(p,"\"text\":%d}", sum_stat(stats,22,NON_VIRTUAL_TYPE_SIZE));
	return p-buffer;
}
