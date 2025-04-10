#include <stdio.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <string.h>
#include <stddef.h>
#include <assert.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <time.h>
#ifdef __APPLE__
#include <sys/errno.h>
#else
#include <errno.h>
#endif

#include "lab.h"

#define handle_error_and_die(msg) \
    do                            \
    {                             \
        perror(msg);              \
        raise(SIGKILL);          \
    } while (0)

/**
 * @brief Convert bytes to the correct K value
 *
 * @param bytes the number of bytes
 * @return size_t the K value that will fit bytes
 */
size_t btok(size_t bytes)
{
    size_t k = 0; 
    while((UINT64_C(1) << k) < bytes)
    {
        k++;
    }
    return k;
}

struct avail *buddy_calc(struct buddy_pool *pool, struct avail *buddy){

    if(!pool || !buddy) {
        return NULL;
    } else {
        uintptr_t buddyBlock = (uintptr_t)buddy - (uintptr_t)pool->base;
        buddyBlock = buddyBlock ^ UINT64_C(1) << buddy->kval;
        return (struct avail *)((uintptr_t)pool->base + buddyBlock);
    }
}

void *buddy_malloc(struct buddy_pool *pool, size_t size)
{
    //printf("Attempting to allocate %zu bytes\n", size); //debug

    if (size == 0 || pool == NULL) {
        return NULL;
    }

    //get the kval for the requested size with enough room for the tag and kval fields
    size_t kval = btok(size + sizeof(struct avail));

    if (kval > pool->kval_m)
    {
        errno = ENOMEM;
        return NULL;
    }

    if (kval < MIN_K) kval = MIN_K;
    if (kval > MAX_K) kval = MAX_K - 1;

    //R1 Find a block
    struct avail *availBlock = NULL;
    for (size_t k = kval; k <= MAX_K; k++) {
        if (pool->avail[k].next != &pool->avail[k]) {
            availBlock = pool->avail[k].next;
            break;
        }
    }
    
    //There was not enough memory to satisfy the request thus we need to set error and return NULL
    if (!availBlock) {
        errno = ENOMEM;
        return NULL;
    }

    //R2 Remove from list;
    availBlock->prev->next = availBlock->next;
    availBlock->next->prev = availBlock->prev;

    //R3 Split required?
    while(availBlock->kval > kval) {
        size_t split_size = UINT64_C(1) << (availBlock->kval - 1);
        struct avail *buddyBlock = (struct avail *)((uintptr_t)availBlock + split_size);

        buddyBlock->tag = BLOCK_AVAIL;
        buddyBlock->kval = availBlock->kval - 1;

        buddyBlock->next = pool->avail[buddyBlock->kval].next;
        buddyBlock->prev = &pool->avail[buddyBlock->kval];
        pool->avail[buddyBlock->kval].next = buddyBlock;

        buddyBlock->next->prev = buddyBlock;
    }

    //R4 Split the block
    availBlock->tag = BLOCK_RESERVED;

    return (void *)((uintptr_t)availBlock + sizeof(struct avail));

}



void buddy_free(struct buddy_pool *pool, void *ptr){
   // printf("Freeing memory at %p\n", ptr); //debug

    struct avail *block = (struct avail *)ptr;
    size_t k = block->kval;

    void *buddyPtr;

    if (((uintptr_t)block - (uintptr_t)pool->base) % (UINT64_C(1) << (k + 1)) == 0) {
         //S1
        buddyPtr = (void *)((uintptr_t)block + (UINT64_C(1) << k));
    } else {
        buddyPtr = (void *)((uintptr_t)block - (UINT64_C(1) << k));
    }

    
    struct avail *buddy = (struct avail *)buddyPtr;


    //S1
    if(buddy->tag == BLOCK_AVAIL && buddy->kval == k) {
        //S2
        buddy->prev->next = buddy->next;
        buddy->next->prev = buddy->prev;
        k++;

        if(buddy < block) {
            block = buddy;
        }

        buddy_free(pool, block);
    } else {
        //S3
        block->tag = BLOCK_AVAIL;
        block->kval = k;
        struct avail *first = &pool->avail[k];
        block->next = first->next;
        block->prev = first;
        first->next->prev = block;
        first->next = block;
    }


}

/**
 * @brief This is a simple version of realloc.
 *
 * @param poolThe memory pool
 * @param ptr  The user memory
 * @param size the new size requested
 * @return void* pointer to the new user memory
 */
// void *buddy_realloc(struct buddy_pool *pool, void *ptr, size_t size)
// {
//     //Required for Grad Students
//     //Optional for Undergrad Students
// }

void buddy_init(struct buddy_pool *pool, size_t size)
{
    size_t kval = 0;
    if (size == 0)
        kval = DEFAULT_K;
    else
        kval = btok(size);

    if (kval < MIN_K)
        kval = MIN_K;
    if (kval > MAX_K)
        kval = MAX_K - 1;

    //make sure pool struct is cleared out
    memset(pool,0,sizeof(struct buddy_pool));
    pool->kval_m = kval;
    pool->numbytes = (UINT64_C(1) << pool->kval_m);
    //Memory map a block of raw memory to manage
    pool->base = mmap(
        NULL,                               /*addr to map to*/
        pool->numbytes,                     /*length*/
        PROT_READ | PROT_WRITE,             /*prot*/
        MAP_PRIVATE | MAP_ANONYMOUS,        /*flags*/
        -1,                                 /*fd -1 when using MAP_ANONYMOUS*/
        0                                   /* offset 0 when using MAP_ANONYMOUS*/
    );
    if (MAP_FAILED == pool->base)
    {
        handle_error_and_die("buddy_init avail array mmap failed");
    }

    //Set all blocks to empty. We are using circular lists so the first elements just point
    //to an available block. Thus the tag, and kval feild are unused burning a small bit of
    //memory but making the code more readable. We mark these blocks as UNUSED to aid in debugging.
    for (size_t i = 0; i <= kval; i++)
    {
        pool->avail[i].next = pool->avail[i].prev = &pool->avail[i];
        pool->avail[i].kval = i;
        pool->avail[i].tag = BLOCK_UNUSED;
    }

    //Add in the first block
    pool->avail[kval].next = pool->avail[kval].prev = (struct avail *)pool->base;
    struct avail *m = pool->avail[kval].next;
    m->tag = BLOCK_AVAIL;
    m->kval = kval;
    m->next = m->prev = &pool->avail[kval];
}

void buddy_destroy(struct buddy_pool *pool)
{
    int rval = munmap(pool->base, pool->numbytes);
    if (-1 == rval)
    {
        handle_error_and_die("buddy_destroy avail array");
    }
    //Zero out the array so it can be reused it needed
    memset(pool,0,sizeof(struct buddy_pool));
}

#define UNUSED(x) (void)x

/**
 * This function can be useful to visualize the bits in a block. This can
 * help when figuring out the buddy_calc function!
 */
// static void printb(unsigned long int b)
// {
//      size_t bits = sizeof(b) * 8;
//      unsigned long int curr = UINT64_C(1) << (bits - 1);
//      for (size_t i = 0; i < bits; i++)
//      {
//           if (b & curr)
//           {
//                printf("1");
//           }
//           else
//           {
//                printf("0");
//           }
//           curr >>= 1L;
//      }
// }

