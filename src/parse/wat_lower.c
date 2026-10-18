/*
 * SPDX-License-Identifier: LGPL-3.0-only
 * Copyright (C) 2026 Corey Pennycuff
 */
#include <ghoti.io/lang-wasm/parse.h>
#include <ghoti.io/lang-wasm/policy.h>
#include "wat_lower_internal.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <math.h>

/*
 * Deliberately small first lowering slice.  It lowers the core function
 * section grammar (numeric params/results, locals, constants and numeric
 * operators) to canonical Wasm sections, then runs the normal decoder and
 * validator. Keeping this path decoder-backed ensures text-built modules
 * share the binary frontend's semantic checks.
 */
typedef struct Bytes { uint8_t *p; size_t n, cap; } Bytes;
typedef struct NameRef { const GLWASM_Text *node; uint32_t index; } NameRef;
typedef struct TagInfo { const GLWASM_Text *node; uint32_t index, type_index; } TagInfo;
static void malformed_error(GLWASM_ParseError *e,const GLWASM_Text *n,
    const char *message);
static size_t implicit_signature_count(const GLWASM_Text *root,size_t first);
static int put(Bytes *b, uint8_t x) {
  if (b->n == b->cap) {
    size_t c = b->cap ? b->cap * 2 : 64;
    if (c < b->cap) return 0;
    void *p = realloc(b->p, c);
    if (!p) return 0;
    b->p = p; b->cap = c;
  }
  b->p[b->n++] = x; return 1;
}
static int bytes(Bytes *b, const uint8_t *p, size_t n) {
  for (size_t i=0;i<n;i++) if (!put(b,p[i])) return 0;
  return 1;
}
static int uleb(Bytes *b, uint32_t v) {
  do { uint8_t x=(uint8_t)(v&127); v >>= 7; if(v)x|=128; if(!put(b,x))return 0; } while(v);
  return 1;
}
static int uleb64(Bytes *b, uint64_t v) {
  do { uint8_t x=(uint8_t)(v&127); v >>= 7; if(v)x|=128; if(!put(b,x))return 0; } while(v);
  return 1;
}
static int sleb(Bytes *b, int64_t v) {
  int more=1;
  while(more) {
    uint8_t x=(uint8_t)(v&127); int sign=(x&64)!=0; v >>= 7;
    more=!((v==0&&!sign)||(v==-1&&sign)); if(more)x|=128;
    if(!put(b,x))return 0;
  }
  return 1;
}
static int section(Bytes *out,uint8_t id,Bytes *payload) {
  return put(out,id)&&uleb(out,(uint32_t)payload->n)&&bytes(out,payload->p,payload->n);
}
static const GLWASM_Text *at(const GLWASM_Text*n,size_t i){return glwasm_text_at(n,i);}
static int atom(const GLWASM_Text*n,const char*s){size_t z;const uint8_t*p=glwasm_text_bytes(n,&z);return p&&z==strlen(s)&&!memcmp(p,s,z);}
static int indirect_atom(const GLWASM_Text*n){return atom(n,"call_indirect")||atom(n,"return_call_indirect");}
static int deferred_instruction_name(const char*s){
  return !strncmp(s,"struct.",7)||!strncmp(s,"array.",6)||
    !strncmp(s,"any.",4)||!strncmp(s,"i31.",4)||
    !strncmp(s,"ref.cast",8)||!strncmp(s,"ref.test",8)||
    !strncmp(s,"br_on_cast",10)||!strncmp(s,"br_on_non_null",14)||
    !strncmp(s,"br_on_null",10)||!strcmp(s,"call_ref")||
    !strcmp(s,"return_call_ref");
}
static size_t untyped_indirect_count(const GLWASM_Text*n){
  size_t count=0;
  if(indirect_atom(n))return 1;
  if(indirect_atom(at(n,0))){
    int typed=0;for(size_t i=1;i<glwasm_text_size(n);i++)if(atom(at(at(n,i),0),"type"))typed=1;
    if(!typed)count++;
  }
  for(size_t i=glwasm_text_kind(n)==GLWASM_TEXT_LIST?1:0;i<glwasm_text_size(n);i++){size_t child=untyped_indirect_count(at(n,i));if(child>SIZE_MAX-count)return SIZE_MAX;count+=child;}
  return count;
}
static int map_untyped_indirects(const GLWASM_Text*n,NameRef*refs,size_t*count,size_t capacity,uint32_t base){
  if(indirect_atom(n)){
    size_t index=*count;if(index>=capacity||index>UINT32_MAX-base)return 0;refs[index]=(NameRef){n,base+(uint32_t)index};*count=index+1;return 1;
  }
  if(indirect_atom(at(n,0))){
    int typed=0;for(size_t i=1;i<glwasm_text_size(n);i++)if(atom(at(at(n,i),0),"type"))typed=1;
    if(!typed){size_t index=*count;if(index>=capacity||index>UINT32_MAX-base)return 0;refs[index]=(NameRef){n,base+(uint32_t)index};*count=index+1;}
  }
  for(size_t i=glwasm_text_kind(n)==GLWASM_TEXT_LIST?1:0;i<glwasm_text_size(n);i++)if(!map_untyped_indirects(at(n,i),refs,count,capacity,base))return 0;
  return 1;
}
static int annotation_node(const GLWASM_Text*n){
  const GLWASM_Text*h=at(n,0);size_t z=0;const uint8_t*p=glwasm_text_bytes(h,&z);
  return glwasm_text_kind(n)==GLWASM_TEXT_LIST&&p&&z&&p[0]=='@';
}
static int valid_annotations(const GLWASM_Text*n,GLWASM_ParseError*e){
  if(annotation_node(n)){
    const GLWASM_Text*head=at(n,0);size_t z=0;const uint8_t*p=glwasm_text_bytes(head,&z);
    if(!p||z<2){malformed_error(e,n,"empty annotation id");return 0;}
    for(size_t i=0;i<glwasm_text_size(n);i++){
      const GLWASM_Text*child=at(n,i);size_t len=0;const uint8_t*bytes=glwasm_text_bytes(child,&len);
      if(i==0)bytes=p,len=z;
      for(size_t j=0;bytes&&j<len;j++)if(bytes[j]>=0x80){malformed_error(e,child,"illegal character in annotation");return 0;}
    }
  }
  for(size_t i=0;i<glwasm_text_size(n);i++)if(!valid_annotations(at(n,i),e))return 0;
  return 1;
}
static int valid_utf8_name(const uint8_t*p,size_t n){
  for(size_t i=0;i<n;){
    uint8_t c=p[i++];if(c<0x80)continue;
    uint32_t value;size_t extra;
    if(c>=0xc2&&c<=0xdf){value=c&0x1f;extra=1;}
    else if(c>=0xe0&&c<=0xef){value=c&0x0f;extra=2;}
    else if(c>=0xf0&&c<=0xf4){value=c&0x07;extra=3;}
    else return 0;
    if(i+extra>n)return 0;
    for(size_t j=0;j<extra;j++){uint8_t d=p[i++];if((d&0xc0)!=0x80)return 0;value=(value<<6)|(d&0x3f);}
    if((extra==1&&value<0x80)||(extra==2&&value<0x800)||
       (extra==3&&value<0x10000)||value>0x10ffff||
       (value>=0xd800&&value<=0xdfff))return 0;
  }
  return 1;
}
static int valid_wat_atoms(const GLWASM_Text*n,GLWASM_ParseError*e){
  if(glwasm_text_kind(n)==GLWASM_TEXT_ATOM){
    size_t z=0;const uint8_t*p=glwasm_text_bytes(n,&z);
    if(p&&z==1&&p[0]=='$'){
      if(e){e->result=GLWASM_ERR_CORRUPT;e->location=glwasm_text_location(n);e->message="empty identifier";}
      return 0;
    }
    if(p&&z&&p[0]=='$'&&!valid_utf8_name(p+1,z-1)){
      if(e){e->result=GLWASM_ERR_CORRUPT;e->location=glwasm_text_location(n);e->message="malformed UTF-8 in identifier";}
      return 0;
    }
    for(size_t i=0;p&&i<z&&(!z||p[0]!='$');i++)if(p[i]<0x20||p[i]==0x7f){
      if(e){e->result=GLWASM_ERR_CORRUPT;e->location=glwasm_text_location(n);e->message="illegal control character in WAT token";}
      return 0;
    }
  }
  for(size_t i=0;i<glwasm_text_size(n);i++)if(!valid_wat_atoms(at(n,i),e))return 0;
  return 1;
}
static int valid_wat_source(const uint8_t*s,size_t n,GLWASM_ParseError*e){
  uint32_t line=1,column=1;
  for(size_t i=0;i<n;){
    uint8_t c=s[i];size_t width=1;
    if(c=='('){size_t j=i+1;while(j<n&&(s[j]==' '||s[j]=='\t'||s[j]=='\r'||s[j]=='\n'))j++;if(j>i+1&&j<n&&s[j]=='@'){
      if(e){e->result=GLWASM_ERR_CORRUPT;e->location=(GLWASM_SourceLoc){i,line,column};e->message="unknown operator before annotation";}
      return 0;
    }}
    if((c<0x20&&c!='\t'&&c!='\n'&&c!='\r')||c==0x7f){
      if(e){e->result=GLWASM_ERR_CORRUPT;e->location=(GLWASM_SourceLoc){i,line,column};e->message="illegal control character in WAT source";}
      return 0;
    }
    if(c>=0x80){
      uint32_t value;size_t need;
      if(c>=0xc2&&c<=0xdf){value=c&0x1f;need=1;}
      else if(c>=0xe0&&c<=0xef){value=c&0x0f;need=2;}
      else if(c>=0xf0&&c<=0xf4){value=c&0x07;need=3;}
      else need=SIZE_MAX;
      if(need==SIZE_MAX||i+need>=n){
        if(e){e->result=GLWASM_ERR_CORRUPT;e->location=(GLWASM_SourceLoc){i,line,column};e->message="malformed UTF-8 in WAT source";}
        return 0;
      }
      width=need+1;
      for(size_t j=1;j<=need;j++){
        uint8_t tail=s[i+j];
        if((tail&0xc0)!=0x80){
          if(e){e->result=GLWASM_ERR_CORRUPT;e->location=(GLWASM_SourceLoc){i,line,column};e->message="malformed UTF-8 in WAT source";}
          return 0;
        }
        value=(value<<6)|(tail&0x3f);
      }
      if((width==3&&value<0x800)||(width==4&&value<0x10000)||
         (value>=0xd800&&value<=0xdfff)||value>0x10ffff){
        if(e){e->result=GLWASM_ERR_CORRUPT;e->location=(GLWASM_SourceLoc){i,line,column};e->message="malformed UTF-8 in WAT source";}
        return 0;
      }
    }
    for(size_t j=0;j<width;j++){
      if(s[i+j]=='\n'){line++;column=1;}else column++;
    }
    i+=width;
  }
  /* WAT strings and atoms are separate lexical tokens.  The generic S
     expression reader intentionally permits adjacent nodes, so enforce the
     WAT separator rule here (parentheses and comments are delimiters too). */
  for(size_t i=0;i<n;i++){
    if(i+1<n&&s[i]==';'&&s[i+1]==';'){
      i+=2;while(i<n&&s[i]!='\n')i++;continue;
    }
    if(i+1<n&&s[i]=='('&&s[i+1]==';'){
      size_t depth=1;i+=2;
      while(i<n&&depth){
        if(i+1<n&&s[i]=='('&&s[i+1]==';'){depth++;i+=2;}
        else if(i+1<n&&s[i]==';'&&s[i+1]==')'){depth--;i+=2;}
        else i++;
      }
      continue;
    }
    if(s[i]!='"')continue;
    size_t j=i+1;int escaped=0;
    while(j<n){
      if(escaped){escaped=0;j++;continue;}
      if(s[j]=='\\'){escaped=1;j++;continue;}
      if(s[j]=='"')break;
      j++;
    }
    if(j==n)break; /* The text parser reports the unterminated string. */
    int in_annotation=0;size_t mark=SIZE_MAX;
    for(size_t k=0;k+1<i;k++)if(s[k]=='('&&s[k+1]=='@')mark=k;
    if(mark!=SIZE_MAX){size_t depth=1;int quoted=0,escape=0;for(size_t k=mark+2;k<i;k++){
      if(quoted){if(escape)escape=0;else if(s[k]=='\\')escape=1;else if(s[k]=='"')quoted=0;continue;}
      if(s[k]=='"'){quoted=1;continue;}if(s[k]=='(')depth++;else if(s[k]==')'&&depth)depth--;
    }in_annotation=depth!=0;}
    if(in_annotation){i=j;continue;}
    int before_ok=i==0||s[i-1]=='$'||s[i-1]==' '||s[i-1]=='\t'||s[i-1]=='\r'||s[i-1]=='\n'||s[i-1]=='('||s[i-1]==')'||
      (i>=2&&s[i-2]==';'&&s[i-1]==';')||(i>=2&&s[i-2]=='('&&s[i-1]==';');
    int after_ok=j+1==n||s[j+1]==' '||s[j+1]=='\t'||s[j+1]=='\r'||s[j+1]=='\n'||s[j+1]=='('||s[j+1]==')'||
      (j+2<n&&s[j+1]==';'&&s[j+2]==';')||(j+2<n&&s[j+1]==';'&&s[j+2]==')');
    if(!before_ok||!after_ok){
      if(e){uint32_t ln=1,col=1;for(size_t k=0;k<i;k++){if(s[k]=='\n'){ln++;col=1;}else col++;}e->result=GLWASM_ERR_CORRUPT;e->location=(GLWASM_SourceLoc){i,ln,col};e->message="missing separator around WAT string";}
      return 0;
    }
    i=j;
  }
  return 1;
}
static uint8_t*strip_wat_annotations(const uint8_t*s,size_t n){
  uint8_t*out=malloc(n?n:1);if(!out)return NULL;if(n)memcpy(out,s,n);
  int string=0,escape=0,line_comment=0;size_t block_comment=0;
  for(size_t i=0;i<n;i++){
    uint8_t c=s[i];
    if(line_comment){if(c=='\n')line_comment=0;continue;}
    if(block_comment){
      if(c=='('&&i+1<n&&s[i+1]==';'){block_comment++;i++;continue;}
      if(c==';'&&i+1<n&&s[i+1]==')'){block_comment--;i++;continue;}
      continue;
    }
    if(string){if(escape)escape=0;else if(c=='\\')escape=1;else if(c=='"')string=0;continue;}
    if(c=='"'){string=1;continue;}
    if(c==';'&&i+1<n&&s[i+1]==';'){line_comment=1;i++;continue;}
    if(c=='('&&i+1<n&&s[i+1]==';'){block_comment=1;i++;continue;}
    if(c=='('&&i+2<n&&s[i+1]=='@'){
      size_t end=i+1,depth=1,comment_depth=0;int in_string=0,in_escape=0,in_line=0;
      for(end=i+1;end<n&&depth;end++){
        uint8_t a=s[end];
        if(in_line){if(a=='\n')in_line=0;continue;}
        if(comment_depth){
          if(a=='('&&end+1<n&&s[end+1]==';'){comment_depth++;end++;continue;}
          if(a==';'&&end+1<n&&s[end+1]==')'){comment_depth--;end++;continue;}
          continue;
        }
        if(in_string){if(in_escape)in_escape=0;else if(a=='\\')in_escape=1;else if(a=='"')in_string=0;continue;}
        if(a=='"'){in_string=1;continue;}
        if(a==';'&&end+1<n&&s[end+1]==';'){in_line=1;end++;continue;}
        if(a=='('&&end+1<n&&s[end+1]==';'){comment_depth=1;end++;continue;}
        if(a=='(')depth++;else if(a==')')depth--;
      }
      if(!depth)for(size_t j=i;j<end;j++)if(out[j]!='\n')out[j]=' ';
    }
  }
  return out;
}
static int tree_has_atom(const GLWASM_Text*n,const char*s){if(atom(n,s))return 1;for(size_t i=0;i<glwasm_text_size(n);i++)if(tree_has_atom(at(n,i),s))return 1;return 0;}
static int typebyte(const GLWASM_Text*n,uint8_t*out){
  if(atom(n,"i32"))*out=0x7f;
  else if(atom(n,"i64"))*out=0x7e;
  else if(atom(n,"f32"))*out=0x7d;
  else if(atom(n,"f64"))*out=0x7c;
  else if(atom(n,"funcref"))*out=0x70;
  else if(atom(n,"externref"))*out=0x6f;
  else if(atom(n,"exnref")||atom(n,"exn"))*out=0x69;
  else if(atom(at(n,0),"ref")){
    size_t i=1;
    if(atom(at(n,i),"null"))i++;
    if(i+1!=glwasm_text_size(n))return 0;
    if(atom(at(n,i),"func"))*out=0x70;
    else if(atom(at(n,i),"extern"))*out=0x6f;
    else if(atom(at(n,i),"exn"))*out=0x69;
    else return 0;
  }
  else return 0;
  return 1;
}
static int resolve_index(const GLWASM_Text*n,const NameRef*refs,size_t count,uint32_t*out);
static int table_reftype(Bytes*b,const GLWASM_Text*n,const NameRef*types,size_t ntypes){
  uint8_t short_type;int nullable=1;const GLWASM_Text*heap=n;
  if(typebyte(n,&short_type)&&glwasm_text_kind(n)!=GLWASM_TEXT_LIST)return put(b,short_type);
  if(!atom(at(n,0),"ref"))return 0;
  size_t i=1;if(atom(at(n,i),"null"))i++;else nullable=0;
  if(i+1!=glwasm_text_size(n))return 0;
  heap=at(n,i);
  int64_t ht;
  if(atom(heap,"func"))ht=-16;
  else if(atom(heap,"extern"))ht=-17;
  else if(atom(heap,"any"))ht=-18;
  else if(atom(heap,"eq"))ht=-19;
  else if(atom(heap,"i31"))ht=-20;
  else if(atom(heap,"struct"))ht=-21;
  else if(atom(heap,"array"))ht=-22;
  else if(atom(heap,"exn"))ht=-23;
  else {uint32_t ix;if(!resolve_index(heap,types,ntypes,&ix))return 0;ht=ix;}
  if(nullable&&(ht==-16||ht==-17||ht==-23))return put(b,ht==-16?0x70:ht==-17?0x6f:0x69);
  return put(b,nullable?0x63:0x64)&&sleb(b,ht);
}
static int value_type(Bytes*b,const GLWASM_Text*n,const NameRef*types,size_t ntypes){
  uint8_t t;
  if(glwasm_text_kind(n)!=GLWASM_TEXT_LIST&&typebyte(n,&t))return put(b,t);
  return table_reftype(b,n,types,ntypes);
}
static int parse_u32(const GLWASM_Text*n,uint32_t*out);
static int parse_u64(const GLWASM_Text*n,uint64_t*out);
static int named_atom(const GLWASM_Text*n);
static const GLWASM_Text*inline_import(const GLWASM_Text*field){
  for(size_t i=1;i<glwasm_text_size(field);i++)
    if(atom(at(at(field,i),0),"import"))return at(field,i);
  return NULL;
}
static int tabletype(const GLWASM_Text*t,Bytes*b,const NameRef*types,size_t ntypes){
  size_t z,ix=1;const uint8_t*p;uint64_t min,max=0;int addr64=0;
  if(named_atom(at(t,ix)))ix++;
  while(ix<glwasm_text_size(t)&&(atom(at(at(t,ix),0),"import")||atom(at(at(t,ix),0),"export")))ix++;
  if(atom(at(t,ix),"i64")){addr64=1;ix++;}
  size_t n=glwasm_text_size(t);if((n!=ix+2&&n!=ix+3)||!parse_u64(at(t,ix),&min))return 0;
  int hasmax=n==ix+3;if(hasmax&&!parse_u64(at(t,ix+1),&max))return 0;
  p=glwasm_text_bytes(at(t,n-1),&z);
  if((!p||!z)&&glwasm_text_kind(at(t,n-1))!=GLWASM_TEXT_LIST)return 0;
  return table_reftype(b,at(t,n-1),types,ntypes)&&uleb(b,(hasmax?1u:0u)|(addr64?4u:0u))&&uleb64(b,min)&&(!hasmax||uleb64(b,max));
}
static int memorytype(const GLWASM_Text*m,Bytes*b){
  size_t ix=1;int addr64=0;uint64_t min,max=0;size_t n=glwasm_text_size(m);
  if(named_atom(at(m,ix)))ix++;
  while(ix<n&&(atom(at(at(m,ix),0),"import")||atom(at(at(m,ix),0),"export")))ix++;
  if(atom(at(m,ix),"i64")){addr64=1;ix++;}
  int hasmax=n==ix+2;
  if((n!=ix+1&&!hasmax)||!parse_u64(at(m,ix),&min)||(hasmax&&!parse_u64(at(m,ix+1),&max)))return 0;
  return uleb(b,(hasmax?1u:0u)|(addr64?4u:0u))&&uleb64(b,min)&&(!hasmax||uleb64(b,max));
}
static int globaltype(const GLWASM_Text*g,Bytes*b,const NameRef*types,size_t ntypes){
  size_t ix=1;if(named_atom(at(g,ix)))ix++;
  const GLWASM_Text*d=at(g,ix);int mut=atom(at(d,0),"mut");
  return value_type(b,mut?at(d,1):d,types,ntypes)&&put(b,mut?1:0);
}
static int emit_signature(Bytes*types,const GLWASM_Text*f,GLWASM_ParseError*e,const NameRef*typenames,size_t ntypenames){
  Bytes params={0},results={0};uint32_t nparams=0,nresults=0;int ok=0;
  for(size_t j=1;j<glwasm_text_size(f);j++){
    const GLWASM_Text*x=at(f,j);
    if(atom(x,"$"))continue;
    if(atom(at(x,0),"param")){
      for(size_t k=1;k<glwasm_text_size(x);k++){
        if(named_atom(at(x,k)))k++;
        if(k>=glwasm_text_size(x)||nparams==UINT32_MAX||!value_type(&params,at(x,k),typenames,ntypenames)){
          if(e){e->result=GLWASM_ERR_UNSUPPORTED;e->location=glwasm_text_location(x);e->message="unsupported function parameter type";}
          goto done;
        }
        nparams++;
      }
    }
    else if(atom(at(x,0),"result")){
      for(size_t k=1;k<glwasm_text_size(x);k++){
        if(nresults==UINT32_MAX||!value_type(&results,at(x,k),typenames,ntypenames)){
          if(e){e->result=GLWASM_ERR_UNSUPPORTED;e->location=glwasm_text_location(x);e->message="unsupported function result type";}
          goto done;
        }
        nresults++;
      }
    }
    else if(atom(at(x,0),"type")){
      if(e){e->result=GLWASM_ERR_UNSUPPORTED;e->location=glwasm_text_location(x);e->message="function type uses are not yet supported by WAT lowering";}
      goto done;
    }
  }
  ok=put(types,0x60)&&uleb(types,nparams)&&bytes(types,params.p,params.n)&&
     uleb(types,nresults)&&bytes(types,results.p,results.n);
done:
  free(params.p);free(results.p);return ok;
}
static int emit_function_signature(Bytes*types,const GLWASM_Text*f,GLWASM_ParseError*e,const NameRef*typenames,size_t ntypenames){
  Bytes params={0},results={0};uint32_t nparams=0,nresults=0;int ok=0;
  for(size_t j=1;j<glwasm_text_size(f);j++){
    const GLWASM_Text*x=at(f,j);
    if(named_atom(x)||atom(at(x,0),"export")||atom(at(x,0),"import")||atom(at(x,0),"type"))continue;
    if(!atom(at(x,0),"param")&&!atom(at(x,0),"result"))break;
    if(atom(at(x,0),"param")){
      for(size_t k=1;k<glwasm_text_size(x);k++){
        if(named_atom(at(x,k)))k++;
        if(k>=glwasm_text_size(x)||nparams==UINT32_MAX||!value_type(&params,at(x,k),typenames,ntypenames)){
          if(e){e->result=GLWASM_ERR_UNSUPPORTED;e->location=glwasm_text_location(x);e->message="unsupported function parameter type";}
          goto done;
        }
        nparams++;
      }
    }else{
      for(size_t k=1;k<glwasm_text_size(x);k++){
        if(nresults==UINT32_MAX||!value_type(&results,at(x,k),typenames,ntypenames)){
          if(e){e->result=GLWASM_ERR_UNSUPPORTED;e->location=glwasm_text_location(x);e->message="unsupported function result type";}
          goto done;
        }
        nresults++;
      }
    }
  }
  ok=put(types,0x60)&&uleb(types,nparams)&&bytes(types,params.p,params.n)&&
     uleb(types,nresults)&&bytes(types,results.p,results.n);
done:
  free(params.p);free(results.p);return ok;
}
static int emit_flat_indirect_signature(Bytes*types,const GLWASM_Text*tree,
    const GLWASM_Text*call,GLWASM_ParseError*e){
  if(glwasm_text_kind(tree)==GLWASM_TEXT_LIST){
    for(size_t i=0;i<glwasm_text_size(tree);i++)if(at(tree,i)==call){
      Bytes params={0},results={0};uint32_t np=0,nr=0;int ok=0;
      for(size_t j=i+1;j<glwasm_text_size(tree);j++){
        const GLWASM_Text*g=at(tree,j);int result=atom(at(g,0),"result");
        if(!result&&!atom(at(g,0),"param"))break;
        for(size_t k=1;k<glwasm_text_size(g);k++){
          if(!result&&named_atom(at(g,k)))k++;
          uint8_t v;Bytes*dst=result?&results:&params;uint32_t*count=result?&nr:&np;
          if(k>=glwasm_text_size(g)||!typebyte(at(g,k),&v)||*count==UINT32_MAX||!put(dst,v)){
            if(e){e->result=GLWASM_ERR_UNSUPPORTED;e->location=glwasm_text_location(g);e->message="unsupported flat call_indirect signature";}
            goto indirect_done;
          }
          (*count)++;
        }
      }
      ok=put(types,0x60)&&uleb(types,np)&&bytes(types,params.p,params.n)&&
         uleb(types,nr)&&bytes(types,results.p,results.n);
indirect_done:
      free(params.p);free(results.p);return ok;
    }
    for(size_t i=0;i<glwasm_text_size(tree);i++)
      if(emit_flat_indirect_signature(types,at(tree,i),call,e))return 1;
  }
  return 0;
}
static int emit_tag_signature(Bytes*types,const GLWASM_Text*t,GLWASM_ParseError*e){
  Bytes params={0},results={0};uint32_t np=0,nr=0;int ok=0;
  for(size_t i=1;i<glwasm_text_size(t);i++){
    const GLWASM_Text*x=at(t,i);
    if(atom(at(x,0),"param")||atom(at(x,0),"result")){
      int result=atom(at(x,0),"result");
      for(size_t j=1;j<glwasm_text_size(x);j++){
        uint8_t v;Bytes*dst=result?&results:&params;uint32_t*count=result?&nr:&np;
        if(named_atom(at(x,j)))j++;
        if(j>=glwasm_text_size(x)||!typebyte(at(x,j),&v)||*count==UINT32_MAX||!put(dst,v)){
          if(e){e->result=GLWASM_ERR_UNSUPPORTED;e->location=glwasm_text_location(x);e->message="unsupported tag signature type";}
          goto done;
        }
        (*count)++;
      }
    }
  }
  ok=put(types,0x60)&&uleb(types,np)&&bytes(types,params.p,params.n)&&
      uleb(types,nr)&&bytes(types,results.p,results.n);
done:
  free(params.p);free(results.p);return ok;
}
static int emit_block_signature(Bytes*types,const GLWASM_Text*t,GLWASM_ParseError*e,const NameRef*typenames,size_t ntypenames){
  Bytes params={0},results={0};uint32_t np=0,nr=0;int ok=0;
  size_t first=1;if(first<glwasm_text_size(t)&&named_atom(at(t,first)))first++;
  for(size_t i=first;i<glwasm_text_size(t);i++){
    const GLWASM_Text*x=at(t,i);int result=atom(at(x,0),"result");
    if(!result&&!atom(at(x,0),"param"))break;
    for(size_t j=1;j<glwasm_text_size(x);j++){
      if(named_atom(at(x,j)))j++;
      Bytes*dst=result?&results:&params;uint32_t*count=result?&nr:&np;
      if(j>=glwasm_text_size(x)||*count==UINT32_MAX||!value_type(dst,at(x,j),typenames,ntypenames)){
        if(e){e->result=GLWASM_ERR_UNSUPPORTED;e->location=glwasm_text_location(x);e->message="unsupported block signature type";}
        goto done;
      }
      (*count)++;
    }
  }
  ok=put(types,0x60)&&uleb(types,np)&&bytes(types,params.p,params.n)&&uleb(types,nr)&&bytes(types,results.p,results.n);
done:
  free(params.p);free(results.p);return ok;
}
static int blockref_add(const GLWASM_Text*n,NameRef**refs,size_t*count,size_t*capacity,uint32_t index){
  if(*count==*capacity){size_t c=*capacity?*capacity*2:16;void*p=realloc(*refs,c*sizeof(**refs));if(!p)return 0;*refs=p;*capacity=c;}
  (*refs)[(*count)++]=(NameRef){n,index};return 1;
}
static int collect_blockrefs(const GLWASM_Text*n,NameRef**refs,size_t*count,size_t*capacity,uint32_t*index){
  if(atom(at(n,0),"block")||atom(at(n,0),"loop")||atom(at(n,0),"if")||atom(at(n,0),"try_table")){
    uint32_t np=0,nr=0;
    size_t first=1;if(first<glwasm_text_size(n)&&named_atom(at(n,first)))first++;
    for(size_t i=first;i<glwasm_text_size(n);i++){
      const GLWASM_Text*x=at(n,i);int result=atom(at(x,0),"result");
      if(!result&&!atom(at(x,0),"param"))break;
      uint32_t*count=result?&nr:&np;
      for(size_t j=1;j<glwasm_text_size(x);j++){if(named_atom(at(x,j)))j++;if(j<glwasm_text_size(x))(*count)++;}
    }
    if(np||nr>1||tree_has_atom(n,"ref")){if(*index==UINT32_MAX||!blockref_add(n,refs,count,capacity,(*index)++))return 0;}
  }
  for(size_t i=0;i<glwasm_text_size(n);i++)if(!collect_blockrefs(at(n,i),refs,count,capacity,index))return 0;
  return 1;
}
static int find_blocktype(const GLWASM_Text*n,const NameRef*refs,size_t count,uint32_t*out){
  for(size_t i=0;i<count;i++)if(refs[i].node==n){*out=refs[i].index;return 1;}
  return 0;
}
static int inline_data_size(const GLWASM_Text*d,uint64_t*out){
  uint64_t total=0;
  for(size_t i=1;i<glwasm_text_size(d);i++){
    size_t n;const uint8_t*p=glwasm_text_bytes(at(d,i),&n);
    if((!p&&n)||n>UINT64_MAX-total)return 0;
    total+=n;
  }
  *out=total;return 1;
}
static int emit_inline_data_segment(Bytes*datas,const GLWASM_Text*d,uint32_t memory,int addr64){
  Bytes payload={0},offset={0};
  for(size_t i=1;i<glwasm_text_size(d);i++){
    size_t n;const uint8_t*p=glwasm_text_bytes(at(d,i),&n);
    if((!p&&n)||n>UINT32_MAX-payload.n||(n&&!bytes(&payload,p,n))){free(payload.p);return 0;}
  }
  int ok=put(&offset,addr64?0x42:0x41)&&sleb(&offset,0)&&put(&offset,0x0b)&&put(datas,memory?2:0)&&
      (!memory||uleb(datas,memory))&&bytes(datas,offset.p,offset.n)&&uleb(datas,(uint32_t)payload.n)&&bytes(datas,payload.p,payload.n);
  free(payload.p);free(offset.p);return ok;
}
static int inline_blocktype_needs_index(const GLWASM_Text*n){
  uint32_t np=0,nr=0;
  size_t first=1;
  if(first<glwasm_text_size(n)&&named_atom(at(n,first)))first++;
  for(size_t i=first;i<glwasm_text_size(n);i++){
    const GLWASM_Text*x=at(n,i);int result=atom(at(x,0),"result");
    if(!result&&!atom(at(x,0),"param"))break;
    uint32_t*count=result?&nr:&np;
    for(size_t j=1;j<glwasm_text_size(x);j++){if(named_atom(at(x,j)))j++;if(j<glwasm_text_size(x))(*count)++;}
  }
  return np||nr>1||tree_has_atom(n,"ref");
}
static int hex_digit(uint8_t c){return (c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F');}
static int numeric_text(const uint8_t*p,size_t z,char*out,size_t capacity){
  if(!p||!z||z>=capacity)return 0;
  size_t w=0;
  for(size_t i=0;i<z;i++){
    if(p[i]=='_'){
      if(i==0||i+1==z||!hex_digit(p[i-1])||!hex_digit(p[i+1]))return 0;
      continue;
    }
    out[w++]=(char)p[i];
  }
  if(!w)return 0;
  out[w]=0;return 1;
}
static int float_separators_valid(const uint8_t*p,size_t z){
  size_t i=0;if(i<z&&(p[i]=='+'||p[i]=='-'))i++;
  if(i+1<z&&p[i]=='0'&&p[i+1]=='X')return 0;
  int hex=i+1<z&&p[i]=='0'&&p[i+1]=='x';
  int payload=0,exponent=0;
  for(;i<z;i++){
    if(p[i]==':'){payload=1;continue;}
    if((hex&&!payload&&(p[i]=='p'||p[i]=='P'))||(!hex&&!payload&&(p[i]=='e'||p[i]=='E')))exponent=1;
    if(p[i]!='_')continue;
    if(i==0||i+1>=z)return 0;
    uint8_t a=p[i-1],b=p[i+1];
    int base=(payload||(hex&&!exponent))?16:10;
    int ad=base==16?hex_digit(a):(a>='0'&&a<='9');
    int bd=base==16?hex_digit(b):(b>='0'&&b<='9');
    if(!ad||!bd)return 0;
  }
  return 1;
}
static int hex_float_syntax(const char*s){
  const char*p=s;
  if(*p=='+'||*p=='-')p++;
  if(p[0]!='0'||p[1]!='x')return 1;
  p+=2;int digits=0;
  while(hex_digit((uint8_t)*p)){digits++;p++;}
  if(*p=='.'){
    p++;
    while(hex_digit((uint8_t)*p)){digits++;p++;}
  }
  if(!digits)return 0;
  /* The pinned wg-3.0 corpus uses bare hexadecimal mantissas. Keep those
   * forms as exponent zero for corpus compatibility; an explicit exponent
   * still has to contain at least one decimal digit. */
  if(!*p)return 1;
  if(*p!='p'&&*p!='P')return 0;
  p++;
  if(*p=='+'||*p=='-')p++;
  int exponent=0;
  while(*p>='0'&&*p<='9'){exponent++;p++;}
  return exponent&&!*p;
}
static int parse_i64(const GLWASM_Text*n,int64_t*out){
  size_t z;const uint8_t*p=glwasm_text_bytes(n,&z);char s[64];if(!numeric_text(p,z,s,sizeof(s)))return 0;
  const char*digits=s+(s[0]=='-'||s[0]=='+'?1:0);int base=digits[0]=='0'&&digits[1]=='x'?16:10;
  char*e;errno=0;long long v=strtoll(s,&e,base);
  if(errno||e!=s+strlen(s)) return 0;
  *out=(int64_t)v;
  return 1;
}
static int parse_integer_constant(const GLWASM_Text*n,unsigned bits,int64_t*out){
  size_t z;const uint8_t*p=glwasm_text_bytes(n,&z);char s[64];
  if(!numeric_text(p,z,s,sizeof(s))||!bits||bits>64)return 0;
  const char*digits=s+(s[0]=='-'||s[0]=='+'?1:0);
  int base=digits[0]=='0'&&digits[1]=='x'?16:10;
  if(s[0]=='-'){
    char*end;errno=0;long long v=strtoll(s,&end,base);
    int64_t minimum=bits==64?INT64_MIN:-(INT64_C(1)<<(bits-1));
    if(errno||end!=s+strlen(s)||v<minimum)return 0;
    *out=(int64_t)v;return 1;
  }
  char*end;errno=0;unsigned long long u=strtoull(s,&end,base);
  uint64_t maximum=bits==64?UINT64_MAX:((UINT64_C(1)<<bits)-1);
  if(errno||end!=s+strlen(s)||u>maximum)return 0;
  uint64_t sign=UINT64_C(1)<<(bits-1);
  *out=u>=sign?-(int64_t)(maximum-u)-1:(int64_t)u;
  return 1;
}
static int parse_u32(const GLWASM_Text*n,uint32_t*out){int64_t v;if(!parse_i64(n,&v)||v<0||v>UINT32_MAX)return 0;*out=(uint32_t)v;return 1;}
static int parse_u64(const GLWASM_Text*n,uint64_t*out){
  size_t z;const uint8_t*p=glwasm_text_bytes(n,&z);if(!p||!z||z>63)return 0;
  char s[64];if(!numeric_text(p,z,s,sizeof(s))||s[0]=='-')return 0;
  int base=s[0]=='0'&&s[1]=='x'?16:10;char*e;errno=0;unsigned long long v=strtoull(s,&e,base);
  if(errno||e!=s+strlen(s))return 0;
  *out=(uint64_t)v;
  return 1;
}
static int keyed_u64(const GLWASM_Text*n,const char*key,uint64_t*out){
  size_t z;const uint8_t*p=glwasm_text_bytes(n,&z),k=strlen(key);
  if(!p||z<=k||memcmp(p,key,k)||p[k]!='=')return 0;
  char s[64];size_t nbytes=z-k-1;if(!numeric_text(p+k+1,nbytes,s,sizeof(s))||s[0]=='-')return 0;
  int base=s[0]=='0'&&s[1]=='x'?16:10;char*end;errno=0;unsigned long long v=strtoull(s,&end,base);
  if(errno||end!=s+strlen(s))return 0;
  *out=(uint64_t)v;return 1;
}
static int parse_float_bits(const GLWASM_Text*n,int is64,Bytes*b){
  size_t z;const uint8_t*p=glwasm_text_bytes(n,&z);if(!p||!z)return 0;
  if(!float_separators_valid(p,z))return 0;
  char*s=malloc(z+1);if(!s)return 0;
  if(!numeric_text(p,z,s,z+1)){free(s);return 0;}
  int negative=s[0]=='-',positive=s[0]=='+';const char*word=s+(negative||positive?1:0);
  if(!hex_float_syntax(s)){free(s);return 0;}
  if(word[0]=='.'){free(s);return 0;}
  if(!strncmp(word,"nan",3)){
    uint64_t bits;const uint64_t sign=negative?(is64?UINT64_C(1)<<63:UINT64_C(1)<<31):0;
    if(word[3]==0)bits=is64?(UINT64_C(0x7ff8000000000000)|sign):(UINT64_C(0x7fc00000)|sign);
    else if(word[3]==':'&&word[4]=='0'&&word[5]=='x'){
      char*end;errno=0;unsigned long long payload=strtoull(word+4,&end,16);
      uint64_t mask=is64?UINT64_C(0x000fffffffffffff):UINT64_C(0x007fffff);
      if(errno||end!=word+strlen(word)||payload==0||payload>mask){free(s);return 0;}
      bits=(is64?UINT64_C(0x7ff0000000000000):UINT64_C(0x7f800000))|sign|payload;
    } else {free(s);return 0;}
    free(s);
    for(unsigned i=0;i<(is64?8u:4u);i++)if(!put(b,(uint8_t)(bits>>(8*i))))return 0;
    return 1;
  }
  char*end;errno=0;
  if(is64){double x=strtod(s,&end);uint64_t bits;memcpy(&bits,&x,sizeof(bits));if(end!=s+strlen(s)||(errno==ERANGE&&(x==HUGE_VAL||x==-HUGE_VAL))){free(s);return 0;}free(s);for(unsigned i=0;i<8;i++)if(!put(b,(uint8_t)(bits>>(8*i))))return 0;}
  else {float x=strtof(s,&end);uint32_t bits;memcpy(&bits,&x,sizeof(bits));if(end!=s+strlen(s)||(errno==ERANGE&&(x==HUGE_VAL||x==-HUGE_VAL))){free(s);return 0;}free(s);for(unsigned i=0;i<4;i++)if(!put(b,(uint8_t)(bits>>(8*i))))return 0;}
  return 1;
}
static void numeric_error(GLWASM_ParseError*e,const GLWASM_Text*n){
  if(e){e->result=GLWASM_ERR_CORRUPT;e->location=glwasm_text_location(n);e->message="malformed or out-of-range numeric literal";}
}
static void malformed_error(GLWASM_ParseError*e,const GLWASM_Text*n,
    const char*message){
  if(e){e->result=GLWASM_ERR_CORRUPT;e->location=glwasm_text_location(n);e->message=message;}
}
static int resolve_index(const GLWASM_Text*n,const NameRef*refs,size_t count,uint32_t*out){
  size_t z;const uint8_t*p=glwasm_text_bytes(n,&z);int64_t v;
  if(p&&z&&p[0]=='$'){
    for(size_t i=0;i<count;i++){
      size_t rz;const uint8_t*rp=glwasm_text_bytes(refs[i].node,&rz);
      if(rp&&rz==z&&!memcmp(rp,p,z)){*out=refs[i].index;return 1;}
    }
    return 0;
  }
  if(!parse_i64(n,&v)||v<0||v>UINT32_MAX)return 0;
  *out=(uint32_t)v;return 1;
}
static const GLWASM_Text *module_index_decl(const GLWASM_Text*field,const char*kind){
  const GLWASM_Text*decl=atom(at(field,0),"import")?at(field,3):field;
  return atom(at(decl,0),kind)?decl:NULL;
}
static int duplicate_module_names(const GLWASM_Text*root,size_t first,
    const char*kind,const char*message,GLWASM_ParseError*e){
  for(size_t i=first;i<glwasm_text_size(root);i++){
    const GLWASM_Text*a=module_index_decl(at(root,i),kind),*an=a?at(a,1):NULL;
    size_t az;const uint8_t*ap;
    if(!named_atom(an))continue;
    ap=glwasm_text_bytes(an,&az);
    for(size_t j=i+1;j<glwasm_text_size(root);j++){
      const GLWASM_Text*b=module_index_decl(at(root,j),kind),*bn=b?at(b,1):NULL;
      size_t bz;const uint8_t*bp;
      if(!named_atom(bn))continue;
      bp=glwasm_text_bytes(bn,&bz);
      if(ap&&bp&&az==bz&&!memcmp(ap,bp,az)){
        malformed_error(e,bn,message);
        return 1;
      }
    }
  }
  return 0;
}
static const GLWASM_Text *duplicate_local_name(const GLWASM_Text*func){
  for(size_t i=1;i<glwasm_text_size(func);i++){
    const GLWASM_Text*group=at(func,i);
    if(!atom(at(group,0),"param")&&!atom(at(group,0),"local"))continue;
    for(size_t j=1;j<glwasm_text_size(group);j++){
      const GLWASM_Text*name=at(group,j);if(!named_atom(name))continue;
      for(size_t k=1;k<=i;k++){
        const GLWASM_Text*prior=at(func,k);
        if(!atom(at(prior,0),"param")&&!atom(at(prior,0),"local"))continue;
        size_t end=k==i?j:glwasm_text_size(prior);
        for(size_t q=1;q<end;q++){
          const GLWASM_Text*other=at(prior,q);size_t an,bn;
          const uint8_t*a=glwasm_text_bytes(name,&an),*b=glwasm_text_bytes(other,&bn);
          if(named_atom(other)&&a&&b&&an==bn&&!memcmp(a,b,an))return name;
        }
      }
    }
  }
  return NULL;
}
static const GLWASM_Text *declared_type_form(const GLWASM_Text*module,
    const GLWASM_Text*type_use_node){
  const GLWASM_Text*id=at(type_use_node,1);uint32_t wanted=0;int by_name=named_atom(id);
  if(!by_name&&!parse_u32(id,&wanted))return NULL;
  uint32_t index=0;
  for(size_t i=1;i<glwasm_text_size(module);i++){
    const GLWASM_Text*t=at(module,i);if(!atom(at(t,0),"type"))continue;
    const GLWASM_Text*name=named_atom(at(t,1))?at(t,1):NULL;
    const GLWASM_Text*form=at(t,name?2:1);
    size_t nz=0,iz=0;const uint8_t*np=name?glwasm_text_bytes(name,&nz):NULL;
    const uint8_t*ip=by_name?glwasm_text_bytes(id,&iz):NULL;
    if((by_name&&np&&ip&&nz==iz&&!memcmp(np,ip,nz))||(!by_name&&index==wanted))return form;
    index++;
  }
  return NULL;
}
static int numeric_type_use_valid(const GLWASM_Text*module,
    const GLWASM_Text*type_use_node){
  const GLWASM_Text*id=at(type_use_node,1);uint32_t index;
  if(named_atom(id))return 1;
  if(!parse_u32(id,&index))return 0;
  uint32_t count=0;
  for(size_t i=1;i<glwasm_text_size(module);i++)
    if(atom(at(at(module,i),0),"type"))count++;
  size_t first=named_atom(at(module,1))?2:1;
  return index<(size_t)count+implicit_signature_count(module,first);
}
static const GLWASM_Text *signature_side_value(const GLWASM_Text*sig,
    size_t ordinal,int want_result){
  for(size_t i=1;i<glwasm_text_size(sig);i++){
    const GLWASM_Text*g=at(sig,i);
    if(!atom(at(g,0),want_result?"result":"param"))continue;
    for(size_t j=1;j<glwasm_text_size(g);j++){
      const GLWASM_Text*v=at(g,j);if(named_atom(v))continue;
      if(!ordinal--)return v;
    }
  }
  return NULL;
}
static const GLWASM_Text *annotation_side_value(const GLWASM_Text*n,
    size_t ordinal,int want_result){
  size_t first=1;
  if((atom(at(n,0),"block")||atom(at(n,0),"loop")||atom(at(n,0),"if")||
      atom(at(n,0),"try_table"))&&named_atom(at(n,first)))first++;
  for(size_t i=first;i<glwasm_text_size(n);i++){
    const GLWASM_Text*g=at(n,i);
    if(!atom(at(g,0),want_result?"result":"param"))continue;
    for(size_t j=1;j<glwasm_text_size(g);j++){
      const GLWASM_Text*v=at(g,j);if(named_atom(v))continue;
      if(!ordinal--)return v;
    }
  }
  return NULL;
}
static int same_signature_side(const GLWASM_Text*expected,
    const GLWASM_Text*actual,int want_result){
  for(size_t i=0;;i++){
    const GLWASM_Text*a=signature_side_value(expected,i,want_result);
    const GLWASM_Text*b=annotation_side_value(actual,i,want_result);
    if(!a||!b)return a==b;
    size_t az,bz;const uint8_t*ap=glwasm_text_bytes(a,&az),*bp=glwasm_text_bytes(b,&bz);
    if(!ap||!bp||az!=bz||memcmp(ap,bp,az))return 0;
  }
}
static int valid_inline_type_order(const GLWASM_Text*n,
    const GLWASM_Text*module,GLWASM_ParseError*e){
  const GLWASM_Text*h=at(n,0);int control=atom(h,"block")||atom(h,"loop")||
      atom(h,"if")||atom(h,"try_table");
  int indirect=indirect_atom(h);
  if(atom(h,"func")){
    int body=0;
    for(size_t i=1;i<glwasm_text_size(n);i++){
      const GLWASM_Text*x=at(n,i);
      if(atom(at(x,0),"type")){
        int inline_sig=0;
        for(size_t j=i+1;j<glwasm_text_size(n);j++)
          if(atom(at(at(n,j),0),"param")||atom(at(at(n,j),0),"result"))inline_sig=1;
        if(inline_sig&&!numeric_type_use_valid(module,x)){
          malformed_error(e,x,"unknown type index");return 0;
        }
      }
      if(atom(at(x,0),"type")||atom(at(x,0),"param")||
          atom(at(x,0),"result")||atom(at(x,0),"local")||
          atom(at(x,0),"export")||atom(at(x,0),"import")){
        size_t prior=i;
        while(prior>1&&(atom(at(at(n,prior-1),0),"param")||
            atom(at(at(n,prior-1),0),"result")||
            atom(at(at(n,prior-1),0),"type")||
            atom(at(at(n,prior-1),0),"table")))prior--;
        if(prior>1&&named_atom(at(n,prior-1)))prior--;
        int flat_control=prior>1&&(atom(at(n,prior-1),"block")||
            atom(at(n,prior-1),"loop")||atom(at(n,prior-1),"if"));
        int flat_indirect=prior>1&&indirect_atom(at(n,prior-1))&&
            (atom(at(x,0),"type")||atom(at(x,0),"param")||atom(at(x,0),"result"));
        int flat_select=prior>1&&atom(at(n,prior-1),"select")&&
            (atom(at(x,0),"type")||atom(at(x,0),"param")||atom(at(x,0),"result"));
        if(body&&!flat_control&&!flat_indirect&&!flat_select){malformed_error(e,x,"function declaration follows body");return 0;}
        continue;
      }
      if(named_atom(x)&&i==1)continue;
      body=1;
      if(atom(x,"block")||atom(x,"loop")||atom(x,"if")){
        size_t j=i+1;if(j<glwasm_text_size(n)&&named_atom(at(n,j)))j++;
        for(;j<glwasm_text_size(n);j++){
          const GLWASM_Text*g=at(n,j);
          if(atom(at(g,0),"param")){
            for(size_t k=1;k<glwasm_text_size(g);k++)if(named_atom(at(g,k))){
              malformed_error(e,at(g,k),"named flat block parameter");return 0;
            }
          }
          if(!atom(at(g,0),"param")&&!atom(at(g,0),"result"))break;
        }
      }
    }
  }
  if(atom(h,"type")){
    const GLWASM_Text*sig=named_atom(at(n,1))?at(n,2):at(n,1);
    if(atom(at(sig,0),"func")){
      int seen_result=0;
      for(size_t i=1;i<glwasm_text_size(sig);i++){
        const GLWASM_Text*g=at(sig,i);
        if(atom(at(g,0),"result")){
          seen_result=1;
          for(size_t j=1;j<glwasm_text_size(g);j++)if(named_atom(at(g,j))){
            malformed_error(e,at(g,j),"named result type");return 0;
          }
        } else if(atom(at(g,0),"param")){
          if(seen_result){malformed_error(e,g,"parameter follows result type");return 0;}
        }
      }
    }
  }
  if(control||indirect){
    size_t i=1;int phase=0,seen_type=0,has_inline_sig=0;const GLWASM_Text*type_use_node=NULL;
    if(control&&i<glwasm_text_size(n)&&named_atom(at(n,i)))i++;
    for(;i<glwasm_text_size(n);i++){
      const GLWASM_Text*x=at(n,i);
      if(atom(at(x,0),"type")){
        if(seen_type||phase){malformed_error(e,x,"unexpected type use position");return 0;}
        int inline_later=0;
        for(size_t j=i+1;j<glwasm_text_size(n);j++)
          if(atom(at(at(n,j),0),"param")||atom(at(at(n,j),0),"result"))inline_later=1;
        if(inline_later&&!numeric_type_use_valid(module,x)){malformed_error(e,x,"unknown type index");return 0;}
        seen_type=1;type_use_node=x;continue;
      }
      if(atom(at(x,0),"table")&&indirect&&phase==0)continue;
      if(atom(at(x,0),"param")){
        if(phase>1){malformed_error(e,x,"parameter annotation follows result annotation");return 0;}
        phase=1;
        has_inline_sig=1;
        for(size_t j=1;j<glwasm_text_size(x);j++)if(named_atom(at(x,j))){malformed_error(e,at(x,j),"named block or instruction parameter");return 0;}
        continue;
      }
      if(atom(at(x,0),"result")){phase=2;has_inline_sig=1;continue;}
      break;
    }
    if(type_use_node&&has_inline_sig){
      const GLWASM_Text*decl=declared_type_form(module,type_use_node);
      if(decl&&atom(at(decl,0),"func")&&
          (!same_signature_side(decl,n,0)||!same_signature_side(decl,n,1))){
        malformed_error(e,type_use_node,"inline function type does not match type use");return 0;
      }
    }
  }
  for(size_t i=0;i<glwasm_text_size(n);i++)
    if(!valid_inline_type_order(at(n,i),module,e))return 0;
  return 1;
}
static const GLWASM_Text *type_use(const GLWASM_Text*n){
  for(size_t i=1;i<glwasm_text_size(n);i++){
    const GLWASM_Text*x=at(n,i);
    if(atom(at(x,0),"type"))return x;
    if(named_atom(x)||atom(at(x,0),"export")||atom(at(x,0),"import")||
       atom(at(x,0),"param")||atom(at(x,0),"result")||atom(at(x,0),"local"))continue;
    break;
  }
  return NULL;
}
static const GLWASM_Text *function_field(const GLWASM_Text*x){
  if(atom(at(x,0),"func"))return x;
  if(atom(at(x,0),"import")&&atom(at(at(x,3),0),"func"))return at(x,3);
  return NULL;
}
static int same_signature(const GLWASM_Text*a,const GLWASM_Text*b){
  Bytes x={0},y={0};int equal=emit_function_signature(&x,a,NULL,NULL,0)&&emit_function_signature(&y,b,NULL,NULL,0)&&
      x.n==y.n&&!memcmp(x.p,y.p,x.n);
  free(x.p);free(y.p);return equal;
}
static const GLWASM_Text*explicit_type_body(const GLWASM_Text*root,size_t first,uint32_t index){
  uint32_t current=0;
  for(size_t i=first;i<glwasm_text_size(root);i++){
    const GLWASM_Text*t=at(root,i);if(!atom(at(t,0),"type"))continue;
    if(current++==index)return at(t,named_atom(at(t,1))?2:1);
  }
  return NULL;
}
static int resolve_type(const GLWASM_Text*n,const NameRef*refs,size_t count,
    uint32_t*out);
static uint32_t signature_param_count(const GLWASM_Text*sig){
  uint32_t n=0;
  for(size_t i=1;sig&&i<glwasm_text_size(sig);i++)if(atom(at(at(sig,i),0),"param"))
    for(size_t j=1;j<glwasm_text_size(at(sig,i));j++){
      if(named_atom(at(at(sig,i),j)))j++;
      if(j<glwasm_text_size(at(sig,i)))n++;
    }
  return n;
}
static uint32_t type_use_param_count(const GLWASM_Text*root,size_t first,
    const GLWASM_Text*type_use_node,const NameRef*typenames,size_t ntypenames){
  uint32_t type_index;
  if(!resolve_type(at(type_use_node,1),typenames,ntypenames,&type_index))
    return 0;
  const GLWASM_Text*decl=explicit_type_body(root,first,type_index);
  return signature_param_count(decl);
}
static int explicit_signature_index(const GLWASM_Text*root,size_t first,
    const GLWASM_Text*function,uint32_t*out){
  uint32_t index=0;
  for(size_t i=first;i<glwasm_text_size(root);i++){
    const GLWASM_Text*t=at(root,i);if(!atom(at(t,0),"type"))continue;
    const GLWASM_Text*f=named_atom(at(t,1))?at(t,2):at(t,1);
    if(same_signature(function,f)){*out=index;return 1;}
    index++;
  }
  return 0;
}
static int implicit_signature_is_duplicate(const GLWASM_Text*root,size_t first,
    size_t before,const GLWASM_Text*function){
  uint32_t ignored;
  if(explicit_signature_index(root,first,function,&ignored))return 1;
  for(size_t i=first;i<before;i++){
    const GLWASM_Text*prior=function_field(at(root,i));
    if(prior&&!type_use(prior)&&same_signature(function,prior))return 1;
  }
  return 0;
}
static size_t implicit_signature_count(const GLWASM_Text*root,size_t first){
  size_t count=0;
  for(size_t i=first;i<glwasm_text_size(root);i++){
    const GLWASM_Text*f=function_field(at(root,i));
    if(f&&!type_use(f)&&!implicit_signature_is_duplicate(root,first,i,f))count++;
  }
  return count;
}
static int function_type_index(const GLWASM_Text*root,size_t first,
    size_t explicit_types,const GLWASM_Text*function,uint32_t*type_index){
  uint32_t explicit_index;
  if(explicit_signature_index(root,first,function,&explicit_index)){
    *type_index=explicit_index;return 1;
  }
  size_t ordinal=0;
  for(size_t i=first;i<glwasm_text_size(root);i++){
    const GLWASM_Text*f=function_field(at(root,i));
    if(!f||type_use(f))continue;
    if(explicit_signature_index(root,first,f,&explicit_index))continue;
    if(same_signature(f,function)){
      if(ordinal>UINT32_MAX-explicit_types)return 0;
      *type_index=(uint32_t)explicit_types+(uint32_t)ordinal;return 2;
    }
    if(!implicit_signature_is_duplicate(root,first,i,f))ordinal++;
  }
  return 0;
}
static int emit_implicit_signatures(Bytes*types,const GLWASM_Text*root,
    size_t first,GLWASM_ParseError*e,const NameRef*typenames,size_t ntypenames){
  for(size_t i=first;i<glwasm_text_size(root);i++){
    const GLWASM_Text*f=function_field(at(root,i));
    if(f&&!type_use(f)&&!implicit_signature_is_duplicate(root,first,i,f)&&
       !emit_function_signature(types,f,e,typenames,ntypenames))return 0;
  }
  return 1;
}
static const GLWASM_Text *import_use(const GLWASM_Text*n){for(size_t i=1;i<glwasm_text_size(n);i++)if(atom(at(at(n,i),0),"import"))return at(n,i);return NULL;}
static int resolve_type(const GLWASM_Text*n,const NameRef*refs,size_t count,uint32_t*out){
  if(named_atom(n))return resolve_index(n,refs,count,out);
  /* Numeric type indices also address implicit function types. The binary
   * decoder checks the final index against the completed type section. */
  return parse_u32(n,out);
}
static int append_export(Bytes*b,uint32_t*count,const GLWASM_Text*decl,
    uint8_t kind,uint32_t index){
  size_t n;const uint8_t*name=glwasm_text_bytes(at(decl,1),&n);
  if(!atom(at(decl,0),"export")||glwasm_text_size(decl)!=2||(!name&&n)||n>UINT32_MAX)return 0;
  return uleb(b,(uint32_t)n)&&bytes(b,name,n)&&put(b,kind)&&uleb(b,index)&&
      ((*count)++,1);
}
static const TagInfo *tag_info_for(const TagInfo*infos,size_t count,
    const GLWASM_Text*node){
  for(size_t i=0;i<count;i++)if(infos[i].node==node)return &infos[i];
  return NULL;
}
static int resolve_depth(const GLWASM_Text*n,const NameRef*labels,size_t nlabels,uint32_t*out){
  size_t z;const uint8_t*p=glwasm_text_bytes(n,&z);
  if(p&&z&&p[0]=='$'){
    for(size_t i=nlabels;i>0;i--){size_t lz;const uint8_t*lp=glwasm_text_bytes(labels[i-1].node,&lz);if(lp&&lz==z&&!memcmp(lp,p,z)){*out=(uint32_t)(nlabels-i);return 1;}}
    return 0;
  }
  return resolve_index(n,NULL,0,out);
}
static int named_atom(const GLWASM_Text*n){size_t z;const uint8_t*p=glwasm_text_bytes(n,&z);return p&&z&&p[0]=='$';}
static int simple_op(const char *n,uint8_t *out){
  static const struct {const char*n;uint8_t op;} map[]={
    {"unreachable",0x00},{"nop",0x01},{"else",0x05},{"end",0x0b},{"return",0x0f},
    {"drop",0x1a},{"select",0x1b},{"ref.is_null",0xd1},{"throw_ref",0x0a},
    {"i32.eqz",0x45},{"i32.eq",0x46},{"i32.ne",0x47},{"i32.lt_s",0x48},{"i32.lt_u",0x49},{"i32.gt_s",0x4a},{"i32.gt_u",0x4b},{"i32.le_s",0x4c},{"i32.le_u",0x4d},{"i32.ge_s",0x4e},{"i32.ge_u",0x4f},
    {"i64.eqz",0x50},{"i64.eq",0x51},{"i64.ne",0x52},{"i64.lt_s",0x53},{"i64.lt_u",0x54},{"i64.gt_s",0x55},{"i64.gt_u",0x56},{"i64.le_s",0x57},{"i64.le_u",0x58},{"i64.ge_s",0x59},{"i64.ge_u",0x5a},
    {"f32.eq",0x5b},{"f32.ne",0x5c},{"f32.lt",0x5d},{"f32.gt",0x5e},{"f32.le",0x5f},{"f32.ge",0x60},{"f64.eq",0x61},{"f64.ne",0x62},{"f64.lt",0x63},{"f64.gt",0x64},{"f64.le",0x65},{"f64.ge",0x66},
    {"i32.clz",0x67},{"i32.ctz",0x68},{"i32.popcnt",0x69},{"i32.add",0x6a},{"i32.sub",0x6b},{"i32.mul",0x6c},{"i32.div_s",0x6d},{"i32.div_u",0x6e},{"i32.rem_s",0x6f},{"i32.rem_u",0x70},{"i32.and",0x71},{"i32.or",0x72},{"i32.xor",0x73},{"i32.shl",0x74},{"i32.shr_s",0x75},{"i32.shr_u",0x76},{"i32.rotl",0x77},{"i32.rotr",0x78},
    {"i64.clz",0x79},{"i64.ctz",0x7a},{"i64.popcnt",0x7b},{"i64.add",0x7c},{"i64.sub",0x7d},{"i64.mul",0x7e},{"i64.div_s",0x7f},{"i64.div_u",0x80},{"i64.rem_s",0x81},{"i64.rem_u",0x82},{"i64.and",0x83},{"i64.or",0x84},{"i64.xor",0x85},{"i64.shl",0x86},{"i64.shr_s",0x87},{"i64.shr_u",0x88},{"i64.rotl",0x89},{"i64.rotr",0x8a},
    {"f32.abs",0x8b},{"f32.neg",0x8c},{"f32.ceil",0x8d},{"f32.floor",0x8e},{"f32.trunc",0x8f},{"f32.nearest",0x90},{"f32.sqrt",0x91},{"f32.add",0x92},{"f32.sub",0x93},{"f32.mul",0x94},{"f32.div",0x95},{"f32.min",0x96},{"f32.max",0x97},{"f32.copysign",0x98},
    {"f64.abs",0x99},{"f64.neg",0x9a},{"f64.ceil",0x9b},{"f64.floor",0x9c},{"f64.trunc",0x9d},{"f64.nearest",0x9e},{"f64.sqrt",0x9f},{"f64.add",0xa0},{"f64.sub",0xa1},{"f64.mul",0xa2},{"f64.div",0xa3},{"f64.min",0xa4},{"f64.max",0xa5},{"f64.copysign",0xa6},
    {"i32.wrap_i64",0xa7},{"i32.trunc_f32_s",0xa8},{"i32.trunc_f32_u",0xa9},{"i32.trunc_f64_s",0xaa},{"i32.trunc_f64_u",0xab},{"i64.extend_i32_s",0xac},{"i64.extend_i32_u",0xad},{"i64.trunc_f32_s",0xae},{"i64.trunc_f32_u",0xaf},{"i64.trunc_f64_s",0xb0},{"i64.trunc_f64_u",0xb1},
    {"f32.convert_i32_s",0xb2},{"f32.convert_i32_u",0xb3},{"f32.convert_i64_s",0xb4},{"f32.convert_i64_u",0xb5},{"f32.demote_f64",0xb6},{"f64.convert_i32_s",0xb7},{"f64.convert_i32_u",0xb8},{"f64.convert_i64_s",0xb9},{"f64.convert_i64_u",0xba},{"f64.promote_f32",0xbb},
    {"i32.reinterpret_f32",0xbc},{"i64.reinterpret_f64",0xbd},{"f32.reinterpret_i32",0xbe},{"f64.reinterpret_i64",0xbf},{"i32.extend8_s",0xc0},{"i32.extend16_s",0xc1},{"i64.extend8_s",0xc2},{"i64.extend16_s",0xc3},{"i64.extend32_s",0xc4}
  };
  for(size_t i=0;i<sizeof(map)/sizeof(map[0]);i++)if(!strcmp(n,map[i].n)){*out=map[i].op;return 1;}
  return 0;
}
static int saturating_op(const char*n,uint32_t*sub){
  static const char*names[]={"i32.trunc_sat_f32_s","i32.trunc_sat_f32_u","i32.trunc_sat_f64_s","i32.trunc_sat_f64_u","i64.trunc_sat_f32_s","i64.trunc_sat_f32_u","i64.trunc_sat_f64_s","i64.trunc_sat_f64_u"};
  for(uint32_t i=0;i<8;i++)if(!strcmp(n,names[i])){*sub=i;return 1;}
  return 0;
}
static GLWASM_Result local_name_add(NameRef**refs,size_t*count,
    size_t*capacity,size_t limit,const GLWASM_Text*name,uint32_t index){
  if(!named_atom(name))return GLWASM_OK;
  if(*count>=limit)return GLWASM_ERR_LIMIT;
  if(*count==*capacity){
    size_t next=*capacity?*capacity*2:4;
    if(next<*capacity||next>SIZE_MAX/sizeof(**refs))return GLWASM_ERR_LIMIT;
    if(next>limit)next=limit;
    NameRef*grown=realloc(*refs,next*sizeof(**refs));
    if(!grown)return GLWASM_ERR_OOM;
    *refs=grown;*capacity=next;
  }
  (*refs)[(*count)++]=(NameRef){name,index};
  return GLWASM_OK;
}
static int memory_op(const uint8_t*p,size_t z,uint8_t*op,uint32_t*align){
  static const struct{const char*n;uint8_t op,align;}m[]={
    {"i32.load",0x28,2},{"i64.load",0x29,3},{"f32.load",0x2a,2},{"f64.load",0x2b,3},
    {"i32.load8_s",0x2c,0},{"i32.load8_u",0x2d,0},{"i32.load16_s",0x2e,1},{"i32.load16_u",0x2f,1},
    {"i64.load8_s",0x30,0},{"i64.load8_u",0x31,0},{"i64.load16_s",0x32,1},{"i64.load16_u",0x33,1},{"i64.load32_s",0x34,2},{"i64.load32_u",0x35,2},
    {"i32.store",0x36,2},{"i64.store",0x37,3},{"f32.store",0x38,2},{"f64.store",0x39,3},
    {"i32.store8",0x3a,0},{"i32.store16",0x3b,1},{"i64.store8",0x3c,0},{"i64.store16",0x3d,1},{"i64.store32",0x3e,2}
  };
  for(size_t i=0;i<sizeof(m)/sizeof(m[0]);i++)if(z==strlen(m[i].n)&&!memcmp(p,m[i].n,z)){*op=m[i].op;*align=m[i].align;return 1;}
  return 0;
}
static int emit_linear_indirect(Bytes*b,const GLWASM_Text*call,const GLWASM_Text*container,
    size_t*next,const NameRef*trefs,size_t ntrefs,GLWASM_ParseError*e){
  size_t cursor=*next+1;uint32_t type_index=0;int found=0;
  if(cursor<glwasm_text_size(container)&&atom(at(at(container,cursor),0),"type")){
    const GLWASM_Text*annotation=at(container,cursor++);
    if(glwasm_text_size(annotation)!=2||!resolve_type(at(annotation,1),trefs,ntrefs,&type_index)){if(e){e->result=GLWASM_ERR_UNSUPPORTED;e->location=glwasm_text_location(annotation);e->message="unresolved nested call_indirect type annotation";}return 0;}
  }else{
    for(size_t i=0;i<ntrefs;i++)if(trefs[i].node==call){type_index=trefs[i].index;found=1;break;}
    if(!found){if(e){e->result=GLWASM_ERR_UNSUPPORTED;e->location=glwasm_text_location(call);e->message="nested call_indirect lacks a synthesized type";}return 0;}
  }
  while(cursor<glwasm_text_size(container)&&
      (atom(at(at(container,cursor),0),"param")||atom(at(at(container,cursor),0),"result")))cursor++;
  size_t z=0;glwasm_text_bytes(call,&z);uint8_t opcode=z==20?0x13:0x11;
  if(!put(b,opcode)||!uleb(b,type_index)||!put(b,0))return 0;
  *next=cursor?cursor-1:0;
  return 1;
}
static int instruction(Bytes*b,const GLWASM_Text*n,GLWASM_ParseError*e,const NameRef*refs,size_t nrefs,const NameRef*lrefs,size_t nlrefs,const NameRef*grefs,size_t ngrefs,const NameRef*trefs,size_t ntrefs,const NameRef*tagrefs,size_t ntagrefs,const NameRef*tablerefs,size_t ntablerefs,const NameRef*memoryrefs,size_t nmemoryrefs,const NameRef*blockrefs,size_t nblockrefs,const NameRef*elemrefs,size_t nelemrefs,const NameRef*datarefs,size_t ndatarefs,NameRef*labels,size_t*nlabels){
  if(glwasm_text_kind(n)==GLWASM_TEXT_LIST){
    const GLWASM_Text*op=at(n,0);size_t z;const uint8_t*p=glwasm_text_bytes(op,&z);
    if(!p)return 0;
    if(z&&p[0]=='@')return 1;
    char name[32];if(z>=sizeof(name))return 0;memcpy(name,p,z);name[z]=0;
    if(!strcmp(name,"item")){
      if(glwasm_text_size(n)==2)return instruction(b,at(n,1),e,refs,nrefs,lrefs,nlrefs,grefs,ngrefs,trefs,ntrefs,tagrefs,ntagrefs,tablerefs,ntablerefs,memoryrefs,nmemoryrefs,blockrefs,nblockrefs,elemrefs,nelemrefs,datarefs,ndatarefs,labels,nlabels);
      if(glwasm_text_size(n)==3&&atom(at(n,1),"ref.func")){uint32_t fi;if(!resolve_index(at(n,2),refs,nrefs,&fi))return 0;return put(b,0xd2)&&uleb(b,fi);}
      for(size_t j=1;j<glwasm_text_size(n);j++)if(!instruction(b,at(n,j),e,refs,nrefs,lrefs,nlrefs,grefs,ngrefs,trefs,ntrefs,tagrefs,ntagrefs,tablerefs,ntablerefs,memoryrefs,nmemoryrefs,blockrefs,nblockrefs,elemrefs,nelemrefs,datarefs,ndatarefs,labels,nlabels))return 0;
      return 1;
    }
    if(!strcmp(name,"local.get")||!strcmp(name,"local.set")||!strcmp(name,"local.tee")){
      uint32_t ix;if(glwasm_text_size(n)<2||!resolve_index(at(n,1),lrefs,nlrefs,&ix))return 0;
      for(size_t j=2;j<glwasm_text_size(n);j++)if(!instruction(b,at(n,j),e,refs,nrefs,lrefs,nlrefs,grefs,ngrefs,trefs,ntrefs,tagrefs,ntagrefs,tablerefs,ntablerefs,memoryrefs,nmemoryrefs,blockrefs,nblockrefs,elemrefs,nelemrefs,datarefs,ndatarefs,labels,nlabels))return 0;
      uint8_t opbyte=!strcmp(name,"local.get")?0x20:!strcmp(name,"local.set")?0x21:0x22;
      return put(b,opbyte)&&uleb(b,ix);
    }
    {uint8_t memop;uint32_t align,memory=0;int has_memory=0;uint64_t offset=0;
      if(memory_op(p,z,&memop,&align)){
        for(size_t j=1;j<glwasm_text_size(n);j++){
          const GLWASM_Text*a=at(n,j);uint64_t keyed;
          if(keyed_u64(a,"offset",&keyed)){offset=keyed;continue;}
          if(keyed_u64(a,"align",&keyed)){
            if(!keyed||(keyed&(keyed-1))){malformed_error(e,a,"invalid memory alignment");return 0;}
            align=0;while(((uint64_t)1<<align)<keyed&&align<63)align++;continue;
          }
          if(keyed_u64(a,"memory",&keyed)){if(keyed>UINT32_MAX)return 0;memory=(uint32_t)keyed;has_memory=1;continue;}
          if(named_atom(a)){if(!resolve_index(a,memoryrefs,nmemoryrefs,&memory))return 0;has_memory=1;continue;}
          if(atom(at(a,0),"offset")){if(glwasm_text_size(a)!=2||!parse_u64(at(a,1),&offset))return 0;continue;}
          if(atom(at(a,0),"align")){uint64_t bytesize;if(glwasm_text_size(a)!=2||!parse_u64(at(a,1),&bytesize)||!bytesize||(bytesize&(bytesize-1))){malformed_error(e,a,"invalid memory alignment");return 0;}align=0;while(((uint64_t)1<<align)<bytesize&&align<63)align++;continue;}
          if(atom(at(a,0),"memory")){if(glwasm_text_size(a)!=2||!resolve_index(at(a,1),memoryrefs,nmemoryrefs,&memory))return 0;has_memory=1;continue;}
          if(!instruction(b,a,e,refs,nrefs,lrefs,nlrefs,grefs,ngrefs,trefs,ntrefs,tagrefs,ntagrefs,tablerefs,ntablerefs,memoryrefs,nmemoryrefs,blockrefs,nblockrefs,elemrefs,nelemrefs,datarefs,ndatarefs,labels,nlabels))return 0;
        }
        return put(b,memop)&&uleb(b,align|(has_memory?64u:0u))&&(!has_memory||uleb(b,memory))&&uleb64(b,offset);
      }
    }
    if(!strcmp(name,"block")||!strcmp(name,"loop")){
      uint8_t op=!strcmp(name,"block")?0x02:0x03,bt=0x40;size_t first=1,oldlabels=nlabels?*nlabels:0;int typed=0;
      const GLWASM_Text*label=NULL;
      if(first<glwasm_text_size(n)&&named_atom(at(n,first)))label=at(n,first++);
      if(labels&&nlabels){if(*nlabels>=256)return 0;labels[(*nlabels)++]=(NameRef){label,0};}
      if(first<glwasm_text_size(n)&&atom(at(at(n,first),0),"type")){
        uint32_t ti;if(glwasm_text_size(at(n,first))!=2||!resolve_type(at(at(n,first),1),trefs,ntrefs,&ti)||!put(b,op)||!sleb(b,ti)){if(nlabels)*nlabels=oldlabels;return 0;}typed=1;first++;
      }
      if(!typed&&inline_blocktype_needs_index(n)){
        uint32_t ti;if(!find_blocktype(n,blockrefs,nblockrefs,&ti)||!put(b,op)||!sleb(b,ti)){if(nlabels)*nlabels=oldlabels;return 0;}typed=1;
      }
      for(size_t k=first;k<glwasm_text_size(n)&&
          (atom(at(at(n,k),0),"param")||atom(at(at(n,k),0),"result"));k++)
        if(atom(at(at(n,k),0),"result")&&!typed){
          if(glwasm_text_size(at(n,k))!=2||!typebyte(at(at(n,k),1),&bt)){if(nlabels)*nlabels=oldlabels;return 0;}
        }
      while(first<glwasm_text_size(n)&&(atom(at(at(n,first),0),"param")||atom(at(at(n,first),0),"result")))first++;
      if(!typed&&(!put(b,op)||!put(b,bt))){if(nlabels)*nlabels=oldlabels;return 0;}
      for(size_t j=first;j<glwasm_text_size(n);j++){
        const GLWASM_Text*item=at(n,j);size_t item_len=0;const uint8_t*item_bytes=glwasm_text_bytes(item,&item_len);int64_t value;
        if(item_bytes&&((item_len==9&&!memcmp(item_bytes,"i32.const",9))||(item_len==9&&!memcmp(item_bytes,"i64.const",9)))){
          int is_i32=item_bytes[1]=='3';
          if(j+1>=glwasm_text_size(n)||!parse_integer_constant(at(n,j+1),is_i32?32:64,&value)||!put(b,is_i32?0x41:0x42)||!sleb(b,value)){
            if(j+1<glwasm_text_size(n))numeric_error(e,at(n,j+1));
            if(nlabels)*nlabels=oldlabels;
            return 0;
          }
          j++;continue;
        }
        if(atom(item,"call_indirect")||atom(item,"return_call_indirect")){
          if(!emit_linear_indirect(b,item,n,&j,trefs,ntrefs,e)){if(nlabels)*nlabels=oldlabels;return 0;}
          continue;
        }
        if(!instruction(b,item,e,refs,nrefs,lrefs,nlrefs,grefs,ngrefs,trefs,ntrefs,tagrefs,ntagrefs,tablerefs,ntablerefs,memoryrefs,nmemoryrefs,blockrefs,nblockrefs,elemrefs,nelemrefs,datarefs,ndatarefs,labels,nlabels)){if(nlabels)*nlabels=oldlabels;return 0;}
      }
      if(nlabels)*nlabels=oldlabels;
      return put(b,0x0b);
    }
    if(!strcmp(name,"try_table")){
      size_t first=1,oldlabels=nlabels?*nlabels:0;const GLWASM_Text*label=NULL;uint8_t bt=0x40;int typed=0,indexed=0;uint32_t block_index=0;
      if(first<glwasm_text_size(n)&&named_atom(at(n,first)))label=at(n,first++);
      if(!put(b,0x1f))return 0;
      if(first<glwasm_text_size(n)&&atom(at(at(n,first),0),"type")){
        if(glwasm_text_size(at(n,first))!=2||!resolve_type(at(at(n,first),1),trefs,ntrefs,&block_index)||!sleb(b,block_index))return 0;
        typed=1;indexed=1;first++;
      }
      if(!typed&&inline_blocktype_needs_index(n)){
        if(!find_blocktype(n,blockrefs,nblockrefs,&block_index)||!sleb(b,block_index))return 0;
        typed=1;indexed=1;
      }
      if(first<glwasm_text_size(n)&&atom(at(at(n,first),0),"result")){
        if(!indexed&&(glwasm_text_size(at(n,first))!=2||!typebyte(at(at(n,first),1),&bt)))return 0;
        first++;
      }
      while(first<glwasm_text_size(n)&&atom(at(at(n,first),0),"param"))first++;
      size_t catches=first;
      while(catches<glwasm_text_size(n)&&
          (atom(at(at(n,catches),0),"catch")||atom(at(at(n,catches),0),"catch_ref")||
           atom(at(at(n,catches),0),"catch_all")||atom(at(at(n,catches),0),"catch_all_ref")))catches++;
      Bytes clauses={0};uint32_t nclauses=0;
      for(size_t i=first;i<catches;i++){
        const GLWASM_Text*c=at(n,i);int all=atom(at(c,0),"catch_all")||atom(at(c,0),"catch_all_ref");
        int ref=atom(at(c,0),"catch_ref")||atom(at(c,0),"catch_all_ref");
        uint32_t tag=0,depth;size_t ti=1;
        if(!all&&(!resolve_index(at(c,ti++),tagrefs,ntagrefs,&tag))){free(clauses.p);return 0;}
        if(ti>=glwasm_text_size(c)||!resolve_depth(at(c,ti),labels&&nlabels?labels:NULL,nlabels?*nlabels:0,&depth)||
            ti+1!=glwasm_text_size(c)||!put(&clauses,(uint8_t)(all?(ref?3:2):(ref?1:0)))||
            (!all&&!uleb(&clauses,tag))||!uleb(&clauses,depth)){free(clauses.p);return 0;}
        nclauses++;
      }
      if(!indexed&&!put(b,bt)){free(clauses.p);return 0;}
      if(!uleb(b,nclauses)||!bytes(b,clauses.p,clauses.n)){free(clauses.p);return 0;}free(clauses.p);
      if(labels&&nlabels){if(*nlabels>=256)return 0;labels[(*nlabels)++]=(NameRef){label,0};}
      for(size_t j=catches;j<glwasm_text_size(n);j++)if(!instruction(b,at(n,j),e,refs,nrefs,lrefs,nlrefs,grefs,ngrefs,trefs,ntrefs,tagrefs,ntagrefs,tablerefs,ntablerefs,memoryrefs,nmemoryrefs,blockrefs,nblockrefs,elemrefs,nelemrefs,datarefs,ndatarefs,labels,nlabels)){if(nlabels)*nlabels=oldlabels;return 0;}
      if(nlabels)*nlabels=oldlabels;
      return put(b,0x0b);
    }
    if(!strcmp(name,"then")||!strcmp(name,"else")){
      if(!strcmp(name,"else")&&!put(b,0x05))return 0;
      for(size_t j=1;j<glwasm_text_size(n);j++){
        const GLWASM_Text*item=at(n,j);size_t item_len=0;const uint8_t*item_bytes=glwasm_text_bytes(item,&item_len);int64_t value;
        if(item_bytes&&((item_len==9&&!memcmp(item_bytes,"i32.const",9))||(item_len==9&&!memcmp(item_bytes,"i64.const",9)))){
          int is_i32=item_bytes[1]=='3';
          if(j+1>=glwasm_text_size(n)||!parse_integer_constant(at(n,j+1),is_i32?32:64,&value)||!put(b,is_i32?0x41:0x42)||!sleb(b,value)){
            if(j+1<glwasm_text_size(n))numeric_error(e,at(n,j+1));
            return 0;
          }
          j++;continue;
        }
        if(atom(item,"call_indirect")||atom(item,"return_call_indirect")){
          if(!emit_linear_indirect(b,item,n,&j,trefs,ntrefs,e))return 0;
          continue;
        }
        if(!instruction(b,item,e,refs,nrefs,lrefs,nlrefs,grefs,ngrefs,trefs,ntrefs,tagrefs,ntagrefs,tablerefs,ntablerefs,memoryrefs,nmemoryrefs,blockrefs,nblockrefs,elemrefs,nelemrefs,datarefs,ndatarefs,labels,nlabels))return 0;
      }
      return 1;
    }
    if(!strcmp(name,"if")){
      size_t j=1,oldlabels=nlabels?*nlabels:0;uint8_t bt=0x40;int typed=0,indexed=0;uint32_t block_index=0;const GLWASM_Text*label=NULL;
      if(j<glwasm_text_size(n)&&named_atom(at(n,j)))label=at(n,j++);
      if(labels&&nlabels){if(*nlabels>=256)return 0;labels[(*nlabels)++]=(NameRef){label,0};}
      if(j<glwasm_text_size(n)&&atom(at(at(n,j),0),"type")){
        if(glwasm_text_size(at(n,j))!=2||!resolve_type(at(at(n,j),1),trefs,ntrefs,&block_index)){if(nlabels)*nlabels=oldlabels;return 0;}typed=1;indexed=1;j++;
      }
      if(!typed&&inline_blocktype_needs_index(n)){
        if(!find_blocktype(n,blockrefs,nblockrefs,&block_index)){if(nlabels)*nlabels=oldlabels;return 0;}typed=1;indexed=1;
      }
      while(j<glwasm_text_size(n)&&
          (atom(at(at(n,j),0),"param")||atom(at(at(n,j),0),"result"))){
        if(atom(at(at(n,j),0),"result")&&!indexed&&
           (glwasm_text_size(at(n,j))!=2||!typebyte(at(at(n,j),1),&bt))){
          if(nlabels)*nlabels=oldlabels;
          return 0;
        }
        j++;
      }
      while(j<glwasm_text_size(n)&&!atom(at(at(n,j),0),"then")&&!atom(at(at(n,j),0),"else"))
        if(!instruction(b,at(n,j++),e,refs,nrefs,lrefs,nlrefs,grefs,ngrefs,trefs,ntrefs,tagrefs,ntagrefs,tablerefs,ntablerefs,memoryrefs,nmemoryrefs,blockrefs,nblockrefs,elemrefs,nelemrefs,datarefs,ndatarefs,labels,nlabels)){if(nlabels)*nlabels=oldlabels;return 0;}
      if(indexed){if(!put(b,0x04)||!sleb(b,block_index)){if(nlabels)*nlabels=oldlabels;return 0;}}
      else if(!put(b,0x04)||!put(b,bt)){if(nlabels)*nlabels=oldlabels;return 0;}
      if(j<glwasm_text_size(n)&&atom(at(at(n,j),0),"then"))if(!instruction(b,at(n,j++),e,refs,nrefs,lrefs,nlrefs,grefs,ngrefs,trefs,ntrefs,tagrefs,ntagrefs,tablerefs,ntablerefs,memoryrefs,nmemoryrefs,blockrefs,nblockrefs,elemrefs,nelemrefs,datarefs,ndatarefs,labels,nlabels)){if(nlabels)*nlabels=oldlabels;return 0;}
      if(j<glwasm_text_size(n)&&atom(at(at(n,j),0),"else"))if(!instruction(b,at(n,j++),e,refs,nrefs,lrefs,nlrefs,grefs,ngrefs,trefs,ntrefs,tagrefs,ntagrefs,tablerefs,ntablerefs,memoryrefs,nmemoryrefs,blockrefs,nblockrefs,elemrefs,nelemrefs,datarefs,ndatarefs,labels,nlabels)){if(nlabels)*nlabels=oldlabels;return 0;}
      if(nlabels)*nlabels=oldlabels;
      return j==glwasm_text_size(n)&&put(b,0x0b);
    }
    if(!strcmp(name,"select")&&glwasm_text_size(n)>1&&atom(at(at(n,1),0),"result")){
      const GLWASM_Text*result=at(n,1);size_t nresults=glwasm_text_size(result)-1;
      for(size_t j=2;j<glwasm_text_size(n);j++)if(!instruction(b,at(n,j),e,refs,nrefs,lrefs,nlrefs,grefs,ngrefs,trefs,ntrefs,tagrefs,ntagrefs,tablerefs,ntablerefs,memoryrefs,nmemoryrefs,blockrefs,nblockrefs,elemrefs,nelemrefs,datarefs,ndatarefs,labels,nlabels))return 0;
      if(!put(b,0x1c)||!uleb(b,(uint32_t)nresults))return 0;
      for(size_t j=1;j<glwasm_text_size(result);j++)
        if(!value_type(b,at(result,j),trefs,ntrefs))return 0;
      return 1;
    }
    uint8_t code;
    {uint32_t sub;if(saturating_op(name,&sub)){for(size_t j=1;j<glwasm_text_size(n);j++)if(!instruction(b,at(n,j),e,refs,nrefs,lrefs,nlrefs,grefs,ngrefs,trefs,ntrefs,tagrefs,ntagrefs,tablerefs,ntablerefs,memoryrefs,nmemoryrefs,blockrefs,nblockrefs,elemrefs,nelemrefs,datarefs,ndatarefs,labels,nlabels))return 0;return put(b,0xfc)&&uleb(b,sub);}}
    if(simple_op(name,&code)){
      for(size_t j=1;j<glwasm_text_size(n);j++)if(!instruction(b,at(n,j),e,refs,nrefs,lrefs,nlrefs,grefs,ngrefs,trefs,ntrefs,tagrefs,ntagrefs,tablerefs,ntablerefs,memoryrefs,nmemoryrefs,blockrefs,nblockrefs,elemrefs,nelemrefs,datarefs,ndatarefs,labels,nlabels))return 0;
      return put(b,code);
    }
    if((!strncmp(name,"i32.const",9)||!strncmp(name,"i64.const",9))&&glwasm_text_size(n)==2){
      int64_t v;if(!parse_integer_constant(at(n,1),name[1]=='3'?32:64,&v)){numeric_error(e,at(n,1));return 0;}
      return put(b,name[1]=='3'?0x41:0x42)&&sleb(b,v);
    }
    if((!strcmp(name,"f32.const")||!strcmp(name,"f64.const"))&&glwasm_text_size(n)==2){
      Bytes imm={0};int is64=name[1]=='6';
      if(!parse_float_bits(at(n,1),is64,&imm)||!put(b,is64?0x44:0x43)||!bytes(b,imm.p,imm.n)){numeric_error(e,at(n,1));free(imm.p);return 0;}
      free(imm.p);return 1;
    }
    if((!strcmp(name,"local.get")||!strcmp(name,"local.set")||!strcmp(name,"local.tee")||!strcmp(name,"global.get")||!strcmp(name,"global.set")||!strcmp(name,"call")||!strcmp(name,"return_call")||!strcmp(name,"ref.func"))&&glwasm_text_size(n)>=2){
      uint32_t ix;uint8_t op=!strcmp(name,"local.get")?0x20:!strcmp(name,"local.set")?0x21:!strcmp(name,"local.tee")?0x22:!strcmp(name,"global.get")?0x23:!strcmp(name,"global.set")?0x24:!strcmp(name,"ref.func")?0xd2:!strcmp(name,"return_call")?0x12:0x10;
      const NameRef*indexrefs=(!strcmp(name,"global.get")||!strcmp(name,"global.set"))?grefs:
          (!strcmp(name,"local.get")||!strcmp(name,"local.set")||!strcmp(name,"local.tee"))?lrefs:refs;
      size_t nindexrefs=(!strcmp(name,"global.get")||!strcmp(name,"global.set"))?ngrefs:
          (!strcmp(name,"local.get")||!strcmp(name,"local.set")||!strcmp(name,"local.tee"))?nlrefs:nrefs;
      if(!resolve_index(at(n,1),indexrefs,nindexrefs,&ix))return 0;
      for(size_t j=2;j<glwasm_text_size(n);j++)if(!instruction(b,at(n,j),e,refs,nrefs,lrefs,nlrefs,grefs,ngrefs,trefs,ntrefs,tagrefs,ntagrefs,tablerefs,ntablerefs,memoryrefs,nmemoryrefs,blockrefs,nblockrefs,elemrefs,nelemrefs,datarefs,ndatarefs,labels,nlabels))return 0;
      return put(b,op)&&uleb(b,ix);
    }
    if((!strcmp(name,"memory.size")||!strcmp(name,"memory.grow"))){
      uint32_t mi=0;size_t first=1;
      if(first<glwasm_text_size(n)&&atom(at(at(n,first),0),"memory")){
        if(glwasm_text_size(at(n,first))!=2||!resolve_index(at(at(n,first),1),memoryrefs,nmemoryrefs,&mi))return 0;
        first++;
      } else if(first<glwasm_text_size(n)&&glwasm_text_kind(at(n,first))!=GLWASM_TEXT_LIST){
        if(!resolve_index(at(n,first),memoryrefs,nmemoryrefs,&mi))return 0;
        first++;
      }
      for(size_t j=first;j<glwasm_text_size(n);j++)if(!instruction(b,at(n,j),e,refs,nrefs,lrefs,nlrefs,grefs,ngrefs,trefs,ntrefs,tagrefs,ntagrefs,tablerefs,ntablerefs,memoryrefs,nmemoryrefs,blockrefs,nblockrefs,elemrefs,nelemrefs,datarefs,ndatarefs,labels,nlabels))return 0;
      return put(b,!strcmp(name,"memory.size")?0x3f:0x40)&&uleb(b,mi);
    }
    if(!strcmp(name,"ref.null")&&glwasm_text_size(n)==2){
      uint8_t heap;
      if(atom(at(n,1),"funcref")||atom(at(n,1),"func"))heap=0x70;
      else if(atom(at(n,1),"externref")||atom(at(n,1),"extern"))heap=0x6f;
      else if(!typebyte(at(n,1),&heap))return 0;
      return put(b,0xd0)&&put(b,heap);
    }
    if((!strcmp(name,"br")||!strcmp(name,"br_if"))&&glwasm_text_size(n)>=2){
      uint32_t depth;if(!resolve_depth(at(n,1),labels&&nlabels?labels:NULL,nlabels?*nlabels:0,&depth))return 0;
      for(size_t j=2;j<glwasm_text_size(n);j++)if(!instruction(b,at(n,j),e,refs,nrefs,lrefs,nlrefs,grefs,ngrefs,trefs,ntrefs,tagrefs,ntagrefs,tablerefs,ntablerefs,memoryrefs,nmemoryrefs,blockrefs,nblockrefs,elemrefs,nelemrefs,datarefs,ndatarefs,labels,nlabels))return 0;
      return put(b,!strcmp(name,"br")?0x0c:0x0d)&&uleb(b,depth);
    }
    if(!strcmp(name,"br_table")&&glwasm_text_size(n)>=2){
      size_t end=glwasm_text_size(n);while(end>1&&glwasm_text_kind(at(n,end-1))==GLWASM_TEXT_LIST)end--;
      if(end<2){malformed_error(e,n,"br_table requires a target");return 0;}
      for(size_t j=end;j<glwasm_text_size(n);j++)if(!instruction(b,at(n,j),e,refs,nrefs,lrefs,nlrefs,grefs,ngrefs,trefs,ntrefs,tagrefs,ntagrefs,tablerefs,ntablerefs,memoryrefs,nmemoryrefs,blockrefs,nblockrefs,elemrefs,nelemrefs,datarefs,ndatarefs,labels,nlabels))return 0;
      if(!put(b,0x0e)||!uleb(b,(uint32_t)(end-2)))return 0;
      for(size_t j=1;j<end-1;j++){uint32_t depth;if(!resolve_depth(at(n,j),labels&&nlabels?labels:NULL,nlabels?*nlabels:0,&depth)){malformed_error(e,at(n,j),"unknown branch label");return 0;}if(!uleb(b,depth))return 0;}
      uint32_t def;if(!resolve_depth(at(n,end-1),labels&&nlabels?labels:NULL,nlabels?*nlabels:0,&def)){malformed_error(e,at(n,end-1),"unknown branch label");return 0;}
      return uleb(b,def);
    }
    if(!strcmp(name,"call_indirect")||!strcmp(name,"return_call_indirect")){
      const GLWASM_Text*tu=NULL;uint32_t ti,table=0;
      for(size_t j=1;j<glwasm_text_size(n);j++)if(atom(at(at(n,j),0),"type")){tu=at(n,j);break;}
      if(tu){if(glwasm_text_size(tu)!=2||!resolve_type(at(tu,1),trefs,ntrefs,&ti))return 0;}
      else {size_t j;for(j=0;j<ntrefs;j++)if(trefs[j].node==n){ti=trefs[j].index;break;}if(j==ntrefs)return 0;}
      for(size_t j=1;j<glwasm_text_size(n);j++){
        const GLWASM_Text*x=at(n,j);
        if(x==tu)continue;
        if(atom(at(x,0),"param")||atom(at(x,0),"result"))continue;
        if(j==1&&(named_atom(x)||
           (glwasm_text_kind(x)==GLWASM_TEXT_ATOM&&parse_u32(x,&table)))){
          if(!resolve_index(x,tablerefs,ntablerefs,&table))return 0;
          continue;
        }
        if(atom(at(x,0),"table")){if(glwasm_text_size(x)!=2||!resolve_index(at(x,1),tablerefs,ntablerefs,&table))return 0;continue;}
        if(!instruction(b,x,e,refs,nrefs,lrefs,nlrefs,grefs,ngrefs,trefs,ntrefs,tagrefs,ntagrefs,tablerefs,ntablerefs,memoryrefs,nmemoryrefs,blockrefs,nblockrefs,elemrefs,nelemrefs,datarefs,ndatarefs,labels,nlabels))return 0;
      }
      return put(b,!strcmp(name,"call_indirect")?0x11:0x13)&&uleb(b,ti)&&uleb(b,table);
    }
    if(!strcmp(name,"throw")&&glwasm_text_size(n)>=2){
      uint32_t ti;if(!resolve_index(at(n,1),tagrefs,ntagrefs,&ti))return 0;
      for(size_t j=2;j<glwasm_text_size(n);j++)if(!instruction(b,at(n,j),e,refs,nrefs,lrefs,nlrefs,grefs,ngrefs,trefs,ntrefs,tagrefs,ntagrefs,tablerefs,ntablerefs,memoryrefs,nmemoryrefs,blockrefs,nblockrefs,elemrefs,nelemrefs,datarefs,ndatarefs,labels,nlabels))return 0;
      return put(b,0x08)&&uleb(b,ti);
    }
    {
      static const struct {const char *name;uint32_t sub;uint8_t immediates;} bulk[]={
        {"memory.init",8,1},{"data.drop",9,1},{"memory.copy",10,0},{"memory.fill",11,0},
        {"table.init",12,1},{"elem.drop",13,1},{"table.copy",14,0},{"table.grow",15,0},
        {"table.size",16,0},{"table.fill",17,0}
      };
      for(size_t bi=0;bi<sizeof(bulk)/sizeof(bulk[0]);bi++)if(!strcmp(name,bulk[bi].name)){
        uint32_t a=0,c=0;size_t first=1;
        if(bulk[bi].immediates){
          if(first>=glwasm_text_size(n))return 0;
          if(bulk[bi].sub==8&&named_atom(at(n,first))&&
             resolve_index(at(n,first),memoryrefs,nmemoryrefs,&c)){
            first++;
            if(first>=glwasm_text_size(n)||!resolve_index(at(n,first),datarefs,ndatarefs,&a))return 0;
            first++;
          }
          else if(bulk[bi].sub==12&&named_atom(at(n,first))&&
             resolve_index(at(n,first),tablerefs,ntablerefs,&c)){
            first++;
            if(first>=glwasm_text_size(n)||
               !resolve_index(at(n,first),elemrefs,nelemrefs,&a))return 0;
            first++;
          }
          else {
          if((bulk[bi].sub==8||bulk[bi].sub==9)&&
             !resolve_index(at(n,first),datarefs,ndatarefs,&a)){if(e){e->result=GLWASM_ERR_CORRUPT;e->location=glwasm_text_location(at(n,first));e->message=ndatarefs?"unresolved data segment name":"data segment name map is empty";}return 0;}
          else if((bulk[bi].sub==12||bulk[bi].sub==13)&&
             !resolve_index(at(n,first),elemrefs,nelemrefs,&a))return 0;
          else if(bulk[bi].sub!=8&&bulk[bi].sub!=9&&bulk[bi].sub!=12&&bulk[bi].sub!=13&&
                  !parse_u32(at(n,first),&a))return 0;
          first++;
          }
        }
        if(bulk[bi].sub==8||bulk[bi].sub==12){
          const char*selector=bulk[bi].sub==8?"memory":"table";
          if(first<glwasm_text_size(n)&&atom(at(at(n,first),0),selector)){
            const NameRef*selrefs=bulk[bi].sub==8?memoryrefs:tablerefs;size_t nselrefs=bulk[bi].sub==8?nmemoryrefs:ntablerefs;
            if(glwasm_text_size(at(n,first))!=2||!resolve_index(at(at(n,first),1),selrefs,nselrefs,&c))return 0;
            first++;
          }
        } else if(bulk[bi].sub==10){
          if(first<glwasm_text_size(n)&&glwasm_text_kind(at(n,first))!=GLWASM_TEXT_LIST&&
              !resolve_index(at(n,first++),memoryrefs,nmemoryrefs,&a))return 0;
          if(first<glwasm_text_size(n)&&glwasm_text_kind(at(n,first))!=GLWASM_TEXT_LIST&&
              !resolve_index(at(n,first++),memoryrefs,nmemoryrefs,&c))return 0;
        } else if(bulk[bi].sub==11){
          if(first<glwasm_text_size(n)&&glwasm_text_kind(at(n,first))!=GLWASM_TEXT_LIST&&
              resolve_index(at(n,first),memoryrefs,nmemoryrefs,&c))first++;
        } else if(bulk[bi].sub==14){
          if(first<glwasm_text_size(n)&&glwasm_text_kind(at(n,first))!=GLWASM_TEXT_LIST&&
              !resolve_index(at(n,first++),tablerefs,ntablerefs,&a))return 0;
          if(first<glwasm_text_size(n)&&glwasm_text_kind(at(n,first))!=GLWASM_TEXT_LIST&&
              !resolve_index(at(n,first++),tablerefs,ntablerefs,&c))return 0;
        } else if(bulk[bi].sub==10){
          /* The MVP spelling uses the default memory/table for both sides. */
        } else if(bulk[bi].sub>=15){
          if(first<glwasm_text_size(n)&&glwasm_text_kind(at(n,first))!=GLWASM_TEXT_LIST&&resolve_index(at(n,first),tablerefs,ntablerefs,&c))first++;
        }
        for(size_t j=first;j<glwasm_text_size(n);j++)if(!instruction(b,at(n,j),e,refs,nrefs,lrefs,nlrefs,grefs,ngrefs,trefs,ntrefs,tagrefs,ntagrefs,tablerefs,ntablerefs,memoryrefs,nmemoryrefs,blockrefs,nblockrefs,elemrefs,nelemrefs,datarefs,ndatarefs,labels,nlabels))return 0;
        if(!put(b,0xfc)||!uleb(b,bulk[bi].sub))return 0;
        if(bulk[bi].sub==8)return uleb(b,a)&&uleb(b,c);
        if(bulk[bi].sub==9||bulk[bi].sub==13)return uleb(b,a);
        if(bulk[bi].sub==12)return uleb(b,a)&&uleb(b,c);
        if(bulk[bi].sub==10)return uleb(b,a)&&uleb(b,c);
        if(bulk[bi].sub==14)return uleb(b,a)&&uleb(b,c);
        return uleb(b,c);
      }
    }
    if((!strcmp(name,"table.get")||!strcmp(name,"table.set"))&&glwasm_text_size(n)>=2){
      size_t first=1;uint32_t table=0,explicit_table;
      if(glwasm_text_kind(at(n,1))!=GLWASM_TEXT_LIST&&resolve_index(at(n,1),tablerefs,ntablerefs,&explicit_table)){
        table=explicit_table;first++;
      }
      for(size_t j=first;j<glwasm_text_size(n);j++)if(!instruction(b,at(n,j),e,refs,nrefs,lrefs,nlrefs,grefs,ngrefs,trefs,ntrefs,tagrefs,ntagrefs,tablerefs,ntablerefs,memoryrefs,nmemoryrefs,blockrefs,nblockrefs,elemrefs,nelemrefs,datarefs,ndatarefs,labels,nlabels))return 0;
      return put(b,!strcmp(name,"table.get")?0x25:0x26)&&uleb(b,table);
    }
    if(e){e->location=glwasm_text_location(n);e->message=deferred_instruction_name(name)?"unsupported or malformed deferred instruction in WAT lowering":"unknown WAT operator";e->result=deferred_instruction_name(name)?GLWASM_ERR_UNSUPPORTED:GLWASM_ERR_CORRUPT;}
    return 0;
  }
  size_t z;const uint8_t*p=glwasm_text_bytes(n,&z);if(!p)return 0;
  if(z==13&&!memcmp(p,"call_indirect",13)){
    for(size_t i=0;i<ntrefs;i++)if(trefs[i].node==n)return put(b,0x11)&&uleb(b,trefs[i].index)&&put(b,0);
    return 0;
  }
  if(z==10&&!memcmp(p,"table.size",10))return put(b,0xfc)&&uleb(b,16)&&uleb(b,0);
  if(z>9&&!memcmp(p,"i32.const",9)){int64_t v;if(!parse_integer_constant(n,32,&v)){numeric_error(e,n);return 0;}return put(b,0x41)&&sleb(b,v);}
  if(z>9&&!memcmp(p,"i64.const",9)){int64_t v;if(!parse_integer_constant(n,64,&v)){numeric_error(e,n);return 0;}return put(b,0x42)&&sleb(b,v);}
  {char name[32];uint8_t op;uint32_t sub;if(z<sizeof(name)){memcpy(name,p,z);name[z]=0;if(simple_op(name,&op))return put(b,op);if(saturating_op(name,&sub))return put(b,0xfc)&&uleb(b,sub);}}
  if(e){char unknown[32]={0};if(z<sizeof(unknown)){memcpy(unknown,p,z);unknown[z]=0;}e->location=glwasm_text_location(n);e->message=deferred_instruction_name(unknown)?"unsupported deferred instruction in WAT lowering":"unknown WAT operator";e->result=deferred_instruction_name(unknown)?GLWASM_ERR_UNSUPPORTED:GLWASM_ERR_CORRUPT;}
  return 0;
}

static GLWASM_Result wat_module_parse_impl(const uint8_t*s,size_t n,
    const GLWASM_Limits*limits,const GLWASM_Allocator*a,GLWASM_ParseError*e,
    GLWASM_Module**out,int strip_annotations,uint8_t**out_binary,
    size_t*out_binary_length){
  GLWASM_Text*tree=NULL;uint8_t*cleaned=NULL;GLWASM_Result r;const char*phase="module scan";Bytes module={0},types={0},imports={0},funcs={0},tables={0},mems={0},globals={0},elems={0},code={0},datas={0},exports={0},tags={0};
  const GLWASM_Text*root,*mod;size_t nf=0,nimports=0,nallimports=0,ntypefields=0,nimplicit=0,ncalltypes=0,ntyperefs=0;
  const GLWASM_Text*start_form=NULL;
  size_t ntables=0,nmems=0,nglobals=0,nelems=0,ndatas=0,nmemimports=0,ntableimports=0,nglobalimports=0,ntagdefs=0,ntagimports=0,ntagimplicit=0;uint32_t nexports=0;
  NameRef*fnames=NULL,*typenames=NULL,*localrefs=NULL,*globalrefs=NULL,*tagrefs=NULL,*tablerefs=NULL,*memoryrefs=NULL,*blockrefs=NULL,*elemrefs=NULL,*datarefs=NULL;TagInfo*taginfos=NULL;size_t named=0,typenamed=0,nlocalrefs=0,nglobalrefs=0,ntagrefs=0,ntablerefs=0,nmemoryrefs=0,nblockrefs=0,nelemrefs=0,ndatarefs=0,blockref_capacity=0;
  size_t localref_capacity=0,localref_limit=limits?limits->locals:1000;
  if(!out||(!s&&n)||(out_binary&&!out_binary_length)||
      (!out_binary&&out_binary_length))return GLWASM_ERR_INVALID;
  if(out_binary)*out_binary=NULL;
  if(out_binary_length)*out_binary_length=0;
  if(!valid_wat_source(s,n,e))return GLWASM_ERR_CORRUPT;
  if(strip_annotations)cleaned=strip_wat_annotations(s,n);
  else {cleaned=malloc(n?n:1);if(cleaned&&n)memcpy(cleaned,s,n);}
  if(!cleaned)return GLWASM_ERR_OOM;
  r=glwasm_text_parse(cleaned,n,a,e,&tree);if(r!=GLWASM_OK){free(cleaned);return r;}
  if(!valid_annotations(tree,e)||!valid_wat_atoms(tree,e)){r=GLWASM_ERR_CORRUPT;goto done;}
  if(glwasm_text_size(tree)!=1||(mod=at(tree,0))==NULL||!atom(at(mod,0),"module")){
    if(e){e->result=GLWASM_ERR_CORRUPT;e->location=glwasm_text_location(tree);e->message="expected one module form";}
    r=GLWASM_ERR_CORRUPT;goto done;
  }
  if(!valid_inline_type_order(mod,mod,e)){r=GLWASM_ERR_CORRUPT;goto done;}
  root=mod;
  size_t first_field=named_atom(at(root,1))?2:1;
  {
    int saw_definition=0;
    for(size_t i=first_field;i<glwasm_text_size(root);i++){
      const GLWASM_Text*field=at(root,i);
      int importing=atom(at(field,0),"import")||inline_import(field)!=NULL;
      if(annotation_node(field)||atom(at(field,0),"type"))continue;
      if(importing&&saw_definition){malformed_error(e,field,"import follows a module definition");r=GLWASM_ERR_CORRUPT;goto done;}
      if(!importing)saw_definition=1;
    }
  }
  if(duplicate_module_names(root,first_field,"func","duplicate function identifier",e)||
     duplicate_module_names(root,first_field,"global","duplicate global identifier",e)||
     duplicate_module_names(root,first_field,"table","duplicate table identifier",e)||
     duplicate_module_names(root,first_field,"memory","duplicate memory identifier",e)){
    r=GLWASM_ERR_CORRUPT;goto done;
  }
  for(size_t i=first_field;i<glwasm_text_size(root);i++){
    const GLWASM_Text*f=at(root,i);
    if(atom(at(f,0),"func")&&!inline_import(f)){
      const GLWASM_Text*dup=duplicate_local_name(f);
      if(dup){malformed_error(e,dup,"duplicate local identifier");r=GLWASM_ERR_CORRUPT;goto done;}
    }
  }
  for(size_t i=first_field;i<glwasm_text_size(root);i++)if(atom(at(at(root,i),0),"func")){
    size_t count=untyped_indirect_count(at(root,i));if(count>SIZE_MAX-ncalltypes){r=GLWASM_ERR_LIMIT;goto done;}ncalltypes+=count;
  }
  for(size_t i=first_field;i<glwasm_text_size(root);i++) {
    const GLWASM_Text *field=at(root,i);
    if(annotation_node(field))continue;
    if(atom(at(field,0),"func")){if(inline_import(field)){nallimports++;nimports++;}else nf++;}
    else if(atom(at(field,0),"import")){
      nallimports++;
      const GLWASM_Text*d=at(field,3);
      if(atom(at(d,0),"func"))nimports++;
      else if(atom(at(d,0),"table"))ntableimports++;
      else if(atom(at(d,0),"tag"))ntagimports++;
      else if(atom(at(d,0),"global"))nglobalimports++;
      else if(atom(at(d,0),"memory"))nmemimports++;
      else {if(e){e->result=GLWASM_ERR_UNSUPPORTED;e->location=glwasm_text_location(field);e->message="unsupported import descriptor";}r=GLWASM_ERR_UNSUPPORTED;goto done;}
    }
    else if(atom(at(field,0),"table")){if(inline_import(field)){nallimports++;ntableimports++;}else {ntables++;for(size_t j=1;j<glwasm_text_size(field);j++)if(atom(at(at(field,j),0),"elem"))nelems++;}}
    else if(atom(at(field,0),"memory")){if(inline_import(field)){nallimports++;nmemimports++;}else {nmems++;for(size_t j=1;j<glwasm_text_size(field);j++)if(atom(at(at(field,j),0),"data"))ndatas++;}}
    else if(atom(at(field,0),"global")){
      if(inline_import(field)){nallimports++;nglobalimports++;}
      else nglobals++;
    }
    else if(atom(at(field,0),"tag")){
      if(inline_import(field)){nallimports++;ntagimports++;}
      else ntagdefs++;
    }
    else if(atom(at(field,0),"elem")) nelems++;
    else if(atom(at(field,0),"data")) ndatas++;
    else if(atom(at(field,0),"type")) ntypefields++;
    else if(atom(at(field,0),"export")) { }
    else if(atom(at(field,0),"start")){if(start_form||glwasm_text_size(field)!=2){if(e){e->result=GLWASM_ERR_CORRUPT;e->location=glwasm_text_location(field);e->message="malformed or duplicate start field";}r=GLWASM_ERR_CORRUPT;goto done;}start_form=field;}
    else {
      if(glwasm_text_kind(at(field,0))!=GLWASM_TEXT_ATOM){malformed_error(e,field,"unknown module operator");r=GLWASM_ERR_CORRUPT;goto done;}
      if(e){e->result=GLWASM_ERR_UNSUPPORTED;e->location=glwasm_text_location(field);e->message="module field is not yet supported by WAT lowering";}
      r=GLWASM_ERR_UNSUPPORTED;goto done;
    }
  }
  if(nf>UINT32_MAX||nimports>UINT32_MAX||nallimports>UINT32_MAX||nf+nimports>UINT32_MAX){r=GLWASM_ERR_LIMIT;goto done;}
  if(ntypefields||ncalltypes){typenames=calloc(ntypefields+ncalltypes,sizeof(*typenames));if(!typenames){r=GLWASM_ERR_OOM;goto done;}uint32_t tx=0;for(size_t i=first_field;i<glwasm_text_size(root);i++){const GLWASM_Text*t=at(root,i);if(!atom(at(t,0),"type"))continue;size_t z;const uint8_t*p=glwasm_text_bytes(at(t,1),&z);if(p&&z&&p[0]=='$')typenames[typenamed++]=(NameRef){at(t,1),tx};tx++;}}
  ntyperefs=typenamed;
  if(nglobalimports+nglobals){globalrefs=calloc(nglobalimports+nglobals,sizeof(*globalrefs));if(!globalrefs){r=GLWASM_ERR_OOM;goto done;}
    uint32_t gx=0;
    for(size_t i=first_field;i<glwasm_text_size(root);i++){
      const GLWASM_Text*x=at(root,i),*g=NULL;
      if(atom(at(x,0),"import")){g=at(x,3);if(!atom(at(g,0),"global"))continue;}
      else if(atom(at(x,0),"global")&&inline_import(x))g=x;
      else continue;
      if(named_atom(at(g,1)))globalrefs[nglobalrefs++]=(NameRef){at(g,1),gx};
      gx++;
    }
    for(size_t i=first_field;i<glwasm_text_size(root);i++){const GLWASM_Text*g=at(root,i);if(!atom(at(g,0),"global")||inline_import(g))continue;if(named_atom(at(g,1)))globalrefs[nglobalrefs++]=(NameRef){at(g,1),gx};gx++;}
  }
  if(ntableimports+ntables){tablerefs=calloc(ntableimports+ntables,sizeof(*tablerefs));if(!tablerefs){r=GLWASM_ERR_OOM;goto done;}
    uint32_t tx=0;
    for(size_t i=first_field;i<glwasm_text_size(root);i++){const GLWASM_Text*x=at(root,i);const GLWASM_Text*t=atom(at(x,0),"import")?at(x,3):(atom(at(x,0),"table")&&inline_import(x)?x:NULL);if(!t||!atom(at(t,0),"table"))continue;if(named_atom(at(t,1)))tablerefs[ntablerefs++]=(NameRef){at(t,1),tx};tx++;}
    for(size_t i=first_field;i<glwasm_text_size(root);i++){const GLWASM_Text*t=at(root,i);if(!atom(at(t,0),"table")||inline_import(t))continue;if(named_atom(at(t,1)))tablerefs[ntablerefs++]=(NameRef){at(t,1),tx};tx++;}
  }
  if(nmemimports+nmems){memoryrefs=calloc(nmemimports+nmems,sizeof(*memoryrefs));if(!memoryrefs){r=GLWASM_ERR_OOM;goto done;}
    uint32_t mi=0;
    for(size_t i=first_field;i<glwasm_text_size(root);i++){const GLWASM_Text*x=at(root,i);const GLWASM_Text*m=atom(at(x,0),"import")?at(x,3):(atom(at(x,0),"memory")&&inline_import(x)?x:NULL);if(!m||!atom(at(m,0),"memory"))continue;if(named_atom(at(m,1)))memoryrefs[nmemoryrefs++]=(NameRef){at(m,1),mi};mi++;}
    for(size_t i=first_field;i<glwasm_text_size(root);i++){const GLWASM_Text*m=at(root,i);if(!atom(at(m,0),"memory")||inline_import(m))continue;if(named_atom(at(m,1)))memoryrefs[nmemoryrefs++]=(NameRef){at(m,1),mi};mi++;}
  }
  if(nelems){elemrefs=calloc(nelems,sizeof(*elemrefs));if(!elemrefs){r=GLWASM_ERR_OOM;goto done;}
    uint32_t ex=0;
    for(size_t i=first_field;i<glwasm_text_size(root);i++){
      const GLWASM_Text*segment=at(root,i);
      if(!atom(at(segment,0),"elem"))continue;
      if(named_atom(at(segment,1)))elemrefs[nelemrefs++]=(NameRef){at(segment,1),ex};
      ex++;
    }
  }
  if(ndatas){datarefs=calloc(ndatas,sizeof(*datarefs));if(!datarefs){r=GLWASM_ERR_OOM;goto done;}
    uint32_t dx=0;
    for(size_t i=first_field;i<glwasm_text_size(root);i++){
      const GLWASM_Text*d=at(root,i);
      if(atom(at(d,0),"memory")&&!inline_import(d)){for(size_t j=1;j<glwasm_text_size(d);j++)if(atom(at(at(d,j),0),"data"))dx++;continue;}
      if(!atom(at(d,0),"data"))continue;
      if(named_atom(at(d,1)))datarefs[ndatarefs++]=(NameRef){at(d,1),dx};
      dx++;
    }
  }
  nimplicit=implicit_signature_count(root,first_field);
  if(ntagdefs+ntagimports){
    taginfos=calloc(ntagdefs+ntagimports,sizeof(*taginfos));
    tagrefs=calloc(ntagdefs+ntagimports,sizeof(*tagrefs));
    if(!taginfos||!tagrefs){r=GLWASM_ERR_OOM;goto done;}
    size_t ti=0,implicit_tag=0;uint32_t tag_index=0;
    for(size_t pass=0;pass<2;pass++)for(size_t i=first_field;i<glwasm_text_size(root);i++){
      const GLWASM_Text*x=at(root,i),*t=NULL;
      if(pass==0&&atom(at(x,0),"import")&&atom(at(at(x,3),0),"tag"))t=at(x,3);
      if(pass==0&&atom(at(x,0),"tag")&&inline_import(x))t=x;
      if(pass==1&&atom(at(x,0),"tag")&&!import_use(x))t=x;
      if(!t)continue;
      const GLWASM_Text*tu=type_use(t);uint32_t type_index;
      if(tu){if(glwasm_text_size(tu)!=2||!resolve_type(at(tu,1),typenames,ntyperefs,&type_index)){if(e){e->result=GLWASM_ERR_UNSUPPORTED;e->location=glwasm_text_location(tu);e->message="tag has an unresolved type use";}r=GLWASM_ERR_UNSUPPORTED;goto done;}}
      else {type_index=(uint32_t)(ntypefields+nimplicit+implicit_tag++);ntagimplicit++;}
      taginfos[ti]=(TagInfo){t,tag_index,type_index};
      if(named_atom(at(t,1)))tagrefs[ntagrefs++]=(NameRef){at(t,1),tag_index};
      ti++;tag_index++;
    }
  }
  if(nf+nimports){fnames=calloc(nf+nimports,sizeof(*fnames));if(!fnames){r=GLWASM_ERR_OOM;goto done;}
    uint32_t index=0;for(size_t i=first_field;i<glwasm_text_size(root);i++){
      const GLWASM_Text*imp=at(root,i),*f=NULL;
      if(atom(at(imp,0),"import"))f=at(imp,3);
      else if(atom(at(imp,0),"func")&&inline_import(imp))f=imp;
      if(f&&atom(at(f,0),"func")){
        size_t z;const uint8_t*p=glwasm_text_bytes(at(f,1),&z);
        if(p&&z&&p[0]=='$')fnames[named++]=(NameRef){at(f,1),index};
        index++;
      }
    }
    for(size_t i=first_field;i<glwasm_text_size(root);i++){
      const GLWASM_Text*f=at(root,i);if(!atom(at(f,0),"func")||inline_import(f))continue;
      size_t z;const uint8_t*p=glwasm_text_bytes(at(f,1),&z);
      if(p&&z&&p[0]=='$')fnames[named++]=(NameRef){at(f,1),index};
      index++;
    }
  }
  {uint32_t block_index=(uint32_t)(ntypefields+nimplicit+ntagimplicit);
    for(size_t i=first_field;i<glwasm_text_size(root);i++){const GLWASM_Text*f=at(root,i);if(atom(at(f,0),"func")&&!collect_blockrefs(f,&blockrefs,&nblockrefs,&blockref_capacity,&block_index)){r=GLWASM_ERR_OOM;goto done;}}
  }
  if(ncalltypes){
    size_t mapped=0;uint64_t base=(uint64_t)ntypefields+nimplicit+ntagimplicit+nblockrefs;
    if(base>UINT32_MAX||!typenames){r=GLWASM_ERR_LIMIT;goto done;}
    for(size_t i=first_field;i<glwasm_text_size(root);i++){const GLWASM_Text*f=at(root,i);if(atom(at(f,0),"func")&&!map_untyped_indirects(f,typenames+ntyperefs,&mapped,ncalltypes,(uint32_t)base)){r=GLWASM_ERR_INTERNAL;goto done;}}
    ntyperefs+=mapped;
  }
  if(!bytes(&module,(const uint8_t*)"\0asm\1\0\0\0",8)){r=GLWASM_ERR_OOM;goto done;}
  if(nf+nimports||nallimports||ntagdefs||ntypefields){
    if(ntypefields+nimplicit+ntagimplicit+nblockrefs+ncalltypes>UINT32_MAX||!uleb(&types,(uint32_t)(ntypefields+nimplicit+ntagimplicit+nblockrefs+ncalltypes))){r=GLWASM_ERR_LIMIT;goto done;}
    for(size_t i=first_field;i<glwasm_text_size(root);i++){const GLWASM_Text*t=at(root,i);if(!atom(at(t,0),"type"))continue;const GLWASM_Text*f=named_atom(at(t,1))?at(t,2):at(t,1);if(!atom(at(f,0),"func")||!emit_signature(&types,f,e,typenames,typenamed)){r=e&&e->result?e->result:GLWASM_ERR_UNSUPPORTED;goto done;}}
    if(!emit_implicit_signatures(&types,root,first_field,e,typenames,typenamed)){r=e&&e->result?e->result:GLWASM_ERR_UNSUPPORTED;goto done;}
    if(nallimports&&!uleb(&imports,(uint32_t)nallimports)){r=GLWASM_ERR_OOM;goto done;}
    uint32_t imported_global_index=0,imported_memory_index=0,imported_table_index=0,imported_func_index=0;
    for(size_t i=first_field;i<glwasm_text_size(root);i++){
      const GLWASM_Text*x=at(root,i),*imp=NULL,*f=NULL;int inline_global=0,inline_memory=0,inline_table=0,inline_func=0,inline_tag=0;
      if(atom(at(x,0),"import")){imp=x;f=at(x,3);}
      else if(atom(at(x,0),"func")&&(imp=inline_import(x))){f=x;inline_func=1;}
      else if(atom(at(x,0),"global")&&(imp=inline_import(x))){f=x;inline_global=1;}
      else if(atom(at(x,0),"memory")&&(imp=inline_import(x))){f=x;inline_memory=1;}
      else if(atom(at(x,0),"table")&&(imp=inline_import(x))){f=x;inline_table=1;}
      else if(atom(at(x,0),"tag")&&(imp=inline_import(x))){f=x;inline_tag=1;}
      else continue;
      size_t ml,fl;const uint8_t*mn=glwasm_text_bytes(at(imp,1),&ml),*fn=glwasm_text_bytes(at(imp,2),&fl);
      Bytes desc={0};uint8_t kind;
      if(((inline_global||inline_memory||inline_table||inline_func||inline_tag)?glwasm_text_size(imp)!=3:glwasm_text_size(imp)!=4)||
          (!mn&&ml)||(!fn&&fl)||ml>UINT32_MAX||fl>UINT32_MAX){
        if(e){e->result=GLWASM_ERR_CORRUPT;e->location=glwasm_text_location(x);e->message="malformed import declaration";}
        r=GLWASM_ERR_CORRUPT;goto done;
      }
      if(atom(at(f,0),"func")){
        kind=0;const GLWASM_Text*tu=type_use(f);uint32_t typeix;
        if(tu){if(glwasm_text_size(tu)!=2||!resolve_type(at(tu,1),typenames,ntyperefs,&typeix)){free(desc.p);if(e){e->result=GLWASM_ERR_UNSUPPORTED;e->location=glwasm_text_location(tu);e->message="function import has an unresolved type use";}r=GLWASM_ERR_UNSUPPORTED;goto done;}}
        else if(!function_type_index(root,first_field,ntypefields,f,&typeix)){free(desc.p);r=GLWASM_ERR_INTERNAL;goto done;}
        if(!uleb(&desc,typeix)){free(desc.p);r=GLWASM_ERR_OOM;goto done;}
        if(inline_func){for(size_t j=1;j<glwasm_text_size(f);j++)if(atom(at(at(f,j),0),"export")){if(!append_export(&exports,&nexports,at(f,j),0,imported_func_index))goto unsupported_import;} }
        imported_func_index++;
      } else if(atom(at(f,0),"table")){kind=1;if(!tabletype(f,&desc,typenames,ntyperefs))goto unsupported_import;if(inline_table){for(size_t j=1;j<glwasm_text_size(f);j++)if(atom(at(at(f,j),0),"export")&&!append_export(&exports,&nexports,at(f,j),1,imported_table_index))goto unsupported_import;}imported_table_index++;}
      else if(atom(at(f,0),"memory")){kind=2;if(!memorytype(f,&desc))goto unsupported_import;if(inline_memory){for(size_t j=1;j<glwasm_text_size(f);j++)if(atom(at(at(f,j),0),"export")&&!append_export(&exports,&nexports,at(f,j),2,imported_memory_index))goto unsupported_import;}imported_memory_index++;}
      else if(atom(at(f,0),"global")){
        kind=3;
        if(inline_global){
          size_t gi=1;int mut=0;
          if(named_atom(at(f,gi)))gi++;
          for(;atom(at(at(f,gi),0),"export");gi++)
            if(!append_export(&exports,&nexports,at(f,gi),3,imported_global_index))goto unsupported_import;
          if(at(f,gi)==imp)gi++;
          const GLWASM_Text*type=at(f,gi);
          if(atom(at(type,0),"mut")){mut=1;type=at(type,1);}
          if(!value_type(&desc,type,typenames,ntyperefs)||!put(&desc,mut?1:0))goto unsupported_import;
        }
        else if(!globaltype(f,&desc,typenames,ntyperefs))goto unsupported_import;
        imported_global_index++;
      }
      else if(atom(at(f,0),"tag")){kind=4;const TagInfo*info=tag_info_for(taginfos,ntagdefs+ntagimports,f);if(!info||!put(&desc,0)||!uleb(&desc,info->type_index))goto unsupported_import;}
      else {
unsupported_import:
        free(desc.p);
        if(e){e->result=GLWASM_ERR_UNSUPPORTED;e->location=glwasm_text_location(x);e->message="unsupported import descriptor";}
        r=GLWASM_ERR_UNSUPPORTED;goto done;
      }
      if(!uleb(&imports,(uint32_t)ml)||!bytes(&imports,mn,ml)||!uleb(&imports,(uint32_t)fl)||!bytes(&imports,fn,fl)||!put(&imports,kind)||!bytes(&imports,desc.p,desc.n)){free(desc.p);r=GLWASM_ERR_OOM;goto done;}
      free(desc.p);
    }
    /* The function section has one type index per defined function. */
    if(!uleb(&funcs,(uint32_t)nf)||!uleb(&code,(uint32_t)nf)){r=GLWASM_ERR_OOM;goto done;}
    uint32_t fi=0;
    for(size_t i=first_field;i<glwasm_text_size(root);i++){
      const GLWASM_Text*f=at(root,i);if(!atom(at(f,0),"func")||inline_import(f))continue;
      free(localrefs);localrefs=NULL;nlocalrefs=0;localref_capacity=0;
      const GLWASM_Text*tu=type_use(f);
      Bytes body={0},locals={0};uint32_t localgroups=0;
      Bytes params={0},results={0};uint32_t nparams=0,nresults=0,nlocals=0;
      NameRef label_stack[256];size_t label_count=0;
      int signature_done=0,saw_type_use=0,saw_param_group=0,saw_result_group=0,saw_local_group=0;
      for(size_t j=1;j<glwasm_text_size(f);j++){
        const GLWASM_Text*x=at(f,j);
        size_t xz;const uint8_t*xp=glwasm_text_bytes(x,&xz);
        if(j==1&&xp&&xz&&xp[0]=='$')continue;
        if(signature_done)continue;
        if(atom(at(x,0),"export")){
          size_t len;const uint8_t*name=glwasm_text_bytes(at(x,1),&len);
          if(glwasm_text_size(x)!=2||(!name&&len)||len>UINT32_MAX||
             !uleb(&exports,(uint32_t)len)||!bytes(&exports,name,len)||
             !put(&exports,0)||!uleb(&exports,(uint32_t)nimports+fi)){
            if(e){e->result=GLWASM_ERR_CORRUPT;e->location=glwasm_text_location(x);e->message="malformed inline function export";}
            free(body.p);free(params.p);free(results.p);free(locals.p);r=GLWASM_ERR_CORRUPT;goto done;
          }
          nexports++;continue;
        }
        if(atom(at(x,0),"local")){
          saw_local_group=1;
          for(size_t k=1;k<glwasm_text_size(x);k++){
            const GLWASM_Text*name=NULL;
            if(named_atom(at(x,k))){name=at(x,k);k++;}
            GLWASM_Result name_result=GLWASM_OK;
            if(k<glwasm_text_size(x)&&localgroups!=UINT32_MAX)
              name_result=local_name_add(&localrefs,&nlocalrefs,
                  &localref_capacity,localref_limit,name,nparams+nlocals);
            if(k>=glwasm_text_size(x)||localgroups==UINT32_MAX||
               name_result!=GLWASM_OK||
               !uleb(&locals,1)||!value_type(&locals,at(x,k),typenames,ntyperefs)){
              r=name_result!=GLWASM_OK?name_result:GLWASM_ERR_UNSUPPORTED;
              if(e){e->result=r;e->location=glwasm_text_location(x);e->message=r==GLWASM_ERR_OOM?"local name allocation failed":r==GLWASM_ERR_LIMIT?"local name limit exceeded":"unsupported local declaration";}
              free(body.p);free(params.p);free(results.p);free(locals.p);goto done;
            }
            localgroups++;
            nlocals++;
          }
          continue;
        }
        if(atom(at(x,0),"param")){
          if(saw_result_group||saw_local_group){malformed_error(e,x,"misordered function signature");free(body.p);free(params.p);free(results.p);free(locals.p);r=GLWASM_ERR_CORRUPT;goto done;}
          saw_param_group=1;
          for(size_t k=1;k<glwasm_text_size(x);k++){
            const GLWASM_Text*name=NULL;
            if(named_atom(at(x,k))){name=at(x,k);k++;}
            GLWASM_Result name_result=GLWASM_OK;
            if(k<glwasm_text_size(x)&&nparams!=UINT32_MAX)
              name_result=local_name_add(&localrefs,&nlocalrefs,
                  &localref_capacity,localref_limit,name,nparams);
            if(k>=glwasm_text_size(x)||nparams==UINT32_MAX||
                name_result!=GLWASM_OK||!value_type(&params,at(x,k),typenames,ntyperefs)){
              r=name_result!=GLWASM_OK?name_result:GLWASM_ERR_UNSUPPORTED;
              if(e){e->result=r;e->location=glwasm_text_location(x);e->message=r==GLWASM_ERR_OOM?"local name allocation failed":r==GLWASM_ERR_LIMIT?"local name limit exceeded":"unsupported function parameter type";}
          free(body.p);free(params.p);free(results.p);free(locals.p);goto done;
            }
            nparams++;
          }
          continue;
        }
        if(atom(at(x,0),"result")){
          if(saw_local_group){malformed_error(e,x,"misordered function signature");free(body.p);free(params.p);free(results.p);free(locals.p);r=GLWASM_ERR_CORRUPT;goto done;}
          saw_result_group=1;
          for(size_t k=1;k<glwasm_text_size(x);k++){if(nresults==UINT32_MAX||!value_type(&results,at(x,k),typenames,ntyperefs)){if(e){e->result=GLWASM_ERR_UNSUPPORTED;e->location=glwasm_text_location(x);e->message="unsupported function result type";}free(body.p);free(params.p);free(results.p);free(locals.p);r=GLWASM_ERR_UNSUPPORTED;goto done;}nresults++;}
          continue;
        }
        if(atom(at(x,0),"type")){if(saw_type_use||saw_param_group||saw_result_group||saw_local_group){malformed_error(e,x,"misordered function type use");free(body.p);free(params.p);free(results.p);free(locals.p);r=GLWASM_ERR_CORRUPT;goto done;}saw_type_use=1;continue;}
        signature_done=1;
      }
      uint32_t typeix;
      if(tu){if(glwasm_text_size(tu)!=2||!resolve_type(at(tu,1),typenames,ntyperefs,&typeix)){if(e){e->result=GLWASM_ERR_UNSUPPORTED;e->location=glwasm_text_location(tu);e->message="function type use must resolve to a declared type";}free(body.p);free(params.p);free(results.p);free(locals.p);r=GLWASM_ERR_UNSUPPORTED;goto done;}
        if(nparams==0){uint32_t offset=type_use_param_count(root,first_field,tu,typenames,ntyperefs);for(size_t q=0;q<nlocalrefs;q++)localrefs[q].index+=offset;}
        if(nparams||nresults){const GLWASM_Text*decl=explicit_type_body(root,first_field,typeix);if(decl&&!same_signature(f,decl)){malformed_error(e,tu,"inline function type does not match type use");free(body.p);free(params.p);free(results.p);free(locals.p);r=GLWASM_ERR_CORRUPT;goto done;}}
      }
      else if(!function_type_index(root,first_field,ntypefields,f,&typeix)){
        free(body.p);free(params.p);free(results.p);free(locals.p);r=GLWASM_ERR_INTERNAL;goto done;
      }
      if(!uleb(&funcs,typeix)){free(body.p);free(params.p);free(results.p);free(locals.p);r=GLWASM_ERR_OOM;goto done;}
      free(params.p);free(results.p);
      for(size_t j=1;j<glwasm_text_size(f);j++){
        const GLWASM_Text*x=at(f,j);int64_t imm;size_t z;const uint8_t*p;
        if(atom(at(x,0),"result")||atom(at(x,0),"param")||atom(at(x,0),"local")||atom(at(x,0),"export")||atom(at(x,0),"type"))continue;
        p=glwasm_text_bytes(x,&z);
        if(j==1&&p&&z&&p[0]=='$')continue;
        if(p&&((z==9&&!memcmp(p,"i32.const",9))||(z==9&&!memcmp(p,"i64.const",9)))){
          if(j+1>=glwasm_text_size(f)||!parse_integer_constant(at(f,j+1),p[1]=='3'?32:64,&imm)||
             !put(&body,p[1]=='3'?0x41:0x42)||!sleb(&body,imm)){
            if(j+1<glwasm_text_size(f))numeric_error(e,at(f,j+1));
            free(body.p);free(locals.p);r=GLWASM_ERR_CORRUPT;goto done;
          }
          j++;continue;
        }
        if(p&&((z==9&&!memcmp(p,"f32.const",9))||(z==9&&!memcmp(p,"f64.const",9)))){
          Bytes imm={0};int is64=p[1]=='6';
          if(j+1>=glwasm_text_size(f)||!parse_float_bits(at(f,j+1),is64,&imm)||!put(&body,is64?0x44:0x43)||!bytes(&body,imm.p,imm.n)){
            if(j+1<glwasm_text_size(f))numeric_error(e,at(f,j+1));
            free(imm.p);free(body.p);free(locals.p);r=GLWASM_ERR_CORRUPT;goto done;
          }
          free(imm.p);j++;continue;
        }
        if(p&&((z==13&&!memcmp(p,"call_indirect",13))||
               (z==20&&!memcmp(p,"return_call_indirect",20)))){
          const GLWASM_Text*tu=NULL;uint32_t ti,table=0;size_t k=j+1;
          while(k<glwasm_text_size(f)){
            const GLWASM_Text*imm=at(f,k);
            if(atom(at(imm,0),"type")&&glwasm_text_size(imm)==2){tu=imm;k++;continue;}
            if(atom(at(imm,0),"table")&&glwasm_text_size(imm)==2){
              if(!resolve_index(at(imm,1),tablerefs,ntablerefs,&table)){free(body.p);free(locals.p);r=GLWASM_ERR_CORRUPT;goto done;}k++;continue;
            }
            if(k==j+1&&glwasm_text_kind(imm)!=GLWASM_TEXT_LIST){
              uint32_t numeric_table;
              if(named_atom(imm)||parse_u32(imm,&numeric_table)){
                if(!resolve_index(imm,tablerefs,ntablerefs,&table)){free(body.p);free(locals.p);r=GLWASM_ERR_CORRUPT;goto done;}k++;continue;
              }
            }
            break;
          }
          if(tu){if(!resolve_type(at(tu,1),typenames,ntyperefs,&ti)){free(body.p);free(locals.p);r=GLWASM_ERR_CORRUPT;goto done;}}
          else {
            size_t q;for(q=0;q<ntyperefs;q++)if(typenames[q].node==at(f,j)){ti=typenames[q].index;break;}
            if(q==ntyperefs){if(e){e->result=GLWASM_ERR_UNSUPPORTED;e->location=glwasm_text_location(at(f,j));e->message="call_indirect missing synthesized type";}free(body.p);free(locals.p);r=GLWASM_ERR_UNSUPPORTED;goto done;}
          }
          for(;k<glwasm_text_size(f)&&glwasm_text_kind(at(f,k))==GLWASM_TEXT_LIST;k++){
            if(atom(at(at(f,k),0),"param")||atom(at(at(f,k),0),"result"))continue;
            if(!instruction(&body,at(f,k),e,fnames,named,localrefs,nlocalrefs,globalrefs,nglobalrefs,typenames,ntyperefs,tagrefs,ntagrefs,tablerefs,ntablerefs,memoryrefs,nmemoryrefs,blockrefs,nblockrefs,elemrefs,nelemrefs,datarefs,ndatarefs,label_stack,&label_count)){free(body.p);free(locals.p);r=e&&e->result?e->result:GLWASM_ERR_UNSUPPORTED;goto done;}
          }
          if(!put(&body,z==13?0x11:0x13)||!uleb(&body,ti)||!uleb(&body,table)){free(body.p);free(locals.p);r=GLWASM_ERR_UNSUPPORTED;goto done;}
          j=k-1;
          continue;
        }
        if(p&&((z==9&&!memcmp(p,"local.get",9))||(z==9&&!memcmp(p,"local.set",9))||
               (z==9&&!memcmp(p,"local.tee",9))||(z==10&&!memcmp(p,"global.get",10))||
               (z==10&&!memcmp(p,"global.set",10))||(z==4&&!memcmp(p,"call",4))||
               (z==11&&!memcmp(p,"return_call",11)))){
          uint32_t ix;const NameRef*idxrefs=(z==9)?localrefs:(z==10?globalrefs:fnames);size_t nidxrefs=(z==9)?nlocalrefs:(z==10?nglobalrefs:named);
          if(j+1>=glwasm_text_size(f)||!resolve_index(at(f,j+1),idxrefs,nidxrefs,&ix)){free(body.p);free(locals.p);r=GLWASM_ERR_CORRUPT;goto done;}
          uint8_t op=z==4?0x10:z==11?0x12:z==10?(!memcmp(p,"global.get",10)?0x23:0x24):!memcmp(p,"local.get",9)?0x20:!memcmp(p,"local.set",9)?0x21:0x22;
          if(!put(&body,op)||!uleb(&body,ix)){free(body.p);free(locals.p);r=GLWASM_ERR_OOM;goto done;}
          j++;continue;
        }
        if(p&&((z==5&&!memcmp(p,"block",5))||(z==4&&!memcmp(p,"loop",4))||(z==2&&!memcmp(p,"if",2)))){
          uint8_t bt=0x40;
          uint32_t block_type_index;
          const GLWASM_Text*label=NULL;
          if(j+1<glwasm_text_size(f)&&named_atom(at(f,j+1))){label=at(f,j+1);j++;}
          if(label_count>=256){free(body.p);free(locals.p);r=GLWASM_ERR_LIMIT;goto done;}
          label_stack[label_count++]=(NameRef){label,0};
          if(j+1<glwasm_text_size(f)&&atom(at(at(f,j+1),0),"result")){
            if(glwasm_text_size(at(f,j+1))!=2||!typebyte(at(at(f,j+1),1),&bt)){free(body.p);free(locals.p);r=GLWASM_ERR_UNSUPPORTED;goto done;}
            j++;
          }
          if(inline_blocktype_needs_index(f)&&find_blocktype(f,blockrefs,nblockrefs,&block_type_index)){
            if(!put(&body,z==5?0x02:z==4?0x03:0x04)||!sleb(&body,block_type_index)){free(body.p);free(locals.p);r=GLWASM_ERR_OOM;goto done;}
          }else if(!put(&body,z==5?0x02:z==4?0x03:0x04)||!put(&body,bt)){free(body.p);free(locals.p);r=GLWASM_ERR_OOM;goto done;}
          continue;
        }
        if(p&&z==4&&!memcmp(p,"else",4)){
          if(!put(&body,0x05)){free(body.p);free(locals.p);r=GLWASM_ERR_OOM;goto done;}
          if(j+1<glwasm_text_size(f)&&named_atom(at(f,j+1))){
            const GLWASM_Text*else_label=at(f,j+1);
            size_t ez=0,lz=0;const uint8_t*eb=glwasm_text_bytes(else_label,&ez);
            const uint8_t*lb=label_count?glwasm_text_bytes(label_stack[label_count-1].node,&lz):NULL;
            if(!lb||ez!=lz||memcmp(eb,lb,ez)){free(body.p);free(locals.p);r=GLWASM_ERR_CORRUPT;goto done;}
            j++;
          }
          continue;
        }
        if(p&&((z==2&&!memcmp(p,"br",2))||(z==5&&!memcmp(p,"br_if",5)))){
          uint32_t depth;if(j+1>=glwasm_text_size(f)||!resolve_depth(at(f,j+1),label_stack,label_count,&depth)||!put(&body,z==2?0x0c:0x0d)||!uleb(&body,depth)){
            free(body.p);free(locals.p);r=GLWASM_ERR_CORRUPT;goto done;
          }
          j++;continue;
        }
        if(p&&z==3&&!memcmp(p,"end",3)){
          if(!put(&body,0x0b)){free(body.p);free(locals.p);r=GLWASM_ERR_OOM;goto done;}
          if(j+1<glwasm_text_size(f)&&named_atom(at(f,j+1))){
            const GLWASM_Text*end_label=at(f,j+1);
            size_t ez=0,lz=0;const uint8_t*eb=glwasm_text_bytes(end_label,&ez);
            const uint8_t*lb=label_count?glwasm_text_bytes(label_stack[label_count-1].node,&lz):NULL;
            if(!lb||ez!=lz||memcmp(eb,lb,ez)){free(body.p);free(locals.p);r=GLWASM_ERR_CORRUPT;goto done;}
            j++;
          }
          if(label_count)label_count--;
          continue;
        }
        if(p&&z==5&&!memcmp(p,"throw",5)){
          uint32_t ti;if(j+1>=glwasm_text_size(f)||!resolve_index(at(f,j+1),tagrefs,ntagrefs,&ti)||!put(&body,0x08)||!uleb(&body,ti)){free(body.p);free(locals.p);r=GLWASM_ERR_UNSUPPORTED;goto done;}
          j++;continue;
        }
        if(p&&((z==11&&!memcmp(p,"memory.size",11))||(z==11&&!memcmp(p,"memory.grow",11)))){
          uint32_t mi=0;
          if(j+1<glwasm_text_size(f)&&glwasm_text_kind(at(f,j+1))!=GLWASM_TEXT_LIST&&resolve_index(at(f,j+1),memoryrefs,nmemoryrefs,&mi))j++;
          if(!put(&body,z==11&&!memcmp(p,"memory.size",11)?0x3f:0x40)||!uleb(&body,mi)){free(body.p);free(locals.p);r=GLWASM_ERR_OOM;goto done;}
          continue;
        }
        {uint8_t op;uint32_t align,memory=0;int has_memory=0;uint64_t offset=0;
          if(p&&memory_op(p,z,&op,&align)){
            while(j+1<glwasm_text_size(f)){
              const GLWASM_Text*a=at(f,j+1);
              uint64_t keyed;
              if(keyed_u64(a,"offset",&keyed)){offset=keyed;j++;continue;}
              if(keyed_u64(a,"align",&keyed)){
                if(!keyed||(keyed&(keyed-1))){malformed_error(e,a,"invalid memory alignment");free(body.p);free(locals.p);r=GLWASM_ERR_CORRUPT;goto done;}
                align=0;while(((uint64_t)1<<align)<keyed&&align<63)align++;j++;continue;
              }
              if(keyed_u64(a,"memory",&keyed)){
                if(keyed>UINT32_MAX){free(body.p);free(locals.p);r=GLWASM_ERR_CORRUPT;goto done;}
                memory=(uint32_t)keyed;has_memory=1;j++;continue;
              }
              if(named_atom(a)){
                if(!resolve_index(a,memoryrefs,nmemoryrefs,&memory)){free(body.p);free(locals.p);r=GLWASM_ERR_CORRUPT;goto done;}
                has_memory=1;j++;continue;
              }
              if(atom(at(a,0),"offset")){
                if(glwasm_text_size(a)!=2||!parse_u64(at(a,1),&offset)){free(body.p);free(locals.p);r=GLWASM_ERR_CORRUPT;goto done;}
                j++;continue;
              }
              if(atom(at(a,0),"align")){
                uint64_t bytesize;if(glwasm_text_size(a)!=2||!parse_u64(at(a,1),&bytesize)||!bytesize||(bytesize&(bytesize-1))){malformed_error(e,a,"invalid memory alignment");free(body.p);free(locals.p);r=GLWASM_ERR_CORRUPT;goto done;}
                align=0;while(((uint64_t)1<<align)<bytesize&&align<63)align++;j++;continue;
              }
              if(atom(at(a,0),"memory")){
                if(glwasm_text_size(a)!=2||!resolve_index(at(a,1),memoryrefs,nmemoryrefs,&memory)){free(body.p);free(locals.p);r=GLWASM_ERR_CORRUPT;goto done;}
                has_memory=1;j++;continue;
              }
              break;
            }
            if(!put(&body,op)||!uleb(&body,align|(has_memory?64u:0u))||(has_memory&&!uleb(&body,memory))||!uleb64(&body,offset)){free(body.p);free(locals.p);r=GLWASM_ERR_OOM;goto done;}
            continue;
          }
        }
        if(!instruction(&body,x,e,fnames,named,localrefs,nlocalrefs,globalrefs,nglobalrefs,typenames,ntyperefs,tagrefs,ntagrefs,tablerefs,ntablerefs,memoryrefs,nmemoryrefs,blockrefs,nblockrefs,elemrefs,nelemrefs,datarefs,ndatarefs,label_stack,&label_count)){
          if(e&&!e->message){e->result=GLWASM_ERR_UNSUPPORTED;e->location=glwasm_text_location(x);e->message="unsupported linear or folded instruction";}
          free(body.p);free(locals.p);r=e&&e->result?e->result:GLWASM_ERR_CORRUPT;goto done;
        }
      }
      if(!put(&body,0x0b)){free(body.p);free(locals.p);r=GLWASM_ERR_OOM;goto done;}
      Bytes enc={0},localvec={0};
      if(!uleb(&localvec,localgroups)||!bytes(&localvec,locals.p,locals.n)||
         !uleb(&enc,(uint32_t)(body.n+localvec.n))||
         !bytes(&code,enc.p,enc.n)||!bytes(&code,localvec.p,localvec.n)||!bytes(&code,body.p,body.n)){
        free(enc.p);free(localvec.p);free(body.p);free(locals.p);r=GLWASM_ERR_OOM;goto done;
      }
      free(enc.p);free(localvec.p);free(body.p);free(locals.p);
      free(localrefs);localrefs=NULL;nlocalrefs=0;
      fi++;
    }
    for(size_t i=0;i<ntagdefs+ntagimports;i++)if(taginfos[i].type_index>=ntypefields+nimplicit&&
        !emit_tag_signature(&types,taginfos[i].node,e)){r=e&&e->result?e->result:GLWASM_ERR_UNSUPPORTED;goto done;}
    for(size_t i=0;i<nblockrefs;i++)if(!emit_block_signature(&types,blockrefs[i].node,e,typenames,ntyperefs)){r=e&&e->result?e->result:GLWASM_ERR_UNSUPPORTED;goto done;}
    for(size_t i=0;i<ntyperefs-typenamed;i++){
      const GLWASM_Text*signature=typenames[typenamed+i].node;
      int emitted=glwasm_text_kind(signature)==GLWASM_TEXT_ATOM
          ?emit_flat_indirect_signature(&types,mod,signature,e)
          :emit_signature(&types,signature,e,typenames,typenamed);
      if(!emitted){r=e&&e->result?e->result:GLWASM_ERR_UNSUPPORTED;goto done;}
    }
    if(!section(&module,1,&types)||(nallimports&&!section(&module,2,&imports))||
       (nf&&!section(&module,3,&funcs))){r=GLWASM_ERR_OOM;goto done;}
  }
  phase="table lowering";
  if(ntables){
    if(!uleb(&tables,(uint32_t)ntables)){r=GLWASM_ERR_OOM;goto done;}
    uint32_t table_index=(uint32_t)ntableimports;
    for(size_t i=first_field;i<glwasm_text_size(root);i++){
      const GLWASM_Text*t=at(root,i);if(!atom(at(t,0),"table")||inline_import(t))continue;
      size_t z,ix=1,n=glwasm_text_size(t);const uint8_t*elem;uint8_t et;uint64_t min,max=0;int addr64=0;
      if(ix<n&&named_atom(at(t,ix)))ix++;
      for(;ix<n&&atom(at(at(t,ix),0),"export");ix++)
        if(!append_export(&exports,&nexports,at(t,ix),1,table_index)){
          if(e){e->result=GLWASM_ERR_CORRUPT;e->location=glwasm_text_location(at(t,ix));e->message="malformed inline table export";}
          r=GLWASM_ERR_CORRUPT;goto done;
        }
      if(ix<n&&named_atom(at(t,ix)))ix++;
      for(;ix<n&&atom(at(at(t,ix),0),"export");ix++){}
      if(ix<n&&atom(at(t,ix),"i64")){addr64=1;ix++;}
      const GLWASM_Text*inline_elem=NULL;
      const GLWASM_Text*table_element_type=NULL;
      for(size_t j=ix;j<n;j++)if(atom(at(at(t,j),0),"elem")){inline_elem=at(t,j);break;}
      const GLWASM_Text*init_expr=NULL;
      for(size_t j=ix;j<n;j++)if(glwasm_text_kind(at(t,j))==GLWASM_TEXT_LIST&&
          !atom(at(at(t,j),0),"elem")&&!atom(at(at(t,j),0),"export")&&
          !atom(at(at(t,j),0),"import")&&!atom(at(at(t,j),0),"ref")){init_expr=at(t,j);break;}
      if(init_expr){
        size_t type_ix=ix+1;uint64_t ignored_max;int hasmax=type_ix<n&&parse_u64(at(t,type_ix),&ignored_max);
        uint32_t flags=addr64?4u:0u;
        if(!parse_u64(at(t,ix),&min)){r=GLWASM_ERR_CORRUPT;goto done;}
        if(hasmax){if(!parse_u64(at(t,type_ix),&max)){r=GLWASM_ERR_CORRUPT;goto done;}type_ix++;flags|=1u;}
        table_element_type=at(t,type_ix);
        if(type_ix>=n||!put(&tables,0x40)||!put(&tables,0)||!table_reftype(&tables,table_element_type,typenames,ntyperefs)||!uleb(&tables,flags)||!uleb64(&tables,min)||(hasmax&&!uleb64(&tables,max))){r=GLWASM_ERR_UNSUPPORTED;goto done;}
        Bytes init={0};
        if(!instruction(&init,init_expr,e,fnames,named,NULL,0,globalrefs,nglobalrefs,typenames,ntyperefs,tagrefs,ntagrefs,tablerefs,ntablerefs,memoryrefs,nmemoryrefs,NULL,0,NULL,0,NULL,0,NULL,NULL)||!put(&init,0x0b)||!bytes(&tables,init.p,init.n)){free(init.p);r=e&&e->result?e->result:GLWASM_ERR_UNSUPPORTED;goto done;}
        free(init.p);table_index++;continue;
      }
      if(inline_elem){
        size_t elem_count=glwasm_text_size(inline_elem)-1;
        table_element_type=at(t,ix);
        const uint8_t*etype=glwasm_text_bytes(at(t,ix),&z);
        if(!elem_count){r=GLWASM_ERR_CORRUPT;goto done;}
        if(etype&&z==7&&!memcmp(etype,"funcref",7))et=0x70;
        else if(etype&&z==9&&!memcmp(etype,"externref",9))et=0x6f;
        else if(!typebyte(at(t,ix),&et)){if(e){e->message="unsupported inline table element type";e->location=glwasm_text_location(at(t,ix));}r=GLWASM_ERR_UNSUPPORTED;goto done;}
        min=elem_count;ix=n;
      } else if((n!=ix+2&&n!=ix+3)||!parse_u64(at(t,ix),&min)){
        if(e){e->result=GLWASM_ERR_CORRUPT;e->location=glwasm_text_location(t);e->message="malformed table declaration";}r=GLWASM_ERR_CORRUPT;goto done;
      }
      int hasmax=!inline_elem&&n==ix+3;
      if(hasmax&&!parse_u64(at(t,ix+1),&max)){r=GLWASM_ERR_CORRUPT;goto done;}
      if(!inline_elem){elem=glwasm_text_bytes(at(t,n-1),&z);
        table_element_type=at(t,n-1);
        if(elem&&z==7&&!memcmp(elem,"funcref",7))et=0x70;else if(elem&&z==9&&!memcmp(elem,"externref",9))et=0x6f;else if(!typebyte(at(t,n-1),&et)){if(e){e->message="unsupported table element type";e->location=glwasm_text_location(at(t,n-1));}r=GLWASM_ERR_UNSUPPORTED;goto done;}}
      if(!table_reftype(&tables,table_element_type,typenames,ntyperefs)||!uleb(&tables,(hasmax?1u:0u)|(addr64?4u:0u))||!uleb64(&tables,min)||(hasmax&&!uleb64(&tables,max))){r=GLWASM_ERR_OOM;goto done;}
      table_index++;
    }
    if(!section(&module,4,&tables)){r=GLWASM_ERR_OOM;goto done;}
  }
  phase="memory lowering";
  if(nmems){
    if(!uleb(&mems,(uint32_t)nmems)){r=GLWASM_ERR_OOM;goto done;}
    uint32_t memory_index=(uint32_t)nmemimports;
    for(size_t i=first_field;i<glwasm_text_size(root);i++){
      const GLWASM_Text*m=at(root,i);if(!atom(at(m,0),"memory")||inline_import(m))continue;
      size_t ix=1,mn=glwasm_text_size(m);int addr64=0;uint64_t min,max=0;
      if(ix<mn&&named_atom(at(m,ix)))ix++;
      for(;ix<mn&&atom(at(at(m,ix),0),"export");ix++)
        if(!append_export(&exports,&nexports,at(m,ix),2,memory_index)){
          if(e){e->result=GLWASM_ERR_CORRUPT;e->location=glwasm_text_location(at(m,ix));e->message="malformed inline memory export";}
          r=GLWASM_ERR_CORRUPT;goto done;
        }
      if(ix<mn&&named_atom(at(m,ix)))ix++;
      if(atom(at(m,ix),"i64")){addr64=1;ix++;}
      if(atom(at(at(m,ix),0),"data")){
        uint64_t data_size;
        if(!inline_data_size(at(m,ix),&data_size)){r=GLWASM_ERR_CORRUPT;goto done;}
        min=(data_size+65535u)/65536u;
        if(!uleb(&mems,addr64?4u:0u)||!uleb64(&mems,min)){r=GLWASM_ERR_OOM;goto done;}
        memory_index++;continue;
      }
      int hasmax=mn==ix+2;
      if((mn!=ix+1&&!hasmax)||!parse_u64(at(m,ix),&min)||(hasmax&&!parse_u64(at(m,ix+1),&max))){
        if(e){e->result=GLWASM_ERR_CORRUPT;e->location=glwasm_text_location(m);e->message="malformed memory declaration";}r=GLWASM_ERR_CORRUPT;goto done;
      }
      if(!uleb(&mems,(hasmax?1u:0u)|(addr64?4u:0u))||!uleb64(&mems,min)||(hasmax&&!uleb64(&mems,max))){r=GLWASM_ERR_OOM;goto done;}
      memory_index++;
    }
    if(!section(&module,5,&mems)){r=GLWASM_ERR_OOM;goto done;}
  }
  if(ntagdefs){
    if(!uleb(&tags,(uint32_t)ntagdefs)){r=GLWASM_ERR_OOM;goto done;}
    for(size_t i=0;i<ntagdefs+ntagimports;i++){
      const GLWASM_Text*t=taginfos[i].node;
      if(i<ntagimports)continue;
      size_t export_at=named_atom(at(t,1))?2:1;
      for(size_t j=export_at;j<glwasm_text_size(t)&&atom(at(at(t,j),0),"export");j++)
        if(!append_export(&exports,&nexports,at(t,j),4,taginfos[i].index)){r=GLWASM_ERR_CORRUPT;goto done;}
      if(!put(&tags,0)||!uleb(&tags,taginfos[i].type_index)){r=GLWASM_ERR_OOM;goto done;}
    }
    if(!section(&module,13,&tags)){r=GLWASM_ERR_OOM;goto done;}
  }
  phase="global lowering";
  if(nglobals){
    if(!uleb(&globals,(uint32_t)nglobals)){r=GLWASM_ERR_OOM;goto done;}
    uint32_t global_index=(uint32_t)nglobalimports;
    for(size_t i=first_field;i<glwasm_text_size(root);i++){
      const GLWASM_Text*g=at(root,i);if(!atom(at(g,0),"global")||inline_import(g))continue;
      size_t gi=1;
      if(gi<glwasm_text_size(g)&&named_atom(at(g,gi)))gi++;
      for(;gi<glwasm_text_size(g)&&atom(at(at(g,gi),0),"export");gi++)
        if(!append_export(&exports,&nexports,at(g,gi),3,global_index)){
          if(e){e->result=GLWASM_ERR_CORRUPT;e->location=glwasm_text_location(at(g,gi));e->message="malformed inline global export";}
          r=GLWASM_ERR_CORRUPT;goto done;
        }
      if(named_atom(at(g,gi)))gi++;
      const GLWASM_Text*decl=at(g,gi++);int mut=atom(at(decl,0),"mut");
      const GLWASM_Text*type=mut?at(decl,1):decl;
      Bytes expr={0};
      for(size_t init_ix=gi;init_ix<glwasm_text_size(g);init_ix++)
        if(!instruction(&expr,at(g,init_ix),e,fnames,named,NULL,0,globalrefs,nglobalrefs,typenames,ntyperefs,tagrefs,ntagrefs,tablerefs,ntablerefs,memoryrefs,nmemoryrefs,NULL,0,NULL,0,NULL,0,NULL,NULL)){
        free(expr.p);if(e&&e->result==GLWASM_OK){e->result=GLWASM_ERR_UNSUPPORTED;e->location=glwasm_text_location(g);e->message="unsupported global declaration or initializer";}
        r=e&&e->result?e->result:GLWASM_ERR_UNSUPPORTED;goto done;
      }
      if(!value_type(&globals,type,typenames,ntyperefs)){
        free(expr.p);malformed_error(e,type,"unknown or invalid global value type");r=GLWASM_ERR_CORRUPT;goto done;
      }
      if(!put(&expr,0x0b)||!put(&globals,mut?1:0)||!bytes(&globals,expr.p,expr.n)){free(expr.p);r=GLWASM_ERR_OOM;goto done;}
      free(expr.p);
      global_index++;
    }
    if(!section(&module,6,&globals)){r=GLWASM_ERR_OOM;goto done;}
  }
  phase="element lowering";
  if(nelems){
    if(!uleb(&elems,(uint32_t)nelems)){r=GLWASM_ERR_OOM;goto done;}
    uint32_t inline_table_index=(uint32_t)ntableimports;
    for(size_t i=first_field;i<glwasm_text_size(root);i++){
      const GLWASM_Text*x=at(root,i);if(!atom(at(x,0),"elem"))continue;
      Bytes offset={0},indices={0},elemtype_bytes={0};uint32_t nfunc=0,table=0;size_t first=1;uint8_t flag=1;int active=0,explicit_table=0,exprs=0;uint8_t refbyte=0x70;
      if(first<glwasm_text_size(x)&&named_atom(at(x,first)))first++;
      if(first<glwasm_text_size(x)&&atom(at(x,first),"declare")){flag=3;first++;}
      else if(first<glwasm_text_size(x)&&atom(at(at(x,first),0),"table")){
        if(glwasm_text_size(at(x,first))!=2||!resolve_index(at(at(x,first),1),tablerefs,ntablerefs,&table)){r=GLWASM_ERR_UNSUPPORTED;goto elem_bad;}
        explicit_table=1;active=1;flag=2;first++;
      } else if(first<glwasm_text_size(x)&&glwasm_text_kind(at(x,first))==GLWASM_TEXT_LIST){active=1;flag=0;}
      if(active){
        if(first>=glwasm_text_size(x))goto elem_bad;
        const GLWASM_Text*offset_expr=at(x,first++);
        if(atom(at(offset_expr,0),"offset")){
          for(size_t oi=1;oi<glwasm_text_size(offset_expr);oi++)if(!instruction(&offset,at(offset_expr,oi),e,fnames,named,NULL,0,globalrefs,nglobalrefs,typenames,ntyperefs,tagrefs,ntagrefs,tablerefs,ntablerefs,memoryrefs,nmemoryrefs,NULL,0,NULL,0,NULL,0,NULL,NULL))goto elem_bad;
        } else if(!instruction(&offset,offset_expr,e,fnames,named,NULL,0,globalrefs,nglobalrefs,typenames,ntyperefs,tagrefs,ntagrefs,tablerefs,ntablerefs,memoryrefs,nmemoryrefs,NULL,0,NULL,0,NULL,0,NULL,NULL))goto elem_bad;
        if(!put(&offset,0x0b))goto elem_bad;
      }
      if(first<glwasm_text_size(x)&&(typebyte(at(x,first),&refbyte))){exprs=1;if(!table_reftype(&elemtype_bytes,at(x,first),typenames,ntyperefs))goto elem_bad;first++;if(!explicit_table)flag=active?(refbyte==0x70?4:6):(flag==3?7:5);else flag=6;}
      if(first<glwasm_text_size(x)&&atom(at(x,first),"func"))first++;
      for(size_t j=first;j<glwasm_text_size(x);j++){
        uint32_t ix;
        const GLWASM_Text*entry=at(x,j);
        if(exprs){if(!instruction(&indices,entry,e,fnames,named,NULL,0,globalrefs,nglobalrefs,typenames,ntyperefs,tagrefs,ntagrefs,tablerefs,ntablerefs,memoryrefs,nmemoryrefs,NULL,0,NULL,0,NULL,0,NULL,NULL)||!put(&indices,0x0b))goto elem_bad;}
        else {if(atom(entry,"func"))continue;if(!resolve_index(entry,fnames,named,&ix)||!uleb(&indices,ix)){r=GLWASM_ERR_CORRUPT;goto elem_bad;}}
        nfunc++;
      }
      if(!put(&elems,flag)||((explicit_table||flag==6)&&!uleb(&elems,table))||(active&&!bytes(&elems,offset.p,offset.n))||(!exprs&&(flag==1||flag==2||flag==3)&&!put(&elems,0))||(exprs&&flag!=4&&!bytes(&elems,elemtype_bytes.p,elemtype_bytes.n))||!uleb(&elems,nfunc)||!bytes(&elems,indices.p,indices.n)){
        r=GLWASM_ERR_OOM;goto elem_bad;
      }
      free(offset.p);free(indices.p);free(elemtype_bytes.p);
      continue;
elem_bad:
      free(offset.p);free(indices.p);free(elemtype_bytes.p);
      if(e&&e->result==GLWASM_OK){e->result=GLWASM_ERR_UNSUPPORTED;e->location=glwasm_text_location(x);e->message="unsupported or malformed element segment";}
      if(!r||r==GLWASM_OK)r=e&&e->result?e->result:GLWASM_ERR_UNSUPPORTED;
      goto done;
    }
    for(size_t i=first_field;i<glwasm_text_size(root);i++){
      const GLWASM_Text*t=at(root,i);if(!atom(at(t,0),"table")||inline_import(t))continue;
      uint32_t table_index=inline_table_index++;
      const GLWASM_Text*inl=NULL;
      for(size_t j=1;j<glwasm_text_size(t);j++)if(atom(at(at(t,j),0),"elem")){inl=at(t,j);break;}
      if(!inl)continue;
      if(named_atom(at(t,1))&&!resolve_index(at(t,1),tablerefs,ntablerefs,&table_index)){r=GLWASM_ERR_CORRUPT;goto done;}
      Bytes offset={0},indices={0};uint32_t count=0;int exprs=glwasm_text_size(inl)>1&&glwasm_text_kind(at(inl,1))==GLWASM_TEXT_LIST;
      int table64=0;uint8_t elemtype=0x70;for(size_t j=1;j<glwasm_text_size(t);j++){if(atom(at(t,j),"i64"))table64=1;if(atom(at(t,j),"externref"))elemtype=0x6f;}
      if(!put(&offset,table64?0x42:0x41)||!sleb(&offset,0)||!put(&offset,0x0b)){free(offset.p);r=GLWASM_ERR_OOM;goto done;}
      for(size_t j=1;j<glwasm_text_size(inl);j++){
        const GLWASM_Text*entry=at(inl,j);
        if(exprs){if(!instruction(&indices,entry,e,fnames,named,NULL,0,globalrefs,nglobalrefs,typenames,ntyperefs,tagrefs,ntagrefs,tablerefs,ntablerefs,memoryrefs,nmemoryrefs,NULL,0,NULL,0,NULL,0,NULL,NULL)||!put(&indices,0x0b)){free(offset.p);free(indices.p);r=e&&e->result?e->result:GLWASM_ERR_UNSUPPORTED;goto done;}}
        else {uint32_t ix;if(!resolve_index(entry,fnames,named,&ix)||!uleb(&indices,ix)){free(offset.p);free(indices.p);r=GLWASM_ERR_CORRUPT;goto done;}}
        count++;
      }
      uint8_t flag=exprs?(elemtype==0x70&&table_index==0?4:6):(table_index==0?0:2);
      if(!put(&elems,flag)||((flag==2||flag==6)&&!uleb(&elems,table_index))||!bytes(&elems,offset.p,offset.n)||(exprs&&flag!=4&&!put(&elems,elemtype))||(!exprs&&flag!=0&&!put(&elems,0))||!uleb(&elems,count)||!bytes(&elems,indices.p,indices.n)){
        free(offset.p);free(indices.p);r=GLWASM_ERR_OOM;goto done;
      }
      free(offset.p);free(indices.p);
    }
  }
  phase="data lowering";
  if(ndatas){
    if(!uleb(&datas,(uint32_t)ndatas)){r=GLWASM_ERR_OOM;goto done;}
    uint32_t inline_memory_index=(uint32_t)nmemimports;
    for(size_t i=first_field;i<glwasm_text_size(root);i++){
      const GLWASM_Text*x=at(root,i);
      if(atom(at(x,0),"memory")&&!inline_import(x)){
        int addr64=0;
        for(size_t j=1;j<glwasm_text_size(x);j++)if(atom(at(x,j),"i64"))addr64=1;
        for(size_t j=1;j<glwasm_text_size(x);j++)if(atom(at(at(x,j),0),"data")&&
            !emit_inline_data_segment(&datas,at(x,j),inline_memory_index,addr64)){r=GLWASM_ERR_OOM;goto done;}
        inline_memory_index++;continue;
      }
      if(!atom(at(x,0),"data"))continue;
      int passive=glwasm_text_size(x)==1||glwasm_text_kind(at(x,1))!=GLWASM_TEXT_LIST,explicit_memory=0;uint32_t memory=0;size_t offindex=1,strindex=2;uint8_t flag=passive?1:0;Bytes offset={0},payload={0};
      if(passive)strindex=1;
      if(!passive&&atom(at(at(x,1),0),"memory")){if(glwasm_text_size(at(x,1))!=2||!resolve_index(at(at(x,1),1),memoryrefs,nmemoryrefs,&memory)){r=GLWASM_ERR_UNSUPPORTED;goto data_bad;}explicit_memory=1;flag=2;offindex=2;strindex=3;}
      const GLWASM_Text*offset_expr=at(x,offindex);
      if(!passive&&atom(at(offset_expr,0),"offset")){
        for(size_t oi=1;oi<glwasm_text_size(offset_expr);oi++){
          if(!instruction(&offset,at(offset_expr,oi),e,fnames,named,NULL,0,globalrefs,nglobalrefs,typenames,ntyperefs,tagrefs,ntagrefs,tablerefs,ntablerefs,memoryrefs,nmemoryrefs,NULL,0,NULL,0,NULL,0,NULL,NULL))goto data_bad;
        }
      } else if(!passive) {
        if(!instruction(&offset,offset_expr,e,fnames,named,NULL,0,globalrefs,nglobalrefs,typenames,ntyperefs,tagrefs,ntagrefs,tablerefs,ntablerefs,memoryrefs,nmemoryrefs,NULL,0,NULL,0,NULL,0,NULL,NULL))goto data_bad;
      }
      if(!passive){
        if(!put(&offset,0x0b))goto data_bad;
      }
      for(size_t si=strindex;si<glwasm_text_size(x);si++){
        size_t chunk;const uint8_t*part=glwasm_text_bytes(at(x,si),&chunk);
        if((!part&&chunk)||chunk>UINT32_MAX-payload.n||!bytes(&payload,part,chunk))goto data_bad;
      }
      if(payload.n>UINT32_MAX){
data_bad:
        free(offset.p);free(payload.p);if(e&&e->result==GLWASM_OK){e->result=GLWASM_ERR_UNSUPPORTED;e->location=glwasm_text_location(x);e->message="unsupported active data segment";}
        r=e&&e->result?e->result:GLWASM_ERR_UNSUPPORTED;goto done;
      }
      if(!put(&datas,flag)||(explicit_memory&&!uleb(&datas,memory))||(!passive&&!bytes(&datas,offset.p,offset.n))||!uleb(&datas,(uint32_t)payload.n)||!bytes(&datas,payload.p,payload.n)){
        free(offset.p);free(payload.p);r=GLWASM_ERR_OOM;goto done;
      }
      free(offset.p);free(payload.p);
    }
  }
  for(size_t i=first_field;i<glwasm_text_size(root);i++) {
    const GLWASM_Text*x=at(root,i);
    if(!atom(at(x,0),"export"))continue;
    size_t len;const uint8_t*name=glwasm_text_bytes(at(x,1),&len);
    const GLWASM_Text*desc=at(x,2);uint32_t ix;uint8_t kind;
    if(glwasm_text_size(x)!=3||(!name&&len)||glwasm_text_size(desc)!=2||len>UINT32_MAX){
      if(e){e->result=GLWASM_ERR_CORRUPT;e->location=glwasm_text_location(x);e->message="malformed function export";}
      r=GLWASM_ERR_CORRUPT;goto done;
    }
    if(atom(at(desc,0),"func")){kind=0;if(!resolve_index(at(desc,1),fnames,named,&ix))goto export_bad;}
    else if(atom(at(desc,0),"table")){kind=1;if(!resolve_index(at(desc,1),tablerefs,ntablerefs,&ix))goto export_bad;}
    else if(atom(at(desc,0),"memory")){kind=2;if(!resolve_index(at(desc,1),memoryrefs,nmemoryrefs,&ix))goto export_bad;}
    else if(atom(at(desc,0),"global")){kind=3;if(!resolve_index(at(desc,1),globalrefs,nglobalrefs,&ix))goto export_bad;}
    else if(atom(at(desc,0),"tag")){kind=4;if(!resolve_index(at(desc,1),tagrefs,ntagrefs,&ix))goto export_bad;}
    else goto export_bad;
    if(!uleb(&exports,(uint32_t)len)||!bytes(&exports,name,len)||!put(&exports,kind)||!uleb(&exports,ix)){r=GLWASM_ERR_OOM;goto done;}
    nexports++;
    continue;
export_bad:
    if(e){e->result=GLWASM_ERR_CORRUPT;e->location=glwasm_text_location(x);e->message="malformed export descriptor";}
    r=GLWASM_ERR_CORRUPT;goto done;
  }
  phase="export lowering";
  if(nexports){Bytes expsec={0};if(!uleb(&expsec,nexports)||!bytes(&expsec,exports.p,exports.n)||!section(&module,7,&expsec)){free(expsec.p);r=GLWASM_ERR_OOM;goto done;}free(expsec.p);}
  if(start_form){Bytes start={0};uint32_t ix;if(!resolve_index(at(start_form,1),fnames,named,&ix)){if(e){e->result=GLWASM_ERR_CORRUPT;e->location=glwasm_text_location(start_form);e->message="unresolved start function index";}free(start.p);r=GLWASM_ERR_CORRUPT;goto done;}if(!uleb(&start,ix)||!section(&module,8,&start)){free(start.p);r=GLWASM_ERR_OOM;goto done;}free(start.p);}
  if(nelems&&!section(&module,9,&elems)){r=GLWASM_ERR_OOM;goto done;}
  if(tree_has_atom(mod,"memory.init")||tree_has_atom(mod,"data.drop")){Bytes dc={0};if(!uleb(&dc,(uint32_t)ndatas)||!section(&module,12,&dc)){free(dc.p);r=GLWASM_ERR_OOM;goto done;}free(dc.p);}
  if(nf&&!section(&module,10,&code)){r=GLWASM_ERR_OOM;goto done;}
  if(ndatas&&!section(&module,11,&datas)){r=GLWASM_ERR_OOM;goto done;}
  phase="binary decode/validation";
  r=glwasm_module_decode(module.p,module.n,limits,a,NULL,out);
  if(r!=GLWASM_OK&&e){e->result=r;e->location=glwasm_text_location(mod);e->message="lowered WAT module failed binary validation";}
  if(r==GLWASM_OK&&out_binary){
    uint8_t*copy=malloc(module.n?module.n:1);
    if(!copy){glwasm_module_destroy(*out);*out=NULL;r=GLWASM_ERR_OOM;}
    else{if(module.n)memcpy(copy,module.p,module.n);*out_binary=copy;*out_binary_length=module.n;}
  }
done:
  if(e&&r!=GLWASM_OK&&!e->message){e->result=r;e->location=(GLWASM_SourceLoc){0,1,1};e->message=phase;}
  free(cleaned);free(module.p);free(types.p);free(imports.p);free(funcs.p);free(tables.p);free(mems.p);free(globals.p);free(elems.p);free(code.p);free(datas.p);free(exports.p);free(tags.p);free(fnames);free(typenames);free(localrefs);free(globalrefs);free(tagrefs);free(tablerefs);free(memoryrefs);free(blockrefs);free(elemrefs);free(datarefs);free(taginfos);glwasm_text_destroy(tree);return r;
}
GLWASM_Result glwasm_wat_module_parse(const uint8_t*s,size_t n,
    const GLWASM_Limits*l,const GLWASM_Allocator*a,GLWASM_ParseError*e,
    GLWASM_Module**out){return wat_module_parse_impl(s,n,l,a,e,out,1,NULL,NULL);}
GLWASM_Result glwasm_wat_module_parse_with_policy(const uint8_t*s,size_t n,
    const GLWASM_Policy*policy,const GLWASM_Allocator*a,GLWASM_ParseError*e,
    GLWASM_Module**out){
  if(!policy||!out)return GLWASM_ERR_INVALID;
  *out=NULL;
  uint8_t*binary=NULL;size_t binary_length=0;
  GLWASM_Result result=glwasm_wat_module_lower_binary(s,n,&policy->limits,a,e,
      &binary,&binary_length);
  if(result==GLWASM_OK){
    GLWASM_Failure failure={0};
    result=glwasm_module_decode_with_policy(binary,binary_length,policy,a,
        &failure,out);
    if(result!=GLWASM_OK&&e){e->result=result;
      e->location=(GLWASM_SourceLoc){0,1,1};
      e->message="lowered WAT module failed policy validation";}
  }
  free(binary);
  return result;
}
GLWASM_Result glwasm_wat_module_parse_verbatim(const uint8_t*s,size_t n,
    const GLWASM_Limits*l,const GLWASM_Allocator*a,GLWASM_ParseError*e,
    GLWASM_Module**out){return wat_module_parse_impl(s,n,l,a,e,out,0,NULL,NULL);}
GLWASM_Result glwasm_wat_module_lower_binary(const uint8_t*s,size_t n,
    const GLWASM_Limits*l,const GLWASM_Allocator*a,GLWASM_ParseError*e,
    uint8_t**out_binary,size_t*out_binary_length){
  GLWASM_Module*module=NULL;
  if(!out_binary||!out_binary_length)return GLWASM_ERR_INVALID;
  GLWASM_Result r=wat_module_parse_impl(s,n,l,a,e,&module,1,out_binary,
      out_binary_length);
  glwasm_module_destroy(module);
  return r;
}
