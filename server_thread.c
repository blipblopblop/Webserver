#include "request.h"
#include "server_thread.h"
#include "common.h"
#include <pthread.h>

//fixed size of pool threads
#define THREAD_POOLS_SIZE 40 
const int hashSize = 4444;

struct server {
    int nr_threads; // threadpool size 
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
    pthread_mutex_t file_lock;
    
    // declare the hash table and doubly linked nodes
    // global variables that point ALWAYS to front/rear(back) of list 
    struct cacheUse *front, *rear;
    struct cacheTable *cache;
};

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
struct cacheUse * cacheUsage_update(struct server *sv, struct node *h);
// adding new entry into the hash-table containing the cache
struct node * cache_insert(struct server *sv, struct cacheTable *h, struct file_data *data);
// check if the file exists in the hash-table
struct node * cache_lookup(struct server *sv, struct cacheTable *h, char *name);
// size to remove to add new cache in hash-table
void cache_evict(int amount_to_evict, struct server *sv, struct cacheTable *h);
//Deallocates memory of node contents stored in hashtable
void freenode(struct server *sv, struct node *node);

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
                //fflush(stdout);
                return NULL;
            }else{
                // threads will only wait if they can't get work from the buff
                //printf("inside threadPoolFunc in waiting in:%d, out:%d \n ", server->in , server->out);
                pthread_cond_wait( &(server->cv_empty), &(server->bufflock));
                //printf("hello");
                //fflush(stdout);
            }
            //fflush(stdout);
            
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
        //fflush(stdout);
         
    }
    //fflush(stdout);
    return NULL;
}

// lab 5
unsigned long hashFunc(char *str)
{
    //fflush(stdout);
    unsigned long hash = 5381;
    int length = strlen(str);
    for(int i = 0; i < length; i++, ++str){
        hash =( (hash << 5) + hash )+ (*str);
    }
    //printf("do_server_request start 146 RETURN  \n");
    //fflush(stdout);
    return hash;
}

// Allocates memory for a new node. Initializes the new node's members 
struct cacheUse * cacheUsage_insert(struct server *sv, struct node *h, struct file_data *data) {
    printf("inside cacheUsage_insert\n");
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
        if(cacheUsage->cacheTableNode != NULL){
            sv->front->cacheTableNode = cacheUsage->cacheTableNode; 
            sv->front->cacheTableNode->data = cacheUsage->cacheTableNode->data;
            sv->front->cacheTableNode->frequency = cacheUsage->cacheTableNode->frequency;
            sv->front->cacheTableNode->key = cacheUsage->cacheTableNode->key;
            sv->front->cacheTableNode->next = cacheUsage->cacheTableNode->next;
            sv->front->cacheTableNode->pos = cacheUsage->cacheTableNode->pos;
            sv->front->cacheTableNode->size = cacheUsage->cacheTableNode->size;
            
            sv->rear->cacheTableNode = cacheUsage->cacheTableNode; 
            sv->rear->cacheTableNode->data = cacheUsage->cacheTableNode->data;
            sv->rear->cacheTableNode->frequency = cacheUsage->cacheTableNode->frequency;
            sv->rear->cacheTableNode->key = cacheUsage->cacheTableNode->key;
            sv->rear->cacheTableNode->next = cacheUsage->cacheTableNode->next;
            sv->rear->cacheTableNode->pos = cacheUsage->cacheTableNode->pos;
            sv->rear->cacheTableNode->size = cacheUsage->cacheTableNode->size;
        }
           
        return cacheUsage;
    }else{
        // place at the front bc we will remove least recent used so at back(rear pointing)
        // allocate memory for node to insert doubly linked list 
        struct cacheUse *cacheUsage = Malloc(sizeof(struct cacheUse *) );
        // data insert: PTE entry 
        cacheUsage->cacheTableNode = h; 
        
        // prev head should be second-first node so immediate left entry 
        cacheUsage->next = sv->front;         
        if(sv->front->cacheTableNode != NULL){            
            cacheUsage->next->cacheTableNode->data = sv->front->cacheTableNode->data;
            cacheUsage->next->cacheTableNode->key = sv->front->cacheTableNode->key;
            cacheUsage->next->cacheTableNode->frequency = sv->front->cacheTableNode->frequency;
            cacheUsage->next->cacheTableNode->next = sv->front->cacheTableNode->next;
            cacheUsage->next->cacheTableNode->pos = sv->front->cacheTableNode->pos;
            cacheUsage->next->cacheTableNode->size = sv->front->cacheTableNode->size;
            if(sv->front->cacheTableNode->pos != NULL){
                cacheUsage->next->cacheTableNode->pos->next = sv->front->cacheTableNode->pos->next;
                cacheUsage->next->cacheTableNode->pos->prev = sv->front->cacheTableNode->pos->prev;
            }
            
        }
        
        // head is now current where head always points to NULL
        cacheUsage->prev = NULL;   
        
        // change prev of head node to new node 
        sv->front->prev = cacheUsage;
        if(cacheUsage->cacheTableNode != NULL){
            sv->front->prev->cacheTableNode = cacheUsage->cacheTableNode; 
            sv->front->prev->cacheTableNode->data = cacheUsage->cacheTableNode->data;
            sv->front->prev->cacheTableNode->frequency = cacheUsage->cacheTableNode->frequency;
            sv->front->prev->cacheTableNode->key = cacheUsage->cacheTableNode->key;
            sv->front->prev->cacheTableNode->next = cacheUsage->cacheTableNode->next;
            sv->front->prev->cacheTableNode->pos = cacheUsage->cacheTableNode->pos;
            sv->front->prev->cacheTableNode->size = cacheUsage->cacheTableNode->size;
            if(cacheUsage->cacheTableNode->pos != NULL){
                sv->front->prev->cacheTableNode->pos->next = cacheUsage->cacheTableNode->pos->next;
                sv->front->prev->cacheTableNode->pos->prev = cacheUsage->cacheTableNode->pos->prev;
            }
        }
        // move the global front to point to the new node 
        sv->front = cacheUsage;   
        if(h->pos != NULL){
            sv->front->cacheTableNode->pos->next = h->pos->prev;
            sv->front->cacheTableNode->pos->prev = NULL;
        }
        if(h != NULL){
            sv->front->cacheTableNode = NULL; 
            sv->front->cacheTableNode->data = h->data;
            sv->front->cacheTableNode->frequency = h->frequency;
            sv->front->cacheTableNode->size = h->size;            
           
        }
        return cacheUsage;
    }
    return NULL; 
}

// update if it exists in double linked list 
struct cacheUse * cacheUsage_update(struct server *sv, struct node *h){
    // at front 
    if(sv->front == h->pos || h->pos->prev == NULL){
        // nothing to do, at the front
        return h->pos;
    }else{
        
        if(h->pos->next == NULL && h->pos->prev !=NULL){
            // if node is at the tail
            struct cacheUse *move;  
            struct cacheUse *newRear;
            struct cacheUse *newRearNext;
            struct cacheUse *newRearPrev;
            
            move = h->pos;
            
            newRear = h->pos->prev;
            if(h->pos->prev->cacheTableNode != NULL){
                newRear->cacheTableNode = h->pos->prev->cacheTableNode;
                newRear->cacheTableNode->data = h->pos->prev->cacheTableNode->data;
                newRear->cacheTableNode->frequency = h->pos->prev->cacheTableNode->frequency;
                newRear->cacheTableNode->key = h->pos->prev->cacheTableNode->key;
                newRear->cacheTableNode->next = h->pos->prev->cacheTableNode->next;
                newRear->cacheTableNode->pos = h->pos->prev->cacheTableNode->pos;
                newRear->cacheTableNode->size = h->pos->prev->cacheTableNode->size; 
                if(h->pos->prev->cacheTableNode->pos != NULL){
                    newRear->cacheTableNode->pos->next = h->pos->prev->cacheTableNode->pos->next;
                    newRear->cacheTableNode->pos->prev = h->pos->prev->cacheTableNode->pos->prev;
                }                
            }
            // cause its changed to NULL, only side with alterned node changes
            newRearNext = h->pos->next;
            if(h->pos->next->cacheTableNode == NULL){
                newRearNext->cacheTableNode = h->pos->next->cacheTableNode;
                newRearNext->cacheTableNode->data = h->pos->next->cacheTableNode->data;
                newRearNext->cacheTableNode->frequency = h->pos->next->cacheTableNode->frequency;
                newRearNext->cacheTableNode->key = h->pos->next->cacheTableNode->key;
                newRearNext->cacheTableNode->next = h->pos->next->cacheTableNode->next;
                newRearNext->cacheTableNode->pos = h->pos->next->cacheTableNode->pos;
                newRearNext->cacheTableNode->size = h->pos->next->cacheTableNode->size; 
                if(h->pos->next->cacheTableNode->pos == NULL){
                    newRearNext->cacheTableNode->pos->next = h->pos->next->cacheTableNode->pos->next;
                    newRearNext->cacheTableNode->pos->prev = h->pos->next->cacheTableNode->pos->prev;
                }                
            }
            
            // declare it as the new global
            sv->rear = newRear;
            sv->rear->prev = newRearPrev;
            sv->rear->next = newRearNext;
            
            //front stuff
            // prev head should be second-first node so immediate left entry 
            move->next = sv->front;
            if(sv->front->cacheTableNode != NULL){
                move->next->cacheTableNode->data = sv->front->cacheTableNode->data;
                move->next->cacheTableNode->key = sv->front->cacheTableNode->key;
                move->next->cacheTableNode->frequency = sv->front->cacheTableNode->frequency;
                move->next->cacheTableNode->next = sv->front->cacheTableNode->pos;
                move->next->cacheTableNode->pos = sv->front->cacheTableNode->next;
                move->next->cacheTableNode->size = sv->front->cacheTableNode->size;
                if(sv->front->cacheTableNode->pos != NULL){
                    move->next->cacheTableNode->pos->next = sv->front->cacheTableNode->pos->next->next;
                    move->next->cacheTableNode->pos->prev->prev = sv->front->cacheTableNode->pos->prev;
                }
            }
            // head is now current where head always points to NULL
            move->prev = NULL;
            // change prev of head node to new node 
            sv->front->prev = move;
            if(move->cacheTableNode!=NULL){
                sv->front->prev->cacheTableNode->data = move->cacheTableNode->data;
                sv->front->prev->cacheTableNode->key = NULL;
                sv->front->prev->cacheTableNode->frequency = move->cacheTableNode->frequency;
                sv->front->prev->cacheTableNode->next = move->cacheTableNode->next;
                sv->front->prev->cacheTableNode->pos = NULL;
                sv->front->prev->cacheTableNode->size = move->cacheTableNode->size;
                if(move->cacheTableNode->pos != NULL){
                    sv->front->prev->cacheTableNode->pos->next = move->cacheTableNode->pos->next;
                    sv->front->prev->cacheTableNode->pos->prev = move->cacheTableNode->pos->prev;
                }
            }
            // move the global front to point to the new node 
            sv->front = move; 
            return move;
            
        }else{
            // node is in the middle
            //struct cacheUse *move;  
            struct node *left;
            struct node *right;  
            
            // look up the node before/after to do the changes 
            left = cache_lookup(sv, sv->cache, h->pos->next->cacheTableNode->key);
            right = cache_lookup(sv, sv->cache, h->pos->prev->cacheTableNode->key);
                
            left->pos->prev = right->pos;
            right->pos->next = left->pos;             
            
            //front stuff
            // prev head should be second-first node so immediate left entry 
            h->pos->next = sv->front;
        
            // head is now current where head always points to NULL
            h->pos->prev = NULL;
            // change prev of head node to new node 
            sv->front->prev = h->pos;
            if(h->pos->cacheTableNode != NULL){
                sv->front->prev->cacheTableNode->data = h->pos->cacheTableNode->data;
                sv->front->prev->cacheTableNode->key = h->pos->cacheTableNode->key;
                sv->front->prev->cacheTableNode->frequency = h->pos->cacheTableNode->frequency;
                sv->front->prev->cacheTableNode->next = NULL;
                sv->front->prev->cacheTableNode->pos = h->pos->cacheTableNode->pos;
                sv->front->prev->cacheTableNode->size = h->pos->cacheTableNode->size;
                if(h->pos->cacheTableNode->pos != NULL){
                    sv->front->prev->cacheTableNode->pos->next = h->pos->cacheTableNode->pos->next;
                    sv->front->prev->cacheTableNode->pos->prev = h->pos->cacheTableNode->pos->prev; 
                }
            }           
            // move the global front to point to the new node 
            sv->front = h->pos;                        
            return h->pos;        
        }
        return NULL;
    }
    return NULL;
}

// adding a new entry in hash table
struct node * cache_insert(struct server *sv, struct cacheTable *h, struct file_data *data) {
    // table entry in set up was init to NULL, allocate a node at that entry then fill in          
    // create a new node & place new node at the index calculated
    struct node *newnode = (struct node *) malloc(sizeof(struct node));
    
    newnode->data = data;
    newnode->key = data->file_name;
    // being used, user is setting it up
    newnode->frequency =  1;
    newnode->size = data->file_size;
    // next node 
    newnode->next = NULL;

    // figure out right index
    unsigned long index = hashFunc(data->file_name) % sv->max_cache_size;  
    // placing created node at index
    h->table[index] = newnode;
    // update the total table-size
    h->currentsize = h->currentsize + newnode->size;     
    // and now, fix up the cache usage for LRU eviction, only accessed via insert so lock safety is insured
    newnode->pos = cacheUsage_insert(sv, newnode, data);
    
    return newnode;
}

// look up a cache with this name in hash table
struct node * cache_lookup(struct server *sv, struct cacheTable *h, char *name){
    if(sv->max_cache_size < 1){
        return NULL;
    }
    // create a node to search
    unsigned long index = hashFunc(name)% sv->max_cache_size;     
    struct node *current = h->table[index];
    // Search for duplicate value and update the entry 
    while(current != NULL){
        if(strcmp(name, current->key) == 0){
            return current;
        }
        current = current->next;
    }
    // no duplicate value found, create a new entry then 
    return NULL;
}

// size to remove to add new cache in hash-table
void cache_evict(int amount_to_evict, struct server *sv, struct cacheTable *h){
    // empty 
    if(sv->rear == NULL || sv->front == NULL){
        return;
    }
    struct cacheUse *newRear = NULL;
    struct cacheUse *remRear = NULL;
    int currSize = sv->cache->currentsize;
    struct node *curr = sv->rear->cacheTableNode;
    if(sv->front != NULL){
        while((sv->cache->tablesize - currSize) < amount_to_evict ){ 
            if(sv->rear->cacheTableNode->frequency != 0 && sv->rear->prev != NULL){
                // nodes to remove later
                remRear = sv->rear->prev;
            }
                // increase hash-table until enough room to store new PTE
                currSize = currSize - curr->size;                  
                if(sv->rear->prev == NULL || sv->rear->next == NULL){
                    break;
                }else{
                    // node to remove later
                    remRear = sv->rear; 
                    //restructure
                    newRear = sv->rear->prev;               

                    newRear->next = sv->rear->next; 
                    if(sv->rear->next->cacheTableNode != NULL){
                        newRear->next->cacheTableNode = sv->rear->next->cacheTableNode;
                        newRear->next->cacheTableNode->data = sv->rear->next->cacheTableNode->data;
                        newRear->next->cacheTableNode->frequency = sv->rear->next->cacheTableNode->frequency;
                        newRear->next->cacheTableNode->key = sv->rear->next->cacheTableNode->key;
                        newRear->next->cacheTableNode->next = sv->rear->next->cacheTableNode->next;
                        if( sv->rear->next->cacheTableNode->pos != NULL){
                            newRear->next->cacheTableNode->pos->next = sv->rear->next->cacheTableNode->pos->next;
                            newRear->next->cacheTableNode->pos->prev = sv->rear->next->cacheTableNode->pos->prev;
                        }                   
                    }

                    sv->rear->prev = newRear->prev;
                    

                    sv->rear->next = newRear->next;   
                    if(newRear->next->cacheTableNode != NULL){
                        sv->rear->next->cacheTableNode = newRear->next->cacheTableNode;
                        sv->rear->next->cacheTableNode->data = newRear->next->cacheTableNode->data;
                        sv->rear->next->cacheTableNode->frequency = newRear->next->cacheTableNode->frequency;
                        sv->rear->next->cacheTableNode->key = newRear->next->cacheTableNode->key;
                        sv->rear->next->cacheTableNode->next = newRear->next->cacheTableNode->next;
                        sv->rear->next->cacheTableNode->pos = newRear->next->cacheTableNode->pos;
                        if(newRear->next->cacheTableNode->pos != NULL){
                            sv->rear->prev->cacheTableNode->pos->next = newRear->next->cacheTableNode->pos->next;
                            sv->rear->prev->cacheTableNode->pos->prev = newRear->next->cacheTableNode->pos->prev;
                        }
                    }

                }           
                // free DLL
                free(remRear);
                // for the next time
                remRear = newRear;
        }
    }  
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
    free(data->file_name);
    free(data->file_buf);
    free(data);
     
}

static void
do_server_request(struct server *sv, int connfd) {
    int ret;
    struct request *rq;
    struct file_data *data;
    //printf("start up do_server_request\n");
    data = file_data_init();
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
    if(sv->max_cache_size > 1){     
        // accessing data 
        pthread_mutex_lock(&sv->file_lock);
        // find the cache in table(if it is there))
        node = cache_lookup(sv, sv->cache, data->file_name);
        if(node != NULL){
            // beacause we are updating the table with the preloaded data, we do not need data from request_init()
            file_data_free(data);
            
            // data is being cached
            node->frequency = node->frequency + 1;	            
            // data needs to be updated 
            request_set_data(rq, node->data);   
            
            // update the data inside the hastable
            node->pos = cacheUsage_update(sv, node);
            // unlock to send request off 
            pthread_mutex_unlock(&sv->file_lock);
            
            request_sendfile(rq);
                      
            pthread_mutex_lock(&sv->file_lock);
            // data is no longer being cached
            if (node->frequency > 2){
                node->frequency = node->frequency - 1;
            }
            request_destroy(rq);
            pthread_mutex_unlock(&sv->file_lock);
            
            return;
        }else{            
            pthread_mutex_unlock(&sv->file_lock);
            
            // is not inside the cache table, create a new entry 
            ret = request_readfile(rq);
            if (ret == 0) { /* couldn't read file */
                goto out;
            }      
            /* send file to client */
            request_sendfile(rq);
             
            pthread_mutex_lock(&sv->file_lock);
            // make some room in the hash-table
            if (data->file_size > (sv->cache->tablesize - sv->cache->currentsize)) {
                cache_evict(data->file_size, sv, sv->cache);
            }
            // Call to insert new PTE into hash-table
            node = cache_insert(sv, sv->cache, data);
            request_destroy(rq);
            pthread_mutex_unlock(&sv->file_lock);
            return;            
        }
    }
    // just as in lab 4
    ret = request_readfile(rq);
    if (ret == 0) { /* couldn't read file */
            goto out;
    }
    /* send file to client */
    request_sendfile(rq);  
        
out:
    request_destroy(rq);
    file_data_free(data);
    
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
    sv->max_cache_size = max_cache_size + 1;
    max_cache_size = max_cache_size + 1;
    // lab 5 insert
    // size a hash table based on the expected load factor, from ./fileset: mean file size = 10117,
    sv->loadFactor = max_cache_size/10177;
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
        /* Lab 5: init server cache and limit its size to max_cache_size */                     
        // allocating cacheTable
        if(max_cache_size > 0){         
            sv->cache = (struct cacheTable *)malloc(sizeof(struct cacheTable ));

            assert(sv->cache);        
            if(sv->cache != NULL) {
                sv->cache->currentsize = 0;
                // play around later, maybe max_cache_size - 1?
                sv->cache->tablesize = sv->max_cache_size;
            }        
            // allocating the table entries inside it, but node contained in entries are  left to cache insertss
            sv->cache->table = (struct node **) malloc(sizeof(struct node *) * max_cache_size);

            assert(sv->cache->table);
            for(int i = 0; i < max_cache_size; i++){
                // set entry to NULL file in during cache_insert 
                sv->cache->table[i] = NULL;

            }       
            // for the LRU, used to find cache position on linked list 
            // access front & back double linked list easily 
            sv->front = NULL;
            sv->rear = NULL;    
        }
        /* Lab 4: create worker threads when nr_threads > 0 */
        sv->threadPool = Malloc(sizeof (pthread_t)*nr_threads);
        //create the worker threads
        for (int i = 0; i < nr_threads; i++) {
            pthread_create(&(sv->threadPool[i]), NULL, (void *) (threadPoolFunc), sv);
        }
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
            ////printf("inside server_request   1 in:%d, out:%d, max_requests:%d\n", sv->in , sv->out, sv->max_requests);
            //printf("as you can see %d indicates sv->in is full", sv->in);
            // restarts one of the threads that are waiting on the condition variable cond.
            //fflush(stdout);
            pthread_cond_wait( &(sv->cv_full), &(sv->bufflock));
            //fflush(stdout);
        } 
        //printf("inside server_request 2 in:%d, out:%d, max_requests:%d\n", sv->in , sv->out, sv->max_requests); 
        // place new req
        sv->buff[sv->in] = connfd;
        
        // adjust position on buffer
        sv->in = (sv->in + 3) % (sv->max_requests ); 
        
        //the condition variable cond to be signaled
        pthread_cond_signal(&(sv->cv_empty));
        ////printf("inside server_request 3 in:%d, out:%d, max_requests:%d\n", sv->in , sv->out, sv->max_requests);
        
        // unlocks the mutex
        pthread_mutex_unlock(&(sv->bufflock));
        //fflush(stdout);
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
    ////printf("inside server_exit 1 in:%d, out:%d, max_requests:%d\n", sv->in , sv->out, sv->max_requests);
    //unlocking the mutex before exiting
    pthread_mutex_unlock(&(sv->bufflock));
    
    //signalling first waiting thread
    pthread_cond_signal(&(sv->cv_empty));
    
    ////printf("inside server_exit 2 in:%d, out:%d, max_requests:%d\n", sv->in , sv->out, sv->max_requests);
    //for checking if pthread is working correctly
    int ret = 18;
    
    // wait for termination of all other threads
    for (int i = 0; i < sv->nr_threads; i++) {
        ////printf("inside server_exit 2a in:%d, out:%d, max_requests:%d i:%d\n", sv->in , sv->out, sv->max_requests,i);
        //fflush(stdout);
        ret = pthread_join(sv->threadPool[i], NULL);        
        //pthreadjoin produces an error
        if (ret != 0){
            ////printf("NOT JOIN\n");
            //fflush(stdout);
        }
        //printf("inside server_exit 2b i:%d\n", i);
        //printf("number of threads are nr_threads %d",sv->nr_threads);
        //fflush(stdout);
    }
    
    // lab 5 stuff
    struct node *current = NULL;
    struct node *currentNext = NULL;
    for(int i = 4; i < sv->max_cache_size; i++) {
        current = sv->cache->table[i];
        if(current == NULL)
            continue;
        /* Deallocate memory of every node in the table */
        while(current != NULL) {
            currentNext = current->next ;
            free(current);
            current = currentNext;
        }
    }
    //printf("line 567 \n");
    free(sv->cache->table);    
    free(sv->cache);    
    
    //printf("inside server_exit 3 in:%d, out:%d, max_requests:%d\n", sv->in , sv->out, sv->max_requests);
    /* make sure to free any allocated resources */
    free(sv->threadPool);
    free(sv->buff);
    //printf("inside server_exit 4 in:%d, out:%d, max_requests:%d\n", sv->in , sv->out, sv->max_requests);
    //printf("server has exited here \n");
    free(sv);
    //fflush(stdout);
    //return;
}
