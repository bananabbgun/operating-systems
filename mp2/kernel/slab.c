#include "types.h"
#include "riscv.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "defs.h"
#include "file.h"
#include "list.h"  // 引入 list.h
#include "slab.h"
#include "debug.h"

// External declarations for file_cache and fileprint_metadata
extern struct kmem_cache *file_cache;
extern void fileprint_metadata(void *f);

// 追蹤所有已創建的 kmem_cache，用於在沒有 cache 指針時找到 slab 所屬的 cache
#define MAX_CACHES 16
static struct kmem_cache* all_caches[MAX_CACHES];
static int num_caches = 0;

// 根據地址範圍找到特定 slab 所屬的 kmem_cache
/*static struct kmem_cache* find_cache_for_slab(struct slab *s) {
  for (int i = 0; i < num_caches; i++) {
    struct kmem_cache *cache = all_caches[i];
    struct slab *slab;
    
    // 查找 partial 列表
    list_for_each_entry(slab, &cache->partial, slab_list) {
      if (slab == s) return cache;
    }
    
    // 查找 full 列表
    list_for_each_entry(slab, &cache->full, slab_list) {
      if (slab == s) return cache;
    }
    
    // 查找 free 列表
    list_for_each_entry(slab, &cache->free, slab_list) {
      if (slab == s) return cache;
    }
  }
  
  return 0;
}*/

// PGSIZE is defined in riscv.h, typically 4096 bytes (4KB)
// Calculate the number of objects that can fit in a slab
static int calc_num_objects(int slab_size, int object_size, int slab_metadata_size)
{
  // Available space for objects = slab size - metadata size
  int available_space = slab_size - slab_metadata_size;
  return available_space / object_size;
}

// Find the slab that an object belongs to
static struct slab *find_slab(struct kmem_cache *cache, void *obj)
{
  struct slab *s;
  
  // Check partial slabs
  list_for_each_entry(s, &cache->partial, slab_list) {
    if ((uint64)obj >= (uint64)s && (uint64)obj < (uint64)s + PGSIZE) {
      return s;
    }
  }

  // Check full slabs
  list_for_each_entry(s, &cache->full, slab_list) {
    if ((uint64)obj >= (uint64)s && (uint64)obj < (uint64)s + PGSIZE) {
      return s;
    }
  }

  // Not found
  return 0;
}

// Create a new slab 
static struct slab *create_slab(struct kmem_cache *cache)
{
  void *mem = kalloc();
  if (!mem) {
    return 0;
  }

  struct slab *s = (struct slab *)mem;
  s->inuse = 0;
  s->total = cache->num_objects_per_slab;
  INIT_LIST_HEAD(&s->slab_list);  // 初始化 list_head

  // Calculate the start of the object space - 8 位元組對齊
  char *obj_space = (char *)mem + sizeof(struct slab);
  obj_space = (char *)(((uint64)obj_space + 7) & ~7);

  // Initialize the freelist
  struct run *last = 0;
  for (int i = 0; i < cache->num_objects_per_slab; i++) {
    struct run *r = (struct run *)(obj_space + i * cache->object_size);
    if (last) {
      last->next = r;
    } else {
      s->freelist = r;
    }
    last = r;
  }
  if (last) {
    last->next = 0;  // Mark the end of the freelist
  }

  debug("[SLAB] A new slab %p (%s) is allocated\n", s, cache->name);
  return s;
}

// Count the number of slabs in a list
static int count_slabs(struct list_head *head)
{
  int count = 0;
  struct slab *s;
  
  list_for_each_entry(s, head, slab_list) {
    count++;
  }
  
  return count;
}

void print_kmem_cache(struct kmem_cache *cache, void (*slab_obj_printer)(void *))
{
  if (!cache) {
    debug("[SLAB] Error: NULL cache passed to print_kmem_cache\n");
    return;
  }

  acquire(&cache->lock);

  debug("[SLAB] kmem_cache { name: %s, object_size: %d, at: %p, in_cache_obj: %d }\n", 
        cache->name, cache->object_size, cache, cache->in_cache_obj_capacity);

  // Print in-cache objects if any
  if (cache->in_cache_obj_capacity > 0) {
    debug("[SLAB]    [ cache slabs ]\n");
    debug("[SLAB]        [ slab %p ] { freelist: %p, in_use: %d, nxt: 0x00000000 }\n", 
          cache, cache->in_cache_freelist, cache->in_cache_obj_used);
    
    // Print objects in this cache
    char *obj_space = (char *)cache + sizeof(struct kmem_cache);
    for (int i = 0; i < cache->in_cache_obj_capacity; i++) {
      void *obj_addr = obj_space + i * cache->object_size;
      void *as_ptr = *(void **)obj_addr;
      
      debug("[SLAB]           [ idx %d ] { addr: %p, as_ptr: %p, as_obj: {", i, obj_addr, as_ptr);
      if (slab_obj_printer) {
        slab_obj_printer(obj_addr);
      }
      debug("} }\n");
    }
  }

  // Print partial slabs
  if (!list_empty(&cache->partial)) {
    debug("[SLAB]    [ partial slabs ]\n");
    struct slab *s;
    list_for_each_entry(s, &cache->partial, slab_list) {
      // 獲取下一個和上一個 slab，需要小心處理，以避免訪問 list_head 而不是 slab
      struct slab *next_s = 0;
      struct slab *prev_s = 0;
      
      if (s->slab_list.next != &cache->partial) {
        next_s = list_entry(s->slab_list.next, struct slab, slab_list);
      }
      
      if (s->slab_list.prev != &cache->partial) {
        prev_s = list_entry(s->slab_list.prev, struct slab, slab_list);
      }
      
      debug("[SLAB]        [ slab %p ] { freelist: %p, in_use: %d, prev: %p, nxt: %p }\n", 
            s, s->freelist, s->inuse, prev_s, next_s);
      
      // 計算物件開始的位置 - 8 位元組對齊
      char *obj_space = (char *)s + sizeof(struct slab);
      obj_space = (char *)(((uint64)obj_space + 7) & ~7);
      
      for (int i = 0; i < s->total; i++) {
        void *obj_addr = obj_space + i * cache->object_size;
        void *as_ptr = *(void **)obj_addr;
        
        debug("[SLAB]           [ idx %d ] { addr: %p, as_ptr: %p, as_obj: {", i, obj_addr, as_ptr);
        if (slab_obj_printer) {
          slab_obj_printer(obj_addr);
        }
        debug("} }\n");
      }
    }
  }

  // Print full slabs
  if (!list_empty(&cache->full)) {
    debug("[SLAB]    [ full slabs ]\n");
    struct slab *s;
    list_for_each_entry(s, &cache->full, slab_list) {
      // 獲取下一個和上一個 slab，需要小心處理，以避免訪問 list_head 而不是 slab
      struct slab *next_s = 0;
      struct slab *prev_s = 0;
      
      if (s->slab_list.next != &cache->full) {
        next_s = list_entry(s->slab_list.next, struct slab, slab_list);
      }
      
      if (s->slab_list.prev != &cache->full) {
        prev_s = list_entry(s->slab_list.prev, struct slab, slab_list);
      }
      
      debug("[SLAB]        [ slab %p ] { freelist: %p, in_use: %d, prev: %p, nxt: %p }\n", 
            s, s->freelist, s->inuse, prev_s, next_s);
      
      // 計算物件開始的位置 - 8 位元組對齊
      char *obj_space = (char *)s + sizeof(struct slab);
      obj_space = (char *)(((uint64)obj_space + 7) & ~7);
      
      for (int i = 0; i < s->total; i++) {
        void *obj_addr = obj_space + i * cache->object_size;
        void *as_ptr = *(void **)obj_addr;
        
        debug("[SLAB]           [ idx %d ] { addr: %p, as_ptr: %p, as_obj: {", i, obj_addr, as_ptr);
        if (slab_obj_printer) {
          slab_obj_printer(obj_addr);
        }
        debug("} }\n");
      }
    }
  }

  // Print free slabs
  if (!list_empty(&cache->free)) {
    debug("[SLAB]    [ free slabs ]\n");
    struct slab *s;
    list_for_each_entry(s, &cache->free, slab_list) {
      // 獲取下一個和上一個 slab，需要小心處理，以避免訪問 list_head 而不是 slab
      struct slab *next_s = 0;
      struct slab *prev_s = 0;
      
      if (s->slab_list.next != &cache->free) {
        next_s = list_entry(s->slab_list.next, struct slab, slab_list);
      }
      
      if (s->slab_list.prev != &cache->free) {
        prev_s = list_entry(s->slab_list.prev, struct slab, slab_list);
      }
      
      debug("[SLAB]        [ slab %p ] { freelist: %p, in_use: %d, prev: %p, nxt: %p }\n", 
            s, s->freelist, s->inuse, prev_s, next_s);
      
      // 計算物件開始的位置 - 8 位元組對齊
      char *obj_space = (char *)s + sizeof(struct slab);
      obj_space = (char *)(((uint64)obj_space + 7) & ~7);
      
      for (int i = 0; i < s->total; i++) {
        void *obj_addr = obj_space + i * cache->object_size;
        void *as_ptr = *(void **)obj_addr;
        
        debug("[SLAB]           [ idx %d ] { addr: %p, as_ptr: %p, as_obj: {", i, obj_addr, as_ptr);
        if (slab_obj_printer) {
          slab_obj_printer(obj_addr);
        }
        debug("} }\n");
      }
    }
  }

  debug("[SLAB] print_kmem_cache end\n");
  release(&cache->lock);
}

struct kmem_cache *kmem_cache_create(char *name, uint object_size)
{
  struct kmem_cache *cache = (struct kmem_cache *)kalloc();
  if (!cache) {
    return 0;
  }

  strncpy(cache->name, name, sizeof(cache->name) - 1);
  cache->name[sizeof(cache->name) - 1] = '\0';
  cache->object_size = object_size;
  initlock(&cache->lock, name);

  // 初始化 list_head
  INIT_LIST_HEAD(&cache->partial);
  INIT_LIST_HEAD(&cache->full);
  INIT_LIST_HEAD(&cache->free);
  
  cache->num_slabs = 0;
  cache->slab_size = PGSIZE;

  int slab_metadata_size = sizeof(struct slab);
  cache->num_objects_per_slab = calc_num_objects(PGSIZE, object_size, slab_metadata_size);

  // Initialize in-cache objects
  cache->in_cache_obj_capacity = (PGSIZE - sizeof(struct kmem_cache)) / object_size;
  cache->in_cache_obj_used = 0;
  cache->in_cache_freelist = 0;

  // Initialize freelist for in-cache objects
  if (cache->in_cache_obj_capacity > 0) {
    char *obj_space = (char *)cache + sizeof(struct kmem_cache);
    struct run *last = 0;
    
    for (int i = 0; i < cache->in_cache_obj_capacity; i++) {
      struct run *r = (struct run *)(obj_space + i * object_size);
      if (last) {
        last->next = r;
      } else {
        cache->in_cache_freelist = r;
      }
      last = r;
    }
    
    if (last) {
      last->next = 0; // Mark the end of the freelist
    }
  }
  
  // 將 cache 加入全域追蹤列表
  if (num_caches < MAX_CACHES) {
    all_caches[num_caches++] = cache;
  }

  debug("[SLAB] New kmem_cache (name: %s, object size: %d bytes, at: %p, max objects per slab: %d, support in cache obj: %d) is created\n",
        cache->name, cache->object_size, cache, cache->num_objects_per_slab, cache->in_cache_obj_capacity);
  
  return cache;
}

void kmem_cache_destroy(struct kmem_cache *cache)
{
  if (!cache) {
    return;
  }

  acquire(&cache->lock);

  // Free all slabs in all lists
  struct slab *s, *tmp;
  
  // Free partial slabs
  list_for_each_entry_safe(s, tmp, &cache->partial, slab_list) {
    list_del(&s->slab_list);
    kfree((void *)s);
  }

  // Free full slabs
  list_for_each_entry_safe(s, tmp, &cache->full, slab_list) {
    list_del(&s->slab_list);
    kfree((void *)s);
  }

  // Free free slabs
  list_for_each_entry_safe(s, tmp, &cache->free, slab_list) {
    list_del(&s->slab_list);
    kfree((void *)s);
  }
  
  // 從全域追蹤列表中移除 cache
  for (int i = 0; i < num_caches; i++) {
    if (all_caches[i] == cache) {
      // 移動後面的元素向前
      for (int j = i; j < num_caches - 1; j++) {
        all_caches[j] = all_caches[j + 1];
      }
      num_caches--;
      break;
    }
  }

  release(&cache->lock);

  kfree((void *)cache);
}

void *kmem_cache_alloc(struct kmem_cache *cache)
{
  if (!cache) {
    return 0;
  }

  debug("[SLAB] Alloc request on cache %s\n", cache->name);
  
  acquire(&cache->lock);

  // First try to allocate from in-cache objects
  if (cache->in_cache_freelist) {
    struct run *r = cache->in_cache_freelist;
    cache->in_cache_freelist = r->next;
    cache->in_cache_obj_used++;
    
    debug("[SLAB] Object %p in slab %p (%s) is allocated and initialized\n", r, cache, cache->name);
    
    release(&cache->lock);
    return (void *)r;
  }

  // If no in-cache objects available, try to get from partial list
  struct slab *s = 0;
  
  if (!list_empty(&cache->partial)) {
    // Get the first slab from partial list
    s = list_first_entry(&cache->partial, struct slab, slab_list);
  } else if (!list_empty(&cache->free)) {
    // Get the first slab from free list
    s = list_first_entry(&cache->free, struct slab, slab_list);
    // Move from free to partial list
    list_move(&s->slab_list, &cache->partial);
  } else {
    // Create a new slab
    s = create_slab(cache);
    if (!s) {
      release(&cache->lock);
      return 0;
    }
    // Add to partial list
    list_add(&s->slab_list, &cache->partial);
    cache->num_slabs++;
  }

  // Allocate an object from the slab
  struct run *r = s->freelist;
  if (!r) {
    // This should not happen if our accounting is correct
    release(&cache->lock);
    return 0;
  }
  
  // Remove from freelist
  s->freelist = r->next;
  s->inuse++;

  // If slab is now full, move it to full list
  if (s->inuse == s->total) {
    list_move(&s->slab_list, &cache->full);
  }

  debug("[SLAB] Object %p in slab %p (%s) is allocated and initialized\n", r, s, cache->name);
  
  release(&cache->lock);
  return (void *)r;
}

void kmem_cache_free(struct kmem_cache *cache, void *obj)
{
  if (!cache || !obj) {
    return;
  }
  
  acquire(&cache->lock);

  // Check if object is in-cache
  if ((uint64)obj >= (uint64)cache && (uint64)obj < (uint64)cache + PGSIZE) {
    // This is an in-cache object
    struct run *r = (struct run *)obj;
    r->next = cache->in_cache_freelist;
    cache->in_cache_freelist = r;
    cache->in_cache_obj_used--;
    
    debug("[SLAB] Free %p in slab %p (%s)\n", obj, cache, cache->name);
    debug("[SLAB] Object is from in-cache\n");
    debug("[SLAB] End of free\n");
    
    release(&cache->lock);
    return;
  }

  // Not an in-cache object, handle regularly
  struct slab *s = find_slab(cache, obj);
  if (!s) {
    debug("[SLAB] Error: Object %p does not belong to cache %s\n", obj, cache->name);
    release(&cache->lock);
    return;
  }
  
  // Slab state before
  char *slab_type_before;
  if (s->inuse == s->total) {
    slab_type_before = "full";
  } else if (s->inuse > 0) {
    slab_type_before = "partial";
  } else {
    slab_type_before = "free";
  }

  debug("[SLAB] Free %p in slab %p (%s)\n", obj, s, cache->name);
  debug("[SLAB] Slab state before freeing: %s\n", slab_type_before);

  // 計算物件開始的位置 - 8 位元組對齊
  char *obj_space = (char *)s + sizeof(struct slab);
  obj_space = (char *)(((uint64)obj_space + 7) & ~7);
  
  // Put object back to freelist
  struct run *r = (struct run *)obj;
  r->next = s->freelist;
  s->freelist = r;
  s->inuse--;

  // Update slab list based on new state
  if (s->inuse == 0) {
    // Slab is now empty, move to free list
    list_move(&s->slab_list, &cache->free);
  } else if (s->inuse == s->total - 1) {
    // Slab was full, now partial
    list_move(&s->slab_list, &cache->partial);
  }

  // Slab state after
  char *slab_type_after;
  if (s->inuse == 0) {
    slab_type_after = "free";
  } else if (s->inuse == s->total) {
    slab_type_after = "full";
  } else {
    slab_type_after = "partial";
  }

  debug("[SLAB] Slab state after freeing: %s\n", slab_type_after);

  // Check if we have too many available slabs
  int avail_slabs = count_slabs(&cache->partial) + count_slabs(&cache->free);

  // If too many available slabs and this slab is empty, free it
  if (avail_slabs > MP2_MIN_AVAIL_SLAB && s->inuse == 0) {
    debug("[SLAB] slab %p (%s) is freed due to save memory\n", s, cache->name);
    
    // Remove from list and free slab
    list_del(&s->slab_list);
    kfree((void *)s);
    cache->num_slabs--;
  }

  debug("[SLAB] End of free\n");
  
  release(&cache->lock);
}

// sys_printfslab for user program `printfslab`
uint64 sys_printfslab(void)
{
  print_kmem_cache(file_cache, fileprint_metadata);
  return 0;
}