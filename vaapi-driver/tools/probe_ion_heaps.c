/*
 * Probe_ion_heads --list header for /dev/ ion and try distributing
 *
 * Why does it need to be (round 84): Round 81 I decided "nabul's Ion is not available"
 * The basis is that heap_id_mask tried from 1 to 0x80 by location. That conclusion was wrong.
 * The device is 25 (mask = 1 < 25), not in the field of exploration.
 *
 * Ion_IOC_HEAP_QUERY can ask it directly, without any guessing. Actual nabu (kernel 4.14):
 *     9 head:qsecom(27) user_contig(26) system(25) add(22)
 *                qsecom_ta(19) secure_carveout(14) spss(13)
 *                secure_display(10) secure_heap(9)
 * id=25 distribution succeeded, and the Android side root is consistent with the normal user results in the container.
 *
 * Another lesson: The difference between ENODEV and EINVAL is informative.
 * Say "Ioctl, just no header," "legacy ABI" returns EINVAL
 * It says "ABI is not right." I lost this clue when I thought both were "unsupported."
 *
 * cc-O2-static-o probe_ion_heads probe_departs.c
 *   (-static easy to run adb push to Android side contrast)
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdint.h>
/* The mother Ion provides Ion_IOC_HEAP_QUERY list of heaps, which is distributed on request. */
struct ion_heap_data { char name[32]; uint32_t type; uint32_t heap_id; uint32_t reserved0,reserved1,reserved2; };
struct ion_heap_query { uint32_t cnt; uint32_t reserved0; uint64_t heaps; uint32_t reserved1,reserved2; };
#define ION_IOC_HEAP_QUERY _IOWR('I',8,struct ion_heap_query)
struct ion_alloc_modern { uint64_t len; uint32_t heap_id_mask; uint32_t flags; uint32_t fd; uint32_t unused; };
#define ION_IOC_ALLOC_M _IOWR('I',0,struct ion_alloc_modern)
int main(void){
    int fd=open("/dev/ion",O_RDONLY|O_CLOEXEC);
    if(fd<0){printf("  open failed\n");return 1;}
    struct ion_heap_query q; memset(&q,0,sizeof q);
    if(ioctl(fd,ION_IOC_HEAP_QUERY,&q)!=0){
        printf("  HEAP_QUERY(Count) failed: %s\n",strerror(errno));
    } else {
        printf("  heap count = %u\n", q.cnt);
        if(q.cnt && q.cnt<64){
            struct ion_heap_data hd[64]; memset(hd,0,sizeof hd);
            q.heaps=(uint64_t)(uintptr_t)hd;
            if(ioctl(fd,ION_IOC_HEAP_QUERY,&q)==0){
                for(unsigned i=0;i<q.cnt;i++)
                    printf("    heap[%u] id=%u type=%u name=%s\n",i,hd[i].heap_id,hd[i].type,hd[i].name);
                /* ids are assigned directly */
                for(unsigned i=0;i<q.cnt;i++){
                    struct ion_alloc_modern a; memset(&a,0,sizeof a);
                    a.len=1<<20; a.heap_id_mask=1u<<hd[i].heap_id;
                    if(ioctl(fd,ION_IOC_ALLOC_M,&a)==0){
                        printf("  ✓ Distribution Success heap=%s id=%u dmabuf_fd=%u\n",hd[i].name,hd[i].heap_id,a.fd);
                        close(a.fd); close(fd); return 0;
                    }
                }
                printf("  All we got. heap All of them have failed.: %s\n",strerror(errno));
            } else printf("  HEAP_QUERY(Data) failed: %s\n",strerror(errno));
        }
    }
    close(fd); return 1;
}
