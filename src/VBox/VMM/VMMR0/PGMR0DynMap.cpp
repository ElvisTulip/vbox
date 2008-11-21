/* $Id$ */
/** @file
 * PGM - Page Manager and Monitor, ring-0 dynamic mapping cache.
 */

/*
 * Copyright (C) 2008 Sun Microsystems, Inc.
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa
 * Clara, CA 95054 USA or visit http://www.sun.com if you need
 * additional information or have any questions.
 */

/*******************************************************************************
*   Internal Functions                                                         *
*******************************************************************************/
#include <VBox/pgm.h>
#include "../PGMInternal.h"
#include <VBox/vm.h>
#include <VBox/sup.h>
#include <VBox/err.h>
#include <iprt/asm.h>
#include <iprt/alloc.h>
#include <iprt/assert.h>
#include <iprt/cpuset.h>
#include <iprt/memobj.h>
#include <iprt/mp.h>
#include <iprt/spinlock.h>
#include <iprt/semaphore.h>


/*******************************************************************************
*   Defined Constants And Macros                                               *
*******************************************************************************/
/** The max size of the mapping cache (in pages). */
#define PGMR0DYNMAP_MAX_PAGES               ((8*_1M) >> PAGE_SHIFT)
/* * The max segment size. */
/** @todo #define PGMR0DYNMAP_SEG_MAX_PAGES           (_1M >> PAGE_SHIFT) */
/** The number of pages we reserve per CPU. */
#define PGMR0DYNMAP_PAGES_PER_CPU           64
/** Calcs the overload threshold. Current set at 50%. */
#define PGMR0DYNMAP_CALC_OVERLOAD(cPages)   ((cPages) / 2)


/*******************************************************************************
*   Structures and Typedefs                                                    *
*******************************************************************************/
/**
 * Ring-0 dynamic mapping cache segment.
 *
 * The dynamic mapping cache can be extended with additional segments if the
 * load is found to be too high.  This done the next time a VM is created, under
 * the protection of the init mutex.  The arrays is reallocated and the new
 * segment is added to the end of these.  Nothing is rehashed of course, as the
 * indexes / addresses must remain unchanged.
 *
 * This structure is only modified while owning the init mutex or during module
 * init / term.
 */
typedef struct PGMR0DYNMAPSEG
{
    /** Pointer to the next segment. */
    struct PGMR0DYNMAPSEG      *pNext;
    /** The memory object for the virtual address range that we're abusing. */
    RTR0MEMOBJ                  hMemObj;
    /** The start page in the cache. (I.e. index into the arrays.) */
    uint16_t                    iPage;
    /** The number of pages this segment contributes. */
    uint16_t                    cPages;
    /** The number of page tables. */
    uint16_t                    cPTs;
    /** The memory objects for the page tables. */
    RTR0MEMOBJ                  ahMemObjPT[1];
} PGMR0DYNMAPSEG;
/** Pointer to a ring-0 dynamic mapping cache segment. */
typedef PGMR0DYNMAPSEG *PPGMR0DYNMAPSEG;


/**
 * Ring-0 dynamic mapping cache entry.
 *
 * This structure tracks
 */
typedef struct PGMR0DYNMAPENTRY
{
    /** The physical address of the currently mapped page.
     * This is duplicate for three reasons: cache locality, cache policy of the PT
     * mappings and sanity checks.   */
    RTHCPHYS                    HCPhys;
    /** Pointer to the page. */
    void                       *pvPage;
    /** The number of references. */
    int32_t volatile            cRefs;
    /** PTE pointer union. */
    union PGMR0DYNMAPENTRY_PPTE
    {
        /** PTE pointer, 32-bit legacy version. */
        PX86PTE                 pLegacy;
        /** PTE pointer, PAE version. */
        PX86PTEPAE              pPae;
    } uPte;
    /** CPUs that haven't invalidated this entry after it's last update. */
    RTCPUSET                    PendingSet;
} PGMR0DYNMAPENTRY;
/** Pointer to a ring-0 dynamic mapping cache entry. */
typedef PGMR0DYNMAPENTRY *PPGMR0DYNMAPENTRY;


/**
 * Ring-0 dynamic mapping cache.
 *
 * This is initialized during VMMR0 module init but no segments are allocated at
 * that time.  Segments will be added when the first VM is started and removed
 * again when the last VM shuts down, thus avoid consuming memory while dormant.
 * At module termination, the remaining bits will be freed up.
 */
typedef struct PGMR0DYNMAP
{
    /** The usual magic number / eye catcher (PGMR0DYNMAP_MAGIC). */
    uint32_t                    u32Magic;
    /** Spinlock serializing the normal operation of the cache. */
    RTSPINLOCK                  hSpinlock;
    /** Array for tracking and managing the pages.  */
    PPGMR0DYNMAPENTRY           paPages;
    /** The cache size given as a number of pages. */
    uint32_t                    cPages;
    /** Whether it's 32-bit legacy or PAE/AMD64 paging mode. */
    bool                        fLegacyMode;
    /** The current load. */
    uint32_t                    cLoad;
    /** The max load.
     * This is maintained to get trigger adding of more mapping space. */
    uint32_t                    cMaxLoad;
    /** Initialization / termination lock. */
    RTSEMFASTMUTEX              hInitLock;
    /** The number of users (protected by hInitLock). */
    uint32_t                    cUsers;
    /** Array containing a copy of the original page tables.
     * The entries are either X86PTE or X86PTEPAE according to fLegacyMode. */
    void                       *pvSavedPTEs;
    /** List of segments. */
    PPGMR0DYNMAPSEG             pSegHead;
    /** The paging mode. */
    SUPPAGINGMODE               enmPgMode;
} PGMR0DYNMAP;
/** Pointer to the ring-0 dynamic mapping cache */
typedef PGMR0DYNMAP *PPGMR0DYNMAP;

/** PGMR0DYNMAP::u32Magic. (Jens Christian Bugge Wesseltoft) */
#define PGMR0DYNMAP_MAGIC       0x19640201


/*******************************************************************************
*   Global Variables                                                           *
*******************************************************************************/
/** Pointer to the ring-0 dynamic mapping cache. */
static PPGMR0DYNMAP g_pPGMR0DynMap;


/*******************************************************************************
*   Internal Functions                                                         *
*******************************************************************************/
static void pgmR0DynMapReleasePage(PPGMR0DYNMAP pThis, uint32_t iPage, uint32_t cRefs);
static int  pgmR0DynMapSetup(PPGMR0DYNMAP pThis);
static int  pgmR0DynMapGrow(PPGMR0DYNMAP pThis);
static void pgmR0DynMapTearDown(PPGMR0DYNMAP pThis);


/**
 * Initializes the ring-0 dynamic mapping cache.
 *
 * @returns VBox status code.
 */
VMMR0DECL(int) PGMR0DynMapInit(void)
{
#ifndef DEBUG_bird
    return VINF_SUCCESS;
#else
    Assert(!g_pPGMR0DynMap);

    /*
     * Create and initialize the cache instance.
     */
    PPGMR0DYNMAP pThis = (PPGMR0DYNMAP)RTMemAllocZ(sizeof(*pThis));
    AssertLogRelReturn(pThis, VERR_NO_MEMORY);
    int             rc = VINF_SUCCESS;
    pThis->enmPgMode = SUPR0GetPagingMode();
    switch (pThis->enmPgMode)
    {
        case SUPPAGINGMODE_32_BIT:
        case SUPPAGINGMODE_32_BIT_GLOBAL:
            pThis->fLegacyMode = false;
            break;
        case SUPPAGINGMODE_PAE:
        case SUPPAGINGMODE_PAE_GLOBAL:
        case SUPPAGINGMODE_PAE_NX:
        case SUPPAGINGMODE_PAE_GLOBAL_NX:
        case SUPPAGINGMODE_AMD64:
        case SUPPAGINGMODE_AMD64_GLOBAL:
        case SUPPAGINGMODE_AMD64_NX:
        case SUPPAGINGMODE_AMD64_GLOBAL_NX:
            pThis->fLegacyMode = false;
            break;
        default:
            rc = VERR_INTERNAL_ERROR;
            break;
    }
    if (RT_SUCCESS(rc))
    {
        rc = RTSemFastMutexCreate(&pThis->hInitLock);
        if (RT_SUCCESS(rc))
        {
            rc = RTSpinlockCreate(&pThis->hSpinlock);
            if (RT_SUCCESS(rc))
            {
                pThis->u32Magic = PGMR0DYNMAP_MAGIC;
                g_pPGMR0DynMap = pThis;
                return VINF_SUCCESS;
            }
            RTSemFastMutexDestroy(pThis->hInitLock);
        }
    }
    RTMemFree(pThis);
    return rc;
#endif
}


/**
 * Terminates the ring-0 dynamic mapping cache.
 */
VMMR0DECL(void) PGMR0DynMapTerm(void)
{
#ifdef DEBUG_bird
    /*
     * Destroy the cache.
     *
     * There is not supposed to be any races here, the loader should
     * make sure about that. So, don't bother locking anything.
     *
     * The VM objects should all be destroyed by now, so there is no
     * dangling users or anything like that to clean up. This routine
     * is just a mirror image of PGMR0DynMapInit.
     */
    PPGMR0DYNMAP pThis = g_pPGMR0DynMap;
    if (pThis)
    {
        AssertPtr(pThis);
        g_pPGMR0DynMap = NULL;

        AssertLogRelMsg(!pThis->cUsers && !pThis->paPages && !pThis->cPages,
                        ("cUsers=%d paPages=%p cPages=%#x\n",
                         pThis->cUsers, pThis->paPages, pThis->cPages));

        /* Free the associated resources. */
        RTSemFastMutexDestroy(pThis->hInitLock);
        pThis->hInitLock = NIL_RTSEMFASTMUTEX;
        RTSpinlockDestroy(pThis->hSpinlock);
        pThis->hSpinlock = NIL_RTSPINLOCK;
        pThis->u32Magic = UINT32_MAX;
        RTMemFree(pThis);
    }
#endif
}


/**
 * Initializes the dynamic mapping cache for a new VM.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the shared VM structure.
 */
VMMR0DECL(int) PGMR0DynMapInitVM(PVM pVM)
{
#ifndef DEBUG_bird
    return VINF_SUCCESS;
#else
    /*
     * Initialize the auto sets.
     */
    VMCPUID idCpu = pVM->cCPUs;
    while (idCpu-- > 0)
    {
        PPGMMAPSET pSet = &pVM->aCpus[idCpu].pgm.s.AutoSet;
        uint32_t j = RT_ELEMENTS(pSet->aEntries);
        while (j-- > 0)
        {
            pSet->aEntries[j].iPage = UINT16_MAX;
            pSet->aEntries[j].cRefs = 0;
        }
        pSet->cEntries = PGMMAPSET_CLOSED;
    }

    /*
     * Do we need the cache? Skip the last bit if we don't.
     */
    Assert(!pVM->pgm.s.pvR0DynMapUsed);
    pVM->pgm.s.pvR0DynMapUsed = NULL;
    if (!HWACCMIsEnabled(pVM))
        return VINF_SUCCESS;

    /*
     * Reference and if necessary setup or grow the cache.
     */
    PPGMR0DYNMAP pThis = g_pPGMR0DynMap;
    AssertPtrReturn(pThis, VERR_INTERNAL_ERROR);
    int rc = RTSemFastMutexRequest(pThis->hInitLock);
    AssertLogRelRCReturn(rc, rc);

    pThis->cUsers++;
    if (pThis->cUsers == 1)
        rc = pgmR0DynMapSetup(pThis);
    else if (pThis->cMaxLoad > PGMR0DYNMAP_CALC_OVERLOAD(pThis->cPages))
        rc = pgmR0DynMapGrow(pThis);
    if (RT_FAILURE(rc))
        pThis->cUsers--;

    RTSemFastMutexRelease(pThis->hInitLock);

    return rc;
#endif
}


/**
 * Terminates the dynamic mapping cache usage for a VM.
 *
 * @param   pVM         Pointer to the shared VM structure.
 */
VMMR0DECL(void) PGMR0DynMapTermVM(PVM pVM)
{
#ifdef DEBUG_bird
    /*
     * Return immediately if we're not using the cache.
     */
    if (!pVM->pgm.s.pvR0DynMapUsed)
        return;

    PPGMR0DYNMAP pThis = g_pPGMR0DynMap;
    AssertPtrReturnVoid(pThis);

    int rc = RTSemFastMutexRequest(pThis->hInitLock);
    AssertLogRelRCReturnVoid(rc);

    if (pVM->pgm.s.pvR0DynMapUsed == pThis)
    {
        pVM->pgm.s.pvR0DynMapUsed = NULL;

        /*
         * Clean up and check the auto sets.
         */
        VMCPUID idCpu = pVM->cCPUs;
        while (idCpu-- > 0)
        {
            PPGMMAPSET pSet = &pVM->aCpus[idCpu].pgm.s.AutoSet;
            uint32_t j = pSet->cEntries;
            if (j <= RT_ELEMENTS(pSet->aEntries))
            {
                /*
                 * The set is open, close it.
                 */
                while (j-- > 0)
                {
                    int32_t cRefs = pSet->aEntries[j].cRefs;
                    uint32_t iPage = pSet->aEntries[j].iPage;
                    LogRel(("PGMR0DynMapTermVM: %d dangling refs to %#x\n", cRefs, iPage));
                    if (iPage < pThis->cPages && cRefs > 0)
                        pgmR0DynMapReleasePage(pThis, iPage, cRefs);
                    else
                        AssertMsgFailed(("cRefs=%d iPage=%#x cPages=%u\n", cRefs, iPage, pThis->cPages));

                    pSet->aEntries[j].iPage = UINT16_MAX;
                    pSet->aEntries[j].cRefs = 0;
                }
                pSet->cEntries = PGMMAPSET_CLOSED;
            }

            j = RT_ELEMENTS(pSet->aEntries);
            while (j-- > 0)
            {
                Assert(pSet->aEntries[j].iPage == UINT16_MAX);
                Assert(!pSet->aEntries[j].cRefs);
            }
        }

        /*
         * Release our reference to the mapping cache.
         */
        Assert(pThis->cUsers > 0);
        pThis->cUsers--;
        if (!pThis->cUsers)
            pgmR0DynMapTearDown(pThis);
    }
    else
        AssertMsgFailed(("pvR0DynMapUsed=%p pThis=%p\n", pVM->pgm.s.pvR0DynMapUsed, pThis));

    RTSemFastMutexRelease(pThis->hInitLock);
#endif
}


/**
 * Calculate the new cache size based on cMaxLoad statistics.
 *
 * @returns Number of pages.
 * @param   pThis       The dynamic mapping cache instance.
 */
static uint32_t pgmR0DynMapCalcNewSize(PPGMR0DYNMAP pThis)
{
    /*
     * cCpus * PGMR0DYNMAP_PAGES_PER_CPU.
     */
    RTCPUID     cCpus = RTMpGetCount();
    uint32_t    cPages = cCpus * PGMR0DYNMAP_PAGES_PER_CPU;

    /* adjust against cMaxLoad. */
    AssertMsg(pThis->cMaxLoad <= PGMR0DYNMAP_MAX_PAGES, ("%#x\n", pThis->cMaxLoad));
    if (pThis->cMaxLoad > PGMR0DYNMAP_MAX_PAGES)
        pThis->cMaxLoad = 0;

    while (pThis->cMaxLoad < PGMR0DYNMAP_CALC_OVERLOAD(cPages))
        cPages += PGMR0DYNMAP_PAGES_PER_CPU;

    /* adjust against max size. */
    if (cPages > PGMR0DYNMAP_MAX_PAGES)
        cPages = PGMR0DYNMAP_MAX_PAGES;

    return cPages;
}


/**
 * Adds a new segment of the specified size.
 *
 * @returns VBox status code.
 * @param   pThis       The dynamic mapping cache instance.
 * @param   cPages      The size of the new segment, give as a page count.
 */
static int pgmR0DynMapAddSeg(PPGMR0DYNMAP pThis, uint32_t cPages)
{
#if 0
    int rc2;

    /*
     * Do the array rellocation first.
     * (Too lazy to clean these up on failure.)
     */
    void *pv = RTMemRealloc(pThis->paPages, sizeof(pThis->paPages[0]) * (pThis->cPages + cPages));
    if (!pv)
        return VERR_NO_MEMORY;
    pThis->paPages = (PPGMR0DYNMAPENTRY)pv;

    pv = RTMemRealloc(pThis->pvSavedPTEs, (pThis->fLegacyMode ? sizeof(X86PGUINT) : sizeof(X86PGPAEUINT)) * (pThis->cPages + cPages));
    if (!pv)
        return VERR_NO_MEMORY;
    pThis->pvSavedPTEs = pv;

    /*
     * Allocate the segment structure and pages memory.
     */
    uint32_t cPTs = cPages / (pThis->fLegacyMode ? X86_PG_ENTRIES : X86_PG_PAE_ENTRIES) + 2;
    PPGMR0DYNMAPSEG pSeg = RTMemAllocZ(RT_UOFFSETOF(PGMR0DYNMAPSEG, ahMemObjPTs[cPTs]));
    if (!pSeg)
        return VERR_NO_MEMORY;
    pSeg->pNext  = NULL;
    pSeg->cPages = cPages;
    pSeg->iPage  = pThis->cPages;
    pSeg->cPTs   = 0;
    int rc = RTR0MemObjAllocPage(&pSeg->hMemObj, cPages << PAGE_SHIFT, false);
    if (RT_SUCCESS(rc))
    {
        /*
         * Walk the paging hierarchy and map the relevant page tables.
         */
        uint8_t    *pbPage = RTR0MemObjAddress(pSeg->hMemObj);
        AssertMsg(VALID_PTR(pbPage) && !((uintptr_t)pbPage & PAGE_OFFSET_MASK), ("%p\n", pbPage));
        uint32_t    iPage = pThis->cPages;
        uint32_t    iEndPage = iPage + cPages;
        struct
        {
            RTHCPHYS        HCPhys;     /**< The entry that's currently mapped */
            RTHCPHYS        fPhysMask;  /**< Mask for extracting HCPhys from uEntry. */
            RTR0MEMOBJ      hMemObj;
            RTR0MEMOBJ      hMapObj;
            uint64_t        fPtrMask;
            uint32_t        fPtrShift;
            uint64_t        fAndMask;
            uint64_t        fResMask;
            union
            {
                void   *pv;
            } u;
        }           a[4];
        RTCCUINTREG cr4 = ASMGetCR4();
        uint32_t    cLevels;
        switch (pThis->enmPgMode)
        {
            case SUPPAGINGMODE_32_BIT:
            case SUPPAGINGMODE_32_BIT_GLOBAL:
                cLevels = 2;
                a[0].fAndMask = X86_PDE_P | X86_PDE_RW | (cr4 & X86_CR4_PSE ? X86_PDE_PS : 0);
                a[0].fResMask = X86_PDE_P | X86_PDE_RW;
                a[0].fPtrMask  = X86_PD_MASK;
                a[0].fPtrShift = X86_PD_SHIFT;
                a[1].fAndMask = X86_PTE_P | X86_PTE_RW;
                a[1].fResMask = X86_PTE_P | X86_PTE_RW;
                a[1].fPtrMask  = X86_PT_MASK;
                a[1].fPtrShift = X86_PT_SHIFT;
                break;

            case SUPPAGINGMODE_PAE:
            case SUPPAGINGMODE_PAE_GLOBAL:
            case SUPPAGINGMODE_PAE_NX:
            case SUPPAGINGMODE_PAE_GLOBAL_NX:
                cLevels = 3;
                a[0].fAndMask = X86_PDPE_P;
                a[0].fResMask = X86_PDPE_P;
                a[0].fPtrMask  = X86_PDPT_MASK_PAE;
                a[0].fPtrShift = X86_PDPT_SHIFT;
                a[1].fAndMask = X86_PDE_P | X86_PDE_RW | (cr4 & X86_CR4_PSE ? X86_PDE_PS : 0);
                a[1].fResMask = X86_PDE_P | X86_PDE_RW;
                a[1].fPtrMask  = X86_PD_MASK;
                a[1].fPtrShift = X86_PD_SHIFT;
                a[2].fAndMask = X86_PTE_P | X86_PTE_RW;
                a[2].fResMask = X86_PTE_P | X86_PTE_RW;
                a[2].fPtrMask  = X86_PT_MASK;
                a[2].fPtrShift = X86_PT_SHIFT;
                break;

            case SUPPAGINGMODE_AMD64:
            case SUPPAGINGMODE_AMD64_GLOBAL:
            case SUPPAGINGMODE_AMD64_NX:
            case SUPPAGINGMODE_AMD64_GLOBAL_NX:
                cLevels = 3;
                a[0].fAndMask = X86_PML4E_P | X86_PML4E_RW;
                a[0].fResMask = X86_PML4E_P | X86_PML4E_RW;
                a[0].fPtrMask  = X86_PML4_MASK;
                a[0].fPtrShift = X86_PML4_SHIFT;
                a[1].fAndMask = X86_PDPE_P | X86_PDPE_RW /** @todo check for X86_PDPT_PS support. */;
                a[1].fResMask = X86_PDPE_P | X86_PDPE_RW;
                a[1].fPtrMask  = X86_PDPT_MASK_AMD64;
                a[1].fPtrShift = X86_PDPT_SHIFT;
                a[2].fAndMask = X86_PDE_P | X86_PDE_RW | (cr4 & X86_CR4_PSE ? X86_PDE_PS : 0);
                a[2].fResMask = X86_PDE_P | X86_PDE_RW;
                a[2].fPtrMask  = X86_PD_MASK;
                a[2].fPtrShift = X86_PD_SHIFT;
                a[3].fAndMask = X86_PTE_P | X86_PTE_RW;
                a[3].fResMask = X86_PTE_P | X86_PTE_RW;
                a[3].fPtrMask  = X86_PT_MASK;
                a[3].fPtrShift = X86_PT_SHIFT;
                break;
            default:
                cLevels = 0;
                break;
        }
        for (uint32_t i = 0; i < RT_ELEMENTS(a); i++)
        {
            a[i].HCPhys = NIL_RTHCPHYS;
            a[i].hMapObj = a[i].hMemObj = NIL_RTR0MEMOBJ;
            a[i].u.pv = NULL;
        }

        for (; iPage < iEndPage && RT_SUCCESS(rc); iPage++, pbPage += PAGE_SIZE)
        {
            /* Initialize it */
            pThis->paPages[iPage].HCPhys = NIL_RTHCPHYS;
            pThis->paPages[iPage].pvPage = pbPage;
            pThis->paPages[iPage].cRefs  = 0;
            pThis->paPages[iPage].uPte.pPae = NULL;
            RTCpuSetFill(&pThis->paPages[iPage].PendingSet);

            /*
             * Map its page table.
             *
             * This is a bit ASSUMPTIVE, it should really do a clean run thru
             * the tables everything something was mapped and disable preemption
             * or/and interrupts.
             */
            X86PGPAEUINT uEntry = ASMGetCR3();
            for (unsigned i = 0; i < cLevels && RT_SUCCESS(rc); i++)
            {
                RTHCPHYS HCPhys = uEntry & a[i].fPhysMask;
                if (a[i].HCPhys != HCPhys)
                {
                    if (i + 1 != cLevels)
                    {
                        RTR0MemObjFree(a[i].hMemObj, true /* fFreeMappings */);
                        a[i].hMemObj = a[i].hMapObj = NIL_RTR0MEMOBJ;
                    }
                    rc = RTR0MemObjEnterPhys(&a[i].hMemObj, HCPhys, PAGE_SIZE);
                    if (RT_SUCCESS(rc))
                        rc = RTR0MemObjMapKernel(&a[i].hMapObj, a[i].hMemObj, &a[i].u.pv, 0, RTMEM_PROT_WRITE | RTMEM_PROT_READ);
                    if (RT_FAILURE(rc))
                        break;
                }

            }


        } /* for each page */

        for (iPage = 0; i < cLevels; )

        rc2 = RTR0MemObjFree(hMemObjCR3, true /* fFreeMappings */); AssertRC(rc2);

        rc2 = RTR0MemObjFree(pSeg->hMemObj, true /* fFreeMappings */); AssertRC(rc2);
    }
    RTMemFree(pSeg);
    return rc;
#else
    return VERR_NOT_IMPLEMENTED;
#endif
}


/**
 * Called by PGMR0DynMapInitVM under the init lock.
 *
 * @returns VBox status code.
 * @param   pThis       The dynamic mapping cache instance.
 */
static int pgmR0DynMapSetup(PPGMR0DYNMAP pThis)
{
    /*
     * Calc the size and add a segment of that size.
     */
    uint32_t cPages = pgmR0DynMapCalcNewSize(pThis);
    AssertReturn(cPages, VERR_INTERNAL_ERROR);
    return pgmR0DynMapAddSeg(pThis, cPages);
}


/**
 * Called by PGMR0DynMapInitVM under the init lock.
 *
 * @returns VBox status code.
 * @param   pThis       The dynamic mapping cache instance.
 */
static int pgmR0DynMapGrow(PPGMR0DYNMAP pThis)
{
    /*
     * Calc the new target size and add a segment of the appropriate size.
     */
    uint32_t cPages = pgmR0DynMapCalcNewSize(pThis);
    if (pThis->cPages >= cPages)
        return VINF_SUCCESS;

    uint32_t cAdd = cPages - pThis->cPages;
    return pgmR0DynMapAddSeg(pThis, cAdd);
}


/**
 * Shoots down the TLBs for all the cache pages, pgmR0DynMapTearDown helper.
 *
 * @param   idCpu           The current CPU.
 * @param   pvUser1         The dynamic mapping cache instance.
 * @param   pvUser2         Unused, NULL.
 */
static DECLCALLBACK(void) pgmR0DynMapShootDownTlbs(RTCPUID idCpu, void *pvUser1, void *pvUser2)
{
    Assert(!pvUser2);
    PPGMR0DYNMAP        pThis   = (PPGMR0DYNMAP)pvUser1;
    AssertPtr(pThis == g_pPGMR0DynMap);
    PPGMR0DYNMAPENTRY   paPages = pThis->paPages;
    uint32_t            iPage   = pThis->cPages;
    while (iPage-- > 0)
        ASMInvalidatePage(paPages[iPage].pvPage);
}


/**
 * Called by PGMR0DynMapTermVM under the init lock.
 *
 * @returns VBox status code.
 * @param   pThis       The dynamic mapping cache instance.
 */
static void pgmR0DynMapTearDown(PPGMR0DYNMAP pThis)
{
    /*
     * Restore the original page table entries
     */
    PPGMR0DYNMAPENTRY   paPages = pThis->paPages;
    uint32_t            iPage   = pThis->cPages;
    if (pThis->fLegacyMode)
    {
        X86PGUINT const    *paSavedPTEs = (X86PGUINT const *)pThis->pvSavedPTEs;
        while (iPage-- > 0)
        {
            X86PGUINT       uOld  = paPages[iPage].uPte.pLegacy->u;
            X86PGUINT       uOld2 = uOld; NOREF(uOld2);
            X86PGUINT       uNew  = paSavedPTEs[iPage];
            while (!ASMAtomicCmpXchgExU32(&paPages[iPage].uPte.pLegacy->u, uNew, uOld, &uOld))
                AssertMsgFailed(("uOld=%#x uOld2=%#x uNew=%#x\n", uOld, uOld2, uNew));
        }
    }
    else
    {
        X86PGPAEUINT const *paSavedPTEs = (X86PGPAEUINT const *)pThis->pvSavedPTEs;
        while (iPage-- > 0)
        {
            X86PGPAEUINT    uOld  = paPages[iPage].uPte.pPae->u;
            X86PGPAEUINT    uOld2 = uOld; NOREF(uOld2);
            X86PGPAEUINT    uNew  = paSavedPTEs[iPage];
            while (!ASMAtomicCmpXchgExU64(&paPages[iPage].uPte.pPae->u, uNew, uOld, &uOld))
                AssertMsgFailed(("uOld=%#llx uOld2=%#llx uNew=%#llx\n", uOld, uOld2, uNew));
        }
    }

    /*
     * Shoot down the TLBs on all CPUs before freeing them.
     * If RTMpOnAll fails, make sure the TLBs are invalidated on the current CPU at least.
     */
    int rc = RTMpOnAll(pgmR0DynMapShootDownTlbs, pThis, NULL);
    AssertRC(rc);
    if (RT_FAILURE(rc))
    {
        iPage = pThis->cPages;
        while (iPage-- > 0)
            ASMInvalidatePage(paPages[iPage].pvPage);
    }

    /*
     * Free the segments.
     */
    while (pThis->pSegHead)
    {
        PPGMR0DYNMAPSEG pSeg = pThis->pSegHead;
        pThis->pSegHead = pSeg->pNext;

        uint32_t iPT = pSeg->cPTs;
        while (iPT-- > 0)
        {
            rc = RTR0MemObjFree(pSeg->ahMemObjPT[iPT], true /* fFreeMappings */); AssertRC(rc);
            pSeg->ahMemObjPT[iPT] = NIL_RTR0MEMOBJ;
        }
        rc = RTR0MemObjFree(pSeg->hMemObj,   true /* fFreeMappings */); AssertRC(rc);
        pSeg->hMemObj   = NIL_RTR0MEMOBJ;
        pSeg->pNext     = NULL;
        pSeg->iPage     = UINT16_MAX;
        pSeg->cPages    = 0;
        pSeg->cPTs      = 0;
        RTMemFree(pSeg);
    }

    /*
     * Free the arrays and restore the initial state.
     * The cLoadMax value is left behind for the next setup.
     */
    RTMemFree(pThis->paPages);
    pThis->paPages = NULL;
    RTMemFree(pThis->pvSavedPTEs);
    pThis->pvSavedPTEs = NULL;
    pThis->cPages = 0;
    pThis->cLoad = 0;
}


/**
 * Release references to a page, caller owns the spin lock.
 *
 * @param   pThis       The dynamic mapping cache instance.
 * @param   iPage       The page.
 * @param   cRefs       The number of references to release.
 */
DECLINLINE(void) pgmR0DynMapReleasePageLocked(PPGMR0DYNMAP pThis, uint32_t iPage, int32_t cRefs)
{
    cRefs = ASMAtomicSubS32(&pThis->paPages[iPage].cRefs, cRefs);
    AssertMsg(cRefs >= 0, ("%d\n", cRefs));
    if (!cRefs)
        pThis->cLoad--;
}


/**
 * Release references to a page, caller does not own the spin lock.
 *
 * @param   pThis       The dynamic mapping cache instance.
 * @param   iPage       The page.
 * @param   cRefs       The number of references to release.
 */
static void pgmR0DynMapReleasePage(PPGMR0DYNMAP pThis, uint32_t iPage, uint32_t cRefs)
{
    RTSPINLOCKTMP Tmp = RTSPINLOCKTMP_INITIALIZER;
    RTSpinlockAcquire(pThis->hSpinlock, &Tmp);
    pgmR0DynMapReleasePageLocked(pThis, iPage, cRefs);
    RTSpinlockRelease(pThis->hSpinlock, &Tmp);
}


/**
 * pgmR0DynMapPage worker that deals with the tedious bits.
 *
 * @returns The page index on success, UINT32_MAX on failure.
 * @param   pThis       The dynamic mapping cache instance.
 * @param   HCPhys      The address of the page to be mapped.
 * @param   iPage       The page index pgmR0DynMapPage hashed HCPhys to.
 */
static uint32_t pgmR0DynMapPageSlow(PPGMR0DYNMAP pThis, RTHCPHYS HCPhys, uint32_t iPage)
{
    /*
     * Check if any of the first 5 pages are unreferenced since the caller
     * already has made sure they aren't matching.
     */
    uint32_t const      cPages  = cPages;
    PPGMR0DYNMAPENTRY   paPages = pThis->paPages;
    uint32_t            iFreePage;
    if (!paPages[iPage].cRefs)
        iFreePage = iPage;
    else if (!paPages[(iPage + 1) % cPages].cRefs)
        iFreePage = iPage;
    else if (!paPages[(iPage + 2) % cPages].cRefs)
        iFreePage = iPage;
    else if (!paPages[(iPage + 3) % cPages].cRefs)
        iFreePage = iPage;
    else if (!paPages[(iPage + 4) % cPages].cRefs)
        iFreePage = iPage;
    else
    {
        /*
         * Search for an unused or matching entry.
         */
        iFreePage = (iPage + 5) % pThis->cPages;
        for (;;)
        {
            if (paPages[iFreePage].HCPhys == HCPhys)
                return iFreePage;
            if (!paPages[iFreePage].cRefs)
                break;

            /* advance */
            iFreePage = (iFreePage + 1) % cPages;
            if (RT_UNLIKELY(iFreePage != iPage))
                return UINT32_MAX;
        }
    }

    /*
     * Setup the new entry.
     */
    paPages[iFreePage].HCPhys = HCPhys;
    RTCpuSetFill(&paPages[iFreePage].PendingSet);
    if (pThis->fLegacyMode)
    {
        X86PGUINT       uOld  = paPages[iFreePage].uPte.pLegacy->u;
        X86PGUINT       uOld2 = uOld; NOREF(uOld2);
        X86PGUINT       uNew  = (uOld & X86_PTE_G | X86_PTE_PAT | X86_PTE_PCD | X86_PTE_PWT)
                              | X86_PTE_P | X86_PTE_A | X86_PTE_D
                              | (HCPhys & X86_PTE_PG_MASK);
        while (!ASMAtomicCmpXchgExU32(&paPages[iFreePage].uPte.pLegacy->u, uNew, uOld, &uOld))
            AssertMsgFailed(("uOld=%#x uOld2=%#x uNew=%#x\n", uOld, uOld2, uNew));
    }
    else
    {
        X86PGPAEUINT    uOld  = paPages[iFreePage].uPte.pPae->u;
        X86PGPAEUINT    uOld2 = uOld; NOREF(uOld2);
        X86PGPAEUINT    uNew  = (uOld & X86_PTE_G | X86_PTE_PAT | X86_PTE_PCD | X86_PTE_PWT)
                              | X86_PTE_P | X86_PTE_A | X86_PTE_D
                              | (HCPhys & X86_PTE_PAE_PG_MASK);
        while (!ASMAtomicCmpXchgExU64(&paPages[iFreePage].uPte.pPae->u, uNew, uOld, &uOld))
            AssertMsgFailed(("uOld=%#llx uOld2=%#llx uNew=%#llx\n", uOld, uOld2, uNew));
    }
    return iFreePage;
}


/**
 * Maps a page into the pool.
 *
 * @returns Pointer to the mapping.
 * @param   pThis       The dynamic mapping cache instance.
 * @param   HCPhys      The address of the page to be mapped.
 * @param   piPage      Where to store the page index.
 */
DECLINLINE(void *) pgmR0DynMapPage(PPGMR0DYNMAP pThis, RTHCPHYS HCPhys, uint32_t *piPage)
{
    RTSPINLOCKTMP   Tmp       = RTSPINLOCKTMP_INITIALIZER;
    RTSpinlockAcquire(pThis->hSpinlock, &Tmp);
    AssertMsg(!(HCPhys & PAGE_OFFSET_MASK), ("HCPhys=%RHp\n", HCPhys));

    /*
     * Find an entry, if possible a matching one. The HCPhys address is hashed
     * down to a page index, collisions are handled by linear searching. Optimize
     * for a hit in the first 5 pages.
     *
     * To the cheap hits here and defer the tedious searching and inserting
     * to a helper function.
     */
    uint32_t const      cPages  = cPages;
    uint32_t            iPage   = (HCPhys >> PAGE_SHIFT) % cPages;
    PPGMR0DYNMAPENTRY   paPages = pThis->paPages;
    if (paPages[iPage].HCPhys != HCPhys)
    {
        uint32_t    iPage2 = (iPage + 1) % cPages;
        if (paPages[iPage2].HCPhys != HCPhys)
        {
            iPage2 = (iPage + 2) % cPages;
            if (paPages[iPage2].HCPhys != HCPhys)
            {
                iPage2 = (iPage + 3) % cPages;
                if (paPages[iPage2].HCPhys != HCPhys)
                {
                    iPage2 = (iPage + 4) % cPages;
                    if (paPages[iPage2].HCPhys != HCPhys)
                    {
                        iPage = pgmR0DynMapPageSlow(pThis, HCPhys, iPage);
                        if (RT_UNLIKELY(iPage == UINT32_MAX))
                        {
                            RTSpinlockRelease(pThis->hSpinlock, &Tmp);
                            return NULL;
                        }
                    }
                    else
                        iPage = iPage2;
                }
                else
                    iPage = iPage2;
            }
            else
                iPage = iPage2;
        }
        else
            iPage = iPage2;
    }

    /*
     * Reference it, update statistics and get the return address.
     */
    if (ASMAtomicIncS32(&paPages[iPage].cRefs) == 1)
    {
        pThis->cLoad++;
        if (pThis->cLoad > pThis->cMaxLoad)
            pThis->cMaxLoad = pThis->cLoad;
        Assert(pThis->cLoad <= pThis->cPages);
    }
    void *pvPage = paPages[iPage].pvPage;

    /*
     * Invalidate the entry?
     */
    RTCPUID idRealCpu = RTMpCpuId();
    bool fInvalidateIt = RTCpuSetIsMember(&paPages[iPage].PendingSet, idRealCpu);
    if (fInvalidateIt)
        RTCpuSetDel(&paPages[iPage].PendingSet, idRealCpu);

    RTSpinlockRelease(pThis->hSpinlock, &Tmp);

    /*
     * Do the actual invalidation outside the spinlock.
     */
    ASMInvalidatePage(pvPage);

    *piPage = iPage;
    return pvPage;
}


/**
 * Signals the start of a new set of mappings.
 *
 * Mostly for strictness. PGMDynMapHCPage won't work unless this
 * API is called.
 *
 * @param   pVCpu       The shared data for the current virtual CPU.
 */
VMMDECL(void) PGMDynMapStartAutoSet(PVMCPU pVCpu)
{
    Assert(pVCpu->pgm.s.AutoSet.cEntries == PGMMAPSET_CLOSED);
    pVCpu->pgm.s.AutoSet.cEntries = 0;
}


/**
 * Releases the dynamic memory mappings made by PGMDynMapHCPage and associates
 * since the PGMDynMapStartAutoSet call.
 *
 * @param   pVCpu       The shared data for the current virtual CPU.
 */
VMMDECL(void) PGMDynMapReleaseAutoSet(PVMCPU pVCpu)
{
    PPGMMAPSET  pSet = &pVCpu->pgm.s.AutoSet;

    /* close the set */
    uint32_t    i = pVCpu->pgm.s.AutoSet.cEntries;
    AssertMsg(i <= RT_ELEMENTS(pVCpu->pgm.s.AutoSet.aEntries), ("%#x (%u)\n", i, i));
    pVCpu->pgm.s.AutoSet.cEntries = PGMMAPSET_CLOSED;

    /* release any pages we're referencing. */
    if (i != 0 && RT_LIKELY(i <= RT_ELEMENTS(pVCpu->pgm.s.AutoSet.aEntries)))
    {
        PPGMR0DYNMAP    pThis = g_pPGMR0DynMap;
        RTSPINLOCKTMP   Tmp = RTSPINLOCKTMP_INITIALIZER;
        RTSpinlockAcquire(pThis->hSpinlock, &Tmp);

        while (i-- > 0)
        {
            uint32_t iPage = pSet->aEntries[i].iPage;
            Assert(iPage < pThis->cPages);
            int32_t  cRefs = pSet->aEntries[i].cRefs;
            Assert(cRefs > 0);
            pgmR0DynMapReleasePageLocked(pThis, iPage, cRefs);

            pSet->aEntries[i].iPage = UINT16_MAX;
            pSet->aEntries[i].cRefs = 0;
        }

        Assert(pThis->cLoad <= pThis->cPages);
        RTSpinlockRelease(pThis->hSpinlock, &Tmp);
    }
}


/**
 * Migrates the automatic mapping set of the current vCPU if necessary.
 *
 * This is called when re-entering the hardware assisted execution mode after a
 * nip down to ring-3.  We run the risk that the CPU might have change and we
 * will therefore make sure all the cache entries currently in the auto set will
 * be valid on the new CPU.  If the cpu didn't change nothing will happen as all
 * the entries will have been flagged as invalidated.
 *
 * @param   pVCpu       The shared data for the current virtual CPU.
 * @thread  EMT
 */
VMMDECL(void) PGMDynMapMigrateAutoSet(PVMCPU pVCpu)
{
    PPGMMAPSET  pSet = &pVCpu->pgm.s.AutoSet;
    uint32_t    i = pVCpu->pgm.s.AutoSet.cEntries;
    AssertMsg(i <= RT_ELEMENTS(pVCpu->pgm.s.AutoSet.aEntries), ("%#x (%u)\n", i, i));
    if (i != 0 && RT_LIKELY(i <= RT_ELEMENTS(pVCpu->pgm.s.AutoSet.aEntries)))
    {
        PPGMR0DYNMAP    pThis = g_pPGMR0DynMap;
        RTCPUID         idRealCpu = RTMpCpuId();

        while (i-- > 0)
        {
            Assert(pSet->aEntries[i].cRefs > 0);
            uint32_t iPage = pSet->aEntries[i].iPage;
            Assert(iPage < pThis->cPages);
            if (RTCpuSetIsMember(&pThis->paPages[iPage].PendingSet, idRealCpu))
            {
                RTCpuSetDel(&pThis->paPages[iPage].PendingSet, idRealCpu);
                ASMInvalidatePage(pThis->paPages[iPage].pvPage);
            }
        }
    }
}


/**
 * As a final resort for a full auto set, try merge duplicate entries.
 *
 * @param   pSet        The set.
 */
static void pgmDynMapOptimizeAutoSet(PPGMMAPSET pSet)
{
    for (uint32_t i = 0 ; i < pSet->cEntries; i++)
    {
        uint16_t const  iPage = pSet->aEntries[i].iPage;
        uint32_t        j     = i + 1;
        while (j < pSet->cEntries)
        {
            if (pSet->aEntries[j].iPage != iPage)
                j++;
            else
            {
                /* merge j with i removing j. */
                pSet->aEntries[i].cRefs += pSet->aEntries[j].cRefs;
                pSet->cEntries--;
                if (j < pSet->cEntries)
                {
                    pSet->aEntries[j] = pSet->aEntries[pSet->cEntries];
                    pSet->aEntries[pSet->cEntries].iPage = UINT16_MAX;
                    pSet->aEntries[pSet->cEntries].cRefs = 0;
                }
                else
                {
                    pSet->aEntries[j].iPage = UINT16_MAX;
                    pSet->aEntries[j].cRefs = 0;
                }
            }
        }
    }
}


/* documented elsewhere - a bit of a mess. */
VMMDECL(int) PGMDynMapHCPage(PVM pVM, RTHCPHYS HCPhys, void **ppv)
{
    /*
     * Validate state.
     */
    AssertMsgReturn(pVM->pgm.s.pvR0DynMapUsed == g_pPGMR0DynMap,
                    ("%p != %p\n", pVM->pgm.s.pvR0DynMapUsed, g_pPGMR0DynMap),
                    VERR_ACCESS_DENIED);
    AssertMsg(!(HCPhys & PAGE_OFFSET_MASK), ("HCPhys=%RHp\n", HCPhys));
    PVMCPU          pVCpu   = VMMGetCpu(pVM);
    PPGMMAPSET      pSet    = &pVCpu->pgm.s.AutoSet;
    AssertPtrReturn(pVCpu, VERR_INTERNAL_ERROR);
    AssertMsgReturn(pSet->cEntries > RT_ELEMENTS(pSet->aEntries),
                    ("%#x (%u)\n", pSet->cEntries, pSet->cEntries), VERR_WRONG_ORDER);

    /*
     * Map it.
     */
    uint32_t        iPage;
    void           *pvPage  = pgmR0DynMapPage(g_pPGMR0DynMap, HCPhys, &iPage);
    if (RT_UNLIKELY(!pvPage))
    {
        static uint32_t s_cBitched = 0;
        if (++s_cBitched < 10)
            LogRel(("PGMDynMapHCPage: cLoad=%u/%u cPages=%u\n",
                    g_pPGMR0DynMap->cLoad, g_pPGMR0DynMap->cMaxLoad, g_pPGMR0DynMap->cPages));
        return VERR_PGM_DYNMAP_FAILED;
    }

    /*
     * Add the page to the auto reference set.
     * If it's less than half full, don't bother looking for duplicates.
     */
    if (pSet->cEntries < RT_ELEMENTS(pSet->aEntries) / 2)
    {
        pSet->aEntries[pSet->cEntries].cRefs = 1;
        pSet->aEntries[pSet->cEntries].iPage = iPage;
    }
    else
    {
        Assert(pSet->cEntries <= RT_ELEMENTS(pSet->aEntries));
        int32_t     i = pSet->cEntries;
        while (i-- > 0)
            if (pSet->aEntries[i].iPage)
            {
                pSet->aEntries[i].cRefs++;
                break;
            }
        if (i < 0)
        {
            if (RT_UNLIKELY(pSet->cEntries >= RT_ELEMENTS(pSet->aEntries)))
                pgmDynMapOptimizeAutoSet(pSet);
            if (RT_LIKELY(pSet->cEntries < RT_ELEMENTS(pSet->aEntries)))
            {
                pSet->aEntries[pSet->cEntries].cRefs = 1;
                pSet->aEntries[pSet->cEntries].iPage = iPage;
            }
            else
            {
                /* We're screwed. */
                pgmR0DynMapReleasePage(g_pPGMR0DynMap, iPage, 1);

                static uint32_t s_cBitched = 0;
                if (++s_cBitched < 10)
                    LogRel(("PGMDynMapHCPage: set is full!\n"));
                return VERR_PGM_DYNMAP_FULL_SET;
            }
        }
    }

    return VINF_SUCCESS;
}

