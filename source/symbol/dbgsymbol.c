#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dbgsymbol.h"

#include "../memutils.h"

void add_symbol_to_entry(struct dbg_sym_entry *entry, int idx, int strtabidx,
        unsigned long vmaddr_start, unsigned long vmaddr_end){
    //return;
    if(!entry)
        return;

    //printf("%s: adding symbol with strtab idx %#x with addr %#lx at idx %d\n",
      //      __func__, strtabidx, vmaddr_start, idx);

    if(!entry->syms)
        entry->syms = malloc(sizeof(struct sym *) * ++entry->cursymarrsz);
    else{
        struct sym **sym_rea = realloc(entry->syms,
                sizeof(struct sym *) * ++entry->cursymarrsz);
        //printf("%s: sym_rea %p current array sz %d\n", __func__, sym_rea,
          //      entry->cursymarrsz);
        entry->syms = sym_rea;
    }
//    printf("%s: entry->syms %p sym arr size %d\n", __func__,
  //          entry->syms, entry->cursymarrsz);

    //entry->syms[idx] = malloc(sizeof(struct sym));
    entry->syms[entry->cursymarrsz - 1] = malloc(sizeof(struct sym));
    
    
    //printf("%s: sizeof(**entry->syms) = %zu\n", __func__, sizeof(**entry->syms));
    //printf("%s: entry->syms[%d] = %p\n", __func__, idx, entry->syms[idx]);
    //entry->syms[idx]->symname = strdup(symname);
    //entry->syms[entry->cursymarrsz - 1]->strtab_fileaddr = entry->strtab_fileaddr;
    entry->syms[entry->cursymarrsz - 1]->strtabidx = strtabidx;
    entry->syms[entry->cursymarrsz - 1]->symaddr_start = vmaddr_start;
    entry->syms[entry->cursymarrsz - 1]->symaddr_end = vmaddr_end;
    //entry->syms[entry->cursymarrsz] = NULL;
}

struct dbg_sym_entry *create_sym_entry(char *imagename,
        unsigned long strtab_vmaddr, unsigned long strtab_fileaddr,
        int from_dsc){
    struct dbg_sym_entry *entry = malloc(sizeof(struct dbg_sym_entry));

    entry->imagename = strdup(imagename);
    entry->cursymarrsz = 0;
    entry->strtab_vmaddr = strtab_vmaddr;
    entry->strtab_fileaddr = strtab_fileaddr;
    entry->syms = NULL;
    entry->from_dsc = from_dsc;

    return entry;
}

int get_symbol_info_from_address(struct linkedlist *symlist,
        unsigned long vmaddr, char **imgnameout, char **symnameout,
        unsigned int *distfromsymstartout){
    struct dbg_sym_entry *best_entry = NULL;
    int best_symbol_idx = 0;

    unsigned long diff = 0;

    for(struct node_t *current = symlist->front;
            current;
            current = current->next){
        struct dbg_sym_entry *entry = current->data;

        for(int i=0; i<entry->cursymarrsz/*numsyms*/; i++){
            //printf("%s: entry->syms[i] %p\n", __func__, entry->syms[i]);
            /*
            if(strcmp(entry->imagename, "/private/var/mobile/testprogs/./params") == 0){
                int len = 64;
                char symname[len];
                memset(symname, 0, len);

                kern_return_t kret =
                    read_memory_at_location(entry->strtab_vmaddr + entry->syms[i]->strtabidx,
                            symname, len);
                            */
/*
                printf("%s: sym name '%s' entry->syms[i]->symaddr_start %#lx "
                        " entry->syms[i]->symaddr_end %#lx\n",
                        __func__, symname, entry->syms[i]->symaddr_start,
                        entry->syms[i]->symaddr_end);
            }
            */
            
            if(!(vmaddr >= entry->syms[i]->symaddr_start &&
                        vmaddr < entry->syms[i]->symaddr_end)){
                continue;
            }

            /*
            printf("%s: vmaddr %#lx vmaddr - entry->syms[i]->symaddr_start %#lx"
                    " diff %#lx\n",
                    __func__, vmaddr, vmaddr - entry->syms[i]->symaddr_start, diff);
    */
            if(diff == 0 || vmaddr - entry->syms[i]->symaddr_start < diff){
                best_entry = entry;
                best_symbol_idx = i;
                
                diff = vmaddr - entry->syms[i]->symaddr_start;

                //printf("****%s: updated best entry %p and idx %d\n",
                  //      __func__, best_entry, best_symbol_idx);
            }


        /*    
            printf("%s: potential: symaddr_start %#lx symaddr_end %#lx "
                   " strtabidx %#x",
                   __func__, entry->syms[i]->symaddr_start,
                   entry->syms[i]->symaddr_end, entry->syms[i]->strtabidx);
            enum { len = 512 };
            char symname[len] = {0};

            if(entry->from_dsc){
                FILE *dscfptr =
                    fopen("/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64", "rb");

                if(!dscfptr){
                    printf("%s: couldn't open shared cache\n", __func__);
                    return 1;
                }

                struct sym *cursym = entry->syms[i];
                unsigned long using_strtab = entry->strtab_fileaddr;

                if(cursym->dsc_use_stroff)
                    using_strtab = cursym->stroff_fileaddr;

                unsigned long file_stroff =
                    using_strtab + cursym->strtabidx;

                fseek(dscfptr, file_stroff, SEEK_SET);
                fread(symname, sizeof(char), len, dscfptr);
                symname[len - 1] = '\0';

                printf(": symname '%s'\n", symname);

                fclose(dscfptr);
            }
            else{
                printf("\n");
            }
            */         
            /*
            int len = 64;
            char symname[len];
            memset(symname, 0, len);

            kern_return_t kret =
                read_memory_at_location(entry->strtab_vmaddr + entry->syms[i]->strtabidx,
                    symname, len);

            //printf("%s: read_memory_at_location says %s\n", __func__,
              //      mach_error_string(kret));

            *imgnameout = strdup(entry->imagename);
            *symnameout = strdup(symname);
            *distfromsymstartout = vmaddr - entry->syms[i]->symaddr_start;
            */
            //if(entry->syms[i]->symaddr_start == 0x1c3944b90)
              //  return 0;
            //return 0;
        }
    }

    if(!best_entry)
        return 1;

    /*
    int len = 64;
    char symname[len];
    memset(symname, 0, len);
    */

    enum { len = 512 };
    char symname[len] = {0};

    if(best_entry->from_dsc){
        FILE *dscfptr =
            fopen("/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64", "rb");

        if(!dscfptr){
            printf("%s: couldn't open shared cache\n", __func__);
            return 1;
        }

        struct sym *best_sym = best_entry->syms[best_symbol_idx];
        unsigned long using_strtab = best_entry->strtab_fileaddr;

        if(best_sym->dsc_use_stroff)
            using_strtab = best_sym->stroff_fileaddr;
        
        unsigned long file_stroff =
            using_strtab + best_sym->strtabidx;

        fseek(dscfptr, file_stroff, SEEK_SET);
        fread(symname, sizeof(char), len, dscfptr);
        symname[len - 1] = '\0';

        printf("%s: got symname '%s' from file offset %#lx\n", __func__, symname,
                file_stroff);

        fclose(dscfptr);
    }
    else{
        kern_return_t kret =
            read_memory_at_location(best_entry->strtab_vmaddr +
                    best_entry->syms[best_symbol_idx]->strtabidx, symname, len);

        printf("%s: kret %s for addr %#lx\n", __func__, mach_error_string(kret),
                best_entry->strtab_vmaddr + best_entry->syms[best_symbol_idx]->strtabidx);
    }

    *imgnameout = strdup(best_entry->imagename);
    *symnameout = strdup(symname);
    *distfromsymstartout = vmaddr - best_entry->syms[best_symbol_idx]->symaddr_start;

    return 0;
}
