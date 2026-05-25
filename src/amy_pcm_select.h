#ifndef AMY_PCM_SET
#define AMY_PCM_SET 0   // 0 = pcm_small (808, default), 1 = pcm_custom1, 2 = pcm_custom2
#endif

#if AMY_PCM_SET == 1
  #include "pcm_custom1.h"
#elif AMY_PCM_SET == 2
  #include "pcm_custom2.h"
#else
  //#include "pcm_tiny.h" //EKOPARTYYYY
  #include "pcm_small.h"
#endif
