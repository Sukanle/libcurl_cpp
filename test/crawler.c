int max_con = 200;
int max_total = 20000;
int max_requests = 500;
int max_link_per_page = 5;
int follow_relative_links = 0;

// char *start_page = "https://www.bilibili.com";
char *start_page[] = {
  "https://www.baidu.com",
  "https://www.bilibili.com",
  "https://cn.bing.com",
  // "https://www.taobao.com",
  // "https://www.jd.com",
};

#include <libxml/HTMLparser.h>
#include <libxml/xpath.h>
#include <libxml/uri.h>
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>

FILE *logfile;
FILE *webfile;

int pending_interrupt = 0;
void closeIOandsignal(bool bcloseio)
{
  int ii = 0;

  for (; ii < 64; ii++) {
    if (bcloseio == true)
      close(ii);

    signal(ii, SIG_IGN);
  }
}
void sighandler(int dummy)
{
  pending_interrupt = 1;
}

/* resizable buffer */
typedef struct {
  char *buf;
  size_t size;
} memory;

size_t grow_buffer(void *contents, size_t sz, size_t nmemb, void *ctx)
{
  size_t realsize = sz * nmemb;
  memory *mem = (memory*) ctx;
  char *ptr = (char *)realloc(mem->buf, mem->size + realsize);
  if(!ptr) {
    /* out of memory */
    fprintf(logfile,"not enough memory (realloc returned NULL)\n");
    return 0;
  }
  mem->buf = ptr;
  memcpy(&(mem->buf[mem->size]), contents, realsize);
  mem->size += realsize;
  return realsize;
}

CURL *make_handle(char *url)
{
  CURL *handle = curl_easy_init();

  /* Important: use HTTP2 over HTTPS */
  curl_easy_setopt(handle, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
  curl_easy_setopt(handle, CURLOPT_URL, url);

  /* buffer body */
  memory *mem =(memory *)malloc(sizeof(memory));
  mem->size = 0;
  mem->buf = (char *)malloc(1);
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, grow_buffer);
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, mem);
  curl_easy_setopt(handle, CURLOPT_PRIVATE, mem);

  /* For completeness */
  curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(handle, CURLOPT_TIMEOUT, 5L);
  curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
  /* only allow redirects to HTTP and HTTPS URLs */
  curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS, "http,https");
  curl_easy_setopt(handle, CURLOPT_AUTOREFERER, 1L);
  curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 10L);
  /* each transfer needs to be done within 20 seconds! */
  curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, 20000L);
  /* connect fast or fail */
  curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
  /* skip files larger than a gigabyte */
  curl_easy_setopt(handle, CURLOPT_MAXFILESIZE_LARGE,
                   (curl_off_t)1024*1024*1024);
  curl_easy_setopt(handle, CURLOPT_COOKIEFILE, "");
  curl_easy_setopt(handle, CURLOPT_FILETIME, 1L);
  curl_easy_setopt(handle, CURLOPT_USERAGENT, "mini crawler");
  curl_easy_setopt(handle, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
  curl_easy_setopt(handle, CURLOPT_UNRESTRICTED_AUTH, 1L);
  curl_easy_setopt(handle, CURLOPT_PROXYAUTH, CURLAUTH_ANY);
  curl_easy_setopt(handle, CURLOPT_EXPECT_100_TIMEOUT_MS, 0L);
  return handle;
}

/* HREF finder implemented in libxml2 but could be any HTML parser */
size_t follow_links(CURLM *multi_handle, memory *mem, char *url)
{
  int opts = HTML_PARSE_NOBLANKS | HTML_PARSE_NOERROR | \
             HTML_PARSE_NOWARNING | HTML_PARSE_NONET;
  htmlDocPtr doc = htmlReadMemory(mem->buf, mem->size, url, NULL, opts);
  if(!doc)
    return 0;
  xmlChar *xpath = (xmlChar*) "//a/@href";
  xmlXPathContextPtr context = xmlXPathNewContext(doc);
  xmlXPathObjectPtr result = xmlXPathEvalExpression(xpath, context);
  xmlXPathFreeContext(context);
  if(!result)
    return 0;
  xmlNodeSetPtr nodeset = result->nodesetval;
  if(xmlXPathNodeSetIsEmpty(nodeset)) {
    xmlXPathFreeObject(result);
    return 0;
  }
  size_t count = 0;
  int i;
  for(i = 0; i < nodeset->nodeNr; i++) {
    double r = rand();
    int x = r * nodeset->nodeNr / RAND_MAX;
    const xmlNode *node = nodeset->nodeTab[x]->xmlChildrenNode;
    xmlChar *href = xmlNodeListGetString(doc, node, 1);
    if(follow_relative_links) {
      xmlChar *orig = href;
      href = xmlBuildURI(href, (xmlChar *) url);
      xmlFree(orig);
    }
    char *link = (char *) href;
    if(!link || strlen(link) < 20)
      continue;
    if(!strncmp(link, "http://", 7) || !strncmp(link, "https://", 8)) {
      curl_multi_add_handle(multi_handle, make_handle(link));
      if(count++ == max_link_per_page)
        break;
    }
    xmlFree(link);
  }
  xmlXPathFreeObject(result);
  return count;
}

int is_html(char *ctype)
{
  return ctype != NULL && strlen(ctype) > 10 && strstr(ctype, "text/html");
}

int main(void)
{
  /* closeIOandsignal(true); */

  /* if (fork()!=0) exit(0); */

  logfile = fopen("log.txt", "w+");
  webfile = fopen("web.html", "w+");
  /* if(daemon(1,1)<0){ */
  /*   fprintf(logfile,"error daemon...\n"); */
  /*   exit(-1); */
  /* } */
  signal(SIGINT, sighandler);
  LIBXML_TEST_VERSION;
  curl_global_init(CURL_GLOBAL_DEFAULT);
  CURLM *multi_handle = curl_multi_init();
  curl_multi_setopt(multi_handle, CURLMOPT_MAX_TOTAL_CONNECTIONS, max_con);
  curl_multi_setopt(multi_handle, CURLMOPT_MAX_HOST_CONNECTIONS, 6L);

  /* enables http/2 if available */
#ifdef CURLPIPE_MULTIPLEX
  curl_multi_setopt(multi_handle, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
#endif

  struct timeval start, end;
  gettimeofday(&start, NULL);
  /* sets html start page */
  for(int i = 0; i < sizeof(start_page)/sizeof(start_page[0]); i++)
    curl_multi_add_handle(multi_handle, make_handle(start_page[i]));

  int msgs_left;
  int pending = 0;
  int complete = 0;
  int still_running = 1;
  while(still_running && !pending_interrupt) {
    int numfds;
    curl_multi_wait(multi_handle, NULL, 0, 1000, &numfds);
    /* printf("numfds: %d\n", numfds); */
    curl_multi_perform(multi_handle, &still_running);

    /* See how the transfers went */
    CURLMsg *m = NULL;
    while((m = curl_multi_info_read(multi_handle, &msgs_left))) {
      if(m->msg == CURLMSG_DONE) {
        CURL *handle = m->easy_handle;
        char *url;
        memory *mem;
        curl_easy_getinfo(handle, CURLINFO_PRIVATE, &mem);
        curl_easy_getinfo(handle, CURLINFO_EFFECTIVE_URL, &url);
        if(m->data.result == CURLE_OK) {
          long res_status;
          curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &res_status);
          if(res_status == 200) {
            char *ctype;
            curl_easy_getinfo(handle, CURLINFO_CONTENT_TYPE, &ctype);
            fprintf(webfile,"[%d] HTTP 200 (%s): %s\n", complete, ctype, url);
            // printf("[%d] HTTP 200 (%s): %s\n", complete, ctype, url);
            if(is_html(ctype) && mem->size > 100) {
              if(pending < max_requests && (complete + pending) < max_total) {
                pending += follow_links(multi_handle, mem, url);
                still_running = 1;
              }
            }
          }
          else {
            fprintf(webfile,"[%d] HTTP %d: %s\n", complete, (int) res_status, url);
            // printf("[%d] HTTP %d: %s\n", complete, (int) res_status, url);
          }
        }
        else {
          fprintf(logfile,"[%d] Connection failure: %s\n", complete, url);
          // printf("[%d] Connection failure: %s\n", complete, url);
        }
        curl_multi_remove_handle(multi_handle, handle);
        curl_easy_cleanup(handle);
        free(mem->buf);
        free(mem);
        complete++;
        pending--;
      }
    }
  }
  gettimeofday(&end, NULL);
  long long elapsed = (end.tv_sec - start.tv_sec) * 1000000 + end.tv_usec - start.tv_usec;
  printf("Elapsed time: %lld us\n", elapsed);
  curl_multi_cleanup(multi_handle);
  curl_global_cleanup();
  fclose(logfile);
  fclose(webfile);
  return 0;
}
