/* Kyty's Xbox pager (1303170): a cached temporary LocalState file, pressure
 * headroom, guest-code pinning, and protection-preserving eviction. Page-in fills
 * a bounded shared frame through an alias before publishing its guest views. */
#include <fileapifromapp.h>
#include <winioctl.h>


/* bbport: kernel32 forwarder for the process memory counters (no psapi import needed). */
extern BOOL WINAPI K32GetProcessMemoryInfo(HANDLE,PROCESS_MEMORY_COUNTERS *,DWORD);
extern uint64_t uncharted_uwp_app_memory_usage(void);
extern uint64_t uncharted_uwp_app_memory_limit(void);
extern void uncharted_uwp_pager_thread_init(void);
/* Independent liveness channel (uwp_heap_check.cpp): a stage mark written to its own file. */
extern void bb_heartbeat_mark(unsigned stage);
extern void bb_heartbeat_text(const char *text);
/* bbport: the raw marker channel writes with its own handle and its own critical section. The pager
 * now reports through it: printf takes the CRT stream lock, and when another thread blocks inside
 * printf holding that lock every printer in the process stops — which is exactly what was measured
 * (pager marks 1,2,3 then 86 s of silence while the process stayed alive). */
extern void bb_log_raw(const char *text);
static void uwp_report(const char *line) {
    bb_log_raw(line);
}
/* Renderer-DLL side (gnmdriver.cpp). The weak definition in milestone_services.c keeps the
 * executable, which has no GNM driver, linkable. */
extern unsigned bbgpu_gnm_stats(char *out,unsigned capacity);
static HANDLE uwp_swap_file=INVALID_HANDLE_VALUE,uwp_swap_section;
static unsigned char *uwp_swap_alias,*uwp_swap_frames;
static uint64_t uwp_swap_frame_count;
static unsigned char uwp_swap_copy[16384];
static uint64_t uwp_swap_out,uwp_swap_in,uwp_swap_disk_pages,uwp_swap_views;
static uint64_t uwp_swap_hash_checks,uwp_swap_hash_errors,uwp_swap_view_evictions;
static uint64_t uwp_swap_view_limit=UINT64_C(64)*1024*1024/PAGE;
static size_t uwp_swap_cursor,uwp_swap_view_cursor;
static int uwp_swap_disabled,uwp_swap_started;
static uint64_t uwp_swap_page_out_locked(uint64_t bytes,UwpPage *exclude,int include_views);

static uint64_t uwp_swap_hash(const void *data) {
    const uint64_t *words=data;
    uint64_t hash=UINT64_C(0xcbf29ce484222325);
    for(size_t i=0;i<PAGE/sizeof(*words);++i) hash=(hash^words[i])*UINT64_C(0x100000001b3);
    return hash;
}
static int uwp_swap_open(void) {
    if(uwp_swap_section) return 1;
    if(uwp_swap_disabled) return 0;
    const char *path=getenv("BB_UWP_SWAP_FILE");
    if(!path||!*path) return 0;
    wchar_t wide[32768];
    if(!MultiByteToWideChar(CP_UTF8,0,path,-1,wide,32768)) return 0;
    CREATEFILE2_EXTENDED_PARAMETERS params={0};
    params.dwSize=sizeof(params);
    params.dwFileAttributes=FILE_ATTRIBUTE_TEMPORARY;
    params.dwFileFlags=FILE_FLAG_DELETE_ON_CLOSE|FILE_FLAG_RANDOM_ACCESS;
    uwp_swap_file=CreateFile2FromAppW(wide,GENERIC_READ|GENERIC_WRITE,0,CREATE_ALWAYS,&params);
    DWORD done=0;
    if(uwp_swap_file==INVALID_HANDLE_VALUE ||
       !DeviceIoControl(uwp_swap_file,FSCTL_SET_SPARSE,NULL,0,NULL,0,&done,NULL)) goto failed;
    uwp_swap_section=CreateFileMappingFromApp(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE|SEC_RESERVE,uwp_swap_view_limit*PAGE,NULL);
    if(!uwp_swap_section) goto failed;
    uwp_swap_alias=MapViewOfFile3FromApp(uwp_swap_section,GetCurrentProcess(),NULL,0,
        uwp_swap_view_limit*PAGE,0,PAGE_READWRITE,NULL,0);
    if(!uwp_swap_alias) goto failed;
    uwp_swap_frames=calloc((size_t)uwp_swap_view_limit,1);
    if(!uwp_swap_frames) goto failed;
    printf("UWP swap: temporary sparse disk backing ready; %" PRIu64 " MiB mapped-window limit\n",uwp_swap_view_limit*PAGE>>20);
    return 1;
failed:
    printf("UWP swap: unavailable, error %lu\n",GetLastError());
    if(uwp_swap_alias) UnmapViewOfFile(uwp_swap_alias);
    if(uwp_swap_section) CloseHandle(uwp_swap_section);
    free(uwp_swap_frames);
    uwp_swap_alias=uwp_swap_frames=NULL; uwp_swap_section=NULL;
    if(uwp_swap_file!=INVALID_HANDLE_VALUE) CloseHandle(uwp_swap_file);
    uwp_swap_file=INVALID_HANDLE_VALUE; uwp_swap_disabled=1;
    return 0;
}
static int uwp_swap_transfer(uint64_t physical,void *data,int write) {
    OVERLAPPED offset={0}; DWORD done=0;
    offset.Offset=(DWORD)physical; offset.OffsetHigh=(DWORD)(physical>>32);
    BOOL ok=write ? WriteFile(uwp_swap_file,data,(DWORD)PAGE,&done,&offset)
                  : ReadFile(uwp_swap_file,data,(DWORD)PAGE,&done,&offset);
    return ok && done==PAGE;
}
static void uwp_swap_split(UwpPage *page) {
    if(page->split) return;
    for(unsigned i=0;i<3;++i)
        if(!VirtualFree((void *)(page->address+i*4096),4096,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER))
            uwp_fail("swap placeholder split",page->address);
    page->split=1;
}
static void uwp_swap_coalesce(UwpPage *page) {
    if(!page->split) return;
    if(!VirtualFree((void *)page->address,PAGE,MEM_RELEASE|MEM_COALESCE_PLACEHOLDERS))
        uwp_fail("swap placeholder coalesce",page->address);
    page->split=0;
}
static void uwp_swap_unmap_view(UwpPage *page) {
    UwpPhysical *physical=&uwp_physical[page->physical/PAGE];
    if(physical->frame) {
        DWORD old;
        for(unsigned i=0;i<4;++i) if((page->view_mask&(1u<<i)) && (page->protection[i]&PROT_WRITE))
            if(!VirtualProtectFromApp((void *)(page->address+i*4096),4096,PAGE_READONLY,&old))
                uwp_fail("swap frame write protection",page->address);
        unsigned char *data=uwp_swap_alias+(physical->frame-1)*PAGE;
        physical->hash=uwp_swap_hash(data); physical->hash_valid=1;
        if(!uwp_swap_transfer(page->physical,data,1)) uwp_fail("swap frame write",page->address);
    }
    for(unsigned i=0;i<4;++i) if(page->view_mask&(1u<<i))
        if(!UnmapViewOfFile2(GetCurrentProcess(),(void *)(page->address+i*4096),MEM_PRESERVE_PLACEHOLDER))
            uwp_fail("swap view release",page->address);
    if(page->view_mask) --uwp_swap_views;
    page->view_mask=0;
    if(physical->frame) {
        uwp_swap_frames[physical->frame-1]=0;
        --uwp_swap_frame_count; physical->frame=0;
    }
    page->kind=4;
}
static int uwp_swap_view_room(UwpPage *exclude) {
    for(size_t n=0;uwp_swap_frame_count>=uwp_swap_view_limit && n<uwp_count;++n) {
        if(uwp_swap_view_cursor>=uwp_count) uwp_swap_view_cursor=0;
        UwpPage *page=&uwp_pages[uwp_swap_view_cursor++];
        if(page==exclude || !page->mapped || (page->kind!=3&&page->kind!=4)) continue;
        UwpPhysical *physical=&uwp_physical[page->physical/PAGE];
        if(physical->pinned || !physical->frame) continue;
        physical->hash_valid=0;
        uwp_swap_unmap_view(page);
        ++uwp_swap_view_evictions;
    }
    return uwp_swap_frame_count<uwp_swap_view_limit;
}
static unsigned char *uwp_swap_bytes(uint64_t offset) {
    UwpPhysical *physical=&uwp_physical[offset/PAGE];
    if(!physical->frame) {
        UwpPage *page=uwp_page(physical->owner);
        if(!uwp_swap_section || !uwp_swap_view_room(page)) uwp_fail("swap frame capacity",physical->owner);
        uint64_t frame=0;
        while(frame<uwp_swap_view_limit && uwp_swap_frames[frame]) ++frame;
        if(frame==uwp_swap_view_limit) uwp_fail("swap frame bookkeeping",physical->owner);
        unsigned char *data=uwp_swap_alias+frame*PAGE;
        if(!VirtualAllocFromApp(data,PAGE,MEM_COMMIT,PAGE_READWRITE)) uwp_fail("swap frame commit",physical->owner);
        if(!uwp_swap_transfer(offset,data,0)) uwp_fail("swap frame read",physical->owner);
        if(physical->hash_valid) {
            ++uwp_swap_hash_checks;
            if(uwp_swap_hash(data)!=physical->hash) {
                ++uwp_swap_hash_errors;
                uwp_fail("swap integrity check",physical->owner);
            }
            physical->hash_valid=0;
        }
        uwp_swap_frames[frame]=1; ++uwp_swap_frame_count;
        physical->frame=(unsigned)frame+1; uwp_swap_in+=PAGE;
    }
    return uwp_swap_alias+(physical->frame-1)*PAGE;
}
static int uwp_swap_write_backing(uint64_t offset,uint64_t within,const void *source,uint64_t size) {
    UwpPhysical *physical=&uwp_physical[offset/PAGE];
    unsigned char *data=physical->frame ? uwp_swap_alias+(physical->frame-1)*PAGE : uwp_swap_copy;
    if(!physical->frame && !uwp_swap_transfer(offset,data,0)) return 0;
    memcpy(data+within,source,(size_t)size);
    physical->hash_valid=0;
    return physical->frame || uwp_swap_transfer(offset,data,1);
}
static int uwp_swap_page_in(UwpPage *page) {
    UwpPhysical *physical=&uwp_physical[page->physical/PAGE];
    uwp_swap_bytes(page->physical);
    unsigned before=page->view_mask;
    for(unsigned i=0;i<4;++i) {
        if((page->view_mask&(1u<<i)) || !page->protection[i]) continue;
        for(unsigned attempt=0;;++attempt) {
            if(MapViewOfFile3FromApp(uwp_swap_section,GetCurrentProcess(),(void *)(page->address+i*4096),
                (physical->frame-1)*PAGE+i*4096,4096,MEM_REPLACE_PLACEHOLDER,win_prot(page->protection[i]),NULL,0)) break;
            if(attempt==7 || !uwp_swap_page_out_locked(UINT64_C(16)*1024*1024,page,1))
                uwp_fail("swap page-in",page->address+i*4096);
        }
        page->view_mask|=1u<<i;
    }
    if(!before && page->view_mask) { page->kind=3; ++uwp_swap_views; }
    return before!=page->view_mask;
}
static int uwp_swap_protect(UwpPage *page,unsigned index,int prot) {
    unsigned bit=1u<<index;
    uintptr_t address=page->address+index*4096;
    if(page->view_mask&bit) {
        DWORD old;
        if(!VirtualProtectFromApp((void *)address,4096,win_prot(prot),&old)) {
            // A read-only section view has a read-only maximum. Replace just that
            // subpage with the new permission; its frame bytes are already complete.
            if(!UnmapViewOfFile2(GetCurrentProcess(),(void *)address,MEM_PRESERVE_PLACEHOLDER)) return 0;
            page->view_mask&=~bit;
            if(prot) {
                if(!MapViewOfFile3FromApp(uwp_swap_section,GetCurrentProcess(),(void *)address,
                    (uwp_physical[page->physical/PAGE].frame-1)*PAGE+index*4096,4096,MEM_REPLACE_PLACEHOLDER,win_prot(prot),NULL,0))
                    uwp_fail("swap protection remap",address);
                page->view_mask|=bit;
            }
            if(!page->view_mask) { page->kind=4; --uwp_swap_views; }
        }
    }
    page->protection[index]=prot;
    return 1;
}
static uint64_t uwp_swap_page_out_locked(uint64_t bytes,UwpPage *exclude,int include_views) {
    if(!uwp_swap_open()) return 0;
    uint64_t released=0;
    for(size_t scanned=0;scanned<uwp_count && released<bytes;++scanned) {
        if(uwp_swap_cursor>=uwp_count) uwp_swap_cursor=0;
        UwpPage *page=&uwp_pages[uwp_swap_cursor++];
        if(page==exclude || !page->mapped || !page->pageable || (page->kind!=1&&page->kind!=3)) continue;
        if((page->kind==1 && !page->committed) || (page->kind==3 && !include_views)) continue;
        UwpPhysical *physical=&uwp_physical[page->physical/PAGE];
        if(physical->pinned || physical->references!=1 || physical->section) continue;
        if(page->kind==3) {
            physical->hash_valid=0;
            uwp_swap_unmap_view(page);
            released+=PAGE; uwp_swap_out+=PAGE;
            continue;
        }
        int readable=1;
        for(unsigned i=0;i<4;++i)
            if(!(page->protection[i]&PROT_READ)||(page->protection[i]&PROT_EXEC)) readable=0;
        if(!readable) continue;
        DWORD old;
        for(unsigned i=0;i<4;++i)
            if(!VirtualProtectFromApp((void *)(page->address+i*4096),4096,PAGE_READONLY,&old))
                uwp_fail("swap write protection",page->address);
        physical->hash=uwp_swap_hash((void *)page->address); physical->hash_valid=1;
        if(!uwp_swap_transfer(page->physical,(void *)page->address,1)) uwp_fail("swap private write",page->address);
        if(!VirtualFree((void *)page->address,PAGE,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER))
            uwp_fail("swap private page release",page->address);
        page->placed=0;
        page->kind=4;
        page->committed=0;
        uwp_swap_split(page);
        if(!physical->disk) { physical->disk=1; ++uwp_swap_disk_pages; }
        released+=PAGE; uwp_swap_out+=PAGE;
    }
    return released;
}
static uint64_t uwp_swap_reclaim(uint64_t bytes,int include_views) {
    read_lock();
    UWP_LOCK_TAKE(4);
    uint64_t result=uwp_swap_page_out_locked(bytes,NULL,include_views);
    UWP_LOCK_DROP();
    read_unlock();
    return result;
}
uint64_t runtime_memory_page_out(uint64_t bytes) { return uwp_swap_reclaim(bytes,1); }
static void uwp_swap_report(void) {
    bb_heartbeat_mark(5);
    read_lock();
    /* bbport: never block here forever. This thread is the one that prints every heartbeat, so a
     * blocking acquire turns a lock problem into "the process died with no log". Two seconds of
     * waiting, reported with the holder's thread and site, then the report is skipped. */
    int got=0;
    for(int i=0;i<20;++i) {
        if(UWP_LOCK_TRY(5)) { got=1; break; }
        if((i&1)==0) {
            char line[96];
            snprintf(line,sizeof(line),"pager: lock busy owner=%ld site=%d wait=%dms",
                     uwp_lock_owner,uwp_lock_site,i*100);
            bb_heartbeat_text(line);
        }
        Sleep(100);
    }
    if(!got) {
        bb_heartbeat_text("pager: swap report skipped, lock held");
        read_unlock();
        return;
    }
    bb_heartbeat_mark(6);
    uint64_t resident=0,pinned=0;
    for(size_t i=0;i<uwp_count;++i) {
        const UwpPage *page=&uwp_pages[i];
        if(!page->mapped || !page->pageable) continue;
        if(uwp_physical[page->physical/PAGE].pinned) pinned+=PAGE;
        else if(page->kind==1 && page->committed) resident+=PAGE;
    }
    uint64_t out=uwp_swap_out,in=uwp_swap_in,disk=uwp_swap_disk_pages,views=uwp_swap_views;
    uint64_t checks=uwp_swap_hash_checks,errors=uwp_swap_hash_errors,evictions=uwp_swap_view_evictions;
    UWP_LOCK_DROP(); read_unlock();
    const char *profile=getenv("BB_IO_STATS");
    if(!out&&!(profile&&*profile=='1')) return;
    printf("UWP swap: out=%" PRIu64 " in=%" PRIu64 " disk=%" PRIu64 " views=%" PRIu64
           " MiB; checks=%" PRIu64 " corrupt=%" PRIu64 " window evictions=%" PRIu64 "\n",
           out>>20,in>>20,disk*PAGE>>20,views*PAGE>>20,checks,errors,evictions);
    printf("UWP swap pools: unpinned private=%" PRIu64 " pinned=%" PRIu64 " MiB\n",resident>>20,pinned>>20);
}
static uint64_t uwp_app_peak_bytes;
static uint64_t uwp_app_sample_peak(uint64_t used) {
    uint64_t peak=__atomic_load_n(&uwp_app_peak_bytes,__ATOMIC_RELAXED);
    while(used>peak && !__atomic_compare_exchange_n(&uwp_app_peak_bytes,&peak,used,0,
                                                      __ATOMIC_RELAXED,__ATOMIC_RELAXED)) {}
    return used>peak ? used : peak;
}
static void *uwp_pager(void *unused) {
    (void)unused;
    uncharted_uwp_pager_thread_init();
    uint64_t start=GetTickCount64(),reported=start;
    int draining=0;
    for(;;) {
        /* bbport: the pager is the only thread that prints the heartbeats (memory sample and the GNM
         * counters). When both stop together it is this loop that blocked, not the log: mark each
         * stage in the independent heartbeat file so the last mark names the call. */
        bb_heartbeat_mark(1);
        uint64_t limit=uncharted_uwp_app_memory_limit(),used=uncharted_uwp_app_memory_usage();
        bb_heartbeat_mark(2);
        uint64_t peak=uwp_app_sample_peak(used);
        uint64_t out=0;
        if(limit>(UINT64_C(1024)<<20)) {
            if(used>limit-(UINT64_C(384)<<20)) draining=1;
            if(used<=limit-(UINT64_C(512)<<20)) draining=0;
            // Shared-frame pages already have a bounded working set. Recycling those
            // repeatedly cannot free more private commit and turns pressure into churn.
            if(draining) out=uwp_swap_reclaim(UINT64_C(2)<<20,0);
        }
        bb_heartbeat_mark(3);
        uint64_t now=GetTickCount64();
        /* bbport: the run grows ~700 MiB/s and ends in ~10 s, so a fixed cadence catches two points at
         * best. Log whenever the peak rises by 64 MiB instead: every step of the growth gets a split,
         * and a single bulk jump shows up as exactly one. */
        static uint64_t peak_logged;
        if(now-reported>=2000 || used>peak_logged+(UINT64_C(64)<<20)) {
            if(used>peak_logged) peak_logged=used;
            char sample[192];
            snprintf(sample,sizeof(sample),
                     "UWP app memory sample: used=%" PRIu64 " peak=%" PRIu64 " limit=%" PRIu64
                     " MiB; elapsed=%" PRIu64 " ms",used>>20,peak>>20,limit>>20,now-start);
            uwp_report(sample);
            /* bbport: the process total says nothing about WHERE it went. These six fields are the
             * guest's own pages, so the gap between them and the total is host-side and attributable. */
            {
                extern void runtime_memory_breakdown(uint64_t out[6]);
                uint64_t split[6];
                runtime_memory_breakdown(split);
                char detail[256];
                snprintf(detail,sizeof(detail),
                         "M5 mem: guest_committed=%" PRIu64 " pinned=%" PRIu64 " section=%" PRIu64
                         " swap_views=%" PRIu64 " swapped_out=%" PRIu64 " reserved=%" PRIu64 " MiB",
                         split[0]>>20,split[1]>>20,split[2]>>20,split[3]>>20,split[4]>>20,split[5]>>20);
                uwp_report(detail);
            }
            /* bbport: the app total and the private commit disagree by ~3.4 GiB. Private vs pagefile vs
             * working set says, in one line, whether that gap is a section (guest backing), a report
             * reading the wrong metric, or ordinary private heap. */
            {
                PROCESS_MEMORY_COUNTERS_EX counters;
                memset(&counters,0,sizeof(counters));
                unsigned long long priv=0,pagefile=0,ws=0;
                if(K32GetProcessMemoryInfo(GetCurrentProcess(),
                                           (PROCESS_MEMORY_COUNTERS *)&counters,sizeof(counters))) {
                    priv=counters.PrivateUsage; pagefile=counters.PagefileUsage;
                    ws=counters.WorkingSetSize;
                }
                char commit[224];
                snprintf(commit,sizeof(commit),
                         "M5 commit: private=%llu pagefile=%llu ws=%llu | app_usage=%llu MiB",
                         priv>>20,pagefile>>20,ws>>20,used>>20);
                uwp_report(commit);
            }
            /* bbport: private commit is ~3.2 GiB above what the VMA and the guest page table account
             * for. Splitting it by type and by guest range says whether it is the guest's own window
             * committed without the table knowing (a bookkeeping bug) or allocations elsewhere. */
            {
                extern void runtime_memory_window(uintptr_t *lo,uintptr_t *hi);
                uintptr_t guest_lo=0,guest_hi=0;
                runtime_memory_window(&guest_lo,&guest_hi);
                uintptr_t at=0;
                unsigned long long sum_total=0,regions=0,last=0;
                unsigned long long h_small=0,h_mid=0,h_big=0,h_huge=0;
                unsigned long long top_size[5]={0,0,0,0,0};
                uintptr_t top_base[5]={0,0,0,0,0};
                unsigned top_type[5]={0,0,0,0,0},top_prot[5]={0,0,0,0,0};
                unsigned long long b_small=0,b_mid=0,b_big=0,b_huge=0;
                unsigned long long priv_in=0,priv_out=0,mapped=0,image=0;
                MEMORY_BASIC_INFORMATION mbi;
                while(VirtualQuery((const void *)at,&mbi,sizeof(mbi))==sizeof(mbi)) {
                    const unsigned long long size=mbi.RegionSize;
                    if(mbi.State==MEM_COMMIT) {
                    if(mbi.State==MEM_COMMIT) sum_total+=size;
                    if(mbi.State==MEM_COMMIT && mbi.Type==MEM_PRIVATE &&
                       ((uintptr_t)mbi.BaseAddress<guest_lo || (uintptr_t)mbi.BaseAddress>=guest_hi)) {
                        if(size<=(UINT64_C(1)<<20)) { ++h_small; b_small+=size; }
                        else if(size<=(UINT64_C(16)<<20)) { ++h_mid; b_mid+=size; }
                        else if(size<=(UINT64_C(64)<<20)) { ++h_big; b_big+=size; }
                        else { ++h_huge; b_huge+=size; }
                        if(size>top_size[4]) {
                            unsigned slot=4;
                            while(slot>0 && top_size[slot-1]<size) { top_size[slot]=top_size[slot-1]; top_base[slot]=top_base[slot-1]; top_type[slot]=top_type[slot-1]; top_prot[slot]=top_prot[slot-1]; --slot; }
                            top_size[slot]=size; top_base[slot]=(uintptr_t)mbi.BaseAddress; top_type[slot]=mbi.Type; top_prot[slot]=mbi.Protect;
                        }
                    }
                    last=(uintptr_t)mbi.BaseAddress+size; ++regions;
                        if(mbi.Type==MEM_PRIVATE) {
                            if((uintptr_t)mbi.BaseAddress>=guest_lo && (uintptr_t)mbi.BaseAddress<guest_hi) priv_in+=size;
                            else priv_out+=size;
                        } else if(mbi.Type==MEM_MAPPED) mapped+=size;
                        else if(mbi.Type==MEM_IMAGE) image+=size;
                    }
                    const uintptr_t next=(uintptr_t)mbi.BaseAddress+size;
                    if(next<=(uintptr_t)mbi.BaseAddress) break;
                    at=next;
                }
                char vq[224];
                snprintf(vq,sizeof(vq),
                         "M5 vq: in_guest=%llu out=%llu mapped=%llu image=%llu total=%llu regions=%llu last=0x%llx MiB",
                         priv_in>>20,priv_out>>20,mapped>>20,image>>20,sum_total>>20,regions,last);
                uwp_report(vq);
                char hist[224];
                snprintf(hist,sizeof(hist),
                         "M5 hist: <1M=%llu/%llu 1-16M=%llu/%llu 16-64M=%llu/%llu >64M=%llu/%llu MiB",
                         h_small,b_small>>20,h_mid,b_mid>>20,h_big,b_big>>20,h_huge,b_huge>>20);
                uwp_report(hist);
                for(unsigned t=0;t<5;++t) {
                    if(!top_size[t]) continue;
                    char one[192];
                    snprintf(one,sizeof(one),"M5 top%u: base=0x%llx size=%llu MiB type=%u prot=0x%x",
                             t,(unsigned long long)top_base[t],top_size[t]>>20,top_type[t],top_prot[t]);
                    uwp_report(one);
                }
            }
            /* bbport: the shape of the commit inside the guest window, before hunting the author: one
             * huge allocation (a single VirtualAlloc over the window), a few big ones (a loop per
             * block), or thousands of small ones (a per-page loop). */
            {
                extern void runtime_memory_window(uintptr_t *lo,uintptr_t *hi);
                uintptr_t guest_lo=0,guest_hi=0;
                runtime_memory_window(&guest_lo,&guest_hi);
                uintptr_t at=0,prev_base=(uintptr_t)-1,big_base=0;
                unsigned bases=0,big_type=0;
                unsigned long long big_size=0;
                MEMORY_BASIC_INFORMATION mbi;
                while(VirtualQuery((const void *)at,&mbi,sizeof(mbi))==sizeof(mbi)) {
                    const unsigned long long size=mbi.RegionSize;
                    if(mbi.State==MEM_COMMIT && (uintptr_t)mbi.BaseAddress>=guest_lo && (uintptr_t)mbi.BaseAddress<guest_hi) {
                        const uintptr_t base=(uintptr_t)mbi.AllocationBase;
                        if(base!=prev_base) { ++bases; prev_base=base; }
                        if(size>big_size) { big_size=size; big_base=base; big_type=mbi.Type; }
                    }
                    const uintptr_t next=(uintptr_t)mbi.BaseAddress+size;
                    if(next<=(uintptr_t)mbi.BaseAddress) break;
                    at=next;
                }
                char va[192];
                snprintf(va,sizeof(va),
                         "M5 valloc: bases=%u biggest_base=0x%llx region=0x%llx type=%u window=0x%llx-0x%llx",
                         bases,(unsigned long long)big_base,(unsigned long long)big_size,big_type,
                         (unsigned long long)guest_lo,(unsigned long long)guest_hi);
                uwp_report(va);
            }
            /* bbport: committed vs released settles whether the 4.6 GiB is real cost or a stale flag. */
            {
                extern volatile long long uwp_committed_bytes,uwp_released_commit_bytes;
                extern volatile long long uwp_hostmap_over_commit;
                extern uint64_t uwp_faults;
                char acc[224];
                snprintf(acc,sizeof(acc),
                         "M5 commits: committed=%llu released=%llu hostmap_over=%llu faults=%llu MiB",
                         (unsigned long long)(__atomic_load_n(&uwp_committed_bytes,__ATOMIC_RELAXED)>>20),
                         (unsigned long long)(__atomic_load_n(&uwp_released_commit_bytes,__ATOMIC_RELAXED)>>20),
                         (unsigned long long)__atomic_load_n(&uwp_hostmap_over_commit,__ATOMIC_RELAXED),
                         (unsigned long long)uwp_faults);
                uwp_report(acc);
            }
            /* bbport: the first gameplay run has to describe itself: how many host-visible device
             * memories the ICD turned into 1 GiB upload heaps, whether the staging pool ever ran out,
             * and how many assets came from the placeholder instead of the dump. */
            {
                extern unsigned BbHostVisibleDevices(void);
                extern unsigned BbStagingPoolExhausted(void);
                extern long long bb_assets_substituted;
                /* Minimal counters-only A/B: no wait, print or histogram in the hot path. */
                extern unsigned BbEopSeen(void);
                extern unsigned BbEopQueued(void);
                extern unsigned BbEopExec(void);
                extern unsigned BbEwSeen(void);
                extern unsigned BbEosReservedIrq(void);
                char wrap[224];
                snprintf(wrap,sizeof(wrap),
                         "M5 wrap: hostvis_devices=%u exhausted=%u assets_substituted=%lld "
                         "eop=%u/%u/%u ew=%u eos_reserved_irq=%u",
                         BbHostVisibleDevices(),BbStagingPoolExhausted(),bb_assets_substituted,
                         BbEopSeen(),BbEopQueued(),BbEopExec(),BbEwSeen(),BbEosReservedIrq());
                uwp_report(wrap);
            }
            uwp_swap_report();
            /* bbport: the guest can sit inside a driver call without printing anything. The counters
             * of the GNM entry points say which one: entered > returned means that call has not come
             * back, and gpu_idle signals are what release sceGnmSubmitDone's wait. */
            if(bbgpu_gnm_stats) {
                char gnm[320];
                if(bbgpu_gnm_stats(gnm,sizeof(gnm))) uwp_report(gnm);
            }
            bb_heartbeat_mark(4);
            reported=now;
        }
        Sleep(out ? 1 : 50);
    }
    return NULL;
}
void runtime_memory_start_paging(void) {
    if(uwp_swap_started) return;
    const char *enabled=getenv("BB_UWP_SWAP");
    if(enabled && *enabled=='0') { uwp_swap_disabled=1; puts("UWP swap: disabled by private setting"); return; }
    runtime_memory_reserve();
    if(!uwp_swap_open()) { puts("UWP swap: pager could not start"); return; }
    pthread_t thread;
    if(pthread_create(&thread,NULL,uwp_pager,NULL)) uwp_fail("pager thread",0);
    pthread_detach(thread); uwp_swap_started=1;
    printf("UWP swap: pager active, app budget=%" PRIu64 " MiB, pressure headroom=384 MiB\n",uncharted_uwp_app_memory_limit()>>20);
}
