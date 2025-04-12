#pragma once

#include "spinlock.h"
#include "types.h"
#include "list.h"  // 引入 list.h 來使用 Linux 風格的雙向鏈表

struct run {
  struct run *next;
};

/**
 * struct slab - Represents a slab in the slab allocator.
 * 優化結構大小：減少欄位大小，移除不必要的指針
 */
struct slab
{
  struct run *freelist;          // Linked list of free objects
  struct list_head slab_list;    // Linux 風格的雙向鏈表結構
  
  // 使用位域來節省空間
  unsigned int inuse : 12;       // 使用中的物件數量 (最多支援 4096 個物件)
  unsigned int total : 12;       // 物件總數
  
  // 因為 slab 都會被加到特定的 list 中 (partial, full, free)
  // 所以可以透過 list 判斷 slab 屬於哪個 kmem_cache，不需要額外的指針
  // struct kmem_cache *cache 欄位被移除
};

/**
 * struct kmem_cache - Represents a cache of slabs.
 * @name: Cache name (e.g., "file").
 * @object_size: Size of a single object.
 * @lock: Lock for cache management.
 */
struct kmem_cache
{
  char name[32];                 // Cache name (e.g., "file")
  uint object_size;              // Size of a single object
  struct spinlock lock;          // Lock for cache management
  
  struct list_head partial;      // Partially allocated slabs
  struct list_head full;         // Completely allocated slabs
  struct list_head free;         // Free slabs
  
  int slab_size;                 // Size of each slab (typically one page)
  int num_objects_per_slab;      // Number of objects that can fit in a slab
  int num_slabs;                 // Total number of slabs managed by this cache
  
  // Internal cache optimization (for bonus)
  void *in_cache_freelist;       // Freelist for objects inside kmem_cache
  int in_cache_obj_capacity;     // Maximum objects that can fit inside kmem_cache
  int in_cache_obj_used;         // Number of objects used inside kmem_cache
};

/**
 * kmem_cache_create - Create a new slab cache.
 * @name: The name of the cache.
 * @object_size: The size of each object in the cache.
 *
 * Return: A pointer to the new cache.
 */
struct kmem_cache *kmem_cache_create(char *name, uint object_size);

/**
 * kmem_cache_destroy - Destroy a slab cache.
 * @cache: The cache to be destroyed.
 */
void kmem_cache_destroy(struct kmem_cache *cache);

/**
 * kmem_cache_alloc - Allocate an object from a slab cache.
 * @cache: The cache to allocate from.
 *
 * Return: A pointer to the allocated object.
 */
void *kmem_cache_alloc(struct kmem_cache *cache);

/**
 * kmem_cache_free - Free an object back to its slab cache.
 * @cache: The cache to free to.
 * @obj: The object to free.
 */
void kmem_cache_free(struct kmem_cache *cache, void *obj);

/**
 * print_kmem_cache - Print the details of a kmem_cache.
 * @cache: The cache to print.
 * @print_fn: Function to print each object in the cache. If NULL (0) is given, will skip object printing part.
 */
void print_kmem_cache(struct kmem_cache *cache, void (*print_fn)(void *));