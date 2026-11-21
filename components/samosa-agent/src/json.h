/* Bounded strict JSON transport primitive, locally owned. */
#ifndef SOURCES_JSON_H
#define SOURCES_JSON_H
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

typedef enum { JT_OBJECT, JT_ARRAY, JT_STRING, JT_NUMBER, JT_BOOL, JT_NULL } JsonType;
typedef struct { JsonType type; size_t begin, end; int next, count; } JsonToken;
#ifndef JSON_TOKENS_MAX
#define JSON_TOKENS_MAX 256
#endif
typedef struct { const char *text; size_t length, offset; int used; JsonToken tokens[JSON_TOKENS_MAX]; } Json;
static inline int utf8_valid(const unsigned char *s, size_t n) {
    for (size_t i=0;i<n;) {
        unsigned c=s[i++], cp, need;
        if (c<128) { if (!c) return 0; continue; }
        if (c>=0xc2 && c<=0xdf) { cp=c&31; need=1; }
        else if (c>=0xe0 && c<=0xef) { cp=c&15; need=2; }
        else if (c>=0xf0 && c<=0xf4) { cp=c&7; need=3; }
        else return 0;
        if (n-i<need) return 0;
        for (unsigned j=0;j<need;j++) { c=s[i++]; if ((c&192)!=128) return 0; cp=(cp<<6)|(c&63); }
        if ((need==1 && cp<128)||(need==2 && cp<2048)||(need==3 && cp<65536)||cp>0x10ffff||(cp>=0xd800 && cp<=0xdfff)) return 0;
    }
    return 1;
}
static inline int json_hex(char c) {
    if (c>='0' && c<='9') return c-'0';
    if (c>='a' && c<='f') return c-'a'+10;
    if (c>='A' && c<='F') return c-'A'+10;
    return -1;
}
static inline int json_codepoint(const char *s, size_t n, size_t *i, unsigned *cp) {
    if (n-*i<4) return 0;
    unsigned v=0;
    for (int j=0;j<4;j++) { int h=json_hex(s[(*i)++]); if(h<0) return 0; v=(v<<4)|(unsigned)h; }
    if (v>=0xd800 && v<=0xdbff) {
        if (n-*i<6 || s[*i]!='\\' || s[*i+1]!='u') return 0;
        *i+=2; unsigned lo=0;
        for(int j=0;j<4;j++) { int h=json_hex(s[(*i)++]); if(h<0) return 0; lo=(lo<<4)|(unsigned)h; }
        if(lo<0xdc00 || lo>0xdfff) return 0;
        v=0x10000+((v-0xd800)<<10)+(lo-0xdc00);
    } else if(v>=0xdc00 && v<=0xdfff) return 0;
    if(!v) return 0; /* Embedded NUL has no representation in C contract strings. */
    *cp=v; return 1;
}
static inline void json_space(Json *j) {
    while(j->offset<j->length && strchr(" \t\r\n",j->text[j->offset])) j->offset++;
}
static inline char *json_string(const Json *j, int t) {
    if(t<0 || j->tokens[t].type!=JT_STRING) return NULL;
    size_t begin=j->tokens[t].begin+1, end=j->tokens[t].end-1, w=0;
    char *out=malloc(end-begin+1); if(!out) return NULL;
    for(size_t i=begin;i<end;) {
        unsigned char c=(unsigned char)j->text[i++];
        if(c!='\\') { out[w++]=(char)c; continue; }
        c=(unsigned char)j->text[i++];
        if(c=='u') {
            unsigned cp;
            if(!json_codepoint(j->text,end,&i,&cp)) { free(out); return NULL; }
            if(cp<128) out[w++]=(char)cp;
            else if(cp<2048) { out[w++]=(char)(192|(cp>>6)); out[w++]=(char)(128|(cp&63)); }
            else if(cp<65536) { out[w++]=(char)(224|(cp>>12)); out[w++]=(char)(128|((cp>>6)&63)); out[w++]=(char)(128|(cp&63)); }
            else { out[w++]=(char)(240|(cp>>18)); out[w++]=(char)(128|((cp>>12)&63)); out[w++]=(char)(128|((cp>>6)&63)); out[w++]=(char)(128|(cp&63)); }
        } else {
            switch(c) { case 'n':c='\n';break;case 'r':c='\r';break;case 't':c='\t';break;case 'b':c='\b';break;case 'f':c='\f';break;default:break; }
            out[w++]=(char)c;
        }
    }
    out[w]=0; return out;
}
static inline int json_value(Json *j, int depth) {
    json_space(j);
    if(depth>16 || j->used==JSON_TOKENS_MAX || j->offset==j->length) return -1;
    int t=j->used++; JsonToken *v=&j->tokens[t];
    v->begin=j->offset; char c=j->text[j->offset++];
    if(c=='{' || c=='[') {
        v->type=c=='{'?JT_OBJECT:JT_ARRAY; char close=c=='{'?'}':']';
        json_space(j);
        if(j->offset<j->length && j->text[j->offset]==close) j->offset++;
        else for(;;) {
            if(c=='{') {
                json_space(j);
                if(j->offset==j->length || j->text[j->offset]!='"') return -1;
                int key=json_value(j,depth+1); if(key<0) return -1;
                char *name=json_string(j,key); if(!name) return -1;
                for(int prior=t+1;prior<key;) {
                    char *old=json_string(j,prior);
                    int duplicate=old && !strcmp(old,name); free(old);
                    if(duplicate) { free(name); return -1; }
                    prior=j->tokens[j->tokens[prior].next].next;
                }
                free(name); json_space(j);
                if(j->offset==j->length || j->text[j->offset++]!=':') return -1;
            }
            if(json_value(j,depth+1)<0) return -1;
            v->count++; json_space(j);
            if(j->offset==j->length) return -1;
            char sep=j->text[j->offset++]; if(sep==close) break;
            if(sep!=',') return -1;
        }
    } else if(c=='"') {
        v->type=JT_STRING; int closed=0;
        while(j->offset<j->length) {
            unsigned char ch=(unsigned char)j->text[j->offset++];
            if(ch=='"') { closed=1;break; }
            if(ch<32) return -1;
            if(ch=='\\') {
                if(j->offset==j->length) return -1;
                ch=(unsigned char)j->text[j->offset++];
                if(ch=='u') { unsigned cp; if(!json_codepoint(j->text,j->length,&j->offset,&cp)) return -1; }
                else if(!strchr("\"\\/bfnrt",ch)) return -1;
            }
        }
        if(!closed) return -1;
    } else if(c=='t' || c=='f' || c=='n') {
        const char *word=c=='t'?"true":c=='f'?"false":"null"; size_t n=strlen(word);
        if(j->length-v->begin<n || memcmp(j->text+v->begin,word,n)) return -1;
        j->offset=v->begin+n; v->type=c=='n'?JT_NULL:JT_BOOL;
    } else {
        v->type=JT_NUMBER; j->offset=v->begin;
        if(j->text[j->offset]=='-') j->offset++;
        if(j->offset==j->length) return -1;
        if(j->text[j->offset]=='0') j->offset++;
        else {
            if(j->text[j->offset]<'1'||j->text[j->offset]>'9') return -1;
            while(j->offset<j->length && isdigit((unsigned char)j->text[j->offset])) j->offset++;
        }
        if(j->offset<j->length && j->text[j->offset]=='.') {
            j->offset++; size_t start=j->offset;
            while(j->offset<j->length && isdigit((unsigned char)j->text[j->offset])) j->offset++;
            if(start==j->offset) return -1;
        }
        if(j->offset<j->length && (j->text[j->offset]=='e'||j->text[j->offset]=='E')) {
            j->offset++; if(j->offset<j->length && (j->text[j->offset]=='+'||j->text[j->offset]=='-')) j->offset++;
            size_t start=j->offset;
            while(j->offset<j->length && isdigit((unsigned char)j->text[j->offset])) j->offset++;
            if(start==j->offset) return -1;
        }
    }
    v->end=j->offset; v->next=j->used; return t;
}
static inline int json_parse(Json *j, const char *text, size_t length) {
    memset(j,0,sizeof(*j)); j->text=text; j->length=length;
    if(!utf8_valid((const unsigned char*)text,length) || json_value(j,0)!=0) return 0;
    json_space(j); return j->offset==length;
}
static inline int json_get(const Json *j, int object, const char *name) {
    if(object<0 || j->tokens[object].type!=JT_OBJECT) return -1;
    for(int t=object+1;t<j->tokens[object].next;) {
        char *key=json_string(j,t); int value=j->tokens[t].next;
        int equal=key && !strcmp(key,name); free(key);
        if(equal) return value;
        t=j->tokens[value].next;
    }
    return -1;
}
static inline int json_uint(const Json *j, int t, size_t *out) {
    if(t<0 || j->tokens[t].type!=JT_NUMBER) return 0;
    size_t n=j->tokens[t].end-j->tokens[t].begin; if(n==0 || n>20) return 0;
    size_t value=0;
    for(size_t i=j->tokens[t].begin;i<j->tokens[t].end;i++) {
        unsigned c=(unsigned char)j->text[i]; if(c<'0'||c>'9'||value>(SIZE_MAX-(c-'0'))/10) return 0;
        value=value*10+(c-'0');
    }
    *out=value; return 1;
}
static inline void json_quote(FILE *out, const void *bytes, size_t n) {
    const unsigned char *s=bytes; fputc('"',out);
    for(size_t i=0;i<n;i++) {
        unsigned c=s[i];
        if(c=='"'||c=='\\') { fputc('\\',out); fputc((int)c,out); }
        else if(c<32) fprintf(out,"\\u%04x",c);
        else fputc((int)c,out);
    }
    fputc('"',out);
}
#endif
