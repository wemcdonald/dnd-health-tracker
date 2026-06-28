#ifndef TEST_ASSERT_H
#define TEST_ASSERT_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ASSERT_TRUE(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define ASSERT_EQ(a,b) do { long _a=(long)(a),_b=(long)(b); if (_a!=_b){fprintf(stderr,"FAIL %s:%d: %ld != %ld\n",__FILE__,__LINE__,_a,_b);exit(1);} } while (0)
#define ASSERT_STR_EQ(a,b) do { if (strcmp((a),(b))){fprintf(stderr,"FAIL %s:%d: \"%s\" != \"%s\"\n",__FILE__,__LINE__,(a),(b));exit(1);} } while (0)
#endif
