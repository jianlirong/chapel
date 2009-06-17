#ifndef _arg_H_
#define _arg_H_

#include <stdint.h>
#include "chpltypes.h"

//
// defined in arg.c
//
extern int32_t blockreport;
extern int32_t taskreport;

void parseNumLocales(const char* numPtr, int32_t lineno, chpl_string filename);
void parseLocalesPerRealm(const char* numPtr, int32_t lineno, 
                          chpl_string filename);
void parseArgs(int* argc, char* argv[]);
int32_t getArgNumLocales(void);
int32_t chpl_localesPerRealm(int32_t r);
int32_t chpl_baseUniqueLocaleID(int32_t r);
const char* chpl_realmType(int32_t r);
int _runInGDB(void);

//
// defined with main()
//
int handleNonstandardArg(int* argc, char* argv[], int argNum, 
                         int32_t lineno, chpl_string filename);

#endif
