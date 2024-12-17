#include "json.h"
#include <stdbool.h>
#include <errno.h>

#if SIG_SPRT == 1
#include <signal.h>
#endif
#define PASS (void)0

#define OBJHASH_BUCKET 11
#define F_OBJEXIST 0x01
#define F_NWRITE 0x02
#define MIN_FILE_BYTES 7
#define MAX_FILE_BYTES SSIZE_MAX

#if defined(_WINDOW_H) 
#include <wchar.h>
#include <wctype.h>
#define isspace(c) iswspace(c)
#define strcmp(s1, s2) wcscmp(s1, s2)
#define Json_strLen(s) wcstrlen(s)
typedef wchar_t JsonChar;
#define JS_CH(txt) L txt
#else
#include <ctype.h>
#define JS_CH(txt) txt
typedef uint8_t  JsonChar;
#define Json_strLen(s) strlen(s)
#endif
typedef JsonChar * JsonString;
typedef intmax_t JsonInteger;
typedef double JsonDecimal;
typedef void ** JsonArray;
typedef void ** JsonObj;
typedef _Bool JsonBool;

enum {
      JSONSTRING_TYPE,
      JSONINTEGER_TYPE,
      JSONDECIMAL_TYPE,
      JSONARRAY_TYPE,
      JSONOBJECT_TYPE,
      JSONBOOL_TYPE
};

typedef struct JsonObjectStruct JsonObjectStruct;
typedef struct JsonObjectStruct * JsonObject;
typedef struct JsonDataStruct JsonDataStruct;
typedef struct JsonDataStruct * JsonData;

#define SIZEOF_JSON_OBJECT sizeof(struct JsonObjectStruct)
#define SIZEOF_JSON_DATA sizeof(struct JsonDataStruct)

struct JsonDataStruct {
    JsonString __key;
    union {
	JsonString __str;
	JsonArray __arr;
	JsonObj __obj;
	JsonDecimal __flt[1];
	JsonInteger __int[1];
	JsonBool __bool[1];
    };
    JsonData __nd;
    JsonData __pd;
    uint8_t __type;
};
struct JsonObjectStruct {
    JsonData __head[OBJHASH_BUCKET];
    JsonData __tail[OBJHASH_BUCKET];
    size_t __sz[OBJHASH_BUCKET];
    JsonObject *__self;
    void * (*string)(JsonObject, JsonString, void *);
    void * (*read)(JsonObject, char *);
    void * (*remove)(JsonObject, JsonString);
    void * (*delete)(JsonObject, char *);
    void * (*jsonDump)(JsonObject *,  char *);
#if JSON_DOBUFFERING
    void *__buf;
    ssize_t __szbuf;
#endif
    uint8_t __hashBkt, __setfl;
};

/* PROTOTYPES */
__NONNULL__ static __inline__ void *lexer(JsonObject, JsonString, ssize_t);
__NONNULL__ void *addStringToHashTable (JsonObject, JsonString, void *);
__NONNULL__ void *removeFromHashTable (JsonObject, JsonString);
static __inline__ void *readfl(JsonObject, int, ssize_t);

#ifdef JSON_DEBUG
#define throwError(errnum, mem, ...) __jsonError__(errnum, #__LINE__, #object ##->##__FUNC__, mem)
#else
#define throwError(errnum, ...) (errno = errnum)
#endif
#define puti(d) printf("%ld\n", (long)(d))
enum {
      ESYNTAX = 7
};

__NONNULL__ __attribute__((warn_unused_result)) JsonObject JsonOpen(unsigned char *file) {
    struct stat statbf;
    JsonObject newObj;
    register int L0 __UNUSED__;
    register int fd __UNUSED__;

    if (*file == 0) {
	throwError(EINVAL, newObj);
	return NULL;
    }
    if ((fd = open(file, O_RDWR | O_NONBLOCK | O_NOFOLLOW)) == -1) {
	throwError(errno, newObj);
	return NULL;
    }
    if (fstat(fd, &statbf)) {
	throwError(errno, newObj);
	return NULL;
    }
    if (statbf.st_size < MIN_FILE_BYTES) {
    }
    if (! (newObj = malloc(SIZEOF_JSON_OBJECT))) {
	throwError(ENOMEM, newObj);
	return NULL;
    }
    
    for (L0 = 0; L0 < OBJHASH_BUCKET; L0++) {
	newObj->__head[L0] = newObj->__tail[L0] = NULL;
    }

    newObj->__setfl |= F_OBJEXIST;
    newObj->__hashBkt = OBJHASH_BUCKET;
    newObj->__self = &newObj;
    newObj->string = addStringToHashTable;
    newObj->remove = removeFromHashTable;

    if(! readfl(newObj, fd, statbf.st_size)) {
	puts("readfl encountered an error");
	perror("");
    }
    return newObj;
}

static __inline__ void *readfl(JsonObject obj, int fd, ssize_t sz) {
    JsonString bf;
    register ssize_t rdbytes, L1;

    if (fd < 0 || sz < 0) {
	return NULL;
    }
#if JSON_DOBUFFERING
    if (sz > MAX_FILE_BYTES) {
	/* TODO: read block by block ( each block reads MAX_FILE_BYTES bytes )  */
	return NULL;
    }
    if (! (bf = malloc(sz + 1))) {
	throwError(ENOMEM, NULL);
	return NULL;
    }
    obj->__buf = bf;
    obj->__sbf = sz + 1;
#endif
    errno = 0;
    while (((L1 = read(fd, bf, sz)) && (L1 != -1)) || (errno == EINTR)) {
	/* TODO: update file pointer using seek if EINTR */
	rdbytes = L1;
    }
    if (L1 == -1) {
	throwError(errno, NULL);
	return NULL;
    }
    bf[rdbytes] = 0;
    if (! lexer(obj, bf, sz)) {
	puts("lexer error");
	return NULL;
    }
    return obj;
}

__NONNULL__ static __inline__ ssize_t getKey(JsonString str, JsonString __restrict *bf) {
    register JsonChar c;
    register ssize_t rsz, i;

    if (*str == 0)
	return -1;
    /* TODO: REMOVE THE STOP AT NULL BYTE */
    for (rsz = 0; (c = str[rsz]) && !(c == JS_CH('"')); rsz++)
	PASS;
    if (c == JS_CH('"')) {
	for (i = rsz + 1; (c = str[i]) && isspace(c); i++)
	    PASS;
	if (c == JS_CH(':')) {
	    *bf = malloc(rsz + 1);
	    if (*bf == NULL) {
		throwError(ENOMEM, NULL);
		return -1;
	    }
	    memcpy(*bf, str, rsz);
	    (*bf)[rsz] = 0;
	    return ++i;
	}
    }
    return -1;
}

#define EXP_VAL 0x04
#define EXP_KEY 0x08
#define MBLK_OBJ 0x80
#define MBLK_ARR 0x40
#define EXP_VAL_R_KEY 0x0c
#define NEXP_STR_ARR_OBJ 0x3f
#define MBLK_START 0x01
#define MBLK_CLOSE 0xe1
/* set to accept key or value */
#define R_EXP_KEY EXP_VAL
#define R_EXP_VAL EXP_KEY
#define SET_OPFLAG(AT, FL) (void)((AT) = (((AT) & MBLK_CLOSE) | FL))
/* increments buffer pointer */
#define M0V_BUFFER_PTR(bf, at) ((bf) += (at))
/* assert end of value characters */
#define EOVAL(c)							\
    (!(((c) ^ JS_CH(',')) && ((c) ^ JS_CH('\n')) && ((c) ^ JS_CH('}')) && ((c) ^ JS_CH(']'))))

__NONNULL__ static __inline__ void *lexer(JsonObject obj, JsonString bf, ssize_t sz) {
    register ssize_t objMtch, arrMtch, nkeys, L2, L3, LINE, CH_AT;
    register JsonChar c;
    uint8_t rswitch;

    JsonString key;
    JsonString value;
    LINE = objMtch = arrMtch = rswitch = 0;
    for (L2 = 0; (c = bf[L2]); L2++) {
	if ((c == JS_CH('\n') ? LINE++ : 0) || isspace(c))
	    continue;
	switch (c) {
	case JS_CH('{'):
	    objMtch += 1;
	    if (rswitch & EXP_VAL) {
		//putchar(c);
	    }
	    rswitch |= MBLK_OBJ | MBLK_START | EXP_KEY;
	    break;
	case JS_CH('}'):
	    /* TODO: handle trailing braces @anonymous position */
	    if ((rswitch & EXP_VAL_R_KEY))
		goto syntaxError;
	    if (objMtch > 1)
		//puts("}");
	    rswitch &= MBLK_CLOSE;
	    objMtch -= 1;
	    break;
	case JS_CH(':'):
	    puts("trailing colon");
	    goto syntaxError;
	    break;
	case JS_CH('"'):
	    if (rswitch & EXP_KEY) {
		if ((L3 = getKey(bf + ++L2, &key)) == -1) {
		    goto syntaxError;
		}
		key[L3] = 0;
		M0V_BUFFER_PTR(bf, L2 + L3);
		L2 = -1;
		SET_OPFLAG(rswitch, EXP_VAL);
		//printf("%s: ", key);
	    }
	    else {
		if (!(rswitch & MBLK_START)) {
		    goto syntaxError;
		}
		M0V_BUFFER_PTR(bf, ++L2);
		L2 = 0;
		for (; (c = bf[L2]) && (c != JS_CH('"')); L2++)
		    PASS;
		if (c == 0) {
		    goto syntaxError;
		}
		if ((value = malloc(L2 + 1)) == NULL) {
		    throwError(ENOMEM, NULL);
		    return NULL;
		}
		memcpy(value, bf, L2);
		M0V_BUFFER_PTR(bf, ++L2);
		value[L2] = 0;
		L2 = -1;
		rswitch &= MBLK_CLOSE;

		obj->string(obj, key, value);
		//puts(value);
	    }
	    break;
	case JS_CH('['):
	    if (!(rswitch & MBLK_START)) {
		goto syntaxError;
	    }
	    arrMtch += 1;
	    rswitch |= MBLK_ARR;
	    SET_OPFLAG(rswitch, EXP_VAL);
	    break;
	case JS_CH(']'):
	    if (! --arrMtch) {
		/* close array */
		rswitch &= ~MBLK_ARR;
	    }
	    rswitch &= MBLK_CLOSE;
	    break;
	case JS_CH(','):
	    if (rswitch & EXP_VAL_R_KEY) {
		goto syntaxError;
	    }
	    rswitch & MBLK_ARR ? SET_OPFLAG(rswitch, EXP_VAL) : SET_OPFLAG(rswitch, EXP_KEY);
	    break;
	default:
	    if (!(rswitch & MBLK_START) || (rswitch & EXP_KEY)) {
	    syntaxError:
		throwError(ESYNTAX, NULL, LINE, L2);
		return NULL;
	    }
	    /* TODO: cut out preceeding space */
	    for (L3 = 0; (c = bf[L3]) && !EOVAL(c); L3++)
		putchar(c);
	    bf += L3;
	    L2 = -1;
	    rswitch &= MBLK_CLOSE;
	    //puts("");
	}
    }
    //printf("%ld\n", LINE);
}

void static __inline__ __jsonError__(int errn, char * __restrict line, char * __restrict objf, char *obj) {
}

/* HASHES */
#ifdef __UINT64_T__
#define HASH_int uint64_t
#define HASH_c1
#define HASH_c2
#define HASH_c3
#else
#define HASH_int uint32_t
#define HASH_c1
#define HASH_c2
#define HASH_c3
#endif
__NONNULL__ __attribute__((pure)) HASH_int getHashKeyFromByte (void *byte, ssize_t len, uint32_t seed) {
    HASH_int c1;

    c1 = 57722121L;
    while (len--) {
	c1 ^= *(char *)byte++ * 23423491L;
	c1 = (c1 * (seed << 13)) ^ 123;
	c1 *= 234239473L;
    }
    return (c1 ^ 9697619231L) ^ (c1 << 13), c1 ^= 11;
}

#define jsonGetIndex(key, bkt) (size_t)(getHashKeyFromByte((key), Json_strLen((key)), 123) % (bkt))

__NONNULL__ void *addStringToHashTable (JsonObject self, JsonString key, void *value) {
    register size_t _atIndex = 1;
    JsonData newData;

    if (*key == 0)
	return self;
    if (! (newData = malloc(SIZEOF_JSON_DATA))) {
	errno = ENOMEM;
	return self;
    }
    newData->__key = key;
    newData->__str = value;
    newData->__type = JSONSTRING_TYPE;
    _atIndex = jsonGetIndex(key, self->__hashBkt);

    if (self->__head[_atIndex] == NULL) {
	newData->__pd = newData->__nd = NULL;
	self->__head[_atIndex] = self->__tail[_atIndex] = newData;
    }
    else {
	newData->__pd = self->__tail[_atIndex];
	newData->__nd = NULL;
	self->__tail[_atIndex] = newData;
    }
    (self->__sz[_atIndex])++;
    return self;
}
#define IGNORE_EXPR(expr) ((void)(expr))
__NONNULL__ void *removeFromHashTable (JsonObject self, JsonString key) {
    register size_t _fromIndex;
    JsonData iterItems;

    if (*key == 0)
	return self;
    _fromIndex = jsonGetIndex(key, self->__hashBkt);

    if (self->__head[_fromIndex] == NULL)
	goto error;

    iterItems = self->__head[_fromIndex];
    while (iterItems != NULL) {
	if (! strcmp(key, iterItems->__key)) {
	    IGNORE_EXPR(iterItems->__pd ? iterItems->__pd->__nd = iterItems->__nd : 0);
	    IGNORE_EXPR(iterItems->__nd ? iterItems->__nd->__pd = iterItems->__pd : 0);
	    break;
	}
	iterItems = iterItems->__nd;
    }
    if (iterItems == NULL) {
    error:
	throwError(EINVAL, "%s does not exist", key);
	return NULL;
    }
    if (--(self->__sz[_fromIndex]) == 0)
	self->__head[_fromIndex] = self->__tail[_fromIndex] = NULL;
    return iterItems;
}
__NONNULL__ static __inline__ void *HashTableToListMap (JsonObject self) {
    register size_t tableSize, i;

    tableSize = self->__hashBkt - 1;

    for (i = 0; i < tableSize; i++) {
	self->__tail[i]->__nd = self->__tail[i + 1];
	self->__head[i + 1]->__pd = self->__tail[i];
    }
    return self->__head[0];
}
__NONNULL__ static __inline__ void *ListMapToHsashTable (JsonObject self) {
    register size_t tableSize, i;

    tableSize = self->__hashBkt - 1;

    for (i = 0; i < tableSize; i++) {
	self->__head[i]->__pd = self->__tail[i]->__nd = NULL;
    }
}
__NONNULL__ static __inline__ ssize_t dump(JsonObject self, ...) {
#if JSON_DOBUFFERING
    dumpbf(self);
#else
#endif
}
typedef struct {
    uint8_t indent_char;
    uint8_t indent_width;
    uint8_t sort;
    uint8_t raw;
} printfmt;

/* ... (tab), indent char */
__NONNULL__ ssize_t dumpbf(JsonObject self, printfmt *fmt) {
    JsonString buf;
    jsonData data;
    size_t szbuf, sztbe;

    szbuf = self->__szbuf;
    if (!self->__buf && !(self->__buf = malloc((szbuf = sizeof(JsonChar) * 1024)))) {
	throwError(ENOMEM, NULL);
	return -1;
    }
    self->__szbuf = szbuf;
    buf = self->__buf;

    *buf++ = JS_CH('{');
    *buf++ = JS_CH('\n');
    szbuf -= 2;

    data = self->__head[0];
    while ((sztbe < self->__hashBkt) || data) {
	if (data == NULL) {
	    data = self->__head[sztbe++];
	    continue;
	}
	do {
	    *buf++ = fmt->indent_character;
	    szbuf--;
	} while (--fmt->indent_width);
	/* TODO: REMOVE ASSUMPTIONS: sz is sufficient and no error */
	buf += snprintf(buf, szbuf - 2, "%s: %s,\n", data->__key, data->__str);
	data = data->__nd;
    }
    *buf++ = JS_CH('}');
    *buf++ = JS_CH('\n');
    *buf = 0;

    szbuf = fprintf(stdout, self->__buf);
    return szbuf;
}
__NONNULL__ void JsonMemcpy(void *dest, void *src, ssize_t sz) {
    ssize_t chk;
}

int main(int argc, char **argv) {
    int fd;
    char bf[1025];
    ssize_t nr;

    if (argc != 2) {
	return -1;
    }

    if ((fd = open(argv[argc - 1], O_RDONLY)) == -1) {
	fprintf(stderr, "open: %s\n", strerror(errno));
	return -1;
    }
    if ((nr = read(fd, bf, 1024)) == -1) {
	fprintf(stderr, "%s\n", strerror(errno));
	return -1;
    }
    bf[nr] = 0;
    /*	if (lexer((JsonObject)1, bf, nr) == NULL) {
	puts("error");
	}*/
    JsonObject myjson = JsonOpen("tjs.json");
    if (myjson == NULL) {
	perror("");
    }
    JsonData d;
    puts((d = myjson->remove(myjson, "debug")) ? d->__str : (void *)"null");
    return 0;
}

