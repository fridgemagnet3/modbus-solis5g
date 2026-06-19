#ifndef JSON_HEAP_H

#define JSON_HEAP_H

#include <stdint.h>
#include <cJSON.h>

// simple class that implements a heap designed to sit on the stack
// for use by the cJSON library
//

#define HEAP_SIZE 2048

class JsonHeap
{
  public:

    JsonHeap() : Ptr(Heap),Remain(HEAP_SIZE)
    {
      cJSON_Hooks Hooks ;

      Instance = this ;
      Hooks.malloc_fn = Alloc ;
      Hooks.free_fn = Delete ;
      cJSON_InitHooks(&Hooks);
    }
    
    JsonHeap(JsonHeap const&) = delete;
    void operator=(JsonHeap const&) = delete;
    
  private:

    static void *Alloc(size_t Sz)
    {
      return Instance->IntAlloc(Sz) ;
    }
              
    static void Delete(void *Ptr)
    {
    }

    void *IntAlloc(size_t Sz)
    {
      uint8_t *Ret = nullptr ;
      
      if ( Sz < Remain )
      {
        Ret = Ptr ;
        Remain-=Sz ;
        Ptr+=Sz ;
      }
      return Ret ;
    }
  
    uint8_t Heap[HEAP_SIZE] ;
    uint8_t *Ptr ;
    uint32_t Remain ;

    static JsonHeap *Instance ;  
} ;

JsonHeap *JsonHeap::Instance;

#endif
