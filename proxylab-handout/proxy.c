/*最终代码：多线程+pthread读写锁*/
#include <stdio.h>
#include "csapp.h"
#include "cache.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
cache_t *mycache;

void doit(int connfd);
void parse_url(char *url, char *hostname, char *path, int *portp);
void request_header(char *header_to_server,char *hostname, 
            char *path, rio_t * rio_client);
int connect_server(char *hostname, int port, char *header_to_server);
void *thread(void *vargp);

//ubuntu: 52184 代理监听端口
int main(int argc, char **argv)
{
    int listenfd, *connfdp;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;/*存取客户端的地址信息*/
    char hostname[MAXLINE], port[MAXLINE];

    if(argc != 2){
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(0);
    }

    if((listenfd = Open_listenfd(argv[1])) < 0)
        exit(0);
    
    mycache = cache_init();//注意：cache_init如果设计传入的是mycache，并不能修改mycache的值

    // printf("main: mycache=%p\n", mycache);
    while(1){
        pthread_t tid;
        clientlen = sizeof(clientaddr);
        connfdp = (int *)Malloc(sizeof(int));
        *connfdp = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        if(*connfdp < 0) continue;
        Getnameinfo((SA *)&clientaddr, clientlen, 
            hostname, MAXLINE, port, MAXLINE, NI_NUMERICHOST|NI_NUMERICSERV);
        printf("Accept connection from (%s, %s)\n", hostname, port);
        Pthread_create(&tid , NULL, thread, (void *)connfdp);
    }
}

void *thread(void *vargp){
    Pthread_detach(Pthread_self());
    int connfd = *(int *)vargp;
    Free(vargp);//记得释放内存块
    printf("thread: mycache=%p\n", mycache);
    doit(connfd);
    Close(connfd);//必须在对等进程中关闭connfd，因为在主进程中关闭会引起竞争
    return NULL;
}

void doit(int connfd){
    char buf[MAXLINE], url[MAXLINE], method[MAXLINE], version[MAXLINE];
    char hostname[MAXLINE], path[MAXLINE];
    char header_to_server[MAXLINE];
    rio_t rio_client, rio_server;
    int port, proxyfd;//port为连接end_server的端口，proxyfd是与end_server连接的描述符
    

    Rio_readinitb(&rio_client, connfd);
    if(Rio_readlineb(&rio_client, buf, MAXLINE) < 0){//读取客户端的请求行,请求头字段保存在rio的缓冲区
        printf("Read request row error\n");
        return;
    }
    sscanf(buf, "%s %s %s", method, url, version);
    if(strcasecmp(method, "GET")){//若不是方法GET,则报错
        printf("fault method: %s\nOnly support method \"GET\"\n", method);
        return;
    }

    printf("method=%s\nurl=%s\nversion=%s\n", method, url, version);

    char *copy = (char *)Malloc(sizeof(url));
    strcpy(copy, url);
    /*解析url得到终点服务器的主机名、端口和资源路径*/
    parse_url(url, hostname, path, &port);
    // free(copy);

    // printf("hostname = %s\npath = %s\nport = %d\n", hostname, path, port);
    // printf("mycache=%p\n", &mycache);
    block_t *target = find_block(mycache, copy);
    int size = mycache->total_size;

    if(target == NULL){//没有找到缓存块，需要向服务器请求资源
        /*构建发送给end_server的请求头,存储在header_to_server中*/
        request_header(header_to_server, hostname, path, &rio_client);

        // printf("header_to_server:\n%s\n", header_to_server);

        /*与服务器连接,然后转发请求头,并返回描述符clientfd*/
        proxyfd = connect_server(hostname, port, header_to_server);
        if(proxyfd < 0){
            printf("Connection error.\n");
            return;
        }

        /*接收end_server传递的数据，并将其转发给客户端,同时储存在缓存中*/
        ssize_t n, cache_size = 0;
        char *cache_store = (char *)Malloc(MAX_OBJECT_SIZE);
        char *temp_ptr = cache_store;

        Rio_readinitb(&rio_server, proxyfd);
        
        while((n = Rio_readnb(&rio_server, buf, MAXLINE)) > 0){
            printf("n=%d\tcache_size=%d\ttemp_ptr=%p\n"
                ,n,cache_size,temp_ptr);

            Rio_writen(connfd, buf, n);
            if(n + cache_size <= MAX_OBJECT_SIZE)
                memcpy(temp_ptr, buf, n);
            cache_size += n;
            temp_ptr += n;
            printf("proxy received %d bytes from end_server\n", (int)n);
        }

        printf("n=%d\tcache_size=%d\ttemp_ptr=%p\n"
                ,n,cache_size,temp_ptr);

        if(cache_size <= MAX_OBJECT_SIZE){
            int accu_size = get_total_size(mycache);
            block_t *block = build_block(copy, cache_store, cache_size);
            block_t *evict_block = mycache->tail->prev;
            free(copy);
            while(cache_size + accu_size > MAX_CACHE_SIZE){//驱逐超出空间的缓存块
                block_t *temp_block = evict_block->prev;
                printf("evict_block=%p\ttemp_block=%p\taccu_size = %d\n"
                    , evict_block, temp_block, accu_size);

                remove_block(mycache, evict_block);//驱逐尾节点左侧的缓存块
                free_block(evict_block);//释放被驱逐缓存块资源
                accu_size = get_total_size(mycache);
                evict_block = temp_block;
            }
            printf("block=%p\n", block);
            insert_block(mycache, block);//插入双向链表的头结点右侧
        }

        Close(proxyfd);
    }
    else{//匹配到了缓存块
        char *content = get_content(target);//target已经上了读锁，可以安全的读取
        int size = get_size(target);
        pthread_rwlock_unlock(&target->rwlock);//接触目标缓存块的读锁
        Rio_writen(connfd, content, size);
    }
}

/**与服务器连接,然后转发请求头,并获得描述符clientfd
     * 如果客户端发送的不是有效的http请求，则直接返回
     * 直接使用Open_clientfd出错会导致代理停止工作，不满足鲁棒性的要求  */
int connect_server(char *hostname, int port, char *header_to_server){
    int proxyfd;
    char port_str[MAXLINE];
    sprintf(port_str, "%d", port);

    proxyfd = Open_clientfd(hostname, port_str);
    if(proxyfd < 0)
        return proxyfd;
    printf("hostname=%s\nport=%s\n", hostname, port_str);
    printf("header_to_server:\n%s\n", header_to_server);

    Rio_writen(proxyfd, header_to_server, strlen(header_to_server));//没注意的bug，写成了sizeof而非strlen
    return proxyfd;
}

/*parse_url解析url,得到主机hostname,资源路径path,服务器监听端口port*/
void parse_url(char *url, char *hostname, char *path, int *portp){
    char *point = strstr(url, "//");
    point += 2;//pos为hostname的位置
    char *point2 = strstr(point, "/");
    char *point3 = strstr(point, ":");
    int length;

    if(point3 == NULL)//使用默认端口80
        *portp = 80;
    else{
        *portp = atoi(point3 + 1);
        *point3 = '\0';
    }

    if(point2 == NULL)
        strcpy(path, "/");
    else{
        strcpy(path, point2);
        *point2 = '\0';
    }
    strcpy(hostname, point);
    // length = strlen(hostname);
    // sprintf(hostname + length, ":%d", *portp);
}/*小心strcpy不能用于字符串常量*/

/**构造发送给end_server的请求头
 * Host, User-Agent, Connection, and Proxy-Connection
*/
void request_header(char *header_to_server, char *hostname, char *path, rio_t *rio_client){
    ssize_t n;
    char buf[MAXLINE];
    sprintf(header_to_server, "GET %s HTTP/1.0\r\n", path);
    sprintf(header_to_server, "%sHost: %s\r\n",  header_to_server, hostname);
    sprintf(header_to_server, "%sConnection: close\r\n", header_to_server);
    sprintf(header_to_server, "%s%s", header_to_server, user_agent_hdr);
    sprintf(header_to_server, "%sProxy-Connection: close\r\n", header_to_server);

    //在doit中已经读了请求行，现在开始读请求头字段
    while((n = Rio_readlineb(rio_client, buf, MAXLINE)) > 0){ 
        if(!strncasecmp(buf, "Host", 4) || !strncasecmp(buf, "Connection", 10)
            || !strncasecmp(buf, "User-Agent", 10) || !strncasecmp(buf, "Proxy-Connection", 16)){
            continue;
        }
        else if(!strncasecmp(buf, "\r\n", 2)){
            sprintf(header_to_server, "%s\r\n", header_to_server);
            break;
        }
        else{
            sprintf(header_to_server, "%s%s", header_to_server, buf);
        }
    }
}
