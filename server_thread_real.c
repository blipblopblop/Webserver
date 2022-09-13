#include "request.h"
#include "server_thread.h"
#include "common.h"
#include <pthread.h>

//fixed size of pool threads
#define THREAD_POOLS_SIZE 40 
const int hashSize = 5000;

struct server {
    int nr_threads; // threadpool size 
    // lab 5
    // maximum amount of bytes used by all files in file cache
    int max_requests; // usable slots 
    int max_cache_size;
    // lab 5: size a hash table based on the expected load factor, from ./fileset: mean file size = 10117,
    int loadFactor;
    int exiting;

    /* add any other parameters you need */
    // slides F1-synch-intro
    int *buff; 
    int in; // place to write to buff
    int out; // place to read the buff

    // pointer to pool of threads
    // hand of each new client connection to one of the worker threads
    pthread_t *threadPool;
    
    // protect calls to remove and add requests to buffer
    pthread_mutex_t bufflock;
    pthread_cond_t cv_full;
    pthread_cond_t cv_empty;
    // lab 5: lock for reading files so multiple copies of same file aren't stored
    pthread_mutex_t file_lock;
    
    // lab 5
    // declare the hash table and doubly linked nodes
    // global variables that point ALWAYS to front/rear(back) of list 
    struct cacheUse *front, *rear;
    struct cacheTable *cache;
};

// lab 5 
// Use doubly linked list to evict the least used node
struct cacheUse{
    // the PTE corresponding to that cache position
    struct node *cacheTableNode; 
    // prev is right pointer, next is left pointer
    struct cacheUse *prev, *next;
};

// Node to get stored in the hash table 
struct node {
    // key is the file name for the cache lookup
    char *key;
    // size of the file 
    int size;
    // reference count to indicate how many users are using this file
    int frequency;
    // data is file data that is fetched from disk
    struct file_data *data;
    // pointer that hold position this node(data entry in LRU) is in cacheUsage
    struct cacheUse *pos;
    // PTE to left(down) in hastable
    struct node *next;
};

// hashtable (cacheUse) to lookup cached files
struct cacheTable {
    struct node **table;
    int currentsize;
    int tablesize;
};

/* static functions */
static void
do_server_request(struct server *sv, int connfd);

// lab 5
// adding new entry into double linked list for tracking cache usage 
struct cacheUse * cacheUsage_insert(struct server *sv, struct node *h, struct file_data *data);
// update if it exists in double linked list 
struct cacheUse * cacheUsage_update(struct server *sv, struct node *h, struct file_data *data);
// adding new entry into the hash-table containing the cache
struct node * cache_insert(struct server *sv, struct cacheTable *h, struct file_data *data);
// check if the file exists in the hash-table
struct node * cache_lookup(struct server *sv, struct cacheTable *h, struct file_data *data);
// size to remove to add new cache in hash-table
void cache_evict(int amount_to_evict, struct server *sv, struct cacheTable *h);
//Deallocates memory of node contents stored in hashtable
void freenode(struct node *node);

void *threadPoolFunc(struct server *server) {
    // code influenced by slides F1-synch-intro + F2-monitors    
    while (1) {
        pthread_mutex_lock(&(server->bufflock));
        
        // the buffer is empty 
        while (server->in == server->out) {
            //printf("threadPoolFunc 1 \n");
            
            // server is exiting here
            if (server->exiting == 1) {
                //wakeup all the threads
                pthread_mutex_unlock(&(server->bufflock));
                pthread_cond_signal(&(server->cv_empty));
                //printf("inside threadPoolFunc in exit in:%d, out:%d \n ", server->in , server->out);
                fflush(stdout);
                return NULL;
            }else{
                // threads will only wait if they can't get work from the buff
                //printf("inside threadPoolFunc in waiting in:%d, out:%d \n ", server->in , server->out);
                pthread_cond_wait( &(server->cv_empty), &(server->bufflock));
                //printf("hello");
                fflush(stdout);
            }
            fflush(stdout);
            
        } 
        //printf("threadPoolFunc 2 in:%d, out:%d \n ", server->in , server->out);
        // pull out new req
        int connfd = server->buff[server->out];   
        // adjust position on buffer
        server->out = (server->out + 1) % (server->max_requests );
        
        // condition variable cond to be signaled.
        pthread_cond_signal(&(server->cv_full));
        
        //printf("threadPoolFunc 3 in:%d, out:%d \n ", server->in , server->out);
        
        // unlocks the mutex and waits for the condition variable cond to be signaled.
        pthread_mutex_unlock(&(server->bufflock));
        
        // workers do work on request  
        do_server_request(server, connfd);
        fflush(stdout);
         
    }
    fflush(stdout);
    return NULL;
}

// lab 5
unsigned long hashFunc(char *str)
{
    fflush(stdout);
    unsigned long hash = 5381;
    int length = strlen(str);
    for(int i = 0; i < length; i++, ++str){
        hash =( (hash << 5) + hash )+ (*str);
    }
    //printf("do_server_request start 146 RETURN  \n");
    fflush(stdout);
    return hash;
}

// Allocates memory for a new node. Initializes the new node's members 
struct cacheUse * cacheUsage_insert(struct server *sv, struct node *h, struct file_data *data) {
    if(sv->front == NULL && sv->rear == NULL){
        //linked list is not setup yet
        // allocate memory for node to insert doubly linked list 
        struct cacheUse *cacheUsage = Malloc(sizeof(struct cacheUse *) );   
        // data insert: PTE entry 
        cacheUsage->cacheTableNode = h; 
        // first node so only points to NULL cause no other nodes exist
        cacheUsage->next = NULL; 
        cacheUsage->prev = NULL;   
        // move the head to point to the new node 
        sv->front = cacheUsage; 
        sv->rear = cacheUsage;
        return cacheUsage;
    }else{
        // place at the front bc we will remove least recent used so at back(rear pointing)
        // allocate memory for node to insert doubly linked list 
        struct cacheUse *cacheUsage = Malloc(sizeof(struct cacheUse *) );
        // data insert: PTE entry 
        cacheUsage->cacheTableNode = h; 
        // prev head should be second-first node so immediate left entry 
        cacheUsage->next = sv->front; 
        // head is now current where head always points to NULL
        cacheUsage->prev = NULL;   
        // change prev of head node to new node 
        sv->front->prev = cacheUsage; 
        // move the global front to point to the new node 
        sv->front = cacheUsage; 
        return cacheUsage;
    }
    return NULL; 
}

// update if it exists in double linked list 
struct cacheUse * cacheUsage_update(struct server *sv, struct node *h, struct file_data *data){
    // at front 
    if(sv->front == h->pos){
        // nothing to do, at the front
        return h->pos;
    }else{
        h->pos->next->prev = h->pos->prev;
        h->pos->prev->next = h->pos->next;
        
        h->pos->next = sv->front;
        h->pos->prev = NULL;
        // change prev of head node to new node 
        sv->front->prev = h->pos; 
        // move the global front to point to the new node 
        sv->front = h->pos;
        return h->pos;
    }
    return NULL;
}

// adding a new entry in hash table
struct node * cache_insert(struct server *sv, struct cacheTable *h, struct file_data *data) {
    // table entry in set up was init to NULL, allocate a node at that entry then fill in          
    // create a new node & place new node at the index calculated
    // aloccate that new node 
    
    // user is using this file so append count and place mutex lock
    pthread_mutex_lock(&sv->file_lock);
    
    struct node *newnode = (struct node *) malloc(sizeof(struct node));
    if(newnode != NULL) {
        newnode->data = data;
        newnode->key = data->file_name;
        // being used, user is setting it up
        newnode->frequency =  1;
        newnode->size = data->file_size;
        // next node 
        newnode->next = NULL;
    }
    // figure out right index
    unsigned long index = hashFunc(data->file_name) % sv->loadFactor;     
    // placing created node at index
    h->table[index] = newnode;
    // update the total table-size
    h->currentsize = h->currentsize + newnode->size;     
    // and now, fix up the cache usage for LRU eviction, only accessed via insert so lock safety is insured
    newnode->pos = cacheUsage_insert(sv, newnode, data);
    
    // user has finished accessing this file
    pthread_mutex_unlock(&sv->file_lock);
    newnode->frequency = newnode->frequency - 1;
    return newnode;
}

// look up a cache with this name in hash table
struct node * cache_lookup(struct server *sv, struct cacheTable *h, struct file_data *data){
    if(sv->loadFactor == 0){
        return NULL;
    }
    // create a node to search
    unsigned long index = hashFunc(data->file_name) % sv->loadFactor;     
    struct node *current = h->table[index];
    // Search for duplicate value and update the entry 
    while(current != NULL) {
        
        // user is using this file so append count and place mutex lock
        pthread_mutex_lock(&sv->file_lock);
        current->frequency = current->frequency + 1;
        
        if(strcmp(data->file_name, current->key) == 0){
            current->pos = cacheUsage_update(sv, current, data);

            // user is finished reading + updating file (update only accesses through lookup so safe)
            pthread_mutex_unlock(&sv->file_lock);
            current->frequency = current->frequency - 1;
            
            return current;
        }else{
            // user has finished accessing this file
            pthread_mutex_unlock(&sv->file_lock);
            current->frequency = current->frequency - 1;
        }
        current = current->next;
    }
    
    // no duplicate value found, create a new entry then 
    return NULL;
}

// size to remove to add new cache in hash-table
void cache_evict(int amount_to_evict, struct server *sv, struct cacheTable *h){
    printf("line 268\n");
        fflush(stdout);
    // empty 
    if(sv->rear == NULL || sv->front == NULL){
        return;
    }
        printf("line 274\n");
        fflush(stdout);
    int memRem = 0;
    struct cacheUse *newRear;
    struct cacheUse *newRearPrev;
    struct cacheUse *remRear;
    struct cacheUse *remRearPrev;
    
    printf("line 282 amount ot evict is %d \n", amount_to_evict);
        fflush(stdout);
    
    if(sv->front != NULL){
        printf("inside the eviction loop\n");
        while(memRem < amount_to_evict || sv->front == NULL){
            
            if(sv->rear->cacheTableNode->frequency > 0){
                printf("stop people are acessing this file\n");
            }
            
            printf("line 301\n");
            fflush(stdout);
            // rear is the PTE, take size of that PTE
            memRem = memRem + sv->rear->cacheTableNode->data->file_size;
             printf("line 305\n");
            // nodes to remove later
            remRear = sv->rear;
             printf("line 308\n");
            remRearPrev = sv->rear->prev;
             printf("line 310\n");
            // restructure the last node
            newRear = sv->rear->prev;
            newRearPrev = sv->rear->prev->prev;
            sv->rear = newRear;
            sv->rear->prev = newRearPrev;
            sv->rear->next = NULL;  // because its the last node now        
            // delete contents of that node 
             printf("line 318\n");
            freenode(remRear->cacheTableNode);
            // free newly deleted node
             printf("line 321\n");
            free(remRear);
             printf("line 323\n");
            free(remRearPrev);
             printf("line 325\n");
            remRear = NULL;
            remRearPrev = NULL;
            
            printf("line 282 amount left tt evict is %d \n", memRem);
            fflush(stdout);
        }
    }
    printf("line 303 memRem is %d\n", memRem);
        fflush(stdout);
}

/* initialize file data */
static struct file_data *
file_data_init(void) {
    struct file_data *data;

    data = Malloc(sizeof (struct file_data));
    data->file_name = NULL;
    data->file_buf = NULL;
    data->file_size = 0;
    return data;
}

/* free all file data */
static void
file_data_free(struct file_data *data) {
    printf("line 360\n");
     fflush(stdout);
    free(data->file_name);
    free(data->file_buf);
    free(data);
     
}

// lab 5:
// Deallocates memory of node
void freenode(struct node *node){
    file_data_free(node->data);
    free(node->key);
    free(node->pos);
    free(node);
}

static void
do_server_request(struct server *sv, int connfd) {
    int ret;
    struct request *rq;
    struct file_data *data;

    data = file_data_init();
    printf("line 336\n");
        fflush(stdout);
    /* fill data->file_name with name of the file being requested */
    rq = request_init(connfd, data);
    if (!rq) {
        file_data_free(data);
        return;
    }
    /* read file, 
     * fills data->file_buf with the file contents,
     * data->file_size with file size. */
    // create a node (cache) to contain the file 
    struct node *node = NULL;
    if(sv->loadFactor > 0){        
        printf("line 351\n");
            fflush(stdout);
        // check if the cachefile is in the cacheTable, synch stuff done in there
        node = cache_lookup(sv, sv->cache, data);
        printf("line 413\n");
            fflush(stdout);
        if(node != NULL){
            printf("line 358\n");
            fflush(stdout);
            // inside cache, so we do not need to make a new entry for it 
            ret = request_readfile(rq);
            if (ret == 0) { /* couldn't read file */;
                goto out;
            }       
        }else{
            printf("line 366\n");
            fflush(stdout);
            // is not inside the cache table, create a new entru 
            ret = request_readfile(rq);
            if (ret == 0) { /* couldn't read file */
                goto out;
            }
            printf("line 373\n");
            fflush(stdout);
            printf("file_size: %d \n",data->file_size);
            printf("currentsize: %d \n",sv->cache->currentsize);
            if(data->file_size > sv->cache->currentsize){
                printf("line 376\n");
                fflush(stdout);
                cache_evict(data->file_size, sv, sv->cache);
            }
            printf("line 380\n");
            fflush(stdout);
            // Call to insert new PTE into hash-table
            node = cache_insert(sv, sv->cache, data);
            printf("line 384\n");
            fflush(stdout);
        }
    }
    printf("line 385\n");
        fflush(stdout);
    /* send file to client */
    request_sendfile(rq);  
        
out:
    printf("line 417\n");
    fflush(stdout);
    request_destroy(rq);
    if(node == NULL){
        printf("line 421\n");
        file_data_free(data);
    }else{
        // user has finished accessing this file
        printf("line 425 the freq is %d\n", node->frequency);
        if (node->frequency == 0){
            printf("I AM FREE\n");
            file_data_free(data);
        }
        pthread_mutex_lock(&sv->file_lock);
        
        node->frequency = node->frequency - 1;
        pthread_mutex_unlock(&sv->file_lock);
    }
    printf("line 453\n");
    fflush(stdout);
}

/* entry point functions */

struct server *
server_init(int nr_threads, int max_requests, int max_cache_size) {
    struct server *sv;

    sv = Malloc(sizeof (struct server));
    sv->nr_threads = nr_threads; 
    // append because last element in buffer is not used
    sv->max_requests = max_requests + 1;
    // append bc you do not want to exceed cache
    sv->max_cache_size = max_cache_size;
    // lab 5 insert
    // size a hash table based on the expected load factor, from ./fileset: mean file size = 10117,
    sv->loadFactor = max_cache_size/10117;
    sv->exiting = 0;
    
    sv->in = 0;
    sv->out = 0;
    
    sv->bufflock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    sv->cv_empty = (pthread_cond_t)PTHREAD_COND_INITIALIZER;
    sv->cv_full = (pthread_cond_t)PTHREAD_COND_INITIALIZER;
    // lab5: to ensure multiple copies aren't stored, implement lock
    sv->file_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
        
    if (nr_threads > 0 || max_requests > 0 || max_cache_size > 0) {
        //TBD();
        /* Lab 4: create queue of max_request size when max_requests > 0 */        
        sv->buff = Malloc(sizeof (int)*(max_requests) );
        printf("the load factor is %d\n", sv->loadFactor);
         fflush(stdout);
            /* Lab 5: init server cache and limit its size to max_cache_size */                     
            // allocating cacheTable
        if(sv->loadFactor > 0){         
            sv->cache = (struct cacheTable *)malloc(sizeof(struct cacheTable ));

            printf("line 490\n");
            fflush(stdout);
            assert(sv->cache);        
            if(sv->cache != NULL) {
                sv->cache->currentsize = 0;
                // play around later, maybe max_cache_size - 1?
                sv->cache->tablesize = sv->max_cache_size;
            }        
            // allocating the table entries inside it, but node contained in entries are  left to cache insertss
            sv->cache->table = (struct node **) malloc(sizeof(struct node *) * sv->loadFactor);

            printf("line 501\n");
            fflush(stdout);
            assert(sv->cache->table);
            for(int i = 0; i < sv->loadFactor; i++){
                // set entry to NULL file in during cache_insert 
                sv->cache->table[i] = NULL;

            //printf("line 425\n");
            fflush(stdout);
            }       
            // for the LRU, used to find cache position on linked list 
            // access front & back double linked list easily 
            sv->front = NULL;
            sv->rear = NULL;        
            printf("line 515\n");
            fflush(stdout);
        }
          printf("line 518\n");
            fflush(stdout);
        /* Lab 4: create worker threads when nr_threads > 0 */
        sv->threadPool = Malloc(sizeof (pthread_t)*nr_threads);
        //create the worker threads
        for (int i = 0; i < nr_threads; i++) {
            //printf("forloop check \n");
            pthread_create(&(sv->threadPool[i]), NULL, (void *) (threadPoolFunc), sv);
        }
        fflush(stdout);
    }
    
    return sv;
}

void
server_request(struct server *sv, int connfd) {
    if (sv->nr_threads == 0) { /* no worker threads */
        do_server_request(sv, connfd);
    } else {
        /*  Save the relevant info in a buffer and have one of the
         *  worker threads do the work. */
        //TBD();
        // code influenced by slides F1-synch-intro / F2-Monitors
        pthread_mutex_lock(&(sv->bufflock));
        // full 
        while (((sv->in - sv->out + sv->max_requests ) % (sv->max_requests ) ) == (sv->max_requests - 1 )) {
            //printf("inside server_request   1 in:%d, out:%d, max_requests:%d\n", sv->in , sv->out, sv->max_requests);
            printf("as you can see %d indicates sv->in is full", sv->in);
            // restarts one of the threads that are waiting on the condition variable cond.
            fflush(stdout);
            pthread_cond_wait( &(sv->cv_full), &(sv->bufflock));
            fflush(stdout);
        } 
        //printf("inside server_request 2 in:%d, out:%d, max_requests:%d\n", sv->in , sv->out, sv->max_requests); 
        // place new req
        sv->buff[sv->in] = connfd;
        
        // adjust position on buffer
        sv->in = (sv->in + 1) % (sv->max_requests ); 
        
        //the condition variable cond to be signaled
        pthread_cond_signal(&(sv->cv_empty));
        //printf("inside server_request 3 in:%d, out:%d, max_requests:%d\n", sv->in , sv->out, sv->max_requests);
        
        // unlocks the mutex
        pthread_mutex_unlock(&(sv->bufflock));
        fflush(stdout);
    }
}

void
server_exit(struct server *sv) {
    /* when using one or more worker threads, use sv->exiting to indicate to
     * these threads that the server is exiting. make sure to call
     * pthread_join in this function so that the main server thread waits
     * for all the worker threads to exit before exiting. */
    //locking the mutex before exiting
    pthread_mutex_lock(&(sv->bufflock));
    sv->exiting = 1;
    //printf("inside server_exit 1 in:%d, out:%d, max_requests:%d\n", sv->in , sv->out, sv->max_requests);
    //unlocking the mutex before exiting
    pthread_mutex_unlock(&(sv->bufflock));
    
    //signalling first waiting thread
    pthread_cond_signal(&(sv->cv_empty));
    
    //printf("inside server_exit 2 in:%d, out:%d, max_requests:%d\n", sv->in , sv->out, sv->max_requests);
    //for checking if pthread is working correctly
    int ret = 9;
    
    // wait for termination of all other threads
    for (int i = 0; i < sv->nr_threads; i++) {
        //printf("inside server_exit 2a in:%d, out:%d, max_requests:%d i:%d\n", sv->in , sv->out, sv->max_requests,i);
        //fflush(stdout);
        ret = pthread_join(sv->threadPool[i], NULL);        
        //pthreadjoin produces an error
        if (ret != 0){
            //printf("NOT JOIN\n");
            fflush(stdout);
        }
        //printf("inside server_exit 2b i:%d\n", i);
        //printf("number of threads are nr_threads %d",sv->nr_threads);
        fflush(stdout);
    }
    
    // lab 5 stuff
    struct node *current = NULL;
    for(int i = 0; i < sv->loadFactor; i++) {
        current = sv->cache->table[i];
        if(current == NULL)
            continue;
        /* Deallocate memory of every node in the table */
        while(current->next != NULL) {
            sv->cache->table[i] = sv->cache->table[i]->next ;
            freenode(current);
            current = sv->cache->table[i];
        }
        freenode(current);
    }
    printf("line 567 \n");
    free(sv->cache);    
    
    //printf("inside server_exit 3 in:%d, out:%d, max_requests:%d\n", sv->in , sv->out, sv->max_requests);
    /* make sure to free any allocated resources */
    free(sv->threadPool);
    free(sv->buff);
    //printf("inside server_exit 4 in:%d, out:%d, max_requests:%d\n", sv->in , sv->out, sv->max_requests);
    printf("server has exited here \n");
    free(sv);
    fflush(stdout);
    //return;
}
