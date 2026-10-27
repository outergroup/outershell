"""Compile the Windows storage command logic with fake runtimes on macOS/Linux."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root/'Windows/ContainerStorage.inc').read_text()
start = source.index('static bool storage_saved_definitions(')
end = source.index('static bool storage_inventory(', start)
source = source[:start] + '''static bool storage_saved_definitions(StringBuilder *out, const char *excluding) {
    (void)excluding; append(out, definitions); return definitions_ok;
}
''' + source[end:]
wsl = (root/'Windows/Wsl.c').read_text()
request = wsl[wsl.index('static char *request_string(const HttpRequest *request, const char *name) {'):wsl.index('static void send_snapshot(')]
containers = (root/'Windows/WslContainers.inc').read_text()
container_string = containers[:containers.index('static const char *container_provider_id(')]
recipes = (root/'Windows/ContainerRecipes.inc').read_text()
uuid = recipes[:recipes.index('static char *container_directory(')]
arguments = recipes[recipes.index('static char *container_json_field('):recipes.index('static bool container_configuration_array(')]
preamble = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>
#include "Backend/OuterShellBuffer.h"
#include "Backend/OuterShellLayout.h"
#define CP_UTF8 65001
#define WC_ERR_INVALID_CHARS 0
#define _stricmp strcasecmp
static int WideCharToMultiByte(unsigned page, unsigned flags, const wchar_t *text, int count, char *out, int capacity, void *a, void *b) {
    (void)page;(void)flags;(void)a;(void)b;
    if (count != 1 || capacity < 1 || text[0] > 127) return 0;
    out[0]=(char)text[0]; return 1;
}
typedef struct {StringBuilder body;} HttpRequest;
typedef enum {RUNTIME_WSL,RUNTIME_WSLC,RUNTIME_DOCKER,RUNTIME_TAR} ContainerRuntime;
typedef struct {int dummy;} Wsl;
typedef struct {Wsl *owner; ContainerRuntime container; char *id;} Distribution;
static void *win_alloc(size_t n) {return calloc(1,n);}
static char *win_strdup(const char *s) {return strdup(s);}
static void append(StringBuilder *b,const char *s) {assert(sb_append(b,s));}
static void bytes(StringBuilder *b,const void *s,size_t n) {assert(sb_append_n(b,s,n));}
static void json_string(StringBuilder *b,const char *s) {
    append(b,"\""); for(;*s;s++) {if(*s=='"'||*s=='\\') append(b,"\\"); bytes(b,s,1);} append(b,"\"");
}
static void field(StringBuilder *b,const char *key,const char *value) {json_string(b,key);append(b,":");json_string(b,value);}
static const char *definitions="", *listing="", *container_list="[]", *inspection="[]";
static bool definitions_ok=true, removal_ok=true;
static int removed=0, caches=0, inventory_reads=0, change_on_read=0;
static bool capture_runtime(Wsl *w,const char *const *args,StringBuilder *out,ContainerRuntime runtime) {
    (void)w;
    if(!strcmp(args[0],"image")&&!strcmp(args[1],"list")) {
        inventory_reads++;
        append(out,change_on_read && inventory_reads>=change_on_read ? "[]" : listing); return true;
    }
    if(!strcmp(args[0],"ps")||!strcmp(args[0],"list")) {append(out,container_list);return true;}
    if(!strcmp(args[0],"inspect")) {append(out,inspection);return true;}
    if(!strcmp(args[0],"image")&&(!strcmp(args[1],"rm")||!strcmp(args[1],"delete"))) {
        assert(args[3]==NULL); assert(!strcmp(args[1],runtime==RUNTIME_DOCKER?"rm":"delete"));
        if(removal_ok) removed++; return removal_ok;
    }
    if(!strcmp(args[0],"builder")) {
        assert(runtime==RUNTIME_DOCKER);assert(!strcmp(args[3],"--filter"));assert(!strcmp(args[4],"until=168h"));
        assert(!strcmp(args[5],"--keep-storage"));assert(!strcmp(args[6],"10GB"));caches++;return true;
    }
    assert(!"Unexpected runtime command");return false;
}
'''
main = r'''
#define ID "11111111-1111-1111-1111-111111111111"
#define REF "outershell/managed-" ID ":latest"
#define IMAGE "{\"Repository\":\"outershell/managed-" ID "\",\"Tag\":\"latest\",\"ID\":\"sha256:one\"}"
static HttpRequest req(const char *body) {HttpRequest r={0};r.body.data=(char*)body;r.body.length=strlen(body);return r;}
int main(void) {
    Wsl w={0};StorageImage *images=NULL;
    listing="[" IMAGE "]";
    assert(storage_inventory(&w,RUNTIME_WSLC,NULL,&images));assert(storage_candidate(images,REF));storage_images_free(images);images=NULL;
    listing=IMAGE "\n";
    assert(storage_inventory(&w,RUNTIME_DOCKER,NULL,&images));assert(storage_candidate(images,REF));storage_images_free(images);images=NULL;
    definitions="FROM alias@sha256:manifest";
    assert(storage_inventory(&w,RUNTIME_DOCKER,NULL,&images));assert(!storage_candidate(images,REF));storage_images_free(images);images=NULL;
    definitions="FROM " REF;
    assert(storage_inventory(&w,RUNTIME_DOCKER,NULL,&images));assert(!storage_candidate(images,REF));storage_images_free(images);images=NULL;
    definitions="";container_list="[{\"ID\":\"container\"}]";inspection="[{\"Image\":\"sha256:one\"}]";
    assert(storage_inventory(&w,RUNTIME_DOCKER,NULL,&images));assert(!storage_candidate(images,REF));storage_images_free(images);images=NULL;
    container_list="[]";
    listing=IMAGE "\n{\"Repository\":\"alias\",\"Tag\":\"latest\",\"ID\":\"sha256:one\"}";
    definitions="FROM alias:latest";
    assert(storage_inventory(&w,RUNTIME_DOCKER,NULL,&images));assert(!storage_candidate(images,REF));storage_images_free(images);images=NULL;
    listing="[{\"Repository\":\"broken\"}]";definitions="";
    assert(!storage_inventory(&w,RUNTIME_DOCKER,NULL,&images));assert(!images);
    listing=IMAGE; definitions_ok=false;assert(!storage_inventory(&w,RUNTIME_DOCKER,NULL,&images));definitions_ok=true;
    HttpRequest r=req("{\"runtimeProviderID\":\"docker\",\"images\":[\"ubuntu:latest\"]}");StringBuilder extra={0};
    assert(!storage_request(&w,&r,"cleanContainerStorage",&extra));assert(!removed);free(extra.data);extra=(StringBuilder){0};
    r=req("{\"runtimeProviderID\":\"docker\",\"images\":[\"" REF "\"]}");
    inventory_reads=0;change_on_read=2;
    assert(storage_request(&w,&r,"cleanContainerStorage",&extra));assert(!removed);assert(strstr(extra.data,"now in use"));free(extra.data);extra=(StringBuilder){0};change_on_read=0;
    removal_ok=false;assert(storage_request(&w,&r,"cleanContainerStorage",&extra));assert(!removed);assert(strstr(extra.data,"deletion failed"));free(extra.data);extra=(StringBuilder){0};
    removal_ok=true;assert(storage_request(&w,&r,"cleanContainerStorage",&extra));assert(removed==1);free(extra.data);extra=(StringBuilder){0};
    assert(storage_request(&w,&r,"cleanContainerBuildCache",&extra));assert(caches==1);free(extra.data);extra=(StringBuilder){0};
    r=req("{\"runtimeProviderID\":\"wsl.container\",\"images\":[\"" REF "\"]}");
    assert(!storage_request(&w,&r,"cleanContainerBuildCache",&extra));assert(caches==1);free(extra.data);extra=(StringBuilder){0};
    assert(storage_request(&w,&r,"cleanContainerStorage",&extra));assert(removed==2);free(extra.data);
    assert(storage_owned_reference("outershell/container-base:18"));assert(storage_owned_reference("outershell/container-base:20"));assert(!storage_owned_reference("outershell/container-base:21"));assert(!storage_owned_reference("outershell/container-base:22"));
    assert(storage_owned_reference("outershell/container-base:17"));assert(!storage_owned_reference("outershell/managed-invalid:latest"));
    Distribution d={&w,RUNTIME_DOCKER,ID};removal_ok=false;assert(!storage_remove_recipe(&d));
    puts("PASS: Windows Docker/WSLC inventories, namespaces, saved references, aliases, malformed inventories, revalidation, failure reporting, non-forced deletion, and bounded Docker cache cleanup");
}
'''
with tempfile.TemporaryDirectory(prefix='outer-storage-c-') as temporary:
    directory=Path(temporary)
    path=directory/'test.c'
    path.write_text(preamble+request+container_string+uuid+arguments+source+main)
    subprocess.run(['cc','-std=c17','-D_DARWIN_C_SOURCE','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror','-Wno-unused-function','-I',str(root),str(path),str(root/'Backend/OuterShellBuffer.c'),'-o',str(directory/'test')],check=True)
    subprocess.run([str(directory/'test')],check=True)
