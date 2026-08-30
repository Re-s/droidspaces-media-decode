/*
 * probe_ion_heaps —— 枚举 /dev/ion 的 heap 并试分配
 *
 * 为什么需要它（第 84 轮）：第 81 轮我判定"nabu 的 ION 不可用"，
 * 依据是把 heap_id_mask 从 1 逐位试到 0x80 全部失败。那个结论是错的 ——
 * 这台设备的 system heap id 是 25（mask = 1<<25），不在试探范围内。
 *
 * ION_IOC_HEAP_QUERY 能直接问出来，不必猜。实测 nabu（内核 4.14）：
 *     9 个 heap：qsecom(27) user_contig(26) system(25) adsp(22)
 *                qsecom_ta(19) secure_carveout(14) spss(13)
 *                secure_display(10) secure_heap(9)
 * 用 id=25 分配成功，Android 侧 root 与容器内普通用户结果一致。
 *
 * 另一条教训：ENODEV 与 EINVAL 的区别是有信息的。modern ABI 返回 ENODEV
 * 表示"内核认识这个 ioctl，只是没有匹配的 heap"，legacy ABI 返回 EINVAL
 * 表示"ABI 布局不对"。当时我把两者都当成"不支持"，丢掉了这条线索。
 *
 * 编译：cc -O2 -static -o probe_ion_heaps probe_ion_heaps.c
 *   （-static 便于 adb push 到 Android 侧对照运行）
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdint.h>
/* modern ION 提供 ION_IOC_HEAP_QUERY 枚举 heap，先问再分配。 */
struct ion_heap_data { char name[32]; uint32_t type; uint32_t heap_id; uint32_t reserved0,reserved1,reserved2; };
struct ion_heap_query { uint32_t cnt; uint32_t reserved0; uint64_t heaps; uint32_t reserved1,reserved2; };
#define ION_IOC_HEAP_QUERY _IOWR('I',8,struct ion_heap_query)
struct ion_alloc_modern { uint64_t len; uint32_t heap_id_mask; uint32_t flags; uint32_t fd; uint32_t unused; };
#define ION_IOC_ALLOC_M _IOWR('I',0,struct ion_alloc_modern)
int main(void){
    int fd=open("/dev/ion",O_RDONLY|O_CLOEXEC);
    if(fd<0){printf("  open 失败\n");return 1;}
    struct ion_heap_query q; memset(&q,0,sizeof q);
    if(ioctl(fd,ION_IOC_HEAP_QUERY,&q)!=0){
        printf("  HEAP_QUERY(计数) 失败: %s\n",strerror(errno));
    } else {
        printf("  heap 数量 = %u\n", q.cnt);
        if(q.cnt && q.cnt<64){
            struct ion_heap_data hd[64]; memset(hd,0,sizeof hd);
            q.heaps=(uint64_t)(uintptr_t)hd;
            if(ioctl(fd,ION_IOC_HEAP_QUERY,&q)==0){
                for(unsigned i=0;i<q.cnt;i++)
                    printf("    heap[%u] id=%u type=%u name=%s\n",i,hd[i].heap_id,hd[i].type,hd[i].name);
                /* 用查到的 id 直接分配 */
                for(unsigned i=0;i<q.cnt;i++){
                    struct ion_alloc_modern a; memset(&a,0,sizeof a);
                    a.len=1<<20; a.heap_id_mask=1u<<hd[i].heap_id;
                    if(ioctl(fd,ION_IOC_ALLOC_M,&a)==0){
                        printf("  ✓ 分配成功 heap=%s id=%u dmabuf_fd=%u\n",hd[i].name,hd[i].heap_id,a.fd);
                        close(a.fd); close(fd); return 0;
                    }
                }
                printf("  所有查到的 heap 都分配失败: %s\n",strerror(errno));
            } else printf("  HEAP_QUERY(取数据) 失败: %s\n",strerror(errno));
        }
    }
    close(fd); return 1;
}
